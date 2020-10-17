/* Aravis - Digital camera library
 *
 * Copyright © 2009-2019 Emmanuel Pacaud
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Emmanuel Pacaud <emmanuel@gnome.org>
 */

#include <arvnetworkprivate.h>
#include <arvdebugprivate.h>

#ifndef G_OS_WIN32
	#include <ifaddrs.h>
#else
	#include <winsock2.h>
	#include <iphlpapi.h>
	#include <winnt.h> // for PWCHAR
#endif

struct _ArvNetworkInterface{
	struct sockaddr *addr;
	struct sockaddr *netmask;
	struct sockaddr *broadaddr;
	char* name;
};

#ifdef G_OS_WIN32

GList* arv_enumerate_network_interfaces(void) {
	/*
	 * docs: https://docs.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getadaptersaddresses
	 *
	 * example source: https://github.com/zeromq/czmq/blob/master/src/ziflist.c#L284
	 * question about a better solution: https://stackoverflow.com/q/64348510/761090
	 */
	ULONG outBufLen = 15000;
	PIP_ADAPTER_ADDRESSES pAddresses = NULL;
	PIP_ADAPTER_ADDRESSES pAddrIter = NULL;
	GList* ret;
	int iter = 0;
	ULONG dwRetVal;

	/* pre-Vista windows don't have PIP_ADAPTER_UNICAST_ADDRESS onLinePrefixLength field.
	 * To get netmask, we build pIPAddrTable (IPv4-only) and find netmask associated to each addr.
	 * This means IPv6 will only work with >= Vista.
	 * See https://stackoverflow.com/a/64358443/761090 for thorough explanation.
	 */
	#if WINVER < _WIN32_WINNT_VISTA
		// https://docs.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getipaddrtable
		PMIB_IPADDRTABLE pIPAddrTable;
		DWORD dwSize = 0;
		/* Variables used to return error message */
		pIPAddrTable = (MIB_IPADDRTABLE *) g_malloc(sizeof (MIB_IPADDRTABLE));
		if (GetIpAddrTable(pIPAddrTable, &dwSize, 0) == ERROR_INSUFFICIENT_BUFFER) {
			g_free(pIPAddrTable);
			pIPAddrTable = (MIB_IPADDRTABLE *) g_malloc(dwSize);
		}
		dwRetVal = GetIpAddrTable( pIPAddrTable, &dwSize, 0 );
		if (dwRetVal != NO_ERROR ) {
			arv_warning_interface("GetIpAddrTable failed.");
			g_free(pIPAddrTable);
		}
	#endif

	do {
		pAddresses = (IP_ADAPTER_ADDRESSES*) g_malloc (outBufLen);
		/* change family to AF_UNSPEC for both IPv4 and IPv6, later */
		dwRetVal = GetAdaptersAddresses(
			/* Family */ AF_INET,
			/* Flags */
				GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
			/* Reserved */ NULL,
			pAddresses,
			&outBufLen
		);
		if (dwRetVal==ERROR_BUFFER_OVERFLOW) {
			g_free (pAddresses);
		} else {
			break;
		}
		iter++;
	} while ((dwRetVal == ERROR_BUFFER_OVERFLOW) && (iter<3));

	if (dwRetVal != ERROR_SUCCESS){
		arv_warning_interface ("Failed to enumerate network interfaces (GetAdaptersAddresses returned %lu)",dwRetVal);
		return NULL;
	}


	ret = NULL;

	for(pAddrIter = pAddresses; pAddrIter != NULL; pAddrIter = pAddrIter->Next){
		PIP_ADAPTER_UNICAST_ADDRESS pUnicast;
		for (pUnicast = pAddrIter->FirstUnicastAddress; pUnicast != NULL; pUnicast = pUnicast->Next){
			// PIP_ADAPTER_PREFIX pPrefix = pAddrIter->FirstPrefix;
			struct sockaddr* lpSockaddr = pUnicast->Address.lpSockaddr;
			ArvNetworkInterface* a;

			gboolean ok = (pAddrIter->OperStatus == IfOperStatusUp)
				/* extend for IPv6 here, later */
				&& ((lpSockaddr->sa_family == AF_INET)
				#if 0 && WINVER >= _WIN32_WINNT_VISTA
					|| (lpSockaddr->sa_family == AF_INET6)
				#endif
				)
			;

			if (!ok) continue;

			a = (ArvNetworkInterface*) g_malloc0(sizeof(ArvNetworkInterface));
			if (lpSockaddr->sa_family == AF_INET){
				struct sockaddr_in* mask;
				struct sockaddr_in* broadaddr;

				/* copy 3x so that sa_family is already set for netmask and broadaddr */
				a->addr = g_memdup (lpSockaddr, sizeof(struct sockaddr));
				a->netmask  = g_memdup (lpSockaddr, sizeof(struct sockaddr));
				a->broadaddr  = g_memdup (lpSockaddr, sizeof(struct sockaddr));
				/* adjust mask & broadcast */
				mask = (struct sockaddr_in*) a->netmask;
				#if WINVER >= _WIN32_WINNT_VISTA
					mask->sin_addr.s_addr = htonl ((0xffffffffU) << (32 - pUnicast->OnLinkPrefixLength));
				#else
					{
						int i;
						gboolean match = FALSE;

						for (i=0; i < pIPAddrTable->dwNumEntries; i++){
							MIB_IPADDRROW* row=&pIPAddrTable->table[i];
							/* both are in network byte order, no need to convert */
							if (row->dwAddr == ((struct sockaddr_in*)a->addr)->sin_addr.s_addr){
								match = TRUE;
								mask->sin_addr.s_addr = row->dwMask;
							}
						}
						if (!match){
							arv_warning_interface("Failed to obtain netmask, using 255.255.255.255.");
							mask->sin_addr.s_addr = 0xffffffffU;
						}
					}
				#endif
				broadaddr = (struct sockaddr_in*) a->broadaddr;
				broadaddr->sin_addr.s_addr |= ~(mask->sin_addr.s_addr);
			}
			#if WINVER >= _WIN32_WINNT_VISTA
			else if (lpSocketaddr->sa_family == AF_INET6){
				arv_warning_interface("IPv6 support not yet implemented.");
			}
			#endif
			/* name is common to IPv4 and IPv6 */
			{
				PWCHAR name = pAddrIter->FriendlyName;
				size_t asciiSize = wcstombs (0, name, 0) + 1;
				a->name = (char *) g_malloc (asciiSize);
				wcstombs (a->name, name, asciiSize);
			}

			ret = g_list_prepend(ret, a);
		}
	}
	g_free (pAddresses);
	#if WINVER < _WIN32_WINNT_VISTA
		g_free(pIPAddrTable);
	#endif

	ret = g_list_reverse(ret);
	return ret;
}

/*
 * mingw only defines inet_ntoa (ipv4-only), inet_ntop (IPv4 & IPv6) is missing from it headers
 * therefore we define it ourselves; code comes from https://www.mail-archive.com/users@ipv6.org/msg02107.html
 */

const char *
inet_ntop (int af, const void *src, char *dst, socklen_t cnt)
{
	if (af == AF_INET) {
		struct sockaddr_in in;

		memset (&in, 0, sizeof(in));
		in.sin_family = AF_INET;
		memcpy (&in.sin_addr, src, sizeof(struct in_addr));
		getnameinfo ((struct sockaddr *)&in, sizeof (struct sockaddr_in), dst, cnt, NULL, 0, NI_NUMERICHOST);
		return dst;
	} else if (af == AF_INET6) {
		struct sockaddr_in6 in;

		memset (&in, 0, sizeof(in));
		in.sin6_family = AF_INET6;
		memcpy (&in.sin6_addr, src, sizeof(struct in_addr6));
		getnameinfo ((struct sockaddr *)&in, sizeof (struct sockaddr_in6), dst, cnt, NULL, 0, NI_NUMERICHOST);
		return dst;
	}

	return NULL;
}

#else

GList*
arv_enumerate_network_interfaces (void)
{
	struct ifaddrs *ifap = NULL;
	struct ifaddrs *ifap_iter;
	GList* ret=NULL;

	if (getifaddrs (&ifap) <0)
		return NULL;

	for (ifap_iter = ifap; ifap_iter != NULL; ifap_iter = ifap_iter->ifa_next) {
		if ((ifap_iter->ifa_flags & IFF_UP) != 0 &&
			(ifap_iter->ifa_flags & IFF_POINTOPOINT) == 0 &&
			(ifap_iter->ifa_addr != NULL) &&
			(ifap_iter->ifa_addr->sa_family == AF_INET)) {
			ArvNetworkInterface* a;

			a = g_new0 (ArvNetworkInterface, 1);

			a->addr = g_memdup (ifap_iter->ifa_addr, sizeof(struct sockaddr));
			if (ifap_iter->ifa_netmask)
				a->netmask = g_memdup (ifap_iter->ifa_netmask, sizeof(struct sockaddr));
			if (ifap_iter->ifa_ifu.ifu_broadaddr)
				a->broadaddr = g_memdup(ifap_iter->ifa_ifu.ifu_broadaddr, sizeof(struct sockaddr));
			if (ifap_iter->ifa_name)
				a->name = g_strdup(ifap_iter->ifa_name);

			ret = g_list_prepend (ret, a);
		}
	}

	freeifaddrs (ifap);

	return g_list_reverse (ret);
};
#endif

struct sockaddr *
arv_network_interface_get_addr(ArvNetworkInterface* a)
{
	return a->addr;
}

struct sockaddr *
arv_network_interface_get_netmask(ArvNetworkInterface* a)
{
	return a->netmask;
}

struct sockaddr *
arv_network_interface_get_broadaddr(ArvNetworkInterface* a)
{
	return a->broadaddr;
}

const char *
arv_network_interface_get_name(ArvNetworkInterface* a)
{
	return a->name;
}

void
arv_network_interface_free(ArvNetworkInterface *a) {
	g_clear_pointer (&a->addr, g_free);
	g_clear_pointer (&a->netmask, g_free);
	g_clear_pointer (&a->broadaddr, g_free);
	g_clear_pointer (&a->name, g_free);
	g_free (a);
}


gboolean
arv_socket_set_recv_buffer_size (int socket_fd, gint buffer_size)
{
	int result;

#ifndef G_OS_WIN32
	result = setsockopt (socket_fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof (buffer_size));
#else
	{
		DWORD _buffer_size=buffer_size;
		result = setsockopt (socket_fd, SOL_SOCKET, SO_RCVBUF, (const char*) &_buffer_size, sizeof (_buffer_size));
	}
#endif

	return result == 0;
}
