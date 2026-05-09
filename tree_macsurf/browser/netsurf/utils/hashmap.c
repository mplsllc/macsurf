/*
 * Copyright 2020 Daniel Silverstone <dsilvers@netsurf-browser.org>
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

#include <stdlib.h>
#include <string.h>

#include "utils/hashmap.h"

/**
 * The default number of buckets in the hashmaps we create.
 */
#define DEFAULT_HASHMAP_BUCKETS (4091)

/**
 * Hashmaps have chains of entries in buckets.
 */
typedef struct hashmap_entry_s {
	struct hashmap_entry_s **prevptr;
	struct hashmap_entry_s *next;
	void *key;
	void *value;
	uint32_t key_hash;
} hashmap_entry_t;

/**
 * The content of a hashmap
 */
struct hashmap_s {
	/**
	 * The parameters to be used for this hashmap
	 */
	hashmap_parameters_t *params;
	
	/**
	 * The buckets for the hash chains
	 */
	hashmap_entry_t **buckets;
	
	/**
	 * The number of buckets in this map
	 */
	uint32_t bucket_count;

	/**
	 * The number of entries in this map
	 */
	size_t entry_count;
};

/* Exported function, documented in hashmap.h */
hashmap_t *
hashmap_create(hashmap_parameters_t *params)
{
	hashmap_t *ret = malloc(sizeof(hashmap_t));
	if (ret == NULL) {
		return NULL;
	}

	ret->params = params;
	ret->bucket_count = DEFAULT_HASHMAP_BUCKETS;
	ret->entry_count = 0;
	ret->buckets = malloc(ret->bucket_count * sizeof(hashmap_entry_t *));

	if (ret->buckets == NULL) {
		free(ret);
		return NULL;
	}

	memset(ret->buckets, 0, ret->bucket_count * sizeof(hashmap_entry_t *));

	return ret;
}

/* Exported function, documented in hashmap.h */
void
hashmap_destroy(hashmap_t *hashmap)
{
	uint32_t bucket;
	hashmap_entry_t *entry;

	for (bucket = 0; bucket < hashmap->bucket_count; bucket++) {
		for (entry = hashmap->buckets[bucket];
		     entry != NULL;) {
			hashmap_entry_t *next = entry->next;
			hashmap->params->value_destroy(entry->value);
			hashmap->params->key_destroy(entry->key);
			free(entry);
			entry = next;
		}
	}

	free(hashmap->buckets);
	free(hashmap);
}

/* Exported function, documented in hashmap.h */
void *
hashmap_lookup(hashmap_t *hashmap, void *key)
{
	uint32_t hash = hashmap->params->key_hash(key);
	hashmap_entry_t *entry = hashmap->buckets[hash % hashmap->bucket_count];

	for(;entry != NULL; entry = entry->next) {
		if (entry->key_hash == hash) {
			if (hashmap->params->key_eq(key, entry->key)) {
				return entry->value;
			}
		}
	}

	return NULL;
}

/* Exported function, documented in hashmap.h */
void *
hashmap_insert(hashmap_t *hashmap, void *key)
{
	uint32_t hash = hashmap->params->key_hash(key);
	uint32_t bucket = hash % hashmap->bucket_count;
	hashmap_entry_t *entry = hashmap->buckets[bucket];
	void *new_key, *new_value;

	for(;entry != NULL; entry = entry->next) {
		if (entry->key_hash == hash) {
			if (hashmap->params->key_eq(key, entry->key)) {
				/* This key is already here */
				new_key = hashmap->params->key_clone(key);
				if (new_key == NULL) {
					/* Allocation failed */
					return NULL;
				}
				new_value = hashmap->params->value_alloc(entry->key);
				if (new_value == NULL) {
					/* Allocation failed */
					hashmap->params->key_destroy(new_key);
					return NULL;
				}
				hashmap->params->value_destroy(entry->value);
				hashmap->params->key_destroy(entry->key);
				entry->value = new_value;
				entry->key = new_key;
				return entry->value;
			}
		}
	}

	/* The key was not found in the map, so allocate a new entry */
	entry = malloc(sizeof(*entry));

	if (entry == NULL) {
		return NULL;
	}
	
	memset(entry, 0, sizeof(*entry));

	entry->key = hashmap->params->key_clone(key);
	if (entry->key == NULL) {
		goto err;
	}
	entry->key_hash = hash;

	entry->value = hashmap->params->value_alloc(entry->key);
	if (entry->value == NULL) {
		goto err;
	}

	entry->prevptr = &(hashmap->buckets[bucket]);
	entry->next = hashmap->buckets[bucket];
	if (entry->next != NULL) {
		entry->next->prevptr = &entry->next;
	}

	hashmap->buckets[bucket] = entry;

	hashmap->entry_count++;

	return entry->value;

err:
	if (entry->value != NULL)
		hashmap->params->value_destroy(entry->value);
	if (entry->key != NULL)
		hashmap->params->key_destroy(entry->key);
	free(entry);

	return NULL;
}

/* Exported function, documented in hashmap.h */
bool
hashmap_remove(hashmap_t *hashmap, void *key)
{
	uint32_t hash = hashmap->params->key_hash(key);
	
	hashmap_entry_t *entry = hashmap->buckets[hash % hashmap->bucket_count];

	for(;entry != NULL; entry = entry->next) {
		if (entry->key_hash == hash) {
			if (hashmap->params->key_eq(key, entry->key)) {
				hashmap->params->value_destroy(entry->value);
				hashmap->params->key_destroy(entry->key);
				if (entry->next != NULL) {
					entry->next->prevptr = entry->prevptr;
				}
				*entry->prevptr = entry->next;
				free(entry);
				hashmap->entry_count--;
				return true;
			}
		}
	}

	return false;
}

/* Exported function, documented in hashmap.h */
bool
hashmap_iterate(hashmap_t *hashmap, hashmap_iteration_cb_t cb, void *ctx)
{
	uint32_t bucket;
	hashmap_entry_t *entry;
	for (bucket = 0;
	     bucket < hashmap->bucket_count;
	     bucket++) {
		for (entry = hashmap->buckets[bucket];
		     entry != NULL;
		     entry = entry->next) {
			/* If the callback returns true, we early-exit */
			if (cb(entry->key, entry->value, ctx))
				return true;
		}
	}

	return false;
}

/* Exported function, documented in hashmap.h */
size_t
hashmap_count(hashmap_t *hashmap)
{
	return hashmap->entry_count;
}
