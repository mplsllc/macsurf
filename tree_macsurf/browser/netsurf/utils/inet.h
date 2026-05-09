/*
 * Copyright 2016 Vincent Sanders <vince@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * internet structures and defines
 *
 * This allows the obtaining of standard bsd sockets and associated
 * functions in a uniform way despite any oddities in headers and
 * supported API between OS.
 *
 * \note This functionality was previously provided as a side effect of the
 *  utils config header include.
 */

#ifndef _NETSURF_UTILS_INET_H_
#define _NETSURF_UTILS_INET_H_

#include "utils/config.h"

#ifdef HAVE_POSIX_INET_HEADERS

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define ns_close_socket close

#ifdef WITH_AMISSL
/* AmiSSL needs everything to be using bsdsocket directly to avoid conflicts */
#include <proto/bsdsocket.h>
#undef ns_close_socket
#define ns_close_socket CloseSocket
#endif

#elif defined(__MACOS9__)

#include "frontends/macos9/shims/mac_inet.h"

#define ns_close_socket(s) ((void)(s), 0)

#else

#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT WSAEAFNOSUPPORT
#endif

#define ns_close_socket closesocket

#endif


#ifndef HAVE_INETATON
int inet_aton(const char *cp, struct in_addr *inp);
#endif

#ifndef HAVE_INETPTON
int inet_pton(int af, const char *src, void *dst);
#endif

#endif
