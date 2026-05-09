/*
 * Copyright 2017 Vincent Sanders <vince@netsurf-browser.org>
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
 * NetSurf types.
 *
 * These are convenience types used throughout the browser.
 */

#ifndef NETSURF_TYPES_H
#define NETSURF_TYPES_H

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/**
 * Colour type: XBGR
 */
typedef uint32_t colour;

/**
 * Rectangle coordinates
 */
typedef struct rect {
	int x0, y0; /**< Top left */
	int x1, y1; /**< Bottom right */
} rect;

#endif
