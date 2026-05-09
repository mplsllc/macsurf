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

#ifndef NETSURF_HASHMAP_H
#define NETSURF_HASHMAP_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Generic hashmap.
 *
 * Hashmaps take ownership of the keys inserted into them by means of a
 * clone function in their parameters.  They also manage the value memory
 * directly.
 */
typedef struct hashmap_s hashmap_t;

/**
 * Key cloning function type
 */
typedef void* (*hashmap_key_clone_t)(void *);

/**
 * Key destructor function type
 */
typedef void (*hashmap_key_destroy_t)(void *);

/**
 * Key hashing function type
 */
typedef uint32_t (*hashmap_key_hash_t)(void *);

/**
 * Key comparison function type
 */
typedef bool (*hashmap_key_eq_t)(void *, void*);

/**
 * Value allocation function type
 */
typedef void* (*hashmap_value_alloc_t)(void *);

/**
 * Value destructor function type
 */
typedef void (*hashmap_value_destroy_t)(void *);

/**
 * Hashmap iteration callback function type.
 *
 * First parameter is the key, second is the value.
 * The final parameter is the context pointer for the iteration.
 *
 * Return true to stop iterating early
 */
typedef bool (*hashmap_iteration_cb_t)(void *, void *, void *);

/**
 * Parameters for hashmaps
 */
typedef struct {
	/**
	 * A function which when called will clone a key and give
	 * ownership of the returned object to the hashmap
	 */
	hashmap_key_clone_t key_clone;

	/**
	 * A function which when given a key will return its hash.
	 */
	hashmap_key_hash_t key_hash;
	
	/**
	 * A function to compare two keys and return if they are equal.
	 * Note: identity is not necessary, nor strict equality, so long
	 * as the function is a full equality model.
	 * (i.e. key1 == key2 => key2 == key1)
	 */
	hashmap_key_eq_t key_eq;
	 
	/**
	 * A function which when called will destroy a key object
	 */
	hashmap_key_destroy_t key_destroy;

	/**
	 * A function which when called will allocate a value object
	 */
	hashmap_value_alloc_t value_alloc;

	/**
	 * A function which when called will destroy a value object
	 */
	hashmap_value_destroy_t value_destroy;
} hashmap_parameters_t;


/**
 * Create a hashmap
 *
 * The provided hashmap parameter table will be used for map operations
 * which need to allocate/free etc.
 *
 * \param params The hashmap parameters for this map
 */
hashmap_t* hashmap_create(hashmap_parameters_t *params);

/**
 * Destroy a hashmap
 *
 * After this, all keys and values will have been destroyed and all memory
 * associated with this hashmap will be invalidated.
 *
 * \param hashmap The hashmap to destroy
 */
void hashmap_destroy(hashmap_t *hashmap);

/**
 * Look up a key in a hashmap
 *
 * If the key has an associated value in the hashmap then the pointer to it
 * is returned, otherwise NULL.
 *
 * \param hashmap The hashmap to look up the key inside
 * \param key The key to look up in the hashmap
 * \return A pointer to the value if found, NULL otherwise
 */
void* hashmap_lookup(hashmap_t *hashmap, void *key);

/**
 * Create an entry in a hashmap
 *
 * This creates a blank value using the parameters and then associates it with
 * a clone of the given key, inserting it into the hashmap.  If a value was
 * present for the given key already, then it is destroyed first.
 *
 * NOTE: If allocation of the new value object fails, then any existing entry
 * will be left alone, but NULL will be returned.
 *
 * \param hashmap The hashmap to insert into
 * \param key The key to insert an entry for
 * \return The value pointer for that key, or NULL if allocation failed.
 */
void *hashmap_insert(hashmap_t *hashmap, void *key);

/**
 * Remove an entry from the hashmap
 *
 * This will remove the entry for the given key from the hashmap
 * If there is no such entry, this will safely do nothing.
 * The value associated with the entry will be destroyed and so should not
 * be used beyond calling this function.
 *
 * \param hashmap The hashmap to remove the entry from
 * \param key The key to remove the entry for
 * \return true if an entry was removed, false otherwise
 */
bool hashmap_remove(hashmap_t *hashmap, void *key);

/**
 * Iterate the hashmap
 *
 * For each key/value pair in the hashmap, call the callback passing in
 * the key and value.  During iteration you MUST NOT mutate the hashmap.
 *
 * \param hashmap The hashmap to iterate
 * \param cb The callback for each key,value pair
 * \param ctx The callback context
 * \return Whether or not we stopped iteration early
 */
bool hashmap_iterate(hashmap_t *hashmap, hashmap_iteration_cb_t cb, void *ctx);

/**
 * Get the number of entries in this map
 *
 * \param hashmap The hashmap to retrieve the entry count from
 * \return The number of entries in the hashmap
 */
size_t hashmap_count(hashmap_t *hashmap);

#endif
