/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * clipboard.c — All gui_clipboard_table callbacks
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "netsurf/clipboard.h"

#include "macos9.h"

static void
macos9_clipboard_get(char **buffer, size_t *length)
{
	/* TODO: GetScrap() with 'TEXT' type, convert to UTF-8 */
	*buffer = NULL;
	*length = 0;
}

static void
macos9_clipboard_set(const char *buffer, size_t length,
		     nsclipboard_styles styles[], int n_styles)
{
	/* TODO: ZeroScrap() + PutScrap() with 'TEXT' type */
}

/* Field order: get, set (see include/netsurf/clipboard.h) */
static struct gui_clipboard_table clipboard_table = {
	macos9_clipboard_get,
	macos9_clipboard_set
};

struct gui_clipboard_table *macos9_clipboard_table = &clipboard_table;
