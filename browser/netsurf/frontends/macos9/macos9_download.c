/*
 * MacSurf - Mac OS 9 frontend for NetSurf
 * macos9_download.c - gui_download_table callbacks + download manager
 *
 * fixes645 - download manager V2.
 *
 * ── Why V1 (fixes313) was broken on real hardware ──────────────────
 * V1 opened a modal Navigation Services save dialog (NavPutFile) from
 * inside the fetch/content callback that fires when NetSurf core decides
 * a response is a download. On the G3 that call returned err=-5699
 * (kNavInvalidSystemConfigErr): Navigation Services refuses to run its
 * modal dialog from that context while OT sync-idle notifiers are
 * pumping. Result: create() returned NULL, the received bytes were
 * discarded, and "downloading did nothing" on HTTPS sites. (It happened
 * to succeed once on a quiescent http page, which is why it looked
 * intermittent.)
 *
 * ── V2 design ──────────────────────────────────────────────────────
 * No modal dialog in the callback. Downloads auto-save into a
 * "MacSurf Downloads" folder next to the app (Desktop fallback), with
 * the server-suggested filename (sanitised for HFS + de-duplicated).
 * Because there is no single modal dialog, several downloads can run at
 * once. Each download is a node in a list that a modeless "Downloads"
 * window draws (filename + live byte progress + Done/Failed). The window
 * auto-opens on the first download and is routed from the main event
 * loop (update / mouseDown) via macos9_download_mgr_* below.
 *
 * Callback wiring:
 *   create → resolve Downloads folder → unique FSSpec → FSpCreate +
 *            FSpOpenDF → push node → show manager. No dialog, no -5699.
 *   data   → FSWrite each chunk; update byte count; repaint manager +
 *            parent status bar (throttled).
 *   done   → FSClose; mark node Done; final repaint. Node is KEPT in the
 *            list (so the manager shows history); freed later by the LRU
 *            cap, not here.
 *   error  → FSClose; FSpDelete the partial file; mark node Failed.
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "utils/ns_errors.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "netsurf/download.h"
#include "desktop/download.h"

#include "macos9.h"
#include "macsurf_debug.h"

#ifdef __MACOS9__
#include <Files.h>
#include <Script.h>
#endif

/* Newest-first list of downloads the manager window draws. Nodes stay
 * after completion (for history) and are freed by the LRU cap. */
static struct gui_download_window *g_dl_list = NULL;
static int g_dl_count = 0;
#define MACSURF_DL_MAX 64

#ifdef __MACOS9__
static WindowRef   g_dl_mgr_win = NULL;
static ControlRef  g_btn_open_folder = NULL;
static ControlRef  g_btn_clear = NULL;
static ControlRef  g_dl_sb = NULL;
static int         g_dl_scroll_top = 0;
static unsigned long g_last_click_time = 0;
static int         g_last_click_row = -1;
static void dl_mgr_progress(void);   /* fwd: dl_cancel calls it */

/* Map a NetSurf MIME string to a Mac type/creator pair. Best-effort -
 * anything not recognised falls back to 'BINA' / '????'. */
static void
macos9_download_mime_to_type(const char *mime,
		OSType *out_type, OSType *out_creator)
{
	*out_type = 'BINA';
	*out_creator = '????';
	if (mime == NULL) return;
	if (strncmp(mime, "application/pdf", 15) == 0) {
		*out_type = 'PDF '; *out_creator = 'CARO'; return;
	}
	if (strncmp(mime, "application/zip", 15) == 0 ||
	    strncmp(mime, "application/x-zip", 17) == 0) {
		*out_type = 'ZIP '; *out_creator = 'SITx'; return;
	}
	if (strncmp(mime, "application/x-stuffit", 21) == 0 ||
	    strncmp(mime, "application/x-sit", 17) == 0) {
		*out_type = 'SITD'; *out_creator = 'SIT!'; return;
	}
	if (strncmp(mime, "image/jpeg", 10) == 0 ||
	    strncmp(mime, "image/jpg", 9) == 0) {
		*out_type = 'JPEG'; *out_creator = '8BIM'; return;
	}
	if (strncmp(mime, "image/png", 9) == 0) {
		*out_type = 'PNGf'; *out_creator = '8BIM'; return;
	}
	if (strncmp(mime, "image/gif", 9) == 0) {
		*out_type = 'GIFf'; *out_creator = '8BIM'; return;
	}
	if (strncmp(mime, "text/", 5) == 0) {
		*out_type = 'TEXT'; *out_creator = 'ttxt'; return;
	}
}

/* C string → Pascal filename, enforced HFS-legal at the Carbon boundary.
 * This is the LAST stop before FSMakeFSSpec/FSpCreate, so it hard-guards
 * both classic-HFS failure modes itself rather than trusting callers:
 *   - hard-truncate to 31 bytes (HFS leaf limit; a >31 name is paramErr).
 *     NOTE: was 63 (Str63's capacity) - wrong for a filename, and the
 *     source of BadPerimeter/paramErr(-50) reports when a long or
 *     un-truncated name slipped through.
 *   - replace ':' (the HFS path separator - illegal in a leaf), '/', and
 *     control chars with '-'.
 * Str63 has room for 31 comfortably. */
static void
macos9_download_cstr_to_p63(const char *src, Str63 dst)
{
	size_t n, i;
	if (src == NULL || src[0] == '\0') src = "download";
	n = strlen(src);
	if (n > 31) n = 31;                 /* HFS hard limit, NOT 63 */
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)src[i];
		if (c == ':' || c == '/' || c < 32) c = '-';
		dst[i + 1] = c;
	}
	dst[0] = (unsigned char)n;
}

/* Bounded C-string copy (no MSL strlcpy on CW8). */
static void
dl_strlcpy(char *dst, const char *src, size_t cap)
{
	size_t n;
	if (cap == 0) return;
	n = strlen(src);
	if (n > cap - 1) n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

/* Sanitise a server-suggested name into a classic-HFS-legal leaf: no
 * ':' (path separator) or control chars, capped to 28 chars so a
 * " NN" de-dup suffix still fits inside the 31-char HFS limit. */
static void
dl_sanitize_name(const char *src, char *dst, size_t cap)
{
	size_t i, n;
	if (src == NULL || src[0] == '\0') src = "download";
	n = strlen(src);
	if (cap > 29) cap = 29;          /* reserve room for " NN" */
	if (n > cap - 1) n = cap - 1;
	for (i = 0; i < n; i++) {
		char c = src[i];
		if (c == ':' || c == '/' || (unsigned char)c < 32) c = '-';
		dst[i] = c;
	}
	dst[n] = '\0';
	if (dst[0] == '\0') { dst[0] = 'd'; dst[1] = '\0'; }
}

/* Find a non-existent FSSpec in (vRef,dirID) for `leaf`, appending
 * " 2".." 99" if the name is taken. On success `leaf` is updated to the
 * name actually used and *out is a spec ready for FSpCreate. */
static OSErr
dl_unique_spec(short vRef, long dirID, char *leaf, size_t leaf_cap,
		FSSpec *out)
{
	char base[32];
	char trial[40];
	Str63 pn;
	int i;
	OSErr err;

	dl_strlcpy(base, leaf, sizeof base);
	if (strlen(base) > 27) base[27] = '\0';

	for (i = 0; i < 100; i++) {
		if (i == 0)
			dl_strlcpy(trial, leaf, sizeof trial);
		else
			sprintf(trial, "%s %d", base, i + 1);
		if (strlen(trial) > 31) trial[31] = '\0';
		macos9_download_cstr_to_p63(trial, pn);
		err = FSMakeFSSpec(vRef, dirID, pn, out);
		if (err == fnfErr) {              /* free slot - use it */
			dl_strlcpy(leaf, trial, leaf_cap);
			return noErr;
		}
		if (err != noErr) return err;     /* real error */
		/* else exists - try next suffix */
	}
	return dupFNErr;
}

/* Push a one-line status message to the parent window's status bar. */
static void
macos9_download_status(struct gui_download_window *dw, const char *msg)
{
	extern struct gui_window_table *macos9_window_table;
	if (dw == NULL || dw->parent == NULL || msg == NULL) return;
	if (macos9_window_table && macos9_window_table->set_status) {
		macos9_window_table->set_status(dw->parent, msg);
	}
}

/* ── Manager window ─────────────────────────────────────────────── */

#ifdef __MACOS9__
static void c_to_pstring(const char *src, unsigned char *dest)
{
	size_t len;
	if (src == NULL) { dest[0] = 0; return; }
	len = strlen(src);
	if (len > 255) len = 255;
	dest[0] = (unsigned char)len;
	memcpy(dest + 1, src, len);
}

static void dl_open_spec(const FSSpec *spec)
{
	AEAddressDesc target;
	AppleEvent ae, reply;
	AEDescList docList;
	OSType finderCreator = 'FNDR';
	OSErr err;

	err = AECreateDesc(typeApplSignature, &finderCreator, sizeof(OSType), &target);
	if (err != noErr) return;

	err = AECreateAppleEvent(kCoreEventClass, kAEOpenDocuments,
		&target, kAutoGenerateReturnID, kAnyTransactionID, &ae);
	AEDisposeDesc(&target);
	if (err != noErr) return;

	err = AECreateList(NULL, 0, false, &docList);
	if (err == noErr) {
		AEPutPtr(&docList, 1, typeFSS, spec, sizeof(FSSpec));
		AEPutParamDesc(&ae, keyDirectObject, &docList);
		AEDisposeDesc(&docList);
	}

	AESend(&ae, &reply, kAENoReply | kAENeverInteract,
		kAENormalPriority, kAEDefaultTimeout, NULL, NULL);
	AEDisposeDesc(&ae);
}

static void dl_mgr_open_folder(void)
{
	short vRef = 0;
	long dirID = 0;
	if (macos9_downloads_dir_get(&vRef, &dirID) == noErr) {
		FSSpec folderSpec;
		if (FSMakeFSSpec(vRef, dirID, "\p", &folderSpec) == noErr) {
			dl_open_spec(&folderSpec);
		}
	}
}

static void dl_mgr_clear_finished(void)
{
	struct gui_download_window **curr = &g_dl_list;
	while (*curr != NULL) {
		struct gui_download_window *entry = *curr;
		if (entry->dl_state != 0) {  /* Done or Stopped */
			*curr = entry->dl_next;
			free(entry);
			g_dl_count--;
		} else {
			curr = &entry->dl_next;
		}
	}
	if (g_dl_scroll_top > 0 && g_dl_count <= 5)
		g_dl_scroll_top = 0;
	dl_mgr_progress();
}

static void dl_format_bytes(unsigned long b, char *buf)
{
	if (b >= 1048576) {
		unsigned long mb = b / 1048576;
		unsigned long frac = ((b % 1048576) * 10) / 1048576;
		sprintf(buf, "%lu.%lu MB", mb, frac);
	} else if (b >= 1024) {
		sprintf(buf, "%lu KB", b / 1024);
	} else {
		sprintf(buf, "%lu bytes", b);
	}
}

static void dl_format_rate(unsigned long bps, char *buf)
{
	if (bps >= 1048576) {
		unsigned long mb = bps / 1048576;
		unsigned long frac = ((bps % 1048576) * 10) / 1048576;
		sprintf(buf, "%lu.%lu MB/s", mb, frac);
	} else if (bps >= 1024) {
		sprintf(buf, "%lu KB/s", bps / 1024);
	} else if (bps > 0) {
		sprintf(buf, "%lu B/s", bps);
	} else {
		strcpy(buf, "-- KB/s");
	}
}

static void dl_format_eta(const struct gui_download_window *dw, char *buf)
{
	if (dw->last_rate_bps > 0 && dw->bytes_written < dw->total_length) {
		unsigned long rem = dw->total_length - dw->bytes_written;
		unsigned long eta = rem / dw->last_rate_bps;
		if (eta < 60)
			sprintf(buf, "%lus left", eta);
		else if (eta < 3600)
			sprintf(buf, "%lum %lus left", eta / 60, eta % 60);
		else
			sprintf(buf, "%luh %lum left", eta / 3600, (eta % 3600) / 60);
	} else {
		strcpy(buf, "-- left");
	}
}

static void dl_draw_btn(const Rect *r, const unsigned char *pstr, int pressed)
{
	RGBColor wht, blk, med, drk;
	short tw, tx, ty;
	wht.red = wht.green = wht.blue = 0xFFFF;
	blk.red = blk.green = blk.blue = 0;
	med.red = 0xDDDD; med.green = 0xDDDD; med.blue = 0xDDDD;
	drk.red = 0x8888; drk.green = 0x8888; drk.blue = 0x8888;

	RGBForeColor(pressed ? &drk : &med);
	PaintRoundRect(r, 6, 6);

	/* highlight top-left */
	RGBForeColor(pressed ? &blk : &wht);
	MoveTo((short)(r->left + 1), (short)(r->bottom - 2));
	LineTo((short)(r->left + 1), (short)(r->top + 1));
	LineTo((short)(r->right - 2), (short)(r->top + 1));

	/* shadow bottom-right */
	RGBForeColor(pressed ? &wht : &drk);
	MoveTo((short)(r->left + 2), (short)(r->bottom - 1));
	LineTo((short)(r->right - 1), (short)(r->bottom - 1));
	LineTo((short)(r->right - 1), (short)(r->top + 2));

	/* border */
	RGBForeColor(&blk);
	FrameRoundRect(r, 6, 6);

	/* label */
	TextFont(1); TextFace(normal); TextSize(10);
	tw = StringWidth(pstr);
	tx = (short)(r->left + (r->right - r->left - tw) / 2);
	ty = (short)(r->top + (r->bottom - r->top) / 2 + 3);
	if (pressed) { tx++; ty++; }
	MoveTo(tx, ty);
	DrawString(pstr);
}

static void dl_draw_progress_bar(const Rect *bar, unsigned long written, unsigned long total, int state)
{
	Rect inner, fill;
	RGBColor c_border, c_bg, c_hi, c_sh, c_fill_top, c_fill_bot;
	int fill_w = 0;
	int bar_w = bar->right - bar->left - 2;

	c_border.red = 0x6666; c_border.green = 0x6666; c_border.blue = 0x6666;
	c_bg.red = 0xEAEA; c_bg.green = 0xEAEA; c_bg.blue = 0xEAEA;
	c_hi.red = 0xFFFF; c_hi.green = 0xFFFF; c_hi.blue = 0xFFFF;
	c_sh.red = 0x9999; c_sh.green = 0x9999; c_sh.blue = 0x9999;

	/* Outer border */
	RGBForeColor(&c_border);
	FrameRect(bar);

	/* Inner sunken bevel */
	inner = *bar;
	InsetRect(&inner, 1, 1);
	RGBForeColor(&c_bg);
	PaintRect(&inner);

	RGBForeColor(&c_sh);
	MoveTo(inner.left, (short)(inner.bottom - 1));
	LineTo(inner.left, inner.top);
	LineTo((short)(inner.right - 1), inner.top);

	RGBForeColor(&c_hi);
	MoveTo((short)(inner.left + 1), (short)(inner.bottom - 1));
	LineTo((short)(inner.right - 1), (short)(inner.bottom - 1));
	LineTo((short)(inner.right - 1), (short)(inner.top + 1));

	if (state == 1) {
		/* Completed: full bar in forest green */
		fill_w = bar_w;
		c_fill_top.red = 0x5555; c_fill_top.green = 0xAAAA; c_fill_top.blue = 0x5555;
		c_fill_bot.red = 0x3333; c_fill_bot.green = 0x8888; c_fill_bot.blue = 0x3333;
	} else if (state == 2) {
		/* Stopped / Failed: muted gray fill */
		if (total > 0 && bar_w > 0)
			fill_w = (int)((unsigned long long)bar_w * written / total);
		if (fill_w > bar_w) fill_w = bar_w;
		c_fill_top.red = 0xAAAA; c_fill_top.green = 0xAAAA; c_fill_top.blue = 0xAAAA;
		c_fill_bot.red = 0x8888; c_fill_bot.green = 0x8888; c_fill_bot.blue = 0x8888;
	} else {
		/* Active */
		if (total > 0 && bar_w > 0) {
			fill_w = (int)((unsigned long long)bar_w * written / total);
			if (fill_w > bar_w) fill_w = bar_w;
		} else {
			/* Indeterminate moving block */
			int block_w = 36;
			int offset = (int)((TickCount() * 2) % (unsigned long)(bar_w + block_w)) - block_w;
			int left_x = inner.left + 1 + offset;
			int right_x = left_x + block_w;
			if (left_x < inner.left + 1) left_x = inner.left + 1;
			if (right_x > inner.right - 1) right_x = inner.right - 1;
			if (right_x > left_x) {
				SetRect(&fill, (short)left_x, (short)(inner.top + 1),
					(short)right_x, (short)(inner.bottom - 1));
				c_fill_top.red = 0x6666; c_fill_top.green = 0x9999; c_fill_top.blue = 0xDDDD;
				c_fill_bot.red = 0x3333; c_fill_bot.green = 0x6666; c_fill_bot.blue = 0xBBBB;
				RGBForeColor(&c_fill_top);
				PaintRect(&fill);
				return;
			}
			return;
		}
		c_fill_top.red = 0x5555; c_fill_top.green = 0x8888; c_fill_top.blue = 0xDDDD;
		c_fill_bot.red = 0x2222; c_fill_bot.green = 0x5555; c_fill_bot.blue = 0xAAAA;
	}

	if (fill_w > 0) {
		short mid_y;
		Rect ftop, fbot;
		SetRect(&fill, (short)(inner.left + 1), (short)(inner.top + 1),
			(short)(inner.left + 1 + fill_w), (short)(inner.bottom - 1));
		mid_y = (short)(fill.top + (fill.bottom - fill.top) / 2);
		SetRect(&ftop, fill.left, fill.top, fill.right, mid_y);
		SetRect(&fbot, fill.left, mid_y, fill.right, fill.bottom);
		RGBForeColor(&c_fill_top);
		PaintRect(&ftop);
		RGBForeColor(&c_fill_bot);
		PaintRect(&fbot);
	}
}

static void dl_mgr_ensure(void)
{
	Rect b, r_open_folder, r_clear, r_sb;
	Str255 title;
	const char *t = "Downloads";
	size_t n;
	if (g_dl_mgr_win != NULL) return;
	SetRect(&b, 100, 120, 640, 460);  /* 540 x 340 px */
	if (CreateNewWindow(kDocumentWindowClass,
			kWindowCloseBoxAttribute | kWindowCollapseBoxAttribute,
			&b, &g_dl_mgr_win) != noErr || g_dl_mgr_win == NULL) {
		g_dl_mgr_win = NULL;
		return;
	}
	n = strlen(t);
	title[0] = (unsigned char)n;
	memcpy(title + 1, t, n);
	SetWTitle(g_dl_mgr_win, title);

	/* Top toolbar native push buttons */
	SetRect(&r_open_folder, 12, 40, 184, 64);
	g_btn_open_folder = NewControl(g_dl_mgr_win, &r_open_folder,
		"\pOpen Downloads Folder", true, 0, 0, 1, kControlPushButtonProc, 0);

	SetRect(&r_clear, 192, 40, 302, 64);
	g_btn_clear = NewControl(g_dl_mgr_win, &r_clear,
		"\pClear Finished", true, 0, 0, 1, kControlPushButtonProc, 0);

	/* Proportional scrollbar (non-live: kControlScrollBarLiveProc crashes on G3) */
	SetRect(&r_sb, 510, 72, 528, 308);
	g_dl_sb = NewControl(g_dl_mgr_win, &r_sb,
		"\p", true, 0, 0, 0, kControlScrollBarProc, 0);
}

/* Paint the manager window into the current port */
static void dl_mgr_paint(void)
{
	Rect pr, list, list_fr;
	struct gui_download_window *nd;
	int shown = 0, cur = 0;
	int n_active = 0, n_done = 0;
	RGBColor blk, wht, sep, row_alt;
	blk.red = blk.green = blk.blue = 0;
	wht.red = wht.green = wht.blue = 0xFFFF;
	sep.red = sep.green = sep.blue = 0xDDDD;
	row_alt.red = row_alt.green = row_alt.blue = 0xF7F7;

	if (g_dl_mgr_win == NULL) return;
	GetWindowPortBounds(g_dl_mgr_win, &pr);
	EraseRect(&pr);

	/* Gold header banner */
	macos9_chrome_mgr_header(&pr, "Downloads", 0);

	/* Scrollbar update */
	if (g_dl_sb != NULL) {
		int maxtop = g_dl_count - 5;
		if (maxtop < 0) maxtop = 0;
		SetControlMaximum(g_dl_sb, (short)maxtop);
		SetControlValue(g_dl_sb, (short)g_dl_scroll_top);
	}

	/* Draw native top controls */
	DrawControls(g_dl_mgr_win);

	/* List Container */
	SetRect(&list, 12, 72, 510, 308);
	list_fr = list;
	InsetRect(&list_fr, -1, -1);
	RGBForeColor(&blk);
	FrameRect(&list_fr);

	if (g_dl_list == NULL || g_dl_count == 0) {
		RGBColor bg;
		bg.red = bg.green = bg.blue = 0xFAFA;
		RGBForeColor(&bg);
		PaintRect(&list);
		RGBForeColor(&blk);
		TextFont(1); TextFace(bold); TextSize(13);
		MoveTo((short)(list.left + 155), (short)(list.top + 95));
		DrawString("\pNo active downloads");
		TextFace(normal); TextSize(11);
		MoveTo((short)(list.left + 95), (short)(list.top + 120));
		DrawString("\pFiles downloaded from web pages will be saved to your");
		MoveTo((short)(list.left + 130), (short)(list.top + 138));
		DrawString("\p\"MacSurf Downloads\" folder and shown here.");
	} else {
		RgnHandle saveclip = NewRgn();
		GetClip(saveclip);
		ClipRect(&list);

		/* Count active and done */
		for (nd = g_dl_list; nd != NULL; nd = nd->dl_next) {
			if (nd->dl_state == 0) n_active++;
			else if (nd->dl_state == 1) n_done++;
		}

		/* Skip to scroll position */
		for (nd = g_dl_list; nd != NULL && cur < g_dl_scroll_top; nd = nd->dl_next)
			cur++;

		for (; nd != NULL && shown < 5; nd = nd->dl_next, shown++) {
			short y = (short)(list.top + shown * 46);
			Rect r_row, r_dot, r_bar;
			Rect col_fn, col_det;
			RGBColor dot_col;
			char b_cur[32], b_tot[32], r_str[32], eta_str[32], det[160];

			/* Row background */
			SetRect(&r_row, list.left, y, list.right, (short)(y + 46));
			RGBForeColor(shown % 2 == 0 ? &wht : &row_alt);
			PaintRect(&r_row);

			/* Row separator */
			RGBForeColor(&sep);
			MoveTo(list.left, (short)(y + 45));
			LineTo(list.right, (short)(y + 45));

			/* Status dot */
			SetRect(&r_dot, (short)(list.left + 8), (short)(y + 8),
				(short)(list.left + 16), (short)(y + 16));
			if (nd->dl_state == 0) {
				dot_col.red = 0x2020; dot_col.green = 0x7070; dot_col.blue = 0xEEEE;
			} else if (nd->dl_state == 1) {
				dot_col.red = 0x3030; dot_col.green = 0xCCCC; dot_col.blue = 0x3030;
			} else {
				dot_col.red = 0xDDDD; dot_col.green = 0x3030; dot_col.blue = 0x3030;
			}
			RGBForeColor(&dot_col);
			PaintOval(&r_dot);

			/* Filename */
			TextFont(1); TextFace(bold); TextSize(12);
			RGBForeColor(&blk);
			SetRect(&col_fn, (short)(list.left + 22), (short)y,
				(short)(list.right - 145), (short)(y + 20));
			ClipRect(&col_fn);
			MoveTo((short)(list.left + 22), (short)(y + 16));
			DrawText(nd->filename, 0, (short)strlen(nd->filename));
			ClipRect(&list);

			/* Action buttons on the right */
			if (nd->dl_state == 0) {
				Rect r_cancel;
				SetRect(&r_cancel, (short)(list.right - 68), (short)(y + 11),
					(short)(list.right - 8), (short)(y + 35));
				dl_draw_btn(&r_cancel, "\pCancel", 0);
			} else if (nd->dl_state == 1) {
				Rect r_reveal, r_open;
				SetRect(&r_reveal, (short)(list.right - 134), (short)(y + 11),
					(short)(list.right - 72), (short)(y + 35));
				SetRect(&r_open, (short)(list.right - 68), (short)(y + 11),
					(short)(list.right - 8), (short)(y + 35));
				dl_draw_btn(&r_reveal, "\pReveal", 0);
				dl_draw_btn(&r_open, "\pOpen", 0);
			} else {
				RGBColor redc;
				redc.red = 0xCCCC; redc.green = 0x3333; redc.blue = 0x3333;
				RGBForeColor(&redc);
				TextFont(1); TextFace(italic); TextSize(11);
				MoveTo((short)(list.right - 62), (short)(y + 24));
				DrawString("\pStopped");
				TextFace(normal);
			}

			/* Progress Bar */
			SetRect(&r_bar, (short)(list.left + 22), (short)(y + 23),
				(short)(list.left + 182), (short)(y + 36));
			dl_draw_progress_bar(&r_bar, nd->bytes_written, nd->total_length, nd->dl_state);

			/* Progress details text */
			dl_format_bytes(nd->bytes_written, b_cur);
			if (nd->dl_state == 1) {
				sprintf(det, "%s - Completed", b_cur);
			} else if (nd->dl_state == 2) {
				sprintf(det, "%s - Stopped", b_cur);
			} else {
				dl_format_rate(nd->last_rate_bps, r_str);
				if (nd->total_length > 0) {
					dl_format_bytes(nd->total_length, b_tot);
					dl_format_eta(nd, eta_str);
					sprintf(det, "%s of %s (%s, %s)", b_cur, b_tot, r_str, eta_str);
				} else {
					sprintf(det, "%s (%s)", b_cur, r_str);
				}
			}

			RGBForeColor(&blk);
			TextFont(1); TextFace(normal); TextSize(10);
			SetRect(&col_det, (short)(list.left + 190), (short)(y + 22),
				(short)(list.right - 142), (short)(y + 38));
			ClipRect(&col_det);
			MoveTo((short)(list.left + 190), (short)(y + 33));
			DrawText(det, 0, (short)strlen(det));
			ClipRect(&list);
		}

		SetClip(saveclip);
		DisposeRgn(saveclip);
	}

	/* Footer status line */
	RGBForeColor(&blk);
	TextFont(1); TextFace(normal); TextSize(10);
	MoveTo(14, 326);
	DrawString("\pFolder: MacSurf Downloads");

	if (g_dl_count > 0) {
		char sum_str[64];
		Str255 psum;
		sprintf(sum_str, "%d active, %d finished", n_active, n_done);
		c_to_pstring(sum_str, psum);
		MoveTo((short)(list.right - StringWidth(psum)), 326);
		DrawString(psum);
	}
}

/* Cancel an active download: stop the fetch, drop the partial file, mark
 * the node Stopped. Safe to call only on an active (dl_state==0) node. */
static void dl_cancel(struct gui_download_window *dw)
{
	if (dw == NULL || dw->dl_state != 0) return;
	dw->aborted = 1;
	dw->dl_state = 2;
	if (dw->refnum >= 0) { FSClose(dw->refnum); dw->refnum = -1; }
	(void)FSpDelete(&dw->fsspec);
	if (dw->dl_ctx != NULL) {
		download_context_abort(dw->dl_ctx);
		dw->dl_ctx = NULL;
	}
	macsurf_debug_log_writef("download CANCEL file=%s at=%ld",
		dw->filename, (long)dw->bytes_written);
	dl_mgr_progress();
}

static void dl_mgr_show(void)
{
	dl_mgr_ensure();
	if (g_dl_mgr_win == NULL) return;
	ShowWindow(g_dl_mgr_win);
	SelectWindow(g_dl_mgr_win);
}

/* fixes883 - public entry point for the View > Downloads menu item.
 *
 * The download manager is a real Carbon window (live byte counts, per-row
 * Cancel, completed-row eviction), but dl_mgr_show was static and its only
 * caller was the start-a-download path -- so the window was reachable ONLY by
 * starting a download, and there was no way back to it once closed. The
 * plumbing was finished; only discoverability was missing. */
void macos9_download_mgr_show(void)
{
	dl_mgr_show();
}

void macos9_download_mgr_hide(void)
{
	if (g_dl_mgr_win != NULL && IsWindowVisible(g_dl_mgr_win))
		HideWindow(g_dl_mgr_win);
}

/* Direct (non-update) repaint used for live progress. Clipped to the
 * window's visRgn by QuickDraw, so it is safe outside BeginUpdate. */
static void dl_mgr_progress(void)
{
	GrafPtr saved;
	if (g_dl_mgr_win == NULL) return;
	if (!IsWindowVisible(g_dl_mgr_win)) return;
	GetPort(&saved);
	SetPortWindowPort(g_dl_mgr_win);
	dl_mgr_paint();
	SetPort(saved);
}

/* Evict the oldest COMPLETED node when the list is full. Never evicts an
 * active download. */
static void dl_list_evict(void)
{
	struct gui_download_window *prev = NULL;
	struct gui_download_window *nd = g_dl_list;
	struct gui_download_window *victim = NULL;
	struct gui_download_window *victim_prev = NULL;
	if (g_dl_count < MACSURF_DL_MAX) return;
	while (nd != NULL) {
		if (nd->dl_state != 0) { victim = nd; victim_prev = prev; }
		prev = nd;
		nd = nd->dl_next;
	}
	if (victim == NULL) return;      /* all active - keep them */
	if (victim_prev == NULL)
		g_dl_list = victim->dl_next;
	else
		victim_prev->dl_next = victim->dl_next;
	free(victim);
	g_dl_count--;
}
#endif /* __MACOS9__ */

/* ── main-loop entry points (declared in macos9.h) ─────────────────── */

long macos9_download_mgr_is(WindowRef w)
{
#ifdef __MACOS9__
	return (g_dl_mgr_win != NULL && w == g_dl_mgr_win) ? 1L : 0L;
#else
	(void)w;
	return 0L;
#endif
}

void macos9_download_mgr_draw(void)
{
#ifdef __MACOS9__
	GrafPtr saved;
	if (g_dl_mgr_win == NULL) return;
	GetPort(&saved);
	SetPortWindowPort(g_dl_mgr_win);
	BeginUpdate(g_dl_mgr_win);
	dl_mgr_paint();
	EndUpdate(g_dl_mgr_win);
	SetPort(saved);
#endif
}

void macos9_download_mgr_click(short part, Point where)
{
#ifdef __MACOS9__
	if (g_dl_mgr_win == NULL) return;
	if (part == inDrag) {
		BitMap sbmp;
		Rect db;
		GetQDGlobalsScreenBits(&sbmp);
		db = sbmp.bounds;
		DragWindow(g_dl_mgr_win, where, &db);
	} else if (part == inGoAway) {
		if (TrackGoAway(g_dl_mgr_win, where))
			HideWindow(g_dl_mgr_win);
	} else if (part == inContent) {
		GrafPtr saved;
		Point p = where;
		ControlRef hit_ctrl = NULL;
		short cpart;
		Rect list;

		if (g_dl_mgr_win != FrontWindow()) {
			SelectWindow(g_dl_mgr_win);
			return;
		}
		GetPort(&saved);
		SetPortWindowPort(g_dl_mgr_win);
		GlobalToLocal(&p);

		/* 1. Check top controls & scrollbar */
		cpart = FindControl(p, g_dl_mgr_win, &hit_ctrl);
		if (cpart != 0 && hit_ctrl != NULL) {
			if (hit_ctrl == g_dl_sb) {
				short code = TrackControl(g_dl_sb, p, NULL);
				if (code != 0) {
					g_dl_scroll_top = GetControlValue(g_dl_sb);
					dl_mgr_paint();
				}
			} else if (TrackControl(hit_ctrl, p, NULL) != 0) {
				if (hit_ctrl == g_btn_open_folder) {
					dl_mgr_open_folder();
				} else if (hit_ctrl == g_btn_clear) {
					dl_mgr_clear_finished();
				}
			}
			SetPort(saved);
			return;
		}

		/* 2. Check list row action clicks */
		SetRect(&list, 12, 72, 510, 308);
		if (p.h >= list.left && p.h <= list.right &&
		    p.v >= list.top && p.v <= list.bottom) {
			int row_idx = (p.v - list.top) / 46;
			int target_idx = g_dl_scroll_top + row_idx;
			struct gui_download_window *nd = g_dl_list;
			int cur = 0;
			while (nd != NULL && cur < target_idx) {
				nd = nd->dl_next;
				cur++;
			}
			if (nd != NULL) {
				short y = (short)(list.top + row_idx * 46);
				if (nd->dl_state == 0) {
					Rect r_cancel;
					SetRect(&r_cancel, (short)(list.right - 68), (short)(y + 11),
						(short)(list.right - 8), (short)(y + 35));
					if (PtInRect(p, &r_cancel)) {
						dl_draw_btn(&r_cancel, "\pCancel", 1);
						dl_cancel(nd);
					}
				} else if (nd->dl_state == 1) {
					Rect r_reveal, r_open;
					SetRect(&r_reveal, (short)(list.right - 134), (short)(y + 11),
						(short)(list.right - 72), (short)(y + 35));
					SetRect(&r_open, (short)(list.right - 68), (short)(y + 11),
						(short)(list.right - 8), (short)(y + 35));
					if (PtInRect(p, &r_reveal)) {
						dl_draw_btn(&r_reveal, "\pReveal", 1);
						dl_mgr_open_folder();
						dl_mgr_paint();
					} else if (PtInRect(p, &r_open)) {
						dl_draw_btn(&r_open, "\pOpen", 1);
						dl_open_spec(&nd->fsspec);
						dl_mgr_paint();
					} else {
						/* Double click anywhere on row opens the file */
						unsigned long now = TickCount();
						if (g_last_click_row == target_idx && (now - g_last_click_time) <= GetDblTime()) {
							dl_open_spec(&nd->fsspec);
						}
						g_last_click_row = target_idx;
						g_last_click_time = now;
					}
				}
			}
		}
		SetPort(saved);
	}
#else
	(void)part; (void)where;
#endif
}

/* ── gui_download_table callbacks ──────────────────────────────────── */

static struct gui_download_window *
macos9_download_create(struct download_context *ctx,
		       struct gui_window *parent)
{
	struct gui_download_window *dw;
#ifdef __MACOS9__
	short           vRef;
	long            dirID;
	FSSpec          spec;
	const char     *suggested;
	const char     *mime;
	OSType          type, creator;
	OSErr           err;
	unsigned long long total_ll;
#endif

	dw = (struct gui_download_window *)calloc(1, sizeof(*dw));
	if (dw == NULL) return NULL;

	dw->parent = parent;
	dw->refnum = -1;
	dw->bytes_written = 0;
	dw->total_length = 0;
	dw->filename[0] = '\0';
	dw->aborted = 0;
	dw->dl_state = 0;
	dw->dl_next = NULL;
	dw->dl_ctx = ctx;   /* fixes646: kept so Cancel can abort the fetch */
	dw->start_ticks = 0;
	dw->last_rate_ticks = 0;
	dw->last_rate_bytes = 0;
	dw->last_rate_bps = 0;

#ifdef __MACOS9__
	dw->start_ticks = TickCount();
	dw->last_rate_ticks = dw->start_ticks;
	suggested = download_context_get_filename(ctx);
	mime = download_context_get_mime_type(ctx);
	total_ll = download_context_get_total_length(ctx);
	/* fixes767 (#133) - a navigation became a DOWNLOAD instead of rendering.
	 * Log the MIME (RECON survives the failures-only gate) so a "site X
	 * downloaded instead of showing" report reveals the exact content-type
	 * that had no handler. text/plain now renders inline via the real
	 * textplain handler (#233, fixes1137); reaching here with a text/plain
	 * mime means Content-Disposition: attachment forced it. */
	macsurf_debug_log_writef("RECON DL mime=%s file=%s",
		mime ? mime : "(null)", suggested ? suggested : "(null)");
	if (total_ll > 0xFFFFFFFFu)
		dw->total_length = 0xFFFFFFFFu;
	else
		dw->total_length = (unsigned long)total_ll;

	dl_sanitize_name(suggested, dw->filename, sizeof dw->filename);
	macos9_download_mime_to_type(mime, &type, &creator);

	if (macos9_downloads_dir_get(&vRef, &dirID) != noErr) {
		macsurf_debug_log_writef(
			"download_create: no Downloads folder");
		free(dw);
		return NULL;
	}

	err = dl_unique_spec(vRef, dirID, dw->filename,
			sizeof dw->filename, &spec);
	if (err != noErr) {
		macsurf_debug_log_writef(
			"download_create: unique-spec err=%d", (int)err);
		free(dw);
		return NULL;
	}
	dw->fsspec = spec;

	err = FSpCreate(&spec, creator, type, smSystemScript);
	if (err != noErr && err != dupFNErr) {
		macsurf_debug_log_writef(
			"download_create: FSpCreate err=%d", (int)err);
		free(dw);
		return NULL;
	}

	err = FSpOpenDF(&spec, fsRdWrPerm, &dw->refnum);
	if (err != noErr) {
		macsurf_debug_log_writef(
			"download_create: FSpOpenDF err=%d", (int)err);
		(void)FSpDelete(&spec);
		free(dw);
		return NULL;
	}

	{
		char status[128];
		sprintf(status, "Downloading %s...", dw->filename);
		macos9_download_status(dw, status);
	}
	macsurf_debug_log_writef("download START file=%s total=%ld",
		dw->filename, (long)dw->total_length);

	dl_list_evict();
	dw->dl_next = g_dl_list;
	g_dl_list = dw;
	g_dl_count++;
	dl_mgr_show();
	dl_mgr_progress();
#else
	(void)ctx;
#endif

	return dw;
}

static nserror
macos9_download_data(struct gui_download_window *dw,
		     const char *data,
		     unsigned int size)
{
#ifdef __MACOS9__
	long count;
	OSErr err;
	unsigned long now_ticks;
#endif

	if (dw == NULL) return NSERROR_OK;
	if (dw->aborted) return NSERROR_OK;

#ifdef __MACOS9__
	if (dw->refnum < 0 || data == NULL || size == 0) return NSERROR_OK;

	count = (long)size;
	err = FSWrite(dw->refnum, &count, data);
	if (err != noErr || count != (long)size) {
		macsurf_debug_log_writef(
			"download_data: FSWrite err=%d wrote=%ld of=%ld",
			(int)err, (long)count, (long)size);
		dw->aborted = 1;
		dw->dl_state = 2;
		dl_mgr_progress();
		return NSERROR_SAVE_FAILED;
	}
	dw->bytes_written += (unsigned long)size;

	/* Throttle UI updates to ~every 16 KB. */
	if ((dw->bytes_written & 0x3FFFul) < (unsigned long)size) {
		char status[128];
		now_ticks = TickCount();
		if (now_ticks >= dw->last_rate_ticks + 30) {
			unsigned long dt = now_ticks - dw->last_rate_ticks;
			unsigned long db = dw->bytes_written - dw->last_rate_bytes;
			if (dt > 0)
				dw->last_rate_bps = (db * 60) / dt;
			dw->last_rate_ticks = now_ticks;
			dw->last_rate_bytes = dw->bytes_written;
		}
		if (dw->total_length > 0) {
			sprintf(status,
				"Downloading %s: %lu of %lu bytes",
				dw->filename,
				dw->bytes_written, dw->total_length);
		} else {
			sprintf(status,
				"Downloading %s: %lu bytes",
				dw->filename, dw->bytes_written);
		}
		macos9_download_status(dw, status);
		dl_mgr_progress();
	}
#else
	(void)data; (void)size;
#endif
	return NSERROR_OK;
}

static void
macos9_download_error(struct gui_download_window *dw,
		      const char *error_msg)
{
#ifdef __MACOS9__
	char status[160];
	const char *m = (error_msg != NULL) ? error_msg : "download failed";
#endif

	if (dw == NULL) return;
	dw->aborted = 1;
	if (dw->dl_state == 0) dw->dl_state = 2;   /* keep 2 if already stopped */
	dw->dl_ctx = NULL;                          /* core destroys ctx now */

#ifdef __MACOS9__
	if (dw->refnum >= 0) {
		FSClose(dw->refnum);
		dw->refnum = -1;
	}
	(void)FSpDelete(&dw->fsspec);

	sprintf(status, "Download failed: %s", m);
	macos9_download_status(dw, status);
	macsurf_debug_log_writef(
		"download_error: file=%s msg=%s", dw->filename, m);
	dl_mgr_progress();
#else
	(void)error_msg;
#endif
	/* Node stays in the manager list (Failed); freed by the LRU cap. */
}

static void
macos9_download_done(struct gui_download_window *dw)
{
#ifdef __MACOS9__
	char status[160];
#endif

	if (dw == NULL) return;
	dw->dl_ctx = NULL;                          /* core destroys ctx now */
	if (dw->dl_state != 0) {                    /* was cancelled/failed */
#ifdef __MACOS9__
		dl_mgr_progress();
#endif
		return;
	}
	dw->dl_state = 1;

#ifdef __MACOS9__
	if (dw->refnum >= 0) {
		FSClose(dw->refnum);
		dw->refnum = -1;
	}
	sprintf(status, "Saved %s (%lu bytes)",
		dw->filename, dw->bytes_written);
	macos9_download_status(dw, status);
	macsurf_debug_log_writef("download DONE file=%s wrote=%ld total=%ld",
		dw->filename, (long)dw->bytes_written,
		(long)dw->total_length);
	dl_mgr_progress();
#endif
	/* Node stays in the manager list (Done); freed by the LRU cap. */
}

/* Field order: create, data, error, done (see include/netsurf/download.h) */
static struct gui_download_table download_table = {
	macos9_download_create,
	macos9_download_data,
	macos9_download_error,
	macos9_download_done
};

struct gui_download_table *macos9_download_table = &download_table;
#endif /* __MACOS9__ */
