/*
 * Copyright 2011 John-Mark Bell <jmb@netsurf-browser.org>
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

#include <stdbool.h>
#include <stdlib.h>

#include "utils/utils.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "netsurf/plotters.h"
#include "netsurf/bitmap.h"
#include "netsurf/content.h"
#include "desktop/gui_internal.h"
#include "desktop/bitmap.h"

#include "image/bmp.h"
#include "image/gif.h"
#include "image/ico.h"
#include "image/jpeg.h"
#include "image/jpegxl.h"
#include "image/nssprite.h"
#include "image/png.h"
#include "image/rsvg.h"
#include "image/svg.h"
#include "image/webp.h"
#include "image/image.h"

/**
 * Initialise image content handlers
 *
 * \return NSERROR_OK on success, appropriate error otherwise.
 */
nserror image_init(void)
{
	nserror error = NSERROR_OK;

#ifdef WITH_BMP
	error = nsbmp_init();
	if (error != NSERROR_OK)
		return error;
#endif

#ifdef WITH_GIF
	error = nsgif_init();
	if (error != NSERROR_OK)
		return error;
#endif

#ifdef WITH_BMP
	error = nsico_init();
	if (error != NSERROR_OK)
		return error;
#endif

#ifdef WITH_JPEG
	error = nsjpeg_init();
	if (error != NSERROR_OK)
		return error;
#endif

#ifdef WITH_JPEGXL
	error = nsjpegxl_init();
	if (error != NSERROR_OK)
		return error;
#endif

#ifdef WITH_PNG
	error = nspng_init();
	if (error != NSERROR_OK)
		return error;
#endif

#ifdef WITH_NSSPRITE
	error = nssprite_init();
	if (error != NSERROR_OK)
		return error;
#endif

	/* Prefer rsvg over libsvgtiny for svgs */
#ifdef WITH_NS_SVG
	error = svg_init();
	if (error != NSERROR_OK)
		return error;
#endif
#ifdef WITH_RSVG
	error = nsrsvg_init();
	if (error != NSERROR_OK)
		return error;
#endif

#ifdef WITH_WEBP
	error = nswebp_init();
	if (error != NSERROR_OK)
		return error;
#endif

	return error;
}


bool image_bitmap_plot(struct bitmap *bitmap,
		       struct content_redraw_data *data,
		       const struct rect *clip,
		       const struct redraw_context *ctx)
{
	bitmap_flags_t flags = BITMAPF_NONE;

	int width;
	int height;
	unsigned char *pixel;
	plot_style_t fill_style;
	struct rect area;

	width = guit->bitmap->get_width(bitmap);
	if (width == 1) {
		height = guit->bitmap->get_height(bitmap);
		if (height == 1) {
			/* optimise 1x1 bitmap plot */
			pixel = guit->bitmap->get_buffer(bitmap);
			fill_style.fill_colour = bitmap_pixel_to_colour(pixel);

			if (guit->bitmap->get_opaque(bitmap) ||
			    ((fill_style.fill_colour & 0xff000000) == 0xff000000)) {

				area = *clip;

				if (data->repeat_x != true) {
					area.x0 = data->x;
					area.x1 = data->x + data->width;
				}

				if (data->repeat_y != true) {
					area.y0 = data->y;
					area.y1 = data->y + data->height;
				}

				fill_style.stroke_type = PLOT_OP_TYPE_NONE;
				fill_style.fill_type = PLOT_OP_TYPE_SOLID;

				return (ctx->plot->rectangle(ctx,
							     &fill_style,
							     &area) == NSERROR_OK);

			} else if ((fill_style.fill_colour & 0xff000000) == 0) {
				/* transparent pixel used as spacer, skip it */
				return true;
			}
		}
	}

	/* do the plot */
	if (data->repeat_x)
		flags |= BITMAPF_REPEAT_X;
	if (data->repeat_y)
		flags |= BITMAPF_REPEAT_Y;

	return (ctx->plot->bitmap(ctx,
				  bitmap,
				  data->x, data->y,
				  data->width, data->height,
				  data->background_colour,
				  flags) == NSERROR_OK);
}
