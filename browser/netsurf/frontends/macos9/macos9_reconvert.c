/*
 * MacSurf — macos9_reconvert.c
 *
 * fixes384 (#167, JS->DOM->render M3): debounced re-convert.
 *
 * JS DOM mutations (appendChild / textContent / setAttribute in
 * macsurf_js_dom.c) call macos9_js_mark_dom_dirty(). The macos9 cooperative
 * scheduler (schedule.c, which dedups identical callback+param) coalesces a
 * mutation burst into ONE re-convert — rebuild the disposable box tree from
 * the (JS-mutated, persistent) DOM and repaint — fired DEBOUNCE_MS after the
 * LAST mutation. A min-interval FLOOR bounds the re-convert rate so a
 * continuously-mutating feed (Facebook) cannot peg a 233 MHz G3.
 *
 * Crash-safety: the scheduled callback never TRUSTS a scheduled content pointer.
 * fixes874 makes it rebuild the contents that actually mutated (a frame's
 * mutations belong to the frame, not to the front window), so it can no longer
 * simply re-derive a guaranteed-live target from the front window. Instead each
 * pending entry carries the content's registry GENERATION TOKEN, captured at
 * mark time and re-validated at fire time -- which rejects both a freed content
 * and a freed-then-recycled address (ABA). The actual teardown guards live in
 * core html_reconvert (see docs/research/js-dom-render-plan.md).
 *
 * This is part of MacSurf, built on the NetSurf engine. Licensed under GPL v2.
 */

#include <string.h>

#include "macos9.h"
#include "macsurf_debug.h"

#include "netsurf/content.h"		/* content_get_type, CONTENT_HTML */
#include "content/hlcache.h"		/* hlcache_handle_get_content     */
#include "content/content_protected.h"	/* content_get_url                */
#include "utils/nsurl.h"		/* nsurl_get_component, NSURL_HOST */

/* core re-convert trigger: 0 = NSERROR_OK (queued), non-zero = busy/skip. */
extern int html_reconvert_content(struct content *c);
/* browser_window -> current content handle. */
extern struct hlcache_handle *browser_window_get_content(
		struct browser_window *bw);
/* macos9 frontend window accessors (window.c). */
extern struct gui_window *macos9_window_list_head(void);
extern struct browser_window *macos9_gw_bw(struct gui_window *g);

/* Debounce: fire this long after the last DOM mutation. ~24 ticks at 60Hz. */
#define RECONVERT_DEBOUNCE_MS	400
/* Floor: minimum ticks between two re-convert starts (~600ms at 60Hz). FB's
 * feed hydration mutates every ~300ms; this keeps the G3 from livelocking. */
#define RECONVERT_FLOOR_TICKS	36UL

/* TickCount() of the last re-convert start (0 = none yet). */
static unsigned long g_last_reconvert_tick = 0;

/* fixes874 (#303) — the PENDING set: which contents actually mutated.
 *
 * Before this, the scheduled callback re-derived its target from the FRONT
 * WINDOW (macos9_reconvert_front_content) and ignored the content that was
 * marked dirty. That was invisible while the allow-list pinned this path to
 * facebook.com, where the mutating document IS the front window's top-level
 * document -- the two were the same thing by luck. They are not the same thing
 * in general: hackaday's Verbum comment form lives in a cross-origin IFRAME, so
 * its mutations belong to a CHILD content while the front window holds
 * hackaday.com. The old code would have rebuilt the parent page (which did not
 * change) and never the frame (which did) -- so the reply box could not paint
 * even with every JS gate open. That is the white box.
 *
 * Registry TOKENS, not bare pointers: a content can be freed between schedule
 * and fire, and the allocator can hand the same address to a NEW content
 * (fixes550's ABA problem in script.c -- pointer-liveness alone returns a false
 * positive and drives the WRONG document). Capture the generation token at MARK
 * time and re-validate at FIRE time; that is the same contract
 * macos9_unpause_payload uses, for the same reason.
 *
 * Small fixed array, no allocation: the scheduler dedups on (cb, param) and we
 * always schedule with param NULL, so a mutation burst still collapses to one
 * fire -- which it would not if each mark carried its own malloc'd payload.
 * Overflow (more than N distinct mutating frames between fires) degrades to
 * "reconvert the front content", i.e. exactly the old behaviour, rather than
 * dropping the work. */
#define RECONVERT_MAX_PENDING 8

struct macos9_reconvert_pending {
	struct content *c;
	unsigned long   token;
};

static struct macos9_reconvert_pending g_pending[RECONVERT_MAX_PENDING];
static int g_pending_overflow = 0;

#ifdef __MACOS9__
extern int macos9_content_is_live(struct content *c);
extern unsigned long macos9_content_token(struct content *c);
extern int macos9_content_token_valid(struct content *c, unsigned long token);
#else
#define macos9_content_is_live(c) (1)
#define macos9_content_token(c) (1UL)
#define macos9_content_token_valid(c, t) (1)
#endif

/* Record c as dirty. Idempotent per content, so a burst of mutations on one
 * document occupies one slot rather than filling the table. */
static void
macos9_reconvert_pending_add(struct content *c)
{
	int i;
	int freeslot = -1;

	if (c == NULL)
		return;
	for (i = 0; i < RECONVERT_MAX_PENDING; i++) {
		if (g_pending[i].c == c) {
			/* Refresh the token: same address, possibly a newer
			 * generation, and the newer one is what we want to
			 * validate against at fire time. */
			g_pending[i].token = macos9_content_token(c);
			return;
		}
		if (g_pending[i].c == NULL && freeslot < 0)
			freeslot = i;
	}
	if (freeslot < 0) {
		g_pending_overflow = 1;
		return;
	}
	g_pending[freeslot].c = c;
	g_pending[freeslot].token = macos9_content_token(c);
}

/* fixes489 — master gate for JS-triggered re-convert.
 *
 * The re-convert path (fixes384/421) rebuilds the box tree from the
 * JS-mutated DOM. On heavily-scripted pages (XenForo 68kmla.org) it
 * exposed a dom_string use-after-free: a CDATA text node's backing
 * struct is freed (refcount 0), the cooperative scheduler reuses that
 * exact block and overwrites data.cdata.ptr with its callback fn-ptr,
 * and box construction — still holding the stale reference — reads the
 * recycled memory. When the recycled pointer lands in valid heap range
 * it slips past the dom_string_data guard and the interned attribute
 * name ("data-xf-init") gets painted as page text all over the document.
 * Default was OFF (fixes489) pending root-cause; the gate stayed off for
 * every site while that investigation ran.
 *
 * fixes843 (#167 S2) — root-caused via an ASan harness (harness/, Linux-
 * only dev tool, never shipped): the OLD box tree's text-node dom_strings
 * were never protected across the teardown+rebuild window the way the box
 * CONTEXT itself is (fixes421's double-buffer). html.c now pins them
 * (html_reconvert_pin_text_strings / _release_pinned_strings), closing
 * that gap.
 *
 * fixes874 (#303) — the per-host allow-list that used to follow this flag is
 * GONE; see macos9_js_mark_dom_dirty() for the full rationale. Short version:
 * that list was never the fix for the crash above (fixes843's string pinning
 * was), it was the rollout gate left standing afterwards because Facebook was
 * the only verified site -- and it was also the single reason JS-driven pages
 * did not repaint at all. This flag remains the emergency global kill via
 * macsurf_js_set_reconvert_enabled(0). */
/* fixes887 — TEMPORARILY OFF at the maintainer's request, so that the
 * fixes876-886 batch can be verified without the reconvert crash confusing the
 * results. This is a rollout switch, NOT a fix: nothing about the underlying
 * defect changed, and this line is meant to go back to 1 once it is closed.
 *
 * CAVEAT on the two MacsBug traces from 2026-07-16/17 -- they are NOT the same
 * crash, and this switch is only expected to address one of them:
 *
 *  (1) unmapped memory at js_shape_hash_unlink+0004C, reached via
 *        html_process_data -> parse_chunk -> handle_before_html ->
 *        append_child -> DOMNodeInserted default action -> content_broadcast
 *        -> browser_window_callback -> js_newthread -> qjs_flush_timers ->
 *        JS_FreeValue -> free_object -> js_free_shape -> js_shape_hash_unlink
 *      That is the NAVIGATION path (a new page is being parsed, js_newthread
 *      builds its realm and flushes the OLD realm's timers). It does not go
 *      through this file at all, so THIS SWITCH WILL PROBABLY NOT STOP IT.
 *      It is also the exact signature fixes875 was written against, which
 *      means the (ctx, generation) gate did not close it.
 *
 *  (2) illegal instruction reached via macos9_handle_mouse_down ->
 *        browser_window_mouse_click -> html_mouse_action ->
 *        get_mouse_action_node -> link_box_for_ancestor -> box_for_node
 *      A click walking a box tree that is being/has been rebuilt. THAT is the
 *      one plausibly on this feature's account.
 *
 * So if (1) still bites with this off, that is expected and is a separate bug
 * to chase in the timer/realm teardown path, not here. */
/* fixes889 — BACK ON. fixes887 turned this off so the fixes876-886 batch could
 * be verified without the reconvert crash in the way; that is done, and this
 * round is the hunt for the crash itself, which needs the feature live.
 *
 * What changed underneath it (fixes889): box_talloc_destructor now retracts
 * each box's own node backlink as it is freed. The dangling
 * corestring_dom___ns_key_box_node_data pointer that box_for_node() handed to
 * link_box_for_ancestor -- the click crash -- should be structurally gone.
 * The "WORK reconvert H1: ... MISSED(float/marker)=N" line reports whether the
 * old children/next-only clear walk really was leaving boxes behind.
 *
 * If it still crashes, the log now says which population and how many. */
static int g_reconvert_enabled = 1;

void
macsurf_js_set_reconvert_enabled(int enabled)
{
	g_reconvert_enabled = enabled ? 1 : 0;
}

/* The live front-window HTML content, or NULL. Never derefs a stale pointer. */
static struct content *
macos9_reconvert_front_content(void)
{
	struct gui_window *gw;
	struct browser_window *bw;
	struct hlcache_handle *h;

	gw = macos9_window_list_head();
	if (gw == NULL)
		return NULL;
	bw = macos9_gw_bw(gw);
	if (bw == NULL)
		return NULL;
	h = browser_window_get_content(bw);
	if (h == NULL)
		return NULL;
	if (content_get_type(h) != CONTENT_HTML)
		return NULL;
	return hlcache_handle_get_content(h);
}

static void
macos9_reconvert_cb(void *p)
{
	struct content *c;
	unsigned long now;
	int rc;
	int i;
	int busy = 0;
	int did_one = 0;

	(void) p;	/* dedup key only — value is never dereferenced */

	now = (unsigned long) TickCount();

	/* min-interval FLOOR: if a re-convert started very recently, re-arm
	 * rather than start another. Bounds cost on a mutating feed. */
	if (g_last_reconvert_tick != 0 &&
	    (now - g_last_reconvert_tick) < RECONVERT_FLOOR_TICKS) {
		(void) macos9_schedule(RECONVERT_DEBOUNCE_MS,
				macos9_reconvert_cb, p);
		return;
	}

	/* fixes874 (#303) — rebuild the contents that actually MUTATED, not
	 * whatever the front window happens to hold. See the pending-set comment
	 * above for why those are not the same thing once frames are involved. */
	for (i = 0; i < RECONVERT_MAX_PENDING; i++) {
		c = g_pending[i].c;
		if (c == NULL)
			continue;

		/* Two-step liveness, in this order, neither sufficient alone:
		 *  - is_live: is this pointer in the registry at all (freed?).
		 *  - token_valid: is it the SAME content generation we marked, or
		 *    did the allocator recycle the address for a new one (ABA)?
		 * Skipping the token check is how fixes550's wild-tokeniser crash
		 * happened: a stale pointer that passes is_live by luck drives the
		 * WRONG document. */
		if (!macos9_content_is_live(c) ||
		    !macos9_content_token_valid(c, g_pending[i].token)) {
			g_pending[i].c = NULL;
			g_pending[i].token = 0;
			continue;
		}

		rc = html_reconvert_content(c);	/* 0 = queued, !=0 = busy */
		macsurf_debug_log_writef(
			"WORK reconvert: html_reconvert_content rc=%d c=%p", rc,
			(void *) c);
		if (rc != 0) {
			/* mid-layout or a convert already in flight — KEEP the
			 * slot and re-arm, so a busy frame is retried rather than
			 * silently dropped. */
			busy = 1;
			continue;
		}
		g_pending[i].c = NULL;
		g_pending[i].token = 0;
		did_one = 1;
	}

	/* Overflow fallback: more distinct frames mutated than the table holds,
	 * so rebuild the front content too rather than lose the work. This is the
	 * pre-fixes874 behaviour, used only as a backstop. */
	if (g_pending_overflow) {
		g_pending_overflow = 0;
		c = macos9_reconvert_front_content();
		if (c != NULL) {
			rc = html_reconvert_content(c);
			macsurf_debug_log_writef(
				"WORK reconvert: OVERFLOW front rc=%d c=%p", rc,
				(void *) c);
			if (rc == 0) did_one = 1;
			else busy = 1;
		}
	}

	if (did_one)
		g_last_reconvert_tick = (unsigned long) TickCount();
	if (busy) {
		(void) macos9_schedule(RECONVERT_DEBOUNCE_MS,
				macos9_reconvert_cb, p);
	}
}

/* Called from the JS DOM-mutation bindings on every successful mutation.
 * schedule.c dedups identical (cb, param) so a burst collapses to one fire. */
void
macos9_js_mark_dom_dirty(struct content *c)
{
	/* fixes489 — master switch. Still here as an emergency global
	 * kill (macsurf_js_set_reconvert_enabled(0)); nothing currently calls
	 * the setter, so it stays at its compiled-in default (armed — see
	 * fixes843 below) unless a future round wires an explicit override. */
	if (!g_reconvert_enabled)
		return;

	/* fixes874 (#303) — the facebook.com-family allow-list that used to sit
	 * here is GONE. JS-mutated DOM now repaints on every site.
	 *
	 * What that list was: NOT the fix for the fixes489 crash, but the staging
	 * fence left standing after it. The crash was real and specific -- on
	 * XenForo (68kmla.org) a CDATA text node's backing struct was freed
	 * (refcount 0), the cooperative scheduler reused that exact block and
	 * overwrote data.cdata.ptr with its own callback fn-pointer, and box
	 * construction read the recycled memory; when the stale pointer landed in
	 * valid heap it slipped past dom_string_data's guard and the interned
	 * attribute name "data-xf-init" got painted as page text across the
	 * document. fixes843 ROOT-CAUSED it with the ASan harness: the old box
	 * tree's text-node dom_strings were never pinned across the
	 * teardown+rebuild window the way fixes421's double-buffer pins the box
	 * CONTEXT. html.c pins them now (html_reconvert_pin_text_strings /
	 * _release_pinned_strings, html.c:1782/1935). The allow-list was kept
	 * afterwards purely because Facebook was the only site verified -- it is a
	 * rollout gate, not a guard.
	 *
	 * Keeping it costs more than it saves now: it is the single reason
	 * JS-driven pages do not repaint at all. hackaday's Verbum comment form
	 * renders correctly into the DOM and then never paints, because
	 * jetpack.wordpress.com is not facebook.com. The same fence is why the
	 * XenForo reply editor cannot repaint either.
	 *
	 * Still standing: the DEBOUNCE (coalesces a burst into one rebuild), the
	 * FLOOR (bounds the rate so a churning feed cannot peg a G3), the
	 * fixes421 double-buffer + quiesce guards, the fixes843 string pinning,
	 * and the registry token check in the callback. Harness Tests 1-2 exercise
	 * this exact shape -- textContent churn plus a data-xf-init setAttribute
	 * across a reconvert -- ASan-clean.
	 *
	 * macsurf_js_set_reconvert_enabled(0) remains the emergency global kill. */
	macsurf_debug_log_writef("WORK reconvert: dirty-mark scheduling c=%p",
			(void *) c);
	macos9_reconvert_pending_add(c);
	/* fixes421 — two crash vectors closed in html_reconvert:
	 * (1) DOUBLE-BUFFER: old bctx deferred past dom_to_box so the re-cascade
	 *     can share already-interned styles rather than free-then-reintern
	 *     (the libcss arena UAF / 0x2710 crash).
	 * (2) QUIESCE GUARD: html_reconvert bails with NSERROR_NEED_DATA while
	 *     base.active > 0 so html_object_callback's pw pointer is never freed
	 *     under it; macos9_reconvert_cb re-arms and retries.
	 * fixes843 — a third vector closed: the OLD tree's text-node
	 * dom_strings are now pinned across the teardown+rebuild window
	 * (html_reconvert_pin_text_strings in html.c). */
	(void) macos9_schedule(RECONVERT_DEBOUNCE_MS, macos9_reconvert_cb, NULL);
}
