/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * macos9_deathrow.c — Stage 1 quiescent-point deferred-free.
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 *
 * See macos9_deathrow.h for the design and the root cause it closes.
 *
 * Generic by construction: each record carries the object pointer, a
 * teardown function supplied by the object's owning file (so the private
 * hlcache_entry / llcache_object structs never need to be visible here),
 * and a `pin_key` content whose pending scheduled continuations must have
 * drained before the free may happen (Seam 2). C89 / CW8-clean.
 */

#include <stdlib.h>

#include "utils/ns_errors.h"

#include "macos9_deathrow.h"
#include "macsurf_debug.h"

/* Operation-depth counter (see header). 0 == quiescent. */
int macos9_op_depth = 0;

/*
 * Fixed record table. Sized far above the live-content count of any real
 * page (a 50-avatar 68kmla thread is ~hundreds). The table is drained
 * every macos9_poll turn, so occupancy is bounded by one loop turn's
 * teardowns. On the (implausible) overflow we LEAK the object rather than
 * free it early: a leak is bounded and harmless, a mid-walk free is the
 * bug we are removing. Overflow is logged.
 */
#define MACOS9_DEATHROW_MAX 1024

/* Bump-log a still-pinned record once it has survived this many drains.
 * ~120 loop turns ≈ a couple of seconds of 60Hz polling: long past any
 * bounded convert/css/object-list reschedule, so a hit means a real
 * continuation-orphan leak to chase, not normal operation. */
#define MACOS9_DEATHROW_STALE_PASSES 120

struct deathrow_rec {
	void *ptr;
	void (*teardown)(void *ptr);
	struct content *pin_key;	/* content to gate on, or NULL */
	unsigned int pin_passes;
	int active;
	int stale_logged;
};

static struct deathrow_rec dr_table[MACOS9_DEATHROW_MAX];
static int dr_count = 0;		/* high-water slot in use */
static int dr_in_drain = 0;	/* re-entrancy guard for the drain */

void
macos9_deathrow_add(void *ptr, void (*teardown)(void *ptr),
		struct content *pin_key)
{
	int i;

	if (ptr == NULL || teardown == NULL) {
		return;
	}

	/* Find a free slot up to the high-water mark, else extend. */
	for (i = 0; i < dr_count; i++) {
		if (dr_table[i].active == 0) {
			break;
		}
	}

	if (i == dr_count) {
		if (dr_count >= MACOS9_DEATHROW_MAX) {
			/* Overflow: leak (safe) and log once per event. */
			macsurf_debug_log_writef(
				"deathrow: OVERFLOW ptr=%p (leaked, raise MAX)",
				ptr);
			return;
		}
		dr_count++;
	}

	dr_table[i].ptr = ptr;
	dr_table[i].teardown = teardown;
	dr_table[i].pin_key = pin_key;
	dr_table[i].pin_passes = 0;
	dr_table[i].stale_logged = 0;
	dr_table[i].active = 1;
}

/*
 * Pinned-check predicate: true if (cb,p) is one of the three content-
 * holding scheduled continuations and it references `arg` (the content).
 */
static bool
deathrow_pin_pred(void (*cb)(void *), void *p, void *arg)
{
	struct content *target = (struct content *)arg;

	if (box_construct_sched_pins(cb, p, target)) {
		return true;
	}
	if (html_css_sched_pins(cb, p, target)) {
		return true;
	}
	/* NOTE: html_object_sched_pins (the html_object_refresh continuation
	 * pin) is intentionally NOT dispatched here. Its host, object.c, would
	 * not export the new symbol under CW8 (box_construct.c / html_css.c
	 * did). Object-refresh continuations are already cancelled by
	 * html_destroy on teardown, so the dominant teardown-during-convert
	 * and CSS cases stay covered. Re-add via a uniquely-named host if a
	 * hardware crash ever implicates html_object_refresh-into-freed. */
	return false;
}

static bool
macos9_deathrow_pinned(struct content *c)
{
	if (c == NULL) {
		return false;
	}
	return macos9_sched_any(deathrow_pin_pred, (void *)c);
}

void
macos9_deathrow_drain(void)
{
	int i;

	/* Only at the quiescent point, and never re-entrantly. */
	if (macos9_op_depth != 0 || dr_in_drain) {
		return;
	}

	dr_in_drain = 1;

	for (i = 0; i < dr_count; i++) {
		struct deathrow_rec *r = &dr_table[i];

		if (r->active == 0) {
			continue;
		}

		/* Seam 2: defer while a scheduled continuation still
		 * references this object's content. */
		if (r->pin_key != NULL && macos9_deathrow_pinned(r->pin_key)) {
			r->pin_passes++;
			if (r->stale_logged == 0 &&
			    r->pin_passes >= MACOS9_DEATHROW_STALE_PASSES) {
				macsurf_debug_log_writef(
					"deathrow: STALE PIN ptr=%p content=%p passes=%u",
					r->ptr, (void *)r->pin_key,
					r->pin_passes);
				r->stale_logged = 1;
			}
			continue;
		}

		/* Free for real. Mark the slot free FIRST so a teardown
		 * that re-enqueues (sub-resource frees) can reuse it. */
		r->active = 0;
		r->teardown(r->ptr);
	}

	dr_in_drain = 0;
}
