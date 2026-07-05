/*
 * MacSurf - macos9_webfont.c
 *
 * Downloadable webfont (@font-face) glyph rendering. Modern sites ship their
 * icon fonts (FontAwesome, Material Design Icons) as @font-face rules whose
 * glyphs live in the Unicode Private-Use Area. Stock NetSurf parses @font-face
 * but never fetches the src, so those glyphs never render (they collapse to
 * zero-width boxes). This module closes the whole gap:
 *
 *   1. Resolve a CSS font-family -> @font-face src URL (via the core accessor
 *      html_macsurf_font_face_url, which owns the select ctx) and fetch +
 *      disk-cache the font file. The core resolver prefers a RAW sfnt src
 *      (.ttf / .otf, format "truetype"/"opentype") over WOFF so we never need
 *      zlib inflate; WOFF2 (Brotli) is out of scope.
 *   2. Parse the sfnt table directory + head/maxp/hhea/hmtx/cmap/loca/glyf.
 *      cmap format 12 is required (icon fonts live in the Supplementary PUA,
 *      U+F0000+, beyond format-4's BMP range).
 *   3. macos9_webfont_advance(): glyph advance in px, so the MEASURE path can
 *      size the icon box (the async fetch is kicked here too — the first
 *      layout returns -1 while the font downloads, then a reformat re-measures
 *      once it's loaded).
 *   4. macos9_webfont_render(): flatten the glyf quadratic outline and fill it
 *      as a QuickDraw region (OpenRgn/CloseRgn = even-odd, so glyph holes like
 *      the ring in an info-circle render correctly), using the current fg.
 *
 * The parse + render geometry was byte-verified on Linux against tinker-
 * different's real materialdesignicons-webfont.ttf before this port.
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
#include "content/fetch.h"
#include "netsurf/content.h"
#include <libwapcaplet/libwapcaplet.h>

#include "macos9_disk_cache.h"
#include "macos9_webfont.h"
#include "macsurf_debug.h"

extern struct gui_window *macos9_window_list_head(void);
extern void macos9_window_request_reformat(struct gui_window *g);

/* Core accessor (content/handlers/html/html.c): family -> @font-face src URL,
 * or NULL if the family has no usable downloadable @font-face rule. Returns a
 * new nsurl ref the caller owns. */
extern struct nsurl *html_macsurf_font_face_url(struct content *c,
		lwc_string *family);

/* ------------------------------------------------------------------ */
/* big-endian sfnt byte readers (bounds are checked by the callers)   */

typedef unsigned char  wf_u8;
typedef unsigned short wf_u16;
typedef unsigned long  wf_u32;

static wf_u16 wf_rd16(const wf_u8 *p)
{
	return (wf_u16) (((wf_u16) p[0] << 8) | p[1]);
}

static wf_u32 wf_rd32(const wf_u8 *p)
{
	return ((wf_u32) p[0] << 24) | ((wf_u32) p[1] << 16) |
	       ((wf_u32) p[2] << 8) | (wf_u32) p[3];
}

static short wf_rds16(const wf_u8 *p)
{
	return (short) wf_rd16(p);
}

/* ------------------------------------------------------------------ */
/* Per-family slot state.                                             */

enum {
	WF_NEW = 0,	/* just allocated, not yet resolved            */
	WF_NONE,	/* family has no @font-face URI (don't retry)   */
	WF_FETCHING,	/* fetch in flight                              */
	WF_LOADED	/* bytes held (ready to parse / parsed)         */
};

struct webfont_slot {
	lwc_string    *family;	/* ref'd; NULL = free slot                 */
	int            state;
	struct nsurl  *url;	/* resolved src URL (ref'd), or NULL       */
	char          *data;	/* font bytes (raw sfnt), or NULL          */
	long           len;
	struct fetch  *fetch;	/* in-flight RAW fetch, or NULL            */
	char          *dl_buf;	/* accumulating download buffer, or NULL   */
	long           dl_len;	/* bytes accumulated so far                */
	long           dl_cap;	/* capacity of dl_buf                       */
	int            fails;	/* failed fetch attempts (storm control)   */

	/* parsed sfnt state: 0 = not parsed, 1 = parsed OK, -1 = bad font */
	int            parsed;
	wf_u32         t_cmap;	/* table offsets into `data`               */
	wf_u32         t_loca;
	wf_u32         t_glyf;
	wf_u32         t_hmtx;
	wf_u16         upem;	/* unitsPerEm                              */
	wf_u16         idx_to_loc;	/* 0 = short loca, 1 = long           */
	wf_u16         num_glyphs;
	wf_u16         num_hm;	/* numberOfHMetrics                        */
	wf_u32         cmap12;	/* offset of a format-12 subtable, or 0    */
	wf_u32         cmap4;	/* offset of a format-4 subtable, or 0     */
};

/* Give up refetching after this many failed attempts. */
#define WEBFONT_MAX_FAILS 3

/* Cap on a font file we'll hold in RAM (raw sfnt). MDI is ~1 MB; 4 MB covers
 * the largest realistic icon/text webfont without letting a rogue URL balloon
 * the heap. */
#define WEBFONT_MAX_BYTES (4L * 1024L * 1024L)

#define WEBFONT_MAX 8
static struct webfont_slot g_webfonts[WEBFONT_MAX];

/* PUA ranges: BMP PUA + Supplementary PUA-A/B (matches macos9_font.c blank). */
#define WF_IS_PUA(cp) (((cp) >= 0xE000UL && (cp) <= 0xF8FFUL) || \
		       ((cp) >= 0xF0000UL && (cp) <= 0x10FFFDUL))

/* Outline decode caps (icon glyphs are tiny; MDI max ~60 pts). Over-cap ->
 * skip drawing (never crash). Static because rendering is single-threaded. */
#define WF_MAXPTS  1024
#define WF_MAXCONT 64
#define WF_QUAD_STEPS 6

static int   s_px[WF_MAXPTS];
static int   s_py[WF_MAXPTS];
static wf_u8 s_on[WF_MAXPTS];
static wf_u8 s_fl[WF_MAXPTS];
static wf_u16 s_ends[WF_MAXCONT];

/* ------------------------------------------------------------------ */
/* fetch completion (Round 1)                                         */

static void webfont_parse(struct webfont_slot *slot);

static void
webfont_dl_reset(struct webfont_slot *slot)
{
	if (slot->dl_buf != NULL) {
		free(slot->dl_buf);
		slot->dl_buf = NULL;
	}
	slot->dl_len = 0;
	slot->dl_cap = 0;
}

/* Raw-fetch callback (content/fetch.h). We fetch the font BELOW the hlcache /
 * content layer on purpose: a CONTENT_ANY retrieve of a font MIME (no content
 * handler) makes NetSurf abort the fetch after the first read (~15 KB) — the
 * whole reason the font never downloaded. fetch_start has no content layer, so
 * the fetcher runs to completion and hands us the bytes directly, which we
 * accumulate and then parse. Ownership: after FINISHED/ERROR the fetch object
 * is freed by NetSurf — just null our handle (mirrors llcache.c). */
static void
webfont_fetch_cb(const fetch_msg *msg, void *pw)
{
	struct webfont_slot *slot = (struct webfont_slot *) pw;
	struct gui_window *gw;

	if (slot == NULL)
		return;

	switch (msg->type) {
	case FETCH_DATA: {
		const unsigned char *b = msg->data.header_or_data.buf;
		long l = (long) msg->data.header_or_data.len;
		if (b == NULL || l <= 0)
			break;
		if (slot->dl_len + l > WEBFONT_MAX_BYTES) {
			/* Too big: stop accumulating, let it finish, discard. */
			webfont_dl_reset(slot);
			slot->dl_len = WEBFONT_MAX_BYTES + 1;	/* poison */
			break;
		}
		if (slot->dl_len == WEBFONT_MAX_BYTES + 1)
			break;	/* already poisoned */
		if (slot->dl_len + l > slot->dl_cap) {
			long ncap = (slot->dl_cap > 0) ? slot->dl_cap : 65536L;
			char *nb;
			while (ncap < slot->dl_len + l)
				ncap *= 2;
			nb = (char *) realloc(slot->dl_buf, (size_t) ncap);
			if (nb == NULL)
				break;	/* OOM: keep what we have; likely fails parse */
			slot->dl_buf = nb;
			slot->dl_cap = ncap;
		}
		memcpy(slot->dl_buf + slot->dl_len, b, (size_t) l);
		slot->dl_len += l;
		break;
	}
	case FETCH_FINISHED:
		slot->fetch = NULL;
		if (slot->dl_buf != NULL && slot->dl_len > 0 &&
				slot->dl_len <= WEBFONT_MAX_BYTES) {
			if (slot->data != NULL)
				free(slot->data);
			slot->data = slot->dl_buf;
			slot->len = slot->dl_len;
			slot->dl_buf = NULL;
			slot->dl_len = 0;
			slot->dl_cap = 0;
			slot->state = WF_LOADED;
			slot->parsed = 0;
			macsurf_debug_log_writef(
					"webfont: loaded %ld bytes fam=%s",
					slot->len,
					(slot->family != NULL) ?
					lwc_string_data(slot->family) : "?");
		} else {
			webfont_dl_reset(slot);
			slot->state = WF_NONE;	/* empty / oversized: don't retry */
		}
		/* Re-layout so the MEASURE path re-runs and sizes the icon
		 * boxes with the now-known advances, then repaint. */
		gw = macos9_window_list_head();
		if (gw != NULL) {
			macos9_window_request_reformat(gw);
			macos9_window_invalidate_content(gw);
		}
		break;
	case FETCH_ERROR:
		slot->fetch = NULL;
		/* Salvage: a close-delimited response (no Content-Length) can
		 * arrive complete and only THEN report the connection close as
		 * an error. If what we accumulated parses as a full sfnt, use
		 * it. webfont_parse validates the table structure, so a genuine
		 * truncation is still rejected here and falls through to retry. */
		if (slot->dl_buf != NULL && slot->dl_len >= 12 &&
				slot->dl_len <= WEBFONT_MAX_BYTES) {
			slot->data = slot->dl_buf;
			slot->len = slot->dl_len;
			slot->dl_buf = NULL;
			slot->dl_len = 0;
			slot->dl_cap = 0;
			slot->parsed = 0;
			webfont_parse(slot);
			if (slot->parsed == 1) {
				slot->state = WF_LOADED;
				macsurf_debug_log_writef(
					"webfont: salvaged %ld bytes on close fam=%s",
					slot->len,
					(slot->family != NULL) ?
					lwc_string_data(slot->family) : "?");
				gw = macos9_window_list_head();
				if (gw != NULL) {
					macos9_window_request_reformat(gw);
					macos9_window_invalidate_content(gw);
				}
				break;
			}
			/* incomplete: drop and retry */
			free(slot->data);
			slot->data = NULL;
			slot->len = 0;
			slot->parsed = 0;
		}
		webfont_dl_reset(slot);
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
			macsurf_debug_log_writef("webfont: fetch error fam=%s (%d)",
					(slot->family != NULL) ?
					lwc_string_data(slot->family) : "?",
					slot->fails);
		}
		gw = macos9_window_list_head();
		if (gw != NULL)
			macos9_window_invalidate_content(gw);
		break;
	default:
		/* FETCH_HEADER / FETCH_PROGRESS / FETCH_REDIRECT / etc: ignore. */
		break;
	}
}

/* Find the slot for a family (interned lwc_string, pointer-compared) or claim a
 * free one. NULL only if the registry is full. */
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
	g_webfonts[free_i].dl_buf = NULL;
	g_webfonts[free_i].dl_len = 0;
	g_webfonts[free_i].dl_cap = 0;
	g_webfonts[free_i].fails = 0;
	g_webfonts[free_i].parsed = 0;
	macsurf_debug_log_writef("webfont: family fam=%s",
			lwc_string_data(family));
	return &g_webfonts[free_i];
}

/* Find WITHOUT claiming (advance/render read the slot after ensure). */
static struct webfont_slot *
webfont_slot_find(lwc_string *family)
{
	int i;
	for (i = 0; i < WEBFONT_MAX; i++) {
		if (g_webfonts[i].family == family)
			return &g_webfonts[i];
	}
	return NULL;
}

/* Locate a table in the raw sfnt directory. Returns 1 + offset/len, else 0. */
static int
webfont_find_table(const wf_u8 *f, wf_u32 flen, const char *tag,
		wf_u32 *off, wf_u32 *len)
{
	wf_u16 numTables, i;

	if (flen < 12)
		return 0;
	numTables = wf_rd16(f + 4);
	for (i = 0; i < numTables; i++) {
		const wf_u8 *rec = f + 12 + (wf_u32) i * 16;
		if ((wf_u32) (rec - f) + 16 > flen)
			return 0;
		if (memcmp(rec, tag, 4) == 0) {
			*off = wf_rd32(rec + 8);
			*len = wf_rd32(rec + 12);
			if (*off > flen || *off + *len > flen)
				return 0;
			return 1;
		}
	}
	return 0;
}

/* Parse the loaded font bytes once. Sets slot->parsed to 1 (ok) or -1 (bad). */
static void
webfont_parse(struct webfont_slot *slot)
{
	const wf_u8 *f = (const wf_u8 *) slot->data;
	wf_u32 flen = (wf_u32) slot->len;
	wf_u32 head_o, head_l, maxp_o, maxp_l, hhea_o, hhea_l, hmtx_o, hmtx_l;
	wf_u32 cmap_o, cmap_l, loca_o, loca_l, glyf_o, glyf_l;
	wf_u16 ntab, i;

	slot->parsed = -1;	/* pessimistic until proven */

	if (f == NULL || flen < 12)
		return;
	/* Accept only raw sfnt (0x00010000 TrueType, or 'OTTO'/'true'/'ttcf').
	 * A WOFF/WOFF2 body starting 'wOFF'/'wOF2' would misparse — reject. */
	{
		wf_u32 ver = wf_rd32(f);
		if (ver != 0x00010000UL &&
				memcmp(f, "OTTO", 4) != 0 &&
				memcmp(f, "true", 4) != 0 &&
				memcmp(f, "ttcf", 4) != 0)
			return;
	}

	if (!webfont_find_table(f, flen, "head", &head_o, &head_l) ||
			!webfont_find_table(f, flen, "maxp", &maxp_o, &maxp_l) ||
			!webfont_find_table(f, flen, "hhea", &hhea_o, &hhea_l) ||
			!webfont_find_table(f, flen, "hmtx", &hmtx_o, &hmtx_l) ||
			!webfont_find_table(f, flen, "cmap", &cmap_o, &cmap_l) ||
			!webfont_find_table(f, flen, "loca", &loca_o, &loca_l) ||
			!webfont_find_table(f, flen, "glyf", &glyf_o, &glyf_l))
		return;
	if (head_o + 52 > flen || maxp_o + 6 > flen || hhea_o + 36 > flen)
		return;

	slot->upem = wf_rd16(f + head_o + 18);
	slot->idx_to_loc = wf_rd16(f + head_o + 50);
	slot->num_glyphs = wf_rd16(f + maxp_o + 4);
	slot->num_hm = wf_rd16(f + hhea_o + 34);
	slot->t_hmtx = hmtx_o;
	slot->t_loca = loca_o;
	slot->t_glyf = glyf_o;
	slot->t_cmap = cmap_o;
	slot->cmap12 = 0;
	slot->cmap4 = 0;
	if (slot->upem == 0)
		return;

	/* Scan the cmap for a usable Unicode subtable: prefer format 12 (full
	 * Unicode, needed for Supplementary-PUA icon codepoints), fall back to
	 * format 4 (BMP). */
	if (cmap_o + 4 > flen)
		return;
	ntab = wf_rd16(f + cmap_o + 2);
	for (i = 0; i < ntab; i++) {
		const wf_u8 *rec = f + cmap_o + 4 + (wf_u32) i * 8;
		wf_u32 sub;
		wf_u16 fmt;
		if ((wf_u32) (rec - f) + 8 > flen)
			break;
		sub = wf_rd32(rec + 4);
		if (cmap_o + sub + 2 > flen)
			continue;
		fmt = wf_rd16(f + cmap_o + sub);
		if (fmt == 12 && slot->cmap12 == 0)
			slot->cmap12 = cmap_o + sub;
		else if (fmt == 4 && slot->cmap4 == 0)
			slot->cmap4 = cmap_o + sub;
	}
	if (slot->cmap12 == 0 && slot->cmap4 == 0)
		return;

	slot->parsed = 1;
	macsurf_debug_log_writef(
			"webfont: parsed fam=%s upem=%d nglyph=%d fmt12=%d",
			(slot->family != NULL) ? lwc_string_data(slot->family) : "?",
			(int) slot->upem, (int) slot->num_glyphs,
			(slot->cmap12 != 0) ? 1 : 0);
}

/* Map a Unicode codepoint -> glyph index via the parsed cmap. 0 = no glyph. */
static wf_u32
webfont_cmap_lookup(struct webfont_slot *slot, wf_u32 cp)
{
	const wf_u8 *f = (const wf_u8 *) slot->data;
	wf_u32 flen = (wf_u32) slot->len;

	if (slot->cmap12 != 0) {
		const wf_u8 *s = f + slot->cmap12;
		wf_u32 ngroups, g;
		if (slot->cmap12 + 16 > flen)
			return 0;
		ngroups = wf_rd32(s + 12);
		for (g = 0; g < ngroups; g++) {
			const wf_u8 *grp = s + 16 + g * 12;
			wf_u32 start, end, sgid;
			if ((wf_u32) (grp - f) + 12 > flen)
				break;
			start = wf_rd32(grp);
			end = wf_rd32(grp + 4);
			sgid = wf_rd32(grp + 8);
			if (cp >= start && cp <= end)
				return sgid + (cp - start);
		}
		return 0;
	}
	if (slot->cmap4 != 0 && cp <= 0xFFFF) {
		const wf_u8 *s = f + slot->cmap4;
		wf_u16 segX2, segc, j;
		const wf_u8 *endc, *startc, *iddelta, *idrange;
		if (slot->cmap4 + 14 > flen)
			return 0;
		segX2 = wf_rd16(s + 6);
		segc = segX2 / 2;
		endc = s + 14;
		startc = endc + segX2 + 2;
		iddelta = startc + segX2;
		idrange = iddelta + segX2;
		for (j = 0; j < segc; j++) {
			wf_u16 end2;
			if ((wf_u32) (endc + j * 2 - f) + 2 > flen)
				return 0;
			end2 = wf_rd16(endc + j * 2);
			if (cp <= end2) {
				wf_u16 start2 = wf_rd16(startc + j * 2);
				short delta = wf_rds16(iddelta + j * 2);
				wf_u16 ro = wf_rd16(idrange + j * 2);
				if (cp < start2)
					return 0;
				if (ro == 0)
					return (wf_u16) (cp + delta);
				else {
					const wf_u8 *gp = idrange + j * 2 + ro +
						(cp - start2) * 2;
					wf_u16 gid;
					if ((wf_u32) (gp - f) + 2 > flen)
						return 0;
					gid = wf_rd16(gp);
					if (gid == 0)
						return 0;
					return (wf_u16) (gid + delta);
				}
			}
		}
	}
	return 0;
}

/* Advance width for a glyph, in font units. */
static wf_u32
webfont_advance_units(struct webfont_slot *slot, wf_u32 gid)
{
	const wf_u8 *f = (const wf_u8 *) slot->data;
	wf_u32 flen = (wf_u32) slot->len;
	wf_u32 idx = (gid < slot->num_hm) ? gid :
		(slot->num_hm > 0 ? (wf_u32) slot->num_hm - 1 : 0);
	if (slot->t_hmtx + idx * 4 + 2 > flen)
		return slot->upem;	/* fall back to 1em */
	return wf_rd16(f + slot->t_hmtx + idx * 4);
}

/* glyf offset + length for a glyph via loca. Returns 1 + off/next, else 0. */
static int
webfont_glyf_range(struct webfont_slot *slot, wf_u32 gid,
		wf_u32 *goff, wf_u32 *gnext)
{
	const wf_u8 *f = (const wf_u8 *) slot->data;
	wf_u32 flen = (wf_u32) slot->len;
	if (slot->idx_to_loc == 0) {
		if (slot->t_loca + (gid + 1) * 2 + 2 > flen)
			return 0;
		*goff = (wf_u32) wf_rd16(f + slot->t_loca + gid * 2) * 2;
		*gnext = (wf_u32) wf_rd16(f + slot->t_loca + (gid + 1) * 2) * 2;
	} else {
		if (slot->t_loca + (gid + 1) * 4 + 4 > flen)
			return 0;
		*goff = wf_rd32(f + slot->t_loca + gid * 4);
		*gnext = wf_rd32(f + slot->t_loca + (gid + 1) * 4);
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* public: ensure fetch/parse                                         */

static void
webfont_load_if_cached(struct webfont_slot *slot)
{
	const char *urlstr = nsurl_access(slot->url);
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
			slot->state = WF_LOADED;
			slot->parsed = 0;
			macsurf_debug_log_writef(
					"webfont: loaded %ld bytes fam=%s",
					blen,
					(slot->family != NULL) ?
					lwc_string_data(slot->family) : "?");
		}
		free(body);
	}
}

void
macos9_webfont_ensure(struct content *content, lwc_string *family)
{
	struct webfont_slot *slot;

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
			macsurf_debug_log_writef("webfont: no-face fam=%s",
					lwc_string_data(family));
			slot->state = WF_NONE;
			return;
		}
		macsurf_debug_log_writef("webfont: resolved fam=%s url=%s",
				lwc_string_data(family),
				nsurl_access(slot->url));
	}

	/* Disk-cache hit: hold the bytes. */
	webfont_load_if_cached(slot);
	if (slot->state == WF_LOADED)
		return;

	/* Cache miss: kick one RAW fetch (below the content layer, so an
	 * unhandled font MIME can't make NetSurf abort it). The callback
	 * accumulates the bytes, then reformats+repaints. */
	if (slot->fetch == NULL) {
		nserror e;
		webfont_dl_reset(slot);
		e = fetch_start(slot->url, NULL, webfont_fetch_cb, slot,
				true, NULL, NULL, true, false, NULL,
				&slot->fetch);
		if (e == NSERROR_OK && slot->fetch != NULL) {
			slot->state = WF_FETCHING;
			macsurf_debug_log_writef(
					"webfont: fetch start fam=%s url=%s",
					lwc_string_data(family),
					nsurl_access(slot->url));
		} else {
			slot->fetch = NULL;
		}
	}
}

/* Shared: ensure loaded+parsed, return the slot if it has a usable glyph for
 * cp, else NULL (and *gid set when non-NULL). */
static struct webfont_slot *
webfont_ready(struct content *content, lwc_string *family, wf_u32 cp,
		wf_u32 *gid_out)
{
	struct webfont_slot *slot;

	macos9_webfont_ensure(content, family);
	slot = webfont_slot_find(family);
	if (slot == NULL || slot->state != WF_LOADED)
		return NULL;
	if (slot->parsed == 0)
		webfont_parse(slot);
	if (slot->parsed != 1)
		return NULL;
	{
		wf_u32 gid = webfont_cmap_lookup(slot, cp);
		if (gid == 0)
			return NULL;
		*gid_out = gid;
		return slot;
	}
}

int
macos9_webfont_advance(struct content *content, lwc_string *family,
		unsigned long cp, int size_px)
{
	struct webfont_slot *slot;
	wf_u32 gid = 0;
	wf_u32 adv_units;
	double adv;

	if (content == NULL || family == NULL || size_px <= 0)
		return -1;
	slot = webfont_ready(content, family, (wf_u32) cp, &gid);
	if (slot == NULL)
		return -1;
	adv_units = webfont_advance_units(slot, gid);
	adv = (double) adv_units * (double) size_px / (double) slot->upem;
	return (int) (adv + 0.5);
}

/* ------------------------------------------------------------------ */
/* glyph outline -> QuickDraw region fill                             */

#define WF_RND(v) (((v) >= 0.0) ? (long) ((v) + 0.5) : (long) ((v) - 0.5))

static int   g_pen_x;
static int   g_base_y;
static double g_scale;
static double g_curx, g_cury, g_ctrlx, g_ctrly;
static int   g_have_ctrl;

static void
wf_screen_lineto(double gx, double gy)
{
	short sx = (short) (g_pen_x + WF_RND(gx * g_scale));
	short sy = (short) (g_base_y - WF_RND(gy * g_scale));
	LineTo(sx, sy);
}

static void
wf_quad(double x0, double y0, double cx, double cy, double x1, double y1)
{
	int s;
	for (s = 1; s <= WF_QUAD_STEPS; s++) {
		double t = (double) s / (double) WF_QUAD_STEPS;
		double u = 1.0 - t;
		double qx = u * u * x0 + 2.0 * u * t * cx + t * t * x1;
		double qy = u * u * y0 + 2.0 * u * t * cy + t * t * y1;
		wf_screen_lineto(qx, qy);
	}
}

/* Frame one contour into the open region (TrueType quadratic outline with
 * implied on-curve midpoints between consecutive off-curve points). */
static void
wf_emit_contour(const int *px, const int *py, const wf_u8 *on, int cnt)
{
	int startx, starty, idx, i;
	short sx, sy;

	if (cnt <= 0)
		return;
	if (on[0]) {
		startx = px[0]; starty = py[0]; idx = 1;
	} else if (on[cnt - 1]) {
		startx = px[cnt - 1]; starty = py[cnt - 1]; idx = 0;
	} else {
		startx = (px[0] + px[cnt - 1]) / 2;
		starty = (py[0] + py[cnt - 1]) / 2;
		idx = 0;
	}
	sx = (short) (g_pen_x + WF_RND((double) startx * g_scale));
	sy = (short) (g_base_y - WF_RND((double) starty * g_scale));
	MoveTo(sx, sy);
	g_curx = startx; g_cury = starty; g_have_ctrl = 0;
	g_ctrlx = 0; g_ctrly = 0;

	for (i = 0; i < cnt; i++) {
		int j = idx + i;
		int k = (j >= cnt) ? (j - cnt) : j;
		double ptx = px[k];
		double pty = py[k];
		if (on[k]) {
			if (g_have_ctrl) {
				wf_quad(g_curx, g_cury, g_ctrlx, g_ctrly, ptx, pty);
				g_have_ctrl = 0;
			} else {
				wf_screen_lineto(ptx, pty);
			}
			g_curx = ptx; g_cury = pty;
		} else {
			if (g_have_ctrl) {
				double mx = (g_ctrlx + ptx) / 2.0;
				double my = (g_ctrly + pty) / 2.0;
				wf_quad(g_curx, g_cury, g_ctrlx, g_ctrly, mx, my);
				g_curx = mx; g_cury = my;
				g_ctrlx = ptx; g_ctrly = pty;
			} else {
				g_ctrlx = ptx; g_ctrly = pty; g_have_ctrl = 1;
			}
		}
	}
	if (g_have_ctrl)
		wf_quad(g_curx, g_cury, g_ctrlx, g_ctrly, startx, starty);
	else
		wf_screen_lineto((double) startx, (double) starty);
}

int
macos9_webfont_render(struct content *content, lwc_string *family,
		unsigned long cp, int pen_x, int baseline_y, int size_px)
{
	struct webfont_slot *slot;
	wf_u32 gid = 0, goff = 0, gnext = 0;
	const wf_u8 *f, *g, *p, *pend;
	wf_u32 flen, adv_units;
	int adv_px, base, ci;
	short nc;
	wf_u16 npts, insLen, fi;
	RgnHandle rgn;

	if (content == NULL || family == NULL || size_px <= 0)
		return -1;
	slot = webfont_ready(content, family, (wf_u32) cp, &gid);
	if (slot == NULL)
		return -1;

	adv_units = webfont_advance_units(slot, gid);
	adv_px = (int) ((double) adv_units * (double) size_px /
			(double) slot->upem + 0.5);

	if (!webfont_glyf_range(slot, gid, &goff, &gnext))
		return adv_px;
	if (gnext <= goff)
		return adv_px;	/* empty glyph (e.g. space): advance only */

	f = (const wf_u8 *) slot->data;
	flen = (wf_u32) slot->len;
	if (slot->t_glyf + gnext > flen || slot->t_glyf + goff + 10 > flen)
		return adv_px;
	g = f + slot->t_glyf + goff;
	pend = g + (gnext - goff);
	nc = wf_rds16(g);
	if (nc <= 0 || nc > WF_MAXCONT)
		return adv_px;	/* composite / degenerate: skip, keep advance */

	/* fixes632 DIAG — first ~15 glyph renders: codepoint, glyph id, pen
	 * (x,baseline y), size, advance, contour count, and glyf bbox (font
	 * units). Tells us where each icon lands vs its box so we can pin the
	 * alignment. */
	{
		static int g_diag_n = 0;
		if (g_diag_n < 15) {
			g_diag_n++;
			macsurf_debug_log_writef(
				"wf render cp=%ld gid=%ld x=%d ybase=%d sz=%d adv=%d nc=%d bboxY=%d..%d",
				(long) cp, (long) gid,
				pen_x, baseline_y, size_px, adv_px, (int) nc,
				(int) wf_rds16(g + 4), (int) wf_rds16(g + 8));
		}
	}

	/* endPtsOfContours */
	p = g + 10;
	for (ci = 0; ci < nc; ci++) {
		if (p + 2 > pend)
			return adv_px;
		s_ends[ci] = wf_rd16(p);
		p += 2;
	}
	npts = (wf_u16) (s_ends[nc - 1] + 1);
	if (npts > WF_MAXPTS)
		return adv_px;
	if (p + 2 > pend)
		return adv_px;
	insLen = wf_rd16(p);
	p += 2;
	p += insLen;
	if (p > pend)
		return adv_px;

	/* flags (with repeat) */
	fi = 0;
	while (fi < npts) {
		wf_u8 fl;
		if (p >= pend)
			return adv_px;
		fl = *p++;
		s_fl[fi] = fl;
		s_on[fi] = (wf_u8) (fl & 1);
		fi++;
		if (fl & 0x08) {
			wf_u8 rep;
			if (p >= pend)
				return adv_px;
			rep = *p++;
			while (rep-- && fi < npts) {
				s_fl[fi] = fl;
				s_on[fi] = (wf_u8) (fl & 1);
				fi++;
			}
		}
	}
	/* x coordinates */
	{
		int x = 0;
		wf_u16 i2;
		for (i2 = 0; i2 < npts; i2++) {
			wf_u8 fl = s_fl[i2];
			if (fl & 0x02) {
				int d;
				if (p >= pend)
					return adv_px;
				d = *p++;
				x += (fl & 0x10) ? d : -d;
			} else if (!(fl & 0x10)) {
				if (p + 2 > pend)
					return adv_px;
				x += wf_rds16(p);
				p += 2;
			}
			s_px[i2] = x;
		}
	}
	/* y coordinates */
	{
		int y = 0;
		wf_u16 i2;
		for (i2 = 0; i2 < npts; i2++) {
			wf_u8 fl = s_fl[i2];
			if (fl & 0x04) {
				int d;
				if (p >= pend)
					return adv_px;
				d = *p++;
				y += (fl & 0x20) ? d : -d;
			} else if (!(fl & 0x20)) {
				if (p + 2 > pend)
					return adv_px;
				y += wf_rds16(p);
				p += 2;
			}
			s_py[i2] = y;
		}
	}

	/* Build the region: each contour framed as a closed loop; OpenRgn/
	 * CloseRgn accumulates them even-odd so glyph holes render. */
	rgn = NewRgn();
	if (rgn == NULL)
		return adv_px;
	g_pen_x = pen_x;
	g_base_y = baseline_y;
	g_scale = (double) size_px / (double) slot->upem;
	OpenRgn();
	base = 0;
	for (ci = 0; ci < nc; ci++) {
		int end = s_ends[ci];
		int cnt = end - base + 1;
		if (cnt > 0 && base >= 0 && end < npts)
			wf_emit_contour(&s_px[base], &s_py[base], &s_on[base], cnt);
		base = end + 1;
	}
	CloseRgn(rgn);
	PaintRgn(rgn);
	DisposeRgn(rgn);
	return adv_px;
}

#endif /* __MACOS9__ */
