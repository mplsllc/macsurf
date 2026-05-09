/*
 * Copyright 2006 Rob Kendrick <rjek@rjek.com>
 * Copyright 2005 James Bursa <bursa@users.sourceforge.net>
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
 * GTK and Cairo plotter implementations.
 *
 * Uses Cairo drawing primitives to render browser output.
 * \todo remove the use of the gdk structure for clipping
 */

#include <math.h>
#include <assert.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>

#include "utils/log.h"
#include "netsurf/plotters.h"
#include "utils/nsoption.h"

#include "gtk/layout_pango.h"
#include "gtk/plotters.h"
#include "gtk/scaffolding.h"
#include "gtk/bitmap.h"

cairo_t *current_cr;

static GdkRectangle cliprect;

/**
 * Set cairo context colour to nsgtk colour.
 *
 * \param c the netsurf colour to set in cairo
 */
void nsgtk_set_colour(colour c)
{
	cairo_set_source_rgba(current_cr, 
			      (c & 0xff) / 255.0, 
			      ((c & 0xff00) >> 8) / 255.0, 
			      ((c & 0xff0000) >> 16) / 255.0, 
			      1.0);
}


/**
 * Set cairo context to solid plot operation.
 */
static inline void nsgtk_set_solid(void)
{
	double dashes = 0;
	cairo_set_dash(current_cr, &dashes, 0, 0);
}


/**
 * Set cairo context to dotted plot operation.
 */
static inline void nsgtk_set_dotted(void)
{
	double cdashes[] = { 1.0, 2.0 };
	cairo_set_dash(current_cr, cdashes, 2, 0);
}


/**
 * Set cairo context to dashed plot operation.
 */
static inline void nsgtk_set_dashed(void)
{
	double cdashes[] = { 8.0, 2.0 };
	cairo_set_dash(current_cr, cdashes, 2, 0);
}


/**
 * Set cairo context line width.
 */
static inline void nsgtk_set_line_width(plot_style_fixed width)
{
	if (width == 0) {
		cairo_set_line_width(current_cr, 1);
	} else {
		cairo_set_line_width(current_cr,
				plot_style_fixed_to_double(width));
	}
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
nsgtk_plot_clip(const struct redraw_context *ctx, const struct rect *clip)
{
	cairo_reset_clip(current_cr);
	cairo_rectangle(current_cr, clip->x0, clip->y0,
			clip->x1 - clip->x0, clip->y1 - clip->y0);
	cairo_clip(current_cr);

	cliprect.x = clip->x0;
	cliprect.y = clip->y0;
	cliprect.width = clip->x1 - clip->x0;
	cliprect.height = clip->y1 - clip->y0;

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
 * \param style Style controlling the arc plot.
 * \param x The x coordinate of the arc.
 * \param y The y coordinate of the arc.
 * \param radius The radius of the arc.
 * \param angle1 The start angle of the arc.
 * \param angle2 The finish angle of the arc.
 * \return NSERROR_OK on success else error code.
 */
static nserror
nsgtk_plot_arc(const struct redraw_context *ctx,
	       const plot_style_t *style,
	       int x, int y, int radius, int angle1, int angle2)
{
	nsgtk_set_colour(style->fill_colour);
	nsgtk_set_solid();

	cairo_set_line_width(current_cr, 1);
	cairo_arc(current_cr, x, y, radius,
		  (angle1 + 90) * (M_PI / 180),
		  (angle2 + 90) * (M_PI / 180));
	cairo_stroke(current_cr);

	return NSERROR_OK;
}


/**
 * Plots a circle
 *
 * Plot a circle centered on (x,y), which is optionally filled.
 *
 * \param ctx The current redraw context.
 * \param style Style controlling the circle plot.
 * \param x x coordinate of circle centre.
 * \param y y coordinate of circle centre.
 * \param radius circle radius.
 * \return NSERROR_OK on success else error code.
 */
static nserror
nsgtk_plot_disc(const struct redraw_context *ctx,
		const plot_style_t *style,
		int x, int y, int radius)
{
	if (style->fill_type != PLOT_OP_TYPE_NONE) {
		nsgtk_set_colour(style->fill_colour);
		nsgtk_set_solid();
		cairo_set_line_width(current_cr, 0);
		cairo_arc(current_cr, x, y, radius, 0, M_PI * 2);
		cairo_fill(current_cr);
		cairo_stroke(current_cr);
	}

	if (style->stroke_type != PLOT_OP_TYPE_NONE) {
		nsgtk_set_colour(style->stroke_colour);

		switch (style->stroke_type) {
		case PLOT_OP_TYPE_SOLID: /* Solid colour */
		default:
			nsgtk_set_solid();
			break;

		case PLOT_OP_TYPE_DOT: /* Doted plot */
			nsgtk_set_dotted();
			break;

		case PLOT_OP_TYPE_DASH: /* dashed plot */
			nsgtk_set_dashed();
			break;
		}

		nsgtk_set_line_width(style->stroke_width);

		cairo_arc(current_cr, x, y, radius, 0, M_PI * 2);

		cairo_stroke(current_cr);
	}

	return NSERROR_OK;
}


/**
 * Plots a line
 *
 * plot a line from (x0,y0) to (x1,y1). Coordinates are at
 *  centre of line width/thickness.
 *
 * \param ctx The current redraw context.
 * \param style Style controlling the line plot.
 * \param line A rectangle defining the line to be drawn
 * \return NSERROR_OK on success else error code.
 */
static nserror
nsgtk_plot_line(const struct redraw_context *ctx,
		const plot_style_t *style,
		const struct rect *line)
{
	nsgtk_set_colour(style->stroke_colour);

	switch (style->stroke_type) {
	case PLOT_OP_TYPE_SOLID: /* Solid colour */
	default:
		nsgtk_set_solid();
		break;

	case PLOT_OP_TYPE_DOT: /* Doted plot */
		nsgtk_set_dotted();
		break;

	case PLOT_OP_TYPE_DASH: /* dashed plot */
		nsgtk_set_dashed();
		break;
	}

	if (style->stroke_type != PLOT_OP_TYPE_NONE) {
		nsgtk_set_colour(style->stroke_colour);
	}

	nsgtk_set_line_width(style->stroke_width);

	/* core expects horizontal and vertical lines to be on pixels, not
	 * between pixels
	 */
	cairo_move_to(current_cr,
		      (line->x0 == line->x1) ? line->x0 + 0.5 : line->x0,
		      (line->y0 == line->y1) ? line->y0 + 0.5 : line->y0);
	cairo_line_to(current_cr,
		      (line->x0 == line->x1) ? line->x1 + 0.5 : line->x1,
		      (line->y0 == line->y1) ? line->y1 + 0.5 : line->y1);
	cairo_stroke(current_cr);

	return NSERROR_OK;
}


/**
 * Plot a caret.
 *
 * @note It is assumed that the plotters have been set up.
 */
void nsgtk_plot_caret(int x, int y, int h)
{
	nsgtk_set_solid(); /* solid line */
	nsgtk_set_colour(0); /* black */
	cairo_set_line_width(current_cr, 1); /* thin line */

	/* core expects horizontal and vertical lines to be on pixels, not
	 * between pixels */
	cairo_move_to(current_cr, x + 0.5, y);
	cairo_line_to(current_cr, x + 0.5, y + h - 1);
	cairo_stroke(current_cr);
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
 * \param style Style controlling the rectangle plot.
 * \param rect A rectangle defining the line to be drawn
 * \return NSERROR_OK on success else error code.
 */
static nserror
nsgtk_plot_rectangle(const struct redraw_context *ctx,
		     const plot_style_t *style,
		     const struct rect *rect)
{
	if (style->fill_type != PLOT_OP_TYPE_NONE) {
		nsgtk_set_colour(style->fill_colour);
		nsgtk_set_solid();

		cairo_set_line_width(current_cr, 0);
		cairo_rectangle(current_cr,
				rect->x0,
				rect->y0,
				rect->x1 - rect->x0,
				rect->y1 - rect->y0);
		cairo_fill(current_cr);
		cairo_stroke(current_cr);
	}

	if (style->stroke_type != PLOT_OP_TYPE_NONE) {
		nsgtk_set_colour(style->stroke_colour);

		switch (style->stroke_type) {
		case PLOT_OP_TYPE_SOLID: /* Solid colour */
		default:
			nsgtk_set_solid();
			break;

		case PLOT_OP_TYPE_DOT: /* Doted plot */
			nsgtk_set_dotted();
			break;

		case PLOT_OP_TYPE_DASH: /* dashed plot */
			nsgtk_set_dashed();
			break;
		}

		nsgtk_set_line_width(style->stroke_width);

		cairo_rectangle(current_cr,
				rect->x0 + 0.5,
				rect->y0 + 0.5,
				rect->x1 - rect->x0,
				rect->y1 - rect->y0);
		cairo_stroke(current_cr);
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
 * \param style Style controlling the polygon plot.
 * \param p verticies of polygon
 * \param n number of verticies.
 * \return NSERROR_OK on success else error code.
 */
static nserror
nsgtk_plot_polygon(const struct redraw_context *ctx,
		   const plot_style_t *style,
		   const int *p,
		   unsigned int n)
{
	unsigned int i;

	nsgtk_set_colour(style->fill_colour);
	nsgtk_set_solid();

	cairo_set_line_width(current_cr, 0);
	cairo_move_to(current_cr, p[0], p[1]);
	for (i = 1; i != n; i++) {
		cairo_line_to(current_cr, p[i * 2], p[i * 2 + 1]);
	}
	cairo_fill(current_cr);
	cairo_stroke(current_cr);

	return NSERROR_OK;
}


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
static nserror
nsgtk_plot_path(const struct redraw_context *ctx,
		const plot_style_t *pstyle,
		const float *p,
		unsigned int n,
		const float transform[6])
{
	unsigned int i;
	cairo_matrix_t old_ctm, n_ctm;

	if (n == 0)
		return NSERROR_OK;

	if (p[0] != PLOTTER_PATH_MOVE) {
		NSLOG(netsurf, INFO, "Path does not start with move");
		return NSERROR_INVALID;
	}

	/* Save CTM */
	cairo_get_matrix(current_cr, &old_ctm);

	/* Set up line style and width */
	nsgtk_set_line_width(pstyle->stroke_width);
	nsgtk_set_solid();

	/* Load new CTM */
	n_ctm.xx = transform[0];
	n_ctm.yx = transform[1];
	n_ctm.xy = transform[2];
	n_ctm.yy = transform[3];
	n_ctm.x0 = transform[4] + old_ctm.x0;
	n_ctm.y0 = transform[5] + old_ctm.y0;

	cairo_set_matrix(current_cr, &n_ctm);

	/* Construct path */
	for (i = 0; i < n; ) {
		if (p[i] == PLOTTER_PATH_MOVE) {
			cairo_move_to(current_cr, p[i+1], p[i+2]);
			i += 3;
		} else if (p[i] == PLOTTER_PATH_CLOSE) {
			cairo_close_path(current_cr);
			i++;
		} else if (p[i] == PLOTTER_PATH_LINE) {
			cairo_line_to(current_cr, p[i+1], p[i+2]);
			i += 3;
		} else if (p[i] == PLOTTER_PATH_BEZIER) {
			cairo_curve_to(current_cr, p[i+1], p[i+2],
				       p[i+3], p[i+4],
				       p[i+5], p[i+6]);
			i += 7;
		} else {
			NSLOG(netsurf, INFO, "bad path command %f", p[i]);
			/* Reset matrix for safety */
			cairo_set_matrix(current_cr, &old_ctm);
			return NSERROR_INVALID;
		}
	}

	/* Restore original CTM */
	cairo_set_matrix(current_cr, &old_ctm);

	/* Now draw path */
	if (pstyle->fill_colour != NS_TRANSPARENT) {
		nsgtk_set_colour(pstyle->fill_colour);

		if (pstyle->stroke_colour != NS_TRANSPARENT) {
			/* Fill & Stroke */
			cairo_fill_preserve(current_cr);
			nsgtk_set_colour(pstyle->stroke_colour);
			cairo_stroke(current_cr);
		} else {
			/* Fill only */
			cairo_fill(current_cr);
		}
	} else if (pstyle->stroke_colour != NS_TRANSPARENT) {
		/* Stroke only */
		nsgtk_set_colour(pstyle->stroke_colour);
		cairo_stroke(current_cr);
	}

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
nsgtk_plot_bitmap(const struct redraw_context *ctx,
		  struct bitmap *bitmap,
		  int x, int y,
		  int width,
		  int height,
		  colour bg,
		  bitmap_flags_t flags)
{
	bool repeat_x = (flags & BITMAPF_REPEAT_X);
	bool repeat_y = (flags & BITMAPF_REPEAT_Y);
	GdkRectangle cliprect_bitmap;
	cairo_surface_t *img_surface;
	int img_width, img_height;

	/* Bail early if we can */
	if (width <= 0 || height <= 0) {
		/* Nothing to plot */
		return NSERROR_OK;
	}

	/* Copy the clip rectangle into bitmap plot clip rectangle */
	cliprect_bitmap = cliprect;

	/* Constrain bitmap plot rectangle for any lack of tiling */
	if (!repeat_x) {
		if (cliprect_bitmap.width > width) {
			cliprect_bitmap.width = width;
		}
		if (cliprect_bitmap.x < x) {
			cliprect_bitmap.x = x;
			cliprect_bitmap.width -= x - cliprect_bitmap.x;
		}
	}
	if (!repeat_y) {
		if (cliprect_bitmap.height > height) {
			cliprect_bitmap.height = height;
		}
		if (cliprect_bitmap.y < y) {
			cliprect_bitmap.y = y;
			cliprect_bitmap.height -= y - cliprect_bitmap.y;
		}
	}

	/* Bail early if we can */
	if (cliprect_bitmap.width <= 0 || cliprect_bitmap.height <= 0) {
		/* Nothing to plot */
		return NSERROR_OK;
	}

	/* Get the image's surface and intrinsic dimensions */
	img_surface = bitmap->surface;
	img_width = cairo_image_surface_get_width(img_surface);
	img_height = cairo_image_surface_get_height(img_surface);

	/* Set the source surface */
	if ((img_width == width) && (img_height == height)) {
		/* Non-scaled rendering */
		cairo_set_source_surface(current_cr, img_surface, x, y);

		/* Enable tiling if we're repeating */
		if (repeat_x || repeat_y) {
			cairo_pattern_set_extend(
					cairo_get_source(current_cr),
					CAIRO_EXTEND_REPEAT);
		}

		/* Render the bitmap */
		cairo_rectangle(current_cr,
				cliprect_bitmap.x,
				cliprect_bitmap.y,
				cliprect_bitmap.width,
				cliprect_bitmap.height);
		cairo_fill(current_cr);
	} else {
		/* Scaled rendering */
		double scale_x = (double)width / img_width;
		double scale_y = (double)height / img_height;

		/* Save cairo rendering context state before scaling */
		cairo_save(current_cr);
		cairo_scale(current_cr, scale_x, scale_y);

		cairo_set_source_surface(current_cr, img_surface,
				x / scale_x, y / scale_y);

		/* Enable tiling if we're repeating */
		if (repeat_x || repeat_y) {
			cairo_pattern_set_extend(
					cairo_get_source(current_cr),
					CAIRO_EXTEND_REPEAT);
		}

		/* Render the bitmap */
		cairo_rectangle(current_cr,
				cliprect_bitmap.x / scale_x,
				cliprect_bitmap.y / scale_y,
				cliprect_bitmap.width / scale_x,
				cliprect_bitmap.height / scale_y);
		cairo_fill(current_cr);

		/* Restore pre-scaling cairo rendering state */
		cairo_restore(current_cr);
	}

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
nsgtk_plot_text(const struct redraw_context *ctx,
		const struct plot_font_style *fstyle,
		int x,
		int y,
		const char *text,
		size_t length)
{
	return nsfont_paint(x, y, text, length, fstyle);
}


/** GTK plotter table */
const struct plotter_table nsgtk_plotters = {
	.clip = nsgtk_plot_clip,
	.arc = nsgtk_plot_arc,
	.disc = nsgtk_plot_disc,
	.line = nsgtk_plot_line,
	.rectangle = nsgtk_plot_rectangle,
	.polygon = nsgtk_plot_polygon,
	.path = nsgtk_plot_path,
	.bitmap = nsgtk_plot_bitmap,
	.text = nsgtk_plot_text,
	.option_knockout = true
};
