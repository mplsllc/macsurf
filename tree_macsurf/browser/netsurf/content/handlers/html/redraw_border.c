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
 * Redrawing CONTENT_HTML borders implementation.
 */

#include <stdbool.h>
#include <stdlib.h>

#include "utils/utils.h"
#include "utils/log.h"
#include "netsurf/plotters.h"
#include "netsurf/css.h"

#include "html/box.h"
#include "html/private.h"


/* MacSurf: positional init for CW8 C89.
 * plot_style_t order: stroke_type, stroke_width, stroke_colour,
 *                     fill_type, fill_colour. */
static plot_style_t plot_style_bdr = {
	PLOT_OP_TYPE_DASH, 0, 0, PLOT_OP_TYPE_NONE, 0
};
static plot_style_t plot_style_fillbdr = {
	PLOT_OP_TYPE_NONE, 0, 0, PLOT_OP_TYPE_SOLID, 0
};
static plot_style_t plot_style_fillbdr_dark = {
	PLOT_OP_TYPE_NONE, 0, 0, PLOT_OP_TYPE_SOLID, 0
};
static plot_style_t plot_style_fillbdr_light = {
	PLOT_OP_TYPE_NONE, 0, 0, PLOT_OP_TYPE_SOLID, 0
};
static plot_style_t plot_style_fillbdr_ddark = {
	PLOT_OP_TYPE_NONE, 0, 0, PLOT_OP_TYPE_SOLID, 0
};
static plot_style_t plot_style_fillbdr_dlight = {
	PLOT_OP_TYPE_NONE, 0, 0, PLOT_OP_TYPE_SOLID, 0
};


static inline nserror
plot_clipped_rectangle(const struct redraw_context *ctx,
		       const plot_style_t *style,
		       const struct rect *clip,
		       struct rect *rect)
{
	nserror res;

	rect->x0 = (clip->x0 > rect->x0) ? clip->x0 : rect->x0;
	rect->y0 = (clip->y0 > rect->y0) ? clip->y0 : rect->y0;
	rect->x1 = (clip->x1 < rect->x1) ? clip->x1 : rect->x1;
	rect->y1 = (clip->y1 < rect->y1) ? clip->y1 : rect->y1;
	if ((rect->x0 < rect->x1) && (rect->y0 < rect->y1)) {
		/* valid clip rectangles only */
		res = ctx->plot->rectangle(ctx, style, rect);
	} else {
		res = NSERROR_OK;
	}
	return res;
}


/**
 * Draw one border.
 *
 * \param side index of border side (TOP, RIGHT, BOTTOM, LEFT)
 * \param p array of precomputed border vertices
 * \param c colour for border
 * \param style border line style
 * \param thickness border thickness
 * \param rectangular whether border is rectangular
 * \param clip cliping area for redrawing border.
 * \param ctx current redraw context
 * \return NSERROR_OK if successful otherwise appropriate error code
 */
static nserror
html_redraw_border_plot(const int side,
			const int *p,
			colour c,
			enum css_border_style_e style,
			int thickness,
			bool rectangular,
			const struct rect *clip,
			const struct redraw_context *ctx)
{
	int z[8]; /* Vertices of border part */
	unsigned int light = side;
	plot_style_t *plot_style_bdr_in;
	plot_style_t *plot_style_bdr_out;
	nserror res = NSERROR_OK;
	struct rect rect;

	if (c == NS_TRANSPARENT) {
		return res;
	}

	plot_style_bdr.stroke_type = PLOT_OP_TYPE_DASH;
	plot_style_bdr.stroke_colour = c;
	plot_style_bdr.stroke_width = (thickness << PLOT_STYLE_RADIX);
	plot_style_fillbdr.fill_colour = c;
	plot_style_fillbdr_dark.fill_colour = darken_colour(c);
	plot_style_fillbdr_light.fill_colour = lighten_colour(c);
	plot_style_fillbdr_ddark.fill_colour = double_darken_colour(c);
	plot_style_fillbdr_dlight.fill_colour = double_lighten_colour(c);

	switch (style) {
	case CSS_BORDER_STYLE_DOTTED:
		plot_style_bdr.stroke_type = PLOT_OP_TYPE_DOT;
		fallthrough;
	case CSS_BORDER_STYLE_DASHED:
		rect.x0 = (p[0] + p[2]) / 2;
		rect.y0 = (p[1] + p[3]) / 2;
		rect.x1 = (p[4] + p[6]) / 2;
		rect.y1 = (p[5] + p[7]) / 2;
		res = ctx->plot->line(ctx, &plot_style_bdr, &rect);
		break;

	case CSS_BORDER_STYLE_SOLID:
		/* solid is the default */
	default:
		if (rectangular || thickness == 1) {

			if (side == TOP || side == RIGHT) {
				rect.x0 = p[2];
				rect.y0 = p[3];
				if ((side == TOP) &&
				    (p[4] - p[6] != 0)) {
					rect.x1 = p[4];
				} else {
					rect.x1 = p[6];
				}
				rect.y1 = p[7];
			} else {
				rect.x0 = p[6];
				rect.y0 = p[7];
				rect.x1 = p[2];
				if ((side == LEFT) &&
				    (p[1] - p[3] != 0)) {
					rect.y1 = p[1];
				} else {
					rect.y1 = p[3];
				}
			}
			res = plot_clipped_rectangle(ctx,
						     &plot_style_fillbdr,
						     clip,
						     &rect);
		} else {
			res = ctx->plot->polygon(ctx, &plot_style_fillbdr, p, 4);
		}
		break;

	case CSS_BORDER_STYLE_DOUBLE:
		z[0] = p[0];
		z[1] = p[1];
		z[2] = (p[0] * 2 + p[2]) / 3;
		z[3] = (p[1] * 2 + p[3]) / 3;
		z[4] = (p[6] * 2 + p[4]) / 3;
		z[5] = (p[7] * 2 + p[5]) / 3;
		z[6] = p[6];
		z[7] = p[7];
		res = ctx->plot->polygon(ctx, &plot_style_fillbdr, z, 4);
		if (res == NSERROR_OK) {
			z[0] = p[2];
			z[1] = p[3];
			z[2] = (p[2] * 2 + p[0]) / 3;
			z[3] = (p[3] * 2 + p[1]) / 3;
			z[4] = (p[4] * 2 + p[6]) / 3;
			z[5] = (p[5] * 2 + p[7]) / 3;
			z[6] = p[4];
			z[7] = p[5];
			res = ctx->plot->polygon(ctx, &plot_style_fillbdr, z, 4);
		}
		break;

	case CSS_BORDER_STYLE_GROOVE:
		light = 3 - light;
		fallthrough;
	case CSS_BORDER_STYLE_RIDGE:
		/* choose correct colours for each part of the border line */
		if (light <= 1) {
			plot_style_bdr_in = &plot_style_fillbdr_dark;
			plot_style_bdr_out = &plot_style_fillbdr_light;
		} else {
			plot_style_bdr_in = &plot_style_fillbdr_light;
			plot_style_bdr_out = &plot_style_fillbdr_dark;
		}

		/* Render border */
		if ((rectangular || thickness == 2) && thickness != 1) {
			/* Border made up from two parts and can be
			 * plotted with rectangles
			 */

			/* First part */
			if (side == TOP || side == RIGHT) {
				rect.x0 = (p[0] + p[2]) / 2;
				rect.y0 = (p[1] + p[3]) / 2;
				rect.x1 = p[6];
				rect.y1 = p[7];
			} else {
				rect.x0 = p[6];
				rect.y0 = p[7];
				rect.x1 = (p[0] + p[2]) / 2;
				rect.y1 = (p[1] + p[3]) / 2;
			}
			res = plot_clipped_rectangle(ctx,
						     plot_style_bdr_in,
						     clip,
						     &rect);
			if (res != NSERROR_OK) {
				return res;
			}

			/* Second part */
			if (side == TOP || side == RIGHT) {
				rect.x0 = p[2];
				rect.y0 = p[3];
				rect.x1 = (p[6] + p[4]) / 2;
				rect.y1 = (p[7] + p[5]) / 2;
			} else {
				rect.x0 = (p[6] + p[4]) / 2;
				rect.y0 = (p[7] + p[5]) / 2;
				rect.x1 = p[2];
				rect.y1 = p[3];
			}
			res = plot_clipped_rectangle(ctx,
						     plot_style_bdr_out,
						     clip,
						     &rect);
		} else if (thickness == 1) {
			/* Border made up from one part which can be
			 * plotted as a rectangle
			 */

			if (side == TOP || side == RIGHT) {
				rect.x0 = p[2];
				rect.y0 = p[3];
				rect.x1 = p[6];
				rect.y1 = p[7];
				rect.x1 = ((side == TOP) && (p[4] - p[6] != 0)) ?
					rect.x1 + p[4] - p[6] : rect.x1;

				res = plot_clipped_rectangle(ctx,
							     plot_style_bdr_in,
							     clip,
							     &rect);
			} else {
				rect.x0 = p[6];
				rect.y0 = p[7];
				rect.x1 = p[2];
				rect.y1 = p[3];
				rect.y1 = ((side == LEFT) && (p[1] - p[3] != 0)) ?
					rect.y1 + p[1] - p[3] : rect.y1;
				res = plot_clipped_rectangle(ctx,
							     plot_style_bdr_out,
							     clip,
							     &rect);
			}
		} else {
			/* Border made up from two parts and can't be
			 * plotted with rectangles
			 */
			z[0] = p[0];
			z[1] = p[1];
			z[2] = (p[0] + p[2]) / 2;
			z[3] = (p[1] + p[3]) / 2;
			z[4] = (p[6] + p[4]) / 2;
			z[5] = (p[7] + p[5]) / 2;
			z[6] = p[6];
			z[7] = p[7];
			res = ctx->plot->polygon(ctx, plot_style_bdr_in, z, 4);
			if (res == NSERROR_OK) {
				z[0] = p[2];
				z[1] = p[3];
				z[6] = p[4];
				z[7] = p[5];
				res = ctx->plot->polygon(ctx,
							 plot_style_bdr_out,
							 z,
							 4);
			}
		}
		break;

	case CSS_BORDER_STYLE_INSET:
		light = (light + 2) % 4;
		fallthrough;
	case CSS_BORDER_STYLE_OUTSET:
		/* choose correct colours for each part of the border line */
		switch (light) {
		case 0:
			plot_style_bdr_in = &plot_style_fillbdr_light;
			plot_style_bdr_out = &plot_style_fillbdr_dlight;
			break;
		case 1:
			plot_style_bdr_in = &plot_style_fillbdr_ddark;
			plot_style_bdr_out = &plot_style_fillbdr_dark;
			break;
		case 2:
			plot_style_bdr_in = &plot_style_fillbdr_dark;
			plot_style_bdr_out = &plot_style_fillbdr_ddark;
			break;
		case 3:
			plot_style_bdr_in = &plot_style_fillbdr_dlight;
			plot_style_bdr_out = &plot_style_fillbdr_light;
			break;
		default:
			plot_style_bdr_in = &plot_style_fillbdr;
			plot_style_bdr_out = &plot_style_fillbdr;
			break;
		}

		/* Render border */
		if ((rectangular || thickness == 2) && thickness != 1) {
			/* Border made up from two parts and can be
			 * plotted with rectangles
			 */

			/* First part */
			if (side == TOP || side == RIGHT) {
				rect.x0 = (p[0] + p[2]) / 2;
				rect.y0 = (p[1] + p[3]) / 2;
				rect.x1 = p[6];
				rect.y1 = p[7];
			} else {
				rect.x0 = p[6];
				rect.y0 = p[7];
				rect.x1 = (p[0] + p[2]) / 2;
				rect.y1 = (p[1] + p[3]) / 2;
			}
			res = plot_clipped_rectangle(ctx,
						     plot_style_bdr_in,
						     clip,
						     &rect);
			if (res != NSERROR_OK) {
				return res;
			}

			/* Second part */
			if (side == TOP || side == RIGHT) {
				rect.x0 = p[2];
				rect.y0 = p[3];
				rect.x1 = (p[6] + p[4]) / 2;
				rect.y1 = (p[7] + p[5]) / 2;
			} else {
				rect.x0 = (p[6] + p[4]) / 2;
				rect.y0 = (p[7] + p[5]) / 2;
				rect.x1 = p[2];
				rect.y1 = p[3];
			}
			res = plot_clipped_rectangle(ctx,
						     plot_style_bdr_out,
						     clip,
						     &rect);
		} else if (thickness == 1) {
			/* Border made up from one part which can be
			 * plotted as a rectangle
			 */

			if (side == TOP || side == RIGHT) {
				rect.x0 = p[2];
				rect.y0 = p[3];
				rect.x1 = p[6];
				rect.y1 = p[7];
				rect.x1 = ((side == TOP) && (p[4] - p[6] != 0)) ?
					rect.x1 + p[4] - p[6] : rect.x1;
				res = plot_clipped_rectangle(ctx,
							     plot_style_bdr_in,
							     clip,
							     &rect);
			} else {
				rect.x0 = p[6];
				rect.y0 = p[7];
				rect.x1 = p[2];
				rect.y1 = p[3];
				rect.y1 = ((side == LEFT) && (p[1] - p[3] != 0)) ?
					rect.y1 + p[1] - p[3] : rect.y1;
				res = plot_clipped_rectangle(ctx,
							     plot_style_bdr_out,
							     clip,
							     &rect);
			}
		} else {
			/* Border made up from two parts and can't be
			 * plotted with rectangles
			 */

			z[0] = p[0];
			z[1] = p[1];
			z[2] = (p[0] + p[2]) / 2;
			z[3] = (p[1] + p[3]) / 2;
			z[4] = (p[6] + p[4]) / 2;
			z[5] = (p[7] + p[5]) / 2;
			z[6] = p[6];
			z[7] = p[7];
			res = ctx->plot->polygon(ctx, plot_style_bdr_in, z, 4);
			if (res != NSERROR_OK) {
				return res;
			}
			z[0] = p[2];
			z[1] = p[3];
			z[6] = p[4];
			z[7] = p[5];
			res = ctx->plot->polygon(ctx, plot_style_bdr_out, z, 4);
		}
		break;
	}

	return res;
}


/**
 * Draw borders for a box.
 *
 * \param box box to draw
 * \param x_parent coordinate of left padding edge of parent of box
 * \param y_parent coordinate of top padding edge of parent of box
 * \param p_width width of padding box
 * \param p_height height of padding box
 * \param clip cliping area for redrawing border.
 * \param scale scale for redraw
 * \param ctx current redraw context
 * \return true if successful, false otherwise
 */
bool
html_redraw_borders(struct box *box,
		    int x_parent,
		    int y_parent,
		    int p_width,
		    int p_height,
		    const struct rect *clip,
		    float scale,
		    const struct redraw_context *ctx)
{
	unsigned int sides[] = { LEFT, RIGHT, TOP, BOTTOM };
	int top = box->border[TOP].width;
	int right = box->border[RIGHT].width;
	int bottom = box->border[BOTTOM].width;
	int left = box->border[LEFT].width;
	int x, y;
	unsigned int i, side;
	int p[8]; /* Box border vertices */
	int z[8]; /* Border vertices */
	bool square_end_1 = false;
        bool square_end_2 = false;
        nserror res;
        css_fixed br_len = 0;
        css_unit br_unit = CSS_UNIT_PX;

        /* MacSurf: If border_radius is present, html_redraw_background handles the border frame. */
        if (box && box->style && css_computed_border_radius(box->style, &br_len, &br_unit) == CSS_BORDER_RADIUS_SET && br_len > 0) {
                return true;
        }

	x = x_parent + box->x;
	y = y_parent + box->y;

	if (scale != 1.0) {
		top *= scale;
		right *= scale;
		bottom *= scale;
		left *= scale;
		x *= scale;
		y *= scale;
	}

	assert(box->style);

	/* Calculate border vertices
	 *
	 *    A----------------------+
	 *    | \                  / |
	 *    |   B--------------+   |
	 *    |   |              |   |
	 *    |   +--------------C   |
	 *    | /                  \ |
	 *    +----------------------D
	 */
	p[0] = x - left;		p[1] = y - top;			/* A */
	p[2] = x;			p[3] = y;			/* B */
	p[4] = x + p_width;		p[5] = y + p_height;		/* C */
	p[6] = x + p_width + right;	p[7] = y + p_height + bottom;	/* D */

	for (i = 0; i != 4; i++) {
		colour col = 0;
		side = sides[i]; /* plot order */

		if (box->border[side].width == 0 ||
		    nscss_color_is_transparent(box->border[side].c)) {
			continue;
		}

		switch (side) {
		case LEFT:
			square_end_1 = (top == 0);
			square_end_2 = (bottom == 0);

			z[0] = p[0];	z[1] = p[7];
			z[2] = p[2];	z[3] = p[5];
			z[4] = p[2];	z[5] = p[3];
			z[6] = p[0];	z[7] = p[1];

			if (nscss_color_is_transparent(box->border[TOP].c) == false &&
			    box->border[TOP].style != CSS_BORDER_STYLE_DOUBLE) {
				/* make border overhang top corner fully,
				 * if top border is opaque
				 */
				z[5] -= top;
				square_end_1 = true;
			}
			if (nscss_color_is_transparent(box->border[BOTTOM].c) == false &&
			    box->border[BOTTOM].style != CSS_BORDER_STYLE_DOUBLE) {
				/* make border overhang bottom corner fully,
				 * if bottom border is opaque
				 */
				z[3] += bottom;
				square_end_2 = true;
			}

			col = nscss_color_to_ns(box->border[side].c);

			res = html_redraw_border_plot(side,
						      z,
						      col,
						      box->border[side].style,
						      box->border[side].width * scale,
						      square_end_1 && square_end_2,
						      clip,
						      ctx);
			if (res != NSERROR_OK) {
				return false;
			}
			break;

		case RIGHT:
			square_end_1 = (top == 0);
			square_end_2 = (bottom == 0);

			z[0] = p[6];	z[1] = p[1];
			z[2] = p[4];	z[3] = p[3];
			z[4] = p[4];	z[5] = p[5];
			z[6] = p[6];	z[7] = p[7];

			if (nscss_color_is_transparent(box->border[TOP].c) == false &&
			    box->border[TOP].style != CSS_BORDER_STYLE_DOUBLE) {
				/* make border overhang top corner fully,
				 * if top border is opaque
				 */
				z[3] -= top;
				square_end_1 = true;
			}
			if (nscss_color_is_transparent(box->border[BOTTOM].c) == false &&
			    box->border[BOTTOM].style != CSS_BORDER_STYLE_DOUBLE) {
				/* make border overhang bottom corner fully,
				 * if bottom border is opaque
				 */
				z[5] += bottom;
				square_end_2 = true;
			}

			col = nscss_color_to_ns(box->border[side].c);

			res = html_redraw_border_plot(side,
						      z,
						      col,
						      box->border[side].style,
						      box->border[side].width * scale,
						      square_end_1 && square_end_2,
						      clip,
						      ctx);
			if (res != NSERROR_OK) {
				return false;
			}
			break;

		case TOP:
			if (clip->y0 > p[3]) {
				/* clip rectangle is below border; nothing to
				 * plot
				 */
				continue;
			}

			square_end_1 = (left == 0);
			square_end_2 = (right == 0);

			z[0] = p[2];	z[1] = p[3];
			z[2] = p[0];	z[3] = p[1];
			z[4] = p[6];	z[5] = p[1];
			z[6] = p[4];	z[7] = p[3];

			if (box->border[TOP].style == CSS_BORDER_STYLE_SOLID &&
			    box->border[TOP].c == box->border[LEFT].c) {
				/* don't bother overlapping left corner if
				 * it's the same colour anyway
				 */
				z[2] += left;
				square_end_1 = true;
			}
			if (box->border[TOP].style == CSS_BORDER_STYLE_SOLID &&
			    box->border[TOP].c == box->border[RIGHT].c) {
				/* don't bother overlapping right corner if
				 * it's the same colour anyway
				 */
				z[4] -= right;
				square_end_2 = true;
			}

			col = nscss_color_to_ns(box->border[side].c);

			res = html_redraw_border_plot(side,
						      z,
						      col,
						      box->border[side].style,
						      box->border[side].width * scale,
						      square_end_1 && square_end_2,
						      clip,
						      ctx);
			if (res != NSERROR_OK) {
				return false;
			}
			break;

		case BOTTOM:
			if (clip->y1 < p[5]) {
				/* clip rectangle is above border; nothing to
				 * plot
				 */
				continue;
			}

			square_end_1 = (left == 0);
			square_end_2 = (right == 0);

			z[0] = p[4];	z[1] = p[5];
			z[2] = p[6];	z[3] = p[7];
			z[4] = p[0];	z[5] = p[7];
			z[6] = p[2];	z[7] = p[5];

			if (box->border[BOTTOM].style == CSS_BORDER_STYLE_SOLID &&
			    box->border[BOTTOM].c == box->border[LEFT].c) {
				/* don't bother overlapping left corner if
				 * it's the same colour anyway
				 */
				z[4] += left;
				square_end_1 = true;
			}
			if (box->border[BOTTOM].style == CSS_BORDER_STYLE_SOLID &&
			    box->border[BOTTOM].c == box->border[RIGHT].c) {
				/* don't bother overlapping right corner if
				 * it's the same colour anyway
				 */
				z[2] -= right;
				square_end_2 = true;
			}

			col = nscss_color_to_ns(box->border[side].c);

			res = html_redraw_border_plot(side,
						      z,
						      col,
						      box->border[side].style,
						      box->border[side].width * scale,
						      square_end_1 && square_end_2,
						      clip,
						      ctx);
			if (res != NSERROR_OK) {
				return false;
			}
			break;

		default:
			assert(side == TOP || side == BOTTOM ||
			       side == LEFT || side == RIGHT);
			break;
		}
	}

	return true;
}


/**
 * Draw an inline's borders.
 *
 * \param box BOX_INLINE which created the border
 * \param b coordinates of border edge rectangle
 * \param clip cliping area for redrawing border.
 * \param scale scale for redraw
 * \param first true if this is the first rectangle associated with the inline
 * \param last true if this is the last rectangle associated with the inline
 * \param ctx current redraw context
 * \return true if successful, false otherwise
 */
bool
html_redraw_inline_borders(struct box *box,
			   struct rect b,
			   const struct rect *clip,
			   float scale,
			   bool first,
			   bool last,
			   const struct redraw_context *ctx)
{
	int top = box->border[TOP].width;
	int right = box->border[RIGHT].width;
	int bottom = box->border[BOTTOM].width;
	int left = box->border[LEFT].width;
	colour col;
	int p[8]; /* Box border vertices */
	int z[8]; /* Border vertices */
	bool square_end_1;
	bool square_end_2;
	nserror res;
        css_fixed br_len = 0;
        css_unit br_unit = CSS_UNIT_PX;

        /* MacSurf: If border_radius is present, html_redraw_inline_background handles the border frame. */
        if (box && box->style && css_computed_border_radius(box->style, &br_len, &br_unit) == CSS_BORDER_RADIUS_SET && br_len > 0) {
                return true;
        }

	if (scale != 1.0) {
		top *= scale;
		right *= scale;
		bottom *= scale;
		left *= scale;
	}

	/* Calculate border vertices
	 *
	 *    A----------------------+
	 *    | \                  / |
	 *    |   B--------------+   |
	 *    |   |              |   |
	 *    |   +--------------C   |
	 *    | /                  \ |
	 *    +----------------------D
	 */
	p[0] = b.x0;				p[1] = b.y0;		/* A */
	p[2] = first ? b.x0 + left : b.x0;	p[3] = b.y0 + top;	/* B */
	p[4] = last ? b.x1 - right : b.x1;	p[5] = b.y1 - bottom;	/* C */
	p[6] = b.x1;				p[7] = b.y1;		/* D */

	assert(box->style);

	/* Left */
	square_end_1 = (top == 0);
	square_end_2 = (bottom == 0);
	if (left != 0 &&
	    first &&
	    nscss_color_is_transparent(box->border[LEFT].c) == false) {
		col = nscss_color_to_ns(box->border[LEFT].c);

		z[0] = p[0];	z[1] = p[7];
		z[2] = p[2];	z[3] = p[5];
		z[4] = p[2];	z[5] = p[3];
		z[6] = p[0];	z[7] = p[1];

		if (nscss_color_is_transparent(box->border[TOP].c) == false &&
		    box->border[TOP].style != CSS_BORDER_STYLE_DOUBLE) {
			/* make border overhang top corner fully,
			 * if top border is opaque
			 */
			z[5] -= top;
			square_end_1 = true;
		}

		if (nscss_color_is_transparent(box->border[BOTTOM].c) == false &&
		    box->border[BOTTOM].style != CSS_BORDER_STYLE_DOUBLE) {
			/* make border overhang bottom corner fully,
			 * if bottom border is opaque
			 */
			z[3] += bottom;
			square_end_2 = true;
		}

		res = html_redraw_border_plot(LEFT,
					      z,
					      col,
					      box->border[LEFT].style,
					      left,
					      square_end_1 && square_end_2,
					      clip,
					      ctx);
		if (res != NSERROR_OK) {
			return false;
		}
	}

	/* Right */
	square_end_1 = (top == 0);
	square_end_2 = (bottom == 0);
	if (right != 0 &&
	    last &&
	    nscss_color_is_transparent(box->border[RIGHT].c) == false) {
		col = nscss_color_to_ns(box->border[RIGHT].c);

		z[0] = p[6];	z[1] = p[1];
		z[2] = p[4];	z[3] = p[3];
		z[4] = p[4];	z[5] = p[5];
		z[6] = p[6];	z[7] = p[7];

		if (nscss_color_is_transparent(box->border[TOP].c) == false &&
		    box->border[TOP].style != CSS_BORDER_STYLE_DOUBLE) {
			/* make border overhang top corner fully,
			 * if top border is opaque
			 */
			z[3] -= top;
			square_end_1 = true;
		}

		if (nscss_color_is_transparent(box->border[BOTTOM].c) == false &&
		    box->border[BOTTOM].style != CSS_BORDER_STYLE_DOUBLE) {
			/* make border overhang bottom corner fully,
			 * if bottom border is opaque
			 */
			z[5] += bottom;
			square_end_2 = true;
		}

		res = html_redraw_border_plot(RIGHT,
					      z,
					      col,
					      box->border[RIGHT].style,
					      right,
					      square_end_1 && square_end_2,
					      clip,
					      ctx);
		if (res != NSERROR_OK) {
			return false;
		}
	}

	/* Top */
	square_end_1 = (left == 0);
	square_end_2 = (right == 0);
	if (top != 0 &&
	    nscss_color_is_transparent(box->border[TOP].c) == false) {
		col = nscss_color_to_ns(box->border[TOP].c);

		z[0] = p[2];	z[1] = p[3];
		z[2] = p[0];	z[3] = p[1];
		z[4] = p[6];	z[5] = p[1];
		z[6] = p[4];	z[7] = p[3];

		if (first &&
		    box->border[TOP].style == CSS_BORDER_STYLE_SOLID &&
		    box->border[TOP].c == box->border[LEFT].c) {
			/* don't bother overlapping left corner if
			 * it's the same colour anyway
			 */
			z[2] += left;
			square_end_1 = true;
		}

		if (last &&
		    box->border[TOP].style == CSS_BORDER_STYLE_SOLID &&
		    box->border[TOP].c == box->border[RIGHT].c) {
			/* don't bother overlapping right corner if
			 * it's the same colour anyway
			 */
			z[4] -= right;
			square_end_2 = true;
		}

		res = html_redraw_border_plot(TOP,
					      z,
					      col,
					      box->border[TOP].style,
					      top,
					      square_end_1 && square_end_2,
					      clip,
					      ctx);
		if (res != NSERROR_OK) {
			return false;
		}
	}

	/* Bottom */
	square_end_1 = (left == 0);
	square_end_2 = (right == 0);
	if (bottom != 0 &&
	    nscss_color_is_transparent(box->border[BOTTOM].c) == false) {
		col = nscss_color_to_ns(box->border[BOTTOM].c);

		z[0] = p[4];	z[1] = p[5];
		z[2] = p[6];	z[3] = p[7];
		z[4] = p[0];	z[5] = p[7];
		z[6] = p[2];	z[7] = p[5];

		if (first &&
		    box->border[BOTTOM].style == CSS_BORDER_STYLE_SOLID &&
		    box->border[BOTTOM].c == box->border[LEFT].c) {
			/* don't bother overlapping left corner if
			 * it's the same colour anyway
			 */
			z[4] += left;
			square_end_1 = true;
		}

		if (last &&
		    box->border[BOTTOM].style == CSS_BORDER_STYLE_SOLID &&
		    box->border[BOTTOM].c == box->border[RIGHT].c) {
			/* don't bother overlapping right corner if
			 * it's the same colour anyway
			 */
			z[2] -= right;
			square_end_2 = true;
		}

		res = html_redraw_border_plot(BOTTOM,
					      z,
					      col,
					      box->border[BOTTOM].style,
					      bottom,
					      square_end_1 && square_end_2,
					      clip,
					      ctx);
		if (res != NSERROR_OK) {
			return false;
		}
	}

	return true;
}
