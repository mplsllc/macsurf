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

#include "utils/errors.h"
#include "utils/log.h"

#include "macos9/macos9.h"

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

	while (sched_queue != NULL && sched_queue->time <= now) {
		entry = sched_queue;
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
