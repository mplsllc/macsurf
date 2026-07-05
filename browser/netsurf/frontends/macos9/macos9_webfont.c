/*
 * MacSurf - macos9_webfont.c
 *
 * Downloadable webfont (@font-face) support, Item 1 of the "closer to the
 * real site" work. Modern sites ship their icon fonts (FontAwesome, Material
 * Design) as @font-face rules whose glyphs live in the Unicode Private-Use
 * Area. NetSurf parses @font-face but never fetches the src, so those glyphs
 * never render. This module:
 *
 *   Round 1 (this file): resolve a CSS font-family -> @font-face src URL (via
 *   the core accessor html_macsurf_font_face_url, which owns the select ctx),
 *   then fetch + disk-cache the font file. No rendering yet.
 *   Round 2: parse the sfnt (cmap + glyf).
 *   Round 3: paint glyph outlines.
 *
 * Fetch/cache/repaint pattern is cloned from macos9_svg_sprite.c: the macos9
 * fetcher stores the body to the disk cache even though CONTENT_ANY has no
 * font handler (so DONE/ERROR are both fine), and the next paint reads it back
 * via macos9_cache_lookup.
 *
 * Part of MacSurf, built on the NetSurf engine. GPL v2.
 */

#include "macos9.h"

#ifdef __MACOS9__

#include <string.h>
#include <stdlib.h>

#include "utils/errors.h"
#include "utils/nsurl.h"
#include "netsurf/content_type.h"
#include "content/hlcache.h"
#include "netsurf/content.h"
#include <libwapcaplet/libwapcaplet.h>

#include "macos9_disk_cache.h"
#include "macos9_webfont.h"
#include "macsurf_debug.h"

extern struct gui_window *macos9_window_list_head(void);

/* Core accessor (content/handlers/html/html.c): family -> @font-face src URL,
 * or NULL if the family has no usable downloadable @font-face rule. Returns a
 * new nsurl ref the caller owns. */
extern struct nsurl *html_macsurf_font_face_url(struct content *c,
		lwc_string *family);

/* Per-family slot state. */
enum {
	WF_NEW = 0,	/* just allocated, not yet resolved            */
	WF_NONE,	/* family has no @font-face URI (don't retry)   */
	WF_FETCHING,	/* fetch in flight                              */
	WF_LOADED	/* bytes held (ready for Round 2 parse)         */
};

struct webfont_slot {
	lwc_string    *family;	/* ref'd; NULL = free slot                 */
	int            state;
	struct nsurl  *url;	/* resolved src URL (ref'd), or NULL       */
	char          *data;	/* font bytes (held for Round 2), or NULL  */
	long           len;
	hlcache_handle *fetch;	/* in-flight fetch handle, or NULL         */
	int            fails;	/* failed fetch attempts (storm control)   */
};

/* Give up refetching after this many failed attempts (stops the storm when a
 * font host stalls/aborts, e.g. slow gstatic over TLS on PPC). */
#define WEBFONT_MAX_FAILS 3

#define WEBFONT_MAX 8
static struct webfont_slot g_webfonts[WEBFONT_MAX];

/* Fetch completion: the fetcher has already disk-cached the body by now
 * (CONTENT_ANY has no font handler, so ERROR is the normal outcome). Release
 * the handle and repaint; the next macos9_webfont_ensure reads the bytes back
 * from the disk cache. */
static nserror
webfont_fetch_cb(hlcache_handle *h, const hlcache_event *event, void *pw)
{
	struct webfont_slot *slot = (struct webfont_slot *) pw;
	struct gui_window *gw;

	switch (event->type) {
	case CONTENT_MSG_DONE:
	case CONTENT_MSG_ERROR:
		if (slot != NULL && slot->fetch == h) {
			hlcache_handle_release(slot->fetch);
			slot->fetch = NULL;
			if (slot->state == WF_FETCHING) {
				if (event->type == CONTENT_MSG_ERROR) {
					/* Fetch failed/aborted (never cached).
					 * Retry a few times, then give up so a
					 * stalling host can't storm the fetcher. */
					slot->fails++;
					if (slot->fails >= WEBFONT_MAX_FAILS) {
						slot->state = WF_NONE;
						macsurf_debug_log_writef(
							"webfont: give up fam=%s after %d fails",
							(slot->family != NULL) ?
							lwc_string_data(slot->family) : "?",
							slot->fails);
					} else {
						slot->state = WF_NEW;
					}
				} else {
					/* DONE: body is disk-cached now; next
					 * ensure reads it back and loads it. */
					slot->state = WF_NEW;
				}
			}
		}
		gw = macos9_window_list_head();
		if (gw != NULL)
			macos9_window_invalidate_content(gw);
		break;
	default:
		break;
	}

	return NSERROR_OK;
}

/* Find the slot for a family (interned lwc_string, compared by pointer), or a
 * free slot to claim. Returns NULL only if the registry is full. */
static struct webfont_slot *
webfont_slot_for(lwc_string *family)
{
	int i;
	int free_i = -1;

	for (i = 0; i < WEBFONT_MAX; i++) {
		if (g_webfonts[i].family == family)
			return &g_webfonts[i];
		if (g_webfonts[i].family == NULL && free_i < 0)
			free_i = i;
	}
	if (free_i < 0)
		return NULL;

	g_webfonts[free_i].family = lwc_string_ref(family);
	g_webfonts[free_i].state = WF_NEW;
	g_webfonts[free_i].url = NULL;
	g_webfonts[free_i].data = NULL;
	g_webfonts[free_i].len = 0;
	g_webfonts[free_i].fetch = NULL;
	g_webfonts[free_i].fails = 0;
	/* fixes616 DIAG — one line per DISTINCT family the plotter ever paints.
	 * Tells us whether icon families (e.g. "Material Design Icons") reach
	 * the plotter at all (via ::before generated content) — the open
	 * question behind the missing icons. */
	macsurf_debug_log_writef("webfont: family fam=%s",
			lwc_string_data(family));
	return &g_webfonts[free_i];
}

/* Ensure the @font-face file for `family` is fetched/cached. Cheap to call per
 * text run: does real work only the first time a family is seen (and once per
 * fetch retry). Round 1 stops at "bytes held"; rendering comes later. */
void
macos9_webfont_ensure(struct content *content, lwc_string *family)
{
	struct webfont_slot *slot;
	const char *urlstr;

	if (content == NULL || family == NULL)
		return;

	slot = webfont_slot_for(family);
	if (slot == NULL)
		return;
	if (slot->state == WF_NONE || slot->state == WF_LOADED ||
			slot->state == WF_FETCHING)
		return;

	/* Resolve the src URL once (needs the content's select ctx). */
	if (slot->url == NULL) {
		slot->url = html_macsurf_font_face_url(content, family);
		if (slot->url == NULL) {
			/* No downloadable @font-face for this family (e.g.
			 * sans-serif, or an icon font whose src formats we
			 * can't use). Remember so we don't re-query every
			 * paint. DIAG distinguishes "family reached the
			 * accessor but no usable @font-face" from "family
			 * never reached us" (the latter shows no such line). */
			macsurf_debug_log_writef("webfont: no-face fam=%s",
					lwc_string_data(family));
			slot->state = WF_NONE;
			return;
		}
		macsurf_debug_log_writef("webfont: resolved fam=%s url=%s",
				lwc_string_data(family),
				nsurl_access(slot->url));
	}

	urlstr = nsurl_access(slot->url);

	/* Disk-cache hit: hold the bytes and we're done for Round 1. */
	{
		char *body = NULL;
		long blen = 0;
		char mime[128];
		int status = 0;

		mime[0] = '\0';
		if (macos9_cache_lookup(urlstr, &body, &blen, mime,
				(int) sizeof(mime), &status) == 1 &&
				body != NULL && blen > 0) {
			if (slot->data != NULL) {
				free(slot->data);
				slot->data = NULL;
			}
			slot->data = (char *) malloc((size_t) blen);
			if (slot->data != NULL) {
				memcpy(slot->data, body, (size_t) blen);
				slot->len = blen;
			}
			free(body);
			slot->state = WF_LOADED;
			macsurf_debug_log_writef(
					"webfont: loaded %ld bytes fam=%s",
					blen, lwc_string_data(family));
			return;
		}
	}

	/* Cache miss: kick one fetch; the callback repaints and the next
	 * ensure reads the cached bytes. */
	if (slot->fetch == NULL) {
		nserror e = hlcache_handle_retrieve(slot->url,
				HLCACHE_RETRIEVE_SNIFF_TYPE, NULL, NULL,
				webfont_fetch_cb, slot, NULL, CONTENT_ANY,
				&slot->fetch);
		if (e == NSERROR_OK) {
			slot->state = WF_FETCHING;
			macsurf_debug_log_writef(
					"webfont: fetch start fam=%s url=%s",
					lwc_string_data(family), urlstr);
		} else {
			slot->fetch = NULL;
		}
	}
}

#endif /* __MACOS9__ */
