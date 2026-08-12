/*
 * MacSurf  -  Mac OS 9 frontend for NetSurf
 * macos9_deathrow.h  -  Stage 1 quiescent-point deferred-free.
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 *
 * Root cause this closes (Class 1, object-freed-by-destroy): MacSurf runs
 * box conversion asynchronously (convert_xml_to_box reschedules itself
 * every ~10 nodes holding a raw ctx->content), while teardown
 * (hlcache_clean / navigation) fires through the same flat
 * macos9_schedule_run drain. A teardown frees a content/entry/handle the
 * deferred walk is still holding -> UAF (the c->encoding strlen crash,
 * fixes428/429 box->object, fixes457 zombie ctx, fixes446d sched alias).
 *
 * The fix: never free at refcount-zero. Owning code UNLINKS synchronously
 * but ENQUEUES the actual free onto a death row, drained at exactly one
 * quiescent point: the top of macos9_poll, when macos9_op_depth == 0 (no
 * walk/parse/convert/redraw/broadcast on the stack) AND no scheduled
 * continuation still references the object.
 */

#ifndef MACSURF_MACOS9_DEATHROW_H
#define MACSURF_MACOS9_DEATHROW_H

struct content;

/*
 * Operation-depth counter. Bracketed (++/--) around every reentry point
 * that runs engine work: macos9_schedule_run's callback(p), the fetch
 * pump, browser_window_redraw, and content_broadcast. While > 0 a walk /
 * parse / convert / redraw / broadcast is on the stack and the death row
 * must NOT drain. Defined in macos9_deathrow.c.
 */
extern int macos9_op_depth;

/*
 * Enqueue an object for deferred free. `teardown` is called on it at the
 * quiescent drain. `pin_key` is the struct content whose pending scheduled
 * continuations must have drained before this object may be freed (the
 * content itself for a content record; entry->content / handle->entry->
 * content for those; NULL for objects no continuation can reference).
 *
 * IDEMPOTENCY / ABA: the caller MUST gate on the object's own `dr_queued`
 * flag before calling (if (obj->dr_queued) return; obj->dr_queued = 1;) so
 * a double-enqueue (-> double-free) is structurally impossible and a
 * freed-then-reused address (dr_queued==0 on fresh memory) is never
 * mistaken for a still-queued prior tenant.
 */
void macos9_deathrow_add(void *ptr, void (*teardown)(void *ptr),
		struct content *pin_key);

/* Drain at the quiescent point (top of macos9_poll, outermost only). */
void macos9_deathrow_drain(void);

/*
 * sched walker (implemented in schedule.c): returns true if `pred` returns
 * true for any pending scheduled entry's (callback, param). Used by the
 * death row's pinned-check.
 */
bool macos9_sched_any(bool (*pred)(void (*cb)(void *), void *p, void *arg),
		void *arg);

/*
 * Per-module "does this scheduled (cb,p) reference content X" predicates.
 * Each lives in the file that owns the continuation's static callback, so
 * it can compare cb to that callback and decode p -> content. Return true
 * iff the entry is that callback AND references `target`.
 */
bool box_construct_sched_pins(void (*cb)(void *), void *p,
		struct content *target);
bool html_css_sched_pins(void (*cb)(void *), void *p,
		struct content *target);
/* html_object_sched_pins (object.c) deliberately omitted: object.c would
 * not export the symbol under CW8. The object-refresh pin is dropped; its
 * continuations are cancelled by html_destroy on teardown. See the NOTE in
 * deathrow_pin_pred (macos9_deathrow.c). */

#endif
