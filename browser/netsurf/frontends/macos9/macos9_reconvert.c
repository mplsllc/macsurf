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
 * Crash-safety: the scheduled callback NEVER dereferences the (possibly stale)
 * scheduled content pointer. It re-fetches the FRONT window's current content
 * each fire, so a navigation between schedule and fire is harmless. The actual
 * teardown guards live in core html_reconvert (see
 * docs/research/js-dom-render-plan.md).
 *
 * This is part of MacSurf, built on the NetSurf engine. Licensed under GPL v2.
 */

#include "macos9.h"
#include "macsurf_debug.h"

#include "netsurf/content.h"		/* content_get_type, CONTENT_HTML */
#include "content/hlcache.h"		/* hlcache_handle_get_content     */

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

	(void) p;	/* dedup key only — value is never dereferenced */

	c = macos9_reconvert_front_content();
	if (c == NULL)
		return;

	now = (unsigned long) TickCount();

	/* min-interval FLOOR: if a re-convert started very recently, re-arm
	 * rather than start another. Bounds cost on a mutating feed. */
	if (g_last_reconvert_tick != 0 &&
	    (now - g_last_reconvert_tick) < RECONVERT_FLOOR_TICKS) {
		(void) macos9_schedule(RECONVERT_DEBOUNCE_MS,
				macos9_reconvert_cb, p);
		return;
	}

	rc = html_reconvert_content(c);		/* 0 = queued, !=0 = busy */
	if (rc != 0) {
		/* mid-layout or a convert already in flight — re-arm */
		(void) macos9_schedule(RECONVERT_DEBOUNCE_MS,
				macos9_reconvert_cb, p);
		return;
	}

	g_last_reconvert_tick = (unsigned long) TickCount();
	macsurf_debug_log_writef("reconvert: debounced fire ran (c=%p)",
			(void *) c);
}

/* Called from the JS DOM-mutation bindings on every successful mutation.
 * schedule.c dedups identical (cb, param) so a burst collapses to one fire. */
void
macos9_js_mark_dom_dirty(struct content *c)
{
	/* fixes402 — JS->DOM->render reconvert RE-ENABLED with the
	 * defer-until-quiescent guard. fixes394 disabled this after the
	 * complex-site hard crash; investigation showed every documented
	 * teardown hazard is in fact covered (hlcache_handle_release detaches
	 * in-flight image callbacks; the box destructor frees box->styles, so
	 * no css_select_results leak). The residual cause is hardware-only and
	 * not reproducible from source, so html_reconvert now (a) DEFERS while
	 * any object fetch is still in flight, and (b) logs every teardown phase
	 * so the next crash log (if any) pinpoints the exact failing step.
	 * schedule.c dedups identical (cb, param) so a mutation burst collapses
	 * to one fire. */
	(void) macos9_schedule(RECONVERT_DEBOUNCE_MS, macos9_reconvert_cb, c);
}
