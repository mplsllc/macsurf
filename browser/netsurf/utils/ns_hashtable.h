/*
 * Copyright 2006 Rob Kendrick <rjek@rjek.com>
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
 * Interface to Write-Once hash table for string to string mapping
 */

#ifndef _NETSURF_UTILS_HASHTABLE_H_
#define _NETSURF_UTILS_HASHTABLE_H_

#include <stdbool.h>

#include "utils/errors.h"

struct hash_table;

/**
 * Create a new hash table
 *
 * Allocate a new hash table and return a context for it.  The memory
 * consumption of a hash table is approximately 8 + (nchains * 12)
 * bytes if it is empty.
 *
 * \param chains Number of chains/buckets this hash table will have.  This
 *		  should be a prime number, and ideally a prime number just
 *		  over a power of two, for best performance and distribution.
 * \return struct hash_table containing the context of this hash table or NULL
 *	   if there is insufficent memory to create it and its chains.
 */
struct hash_table *hash_create(unsigned int chains);

/**
 * Destroys a hash table
 *
 * Destroy a hash table freeing all memory associated with it.
 *
 * \param ht Hash table to destroy. After the function returns, this
 *             will no longer be valid.
 */
void hash_destroy(struct hash_table *ht);

/**
 * Adds a key/value pair to a hash table.
 *
 * If the key you're adding is already in the hash table, it does not
 * replace it, but it does take precedent over it.  The old key/value
 * pair will be inaccessable but still in memory until hash_destroy()
 * is called on the hash table.
 *
 * \param  ht	  The hash table context to add the key/value pair to.
 * \param  key	  The key to associate the value with.  A copy is made.
 * \param  value  The value to associate the key with.  A copy is made.
 * \return NSERROR_OK if the add succeeded else error code. (Failure most likely
 *	   indicates insufficent memory to make copies of the key and value.
 */
nserror hash_add(struct hash_table *ht, const char *key, const char *value);

/**
 * Looks up a the value associated with with a key from a specific hash table.
 *
 * \param  ht The hash table context to look up the key in.
 * \param  key The key to search for.
 * \return The value associated with the key, or NULL if it was not found.
 */
const char *hash_get(struct hash_table *ht, const char *key);

/**
 * Add key/value pairs to a hash table with data from a file
 *
 * The file should be formatted as a series of lines terminated with
 *  newline character. Each line should contain a key/value pair
 *  separated by a colon. If a line is empty or starts with a #
 *  character it will be ignored.
 *
 * The file may be optionally gzip compressed.
 *
 * \param ht The hash table context to add the key/value pairs to.
 * \param path Path to file with key/value pairs in.
 * \return NSERROR_OK on success else error code
 */
nserror hash_add_file(struct hash_table *ht, const char *path);

/**
 * Add key/value pairs to a hash table with data from a memory buffer
 *
 * The data format is the same as in hash_add_file() but held in memory
 *
 * The data may optionally be gzip compressed.
 *
 * \param ht The hash table context to add the key/value pairs to.
 * \param data Source of key/value pairs
 * \param size length of \a data
 * \return NSERROR_OK on success else error code
 */
nserror hash_add_inline(struct hash_table *ht, const uint8_t *data, size_t size);

#endif
