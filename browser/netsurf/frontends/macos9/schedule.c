/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * schedule.c — Cooperative scheduler using TickCount()
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
#include "macsurf_debug_log.h"		/* fixes406 -- UAF guard logging */

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

/**
 * Get current tick count.
 *
 * On Mac OS 9, TickCount() returns the number of ticks (1/60s) since
 * system boot — a monotonic timer suitable for scheduling.
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
			free(entry);
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

nserror
macos9_schedule(int t, void (*callback)(void *p), void *p)
{
	struct sched_entry *entry;
	struct sched_entry **queue;
	unsigned long now;
	unsigned long due;

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

	entry = malloc(sizeof(*entry));
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

/*
 * fixes406 -- guard against dispatching a freed/corrupted sched_entry.
 *
 * A use-after-free elsewhere (root trigger observed: the https-timeout
 * auto-upgrade FETCH_REDIRECT cascade in macos9_https_fetcher.c hctx_fail,
 * which re-enters NetSurf's llcache SYNCHRONOUSLY -- aborting the fetch,
 * building a replacement http fetch, and freeing the parent fetch all in
 * one nested call) can free a struct whose memory is then reused by a
 * later allocation that writes small integer values over it. If a
 * sched_entry's block is the one reused, its callback field is overwritten
 * with a small freelist/length value such as 0x00002800. macos9_schedule_run
 * then calls that as a function: "PowerPC illegal instruction at 00002800",
 * R3=0 (the universal p==NULL param), the hard crash we are chasing.
 *
 * On Mac OS 9 PPC every real callback is a CFM code pointer living high in
 * memory (observed: app code around 0x3Fxxxxxx / 0x40xxxxxx, app heap around
 * 0x001Axxxx); a 4-byte-aligned heap/code pointer is always >= 0x00010000.
 * Anything in the first 64 KB, or misaligned, is a smashed pointer. We check
 * BOTH the entry pointer (in case a corrupt entry's ->next dangled into the
 * queue) AND the callback, BEFORE any deref-and-call. On a hit we cannot
 * trust this entry's ->next either, so we drop the whole remaining queue and
 * log rather than branch through garbage. Pending callbacks are lost (the
 * in-flight fetch may stall), but the machine survives and NetSurf re-arms
 * fetcher_poll on the next navigation. The log line also confirms the
 * scheduler-UAF diagnosis and prints the smashed values for the next pass.
 */
#define SCHED_PTR_OK(x) \
	(((unsigned long)(x) & 0x3UL) == 0 && \
	 (unsigned long)(x) >= 0x00010000UL)

bool
macos9_schedule_run(void)
{
	struct sched_entry *entry;
	void (*callback)(void *p);
	void *p;
	unsigned long now;

	/* fixes146 -- during shutdown, NetSurf's teardown path frees
	 * state that scheduled callbacks may reference. Don't drive
	 * any more callbacks after macos9_quitting is set. */
	if (macos9_quitting) return false;

	now = macos9_get_ticks();

	while (sched_queue != NULL) {
		entry = sched_queue;

		/* fixes406 -- the entry pointer itself may be a wild value
		 * that dangled in via a previous entry's smashed ->next. */
		if (!SCHED_PTR_OK(entry)) {
			macsurf_debug_log_writef(
				"schedule: UAF guard -- wild queue ptr %p "
				"dropped (corruption upstream)", (void *)entry);
			sched_queue = NULL;
			break;
		}

		/* Check the callback BEFORE the time test so a smashed entry
		 * with a garbage (possibly huge) time field can't stall the
		 * head forever -- we want it dropped, not silently parked. */
		if (!SCHED_PTR_OK(entry->callback)) {
			macsurf_debug_log_writef(
				"schedule: UAF guard -- smashed callback %p "
				"(entry %p time=%ld p=%p next=%p) -- dropping "
				"queue", (void *)(unsigned long)entry->callback,
				(void *)entry, (long)entry->time, entry->p,
				(void *)entry->next);
			sched_queue = NULL;
			break;
		}

		if (entry->time > now) {
			break;	/* head not due; rest are later (sorted) */
		}

		callback = entry->callback;
		p = entry->p;
		sched_queue = entry->next;
		free(entry);

		/*
		 * The callback may call macos9_schedule(), so the queue
		 * must be in a consistent state before we invoke it.
		 * (Same safety pattern as RISC OS frontend.)
		 */
		callback(p);
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
