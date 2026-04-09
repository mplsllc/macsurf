/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * plotters.c — All plotter_table callbacks
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 */

#include <stdlib.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "netsurf/types.h"
#include "netsurf/plot_style.h"
#include "netsurf/plotters.h"

#include "macos9/macos9.h"

static nserror
macos9_plot_clip(const struct redraw_context *ctx, const struct rect *clip)
{
	/* TODO: ClipRect() */
	return NSERROR_OK;
}

static nserror
macos9_plot_arc(const struct redraw_context *ctx,
		const plot_style_t *pstyle,
		int x, int y, int radius, int angle1, int angle2)
{
	/* TODO: FrameArc() */
	return NSERROR_OK;
}

static nserror
macos9_plot_disc(const struct redraw_context *ctx,
		 const plot_style_t *pstyle,
		 int x, int y, int radius)
{
	/* TODO: PaintOval() / FrameOval() */
	return NSERROR_OK;
}

static nserror
macos9_plot_line(const struct redraw_context *ctx,
		 const plot_style_t *pstyle,
		 const struct rect *line)
{
	/* TODO: MoveTo() + LineTo() */
	return NSERROR_OK;
}

static nserror
macos9_plot_rectangle(const struct redraw_context *ctx,
		      const plot_style_t *pstyle,
		      const struct rect *rectangle)
{
	/* TODO: PaintRect() / FrameRect() */
	return NSERROR_OK;
}

static nserror
macos9_plot_polygon(const struct redraw_context *ctx,
		    const plot_style_t *pstyle,
		    const int *p,
		    unsigned int n)
{
	/* TODO: OpenPoly() + LineTo() + ClosePoly() + PaintPoly() */
	return NSERROR_OK;
}

static nserror
macos9_plot_path(const struct redraw_context *ctx,
		 const plot_style_t *pstyle,
		 const float *p,
		 unsigned int n,
		 const float transform[6])
{
	/* TODO: bezier flattening via de Casteljau subdivision */
	return NSERROR_OK;
}

static nserror
macos9_plot_bitmap(const struct redraw_context *ctx,
		   struct bitmap *bitmap,
		   int x, int y,
		   int width, int height,
		   colour bg,
		   bitmap_flags_t flags)
{
	/* TODO: CopyBits() from GWorld, manual alpha compositing */
	return NSERROR_OK;
}

static nserror
macos9_plot_text(const struct redraw_context *ctx,
		 const plot_font_style_t *fstyle,
		 int x, int y,
		 const char *text,
		 size_t length)
{
	/* TODO: TextFont() + TextSize() + DrawText() */
	return NSERROR_OK;
}

/* Field order: clip, arc, disc, line, rectangle, polygon, path,
 * bitmap, text, group_start, group_end, flush, option_knockout
 * (see include/netsurf/plotters.h) */
const struct plotter_table macos9_plotters = {
	macos9_plot_clip,
	macos9_plot_arc,
	macos9_plot_disc,
	macos9_plot_line,
	macos9_plot_rectangle,
	macos9_plot_polygon,
	macos9_plot_path,
	macos9_plot_bitmap,
	macos9_plot_text,
	NULL,				/* group_start */
	NULL,				/* group_end */
	NULL,				/* flush */
	true				/* option_knockout */
};
