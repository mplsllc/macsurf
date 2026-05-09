/*
 * Copyright 2006 John M Bell <jmb202@ecs.soton.ac.uk>
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
 * Unified cookie database public interface.
 */

#ifndef _NETSURF_COOKIE_DB_H_
#define _NETSURF_COOKIE_DB_H_

#include <stdbool.h>
#include <time.h>

#include "utils/errors.h"

/**
 * Version of cookie
 *
 * RFC2109 and RFC2965 "HTTP State Management Mechanism" defined
 * alternative versions of cookies. Nothing used them and these RFC
 * are now obsoleted by RFC6265 which completely removes the
 * alternative versions.
 */
enum cookie_version {
	COOKIE_NETSCAPE = 0,
	COOKIE_RFC2109 = 1,
	COOKIE_RFC2965 = 2
};

struct cookie_data {
	const struct cookie_data *prev;	/**< Previous in list */
	const struct cookie_data *next;	/**< Next in list */

	const char *name;            /**< Cookie name */
	const char *value;	     /**< Cookie value */
	const bool value_was_quoted; /**< Value was quoted in Set-Cookie: */
	const char *comment;	     /**< Cookie comment */
	const bool domain_from_set;  /**< Domain came from Set-Cookie: header */
	const char *domain;	     /**< Domain */
	const bool path_from_set;    /**< Path came from Set-Cookie: header */
	const char *path;	     /**< Path */
	const time_t expires;	     /**< Expiry timestamp, or 1 for session */
	const time_t last_used;	     /**< Last used time */
	const bool secure;	     /**< Only send for HTTPS requests */
	const bool http_only;	     /**< Only expose to HTTP(S) requests */
	enum cookie_version version; /**< Specification compliance */

        /** Never destroy this cookie, unless it's expired */
        const bool no_destroy;
};

/**
 * Iterate over all cookies in database
 *
 * \param callback Function to callback for each entry
 */
void urldb_iterate_cookies(bool (*callback)(const struct cookie_data *cookie));

/**
 * Delete a cookie
 *
 * \param domain The cookie's domain
 * \param path The cookie's path
 * \param name The cookie's name
 */
void urldb_delete_cookie(const char *domain, const char *path, const char *name);

/**
 * Load a cookie file into the database
 *
 * \param filename File to load
 */
nserror urldb_load_cookies(const char *filename);

/**
 * Save persistent cookies to file
 *
 * \param filename Path to save to
 * \return NSERROR_OK on success else error code
 */
nserror urldb_save_cookies(const char *filename);

#endif
