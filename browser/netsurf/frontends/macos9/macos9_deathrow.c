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

/* fixes914 -- the death-row entry currently being torn down, or NULL when the
 * drain is not inside a teardown. Two plain stores per entry, no I/O: a crash
 * reporter that can still run (talloc's TALLOC_ABORT does) prints these so a
 * teardown that never returns names itself. Replaces the fixes911 per-entry
 * log lines, which cost a FlushVol each and still missed the victim when it
 * fell past the line cap. */
void *macos9_deathrow_cur_ptr = NULL;
void *macos9_deathrow_cur_fn = NULL;

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

/* fixes911 — how many individual "LIFE deathrow free" lines one drain may emit
 * before falling back to the drained= total alone. See the call site. */
#define MACOS9_DEATHROW_TRACE_MAX 32

/* fixes913 — per-ENTRY tracing is OFF by default. fixes911 turned it on to name
 * the victim of a teardown that never returns, and it worked, but the cost was
 * mispriced: one hardware session emitted 3035 of its 3289 log lines here, and
 * debug builds FlushVol every kept line (~10-50ms of HFS sync each) -- tens of
 * seconds of stall across a session, on a browser already reported as sluggish.
 *
 * It also answered its question. The reconvert-window guard check it existed to
 * test came back CLEAN on that log (zero death-row frees inside an
 * in_progress=1..0 window, every seq balanced), so the per-entry stream is now
 * paying a large cost to re-confirm a negative. The per-drain "drained=" total
 * stays -- it is one line per drain and still shows the shape of the churn.
 *
 * Re-enable by defining this when a teardown crash needs its victim named. */
/* #define MACOS9_DEATHROW_TRACE_ENTRIES 1 */

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

	/* fixes955 — REFUSE a pointer already on the row.
	 *
	 * Until now the only thing stopping the same object being queued twice
	 * was the caller's dr_queued flag -- which lives INSIDE the object being
	 * freed. That makes idempotence depend on every caller getting its own
	 * bookkeeping right, and on that field being initialised (it was not:
	 * hlcache's main entry path used malloc, see fixes955 in hlcache.c).
	 * Queue a pointer twice and the drain frees it twice; a double free
	 * corrupts the MSL free list and the crash lands INSIDE free(), far from
	 * the cause -- which is exactly the shape seen here:
	 *   free <- hlcache_node_deathrow_teardown <- macos9_deathrow_drain
	 * and the same shape fixes906 recorded ("double-free -> corrupted free
	 * list -> the Block_link crash in the death-row hlcache teardown").
	 *
	 * The row is the single choke point every deferred free passes through,
	 * so enforcing uniqueness HERE closes the whole class no matter which
	 * caller is buggy. Linear scan over <=1024 slots on a free is cheap
	 * against a corrupted heap. */
	for (i = 0; i < dr_count; i++) {
		if (dr_table[i].active != 0 && dr_table[i].ptr == ptr) {
			macsurf_debug_log_writef(
				"deathrow: DUPLICATE ptr=%p already queued slot=%d -- refused",
				ptr, i);
			return;
		}
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
	int freed = 0;	/* fixes911 — lifecycle trail */

	/* Only at the quiescent point, and never re-entrantly. */
	if (macos9_op_depth != 0 || dr_in_drain) {
		return;
	}

	/* fixes901 — NEVER free while a reconvert box build is in flight. The
	 * reconvert yields between batches, and op_depth returns to 0 in that gap,
	 * so a deferred free draining here -- e.g. a blocked tracker/image
	 * content_destroy on hackaday -- ran content teardown against a half-rebuilt
	 * tree (the box-node-80 crash the durable ReconvPos marker points at). Hold
	 * every deferred free until html_reconvert_done clears the flag; they drain
	 * on the first poll after. Delaying a deferred free is always safe -- the
	 * object was already condemned; only the timing changes. */
	{
		extern int macsurf_reconvert_in_progress;
		if (macsurf_reconvert_in_progress) {
			return;
		}
	}

	/* fixes906 — REMOVED the fixes901 poll-level macos9_content_drain_deferred()
	 * call that used to sit here. It ran content_destroy() (broadcasts,
	 * callbacks, frees) BEFORE dr_in_drain was set, so a re-entrant
	 * macos9_deathrow_drain (via that callback chain) saw dr_in_drain==0,
	 * proceeded, and drained dr_table -- then the outer drain drained it AGAIN
	 * -> double-free -> corrupted free list -> the Block_link crash in the
	 * death-row hlcache teardown (CW stack, iMac). It was also redundant: with
	 * the SYNCHRONOUS reconvert (fixes903) the convert wrapper
	 * (convert_xml_to_box) drains the deferred-content list itself after the
	 * build, with in_progress already cleared by html_reconvert_done. So the
	 * death row must ONLY ever run its own dr_table free -- never re-enter
	 * content teardown. */
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
					/* fixes927 — %ld, not %u (unsupported). */
					"deathrow: STALE PIN ptr=%p content=%p passes=%ld",
					r->ptr, (void *)r->pin_key,
					(long) r->pin_passes);
				r->stale_logged = 1;
			}
			continue;
		}

		/* fixes911 — name the victim BEFORE calling into it.
		 *
		 * The 2026-07-19 hardware bomb (unmapped memory at an MSL pool
		 * header) died INSIDE a teardown, three frames below here:
		 * llcache_object_deathrow_teardown -> destroy_now -> nsurl_unref ->
		 * lwc_string_destroy -> free. Logging after the call, or only a
		 * per-drain summary, records nothing when the teardown never
		 * returns -- which is exactly the case worth recording. Written
		 * before the call, the last LIFE line on disk IS the entry that
		 * killed us (debug builds FlushVol every kept line, fixes911).
		 *
		 * CAPPED per drain: debug builds FlushVol every kept line (~10-50ms
		 * of HFS sync each), and navigating away from a heavy page can queue
		 * a hundred-plus sub-resource frees in one turn -- naming every one
		 * would add seconds to the very navigation that already feels slow.
		 * The cap keeps the common (small) drain fully named while bounding
		 * the worst case; the drained= total below still reports everything,
		 * so a bomb past the cap shows up as a large count with the tail
		 * unnamed, which is the signal to raise it. */
#ifdef MACOS9_DEATHROW_TRACE_ENTRIES
		if (freed < MACOS9_DEATHROW_TRACE_MAX) {
			macsurf_debug_log_writef(
				"LIFE deathrow free slot=%d ptr=%p fn=%p pin=%p",
				i, r->ptr, (void *) r->teardown,
				(void *) r->pin_key);
		}
#endif

		/* fixes914 — record the victim in RAM, cost-free.
		 *
		 * fixes911 logged every entry so a teardown that never returns could
		 * be named, then fixes913 turned that off because it cost 3035 log
		 * lines (a FlushVol each) in one session. Both were the wrong shape:
		 * the 2026-07-19 wild-pointer abort landed on entry 33, one past the
		 * 32-line cap, so we paid for 32 flushed lines and still did not name
		 * the victim.
		 *
		 * Two plain stores instead. Any crash reporter that can still run --
		 * talloc's abort does, it is how we saw this at all -- reads them and
		 * names the entry, with no I/O on the hot path. */
		macos9_deathrow_cur_ptr = r->ptr;
		macos9_deathrow_cur_fn = (void *) r->teardown;

		/* Free for real. Mark the slot free FIRST so a teardown
		 * that re-enqueues (sub-resource frees) can reuse it. */
		r->active = 0;
		r->teardown(r->ptr);
		freed++;

		macos9_deathrow_cur_ptr = NULL;
		macos9_deathrow_cur_fn = NULL;
	}

	if (freed > 0) {
		macsurf_debug_log_writef("LIFE deathrow drained=%d", freed);
	}

	dr_in_drain = 0;
}
