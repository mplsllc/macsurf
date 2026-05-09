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
 * Generic bitmap handling interface.
 *
 * This interface wraps the native platform-specific image format.
 *
 * Bitmaps are required to be 32bpp with 8-bit components. The components are
 * red, green, blue, and alpha, in client specified order.
 *
 * The component order may be set in the front ends by calling
 * \ref bitmap_set_format().
 */

#ifndef _NETSURF_BITMAP_H_
#define _NETSURF_BITMAP_H_

/** Bitmap creation flags. */
enum gui_bitmap_flags {
	BITMAP_NONE   = 0,
	BITMAP_OPAQUE = (1 << 0), /**< image is opaque */
	BITMAP_CLEAR  = (1 << 1), /**< memory should be wiped to 0 */
};

/**
 * NetSurf bitmap pixel layout.
 *
 * All pixels are 32 bits per pixel (bpp). The different layouts allow control
 * over the ordering of colour channels. All colour channels are 8 bits wide.
 */
enum bitmap_layout {
	/** Bite-wise RGBA: Byte order: 0xRR, 0xGG, 0xBB, 0xAA. */
	BITMAP_LAYOUT_R8G8B8A8,

	/** Bite-wise BGRA: Byte order: 0xBB, 0xGG, 0xRR, 0xAA. */
	BITMAP_LAYOUT_B8G8R8A8,

	/** Bite-wise ARGB: Byte order: 0xAA, 0xRR, 0xGG, 0xBB. */
	BITMAP_LAYOUT_A8R8G8B8,

	/** Bite-wise ABGR: Byte order: 0xAA, 0xBB, 0xGG, 0xRR. */
	BITMAP_LAYOUT_A8B8G8R8,

	/**
	 * 32-bit RGBA (0xRRGGBBAA).
	 *
	 * * On little endian host, same as \ref BITMAP_LAYOUT_A8B8G8R8.
	 * * On big endian host, same as \ref BITMAP_LAYOUT_R8G8B8A8.
	 */
	BITMAP_LAYOUT_RGBA8888,

	/**
	 * 32-bit BGRA (0xBBGGRRAA).
	 *
	 * * On little endian host, same as \ref BITMAP_LAYOUT_A8R8G8B8.
	 * * On big endian host, same as \ref BITMAP_LAYOUT_B8G8R8A8.
	 */
	BITMAP_LAYOUT_BGRA8888,

	/**
	 * 32-bit ARGB (0xAARRGGBB).
	 *
	 * * On little endian host, same as \ref BITMAP_LAYOUT_B8G8R8A8.
	 * * On big endian host, same as \ref BITMAP_LAYOUT_A8R8G8B8.
	 */
	BITMAP_LAYOUT_ARGB8888,

	/**
	 * 32-bit BGRA (0xAABBGGRR).
	 *
	 * * On little endian host, same as \ref BITMAP_LAYOUT_R8G8B8A8.
	 * * On big endian host, same as \ref BITMAP_LAYOUT_A8B8G8R8.
	 */
	BITMAP_LAYOUT_ABGR8888,
};

/** Bitmap format specifier. */
typedef struct bitmap_fmt {
	enum bitmap_layout layout; /**< Colour component layout. */
	bool pma;                  /**< Premultiplied alpha. */
} bitmap_fmt_t;

struct content;
struct bitmap;
struct hlcache_handle;

/**
 * Set client bitmap format.
 *
 * Set this to ensure that the bitmaps decoded by the core are in the
 * correct format for the front end.
 *
 * \param[in]  bitmap_format  The bitmap format specification to set.
 */
void bitmap_set_format(const bitmap_fmt_t *bitmap_format);

/**
 * Test whether a bitmap is completely opaque (no transparency).
 *
 * \param[in]  bitmap  The bitmap to test.
 * \return Returns true if the bitmap is opaque, false otherwise.
 */
bool bitmap_test_opaque(void *bitmap);

/**
 * Bitmap operations.
 */
struct gui_bitmap_table {
	/* Mandatory entries */

	/**
	 * Create a new bitmap.
	 *
	 * \param width   width of image in pixels
	 * \param height  height of image in pixels
	 * \param flags   flags for bitmap creation
	 * \return A bitmap structure or NULL on error.
	 */
	void *(*create)(int width, int height, enum gui_bitmap_flags flags);

	/**
	 * Destroy a bitmap.
	 *
	 * \param bitmap The bitmap to destroy.
	 */
	void (*destroy)(void *bitmap);

	/**
	 * Set the opacity of a bitmap.
	 *
	 * \param bitmap The bitmap to set opacity on.
	 * \param opaque The bitmap opacity to set.
	 */
	void (*set_opaque)(void *bitmap, bool opaque);

	/**
	 * Get the opacity of a bitmap.
	 *
	 * \param bitmap The bitmap to examine.
	 * \return The bitmap opacity.
	 */
	bool (*get_opaque)(void *bitmap);

	/**
	 * Get the image buffer from a bitmap
	 *
	 * Note that all pixels must be 4-byte aligned.
	 *
	 * \param bitmap The bitmap to get the buffer from.
	 * \return The image buffer or NULL if there is none.
	 */
	unsigned char *(*get_buffer)(void *bitmap);

	/**
	 * Get the number of bytes per row of the image
	 *
	 * \param bitmap The bitmap
	 * \return The number of bytes for a row of the bitmap.
	 */
	size_t (*get_rowstride)(void *bitmap);

	/**
	 * Get the bitmap width
	 *
	 * \param bitmap The bitmap
	 * \return The bitmap width in pixels.
	 */
	int (*get_width)(void *bitmap);

	/**
	 * Get the bitmap height
	 *
	 * \param bitmap The bitmap
	 * \return The bitmap height in pixels.
	 */
	int (*get_height)(void *bitmap);

	/**
	 * Marks a bitmap as modified.
	 *
	 * \param bitmap The bitmap set as modified.
	 */
	void (*modified)(void *bitmap);

	/**
	 * Render content into a bitmap.
	 *
	 * \param bitmap The bitmap to render into.
	 * \param content The content to render.
	 */
	nserror (*render)(struct bitmap *bitmap, struct hlcache_handle *content);
};

#endif
