/*
 * Copyright 2022 Michael Drake <tlsa@netsurf-browser.org>
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

/** \file
 * Internal core bitmap interface.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "utils/log.h"
#include "utils/ns_errors.h"

#include "desktop/bitmap.h"
#include "desktop/gui_internal.h"

/** The client bitmap format. */
bitmap_fmt_t bitmap_fmt;

/** The client bitmap colour channel layout. */
/* C89: positional initializer (field order: r, g, b, a) */
struct bitmap_colour_layout bitmap_layout = { 0, 1, 2, 3 };

/**
 * Get the colour layout for the given bitmap format.
 */
static struct bitmap_colour_layout bitmap__get_colour_layout(
		const bitmap_fmt_t *fmt)
{
	struct bitmap_colour_layout layout;
	switch (fmt->layout) {
	default:
	case BITMAP_LAYOUT_R8G8B8A8:
		layout.r = 0; layout.g = 1; layout.b = 2; layout.a = 3;
		break;
	case BITMAP_LAYOUT_B8G8R8A8:
		layout.b = 0; layout.g = 1; layout.r = 2; layout.a = 3;
		break;
	case BITMAP_LAYOUT_A8R8G8B8:
		layout.a = 0; layout.r = 1; layout.g = 2; layout.b = 3;
		break;
	case BITMAP_LAYOUT_A8B8G8R8:
		layout.a = 0; layout.b = 1; layout.g = 2; layout.r = 3;
		break;
	}
	return layout;
}

/**
 * Get string for given pixel layout.
 */
static const char *bitmap__layout_to_str(enum bitmap_layout layout)
{
	switch (layout) {
	case BITMAP_LAYOUT_R8G8B8A8: return "Byte-wise RGBA";
	case BITMAP_LAYOUT_B8G8R8A8: return "Byte-wise BGRA";
	case BITMAP_LAYOUT_A8R8G8B8: return "Byte-wise ARGB";
	case BITMAP_LAYOUT_A8B8G8R8: return "Byte-wise ABGR";
	case BITMAP_LAYOUT_RGBA8888: return "0xRRGGBBAA (native endian)";
	case BITMAP_LAYOUT_BGRA8888: return "0xBBGGRRAA (native endian)";
	case BITMAP_LAYOUT_ARGB8888: return "0xAARRGGBB (native endian)";
	case BITMAP_LAYOUT_ABGR8888: return "0xAABBGGRR (native endian)";
	default:                     return "Unknown";
	}
}

/* Exported function, documented in include/netsurf/bitmap.h */
void bitmap_set_format(const bitmap_fmt_t *bitmap_format)
{
	bitmap_fmt = *bitmap_format;

	NSLOG(netsurf, INFO, "Setting core bitmap format to: %s%s",
			bitmap__layout_to_str(bitmap_format->layout),
			bitmap_format->pma ? " pre multiplied alpha" : "");

	bitmap_fmt.layout = bitmap_sanitise_bitmap_layout(bitmap_fmt.layout);

	if (bitmap_format->layout != bitmap_fmt.layout) {
		NSLOG(netsurf, INFO, "Sanitised layout to: %s",
				bitmap__layout_to_str(bitmap_fmt.layout));
	}

	bitmap_layout = bitmap__get_colour_layout(&bitmap_fmt);
}

/**
 * Swap colour component order.
 */
static void bitmap__format_convert(
		int width,
		int height,
		uint8_t *buffer,
		size_t rowstride,
		struct bitmap_colour_layout to,
		struct bitmap_colour_layout from)
{
	int y, x;
	for (y = 0; y < height; y++) {
		uint8_t *row = buffer;

		for (x = 0; x < width; x++) {
			const uint32_t px = *((uint32_t *)(void *) row);

			row[to.r] = ((const uint8_t *) &px)[from.r];
			row[to.g] = ((const uint8_t *) &px)[from.g];
			row[to.b] = ((const uint8_t *) &px)[from.b];
			row[to.a] = ((const uint8_t *) &px)[from.a];

			row += sizeof(uint32_t);
		}

		buffer += rowstride;
	}
}

/**
 * Convert plain alpha to premultiplied alpha.
 */
static void bitmap__format_convert_to_pma(
		int width,
		int height,
		uint8_t *buffer,
		size_t rowstride,
		struct bitmap_colour_layout to,
		struct bitmap_colour_layout from)
{
	int y, x;
	for (y = 0; y < height; y++) {
		uint8_t *row = buffer;

		for (x = 0; x < width; x++) {
			const uint32_t px = *((uint32_t *)(void *) row);
			uint32_t a, r, g, b;

			r = ((const uint8_t *) &px)[from.r];
			g = ((const uint8_t *) &px)[from.g];
			b = ((const uint8_t *) &px)[from.b];
			a = ((const uint8_t *) &px)[from.a];

			if (a != 0) {
				r = ((r * (a + 1)) >> 8) & 0xff;
				g = ((g * (a + 1)) >> 8) & 0xff;
				b = ((b * (a + 1)) >> 8) & 0xff;
			} else {
				r = g = b = 0;
			}

			row[to.r] = r;
			row[to.g] = g;
			row[to.b] = b;
			row[to.a] = a;

			row += sizeof(uint32_t);
		}

		buffer += rowstride;
	}
}

/**
 * Convert from premultiplied alpha to plain alpha.
 */
static void bitmap__format_convert_from_pma(
		int width,
		int height,
		uint8_t *buffer,
		size_t rowstride,
		struct bitmap_colour_layout to,
		struct bitmap_colour_layout from)
{
	int y, x;
	for (y = 0; y < height; y++) {
		uint8_t *row = buffer;

		for (x = 0; x < width; x++) {
			const uint32_t px = *((uint32_t *)(void *) row);
			uint32_t a, r, g, b;

			r = ((const uint8_t *) &px)[from.r];
			g = ((const uint8_t *) &px)[from.g];
			b = ((const uint8_t *) &px)[from.b];
			a = ((const uint8_t *) &px)[from.a];

			if (a != 0) {
				r = (r << 8) / a;
				g = (g << 8) / a;
				b = (b << 8) / a;

				r = (r > 255) ? 255 : r;
				g = (g > 255) ? 255 : g;
				b = (b > 255) ? 255 : b;
			} else {
				r = g = b = 0;
			}

			row[to.r] = r;
			row[to.g] = g;
			row[to.b] = b;
			row[to.a] = a;

			row += sizeof(uint32_t);
		}

		buffer += rowstride;
	}
}

/* Exported function, documented in desktop/bitmap.h */
void bitmap_format_convert(void *bitmap,
		const bitmap_fmt_t *fmt_from,
		const bitmap_fmt_t *fmt_to)
{
	int width = guit->bitmap->get_width(bitmap);
	int height = guit->bitmap->get_height(bitmap);
	bool opaque = guit->bitmap->get_opaque(bitmap);
	uint8_t *buffer = guit->bitmap->get_buffer(bitmap);
	size_t rowstride = guit->bitmap->get_rowstride(bitmap);
	struct bitmap_colour_layout to = bitmap__get_colour_layout(fmt_to);
	struct bitmap_colour_layout from = bitmap__get_colour_layout(fmt_from);

	NSLOG(netsurf, DEEPDEBUG, "%p: format conversion (%u%s --> %u%s)",
			bitmap,
			fmt_from->layout, fmt_from->pma ? " pma" : "",
			fmt_to->layout, fmt_to->pma ? " pma" : "");

	if (fmt_from->pma == fmt_to->pma) {
		bitmap__format_convert(
				width, height, buffer,
				rowstride, to, from);

	} else if (opaque == false) {
		if (fmt_to->pma) {
			bitmap__format_convert_to_pma(
					width, height, buffer,
					rowstride, to, from);
		} else {
			bitmap__format_convert_from_pma(
					width, height, buffer,
					rowstride, to, from);
		}
	}
}

/* Exported function, documented in desktop/bitmap.h */
bool bitmap_test_opaque(void *bitmap)
{
	int width = guit->bitmap->get_width(bitmap);
	int height = guit->bitmap->get_height(bitmap);
	size_t rowstride = guit->bitmap->get_rowstride(bitmap);
	const uint8_t *buffer = guit->bitmap->get_buffer(bitmap);
	int y, x;

	width *= sizeof(uint32_t);

	for (y = 0; y < height; y++) {
		const uint8_t *row = buffer;

		for (x = bitmap_layout.a; x < width; x += 4) {
			if (row[x] != 0xff) {
				return false;
			}
		}

		buffer += rowstride;
	}

	return true;
}
