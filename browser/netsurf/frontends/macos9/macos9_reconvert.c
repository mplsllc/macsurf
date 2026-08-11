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
#include "macos9_reconvert.h"

#include "netsurf/content.h"		/* content_get_type, CONTENT_HTML */
#include "content/hlcache.h"		/* hlcache_handle_get_content     */
#include "content/content_protected.h"	/* content_get_url                */
#include "utils/nsurl.h"		/* nsurl_get_component, NSURL_HOST */

/* core re-convert trigger: 0 = NSERROR_OK (queued), non-zero = busy/skip. */
extern int html_reconvert_content(struct content *c);
/* fixes1094 (#265 Round B) — see html.c. */
extern int macsurf_html_has_droppable_inflight(struct content *c);
/* browser_window -> current content handle. */
extern struct hlcache_handle *browser_window_get_content(
		struct browser_window *bw);
/* macos9 frontend window accessors (window.c). */
extern struct gui_window *macos9_window_list_head(void);
extern struct browser_window *macos9_gw_bw(struct gui_window *g);

/* Debounce: fire this long after the last DOM mutation. ~24 ticks at 60Hz. */
#define RECONVERT_DEBOUNCE_MS	400

/* fixes1024 — BACK OFF A COSMETIC TICKER.
 *
 * Hardware, hackaday, JS quiesced: NINE full box-tree reconverts of a
 * ~5000px page in one session, each answering a census of `total=2..4,
 * CLASS=2 STYLE=2` and NOTHING else. That is a widget's interval handler
 * toggling a class every ~900 ticks, and each toggle bought a complete
 * teardown+rebuild+relayout. It is the single largest avoidable cost on the
 * page and it never stops, because a timer does not care that the last
 * rebuild changed nothing.
 *
 * A class/style-only mutation still needs a reconvert in this engine (there
 * is no recascade-only path yet -- that is the real fix and its own round),
 * so the safe lever is CADENCE: while consecutive batches are cosmetic-only,
 * double the debounce, capped. Any STRUCTURAL mutation (append/remove/
 * insert/innerHTML/textContent) resets it to 400ms immediately, so real
 * content changes stay as responsive as they are today. */
#define RECONVERT_DEBOUNCE_MAX_MS	6400
/* Floor: minimum ticks between two re-convert starts (~600ms at 60Hz). FB's
 * feed hydration mutates every ~300ms; this keeps the G3 from livelocking. */
#define RECONVERT_FLOOR_TICKS	36UL

/* R1.4 — mark-to-fire window that counts as "deferred". Anything past this is
 * slow but CORRECT (cosmetic cadence escalation to 6400ms, floor re-arms, busy
 * re-arms), and a log line says so instead of reading as "never rendered". */
#define RECONVERT_DEFER_REPORT_MS	2000UL

/* TickCount() of the last re-convert start (0 = none yet). */
static unsigned long g_last_reconvert_tick = 0;

/* R1.4 — TickCount() of the FIRST dirty mark of the current batch (0 = no
 * batch in flight). The debounced callback measures the mark-to-fire window
 * against this and emits `LIFE RECONVERT DEFERRED` when it grows past
 * RECONVERT_DEFER_REPORT_MS. Reset whenever the batch is fully consumed (a
 * fire that ran the work, a drain without work, or a sync flush that emptied
 * the table). g_defer_reported latches the line to once per batch. */
static unsigned long g_first_mark_tick = 0;
static int g_defer_reported = 0;

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
	/* fixes910 Phase 0 — WHAT changed. `node` is an opaque, REFERENCED
	 * dom_node (never dereferenced on this side; see macos9_reconvert.h for
	 * why the ref is mandatory across the debounce). `multi` means more than
	 * one distinct node/kind mutated before we fired, so precision is gone
	 * and the consumer must assume the whole document — which is exactly
	 * what it does today regardless. Recorded only; no consumer yet. */
	void           *node;
	int             kind;
	int             multi;
};

/* fixes910 Phase 0 — implemented in content/handlers/html/html.c, where the real
 * dom headers are safe to include (the frontend's dom/dom.h is a stub that
 * shadows them under CW8's access paths). */
extern void *macsurf_reconvert_node_ref(void *n);
extern void macsurf_reconvert_node_unref(void *n);

static struct macos9_reconvert_pending g_pending[RECONVERT_MAX_PENDING];
static int g_pending_overflow = 0;

/* fixes1024 — current debounce, doubled while only cosmetic batches arrive. */
static int g_reconvert_debounce_ms = RECONVERT_DEBOUNCE_MS;

/* fixes1135 — cap consecutive cosmetic-only reconverts. A JS timer that
 * toggles a style every N seconds produces infinite O(document) rebuilds:
 * each reconvert costs ~1.5s, during which the JS pump is frozen. After
 * MAX_CONSECUTIVE cosmetic-only rebuilds, suppress further ones until a
 * structural mutation (append/remove/innerHTML/textContent) resets the
 * counter. Reset per navigation. */
#define RECONVERT_COSMETIC_MAX_CONSECUTIVE 2
static int g_consecutive_cosmetic = 0;

static int macos9_reconvert_kind_is_cosmetic(int kind)
{
	return (kind == MACOS9_DOMMUT_SETATTR_CLASS ||
		kind == MACOS9_DOMMUT_SETATTR_STYLE);
}

#ifdef __MACOS9__
extern int macos9_content_is_live(struct content *c);
extern unsigned long macos9_content_token(struct content *c);
extern int macos9_content_token_valid(struct content *c, unsigned long token);
#else
#define macos9_content_is_live(c) (1)
#define macos9_content_token(c) (1UL)
#define macos9_content_token_valid(c, t) (1)
#endif

/*
 * fixes925 (census) — WHAT KIND of DOM mutation are real pages actually doing?
 *
 * Phase 2 wants to skip the reconvert for mutations that cannot change
 * geometry, and the size of that win depends entirely on the mix: a page whose
 * mutations are mostly setAttribute(class) wants a cascade-only path, one doing
 * textContent on leaves wants the html_texty_element_update path, and one doing
 * appendChild may have no win at all. Rather than classify from reading code,
 * measure it on hackaday / 68kmla / facebook.
 *
 * Priced deliberately: counting is two instructions in RAM, and the log line is
 * emitted ONCE PER RECONVERT (a handful per page), never per mutation. fixes911
 * is the cautionary tale -- 3035 flushed lines to re-confirm a negative.
 */
static unsigned long g_mut_counts[MACOS9_DOMMUT_SETATTR_STYLE + 1];
static unsigned long g_mut_total = 0;

static void macos9_reconvert_census_dump(void)
{
	if (g_mut_total == 0) {
		return;
	}
	/* fixes927 — %ld with (long) casts, NOT %lu. macsurf_debug_log_writef is a
	 * hand-rolled formatter (macsurf_debug_log.c), not MSL vsnprintf: it
	 * understands %d, %ld, %p, %s and %% ONLY. An unsupported specifier is
	 * emitted literally, which is exactly what fixes925/926 did -- the whole
	 * census round printed its format string and produced no data. */
	macsurf_debug_log_writef(
		"LIFE mutcensus total=%ld setattr=%ld rmattr=%ld text=%ld "
		"innerhtml=%ld append=%ld remove=%ld insert=%ld chardata=%ld "
		"unknown=%ld CLASS=%ld STYLE=%ld",
		(long) g_mut_total,
		(long) g_mut_counts[MACOS9_DOMMUT_SETATTRIBUTE],
		(long) g_mut_counts[MACOS9_DOMMUT_REMOVEATTRIBUTE],
		(long) g_mut_counts[MACOS9_DOMMUT_TEXTCONTENT],
		(long) g_mut_counts[MACOS9_DOMMUT_INNERHTML],
		(long) g_mut_counts[MACOS9_DOMMUT_APPENDCHILD],
		(long) g_mut_counts[MACOS9_DOMMUT_REMOVECHILD],
		(long) g_mut_counts[MACOS9_DOMMUT_INSERTBEFORE],
		(long) g_mut_counts[MACOS9_DOMMUT_CHARDATA],
		(long) g_mut_counts[MACOS9_DOMMUT_UNKNOWN],
		(long) g_mut_counts[MACOS9_DOMMUT_SETATTR_CLASS],
		(long) g_mut_counts[MACOS9_DOMMUT_SETATTR_STYLE]);
	memset(g_mut_counts, 0, sizeof(g_mut_counts));
	g_mut_total = 0;
}

/* fixes910 Phase 0 — drop this slot's node reference. Idempotent; must be
 * called on EVERY path that releases a slot, or we leak a dom_node (which pins
 * its whole document subtree alive). */
static void
macos9_reconvert_slot_drop_node(int i)
{
	if (g_pending[i].node != NULL) {
		macsurf_reconvert_node_unref(g_pending[i].node);
		g_pending[i].node = NULL;
	}
	g_pending[i].kind = MACOS9_DOMMUT_UNKNOWN;
}

/* fixes910 Phase 0 — clear a slot completely (content + node ref). */
static void
macos9_reconvert_slot_clear(int i)
{
	macos9_reconvert_slot_drop_node(i);
	g_pending[i].c = NULL;
	g_pending[i].token = 0;
	g_pending[i].multi = 0;
}

/* Record c as dirty. Idempotent per content, so a burst of mutations on one
 * document occupies one slot rather than filling the table.
 *
 * fixes910 Phase 0 — additionally records WHICH node changed, degrading safely:
 * a second DISTINCT mutation collapses the slot to `multi` and forgets the node,
 * so a consumer can only ever be MORE conservative than the truth, never less.
 * Behaviour is unchanged today — nothing reads these fields yet. */
static void
macos9_reconvert_pending_add(struct content *c, void *node, int kind)
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

			/* Merge this mutation into what the slot already holds.
			 * Same node AND same kind = the same logical edit
			 * repeating (a textContent loop), so stay precise;
			 * anything else means we can no longer name one site. */
			if (g_pending[i].multi) {
				return;			/* already coarse */
			}
			if (node == NULL || g_pending[i].node == NULL ||
			    g_pending[i].node != node ||
			    g_pending[i].kind != kind) {
				g_pending[i].multi = 1;
				macos9_reconvert_slot_drop_node(i);
			}
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
	/* Take the reference HERE, not at the call site: this is the only place
	 * that decides the pointer is worth keeping past the debounce. */
	g_pending[freeslot].node = macsurf_reconvert_node_ref(node);
	g_pending[freeslot].kind = (node != NULL) ? kind : MACOS9_DOMMUT_UNKNOWN;
	g_pending[freeslot].multi = (node == NULL) ? 1 : 0;
}

/* fixes1016 — is a JS DOM mutation awaiting its reconvert for this content?
 *
 * The audit round proved the failure this answers: slick measured its slides
 * IN THE GAP between innerHTML= and the debounced reconvert, got "no box"
 * (a lie -- the elements are visible content that simply has no box YET),
 * wrote the resulting zeros back as inline sizes, and the featured carousel
 * collapsed. The JS geometry layer uses this to answer `undefined` (which
 * NaN-propagates into a no-op) instead of a fabricated 0 in that window.
 * void* so the JS glue can call it without the content type in scope. */
int macos9_reconvert_pending_for(void *cv)
{
	int i;
	if (cv == NULL) return 0;
	if (g_pending_overflow) return 1;
	for (i = 0; i < RECONVERT_MAX_PENDING; i++) {
		if (g_pending[i].c == (struct content *)cv) return 1;
	}
	return 0;
}

/* R1.4 — how many dirty marks are queued right now: occupied slots, plus one
 * for overflow (more distinct contents than the table holds, which the
 * callback's front-content fallback will rebuild). The overflow tail is only
 * visible here — its own log line is WORK-prefixed and drops on release
 * builds. */
static int
macos9_reconvert_pending_count(void)
{
	int i;
	int n = 0;
	for (i = 0; i < RECONVERT_MAX_PENDING; i++) {
		if (g_pending[i].c != NULL)
			n++;
	}
	if (g_pending_overflow)
		n++;
	return n;
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
/* fixes895 — HUNT ONLY: back ON so the reproduced hackaday/68kmla box-build
 * crash can be captured with the new dense dark-window instrumentation
 * (html.c/box_construct.c breadcrumbs + the durable MacSurf ReconvPos.txt
 * marker, gated by MACSURF_VERBOSE_RECONVERT in macsurf_debug_log.c). This is a
 * DIAGNOSTIC build switch, NOT a fix: validate against a hardware log before
 * making it permanent. macsurf_js_set_reconvert_enabled(0) remains the runtime
 * kill. */
static int g_reconvert_enabled = 1;

void
macsurf_js_set_reconvert_enabled(int enabled)
{
	g_reconvert_enabled = enabled ? 1 : 0;
}

/* ------------------------------------------------------------------
 * fixes1073 (#265) — THE FORCED SYNCHRONOUS LAYOUT PASS.
 *
 * The measure/mutate contract is the one every modern component is built on:
 * change the DOM, then immediately ask how big something is. A real browser
 * answers by reflowing right there and telling the truth.
 *
 * This engine could not, so it declined instead: fixes1016 made geometry return
 * `undefined` whenever a mutation was awaiting its debounced reconvert, on the
 * reasoning that `undefined` NaN-propagates into a no-op while a fabricated 0
 * gets written back as an inline size and destroys the page (which is exactly
 * how slick collapsed hackaday's featured carousel). That was the right call
 * for a lie, but declining is still a wrong answer -- the elements ARE visible
 * content, they simply have no box YET. Every widget that measures before
 * laying itself out gets nothing and lays itself out wrong.
 *
 * This closes it. Flushing is possible at all because fixes903 made the
 * reconvert build SYNCHRONOUS: html_reconvert_content runs teardown, dom_to_box,
 * html_reconvert_done, content__reformat and layout_document to completion
 * before returning, with no event-loop re-entry. So on return the box tree is
 * rebuilt and laid out, and geometry can read the truth.
 *
 * SAFETY. Rebuilding the box tree frees the old one, so this must never run
 * while anything is walking it. The layers, outermost first:
 *   - in_flush: re-entrancy. A geometry read inside a reconvert (via a
 *     mutation callback) must not start a second one.
 *   - macos9_paint_gw: a redraw is on the stack walking boxes. fixes903's whole
 *     hunt was crashes from exactly this window.
 *   - macsurf_reconvert_in_progress: a reconvert is already mid-flight.
 *   - html_reconvert's own preconditions, which it enforces itself and which
 *     are the reason it returns non-zero: status != DONE, c->reflowing (never
 *     free boxes mid-layout), a convert already in flight, or sub-resource
 *     fetches still active (fixes421's use-after-free).
 * A refusal at any layer degrades to the fixes1016 behaviour -- `undefined` --
 * which is safe by construction.
 *
 * BUDGET. Layout here costs ~1.6s on a heavy page (hardware-measured), so a
 * script that mutates and measures in a loop would force one full reconvert
 * per iteration. Browsers have the same hazard (it is called layout thrashing)
 * and survive it because their layout is cheap; ours is not. The budget bounds
 * a pathological page to a known cost (MACOS9_SYNC_BUDGET_US, 30s of
 * cumulative flush time per nav) and then degrades to `undefined`, and the
 * counters say out loud how often real pages hit it -- which is the number
 * that decides whether the next round needs incremental layout. A silent cap
 * would read as "nothing to see here".
 * ------------------------------------------------------------------ */
static long g_sync_flushes      = 0;	/* flushes actually run, this nav  */
static long g_sync_declined     = 0;	/* asked, refused (guard or budget) */
static long g_sync_us           = 0;	/* cumulative cost of the flushes   */

/* fixes1075 — WHY a flush was refused, because the first hardware log of
 * fixes1073 reported declined=660 flush=0 on hackaday and the instrument could
 * not say which guard was firing. A refusal count with no reason attached is
 * the same shape of unhelpful as `js=25s` with no compile/run split. */
static long g_sync_r_notdone  = 0;	/* content not CONTENT_STATUS_DONE   */
static long g_sync_r_active   = 0;	/* sub-resource fetches still in air */
static long g_sync_r_paint    = 0;	/* a redraw is walking the box tree  */
static long g_sync_r_inprog   = 0;	/* reconvert already in flight       */
static long g_sync_r_budget   = 0;	/* time budget for this nav spent    */
static long g_sync_r_busy     = 0;	/* html_reconvert refused, other     */

/* fixes1075 — budget by TIME, not by count.
 *
 * fixes1073 allowed 24 forced layouts per navigation on an estimate of ~100ms
 * each. Hardware measured 1.08s each (the Jetpack comment iframe spent 4.34s on
 * four of them), which makes that ceiling a 26-second worst case -- exactly the
 * regression this whole effort is trying not to cause.
 *
 * A count cannot bound a cost whose per-unit price is unknown and varies with
 * page size. Cumulative microseconds can, and it degrades where it should: a
 * page gets as many reflows as fit in the budget, cheap ones get more of them,
 * and once spent geometry falls back to `undefined` and JSSYNC says so.
 *
 * fixes1126 (#265) — 2s was an order of magnitude too tight. Hardware on
 * hackaday: the first TWO flushes cost 3.27s, so the budget was spent before
 * slick finished its opening measure burst and every later measurement
 * declined (budget=703 of 1240, flush=2). The per-flush price on that page is
 * ~1.6s (the reconvert's O(document) rebuild) -- that is the number to budget
 * against: 30s buys ~18 real reflows per navigation, enough for a widget's
 * whole init measure/mutate loop, while a genuinely pathological layout-thrash
 * page is still bounded and stays user-abortable: the JS interrupt handler
 * polls WaitNextEvent for Cmd-. every ~200ms between bytecodes, i.e. between
 * flushes (qjs_interrupt_handler), and the per-navigation reset bounds it to
 * one navigation's worth of cost. */
/* fixes1133 — raised from 30s to 120s. Hardware measured 19 priority flushes
 * consuming 31s on hackaday (1.6s each); the budget was spent before slick's
 * init ran, producing 1134 declines and stranding the measure/mutate cycle.
 * 120s buys ~75 flushes — enough headroom for any real page without removing
 * the safety valve entirely. Settle-once still limits to one flush per burst. */
#define MACOS9_SYNC_BUDGET_US 120000000L

void
macos9_reconvert_sync_stats(long *flushes, long *declined, long *us)
{
	if (flushes != NULL)  *flushes  = g_sync_flushes;
	if (declined != NULL) *declined = g_sync_declined;
	if (us != NULL)       *us       = g_sync_us;
}

/* fixes1075 — the per-reason breakdown behind `declined`. See the counters'
 * declarations for what each one means and which of them would change the
 * next round's target. */
void
macos9_reconvert_sync_reasons(long *notdone, long *active, long *paint,
		long *inprog, long *budget, long *busy)
{
	if (notdone != NULL) *notdone = g_sync_r_notdone;
	if (active != NULL)  *active  = g_sync_r_active;
	if (paint != NULL)   *paint   = g_sync_r_paint;
	if (inprog != NULL)  *inprog  = g_sync_r_inprog;
	if (budget != NULL)  *budget  = g_sync_r_budget;
	if (busy != NULL)    *busy    = g_sync_r_busy;
}

void
macos9_reconvert_sync_reset(void)
{
	g_sync_flushes  = 0;
	g_sync_declined = 0;
	g_sync_us       = 0;
	g_sync_r_notdone = 0; g_sync_r_active = 0; g_sync_r_paint = 0;
	g_sync_r_inprog = 0;  g_sync_r_budget = 0; g_sync_r_busy = 0;
	g_consecutive_cosmetic = 0;
}

/* fixes1126 (#265) — a transient flush refusal must not strand the pending
 * rebuild behind the escalating cosmetic debounce. fixes1024 doubles the
 * debounce (400ms -> 6400ms) while only cosmetic mutations arrive, and a
 * measure/mutate widget's style writes ARE cosmetic -- so the async rebuild
 * can be 13+ seconds away (`LIFE RECONVERT DEFERRED 13900` on hardware) while
 * the sync flush path declines every read. That is the busy=537 story from
 * the hackaday log: the flush cannot run (no select_ctx yet, mid-layout, ...),
 * and the work waits behind a debounce that keeps doubling.
 *
 * Pull the work forward instead: the scheduler dedups on (cb, param), so a
 * burst of refusals costs one pending entry and the fire lands
 * RECONVERT_SYNC_RETRY_MS after the LAST refusal, at the next JS yield. The
 * retry runs the SAME debounced callback the mutations already scheduled, so
 * the liveness tokens, the min-interval floor and the multi-content walk all
 * apply unchanged. Refusals that cannot clear do not retry: a dead content
 * (!live), a terminal status (notdone), and the spent time budget -- the
 * debounced callback still runs those at its own cadence. */
#define RECONVERT_SYNC_RETRY_MS 80

static void macos9_reconvert_cb(void *p);	/* defined below */

static void
macos9_reconvert_sync_retry(void)
{
	(void) macos9_schedule(RECONVERT_SYNC_RETRY_MS, macos9_reconvert_cb,
			NULL);
}

int
macos9_reconvert_flush_now(void *cv)
{
	static int in_flush = 0;
	extern int macsurf_reconvert_in_progress;
	extern struct gui_window *macos9_paint_gw;
	extern double macos9_micros(void);
	struct content *c = (struct content *) cv;
	double t0;
	int i;
	int rc;

	if (c == NULL)
		return 0;
	/* Nothing dirty: the box tree already answers for the current DOM. */
	if (!macos9_reconvert_pending_for(cv))
		return 0;

	/* fixes1075 — attribute the refusal. Ordered most-specific first so the
	 * counter names the ACTUAL blocker rather than whichever guard happens
	 * to be listed earliest. */
	if (in_flush || macsurf_reconvert_in_progress) {
		/* A reconvert is on the stack or mid-flight. No tree is safe to
		 * read -- the old one is being torn down -- so the answer is
		 * undefined, and there is no way to "wait" for it here: the
		 * flush is synchronous and JS cannot yield mid-read. The work
		 * is not stranded though: the in-flight reconvert will clear it,
		 * and the retry catches the case where it cannot. */
		g_sync_r_inprog++; g_sync_declined++;
		macos9_reconvert_sync_retry();
		return 0;
	}
	if (macos9_paint_gw != NULL) {
		/* A redraw is walking the box tree; a flush will be safe on the
		 * next event-loop pass, so retry then. */
		g_sync_r_paint++; g_sync_declined++;
		macos9_reconvert_sync_retry();
		return 0;
	}
	if (!g_reconvert_enabled || !macos9_content_is_live(c)) {
		g_sync_r_busy++; g_sync_declined++; return 0;
	}
	/* These two are html_reconvert's own preconditions, checked here only so
	 * the refusal can be NAMED. html_reconvert enforces them regardless.
	 *   notdone: the document has not finished loading. Script init -- which
	 *     is when widgets measure -- runs inside this window, so if this is
	 *     the dominant reason then geometry is dead exactly when it matters
	 *     and the gate itself is the next problem, not the budget.
	 *   active: sub-resource fetches still in flight. Rebuilding now would
	 *     free an object list that html_object_callback still points into
	 *     (the fixes421 use-after-free). */
	/* fixes1094 (#265 Round B) — mirror html_reconvert's relaxed
	 * preconditions. These are screened here ONLY so a refusal can be named;
	 * html_reconvert enforces them regardless, so if the two drift this side
	 * silently declines work the other would have accepted. That is exactly
	 * what hid the problem before: hardware read notdone=630 of 630 declines
	 * with flush=0, because this DONE test ran first and the guard below was
	 * never even evaluated.
	 *
	 * READY (not DONE) is the real precondition -- READY is when the first
	 * box tree exists -- and the active-fetch hazard is now the narrow
	 * "in-flight entry the reconvert would FREE", not "any fetch at all".
	 * See the fixes1094 comments in html_reconvert. */
	/* fixes1096 (#265 Round C3) — LOADING too. Mirrors html_reconvert; see
	 * the safety argument there. This is the window `notdone` was counting
	 * (565 of 1247 declines on hardware) and the one the featured slider
	 * measures in. */
	if (c->status != CONTENT_STATUS_LOADING &&
	    c->status != CONTENT_STATUS_READY &&
	    c->status != CONTENT_STATUS_DONE) {
		g_sync_r_notdone++; g_sync_declined++; return 0;
	}
	if (c->active > 0 && macsurf_html_has_droppable_inflight(c)) {
		/* Transient: the in-flight entry a rebuild would free completes
		 * shortly. Retry on the next pass. */
		g_sync_r_active++; g_sync_declined++;
		macos9_reconvert_sync_retry();
		return 0;
	}
	if (g_sync_us >= MACOS9_SYNC_BUDGET_US) {
		g_sync_r_budget++; g_sync_declined++; return 0;
	}

	in_flush = 1;
	t0 = macos9_micros();
	rc = html_reconvert_content(c);	/* SYNCHRONOUS -- see fixes903 */
	in_flush = 0;

	if (rc != 0) {
		/* Busy for a reason html_reconvert owns that we did not screen
		 * for above (mid-layout, a convert already in flight, or -- the
		 * dominant case on hardware -- no select_ctx yet, which arrives
		 * at finish_conversion). Leave the slot pending and pull the
		 * rebuild forward: the retry fires shortly after the JS burst
		 * yields, by which time the blocker may have cleared. Answer
		 * undefined. */
		g_sync_r_busy++;
		g_sync_declined++;
		macos9_reconvert_sync_retry();
		return 0;
	}

	g_sync_us += (long)(macos9_micros() - t0);
	g_sync_flushes++;

	/* This flush answered every pending mutation for c, so retire its
	 * slots -- otherwise the debounced callback rebuilds the same tree
	 * again for work already done. */
	for (i = 0; i < RECONVERT_MAX_PENDING; i++) {
		if (g_pending[i].c == c)
			macos9_reconvert_slot_clear(i);
	}
	/* R1.4 — if this flush drained the batch, restart the batch clock;
	 * otherwise the next mark would inherit the old batch's age and
	 * falsely report DEFERRED. Overflow keeps the batch alive: marks
	 * beyond the table still await the front-content fallback rebuild. */
	if (g_pending_overflow == 0 && macos9_reconvert_pending_count() == 0) {
		g_first_mark_tick = 0;
		g_defer_reported = 0;
	}
	g_last_reconvert_tick = (unsigned long) TickCount();
	return 1;
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

	/* R1.4 — defer diagnostic. A batch whose mark-to-fire window grew well
	 * past the base debounce (cosmetic cadence escalation to 6400ms, floor
	 * re-arms, busy re-arms) is SLOW, not stuck; without this line a
	 * deferred render reads as "never rendered" in a triage log. Reported
	 * once per batch, even if the callback keeps re-arming. */
	if (g_first_mark_tick != 0 && !g_defer_reported) {
		unsigned long elapsed_ms = (now - g_first_mark_tick) *
			1000UL / 60UL;
		if (elapsed_ms > RECONVERT_DEFER_REPORT_MS) {
			macsurf_debug_log_writef("LIFE RECONVERT DEFERRED %ld %ld",
				(long) elapsed_ms,
				(long) macos9_reconvert_pending_count());
			g_defer_reported = 1;
		}
	}

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
			macos9_reconvert_slot_clear(i);
			continue;
		}

		/* fixes1135 — cosmetic-only batch: if we have already run
		 * MAX_CONSECUTIVE cosmetic rebuilds in a row without a
		 * structural mutation, suppress this one. The timer-driven
		 * style churn (slick autoplay, CSS animation polyfills)
		 * produces infinite O(document) rebuilds that cost ~1.5s
		 * each. The page still renders correctly — the styles were
		 * already applied by the JS setAttribute, and the next
		 * structural mutation forces a real rebuild. */
		{
			int cosm;
			int any_structural = 0;
			for (cosm = 0;
			     cosm <= MACOS9_DOMMUT_SETATTR_STYLE; cosm++) {
				if (cosm == MACOS9_DOMMUT_SETATTR_CLASS ||
				    cosm == MACOS9_DOMMUT_SETATTR_STYLE)
					continue;
				if (g_mut_counts[cosm] > 0) {
					any_structural = 1;
					break;
				}
			}
			if (!any_structural &&
			    g_consecutive_cosmetic >=
			    RECONVERT_COSMETIC_MAX_CONSECUTIVE) {
				macsurf_debug_log_writef(
					"LIFE reconvert cosmetic-suppress "
					"consec=%d cap=%d",
					g_consecutive_cosmetic,
					RECONVERT_COSMETIC_MAX_CONSECUTIVE);
				macos9_reconvert_slot_clear(i);
				memset(g_mut_counts, 0,
					sizeof(g_mut_counts));
				g_mut_total = 0;
				did_one = 1;
				continue;
			}
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
		macos9_reconvert_slot_clear(i);
		did_one = 1;

		/* fixes1135 — track consecutive cosmetic-only reconverts. */
		{
			int cosm;
			int any_structural = 0;
			for (cosm = 0;
			     cosm <= MACOS9_DOMMUT_SETATTR_STYLE; cosm++) {
				if (cosm == MACOS9_DOMMUT_SETATTR_CLASS ||
				    cosm == MACOS9_DOMMUT_SETATTR_STYLE)
					continue;
				if (g_mut_counts[cosm] > 0) {
					any_structural = 1;
					break;
				}
			}
			if (!any_structural)
				g_consecutive_cosmetic++;
			else
				g_consecutive_cosmetic = 0;
		}

		/* fixes925 — dump the census for the batch this reconvert answers.
		 * fixes1158 — AFTER both fixes1135 readers: the dump zeroes
		 * g_mut_counts, so it must not run before the cosmetic-suppression
		 * check or the consecutive-cosmetic tracker, both of which decide
		 * from those counts. */
		macos9_reconvert_census_dump();
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
	} else {
		/* R1.4 — batch consumed: the work ran (did_one), or the table
		 * drained without it (a sync flush answered the marks, or every
		 * slot died). While busy re-arms, the clock keeps running so the
		 * deferred window stays the TRUE mark-to-render time. */
		g_first_mark_tick = 0;
		g_defer_reported = 0;
	}
}

/* Called from the JS DOM-mutation bindings on every successful mutation.
 * schedule.c dedups identical (cb, param) so a burst collapses to one fire. */
void
macos9_js_mark_dom_dirty(struct content *c)
{
	/* fixes910 Phase 0 — "something changed, no idea what": always the full
	 * rebuild, exactly as before. */
	macos9_js_mark_dom_dirty_node(c, NULL, MACOS9_DOMMUT_UNKNOWN);
}

void
macos9_js_mark_dom_dirty_node(struct content *c, void *node, int kind)
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
	/* fixes925 — census: RAM only, no I/O on the mutation path. */
	if (kind >= 0 && kind <= MACOS9_DOMMUT_SETATTR_STYLE) {
		g_mut_counts[kind]++;
	} else {
		g_mut_counts[MACOS9_DOMMUT_UNKNOWN]++;
	}
	g_mut_total++;

	/* fixes1024 — cadence control, decided by WHAT changed. */
	if (macos9_reconvert_kind_is_cosmetic(kind)) {
		if (g_reconvert_debounce_ms < RECONVERT_DEBOUNCE_MAX_MS) {
			g_reconvert_debounce_ms *= 2;
			if (g_reconvert_debounce_ms > RECONVERT_DEBOUNCE_MAX_MS)
				g_reconvert_debounce_ms =
					RECONVERT_DEBOUNCE_MAX_MS;
			/* fixes1032 — log only on reaching the CAP. The
			 * cosmetic/structural pair flapped 108 times in one
			 * session, which is 108 flushed writes to say the
			 * cadence is working. */
			if (g_reconvert_debounce_ms == RECONVERT_DEBOUNCE_MAX_MS)
				macsurf_debug_log_writef(
					"LIFE reconvert cosmetic-only, debounce "
					"capped at %dms", g_reconvert_debounce_ms);
		}
	} else if (g_reconvert_debounce_ms != RECONVERT_DEBOUNCE_MS) {
		g_reconvert_debounce_ms = RECONVERT_DEBOUNCE_MS;   /* silent */
	}
	/* fixes1135 -- structural mutation resets the cosmetic-suppression
	 * counter so the page can converge after the animation stops. */
	if (!macos9_reconvert_kind_is_cosmetic(kind))
		g_consecutive_cosmetic = 0;

	/* R1.4 — start the batch clock on the first mark of a batch. A
	 * non-zero tick means a batch is in flight, so later marks in the
	 * burst keep the ORIGINAL first-mark time; the callback measures the
	 * mark-to-fire window against this (see the RECONVERT DEFERRED
	 * diagnostic there) and resets it when the batch is consumed. */
	if (g_first_mark_tick == 0)
		g_first_mark_tick = (unsigned long) TickCount();

	macos9_reconvert_pending_add(c, node, kind);
	/* fixes1148 — DON'T reschedule if already queued.
	 *
	 * The old code called macos9_schedule() on EVERY mutation, which
	 * sched_remove()'d the existing entry and re-inserted a new one at
	 * now+debounce. A continuous mutation stream (job.php polling at
	 * ~120 req/s) therefore pushed the reconvert out FOREVER — each
	 * new mark reset the clock. The pending table already accumulates
	 * all the work; when the FIRST schedule eventually fires, it
	 * processes every slot. Rescheduling on each mark only DELAYS
	 * the work without changing what work is done.
	 *
	 * Only the FIRST mark in a burst schedules; later marks just
	 * add to the pending slots that the same callback will consume.
	 * The debounce still escalates for cosmetic-only bursts, but
	 * structural mutations reset it and the callback handles all
	 * accumulated work at once. */
	{
		extern int macos9_sched_is_queued(
			void (*callback)(void *p), void *p);
		if (!macos9_sched_is_queued(macos9_reconvert_cb, NULL)) {
			(void) macos9_schedule(g_reconvert_debounce_ms,
					macos9_reconvert_cb, NULL);
		}
	}
}
