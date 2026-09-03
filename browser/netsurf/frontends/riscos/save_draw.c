/*
 * Copyright 2004 John M Bell <jmb202@ecs.soton.ac.uk>
 * Copyright 2004-2008 John Tytgat <joty@netsurf-browser.org>
 * Copyright 2007 James Bursa <bursa@users.sourceforge.net>
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
 * Export a content as a DrawFile (implementation).
 */

#ifdef WITH_DRAW_EXPORT

#include <assert.h>
#include <limits.h>
#include <oslib/draw.h>
#include <oslib/osfile.h>
#include <pencil.h>

#include "utils/log.h"
#include "netsurf/plotters.h"
#include "netsurf/content.h"

#include "riscos/bitmap.h"
#include "riscos/gui.h"
#include "riscos/save_draw.h"
#include "riscos/font.h"


static struct pencil_diagram *ro_save_draw_diagram;
static int ro_save_draw_width;
static int ro_save_draw_height;


/**
 * Report an error from pencil.
 *
 * \param  code  error code
 * \return  false
 */
static nserror ro_save_draw_error(pencil_code code)
{
	NSLOG(netsurf, INFO, "code %i", code);

	switch (code) {
	case pencil_OK:
		assert(0);
		break;

	case pencil_OUT_OF_MEMORY:
		ro_warn_user("NoMemory", 0);
		break;

	case pencil_FONT_MANAGER_ERROR:
		ro_warn_user("SaveError", rufl_fm_error->errmess);
		break;

	case pencil_FONT_NOT_FOUND:
	case pencil_IO_ERROR:
	case pencil_IO_EOF:
		ro_warn_user("SaveError", "generating the DrawFile failed");
		break;
	}

	return NSERROR_INVALID;
}

/**
 * \brief Sets a clip rectangle for subsequent plot operations.
 *
 * \param ctx The current redraw context.
 * \param clip The rectangle to limit all subsequent plot
 *              operations within.
 * \return NSERROR_OK on success else error code.
 */
static nserror
ro_save_draw_clip(const struct redraw_context *ctx, const struct rect *clip)
{
	return NSERROR_OK;
}


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
static nserror
ro_save_draw_arc(const struct redraw_context *ctx,
	       const plot_style_t *style,
	       int x, int y, int radius, int angle1, int angle2)
{
	return NSERROR_OK;
}


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
static nserror
ro_save_draw_disc(const struct redraw_context *ctx,
		const plot_style_t *style,
		int x, int y, int radius)
{
	return NSERROR_OK;
}


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
static nserror
ro_save_draw_line(const struct redraw_context *ctx,
		const plot_style_t *style,
		const struct rect *line)
{
	pencil_code code;
	const int path[] = {
		draw_MOVE_TO, line->x0 * 2, -line->y0 * 2 - 1,
		draw_LINE_TO, line->x1 * 2, -line->y1 * 2 - 1,
		draw_END_PATH
	};

	code = pencil_path(ro_save_draw_diagram,
			   path,
			   sizeof path / sizeof path[0],
			   pencil_TRANSPARENT,
			   style->stroke_colour << 8,
			   plot_style_fixed_to_int(style->stroke_width),
			   pencil_JOIN_MITRED,
			   pencil_CAP_BUTT,
			   pencil_CAP_BUTT,
			   0, 0, false,
			   pencil_SOLID);
	if (code != pencil_OK)
		return ro_save_draw_error(code);

	return NSERROR_OK;
}


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
static nserror
ro_save_draw_rectangle(const struct redraw_context *ctx,
		     const plot_style_t *style,
		     const struct rect *rect)
{
	pencil_code code;
	const int path[] = {
		draw_MOVE_TO, rect->x0 * 2, -rect->y0 * 2 - 1,
		draw_LINE_TO, rect->x1 * 2, -rect->y0 * 2 - 1,
		draw_LINE_TO, rect->x1 * 2, -rect->y1 * 2 - 1,
		draw_LINE_TO, rect->x0 * 2, -rect->y1 * 2 - 1,
		draw_CLOSE_LINE,
		draw_END_PATH
	};

	if (style->fill_type != PLOT_OP_TYPE_NONE) {

		code = pencil_path(ro_save_draw_diagram,
				   path,
				   sizeof path / sizeof path[0],
				   style->fill_colour << 8,
				   pencil_TRANSPARENT,
				   0,
				   pencil_JOIN_MITRED,
				   pencil_CAP_BUTT,
				   pencil_CAP_BUTT,
				   0,
				   0,
				   false,
				   pencil_SOLID);
		if (code != pencil_OK)
			return ro_save_draw_error(code);
	}

	if (style->stroke_type != PLOT_OP_TYPE_NONE) {

		code = pencil_path(ro_save_draw_diagram,
				   path,
				   sizeof path / sizeof path[0],
				   pencil_TRANSPARENT,
				   style->stroke_colour << 8,
				   plot_style_fixed_to_int(style->stroke_width),
				   pencil_JOIN_MITRED,
				   pencil_CAP_BUTT,
				   pencil_CAP_BUTT,
				   0,
				   0,
				   false,
				   pencil_SOLID);

		if (code != pencil_OK)
			return ro_save_draw_error(code);
	}
	return NSERROR_OK;
}


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
static nserror
ro_save_draw_polygon(const struct redraw_context *ctx,
		     const plot_style_t *style,
		     const int *p,
		     unsigned int n)
{
	pencil_code code;
	int path[n * 3 + 1];
	unsigned int i;

	for (i = 0; i != n; i++) {
		path[i * 3 + 0] = draw_LINE_TO;
		path[i * 3 + 1] = p[i * 2 + 0] * 2;
		path[i * 3 + 2] = -p[i * 2 + 1] * 2;
	}
	path[0] = draw_MOVE_TO;
	path[n * 3] = draw_END_PATH;

	code = pencil_path(ro_save_draw_diagram,
			   path, n * 3 + 1,
			   style->fill_colour << 8,
			   pencil_TRANSPARENT,
			   0,
			   pencil_JOIN_MITRED,
			   pencil_CAP_BUTT,
			   pencil_CAP_BUTT,
			   0,
			   0,
			   false,
			   pencil_SOLID);
	if (code != pencil_OK)
		return ro_save_draw_error(code);

	return NSERROR_OK;
}


/**
 * Plots a path.
 *
 * Path plot consisting of cubic Bezier curves. Line and fill colour is
 *  controlled by the plot style.
 *
 * \param ctx The current redraw context.
 * \param pstyle Style controlling the path plot.
 * \param p elements of path
 * \param n nunber of elements on path
 * \param transform A transform to apply to the path.
 * \return NSERROR_OK on success else error code.
 */
static nserror
ro_save_draw_path(const struct redraw_context *ctx,
		  const plot_style_t *pstyle,
		  const float *p,
		  unsigned int n,
		  const float transform[6])
{
	pencil_code code;
	int *path;
	unsigned int i;
	bool empty_path = true;

	if (n == 0)
		return NSERROR_OK;

	if (p[0] != PLOTTER_PATH_MOVE) {
		NSLOG(netsurf, INFO, "path doesn't start with a move");
		return NSERROR_INVALID;
	}

	path = malloc(sizeof *path * (n + 10));
	if (!path) {
		NSLOG(netsurf, INFO, "out of memory");
		return NSERROR_INVALID;
	}

	for (i = 0; i < n; ) {
		if (p[i] == PLOTTER_PATH_MOVE) {
			path[i] = draw_MOVE_TO;
			path[i + 1] = (transform[0] * p[i + 1] +
					transform[2] * -p[i + 2] +
					transform[4]) * 2;
			path[i + 2] = (transform[1] * p[i + 1] +
					transform[3] * -p[i + 2] +
					-transform[5]) * 2;
			i += 3;
		} else if (p[i] == PLOTTER_PATH_CLOSE) {
			path[i] = draw_CLOSE_LINE;
			i++;
		} else if (p[i] == PLOTTER_PATH_LINE) {
			path[i] = draw_LINE_TO;
			path[i + 1] = (transform[0] * p[i + 1] +
					transform[2] * -p[i + 2] +
					transform[4]) * 2;
			path[i + 2] = (transform[1] * p[i + 1] +
					transform[3] * -p[i + 2] +
					-transform[5]) * 2;
			i += 3;
			empty_path = false;
		} else if (p[i] == PLOTTER_PATH_BEZIER) {
			path[i] = draw_BEZIER_TO;
			path[i + 1] = (transform[0] * p[i + 1] +
					transform[2] * -p[i + 2] +
					transform[4]) * 2;
			path[i + 2] = (transform[1] * p[i + 1] +
					transform[3] * -p[i + 2] +
					-transform[5]) * 2;
			path[i + 3] = (transform[0] * p[i + 3] +
					transform[2] * -p[i + 4] +
					transform[4]) * 2;
			path[i + 4] = (transform[1] * p[i + 3] +
					transform[3] * -p[i + 4] +
					-transform[5]) * 2;
			path[i + 5] = (transform[0] * p[i + 5] +
					transform[2] * -p[i + 6] +
					transform[4]) * 2;
			path[i + 6] = (transform[1] * p[i + 5] +
					transform[3] * -p[i + 6] +
					-transform[5]) * 2;
			i += 7;
			empty_path = false;
		} else {
			NSLOG(netsurf, INFO, "bad path command %f", p[i]);
			free(path);
			return NSERROR_INVALID;
		}
	}
	path[i] = draw_END_PATH;

	if (empty_path) {
		free(path);
		return NSERROR_OK;
	}

	code = pencil_path(ro_save_draw_diagram,
			   path, i + 1,
			   pstyle->fill_colour == NS_TRANSPARENT ?
			   pencil_TRANSPARENT :
			   pstyle->fill_colour << 8,
			   pstyle->stroke_colour == NS_TRANSPARENT ?
			   pencil_TRANSPARENT :
			   pstyle->stroke_colour << 8,
			   plot_style_fixed_to_int(pstyle->stroke_width),
			   pencil_JOIN_MITRED,
			   pencil_CAP_BUTT,
			   pencil_CAP_BUTT,
			   0,
			   0,
			   false,
			   pencil_SOLID);
	free(path);
	if (code != pencil_OK)
		return ro_save_draw_error(code);

	return NSERROR_OK;
}


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
static nserror
ro_save_draw_bitmap(const struct redraw_context *ctx,
		    struct bitmap *bitmap,
		    int x, int y,
		    int width,
		    int height,
		    colour bg,
		    bitmap_flags_t flags)
{
	pencil_code code;
	const uint8_t *buffer;

	buffer = riscos_bitmap_get_buffer(bitmap);
	if (!buffer) {
		ro_warn_user("NoMemory", 0);
		return NSERROR_INVALID;
	}

	code = pencil_sprite(ro_save_draw_diagram,
			     x * 2, (-y - height) * 2,
			     width * 2, height * 2,
			     ((char *) bitmap->sprite_area) +
			     bitmap->sprite_area->first);
	if (code != pencil_OK)
		return ro_save_draw_error(code);

	return NSERROR_OK;
}


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
static nserror
ro_save_draw_text(const struct redraw_context *ctx,
		const struct plot_font_style *fstyle,
		int x,
		int y,
		const char *text,
		size_t length)
{
	pencil_code code;
	const char *font_family;
	unsigned int font_size;
	rufl_style font_style;

	nsfont_read_style(fstyle, &font_family, &font_size, &font_style);

	code = pencil_text(ro_save_draw_diagram, x * 2, -y * 2, font_family,
			font_style, font_size, text, length,
			fstyle->foreground << 8);
	if (code != pencil_OK)
		return ro_save_draw_error(code);

	return NSERROR_OK;
}


/**
 * Start of a group of objects.
 *
 * \param ctx The current redraw context.
 * \return NSERROR_OK on success else error code.
 */
static nserror
ro_save_draw_group_start(const struct redraw_context *ctx, const char *name)
{
	pencil_code code;

	code = pencil_group_start(ro_save_draw_diagram, name);
	if (code != pencil_OK)
		return ro_save_draw_error(code);

	return NSERROR_OK;
}


/**
 * End of the most recently started group.
 *
 * \param ctx The current redraw context.
 * \return NSERROR_OK on success else error code.
 */
static nserror
ro_save_draw_group_end(const struct redraw_context *ctx)
{
	pencil_code code;

	code = pencil_group_end(ro_save_draw_diagram);
	if (code != pencil_OK)
		return ro_save_draw_error(code);

	return NSERROR_OK;
}


static const struct plotter_table ro_save_draw_plotters = {
	.rectangle = ro_save_draw_rectangle,
	.line = ro_save_draw_line,
	.polygon = ro_save_draw_polygon,
	.clip = ro_save_draw_clip,
	.text = ro_save_draw_text,
	.disc = ro_save_draw_disc,
	.arc = ro_save_draw_arc,
	.bitmap = ro_save_draw_bitmap,
	.group_start = ro_save_draw_group_start,
	.group_end = ro_save_draw_group_end,
	.path = ro_save_draw_path,
	.option_knockout = false,
};


/* exported interface documented in save_draw.h */
bool save_as_draw(struct hlcache_handle *h, const char *path)
{
	pencil_code code;
	char *drawfile_buffer;
	struct rect clip;
	struct content_redraw_data data;
	size_t drawfile_size;
	os_error *error;
	struct redraw_context ctx = {
		.interactive = false,
		.background_images = true,
		.plot = &ro_save_draw_plotters
	};

	ro_save_draw_diagram = pencil_create();
	if (!ro_save_draw_diagram) {
		ro_warn_user("NoMemory", 0);
		return false;
	}

	ro_save_draw_width = content_get_width(h);
	ro_save_draw_height = content_get_height(h);

	clip.x0 = clip.y0 = INT_MIN;
	clip.x1 = clip.y1 = INT_MAX;

	data.x = 0;
	data.y = -ro_save_draw_height;
	data.width = ro_save_draw_width;
	data.height = ro_save_draw_height;
	data.background_colour = 0xFFFFFF;
	data.scale = 1;
	data.repeat_x = false;
	data.repeat_y = false;
	data.background_blend_mode = 0;

	if (!content_redraw(h, &data, &clip, &ctx)) {
		pencil_free(ro_save_draw_diagram);
		return false;
	}

	/*pencil_dump(ro_save_draw_diagram);*/

	code = pencil_save_drawfile(ro_save_draw_diagram, "NetSurf",
			&drawfile_buffer, &drawfile_size);
	if (code != pencil_OK) {
		ro_warn_user("SaveError", 0);
		pencil_free(ro_save_draw_diagram);
		return false;
	}
	assert(drawfile_buffer);

	error = xosfile_save_stamped(path, osfile_TYPE_DRAW,
			(byte *) drawfile_buffer,
			(byte *) drawfile_buffer + drawfile_size);
	if (error) {
		NSLOG(netsurf, INFO, "xosfile_save_stamped failed: 0x%x: %s",
		      error->errnum, error->errmess);
		ro_warn_user("SaveError", error->errmess);
		pencil_free(ro_save_draw_diagram);
		return false;
	}

	pencil_free(ro_save_draw_diagram);

	return true;
}

#endif
