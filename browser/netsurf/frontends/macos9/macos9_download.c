/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * macos9_download.c — gui_download_table callbacks
 *
 * fixes313 — V1 download manager.
 *
 * NetSurf core detects non-renderable responses (Content-Disposition:
 * attachment, unknown MIME, save-link-as gesture) and routes them
 * through browser_window_download → gui_download_table.create. Pre-
 * fix313 all four callbacks were TODOs: the create stub allocated a
 * struct but never opened a file, data dropped every chunk on the
 * floor, error/done did nothing. Net result: clicking any download
 * link looked like a no-op.
 *
 * V1 wires the four callbacks to the Mac Toolbox:
 *
 *   create → StandardPutFile (modal save dialog) with the suggested
 *            filename from NetSurf as default → FSpCreate (binary file,
 *            'BINA'/'????' if MIME doesn't map to a registered type) →
 *            FSpOpenDF for write. Stash the FSSpec + refnum + total
 *            length in dw. If the user cancels, refnum stays -1 and
 *            data callbacks silently drop bytes.
 *   data   → FSWrite each chunk; update bytes_written; report progress
 *            via the parent gw's status bar.
 *   done   → FSClose, status bar success message, free dw.
 *   error  → FSClose, FSpDelete (partial file cleanup), error alert,
 *            free dw.
 *
 * V1 caps to ONE active download at a time. A second download attempt
 * while the slot is occupied returns NULL from create — NetSurf surfaces
 * that as a fetch failure rather than silently overwriting. Multi-slot
 * is a V2 follow-up once the single-download path is stable.
 *
 * Progress UI is the parent window's status bar (already wired). A
 * dedicated modeless progress window is deferred to V2.
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "netsurf/download.h"
#include "desktop/download.h"

#include "macos9.h"
#include "macsurf_debug.h"

#ifdef __MACOS9__
#include <Files.h>
#include <Script.h>
#include <Navigation.h>
#include <AppleEvents.h>
#endif

/* Active-download slot. NULL = idle. V1 only allows one at a time. */
static struct gui_download_window *g_active_dw = NULL;

#ifdef __MACOS9__
/* Map a NetSurf MIME string to a Mac type/creator pair. Best-effort —
 * anything not recognised falls back to 'BINA' / '????' which gives a
 * generic binary-file icon and no default-open behaviour. The mapping
 * is short on purpose; expanding it doesn't affect correctness, only
 * which app Finder offers as the default. */
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

/* Convert a C string to a Pascal string, truncating to 63 chars + len
 * byte so it fits in Str63 (StandardPutFile's default-name buffer). */
static void
macos9_download_cstr_to_p63(const char *src, Str63 dst)
{
	size_t n;
	if (src == NULL) src = "untitled";
	n = strlen(src);
	if (n > 63) n = 63;
	dst[0] = (unsigned char)n;
	memcpy(dst + 1, src, n);
}

/* Push a one-line status message to the parent window's status bar. The
 * status function on macos9_window_table tolerates a NULL gw. */
static void
macos9_download_status(struct gui_download_window *dw, const char *msg)
{
	extern struct gui_window_table *macos9_window_table;
	if (dw == NULL || dw->parent == NULL || msg == NULL) return;
	if (macos9_window_table && macos9_window_table->set_status) {
		macos9_window_table->set_status(dw->parent, msg);
	}
}
#endif

static struct gui_download_window *
macos9_download_create(struct download_context *ctx,
		       struct gui_window *parent)
{
	struct gui_download_window *dw;
#ifdef __MACOS9__
	NavReplyRecord  reply;
	NavDialogOptions options;
	AEKeyword       keyword;
	DescType        actual_type;
	Size            actual_size;
	Str63           default_name;
	const char     *suggested;
	const char     *mime;
	OSType          type, creator;
	OSErr           err;
	unsigned long long total_ll;
#endif

	if (g_active_dw != NULL) {
		/* fixes313 V1 — one download at a time. */
		macsurf_debug_log_writef(
			"download_create: slot busy (current=%s)",
			g_active_dw->filename);
		return NULL;
	}

	dw = (struct gui_download_window *)calloc(1, sizeof(*dw));
	if (dw == NULL) return NULL;

	dw->parent = parent;
	dw->refnum = -1;
	dw->bytes_written = 0;
	dw->total_length = 0;
	dw->filename[0] = '\0';
	dw->aborted = 0;

#ifdef __MACOS9__
	suggested = download_context_get_filename(ctx);
	mime = download_context_get_mime_type(ctx);
	total_ll = download_context_get_total_length(ctx);
	if (total_ll > 0xFFFFFFFFu) {
		/* Truncate the displayed total to 32-bit; the actual file
		 * write keeps going as long as data flows. Rare case for
		 * >4 GB downloads on a Mac with limited free space anyway. */
		dw->total_length = 0xFFFFFFFFu;
	} else {
		dw->total_length = (unsigned long)total_ll;
	}

	if (suggested != NULL) {
		size_t n = strlen(suggested);
		if (n >= sizeof(dw->filename)) n = sizeof(dw->filename) - 1;
		memcpy(dw->filename, suggested, n);
		dw->filename[n] = '\0';
	} else {
		strcpy(dw->filename, "untitled");
	}

	macos9_download_cstr_to_p63(dw->filename, default_name);

	macos9_download_mime_to_type(mime, &type, &creator);

	/* NavPutFile is the Carbon save-dialog API; CarbonLib already
	 * linked. NavGetDefaultDialogOptions fills the options struct
	 * with the system defaults (including the modern Save panel
	 * appearance), then we overwrite savedFileName with our suggested
	 * filename so the dialog opens with it pre-filled. */
	err = NavGetDefaultDialogOptions(&options);
	if (err != noErr) {
		macsurf_debug_log_writef(
			"download_create: NavGetDefaultDialogOptions err=%d",
			(int)err);
		free(dw);
		return NULL;
	}
	/* Copy our Pascal-format filename into the options. savedFileName
	 * is a Str255 (256-byte Pascal string buffer) inside NavDialogOptions. */
	{
		unsigned char n = default_name[0];
		options.savedFileName[0] = n;
		memcpy(options.savedFileName + 1, default_name + 1, n);
	}

	err = NavPutFile(NULL, &reply, &options, NULL,
			type, creator, NULL);
	if (err != noErr || !reply.validRecord) {
		macsurf_debug_log_writef(
			"download_create: NavPutFile err=%d valid=%d",
			(int)err, (int)reply.validRecord);
		if (err == noErr) NavDisposeReply(&reply);
		free(dw);
		return NULL;
	}

	/* Extract the chosen FSSpec from the reply's selection AEDescList. */
	err = AEGetNthPtr(&reply.selection, 1, typeFSS,
			&keyword, &actual_type,
			&dw->fsspec, sizeof(dw->fsspec), &actual_size);
	NavDisposeReply(&reply);
	if (err != noErr) {
		macsurf_debug_log_writef(
			"download_create: AEGetNthPtr err=%d", (int)err);
		free(dw);
		return NULL;
	}

	/* Delete any existing file at this path so FSpCreate succeeds (the
	 * dialog prompted the user to replace; saying yes returns the
	 * existing FSSpec but FSpCreate refuses to overwrite). */
	(void)FSpDelete(&dw->fsspec);

	err = FSpCreate(&dw->fsspec, creator, type, smSystemScript);
	if (err != noErr) {
		macsurf_debug_log_writef(
			"download_create: FSpCreate err=%d", (int)err);
		free(dw);
		return NULL;
	}

	err = FSpOpenDF(&dw->fsspec, fsRdWrPerm, &dw->refnum);
	if (err != noErr) {
		macsurf_debug_log_writef(
			"download_create: FSpOpenDF err=%d", (int)err);
		(void)FSpDelete(&dw->fsspec);
		free(dw);
		return NULL;
	}

	{
		char status[128];
		sprintf(status, "Downloading %s...", dw->filename);
		macos9_download_status(dw, status);
	}
#else
	(void)ctx;
#endif

	g_active_dw = dw;
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
		return NSERROR_SAVE_FAILED;
	}
	dw->bytes_written += (unsigned long)size;

	/* Throttle status updates: ~every 16 KB so we don't repaint the
	 * status bar for every TCP segment. */
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
#else
	(void)error_msg;
#endif

	if (g_active_dw == dw) g_active_dw = NULL;
	free(dw);
}

static void
macos9_download_done(struct gui_download_window *dw)
{
#ifdef __MACOS9__
	char status[160];
#endif

	if (dw == NULL) return;

#ifdef __MACOS9__
	if (dw->refnum >= 0) {
		FSClose(dw->refnum);
		dw->refnum = -1;
	}
	sprintf(status, "Saved %s (%lu bytes)",
		dw->filename, dw->bytes_written);
	macos9_download_status(dw, status);
	macsurf_debug_log_writef("%s", status);
#endif

	if (g_active_dw == dw) g_active_dw = NULL;
	free(dw);
}

/* Field order: create, data, error, done (see include/netsurf/download.h) */
static struct gui_download_table download_table = {
	macos9_download_create,
	macos9_download_data,
	macos9_download_error,
	macos9_download_done
};

struct gui_download_table *macos9_download_table = &download_table;
