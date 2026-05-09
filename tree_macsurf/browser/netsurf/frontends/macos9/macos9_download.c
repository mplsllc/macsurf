/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * macos9_download.c — All gui_download_table callbacks
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "netsurf/download.h"

#include "macos9/macos9.h"

static struct gui_download_window *
macos9_download_create(struct download_context *ctx,
		       struct gui_window *parent)
{
	struct gui_download_window *dw;

	/* TODO: CreateNewWindow() with progress bar */
	dw = calloc(1, sizeof(*dw));
	if (dw == NULL) {
		return NULL;
	}

	dw->parent = parent;
	return dw;
}

static nserror
macos9_download_data(struct gui_download_window *dw,
		     const char *data,
		     unsigned int size)
{
	/* TODO: FSWrite() to file, update progress */
	return NSERROR_OK;
}

static void
macos9_download_error(struct gui_download_window *dw,
		      const char *error_msg)
{
	/* TODO: StandardAlert() */
}

static void
macos9_download_done(struct gui_download_window *dw)
{
	/* TODO: close progress window */
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
