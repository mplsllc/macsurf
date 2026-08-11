/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * macos9_download.c — gui_download_table callbacks + download manager
 *
 * fixes645 — download manager V2.
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

/* Manager-window row layout (window-local coords). */
#define DL_ROW0_BASE 50   /* text baseline of row 0, below the header rule */
#define DL_ROW_H     18   /* per-row vertical step */
#define DL_CANCEL_W  56   /* width of the per-row [Cancel] box */
#define DL_ROWS_MAX  10   /* max rows drawn / hit-tested */

#ifdef __MACOS9__
static WindowRef g_dl_mgr_win = NULL;
static void dl_mgr_progress(void);   /* fwd: dl_cancel calls it */
#endif

#ifdef __MACOS9__
/* Map a NetSurf MIME string to a Mac type/creator pair. Best-effort —
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
 *     NOTE: was 63 (Str63's capacity) — wrong for a filename, and the
 *     source of BadPerimeter/paramErr(-50) reports when a long or
 *     un-truncated name slipped through.
 *   - replace ':' (the HFS path separator — illegal in a leaf), '/', and
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
		if (err == fnfErr) {              /* free slot — use it */
			dl_strlcpy(leaf, trial, leaf_cap);
			return noErr;
		}
		if (err != noErr) return err;     /* real error */
		/* else exists — try next suffix */
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

static void dl_mgr_ensure(void)
{
	Rect b;
	Str255 title;
	const char *t = "Downloads";
	size_t n;
	if (g_dl_mgr_win != NULL) return;
	SetRect(&b, 60, 250, 500, 460);
	if (CreateNewWindow(kDocumentWindowClass, kWindowCloseBoxAttribute,
			&b, &g_dl_mgr_win) != noErr) {
		g_dl_mgr_win = NULL;
		return;
	}
	n = strlen(t);
	title[0] = (unsigned char)n;
	memcpy(title + 1, t, n);
	SetWTitle(g_dl_mgr_win, title);
}

/* Paint the list into the current (already-set) port. */
static void dl_mgr_paint(void)
{
	Rect pr;
	struct gui_download_window *nd;
	int shown = 0;
	short cancel_x;
	if (g_dl_mgr_win == NULL) return;
	GetWindowPortBounds(g_dl_mgr_win, &pr);
	EraseRect(&pr);
	TextFont(3);          /* Geneva */
	TextSize(10);
	TextFace(1);          /* bold (Style bit) */
	MoveTo((short)(pr.left + 12), (short)(pr.top + 22));
	DrawString("\pDownloads");
	TextFace(0);          /* normal */
	MoveTo((short)(pr.left + 8), (short)(pr.top + 30));
	LineTo((short)(pr.right - 8), (short)(pr.top + 30));
	if (g_dl_list == NULL) {
		MoveTo((short)(pr.left + 12), (short)(pr.top + DL_ROW0_BASE));
		DrawString("\p(no downloads yet)");
		return;
	}
	cancel_x = (short)(pr.right - DL_CANCEL_W - 6);
	for (nd = g_dl_list; nd != NULL && shown < DL_ROWS_MAX;
	     nd = nd->dl_next, shown++) {
		char line[180];
		size_t ln;
		Rect tclip;
		short y = (short)(pr.top + DL_ROW0_BASE + shown * DL_ROW_H);
		if (nd->dl_state == 1) {
			sprintf(line, "%s  -  Done (%lu bytes)",
				nd->filename, nd->bytes_written);
		} else if (nd->dl_state == 2) {
			sprintf(line, "%s  -  Stopped (%lu bytes)",
				nd->filename, nd->bytes_written);
		} else if (nd->total_length > 0) {
			sprintf(line, "%s  -  %lu / %lu",
				nd->filename, nd->bytes_written,
				nd->total_length);
		} else {
			sprintf(line, "%s  -  %lu bytes",
				nd->filename, nd->bytes_written);
		}
		ln = strlen(line);
		if (ln > 179) ln = 179;
		/* Clip the row text so it can't run under the Cancel box. */
		tclip = pr;
		tclip.right = (nd->dl_state == 0) ?
			(short)(cancel_x - 6) : (short)(pr.right - 6);
		ClipRect(&tclip);
		MoveTo((short)(pr.left + 12), y);
		DrawText(line, 0, (short)ln);
		ClipRect(&pr);
		if (nd->dl_state == 0) {
			Rect cb;
			cb.left = cancel_x;
			cb.top = (short)(y - 12);
			cb.right = (short)(pr.right - 6);
			cb.bottom = (short)(y + 3);
			FrameRect(&cb);
			MoveTo((short)(cancel_x + 6), y);
			DrawString("\pCancel");
		}
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

/* fixes883 — public entry point for the View > Downloads menu item.
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
	if (victim == NULL) return;      /* all active — keep them */
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
		Rect b;
		b.left = -32000; b.top = -32000;
		b.right = 32000; b.bottom = 32000;
		DragWindow(g_dl_mgr_win, where, &b);
	} else if (part == inGoAway) {
		if (TrackGoAway(g_dl_mgr_win, where))
			HideWindow(g_dl_mgr_win);
	} else if (part == inContent) {
		GrafPtr saved;
		Point p = where;
		struct gui_download_window *nd;
		int shown = 0;
		short cancel_x;
		Rect pr;
		if (g_dl_mgr_win != FrontWindow()) {
			SelectWindow(g_dl_mgr_win);
			return;
		}
		GetPort(&saved);
		SetPortWindowPort(g_dl_mgr_win);
		GlobalToLocal(&p);
		GetWindowPortBounds(g_dl_mgr_win, &pr);
		cancel_x = (short)(pr.right - DL_CANCEL_W - 6);
		for (nd = g_dl_list; nd != NULL && shown < DL_ROWS_MAX;
		     nd = nd->dl_next, shown++) {
			short y = (short)(pr.top + DL_ROW0_BASE +
					shown * DL_ROW_H);
			if (nd->dl_state == 0 &&
			    p.v >= (short)(y - 12) && p.v <= (short)(y + 3) &&
			    p.h >= cancel_x && p.h <= (short)(pr.right - 6)) {
				dl_cancel(nd);
				break;
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

#ifdef __MACOS9__
	suggested = download_context_get_filename(ctx);
	mime = download_context_get_mime_type(ctx);
	total_ll = download_context_get_total_length(ctx);
	/* fixes767 (#133) — a navigation became a DOWNLOAD instead of rendering.
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
#endif

	if (dw == NULL) return NSERROR_OK;
	if (dw->aborted) return NSERROR_OK;

#ifdef __MACOS9__
	if (dw->refnum < 0 || data == NULL || size == 0) return NSERROR_OK;

	count = (long)size;
	err = FSWrite(dw->refnum, &count, data);
	if (err != noErr || count != (long)size) {
		macsurf_debug_log_writef(
			"download_data: FSWrite err=%d wrote=%ld of=%u",
			(int)err, (long)count, (unsigned)size);
		dw->aborted = 1;
		dw->dl_state = 2;
		dl_mgr_progress();
		return NSERROR_SAVE_FAILED;
	}
	dw->bytes_written += (unsigned long)size;

	/* Throttle UI updates to ~every 16 KB. */
	if ((dw->bytes_written & 0x3FFFul) < (unsigned long)size) {
		char status[128];
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
