/*
 * Copyright 2006 Rob Kendrick <rjek@rjek.com>
 * Copyright 2005 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2008 Adam Blokus <adamblokus@gmail.com>
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
  * GTK printing implementation.
  * All the functions and structures necessary for printing( signal handlers,
  * plotters, printer) are here.
  * Most of the plotters have been copied from the gtk_plotters.c file.
  */

#include "utils/config.h"

#include <math.h>
#include <assert.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>

#include "utils/log.h"
#include "utils/utils.h"
#include "utils/nsoption.h"
#include "netsurf/plotters.h"
#include "desktop/print.h"
#include "desktop/printer.h"

#include "gtk/layout_pango.h"
#include "gtk/bitmap.h"
#include "gtk/print.h"
#include "gtk/scaffolding.h"

/* Globals */
cairo_t *gtk_print_current_cr;
static struct print_settings* settings;
struct hlcache_handle *content_to_print;
static GdkRectangle cliprect;

static inline void nsgtk_print_set_colour(colour c)
{
	int r, g, b;

	r = c & 0xff;
	g = (c & 0xff00) >> 8;
	b = (c & 0xff0000) >> 16;

#ifdef FIXME
	GdkColor colour;
	colour.red = r | (r << 8);
	colour.green = g | (g << 8);
	colour.blue = b | (b << 8);
	colour.pixel = (r << 16) | (g << 8) | b;
	gdk_colormap_alloc_color(gdk_colormap_get_system(), &colour, true, true);
#endif
	cairo_set_source_rgba(gtk_print_current_cr, r / 255.0,
			g / 255.0, b / 255.0, 1.0);
}



static nserror gtk_print_font_paint(int x, int y, 
		const char *string, size_t length,
		const plot_font_style_t *fstyle)
{
	PangoFontDescription *desc;
	PangoLayout *layout;
	gint size;
	PangoLayoutLine *line;

	if (length == 0)
		return NSERROR_OK;

	desc = nsfont_style_to_description(fstyle);
	size = (gint) ((double) pango_font_description_get_size(desc) * 
				settings->scale);

	if (pango_font_description_get_size_is_absolute(desc))
		pango_font_description_set_absolute_size(desc, size);
	else
		pango_font_description_set_size(desc, size);

	layout = pango_cairo_create_layout(gtk_print_current_cr);

	pango_layout_set_font_description(layout, desc);
	pango_layout_set_text(layout, string, length);
	
	line = pango_layout_get_line(layout, 0);

	cairo_move_to(gtk_print_current_cr, x, y);
	nsgtk_print_set_colour(fstyle->foreground);
	pango_cairo_show_layout_line(gtk_print_current_cr, line);

	g_object_unref(layout);
	pango_font_description_free(desc);

	return NSERROR_OK;
}


/** Set cairo context to solid plot operation. */
static inline void nsgtk_print_set_solid(void)
{
	double dashes = 0;
	cairo_set_dash(gtk_print_current_cr, &dashes, 0, 0);
}

/** Set cairo context to dotted plot operation. */
static inline void nsgtk_print_set_dotted(void)
{
	double cdashes[] = { 1.0, 2.0 };
	cairo_set_dash(gtk_print_current_cr, cdashes, 1, 0);
}

/** Set cairo context to dashed plot operation. */
static inline void nsgtk_print_set_dashed(void)
{
	double cdashes[] = { 8.0, 2.0 };
	cairo_set_dash(gtk_print_current_cr, cdashes, 1, 0);
}

/** Set cairo context line width. */
static inline void nsgtk_set_line_width(plot_style_fixed width)
{
	if (width == 0) {
		cairo_set_line_width(gtk_print_current_cr, 1);
	} else {
		cairo_set_line_width(gtk_print_current_cr,
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
nsgtk_print_plot_clip(const struct redraw_context *ctx, const struct rect *clip)
{
	NSLOG(netsurf, INFO,
	      "Clipping. x0: %i ;\t y0: %i ;\t x1: %i ;\t y1: %i", clip->x0,
	      clip->y0, clip->x1, clip->y1);	
	
	/* Normalize cllipping area - to prevent overflows.
	 * See comment in pdf_plot_fill. */
	int clip_x0 = min(max(clip->x0, 0), settings->page_width);
	int clip_y0 = min(max(clip->y0, 0), settings->page_height);
	int clip_x1 = min(max(clip->x1, 0), settings->page_width);
	int clip_y1 = min(max(clip->y1, 0), settings->page_height);
	
	cairo_reset_clip(gtk_print_current_cr);
	cairo_rectangle(gtk_print_current_cr, clip_x0, clip_y0,
			clip_x1 - clip_x0, clip_y1 - clip_y0);
	cairo_clip(gtk_print_current_cr);

	cliprect.x = clip_x0;
	cliprect.y = clip_y0;
	cliprect.width = clip_x1 - clip_x0;
	cliprect.height = clip_y1 - clip_y0;
	
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
nsgtk_print_plot_arc(const struct redraw_context *ctx,
	       const plot_style_t *style,
	       int x, int y, int radius, int angle1, int angle2)
{
	nsgtk_print_set_colour(style->fill_colour);
	nsgtk_print_set_solid();

	cairo_set_line_width(gtk_print_current_cr, 1);
	cairo_arc(gtk_print_current_cr, x, y, radius,
			(angle1 + 90) * (M_PI / 180),
			(angle2 + 90) * (M_PI / 180));
	cairo_stroke(gtk_print_current_cr);

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
nsgtk_print_plot_disc(const struct redraw_context *ctx,
		const plot_style_t *style,
		int x, int y, int radius)
{
	if (style->fill_type != PLOT_OP_TYPE_NONE) {
		nsgtk_print_set_colour(style->fill_colour);
		nsgtk_print_set_solid();
		cairo_set_line_width(gtk_print_current_cr, 0);
		cairo_arc(gtk_print_current_cr, x, y, radius, 0, M_PI * 2);
		cairo_fill(gtk_print_current_cr);
		cairo_stroke(gtk_print_current_cr);
	}

	if (style->stroke_type != PLOT_OP_TYPE_NONE) {
		nsgtk_print_set_colour(style->stroke_colour);

		switch (style->stroke_type) {
		case PLOT_OP_TYPE_SOLID: /**< Solid colour */
		default:
			nsgtk_print_set_solid();
			break;

		case PLOT_OP_TYPE_DOT: /**< Doted plot */
			nsgtk_print_set_dotted();
			break;

		case PLOT_OP_TYPE_DASH: /**< dashed plot */
			nsgtk_print_set_dashed();
			break;
		}

		nsgtk_set_line_width(style->stroke_width);

		cairo_arc(gtk_print_current_cr, x, y, radius, 0, M_PI * 2);

		cairo_stroke(gtk_print_current_cr);
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
nsgtk_print_plot_line(const struct redraw_context *ctx,
		const plot_style_t *style,
		const struct rect *line)
{
	nsgtk_print_set_colour(style->stroke_colour);

	switch (style->stroke_type) {
	case PLOT_OP_TYPE_SOLID: /**< Solid colour */
	default:
		nsgtk_print_set_solid();
		break;

	case PLOT_OP_TYPE_DOT: /**< Doted plot */
		nsgtk_print_set_dotted();
		break;

	case PLOT_OP_TYPE_DASH: /**< dashed plot */
		nsgtk_print_set_dashed();
		break;
	}

	nsgtk_set_line_width(style->stroke_width);

	cairo_move_to(gtk_print_current_cr, line->x0 + 0.5, line->y0 + 0.5);
	cairo_line_to(gtk_print_current_cr, line->x1 + 0.5, line->y1 + 0.5);
	cairo_stroke(gtk_print_current_cr);

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
 * \param style Style controlling the rectangle plot.
 * \param rect A rectangle defining the line to be drawn
 * \return NSERROR_OK on success else error code.
 */
static nserror
nsgtk_print_plot_rectangle(const struct redraw_context *ctx,
		     const plot_style_t *style,
		     const struct rect *rect)
{
	NSLOG(netsurf, INFO, "x0: %i ;\t y0: %i ;\t x1: %i ;\t y1: %i",
	      rect->x0, rect->y0, rect->x1, rect->y1);

        if (style->fill_type != PLOT_OP_TYPE_NONE) { 
		int x0,y0,x1,y1;

		nsgtk_print_set_colour(style->fill_colour);
		nsgtk_print_set_solid();
	
		/* Normalize boundaries of the area - to prevent overflows.
		 * See comment in pdf_plot_fill. */
		x0 = min(max(rect->x0, 0), settings->page_width);
		y0 = min(max(rect->y0, 0), settings->page_height);
		x1 = min(max(rect->x1, 0), settings->page_width);
		y1 = min(max(rect->y1, 0), settings->page_height);

		cairo_set_line_width(gtk_print_current_cr, 0);
		cairo_rectangle(gtk_print_current_cr,
				x0, y0,
				x1 - x0, y1 - y0);
		cairo_fill(gtk_print_current_cr);
		cairo_stroke(gtk_print_current_cr);
	}

        if (style->stroke_type != PLOT_OP_TYPE_NONE) { 

		nsgtk_print_set_colour(style->stroke_colour);

                switch (style->stroke_type) {
                case PLOT_OP_TYPE_SOLID: /**< Solid colour */
                default:
                        nsgtk_print_set_solid();
                        break;

                case PLOT_OP_TYPE_DOT: /**< Doted plot */
                        nsgtk_print_set_dotted();
                        break;

                case PLOT_OP_TYPE_DASH: /**< dashed plot */
                        nsgtk_print_set_dashed();
                        break;
                }

		nsgtk_set_line_width(style->stroke_width);

		cairo_rectangle(gtk_print_current_cr,
				rect->x0, rect->y0,
				rect->x1 - rect->x0, rect->y1 - rect->y0);

		cairo_stroke(gtk_print_current_cr);
	}
	
	return NSERROR_OK;
}


static nserror
nsgtk_print_plot_polygon(const struct redraw_context *ctx,
		   const plot_style_t *style,
		   const int *p,
		   unsigned int n)
{
	unsigned int i;

	NSLOG(netsurf, INFO, "Plotting polygon.");	

	nsgtk_print_set_colour(style->fill_colour);
	nsgtk_print_set_solid();
	
	cairo_set_line_width(gtk_print_current_cr, 0);
	cairo_move_to(gtk_print_current_cr, p[0], p[1]);

	NSLOG(netsurf, INFO, "Starting line at: %i\t%i", p[0], p[1]);

	for (i = 1; i != n; i++) {
		cairo_line_to(gtk_print_current_cr, p[i * 2], p[i * 2 + 1]);
		NSLOG(netsurf, INFO, "Drawing line to: %i\t%i", p[i * 2],
		      p[i * 2 + 1]);
	}

	cairo_fill(gtk_print_current_cr);
	cairo_stroke(gtk_print_current_cr);

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
nsgtk_print_plot_path(const struct redraw_context *ctx,
		const plot_style_t *pstyle,
		const float *p,
		unsigned int n,
		const float transform[6])
{
	/* Only the internal SVG renderer uses this plot call currently,
	 * and the GTK version uses librsvg.  Thus, we ignore this complexity,
	 * and just return true obliviously. */

	return NSERROR_OK;
}


static bool nsgtk_print_plot_pixbuf(int x, int y, int width, int height,
			      struct bitmap *bitmap, colour bg)
{
	int x0, y0, x1, y1;
	int dsrcx, dsrcy, dwidth, dheight;
	int bmwidth, bmheight;

	cairo_surface_t *bmsurface = bitmap->surface;

	/* Bail early if we can */
	if (width == 0 || height == 0)
		/* Nothing to plot */
		return true;
	if ((x > (cliprect.x + cliprect.width)) ||
			((x + width) < cliprect.x) ||
			(y > (cliprect.y + cliprect.height)) ||
			((y + height) < cliprect.y)) {
		/* Image completely outside clip region */
		return true;	
	}

	/* Get clip rectangle / image rectangle edge differences */
	x0 = cliprect.x - x;
	y0 = cliprect.y - y;
	x1 = (x + width)  - (cliprect.x + cliprect.width);
	y1 = (y + height) - (cliprect.y + cliprect.height);

	/* Set initial draw geometry */
	dsrcx = x;
	dsrcy = y;
	dwidth = width;
	dheight = height;

	/* Manually clip draw coordinates to area of image to be rendered */
	if (x0 > 0) {
		/* Clip left */
		dsrcx += x0;
		dwidth -= x0;
	}
	if (y0 > 0) {
		/* Clip top */
		dsrcy += y0;
		dheight -= y0;
	}
	if (x1 > 0) {
		/* Clip right */
		dwidth -= x1;
	}
	if (y1 > 0) {
		/* Clip bottom */
		dheight -= y1;
	}

	if (dwidth == 0 || dheight == 0)
		/* Nothing to plot */
		return true;

	bmwidth = cairo_image_surface_get_width(bmsurface);
	bmheight = cairo_image_surface_get_height(bmsurface);

	/* Render the bitmap */
	if ((bmwidth == width) && (bmheight == height)) {
		/* Bitmap is not scaled */
		/* Plot the bitmap */
		cairo_set_source_surface(gtk_print_current_cr, bmsurface, x, y);
		cairo_rectangle(gtk_print_current_cr, dsrcx, dsrcy, dwidth, dheight);
		cairo_fill(gtk_print_current_cr);

	} else {
		/* Bitmap is scaled */
		if ((bitmap->scsurface != NULL) && 
		    ((cairo_image_surface_get_width(bitmap->scsurface) != width) || 
		     (cairo_image_surface_get_height(bitmap->scsurface) != height))){
			cairo_surface_destroy(bitmap->scsurface);
			bitmap->scsurface = NULL;
		} 

		if (bitmap->scsurface == NULL) {
			bitmap->scsurface = cairo_surface_create_similar(bmsurface,CAIRO_CONTENT_COLOR_ALPHA, width, height);
			cairo_t *cr = cairo_create(bitmap->scsurface);

			/* Scale *before* setting the source surface (1) */
			cairo_scale(cr, (double)width / bmwidth, (double)height / bmheight);
			cairo_set_source_surface(cr, bmsurface, 0, 0);

			/* To avoid getting the edge pixels blended with 0
			 * alpha, which would occur with the default
			 * EXTEND_NONE. Use EXTEND_PAD for 1.2 or newer (2)
			 */
			cairo_pattern_set_extend(cairo_get_source(cr), 
						 CAIRO_EXTEND_REFLECT); 

			/* Replace the destination with the source instead of
			 * overlaying 
			 */
			cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

			/* Do the actual drawing */
			cairo_paint(cr);
   
			cairo_destroy(cr);

		}
		/* Plot the scaled bitmap */
		cairo_set_source_surface(gtk_print_current_cr, bitmap->scsurface, x, y);
		cairo_rectangle(gtk_print_current_cr, dsrcx, dsrcy, dwidth, dheight);
		cairo_fill(gtk_print_current_cr);
   
	}

	return true;
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
nsgtk_print_plot_bitmap(const struct redraw_context *ctx,
		  struct bitmap *bitmap,
		  int x, int y,
		  int width, int height,
		  colour bg,
		  bitmap_flags_t flags)
{
	int doneheight = 0, donewidth = 0;
	bool repeat_x = (flags & BITMAPF_REPEAT_X);
	bool repeat_y = (flags & BITMAPF_REPEAT_Y);

	if (!(repeat_x || repeat_y)) {
		/* Not repeating at all, so just pass it on */
		return nsgtk_print_plot_pixbuf(x, y, width, height, bitmap, bg);
	}

	width = nsgtk_bitmap_get_width(bitmap);
	height = nsgtk_bitmap_get_height(bitmap);

	/* Bail early if we can */
	if (width == 0 || height == 0)
		/* Nothing to plot */
		return true;

	if (y > cliprect.y) {
		doneheight = (cliprect.y - height) + ((y - cliprect.y) % height);
	} else {
		doneheight = y;
	}

	while (doneheight < (cliprect.y + cliprect.height)) {
		if (x > cliprect.x) {
			donewidth = (cliprect.x - width) + ((x - cliprect.x) % width);
		} else {
			donewidth = x;
		}

		while (donewidth < (cliprect.x + cliprect.width)) {
			nsgtk_print_plot_pixbuf(donewidth, doneheight,
					  width, height, bitmap, bg);
			donewidth += width;
			if (!repeat_x) 
				break;
		}
		doneheight += height;

		if (!repeat_y) 
			break;
	}

	return true;
}


static nserror
nsgtk_print_plot_text(const struct redraw_context *ctx,
		const struct plot_font_style *fstyle,
		int x,
		int y,
		const char *text,
		size_t length)
{
	return gtk_print_font_paint(x, y, text, length, fstyle);
}


/** GTK print plotter table */
static const struct plotter_table nsgtk_print_plotters = {
	.clip = nsgtk_print_plot_clip,
	.arc = nsgtk_print_plot_arc,
	.disc = nsgtk_print_plot_disc,
	.line = nsgtk_print_plot_line,
	.rectangle = nsgtk_print_plot_rectangle,
	.polygon = nsgtk_print_plot_polygon,
	.path = nsgtk_print_plot_path,
	.bitmap = nsgtk_print_plot_bitmap,
	.text = nsgtk_print_plot_text,
	.option_knockout = false,
};

static bool gtk_print_begin(struct print_settings* settings)
{
	return true;
}

static bool gtk_print_next_page(void)
{
	return true;
}

static void gtk_print_end(void)
{
}

static const struct printer gtk_printer = {
	&nsgtk_print_plotters,
	gtk_print_begin,
	gtk_print_next_page,
	gtk_print_end
};

/** 
 * Handle the begin_print signal from the GtkPrintOperation
 *
 * \param operation the operation which emited the signal
 * \param context the print context used to set up the pages
 * \param user_data nothing in here
 */
void gtk_print_signal_begin_print (GtkPrintOperation *operation,
		GtkPrintContext *context, gpointer user_data)
{
	int page_number;	
	double height_on_page, height_to_print;
	
	NSLOG(netsurf, INFO, "Begin print");
	
	settings = user_data;
		
	settings->margins[MARGINTOP] = 0;
	settings->margins[MARGINLEFT] = 0;
	settings->margins[MARGINBOTTOM] = 0;
	settings->margins[MARGINRIGHT] = 0;
	settings->page_width = gtk_print_context_get_width(context);
	settings->page_height = gtk_print_context_get_height(context);
	settings->scale = 0.7; /* at 0.7 the pages look the best */
	settings->font_func = nsgtk_layout_table;
	
	if (print_set_up(content_to_print, &gtk_printer, 
			 settings, &height_to_print) == false) {
		gtk_print_operation_cancel(operation);
		
	} else {

		NSLOG(netsurf, INFO,
		      "page_width: %f ;page_height: %f; content height: %lf",
		      settings->page_width,
		      settings->page_height,
		      height_to_print);
	
		height_on_page = settings->page_height;
		height_on_page = height_on_page - 
			FIXTOFLT(FSUB(settings->margins[MARGINTOP],
				      settings->margins[MARGINBOTTOM]));
		height_to_print *= settings->scale;
	
		page_number = height_to_print / height_on_page;

		if (height_to_print - page_number * height_on_page > 0)
			page_number += 1;
				
		gtk_print_operation_set_n_pages(operation, page_number);
	}
}

/** 
 * Handle the draw_page signal from the GtkPrintOperation.
 * This function changes only the cairo context to print on.
 */  		
void gtk_print_signal_draw_page(GtkPrintOperation *operation,
		GtkPrintContext *context, gint page_nr, gpointer user_data)
{
	NSLOG(netsurf, INFO, "Draw Page");
	gtk_print_current_cr = gtk_print_context_get_cairo_context(context);
	print_draw_next_page(&gtk_printer, settings);
}

/** 
 * Handle the end_print signal from the GtkPrintOperation.
 * This functions calls only the print_cleanup function from the print interface
 */  	
void gtk_print_signal_end_print(GtkPrintOperation *operation,
		GtkPrintContext *context, gpointer user_data)
{
	NSLOG(netsurf, INFO, "End print");	
	print_cleanup(content_to_print, &gtk_printer, user_data);
}


