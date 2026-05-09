/*
 * Copyright 2004 James Bursa <bursa@users.sourceforge.net>
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
 * Localised message support (interface).
 *
 * The messages module loads a file of keys and associated strings, and
 * provides fast lookup by key. The messages file consists of key:value lines,
 * comment lines starting with #, and other lines are ignored. Use
 * messages_load() to read the file into memory. To lookup a key, use
 * messages_get("key").
 *
 * It can also load additional messages files into different contexts and allow
 * you to look up values in it independantly from the standard shared Messages
 * file table.  Use the _ctx versions of the functions to do this.
 */

#ifndef NETSURF_UTILS_MESSAGES_H_
#define NETSURF_UTILS_MESSAGES_H_

#include <stdint.h>

#include "utils/errors.h"
#include "netsurf/ssl_certs.h"

/**
 * Read keys and values from messages file into the standard Messages hash.
 *
 * The messages are merged with any previously loaded messages. Any
 * keys which are present already are replaced with the new value. The
 * file may be gzip compressed.
 *
 * \param path pathname of messages file.
 * \return NSERROR_OK on success or error code on faliure.
 */
nserror messages_add_from_file(const char *path);

/**
 * Read keys and values from inline message data into the standard Messages hash.
 *
 * The messages are merged with any previously loaded messages. Any
 * keys which are present already are replaced with the new value. The
 * data may be gzip compressed.
 *
 * \param data The inline message data.
 * \param data_size The length of the message data.
 * \return NSERROR_OK on success or error code on faliure.
 */
nserror messages_add_from_inline(const uint8_t *data, size_t data_size);

/**
 * Add a single message.
 *
 * The message is merged with any previously loaded messages.
 *
 * \return NSERROR_OK on success or error code on faliure.

 */
nserror messages_add_key_value(const char *key, const char *value);

/**
 * Fast lookup of a message by key from the standard Messages hash.
 *
 * \param  key  key of message
 * \return value of message, or key if not found
 */
const char *messages_get(const char *key);

/**
 * lookup of a message by errorcode from the standard Messages hash.
 *
 * \param code errorcode of message
 * \return message text
 */
const char *messages_get_errorcode(nserror code);

/**
 * lookup of a message by SSL error code from the standard Messages hash.
 *
 * \param code ssl error code
 * \return message text
 */
const char *messages_get_sslcode(ssl_cert_err code);

/**
 * Formatted message from a key in the global message hash.
 *
 * \param key key of message
 * \param ... message parameters
 * \return buffer containing formatted message text or NULL if key is
 *         unavailable or memory allocation failed. The caller owns the
 *         returned buffer and is responsible for freeing it.
 */
char *messages_get_buff(const char *key, ...);

/**
 * Free memory used by the standard Messages hash
 */
void messages_destroy(void);

#endif
