/*
 * Copyright 2004 James Bursa <bursa@users.sourceforge.net>
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
 * Target independent plotting interface.
 */

#ifndef _NETSURF_PLOTTERS_H_
#define _NETSURF_PLOTTERS_H_

#include <stdbool.h>
#include <stdio.h>

#include "netsurf/plot_style.h"

struct bitmap;
struct rect;
struct plotter_table;

typedef unsigned long bitmap_flags_t;
#define BITMAPF_NONE 0
#define BITMAPF_REPEAT_X 1
#define BITMAPF_REPEAT_Y 2

enum path_command {
	PLOTTER_PATH_MOVE,
	PLOTTER_PATH_CLOSE,
	PLOTTER_PATH_LINE,
	PLOTTER_PATH_BEZIER,
};

/**
 * Redraw context
 */
struct redraw_context {
	/**
	 * Redraw to show interactive features.
	 *
	 * Active features include selections etc.
	 *
	 * \note Should be off for printing.
	 */
	bool interactive;

	/**
	 * Render background images.
	 *
	 * \note May want it off for printing.
	 */
	bool background_images;

	/**
	 * Current plot operation table
	 *
	 * \warning must be assigned before use.
	 */
	const struct plotter_table *plot;

	/**
	 * Private context.
	 *
	 * Private context allows callers to pass context through to
	 *  plot operations without using a global.
	 */
	void *priv;
};


/**
 * Plotter operations table.
 *
 * Coordinates are from top left of canvas and (0,0) is the top left grid
 * denomination. If a "fill" is drawn from (0,0) to (4,3), the result is:
 *
 *     0 1 2 3 4 5
 *    +-+-+-+-+-+-
 *  0 |#|#|#|#| |
 *    +-+-+-+-+-+-
 *  1 |#|#|#|#| |
 *    +-+-+-+-+-+-
 *  2 |#|#|#|#| |
 *    +-+-+-+-+-+-
 *  3 | | | | | |
 *
 */
struct plotter_table {
	/**
	 * \brief Sets a clip rectangle for subsequent plot operations.
	 *
	 * \param ctx The current redraw context.
	 * \param clip The rectangle to limit all subsequent plot
	 *              operations within.
	 * \return NSERROR_OK on success else error code.
	 */
	nserror (*clip)(
			const struct redraw_context *ctx,
			const struct rect *clip);

	/**
	 * Plots an arc
	 *
	 * plot an arc segment around (x,y), anticlockwise from angle1
	 *  to angle2. Angles are measured anticlockwise from
	 *  horizontal, in degrees.
	 *
	 * \param ctx The current redraw context.
	 * \param pstyle Style controlling the arc plot.
	 * \param x The x coordinate of the arc.
	 * \param y The y coordinate of the arc.
	 * \param radius The radius of the arc.
	 * \param angle1 The start angle of the arc.
	 * \param angle2 The finish angle of the arc.
	 * \return NSERROR_OK on success else error code.
	 */
	nserror (*arc)(
			const struct redraw_context *ctx,
			const plot_style_t *pstyle,
			int x,
			int y,
			int radius,
			int angle1,
			int angle2);

	/**
	 * Plots a circle
	 *
	 * Plot a circle centered on (x,y), which is optionally filled.
	 *
	 * \param ctx The current redraw context.
	 * \param pstyle Style controlling the circle plot.
	 * \param x The x coordinate of the circle.
	 * \param y The y coordinate of the circle.
	 * \param radius The radius of the circle.
	 * \return NSERROR_OK on success else error code.
	 */
	nserror (*disc)(
			const struct redraw_context *ctx,
			const plot_style_t *pstyle,
			int x,
			int y,
			int radius);

	/**
	 * Plots a line
	 *
	 * plot a line from (x0,y0) to (x1,y1). Coordinates are at
	 *  centre of line width/thickness.
	 *
	 * \param ctx The current redraw context.
	 * \param pstyle Style controlling the line plot.
	 * \param line A rectangle defining the line to be drawn
	 * \return NSERROR_OK on success else error code.
	 */
	nserror (*line)(
			const struct redraw_context *ctx,
			const plot_style_t *pstyle,
			const struct rect *line);

	/**
	 * Plots a rectangle.
	 *
	 * The rectangle can be filled an outline or both controlled
	 *  by the plot style The line can be solid, dotted or
	 *  dashed. Top left corner at (x0,y0) and rectangle has given
	 *  width and height.
	 *
	 * \param ctx The current redraw context.
	 * \param pstyle Style controlling the rectangle plot.
	 * \param rect A rectangle defining the line to be drawn
	 * \return NSERROR_OK on success else error code.
	 */
	nserror (*rectangle)(
			const struct redraw_context *ctx,
			const plot_style_t *pstyle,
			const struct rect *rectangle);

	/**
	 * Plot a polygon
	 *
	 * Plots a filled polygon with straight lines between
	 * points. The lines around the edge of the ploygon are not
	 * plotted. The polygon is filled with the non-zero winding
	 * rule.
	 *
	 * \param ctx The current redraw context.
	 * \param pstyle Style controlling the polygon plot.
	 * \param p verticies of polygon
	 * \param n number of verticies.
	 * \return NSERROR_OK on success else error code.
	 */
	nserror (*polygon)(
			const struct redraw_context *ctx,
			const plot_style_t *pstyle,
			const int *p,
			unsigned int n);

	/**
	 * Plots a path.
	 *
	 * Path plot consisting of cubic Bezier curves. Line and fill colour is
	 *  controlled by the plot style.
	 *
	 * The transform to apply is affine (meaning it omits the three
	 * projection factor parameters from the standard 3x3 matrix assumining
	 * default values)
	 *
	 * +--------------+--------------+--------------+
	 * | transform[0] | transform[1] |      0.0     |
	 * +--------------+--------------+--------------+
	 * | transform[2] | transform[3] |      0.0     |
	 * +--------------+--------------+--------------+
	 * | transform[4] | transform[5] |      1.0     |
	 * +--------------+--------------+--------------+
	 *
	 * \param ctx The current redraw context.
	 * \param pstyle Style controlling the path plot.
	 * \param p elements of path
	 * \param n nunber of elements on path
	 * \param transform A transform to apply to the path.
	 * \return NSERROR_OK on success else error code.
	 */
	nserror (*path)(
			const struct redraw_context *ctx,
			const plot_style_t *pstyle,
			const float *p,
			unsigned int n,
			const float transform[6]);

	/**
	 * Plot a bitmap
	 *
	 * Tiled plot of a bitmap image. (x,y) gives the top left
	 * coordinate of an explicitly placed tile. From this tile the
	 * image can repeat in all four directions -- up, down, left
	 * and right -- to the extents given by the current clip
	 * rectangle.
	 *
	 * The bitmap_flags say whether to tile in the x and y
	 * directions. If not tiling in x or y directions, the single
	 * image is plotted. The width and height give the dimensions
	 * the image is to be scaled to.
	 *
	 * \param ctx The current redraw context.
	 * \param bitmap The bitmap to plot
	 * \param x The x coordinate to plot the bitmap
	 * \param y The y coordiante to plot the bitmap
	 * \param width The width of area to plot the bitmap into
	 * \param height The height of area to plot the bitmap into
	 * \param bg the background colour to alpha blend into
	 * \param flags the flags controlling the type of plot operation
	 * \return NSERROR_OK on success else error code.
	 */
	nserror (*bitmap)(
			const struct redraw_context *ctx,
			struct bitmap *bitmap,
			int x,
			int y,
			int width,
			int height,
			colour bg,
			bitmap_flags_t flags);

	/**
	 * Text plotting.
	 *
	 * \param ctx The current redraw context.
	 * \param fstyle plot style for this text
	 * \param x x coordinate
	 * \param y y coordinate
	 * \param text UTF-8 string to plot
	 * \param length length of string, in bytes
	 * \return NSERROR_OK on success else error code.
	 */
	nserror (*text)(
			const struct redraw_context *ctx,
			const plot_font_style_t *fstyle,
			int x,
			int y,
			const char *text,
			size_t length);

	/**
	 * Start of a group of objects.
	 *
	 * optional, may be NULL. Used when plotter implements export
	 *  to a vector graphics file format.
	 *
	 * \param ctx The current redraw context.
	 * \return NSERROR_OK on success else error code.
	 */
	nserror (*group_start)(
			const struct redraw_context *ctx,
			const char *name);

	/**
	 * End of the most recently started group.
	 *
	 * optional, may be NULL
	 *
	 * \param ctx The current redraw context.
	 * \return NSERROR_OK on success else error code.
	 */
	nserror (*group_end)(
			const struct redraw_context *ctx);

	/**
	 * Only used internally by the knockout code. Must be NULL in
	 * any front end display plotters or export plotters.
	 *
	 * \param ctx The current redraw context.
	 * \return NSERROR_OK on success else error code.
	 */
	nserror (*flush)(
			const struct redraw_context *ctx);

	/* flags */
	/**
	 * flag to enable knockout rendering.
	 *
	 * Optimisation particularly for unaccelerated screen
	 *  redraw. It tries to avoid plotting to the same area more
	 *  than once. See desktop/knockout.c
	 */
	bool option_knockout;
};

#endif
