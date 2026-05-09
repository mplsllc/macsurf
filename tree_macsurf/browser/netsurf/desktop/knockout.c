/*
 * Copyright 2006 Richard Wilson <info@tinct.net>
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
 * Knockout rendering implementation.
 *
 * Knockout rendering is an optimisation which is particularly for
 * unaccelerated screen redraw. It tries to avoid plotting the same area more
 * than once.
 *
 * If the object is to plot two overlapping rectangles (one large, one small),
 * such as:
 *
 *   +-----------------+
 *   |#################|
 *   |####+-------+####|
 *   |####|:::::::|####|
 *   |####|:::::::|####|
 *   |####|:::::::|####|
 *   |####+-------+####|
 *   |#################|
 *   +-----------------+
 *
 * Without knockout rendering we plot the bottom rectangle and then the top one:
 *
 *   +-----------------+                 +-----------------+
 *   |#################|                 |#################|
 *   |#################|                 |####+-------+####|
 *   |#################|                 |####|:::::::|####|
 *   |#################|    and then,    |####|:::::::|####|
 *   |#################|                 |####|:::::::|####|
 *   |#################|                 |####+-------+####|
 *   |#################|                 |#################|
 *   +-----------------+                 +-----------------+
 *
 * With knockout rendering, the bottom rectangle is split up into smaller
 * ones and each pixel is just plotted once:
 *
 *   +-----------------+
 *   |#################|
 *   +----+-------+----+
 *   |####|:::::::|####|
 *   |####|:::::::|####|
 *   |####|:::::::|####|
 *   +----+-------+----+
 *   |#################|
 *   +-----------------+
 */

#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "utils/utils.h"
#include "utils/log.h"
#include "utils/errors.h"
#include "netsurf/bitmap.h"
#include "content/content.h"
#include "netsurf/plotters.h"

#include "desktop/gui_internal.h"
#include "desktop/knockout.h"

/* Define to enable knockout debug */
#undef KNOCKOUT_DEBUG

#define KNOCKOUT_ENTRIES 3072	/* 40 bytes each */
#define KNOCKOUT_BOXES 768	/* 28 bytes each */
#define KNOCKOUT_POLYGONS 3072	/* 4 bytes each */

struct knockout_box;
struct knockout_entry;

typedef enum {
	KNOCKOUT_PLOT_RECTANGLE,
	KNOCKOUT_PLOT_LINE,
	KNOCKOUT_PLOT_POLYGON,
	KNOCKOUT_PLOT_FILL,		/* knockout, knocked out */
	KNOCKOUT_PLOT_CLIP,
	KNOCKOUT_PLOT_TEXT,
	KNOCKOUT_PLOT_DISC,
	KNOCKOUT_PLOT_ARC,
	KNOCKOUT_PLOT_BITMAP,		/* knockout, knocked out */
	KNOCKOUT_PLOT_GROUP_START,
	KNOCKOUT_PLOT_GROUP_END,
} knockout_type;


struct knockout_box {
	struct rect bbox;
	bool deleted;			/* box has been deleted, ignore */
	struct knockout_box *child;
	struct knockout_box *next;
};


struct knockout_entry {
	knockout_type type;
	struct knockout_box *box;	/* relating series of knockout clips */
	union {
		struct {
			struct rect r;
			plot_style_t plot_style;
		} rectangle;
		struct {
			struct rect l;
			plot_style_t plot_style;
		} line;
		struct {
			int *p;
			unsigned int n;
			plot_style_t plot_style;
		} polygon;
		struct {
			struct rect r;
			plot_style_t plot_style;
		} fill;
		struct rect clip;
		struct {
			int x;
			int y;
			const char *text;
			size_t length;
			plot_font_style_t font_style;
		} text;
		struct {
			int x;
			int y;
			int radius;
			plot_style_t plot_style;
		} disc;
		struct {
			int x;
			int y;
			int radius;
			int angle1;
			int angle2;
			plot_style_t plot_style;
		} arc;
		struct {
			int x;
			int y;
			int width;
			int height;
			struct bitmap *bitmap;
			colour bg;
			bitmap_flags_t flags;
		} bitmap;
		struct {
			const char *name;
		} group_start;
	} data;
};


static struct knockout_entry knockout_entries[KNOCKOUT_ENTRIES];
static struct knockout_box knockout_boxes[KNOCKOUT_BOXES];
static int knockout_polygons[KNOCKOUT_POLYGONS];
static int knockout_entry_cur = 0;
static int knockout_box_cur = 0;
static int knockout_polygon_cur = 0;
static struct knockout_box *knockout_list = NULL;

static struct plotter_table real_plot;

static struct rect clip_cur;
static int nested_depth = 0;


/**
 * fill an area recursively
 */
static nserror
knockout_plot_fill_recursive(const struct redraw_context *ctx,
			     struct knockout_box *box,
			     plot_style_t *plot_style)
{
	struct knockout_box *parent;
	nserror res;
	nserror ffres = NSERROR_OK; /* first failing result */

	for (parent = box; parent; parent = parent->next) {
		if (parent->deleted)
			continue;
		if (parent->child) {
			res = knockout_plot_fill_recursive(ctx,
							   parent->child,
							   plot_style);
		} else {
			res = real_plot.rectangle(ctx, plot_style, &parent->bbox);
		}
		/* remember the first error */
		if ((res != NSERROR_OK) && (ffres == NSERROR_OK)) {
			ffres = res;
		}
	}
	return ffres;
}


/**
 * bitmap plot recusivley
 */
static nserror
knockout_plot_bitmap_recursive(const struct redraw_context *ctx,
			       struct knockout_box *box,
			       struct knockout_entry *entry)
{
	nserror res;
	nserror ffres = NSERROR_OK; /* first failing result */
	struct knockout_box *parent;

	for (parent = box; parent; parent = parent->next) {
		if (parent->deleted)
			continue;
		if (parent->child) {
			res = knockout_plot_bitmap_recursive(ctx,
							     parent->child,
							     entry);
		} else {
			real_plot.clip(ctx, &parent->bbox);
			res = real_plot.bitmap(ctx,
					       entry->data.bitmap.bitmap,
					       entry->data.bitmap.x,
					       entry->data.bitmap.y,
					       entry->data.bitmap.width,
					       entry->data.bitmap.height,
					       entry->data.bitmap.bg,
					       entry->data.bitmap.flags);
		}
		/* remember the first error */
		if ((res != NSERROR_OK) && (ffres == NSERROR_OK)) {
			ffres = res;
		}

	}
	return ffres;
}

/**
 * Flush the current knockout session to empty the buffers
 *
 * \return  true on success, false otherwise
 */
static nserror knockout_plot_flush(const struct redraw_context *ctx)
{
	int i;
	struct knockout_box *box;
	nserror res = NSERROR_OK; /* operation result */
	nserror ffres = NSERROR_OK; /* first failing result */

	/* debugging information */
#ifdef KNOCKOUT_DEBUG
	NSLOG(netsurf, INFO, "Entries are %i/%i, %i/%i, %i/%i",
	      knockout_entry_cur, KNOCKOUT_ENTRIES, knockout_box_cur,
	      KNOCKOUT_BOXES, knockout_polygon_cur, KNOCKOUT_POLYGONS);
#endif

	for (i = 0; i < knockout_entry_cur; i++) {
		switch (knockout_entries[i].type) {
		case KNOCKOUT_PLOT_RECTANGLE:
			res = real_plot.rectangle(ctx,
				&knockout_entries[i].data.rectangle.plot_style,
				&knockout_entries[i].data.rectangle.r);
			break;

		case KNOCKOUT_PLOT_LINE:
			res = real_plot.line(ctx,
				&knockout_entries[i].data.line.plot_style,
				&knockout_entries[i].data.line.l);
			break;

		case KNOCKOUT_PLOT_POLYGON:
			res = real_plot.polygon(ctx,
				&knockout_entries[i].data.polygon.plot_style,
				knockout_entries[i].data.polygon.p,
				knockout_entries[i].data.polygon.n);
			break;

		case KNOCKOUT_PLOT_FILL:
			box = knockout_entries[i].box->child;
			if (box) {
				res = knockout_plot_fill_recursive(ctx,
								   box,
				      &knockout_entries[i].data.fill.plot_style);
			} else if (!knockout_entries[i].box->deleted) {
				res = real_plot.rectangle(ctx,
				       &knockout_entries[i].data.fill.plot_style,
				       &knockout_entries[i].data.fill.r);
			}
			break;

		case KNOCKOUT_PLOT_CLIP:
			res = real_plot.clip(ctx, &knockout_entries[i].data.clip);
			break;

		case KNOCKOUT_PLOT_TEXT:
			res = real_plot.text(ctx,
					&knockout_entries[i].data.text.font_style,
					knockout_entries[i].data.text.x,
					knockout_entries[i].data.text.y,
					knockout_entries[i].data.text.text,
					knockout_entries[i].data.text.length);
			break;

		case KNOCKOUT_PLOT_DISC:
			res = real_plot.disc(ctx,
					&knockout_entries[i].data.disc.plot_style,
					knockout_entries[i].data.disc.x,
					knockout_entries[i].data.disc.y,
					knockout_entries[i].data.disc.radius);
			break;

		case KNOCKOUT_PLOT_ARC:
			res = real_plot.arc(ctx,
					&knockout_entries[i].data.arc.plot_style,
					knockout_entries[i].data.arc.x,
					knockout_entries[i].data.arc.y,
					knockout_entries[i].data.arc.radius,
					knockout_entries[i].data.arc.angle1,
					knockout_entries[i].data.arc.angle2);
			break;

		case KNOCKOUT_PLOT_BITMAP:
			box = knockout_entries[i].box->child;
			if (box) {
				res = knockout_plot_bitmap_recursive(ctx,
						box,
						&knockout_entries[i]);
			} else if (!knockout_entries[i].box->deleted) {
				res = real_plot.bitmap(ctx,
					knockout_entries[i].data.bitmap.bitmap,
					knockout_entries[i].data.bitmap.x,
					knockout_entries[i].data.bitmap.y,
					knockout_entries[i].data.bitmap.width,
					knockout_entries[i].data.bitmap.height,
					knockout_entries[i].data.bitmap.bg,
					knockout_entries[i].data.bitmap.flags);
			}
			break;

		case KNOCKOUT_PLOT_GROUP_START:
			res = real_plot.group_start(ctx,
				knockout_entries[i].data.group_start.name);
			break;

		case KNOCKOUT_PLOT_GROUP_END:
			res = real_plot.group_end(ctx);
			break;
		}

		/* remember the first error */
		if ((res != NSERROR_OK) && (ffres == NSERROR_OK)) {
			ffres = res;
		}
	}

	knockout_entry_cur = 0;
	knockout_box_cur = 0;
	knockout_polygon_cur = 0;
	knockout_list = NULL;

	return ffres;
}


/**
 * Knockout a section of previous rendering
 *
 * \param ctx The current redraw context.
 * \param x0    The left edge of the removal box
 * \param y0    The bottom edge of the removal box
 * \param x1    The right edge of the removal box
 * \param y1    The top edge of the removal box
 * \param owner The parent box set to consider, or NULL for top level
 */
static void
knockout_calculate(const struct redraw_context *ctx,
		   int x0, int y0, int x1, int y1,
		   struct knockout_box *owner)
{
	struct knockout_box *box;
	struct knockout_box *parent;
	struct knockout_box *prev = NULL;
	int nx0, ny0, nx1, ny1;

	if (owner == NULL)
		box = knockout_list;
	else
		box = owner->child;

	for (parent = box; parent; parent = parent->next) {
		/* permanently delink deleted nodes */
		if (parent->deleted) {
			if (prev) {
				/* not the first valid element: just skip future */
				prev->next = parent->next;
			} else {
				if (owner) {
					/* first valid element: update child reference */
					owner->child = parent->next;
					/* have we deleted all child nodes? */
					if (!owner->child)
						owner->deleted = true;
				} else {
					/* we are the head of the list */
					knockout_list = parent->next;
				}
			}
			continue;
		} else {
			prev = parent;
		}

		/* get the parent dimensions */
		nx0 = parent->bbox.x0;
		ny0 = parent->bbox.y0;
		nx1 = parent->bbox.x1;
		ny1 = parent->bbox.y1;

		/* reject non-overlapping boxes */
		if ((nx0 >= x1) || (nx1 <= x0) || (ny0 >= y1) || (ny1 <= y0))
			continue;

		/* check for a total knockout */
		if ((x0 <= nx0) && (x1 >= nx1) && (y0 <= ny0) && (y1 >= ny1)) {
			parent->deleted = true;
			continue;
		}

		/* has the box been replaced by children? */
		if (parent->child) {
			knockout_calculate(ctx, x0, y0, x1, y1, parent);
		} else {
			/* we need a maximum of 4 child boxes */
			if (knockout_box_cur + 4 >= KNOCKOUT_BOXES) {
				knockout_plot_flush(ctx);
				return;
			}

			/* clip top */
			if (y1 < ny1) {
				knockout_boxes[knockout_box_cur].bbox.x0 = nx0;
				knockout_boxes[knockout_box_cur].bbox.y0 = y1;
				knockout_boxes[knockout_box_cur].bbox.x1 = nx1;
				knockout_boxes[knockout_box_cur].bbox.y1 = ny1;
				knockout_boxes[knockout_box_cur].deleted = false;
				knockout_boxes[knockout_box_cur].child = NULL;
				knockout_boxes[knockout_box_cur].next = parent->child;
				parent->child = &knockout_boxes[knockout_box_cur++];
				ny1 = y1;
			}
			/* clip bottom */
			if (y0 > ny0) {
				knockout_boxes[knockout_box_cur].bbox.x0 = nx0;
				knockout_boxes[knockout_box_cur].bbox.y0 = ny0;
				knockout_boxes[knockout_box_cur].bbox.x1 = nx1;
				knockout_boxes[knockout_box_cur].bbox.y1 = y0;
				knockout_boxes[knockout_box_cur].deleted = false;
				knockout_boxes[knockout_box_cur].child = NULL;
				knockout_boxes[knockout_box_cur].next = parent->child;
				parent->child = &knockout_boxes[knockout_box_cur++];
				ny0 = y0;
			}
			/* clip right */
			if (x1 < nx1) {
				knockout_boxes[knockout_box_cur].bbox.x0 = x1;
				knockout_boxes[knockout_box_cur].bbox.y0 = ny0;
				knockout_boxes[knockout_box_cur].bbox.x1 = nx1;
				knockout_boxes[knockout_box_cur].bbox.y1 = ny1;
				knockout_boxes[knockout_box_cur].deleted = false;
				knockout_boxes[knockout_box_cur].child = NULL;
				knockout_boxes[knockout_box_cur].next = parent->child;
				parent->child = &knockout_boxes[knockout_box_cur++];
				/* nx1 isn't used again, but if it was it would
				 * need to be updated to x1 here. */
			}
			/* clip left */
			if (x0 > nx0) {
				knockout_boxes[knockout_box_cur].bbox.x0 = nx0;
				knockout_boxes[knockout_box_cur].bbox.y0 = ny0;
				knockout_boxes[knockout_box_cur].bbox.x1 = x0;
				knockout_boxes[knockout_box_cur].bbox.y1 = ny1;
				knockout_boxes[knockout_box_cur].deleted = false;
				knockout_boxes[knockout_box_cur].child = NULL;
				knockout_boxes[knockout_box_cur].next = parent->child;
				parent->child = &knockout_boxes[knockout_box_cur++];
				/* nx0 isn't used again, but if it was it would
				 * need to be updated to x0 here. */
			}
		}
	}
}


/**
 * knockout rectangle plotting.
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
knockout_plot_rectangle(const struct redraw_context *ctx,
			const plot_style_t *pstyle,
			const struct rect *rect)
{
	int kx0, ky0, kx1, ky1;
	nserror res = NSERROR_OK;

	if (pstyle->fill_type != PLOT_OP_TYPE_NONE) {
		/* filled draw */

		/* get our bounds */
		kx0 = (rect->x0 > clip_cur.x0) ? rect->x0 : clip_cur.x0;
		ky0 = (rect->y0 > clip_cur.y0) ? rect->y0 : clip_cur.y0;
		kx1 = (rect->x1 < clip_cur.x1) ? rect->x1 : clip_cur.x1;
		ky1 = (rect->y1 < clip_cur.y1) ? rect->y1 : clip_cur.y1;
		if ((kx0 > clip_cur.x1) || (kx1 < clip_cur.x0) ||
		    (ky0 > clip_cur.y1) || (ky1 < clip_cur.y0)) {
			return NSERROR_OK;
		}

		/* fills both knock out and get knocked out */
		knockout_calculate(ctx, kx0, ky0, kx1, ky1, NULL);
		knockout_boxes[knockout_box_cur].bbox = *rect;
		knockout_boxes[knockout_box_cur].deleted = false;
		knockout_boxes[knockout_box_cur].child = NULL;
		knockout_boxes[knockout_box_cur].next = knockout_list;
		knockout_list = &knockout_boxes[knockout_box_cur];
		knockout_entries[knockout_entry_cur].box = &knockout_boxes[knockout_box_cur];
		knockout_entries[knockout_entry_cur].data.fill.r = *rect;
		knockout_entries[knockout_entry_cur].data.fill.plot_style = *pstyle;
		knockout_entries[knockout_entry_cur].data.fill.plot_style.stroke_type = PLOT_OP_TYPE_NONE; /* ensure we only plot the fill */
		knockout_entries[knockout_entry_cur].type = KNOCKOUT_PLOT_FILL;
		if ((++knockout_entry_cur >= KNOCKOUT_ENTRIES) ||
		    (++knockout_box_cur >= KNOCKOUT_BOXES)) {
			res = knockout_plot_flush(ctx);
		}
	}

	if (pstyle->stroke_type != PLOT_OP_TYPE_NONE) {
		/* draw outline */

		knockout_entries[knockout_entry_cur].data.rectangle.r = *rect;
		knockout_entries[knockout_entry_cur].data.fill.plot_style = *pstyle;
		knockout_entries[knockout_entry_cur].data.fill.plot_style.fill_type = PLOT_OP_TYPE_NONE; /* ensure we only plot the outline */
		knockout_entries[knockout_entry_cur].type = KNOCKOUT_PLOT_RECTANGLE;
		if (++knockout_entry_cur >= KNOCKOUT_ENTRIES) {
			res = knockout_plot_flush(ctx);
		}
	}
	return res;
}


/**
 * Knockout line plotting.
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
knockout_plot_line(const struct redraw_context *ctx,
		   const plot_style_t *pstyle,
		   const struct rect *line)
{
	knockout_entries[knockout_entry_cur].data.line.l = *line;
	knockout_entries[knockout_entry_cur].data.line.plot_style = *pstyle;
	knockout_entries[knockout_entry_cur].type = KNOCKOUT_PLOT_LINE;
	if (++knockout_entry_cur >= KNOCKOUT_ENTRIES) {
		return knockout_plot_flush(ctx);
	}
	return NSERROR_OK;
}


/**
 * Knockout polygon plotting.
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
knockout_plot_polygon(const struct redraw_context *ctx,
		      const plot_style_t *pstyle,
		      const int *p,
		      unsigned int n)
{
	int *dest;
	nserror res = NSERROR_OK;
	nserror ffres = NSERROR_OK;

	/* ensure we have sufficient room even when flushed */
	if (n * 2 >= KNOCKOUT_POLYGONS) {
		ffres = knockout_plot_flush(ctx);
		res = real_plot.polygon(ctx, pstyle, p, n);
		/* return the first error */
		if ((res != NSERROR_OK) && (ffres == NSERROR_OK)) {
			ffres = res;
		}
		return ffres;
	}

	/* ensure we have enough room right now */
	if (knockout_polygon_cur + n * 2 >= KNOCKOUT_POLYGONS) {
		ffres = knockout_plot_flush(ctx);
	}

	/* copy our data */
	dest = &(knockout_polygons[knockout_polygon_cur]);
	memcpy(dest, p, n * 2 * sizeof(int));
	knockout_polygon_cur += n * 2;
	knockout_entries[knockout_entry_cur].data.polygon.p = dest;
	knockout_entries[knockout_entry_cur].data.polygon.n = n;
	knockout_entries[knockout_entry_cur].data.polygon.plot_style = *pstyle;
	knockout_entries[knockout_entry_cur].type = KNOCKOUT_PLOT_POLYGON;
	if (++knockout_entry_cur >= KNOCKOUT_ENTRIES) {
		res = knockout_plot_flush(ctx);
	}
	/* return the first error */
	if ((res != NSERROR_OK) && (ffres == NSERROR_OK)) {
		ffres = res;
	}
	return ffres;
}


/**
 * knockout path plotting.
 *
 * The knockout implementation simply flushes the queue and plots the path
 *  directly using real plotter.
 *
 * \param ctx The current redraw context.
 * \param pstyle Style controlling the path plot.
 * \param p elements of path
 * \param n nunber of elements on path
 * \param transform A transform to apply to the path.
 * \return NSERROR_OK on success else error code.
 */
static nserror
knockout_plot_path(const struct redraw_context *ctx,
		   const plot_style_t *pstyle,
		   const float *p,
		   unsigned int n,
		   const float transform[6])
{
	nserror res;
	nserror ffres;

	ffres = knockout_plot_flush(ctx);
	res = real_plot.path(ctx, pstyle, p, n, transform);

	/* return the first error */
	if ((res != NSERROR_OK) && (ffres == NSERROR_OK)) {
		ffres = res;
	}
	return ffres;
}


static nserror
knockout_plot_clip(const struct redraw_context *ctx, const struct rect *clip)
{
	nserror res = NSERROR_OK;

	if (clip->x1 < clip->x0 || clip->y0 > clip->y1) {
#ifdef KNOCKOUT_DEBUG
		NSLOG(netsurf, INFO, "bad clip rectangle %i %i %i %i",
		      clip->x0, clip->y0, clip->x1, clip->y1);
#endif
		return NSERROR_BAD_SIZE;
	}

	/* memorise clip for bitmap tiling */
	clip_cur = *clip;

	knockout_entries[knockout_entry_cur].data.clip = *clip;
	knockout_entries[knockout_entry_cur].type = KNOCKOUT_PLOT_CLIP;
	if (++knockout_entry_cur >= KNOCKOUT_ENTRIES) {
		res = knockout_plot_flush(ctx);
	}
	return res;
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
knockout_plot_text(const struct redraw_context *ctx,
		   const plot_font_style_t *fstyle,
		   int x,
		   int y,
		   const char *text,
		   size_t length)
{
	nserror res = NSERROR_OK;

	knockout_entries[knockout_entry_cur].data.text.x = x;
	knockout_entries[knockout_entry_cur].data.text.y = y;
	knockout_entries[knockout_entry_cur].data.text.text = text;
	knockout_entries[knockout_entry_cur].data.text.length = length;
	knockout_entries[knockout_entry_cur].data.text.font_style = *fstyle;
	knockout_entries[knockout_entry_cur].type = KNOCKOUT_PLOT_TEXT;
	if (++knockout_entry_cur >= KNOCKOUT_ENTRIES) {
		res = knockout_plot_flush(ctx);
	}
	return res;
}


/**
 * knockout circle plotting
 *
 * Plot a circle centered on (x,y), which is optionally filled.
 *
 * \param ctx The current redraw context.
 * \param pstyle Style controlling the circle plot.
 * \param x x coordinate of circle centre.
 * \param y y coordinate of circle centre.
 * \param radius circle radius.
 * \return NSERROR_OK on success else error code.
 */
static nserror
knockout_plot_disc(const struct redraw_context *ctx,
		   const plot_style_t *pstyle,
		   int x,
		   int y,
		   int radius)
{
	nserror res = NSERROR_OK;

	knockout_entries[knockout_entry_cur].data.disc.x = x;
	knockout_entries[knockout_entry_cur].data.disc.y = y;
	knockout_entries[knockout_entry_cur].data.disc.radius = radius;
	knockout_entries[knockout_entry_cur].data.disc.plot_style = *pstyle;
	knockout_entries[knockout_entry_cur].type = KNOCKOUT_PLOT_DISC;
	if (++knockout_entry_cur >= KNOCKOUT_ENTRIES) {
		res = knockout_plot_flush(ctx);
	}
	return res;
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
knockout_plot_arc(const struct redraw_context *ctx,
		  const plot_style_t *pstyle,
		  int x,
		  int y,
		  int radius,
		  int angle1,
		  int angle2)
{
	nserror res = NSERROR_OK;

	knockout_entries[knockout_entry_cur].data.arc.x = x;
	knockout_entries[knockout_entry_cur].data.arc.y = y;
	knockout_entries[knockout_entry_cur].data.arc.radius = radius;
	knockout_entries[knockout_entry_cur].data.arc.angle1 = angle1;
	knockout_entries[knockout_entry_cur].data.arc.angle2 = angle2;
	knockout_entries[knockout_entry_cur].data.arc.plot_style = *pstyle;
	knockout_entries[knockout_entry_cur].type = KNOCKOUT_PLOT_ARC;
	if (++knockout_entry_cur >= KNOCKOUT_ENTRIES) {
		res = knockout_plot_flush(ctx);
	}
	return res;
}


/**
 * knockout bitmap plotting.
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
knockout_plot_bitmap(const struct redraw_context *ctx,
		     struct bitmap *bitmap,
		     int x, int y,
		     int width, int height,
		     colour bg,
		     bitmap_flags_t flags)
{
	int kx0, ky0, kx1, ky1;
	nserror res;
	nserror ffres = NSERROR_OK;

	/* get our bounds */
	kx0 = clip_cur.x0;
	ky0 = clip_cur.y0;
	kx1 = clip_cur.x1;
	ky1 = clip_cur.y1;
	if (!(flags & BITMAPF_REPEAT_X)) {
		if (x > kx0)
			kx0 = x;
		if (x + width < kx1)
			kx1 = x + width;
		if ((kx0 > clip_cur.x1) || (kx1 < clip_cur.x0))
			return NSERROR_OK;
	}
	if (!(flags & BITMAPF_REPEAT_Y)) {
		if (y > ky0)
			ky0 = y;
		if (y + height < ky1)
			ky1 = y + height;
		if ((ky0 > clip_cur.y1) || (ky1 < clip_cur.y0))
			return NSERROR_OK;
	}

	/* tiled bitmaps both knock out and get knocked out */
	if (guit->bitmap->get_opaque(bitmap)) {
		knockout_calculate(ctx, kx0, ky0, kx1, ky1, NULL);
	}
	knockout_boxes[knockout_box_cur].bbox.x0 = kx0;
	knockout_boxes[knockout_box_cur].bbox.y0 = ky0;
	knockout_boxes[knockout_box_cur].bbox.x1 = kx1;
	knockout_boxes[knockout_box_cur].bbox.y1 = ky1;
	knockout_boxes[knockout_box_cur].deleted = false;
	knockout_boxes[knockout_box_cur].child = NULL;
	knockout_boxes[knockout_box_cur].next = knockout_list;
	knockout_list = &knockout_boxes[knockout_box_cur];
	knockout_entries[knockout_entry_cur].box = &knockout_boxes[knockout_box_cur];
	knockout_entries[knockout_entry_cur].data.bitmap.x = x;
	knockout_entries[knockout_entry_cur].data.bitmap.y = y;
	knockout_entries[knockout_entry_cur].data.bitmap.width = width;
	knockout_entries[knockout_entry_cur].data.bitmap.height = height;
	knockout_entries[knockout_entry_cur].data.bitmap.bitmap = bitmap;
	knockout_entries[knockout_entry_cur].data.bitmap.bg = bg;
	knockout_entries[knockout_entry_cur].data.bitmap.flags = flags;
	knockout_entries[knockout_entry_cur].type = KNOCKOUT_PLOT_BITMAP;

	if ((++knockout_entry_cur >= KNOCKOUT_ENTRIES) ||
	    (++knockout_box_cur >= KNOCKOUT_BOXES)) {
		ffres = knockout_plot_flush(ctx);
	}
	res = knockout_plot_clip(ctx, &clip_cur);
	/* return the first error */
	if ((res != NSERROR_OK) && (ffres == NSERROR_OK)) {
		ffres = res;
	}
	return ffres;
}


/**
 * Start of a group of objects.
 *
 * Used when plotter implements export to a vector graphics file format.
 *
 * \param ctx The current redraw context.
 * \param name The name of the group being started.
 * \return NSERROR_OK on success else error code.
 */
static nserror
knockout_plot_group_start(const struct redraw_context *ctx, const char *name)
{
	if (real_plot.group_start == NULL) {
		return NSERROR_OK;
	}

	knockout_entries[knockout_entry_cur].data.group_start.name = name;
	knockout_entries[knockout_entry_cur].type = KNOCKOUT_PLOT_GROUP_START;
	if (++knockout_entry_cur >= KNOCKOUT_ENTRIES) {
		return knockout_plot_flush(ctx);
	}
	return NSERROR_OK;
}


/**
 * End a group of objects.
 *
 * Used when plotter implements export to a vector graphics file format.
 *
 * \param ctx The current redraw context.
 * \return NSERROR_OK on success else error code.
 */
static nserror knockout_plot_group_end(const struct redraw_context *ctx)
{
	if (real_plot.group_end == NULL) {
		return NSERROR_OK;
	}

	knockout_entries[knockout_entry_cur].type = KNOCKOUT_PLOT_GROUP_END;
	if (++knockout_entry_cur >= KNOCKOUT_ENTRIES) {
		return knockout_plot_flush(ctx);
	}
	return NSERROR_OK;
}

/* exported functions documented in desktop/knockout.h */
bool knockout_plot_start(const struct redraw_context *ctx,
			 struct redraw_context *knk_ctx)
{
	/* check if we're recursing */
	if (nested_depth++ > 0) {
		/* we should already have the knockout renderer as default */
		assert(ctx->plot->rectangle == knockout_plotters.rectangle);
		*knk_ctx = *ctx;
		return true;
	}

	/* end any previous sessions */
	if (knockout_entry_cur > 0)
		knockout_plot_end(ctx);

	/* get copy of real plotter table */
	real_plot = *(ctx->plot);

	/* set up knockout rendering context */
	*knk_ctx = *ctx;
	knk_ctx->plot = &knockout_plotters;
	return true;
}


/* exported functions documented in desktop/knockout.h */
bool knockout_plot_end(const struct redraw_context *ctx)
{
	/* only output when we've finished any nesting */
	if (--nested_depth == 0) {
		return knockout_plot_flush(ctx);
	}

	assert(nested_depth > 0);
	return true;
}


/**
 * knockout plotter operation table
 */
const struct plotter_table knockout_plotters = {
	.rectangle = knockout_plot_rectangle,
	.line = knockout_plot_line,
	.polygon = knockout_plot_polygon,
	.clip = knockout_plot_clip,
	.text = knockout_plot_text,
	.disc = knockout_plot_disc,
	.arc = knockout_plot_arc,
	.bitmap = knockout_plot_bitmap,
	.group_start = knockout_plot_group_start,
	.group_end = knockout_plot_group_end,
	.flush = knockout_plot_flush,
	.path = knockout_plot_path,
	.option_knockout = true,
};
