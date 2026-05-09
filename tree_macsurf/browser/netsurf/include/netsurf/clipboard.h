/*
 * Copyright 2014 Vincent Sanders <vince@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 *
 * Interface to platform-specific clipboard operations.
 */

#ifndef _NETSURF_CLIPBOARD_H_
#define _NETSURF_CLIPBOARD_H_

#include <stddef.h>

#include "utils/errors.h"
#include "netsurf/plot_style.h"

typedef struct nsnsclipboard_styles {
	size_t start;			/**< Start of run */

	plot_font_style_t style;	/**< Style to give text run */
} nsclipboard_styles;

/**
 * function table for clipboard operations.
 */
struct gui_clipboard_table {
	/**
	 * Core asks front end for clipboard contents.
	 *
	 * \param  buffer  UTF-8 text, allocated by front end, ownership yeilded to core
	 * \param  length  Byte length of UTF-8 text in buffer
	 */
	void (*get)(char **buffer, size_t *length);

	/**
	 * Core tells front end to put given text in clipboard
	 *
	 * \param  buffer    UTF-8 text, owned by core
	 * \param  length    Byte length of UTF-8 text in buffer
	 * \param  styles    Array of styles given to text runs, owned by core, or NULL
	 * \param  n_styles  Number of text run styles in array
	 */
	void (*set)(const char *buffer, size_t length, nsclipboard_styles styles[], int n_styles);
};

#endif
