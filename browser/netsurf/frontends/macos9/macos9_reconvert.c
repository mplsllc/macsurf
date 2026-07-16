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
 * that gap. Default flips to ON, but macos9_reconvert_host_allowed()
 * below is the REAL safety boundary — only facebook.com family hosts can
 * actually reach macos9_schedule() from here, so a regression on XenForo
 * or anywhere else is structurally impossible regardless of this flag.
 * macsurf_js_set_reconvert_enabled(0) remains available as an emergency
 * global kill if the host-list approach ever needs to be paused wholesale. */
static int g_reconvert_enabled = 1;

void
macsurf_js_set_reconvert_enabled(int enabled)
{
	g_reconvert_enabled = enabled ? 1 : 0;
}

/* fixes843 (#167 S2) — per-host allow-list. Even with the master switch
 * armed, JS DOM mutations only schedule a box-tree rebuild for Facebook
 * hosts. This is the actual safety boundary: it keeps the blast radius of
 * re-arming reconvert to exactly one site. XenForo (68kmla.org /
 * tinkerdifferent.com — the ORIGINAL crash site fixes489 was written for)
 * and every other page stay on the pre-fixes384 behaviour (JS mutates the
 * DOM, nothing repaints) regardless of the master switch. Root cause of the
 * UAF itself is addressed separately (fixes843, html.c: the OLD tree's
 * text-node dom_strings are now pinned across the teardown+rebuild window,
 * closing the gap fixes421's box-context deferral left open) — this
 * host-list is the second, independent layer, not a substitute for that
 * fix. Suffix list matches the fetcher's host_is_fb_asset() in
 * macos9_tls_fetcher.c (kept separate/static per-file rather than shared,
 * same pattern already used for the UA table). */
static int
macos9_reconvert_host_allowed(struct content *c)
{
	struct nsurl *url;
	lwc_string *host;
	const char *h;
	size_t hl, sl, i, n;
	static const char *const allow_suffixes[] = {
		"facebook.com", "fbcdn.net", "fbsbx.com", "cdninstagram.com"
	};

	/* content_get_url -> llcache_handle_get_url derefs the llcache handle
	 * unconditionally; guard here rather than assume every caller of
	 * macos9_js_mark_dom_dirty only ever sees a fully-live content. */
	if (c == NULL || c->llcache == NULL)
		return 0;

	url = content_get_url(c);
	if (url == NULL)
		return 0;

	host = nsurl_get_component(url, NSURL_HOST);
	if (host == NULL)
		return 0;

	h = lwc_string_data(host);
	hl = lwc_string_length(host);
	n = sizeof(allow_suffixes) / sizeof(allow_suffixes[0]);
	for (i = 0; i < n; i++) {
		sl = strlen(allow_suffixes[i]);
		if (hl >= sl &&
		    strncasecmp(h + hl - sl, allow_suffixes[i], sl) == 0 &&
		    (hl == sl || h[hl - sl - 1] == '.')) {
			lwc_string_unref(host);
			return 1;
		}
	}
	lwc_string_unref(host);
	return 0;
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
	/* fixes489 — master switch. Still here as an emergency global
	 * kill (macsurf_js_set_reconvert_enabled(0)); nothing currently calls
	 * the setter, so it stays at its compiled-in default (armed — see
	 * fixes843 below) unless a future round wires an explicit override. */
	if (!g_reconvert_enabled)
		return;
	/* fixes843 — the real safety boundary: facebook.com family only.
	 * See macos9_reconvert_host_allowed()'s comment for the full
	 * rationale. Everything else (XenForo included) stays on the
	 * pre-fixes384 behaviour no matter what g_reconvert_enabled is. */
	if (!macos9_reconvert_host_allowed(c))
		return;
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
