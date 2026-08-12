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
 * fixes584  -  scheduler freeze-proofing.
 *
 * The sched_queue is a plain singly-linked list. If memory corruption (the
 * open UAF family) ever writes a ->next that points back into the list, EVERY
 * walk of the queue (sched_remove, the time-sorted insert in macos9_schedule,
 * the drain in macos9_schedule_run) spins forever and the whole cooperative
 * event loop hard-freezes with no crash  -  exactly the tinkerdifferent symptom
 * (frozen, blank, 'evloop: hb' stops while 'qjs: interrupt hb' never fires;
 * the last log line is inside macos9_schedule_unpause -> macos9_schedule).
 *
 * A scheduler must never be able to infinite-loop, regardless of what
 * corrupted it. sched_queue_is_cyclic() walks at most a hard bound (well above
 * any legitimate queue length: the pool caps concurrent entries at
 * MACOS9_SCHED_POOL_SIZE) and reports a cycle. sched_hard_reset() abandons the
 * corrupted queue and rebuilds the free-list to pristine  -  the current caller
 * then proceeds on a clean queue, so the in-flight callback (e.g. the deferred
 * parser unpause) is still scheduled and the page load can continue past the
 * corruption instead of wedging the machine.
 */
#define MACOS9_SCHED_WALK_MAX (MACOS9_SCHED_POOL_SIZE + 64)
/* Per-tick callback dispatch ceiling  -  far above any legitimate burst. */
#define MACOS9_SCHED_DRAIN_MAX 8192

static int
sched_queue_is_cyclic(void)
{
	struct sched_entry *e = sched_queue;
	int n = 0;

	while (e != NULL) {
		if (++n > MACOS9_SCHED_WALK_MAX) {
			return 1;
		}
		e = e->next;
	}
	return 0;
}

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

/* Detect-and-recover guard, called before any sched_queue walk. */
static void
sched_guard(void)
{
	if (sched_queue_is_cyclic()) {
		sched_hard_reset();
	}
}

/**
 * Get current tick count.
 *
 * On Mac OS 9, TickCount() returns the number of ticks (1/60s) since
 * system boot  -  a monotonic timer suitable for scheduling.
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
 * Remove any queued entry matching callback+p.
 *
 * There can only be one entry per callback+param pair, since
 * macos9_schedule() removes duplicates before inserting.
 */
static void
sched_remove(void (*callback)(void *p), void *p)
{
	struct sched_entry **prev = &sched_queue;
	struct sched_entry *entry;

	while (*prev != NULL) {
		entry = *prev;
		if (entry->callback == callback && entry->p == p) {
			*prev = entry->next;
			sched_entry_free(entry);
			return;
		}
		prev = &entry->next;
	}
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
 * teardown code to know every fn that might be queued for that object  - 
 * and it never does, which is why the crashes keep reappearing at new
 * sites.  This removes EVERY queued entry whose ->p matches, regardless of
 * callback.  Call it once from the object's destroy path and no scheduled
 * callback can ever fire against the freed object again  -  including
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

	/* fixes584  -  never walk a cyclic queue (freeze-proof). */
	sched_guard();

	while (*prev != NULL) {
		entry = *prev;
		if (entry->p == p) {
			*prev = entry->next;
			/* fixes584  -  MUST use sched_entry_free (pool-aware); a
			 * plain free() on a pool slot (static array address)
			 * corrupts the Memory Manager arena and is itself a
			 * prime suspect for the very cycle this guards against. */
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
	struct sched_entry **queue;
	unsigned long now;
	unsigned long due;

	/* fixes584  -  never walk a cyclic queue (freeze-proof). */
	sched_guard();

	/* Always remove any existing entry for this callback+param */
	sched_remove(callback, p);

	/* Negative t means cancel only */
	if (t < 0) {
		sched_update_state();
		return NSERROR_OK;
	}

	now = macos9_get_ticks();

	/* Convert ms to ticks: 1 tick = 1/60s ≈ 16.67ms */
	due = now + ((unsigned long)t * 60 / 1000);

	entry = sched_entry_alloc();
	if (entry == NULL) {
		NSLOG(netsurf, INFO, "malloc failed for scheduler entry");
		return NSERROR_NOMEM;
	}

	entry->time = due;
	entry->callback = callback;
	entry->p = p;

	/* Insert in time-sorted order */
	queue = &sched_queue;
	while (*queue != NULL && (*queue)->time <= due) {
		queue = &(*queue)->next;
	}
	entry->next = *queue;
	*queue = entry;

	sched_update_state();

	return NSERROR_OK;
}

/* fixes1148  -  check whether a (callback, p) pair is already queued,
 * without modifying the queue. Used by the reconvert path to avoid
 * rescheduling on every DOM mutation when the callback is already
 * pending. */
int macos9_sched_is_queued(void (*callback)(void *p), void *p)
{
	struct sched_entry *entry;

	for (entry = sched_queue; entry != NULL; entry = entry->next) {
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

	/* fixes584  -  never drain a cyclic queue (freeze-proof). */
	sched_guard();

	now = macos9_get_ticks();

	/* fixes584  -  belt-and-suspenders: a callback could corrupt the queue
	 * into a cycle mid-drain (after the entry guard ran). Cap the number of
	 * callbacks dispatched per tick far above any legitimate burst; if the
	 * cap is hit, reset and bail rather than spin the machine. */
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
		 * so re-scheduling the same callback inside callback() calls
		 * sched_remove() as a no-op - delayed free is safe.
		 * MUST free AFTER callback: struct sched_entry (16 bytes,
		 * callback at offset 4) is the same size and layout as
		 * dom_string_internal. free-before-callback let malloc return
		 * entry's memory for a new dom_string_internal, aliasing
		 * entry->callback with data.cdata.ptr.  dom_string_intern then
		 * called lwc_intern_string(draw_url_bar, gui_window_ptr) which
		 * scanned megabytes of code into unmapped memory and crashed. */
		/* Stage 1: a scheduled callback (convert_xml_to_box,
		 * hlcache_clean, reformat, JS timers...) is engine work; mark
		 * the operation depth non-zero so the death-row drain cannot
		 * free anything while this callback is on the stack. */
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

	for (e = sched_queue; e != NULL; e = e->next) {
		if (pred(e->callback, e->p, arg)) {
			return true;
		}
	}
	return false;
}
