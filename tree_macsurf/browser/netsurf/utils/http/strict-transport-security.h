/*
 * Copyright 2018 John-Mark Bell <jmb@netsurf-browser.org>
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

#ifndef NETSURF_UTILS_HTTP_STRICT_TRANSPORT_SECURITY_H_
#define NETSURF_UTILS_HTTP_STRICT_TRANSPORT_SECURITY_H_

#include <libwapcaplet/libwapcaplet.h>

typedef struct http_strict_transport_security http_strict_transport_security;

/**
 * Parse an HTTP Strict-Transport-Security header value
 *
 * \param header_value  Header value to parse
 * \param result        Pointer to location to receive result
 * \return NSERROR_OK on success,
 *         NSERROR_NOMEM on memory exhaustion,
 *         appropriate error otherwise
 */
nserror http_parse_strict_transport_security(const char *header_value,
		http_strict_transport_security **result);

/**
 * Destroy a strict transport security object
 *
 * \param victim  Object to destroy
 */
void http_strict_transport_security_destroy(
		http_strict_transport_security *victim);

/**
 * Get the value of a strict transport security's max-age
 *
 * \param sts Object to inspect
 * \return Max age, in delta-seconds
 */
uint32_t http_strict_transport_security_max_age(
		http_strict_transport_security *sts);

/**
 * Get the value of a strict transport security's includeSubDomains flag
 *
 * \param sts Object to inspect
 * \return Whether subdomains should be included
 */
bool http_strict_transport_security_include_subdomains(
		http_strict_transport_security *sts);

#endif
