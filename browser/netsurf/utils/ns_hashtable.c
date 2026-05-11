/*
 * Copyright 2006 Rob Kendrick <rjek@rjek.com>
 * Copyright 2006 Richard Wilson <info@tinct.net>
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
 * Write-Once hash table for string to string mappings.
 *
 * This implementation is unit tested, if you make changes please
 * ensure the tests continute to pass and if possible, through
 * valgrind to make sure there are no memory leaks or invalid memory
 * accesses.  If you add new functionality, please include a test for
 * it that has good coverage along side the other tests.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <errno.h>

#include "utils/log.h"
#include "utils/errors.h"
#include "utils/ns_hashtable.h"


struct hash_entry {
	char *pairing;		 /**< block containing 'key\0value\0' */
	unsigned int key_length; /**< length of key */
	struct hash_entry *next; /**< next entry */
};

struct hash_table {
	unsigned int nchains;
	struct hash_entry **chain;
};

/** maximum length of line for file or inline add */
#define LINE_BUFFER_SIZE 512

/**
 * Hash a string, returning a 32bit value.  The hash algorithm used is
 * Fowler Noll Vo - a very fast and simple hash, ideal for short strings.
 * See http://en.wikipedia.org/wiki/Fowler_Noll_Vo_hash for more details.
 *
 * \param  datum   The string to hash.
 * \param  len	   Pointer to unsigned integer to record datum's length in.
 * \return The calculated hash value for the datum.
 */
static inline unsigned int hash_string_fnv(const char *datum, unsigned int *len)
{
	unsigned int z = 0x811c9dc5;
	const char *start = datum;
	*len = 0;

	if (datum == NULL)
		return 0;

	while (*datum) {
		z *= 0x01000193;
		z ^= *datum++;
	}
	*len = datum - start;

	return z;
}



/**
 * process a line of input.
 *
 * \param hash The hash table to add the line to
 * \param ln The line to process
 * \param lnlen The length of \ln
 * \return NSERROR_OK on success else NSERROR_INVALID
 */
static nserror
process_line(struct hash_table *hash, uint8_t *ln, int lnlen)
{
	uint8_t *key;
	uint8_t *value;
	uint8_t *colon;
	nserror res;

	key = ln; /* set key to start of line */
	value = ln + lnlen; /* set value to end of line */

	/* skip leading whitespace */
	while ((key < value) &&
	       ((*key == ' ') || (*key == '\t'))) {
		key++;
	}

	/* empty or comment lines */
	if ((*key == 0) || (*key == '#')) {
		return NSERROR_OK;
	}

	/* find first colon as key/value separator */
	for (colon = key; colon < value; colon++) {
		if (*colon == ':') {
			break;
		}
	}
	if (colon == value) {
		/* no colon found */
		return NSERROR_INVALID;
	}

	*colon = 0;  /* terminate key */
	value = colon + 1;

	res = hash_add(hash, (char *)key, (char *)value);
	if (res != NSERROR_OK) {
		nslog_log(__FILE__, "", __LINE__, "Unable to add %s:%s to hash table", ln, value);
	}
	return res;
}


/**
 * adds key/value pairs to a hash from a memory area
 */
static nserror
hash_add_inline_plain(struct hash_table *ht, const uint8_t *data, size_t size)
{
	uint8_t s[LINE_BUFFER_SIZE]; /* line buffer */
	unsigned int slen = 0;
	nserror res = NSERROR_OK;

	while (size > 0) {
		s[slen] = *data;

		if (s[slen] == '\n') {
			s[slen] = 0; /* replace newline with null termination */
			res = process_line(ht, s, slen);
			slen = 0;
			if (res != NSERROR_OK) {
				break;
			}
		} else {
			slen++;
			if (slen > sizeof s) {
				NSLOG(netsurf, INFO, "Overlength line\n");
				slen = 0;
			}
		}

		size--;
		data++;
	}
	if (slen > 0) {
		s[slen] = 0;
		res = process_line(ht, s, slen);
	}

	return res;
}

/**
 * adds key/value pairs to a hash from a compressed memory area
 */
static nserror
hash_add_inline_gzip(struct hash_table *ht, const uint8_t *data, size_t size)
{
	nserror res;
	int ret; /* zlib return value */
	z_stream strm;
	uint8_t s[LINE_BUFFER_SIZE]; /* line buffer */
	size_t used = 0; /* number of bytes in buffer in use */
	uint8_t *nl;

	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;

	strm.next_in = (uint8_t *)data;
	strm.avail_in = size;

	ret = inflateInit2(&strm, 32 + MAX_WBITS);
	if (ret != Z_OK) {
		nslog_log(__FILE__, "", __LINE__, "inflateInit returned %d", ret);
		return NSERROR_INVALID;
	}

	do {
		strm.next_out = s + used;
		strm.avail_out = sizeof(s) - used;

		ret = inflate(&strm, Z_NO_FLUSH);
		if ((ret != Z_OK) && (ret != Z_STREAM_END)) {
			break;
		}

		used = sizeof(s) - strm.avail_out;
		while (used > 0) {
			/* find nl */
			for (nl = &s[0]; nl < &s[used]; nl++) {
				if (*nl == '\n') {
					break;
				}
			}
			if (nl == &s[used]) {
				/* no nl found */
				break;
			}
			/* found newline */
			*nl = 0; /* null terminate line */
			res = process_line(ht, &s[0], nl - &s[0]);
			if (res != NSERROR_OK) {
				inflateEnd(&strm);
				return res;
			}

			/* move data down */
			memmove(&s[0], nl + 1, used - ((nl + 1) - &s[0]) );
			used -= ((nl +1) - &s[0]);
		}
		if (used == sizeof(s)) {
			/* entire buffer used and no newline */
			NSLOG(netsurf, INFO, "Overlength line");
			used = 0;
		}
	} while (ret != Z_STREAM_END);

	inflateEnd(&strm);

	if (ret != Z_STREAM_END) {
		nslog_log(__FILE__, "", __LINE__, "inflate returned %d", ret);
		return NSERROR_INVALID;
	}
	return NSERROR_OK;

}


/* exported interface documented in utils/hashtable.h */
struct hash_table *hash_create(unsigned int chains)
{
	struct hash_table *r = malloc(sizeof(struct hash_table));

	if (r == NULL) {
		NSLOG(netsurf, INFO, "Not enough memory for hash table.");
		return NULL;
	}

	r->nchains = chains;
	r->chain = calloc(chains, sizeof(struct hash_entry *));

	if (r->chain == NULL) {
		nslog_log(__FILE__, "", __LINE__,
			      "Not enough memory for %d hash table chains.",
			      chains);
		free(r);
		return NULL;
	}

	return r;
}


/* exported interface documented in utils/hashtable.h */
void hash_destroy(struct hash_table *ht)
{
	unsigned int i;

	if (ht == NULL)
		return;

	for (i = 0; i < ht->nchains; i++) {
		if (ht->chain[i] != NULL) {
			struct hash_entry *e = ht->chain[i];
			while (e) {
				struct hash_entry *n = e->next;
				free(e->pairing);
				free(e);
				e = n;
			}
		}
	}

	free(ht->chain);
	free(ht);
}


/* exported interface documented in utils/hashtable.h */
nserror hash_add(struct hash_table *ht, const char *key, const char *value)
{
	unsigned int h, c, v;
	struct hash_entry *e;

	if (ht == NULL || key == NULL || value == NULL)
		return NSERROR_BAD_PARAMETER;

	e = malloc(sizeof(struct hash_entry));
	if (e == NULL) {
		NSLOG(netsurf, INFO, "Not enough memory for hash entry.");
		return NSERROR_NOMEM;
	}

	h = hash_string_fnv(key, &(e->key_length));
	c = h % ht->nchains;

	v = strlen(value) ;
	e->pairing = malloc(v + e->key_length + 2);
	if (e->pairing == NULL) {
		NSLOG(netsurf, INFO,
		      "Not enough memory for string duplication.");
		free(e);
		return NSERROR_NOMEM;
	}
	memcpy(e->pairing, key, e->key_length + 1);
	memcpy(e->pairing + e->key_length + 1, value, v + 1);

	e->next = ht->chain[c];
	ht->chain[c] = e;

	return NSERROR_OK;
}


/* exported interface documented in utils/hashtable.h */
const char *hash_get(struct hash_table *ht, const char *key)
{
	unsigned int h, c, key_length;
	struct hash_entry *e;

	if (ht == NULL || key == NULL)
		return NULL;

	h = hash_string_fnv(key, &key_length);
	c = h % ht->nchains;

	for (e = ht->chain[c]; e; e = e->next) {
		if ((key_length == e->key_length) &&
		    (memcmp(key, e->pairing, key_length) == 0)) {
			return e->pairing + key_length + 1;
		}
	}

	return NULL;
}



/* exported interface documented in utils/hashtable.h */
nserror hash_add_file(struct hash_table *ht, const char *path)
{
	nserror res = NSERROR_OK;
	char s[LINE_BUFFER_SIZE]; /* line buffer */
	gzFile fp; /* compressed file handle */

	if (path == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	fp = gzopen(path, "r");
	if (!fp) {
		nslog_log(__FILE__, "", __LINE__,
			      "Unable to open file \"%.100s\": %s",
			      path,
			      strerror(errno));

		return NSERROR_NOT_FOUND;
	}

	while (gzgets(fp, s, sizeof s)) {
		int slen = strlen(s);
		s[--slen] = 0;  /* remove \n at end */

		res = process_line(ht, (uint8_t *)s, slen);
		if (res != NSERROR_OK) {
			break;
		}
	}

	gzclose(fp);

	return res;
}


/* exported interface documented in utils/hashtable.h */
nserror hash_add_inline(struct hash_table *ht, const uint8_t *data, size_t size)
{
	if ((data[0]==0x1f) && (data[1] == 0x8b)) {
		/* gzip header detected */
		return hash_add_inline_gzip(ht, data, size);
	}
	return hash_add_inline_plain(ht, data, size);
}
