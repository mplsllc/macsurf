/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * font.c — All gui_layout_table callbacks (font measurement)
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include <stdlib.h>
#include <string.h>

#include "utils/ns_errors.h"
#include "utils/log.h"
#include "utils/utf8.h"
#include "netsurf/plot_style.h"
#include "netsurf/layout.h"

#include "macos9/macos9.h"

static nserror
macos9_font_width(const struct plot_font_style *fstyle,
		  const char *string,
		  size_t length,
		  int *width)
{
	/* TODO: TextFont() + TextSize() + TextWidth() */
	/* Stub: estimate 8px per character */
	*width = (int)length * 8;
	return NSERROR_OK;
}

static nserror
macos9_font_position(const struct plot_font_style *fstyle,
		     const char *string,
		     size_t length,
		     int x,
		     size_t *char_offset,
		     int *actual_x)
{
	/* TODO: binary search with TextWidth() */
	/* Stub: assume 8px per character */
	size_t idx = (size_t)(x / 8);

	if (idx > length) {
		idx = length;
	}
	*char_offset = idx;
	*actual_x = (int)idx * 8;
	return NSERROR_OK;
}

static nserror
macos9_font_split(const struct plot_font_style *fstyle,
		  const char *string,
		  size_t length,
		  int x,
		  size_t *char_offset,
		  int *actual_x)
{
	/* TODO: TextWidth() + walk back to nearest space */
	/* Stub: find split point, walk back to space */
	size_t idx = (size_t)(x / 8);

	if (idx > length) {
		idx = length;
	}

	/* Walk back to find a space */
	while (idx > 0 && string[idx - 1] != ' ') {
		idx--;
	}

	if (idx == 0) {
		/* No space found, split at full width */
		idx = (size_t)(x / 8);
		if (idx > length) {
			idx = length;
		}
	}

	if (idx == 0 && length > 0) {
		idx = 1; /* Never return 0 */
	}

	*char_offset = idx;
	*actual_x = (int)idx * 8;
	return NSERROR_OK;
}

/* Field order: width, position, split (see include/netsurf/layout.h) */
static struct gui_layout_table layout_table = {
	macos9_font_width,
	macos9_font_position,
	macos9_font_split
};

struct gui_layout_table *macos9_layout_table = &layout_table;
