/*
 * MacSurf -- Mac OS 9 frontend for NetSurf
 * macos9_content_registry.h -- live-content pointer registry (anti-UAF)
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 *
 * SYSTEMIC defence against the "scheduled callback fires after its owner
 * content was destroyed and its heap reused" crash family.
 *
 * The problem: a callback queued via guit->misc->schedule(delay, fn, ctx)
 * can fire one cooperative-scheduler tick after the struct content it
 * targets was freed by content_destroy / html_destroy (typically a bulk
 * hlcache_clean pass).  By then the struct's memory is freed and reused, so
 * its own fields (handler, aborted, status) read as plausibly-alive garbage
 * and EVERY field-based guard (CONTENT_IS_DEAD, c->aborted, ...) passes.
 * Point-guards that read possibly-reused struct bytes are therefore
 * fundamentally unreliable; see the long history of fixes500-521.
 *
 * The cure: never read the suspect struct to decide whether it is alive.
 * Instead keep an out-of-band table of currently-live content pointers,
 * each stamped with a monotonically-increasing generation token.  A content
 * registers itself in content__init (its address enters the table and is
 * assigned a fresh generation) and deregisters as the FIRST action of
 * content_destroy (its slot is cleared, BEFORE the struct is freed).  A
 * scheduled callback validates the content pointer it carries against the
 * table:
 *
 *   - macos9_content_is_live(c)        membership only (pointer present?)
 *   - macos9_content_token(c)          current generation, or 0 if absent
 *   - macos9_content_token_valid(c, t) present AND generation still == t
 *
 * Because the table stores the pointer VALUE (not the struct contents), a
 * freed content is simply absent -- no dereference of reused memory occurs.
 * The generation token additionally defeats the ABA case: if the allocator
 * hands the same address back to a brand-new content before the stale
 * callback fires, the new content gets a new generation, so a token captured
 * against the old content no longer matches.  Capture the token at schedule
 * time, validate at fire time.
 *
 * Implementation is a fixed-capacity array -- no malloc, no list surgery in
 * the destroy path, C89/CW8-clean, and safe to call from inside a running
 * scheduled callback.
 *
 * These symbols are defined only in the macos9 frontend.  Core content files
 * (ns_content.c, box_construct.c, object.c) reach them through #ifdef
 * __MACOS9__ extern declarations + no-op macros on non-Mac syntax-check
 * builds, mirroring the macos9_schedule_cancel_owner pattern.
 */

#ifndef MACSURF_CONTENT_REGISTRY_H
#define MACSURF_CONTENT_REGISTRY_H

struct content;

/**
 * Register a content as live.  Called from content__init once the struct is
 * initialised.  Assigns and stores a fresh generation token for the slot.
 * Idempotent: registering an already-present pointer refreshes nothing and
 * keeps the existing token.
 */
void macos9_content_register(struct content *c);

/**
 * Deregister a content.  Called as the FIRST mutating action of
 * content_destroy, before the struct is freed.  After this returns,
 * macos9_content_is_live(c) is false and any token captured for c is stale.
 */
void macos9_content_unregister(struct content *c);

/**
 * Registry-membership liveness test.  Returns 1 if c is currently a live
 * registered content, 0 otherwise.  Does NOT dereference c.
 */
int macos9_content_is_live(struct content *c);

/**
 * Current generation token for c, or 0 if c is not registered.  0 is never a
 * valid live token, so a captured token of 0 always fails validation.
 */
unsigned long macos9_content_token(struct content *c);

/**
 * Validate a captured token: returns 1 iff c is still registered AND its
 * generation still equals token.  This is the validate-before-fire check a
 * scheduled callback should use.
 */
int macos9_content_token_valid(struct content *c, unsigned long token);

#endif /* MACSURF_CONTENT_REGISTRY_H */
