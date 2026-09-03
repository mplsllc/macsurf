/*
 * MacSurf  -  Mac OS 9 frontend for NetSurf
 * schedule.c  -  Cooperative scheduler using TickCount()
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 *
 * Maintains an ordered linked list of timed callbacks, checked each
 * iteration of the WaitNextEvent loop. Timing uses TickCount()
 * (1 tick = 1/60th second). Follows the RISC OS frontend pattern.
 */

#include <stdlib.h>

#include "utils/ns_errors.h"
#include "utils/log.h"

#include "macos9.h"
#include "macsurf_debug.h"
#include "macos9_deathrow.h"

#ifdef __MACOS9__
#include <Timer.h>
#else
/* Linux build: stub for syntax checking */
static unsigned long stub_ticks = 0;
#endif

/** Default WaitNextEvent sleep when scheduler queue is empty (ticks). */
#define MACOS9_SCHED_IDLE_SLEEP 15

struct sched_entry {
	unsigned long time;		/* absolute time in ticks */
	void (*callback)(void *p);
	void *p;
	struct sched_entry *next;
};

static struct sched_entry *sched_queue = NULL;

bool macos9_sched_active = false;
unsigned long macos9_sched_time = 0;

/*
 * fixes570 - dedicated sched_entry pool.
 *
 * struct sched_entry is exactly 16 bytes with the SAME field offsets as
 * libdom's dom_string_internal (callback @off4 == cdata.ptr, p @off8 ==
 * cdata.len). While both draw from the general malloc free-list, a freed
 * dom_string block gets reused by a scheduled callback, so a still-referenced
 * (over-freed) dom_string reads a code-space callback pointer where its data
 * pointer should be -> the Script-Manager strlen crash (the whole fixes446g/
 * 489/569 family). Allocating sched_entry from a private pool that is NEVER
 * returned to the general heap makes that aliasing structurally impossible:
 * a dom_string malloc can never receive a sched_entry block and vice-versa.
 * Only the fatal code-pointer variant is removed; benign dom_string<->
 * dom_string reuse is still handled by the existing dom_string_data/
 * byte_length guards.
 */
#define MACOS9_SCHED_POOL_SIZE 256	/* 4 KB static; peak concurrent entries
					 * (JS timers + fetchers + gif + convert
					 * reschedule) stays well under this */
static struct sched_entry sched_pool[MACOS9_SCHED_POOL_SIZE];
static struct sched_entry *sched_freelist = NULL;
static int sched_pool_inited = 0;

static void
sched_pool_init(void)
{
	int i;
	for (i = 0; i < MACOS9_SCHED_POOL_SIZE; i++) {
		sched_pool[i].next = sched_freelist;
		sched_freelist = &sched_pool[i];
	}
	sched_pool_inited = 1;
}

static struct sched_entry *
sched_entry_alloc(void)
{
	struct sched_entry *e;

	if (sched_pool_inited == 0) {
		sched_pool_init();
	}
	if (sched_freelist == NULL) {
		/* Pool exhausted: rare; fall back to malloc (this one entry
		 * can alias, but the pool is sized so this ~never happens). */
		extern void macsurf_debug_log_writef(const char *fmt, ...);
		macsurf_debug_log_writef("sched: POOL EXHAUSTED -> malloc fallback");
		return (struct sched_entry *) malloc(sizeof(struct sched_entry));
	}
	e = sched_freelist;
	sched_freelist = e->next;
	return e;
}

static void
sched_entry_free(struct sched_entry *e)
{
	unsigned long a = (unsigned long) e;

	if (a >= (unsigned long) &sched_pool[0] &&
	    a <  (unsigned long) &sched_pool[MACOS9_SCHED_POOL_SIZE]) {
		/* Pool entry: return to the private free-list, NOT the heap. */
		e->next = sched_freelist;
		sched_freelist = e;
	} else {
		/* Was a malloc fallback entry. */
		free(e);
	}
}

/*
 * fixes584 - scheduler freeze-proofing.
 *
 * The queue is a plain singly-linked list. A corrupted ->next must never turn
 * a defensive walk into an infinite loop. MACOS9_SCHED_WALK_MAX bounds every
 * full queue walk; the hot scheduler paths do cycle detection while doing the
 * useful search/removal/insertion rather than making a separate O(n) guard
 * pass first. This keeps the corruption recovery guarantee without paying an
 * extra list traversal on every schedule call and every event-loop tick.
 */
#define MACOS9_SCHED_WALK_MAX (MACOS9_SCHED_POOL_SIZE + 64)
/* Per-tick callback dispatch ceiling - far above any legitimate burst. */
#define MACOS9_SCHED_DRAIN_MAX 8192

static void
sched_hard_reset(void)
{
	int i;

	/* Abandon the (cyclic) queue wholesale and rebuild the pool free-list.
	 * Abandoned pool slots become reachable again via the fresh free-list;
	 * any malloc-fallback entries in the old queue leak (rare, bounded). */
	sched_queue = NULL;
	sched_freelist = NULL;
	for (i = 0; i < MACOS9_SCHED_POOL_SIZE; i++) {
		sched_pool[i].next = sched_freelist;
		sched_freelist = &sched_pool[i];
	}
	sched_pool_inited = 1;
	macos9_sched_active = false;
	macos9_sched_time = 0;
	macsurf_debug_log_writef(
		"sched: QUEUE CORRUPT (cycle) - hard reset, pending callbacks dropped");
}

/**
 * Get current tick count.
 *
 * On Mac OS 9, TickCount() returns the number of ticks (1/60s) since
 * system boot - a monotonic timer suitable for scheduling.
 */
static unsigned long
macos9_get_ticks(void)
{
#ifdef __MACOS9__
	return (unsigned long)TickCount();
#else
	return stub_ticks;
#endif
}

/**
 * Update exported state from queue head.
 */
static void
sched_update_state(void)
{
	if (sched_queue != NULL) {
		macos9_sched_active = true;
		macos9_sched_time = sched_queue->time;
	} else {
		macos9_sched_active = false;
	}
}

/**
 * fixes517: UNIVERSAL scheduled-callback cancellation by owner pointer.
 *
 * Every crash in the "callback outlives its object" family is a
 * guit->misc->schedule(delay, fn, ctx) entry whose ctx was freed before
 * fn fired (box conversion, deferred parser unpause, object refresh, ...).
 * Cancelling one callback at a time (schedule(-1, fn, ctx)) requires the
 * teardown code to know every fn that might be queued for that object -
 * and it never does, which is why the crashes keep reappearing at new
 * sites. This removes EVERY queued entry whose ->p matches, regardless of
 * callback. Call it once from the object's destroy path and no scheduled
 * callback can ever fire against the freed object again - including
 * callbacks added by future code, as long as they pass the object as p.
 *
 * Safe to call from inside a running callback: macos9_schedule_run() has
 * already unlinked the in-flight entry from sched_queue before invoking it,
 * so the entry being executed is never in the list this walk touches.
 */
void
macos9_schedule_cancel_owner(void *p)
{
	struct sched_entry **prev = &sched_queue;
	struct sched_entry *entry;
	int removed = 0;
	int walked = 0;

	while (*prev != NULL) {
		if (++walked > MACOS9_SCHED_WALK_MAX) {
			sched_hard_reset();
			return;
		}
		entry = *prev;
		if (entry->p == p) {
			*prev = entry->next;
			/* MUST use sched_entry_free (pool-aware); a plain free() on a
			 * pool slot corrupts the Memory Manager arena. */
			sched_entry_free(entry);
			removed++;
		} else {
			prev = &entry->next;
		}
	}

	if (removed != 0) {
		macsurf_debug_log_writef(
			"sched_cancel_owner: dropped %d callback(s) for owner=%p",
			removed, p);
	}

	sched_update_state();
}

nserror
macos9_schedule(int t, void (*callback)(void *p), void *p)
{
	struct sched_entry *entry;
	struct sched_entry *cur;
	struct sched_entry **queue;
	struct sched_entry **insert_at;
	unsigned long now;
	unsigned long due;
	int walked;

	/* Cancellation is one bounded pass. The old path first performed a full
	 * corruption-guard walk and then a second removal walk. */
	if (t < 0) {
		queue = &sched_queue;
		walked = 0;
		while (*queue != NULL) {
			if (++walked > MACOS9_SCHED_WALK_MAX) {
				sched_hard_reset();
				return NSERROR_OK;
			}
			cur = *queue;
			if (cur->callback == callback && cur->p == p) {
				*queue = cur->next;
				sched_entry_free(cur);
				break;
			}
			queue = &cur->next;
		}
		sched_update_state();
		return NSERROR_OK;
	}

	now = macos9_get_ticks();

	/* Convert ms to ticks: 1 tick = 1/60s ~= 16.67ms */
	due = now + ((unsigned long)t * 60 / 1000);

	/* One bounded queue pass does all three jobs that previously needed
	 * separate traversals: corruption detection, duplicate removal and
	 * locating the sorted insertion point. */
	queue = &sched_queue;
	insert_at = NULL;
	walked = 0;
	while (*queue != NULL) {
		if (++walked > MACOS9_SCHED_WALK_MAX) {
			sched_hard_reset();
			queue = &sched_queue;
			insert_at = queue;
			break;
		}
		cur = *queue;
		if (insert_at == NULL && cur->time > due) {
			insert_at = queue;
		}
		if (cur->callback == callback && cur->p == p) {
			*queue = cur->next;
			sched_entry_free(cur);
			continue;
		}
		queue = &cur->next;
	}
	if (insert_at == NULL) {
		insert_at = queue;
	}

	entry = sched_entry_alloc();
	if (entry == NULL) {
		NSLOG(netsurf, INFO, "malloc failed for scheduler entry");
		return NSERROR_NOMEM;
	}

	entry->time = due;
	entry->callback = callback;
	entry->p = p;
	entry->next = *insert_at;
	*insert_at = entry;

	sched_update_state();

	return NSERROR_OK;
}

/* fixes1148 - check whether a (callback, p) pair is already queued,
 * without modifying the queue. Used by the reconvert path to avoid
 * rescheduling on every DOM mutation when the callback is already pending. */
int macos9_sched_is_queued(void (*callback)(void *p), void *p)
{
	struct sched_entry *entry;
	int walked = 0;

	for (entry = sched_queue; entry != NULL; entry = entry->next) {
		if (++walked > MACOS9_SCHED_WALK_MAX) {
			sched_hard_reset();
			return 0;
		}
		if (entry->callback == callback && entry->p == p)
			return 1;
	}
	return 0;
}

bool
macos9_schedule_run(void)
{
	struct sched_entry *entry;
	void (*callback)(void *p);
	void *p;
	unsigned long now;
	long drained;

	/* fixes146 - during shutdown, NetSurf's teardown path frees
	 * state that scheduled callbacks may reference. Don't drive
	 * any more callbacks after macos9_quitting is set. */
	if (macos9_quitting) return false;

	/* Do not pre-walk the whole queue here. schedule_run is called every
	 * event-loop turn; the head check below is O(1), and if a corrupt cycle
	 * ever becomes runnable the existing dispatch ceiling hard-resets it.
	 * Bounded full walks in schedule/cancel/is_queued catch corruption while
	 * doing useful work instead of charging every idle loop pass. */
	now = macos9_get_ticks();

	/* Belt-and-suspenders: a callback could corrupt the queue into a cycle
	 * mid-drain. Cap dispatch per tick far above any legitimate burst. */
	drained = 0;
	while (sched_queue != NULL && sched_queue->time <= now) {
		if (++drained > MACOS9_SCHED_DRAIN_MAX) {
			sched_hard_reset();
			break;
		}
		entry = sched_queue;
		callback = entry->callback;
		p = entry->p;
		sched_queue = entry->next;
		/* fixes446d: entry is already removed from the queue above,
		 * so re-scheduling the same callback inside callback() cannot alias
		 * this entry. MUST free AFTER callback: sched_entry is the same size
		 * and layout as dom_string_internal, and free-before-callback let a
		 * new dom_string reuse the block while callback/p were still live. */
		/* Stage 1: a scheduled callback is engine work; mark operation depth
		 * non-zero so the death-row drain cannot free while it is on stack. */
		macos9_op_depth++;
		callback(p);
		macos9_op_depth--;
		sched_entry_free(entry);
	}

	sched_update_state();

	return macos9_sched_active;
}

int
macos9_get_next_delay(void)
{
	unsigned long now;
	long delta;

	if (sched_queue == NULL) {
		return MACOS9_SCHED_IDLE_SLEEP;
	}

	now = macos9_get_ticks();
	delta = (long)(sched_queue->time - now);

	if (delta <= 0) {
		return 0;
	}

	return (int)delta;
}

/*
 * Stage 1 death-row support: visit every pending scheduled entry's
 * (callback, param) and return true if `pred` matches any. Used by the
 * death-row drain to keep a content alive while a scheduled continuation
 * (convert_xml_to_box / html_css_process_modified_styles /
 * html_object_refresh) still references it. Read-only over the queue.
 */
bool
macos9_sched_any(bool (*pred)(void (*cb)(void *), void *p, void *arg),
		void *arg)
{
	struct sched_entry *e;
	int walked = 0;

	for (e = sched_queue; e != NULL; e = e->next) {
		if (++walked > MACOS9_SCHED_WALK_MAX) {
			sched_hard_reset();
			return false;
		}
		if (pred(e->callback, e->p, arg)) {
			return true;
		}
	}
	return false;
}