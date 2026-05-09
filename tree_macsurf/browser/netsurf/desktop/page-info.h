/*
 * Copyright 2020 Michael Drake <tlsa@netsurf-browser.org>
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
 * Pave info viewer window interface
 */

#ifndef NETSURF_DESKTOP_PAGE_INFO_H
#define NETSURF_DESKTOP_PAGE_INFO_H

#include <stdint.h>
#include <stdbool.h>

#include "utils/errors.h"
#include "netsurf/mouse.h"

struct rect;
struct nsurl;
struct page_info;
struct core_window;
struct browser_window;
struct redraw_context;

/**
 * Initialise the page_info module.
 *
 * \return NSERROR_OK on success, appropriate error code otherwise.
 */
nserror page_info_init(void);

/**
 * Finalise the page_info module.
 *
 * \return NSERROR_OK on success, appropriate error code otherwise.
 */
nserror page_info_fini(void);

/**
 * Create a page info corewindow.
 *
 * The page info window is opened for a particular browser window.
 * It can be destroyed before the browser window is destroyed by calling
 * \ref page_info_destroy.
 *
 * \param[in]  cw_h    Handle for the containing core_window.
 * \param[in]  bw      Browser window to show page info for.
 * \param[out] pi_out  The created page info window handle.
 * \return NSERROR_OK on success, appropriate error code otherwise.
 */
nserror page_info_create(struct core_window *cw_h,
			 struct browser_window *bw,
			 struct page_info **pi_out);

/**
 * Destroy a page info corewindow.
 *
 * \param[in] pi  The page info window handle.
 */
nserror page_info_destroy(struct page_info *pi);

/**
 * change the browser window the page information refers to
 *
 * \param[in] pgi The page info window context
 * \param[in] bw The new browser window
 * \return NSERROR_OK on sucess else error code.
 */
nserror page_info_set(struct page_info *pgi, struct browser_window *bw);

/**
 * Redraw the page info window.
 *
 * Causes the page info window to issue plot operations to redraw
 * the specified area of the viewport.
 *
 * \param[in] pi    The page info window handle.
 * \param[in] x     X coordinate to render page_info at.
 * \param[in] y     Y coordinate to render page_info at.
 * \param[in] clip  Current clip rectangle.
 * \param[in] ctx   Current redraw context.
 * \return NSERROR_OK on success, appropriate error code otherwise.
 */
nserror page_info_redraw(
		const struct page_info *pi,
		int x,
		int y,
		const struct rect *clip,
		const struct redraw_context *ctx);

/**
 * Mouse action handling.
 *
 * \param[in] pi     The page info window handle.
 * \param[in] mouse  The current mouse state
 * \param[in] x      The current mouse X coordinate
 * \param[in] y      The current mouse Y coordinate
 * \param[out] did_something Set to true if this resulted in some action
 * \return NSERROR_OK on success, appropriate error code otherwise.
 */
nserror page_info_mouse_action(
		struct page_info *pi,
		enum browser_mouse_state mouse,
		int x,
		int y,
		bool *did_something);

/**
 * Key press handling.
 *
 * \param[in] pi   The page info window handle.
 * \param[in] key  The ucs4 character codepoint.
 * \return true if the keypress is dealt with, false otherwise.
 */
bool page_info_keypress(
		struct page_info *pi,
		int32_t key);

/**
 * Get size of page info content area.
 *
 * \param[in]  pi      The page info window handle.
 * \param[out] width   On success, return the page info content width.
 * \param[out] height  On success, return the page info content height.
 * \return NSERROR_OK on success, appropriate error code otherwise.
 */
nserror page_info_get_size(
		struct page_info *pi,
		int *width,
		int *height);

#endif
