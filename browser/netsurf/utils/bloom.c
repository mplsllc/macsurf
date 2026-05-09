/*
 * Copyright 2013 Rob Kendrick <rjek@netsurf-browser.org>
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
 * Trivial bloom filter
 */

#include <stdlib.h>
#include "bloom.h"
#include "utils.h"

/* Forward declarations: bloom_insert_str / bloom_search_str call into
 * bloom_insert_hash / bloom_search_hash before they are defined below.
 * The declarations in bloom.h should be enough, but in this codebase
 * those headers are sometimes shadowed by libcss/src/select/bloom.h on
 * the CW8 access-path search, leaving the call sites with no visible
 * prototype and CW8 falls back to the C89 default-int. Re-stating the
 * prototypes here is a no-op when bloom.h was found correctly and a
 * rescue when it was not. */
void bloom_insert_hash(struct bloom_filter *b, uint32_t hash);
bool bloom_search_hash(struct bloom_filter *b, uint32_t hash);

/**
 * Hash a string, returning a 32bit value.  The hash algorithm used is
 * Fowler Noll Vo - a very fast and simple hash, ideal for short strings.
 * See http://en.wikipedia.org/wiki/Fowler_Noll_Vo_hash for more details.
 *
 * \param  datum   The string to hash.
 * \param  len	   size_t of data length.
 * \return The calculated hash value for the datum.
 */

static inline uint32_t fnv(const char *datum, size_t len)
{
	uint32_t z = 0x811c9dc5;
	
	if (datum == NULL)
		return 0;

	while (len--) {
		z *= 0x01000193;
		z ^= *datum++;
	}

	return z;
}

struct bloom_filter {
	size_t size;
	uint32_t items;
	uint8_t filter[FLEX_ARRAY_LEN_DECL];
};

struct bloom_filter *bloom_create(size_t size)
{
	struct bloom_filter *r = calloc(1, sizeof(*r) + size);
        
	if (r == NULL)
		return NULL;
        
	r->size = size;
        
	return r;
}

void bloom_destroy(struct bloom_filter *b)
{
        free(b);
}

void bloom_insert_str(struct bloom_filter *b, const char *s, size_t z)
{
	uint32_t hash = fnv(s, z);
	bloom_insert_hash(b, hash);
}

void bloom_insert_hash(struct bloom_filter *b, uint32_t hash)
{
	unsigned int index = hash % (b->size << 3);
	unsigned int byte_index = index >> 3;
	unsigned int bit_index = index & 7;

	b->filter[byte_index] |= (1 << bit_index);
	b->items++;
}

bool bloom_search_str(struct bloom_filter *b, const char *s, size_t z)
{
	uint32_t hash = fnv(s, z);
	return bloom_search_hash(b, hash);
}

bool bloom_search_hash(struct bloom_filter *b, uint32_t hash)
{
	unsigned int index = hash % (b->size << 3);
	unsigned int byte_index = index >> 3;
	unsigned int bit_index = index & 7;
	
	return (b->filter[byte_index] & (1 << bit_index)) != 0;
}

uint32_t bloom_items(struct bloom_filter *b)
{
	return b->items;
}
