/*
 * MacSurf - Mac OS 9 frontend for NetSurf
 * macos9_content_registry.c - live-content pointer registry (anti-UAF)
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 *
 * See macos9_content_registry.h for the rationale.  Fixed-capacity array of
 * (content pointer, generation) slots.  No malloc. The authoritative table
 * remains a linear array, with an epoch-tagged pointer cache for repeated
 * lookups on hot paths such as box construction and JS geometry.
 * C89 / CW8-clean: all declarations at the top of their block,
 * no // comments, no C99 features.
 */

#include <stdlib.h>

#include "macsurf_debug.h"
#include "macos9_content_registry.h"

/* Capacity ceiling.  A loaded page plus its CSS / image / iframe / script
 * sub-contents rarely exceeds a few dozen live contents at once; 256 leaves
 * generous headroom.  If the table ever fills, register() logs and the new
 * content is simply not tracked - it then behaves as it did before this
 * registry existed (field-based guards remain as the fallback), so an
 * overflow degrades safety gracefully rather than crashing. */
#define MACOS9_CONTENT_REGISTRY_CAP 256

struct macos9_content_slot {
	struct content *c;	/* live content pointer, or NULL if free */
	unsigned long gen;	/* generation token for this occupancy */
};

static struct macos9_content_slot
	macos9_content_table[MACOS9_CONTENT_REGISTRY_CAP];

/* Monotonic generation source.  Starts at 1 so 0 is reserved to mean
 * "not registered" / "invalid token".  Wraps after ~4 billion registrations,
 * which is unreachable in any real session; even so a wrap only risks a
 * collision if a 4-billion-old token is still pending, which cannot happen. */
static unsigned long macos9_content_gen_next = 1;

/* Count of occupied slots, maintained for cheap logging only. */
static int macos9_content_live_count = 0;

/* fixes1077  -  registry EPOCH, bumped on every register/unregister.
 *
 * macos9_content_is_live() is a linear scan of a 256-entry table. That was
 * fine while its callers were rare (a reconvert, a teardown), but fixes1073
 * put it on the JS geometry path, where it runs on EVERY getBoundingClientRect
 * / offsetWidth / getComputedStyle a page performs. hackaday's navigation.js
 * measures relentlessly and went from 5.96s to 19.04s the moment geometry was
 * enabled - with the forced reflow never once firing, so the cost was pure
 * gate overhead buying nothing.
 *
 * The answer cannot change unless the table changes, so a caller can cache its
 * result against this counter and rescan only when it actually moves. */
unsigned long macos9_content_registry_epoch = 0;


/* Small epoch-tagged lookup cache.  The live table can change only when
 * macos9_content_registry_epoch changes, so a cached slot/absence from the
 * current epoch is exact.  Box construction asks about the same html_content
 * before and after every element; this turns those 256-slot scans into one
 * pointer/hash check after the first lookup in a build. */
#define MACOS9_CONTENT_FIND_CACHE 8
struct macos9_content_find_cache_entry {
	struct content *c;
	unsigned long epoch;
	int idx;
};
static struct macos9_content_find_cache_entry
	macos9_content_find_cache[MACOS9_CONTENT_FIND_CACHE];

/**
 * Find the slot index holding c, or -1 if absent.
 */
static int
macos9_content_find(struct content *c)
{
	unsigned long h;
	struct macos9_content_find_cache_entry *ce;
	int i;

	if (c == NULL)
		return -1;

	h = (((unsigned long)c) >> 4) & (MACOS9_CONTENT_FIND_CACHE - 1);
	ce = &macos9_content_find_cache[h];
	if (ce->c == c && ce->epoch == macos9_content_registry_epoch)
		return ce->idx;

	for (i = 0; i < MACOS9_CONTENT_REGISTRY_CAP; i++) {
		if (macos9_content_table[i].c == c) {
			ce->c = c;
			ce->epoch = macos9_content_registry_epoch;
			ce->idx = i;
			return i;
		}
	}

	ce->c = c;
	ce->epoch = macos9_content_registry_epoch;
	ce->idx = -1;
	return -1;
}


/* documented in macos9_content_registry.h */
void
macos9_content_register(struct content *c)
{
	int i;
	unsigned long g;

	if (c == NULL)
		return;

	/* Already present: keep the existing generation (idempotent). */
	if (macos9_content_find(c) >= 0)
		return;

	for (i = 0; i < MACOS9_CONTENT_REGISTRY_CAP; i++) {
		if (macos9_content_table[i].c == NULL) {
			g = macos9_content_gen_next++;
			if (macos9_content_gen_next == 0)
				macos9_content_gen_next = 1; /* skip 0 on wrap */

			macos9_content_table[i].c = c;
			macos9_content_table[i].gen = g;
			macos9_content_live_count++;
			macos9_content_registry_epoch++;	/* fixes1077 */
			return;
		}
	}

	/* Table full: do not track this content.  Field-based guards remain as
	 * the fallback for it.  Log so the ceiling can be raised if it ever
	 * actually happens on real pages. */
	macsurf_debug_log_writef(
		"content_registry: FULL (cap=%d), not tracking c=%p",
		MACOS9_CONTENT_REGISTRY_CAP, (void *)c);
}


/* documented in macos9_content_registry.h */
void
macos9_content_unregister(struct content *c)
{
	int idx;

	if (c == NULL)
		return;

	idx = macos9_content_find(c);
	if (idx < 0)
		return;

	macos9_content_table[idx].c = NULL;
	macos9_content_table[idx].gen = 0;
	if (macos9_content_live_count > 0)
		macos9_content_live_count--;
	macos9_content_registry_epoch++;		/* fixes1077 */
}


/* documented in macos9_content_registry.h */
int
macos9_content_is_live(struct content *c)
{
	return (macos9_content_find(c) >= 0) ? 1 : 0;
}


/* documented in macos9_content_registry.h */
unsigned long
macos9_content_token(struct content *c)
{
	int idx;

	idx = macos9_content_find(c);
	if (idx < 0)
		return 0;

	return macos9_content_table[idx].gen;
}


/* documented in macos9_content_registry.h */
int
macos9_content_token_valid(struct content *c, unsigned long token)
{
	int idx;

	if (token == 0)
		return 0;

	idx = macos9_content_find(c);
	if (idx < 0)
		return 0;

	return (macos9_content_table[idx].gen == token) ? 1 : 0;
}
