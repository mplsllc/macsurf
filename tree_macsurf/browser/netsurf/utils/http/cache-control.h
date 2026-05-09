/*
 * Copyright 2019 John-Mark Bell <jmb@netsurf-browser.org>
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

#ifndef NETSURF_UTILS_HTTP_CACHE_CONTROL_H_
#define NETSURF_UTILS_HTTP_CACHE_CONTROL_H_

#include <libwapcaplet/libwapcaplet.h>

typedef struct http_cache_control http_cache_control;

/**
 * Parse an HTTP Cache-Control header value
 *
 * \param header_value  Header value to parse
 * \param result        Pointer to location to receive result
 * \return NSERROR_OK on success,
 *         NSERROR_NOMEM on memory exhaustion,
 *         appropriate error otherwise
 */
nserror http_parse_cache_control(const char *header_value,
		http_cache_control **result);

/**
 * Destroy a cache_control object
 *
 * \param victim  Object to destroy
 */
void http_cache_control_destroy(http_cache_control *victim);

/**
 * Determine if a valid max-age directive is present
 *
 * \param cc Object to inspect
 * \return Whether max-age is valid
 */
bool http_cache_control_has_max_age(http_cache_control *cc);

/**
 * Get the value of a cache control's max-age
 *
 * \param cc Object to inspect
 * \return Max age, in delta-seconds
 */
uint32_t http_cache_control_max_age(http_cache_control *cc);

/**
 * Get the value of a cache control's no-cache flag
 *
 * \param cc Object to inspect
 * \return Whether caching is forbidden
 */
bool http_cache_control_no_cache(http_cache_control *cc);

/**
 * Get the value of a cache control's no-store flag
 *
 * \param cc Object to inspect
 * \return Whether persistent caching is forbidden
 */
bool http_cache_control_no_store(http_cache_control *cc);

#endif
