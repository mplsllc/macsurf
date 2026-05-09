/*
 * Copyright 2013 Michael Drake <tlsa@netsurf-browser.org>
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
 
#ifndef _NETSURF_DESKTOP_HOTLIST_H_
#define _NETSURF_DESKTOP_HOTLIST_H_

#include <stdbool.h>
#include <stdint.h>

#include "utils/errors.h"
#include "netsurf/mouse.h"

struct redraw_context;
struct nsurl;
struct rect;

/**
 * Initialise the hotlist.
 *
 * This opens the hotlist file, construct the hostlist, and creates a
 * treeview.  If there's no hotlist file, it generates a default hotlist.
 *
 * This must be called before any other hotlist_* function.  It must
 * be called before URLs can be added to the hotlist, and before the
 * hotlist can be queried to ask if URLs are present in the hotlist.
 *
 * In read-only mode the hotlist can be modified, but changes will not
 * persist over sessions.
 *
 * \param load_path The path to load hotlist from.
 * \param save_path The path to save hotlist to, or NULL for read-only mode.
 * \return NSERROR_OK on success, appropriate error otherwise
 */
nserror hotlist_init(const char *load_path, const char *save_path);

/**
 * Initialise the hotlist manager.
 *
 * This connects the underlying hotlist treeview to a corewindow for display.
 *
 * The provided core window handle must be valid until hotlist_fini is called.
 *
 * \param core_window_handle The handle in which the hotlist is shown
 * \return NSERROR_OK on success, appropriate error otherwise
 */
nserror hotlist_manager_init(void *core_window_handle);


/**
 * Finalise the hotlist manager.
 *
 * This simply disconnects the underlying treeview from its corewindow,
 * allowing destruction of a GUI hotlist window, without finalising the
 * hotlist module.
 *
 * \return NSERROR_OK on success, appropriate error otherwise
 */
nserror hotlist_manager_fini(void);

/**
 * Finalise the hotlist.
 *
 * This destroys the hotlist treeview and the hotlist module's
 * internal data.  After calling this if hotlist is required again,
 * hotlist_init must be called.
 *
 * \return NSERROR_OK on success, appropriate error otherwise
 */
nserror hotlist_fini(void);

/**
 * Add an entry to the hotlist for given URL.
 *
 * \param url		URL for node being added
 * \return NSERROR_OK on success, appropriate error otherwise
 */
nserror hotlist_add_url(struct nsurl *url);

/**
 * Check whether given URL is present in hotlist
 *
 * \param url		Address to look for in hotlist
 * \return true iff url is present in hotlist, false otherwise
 */
bool hotlist_has_url(struct nsurl *url);

/**
 * Remove any entries matching the given URL from the hotlist
 *
 * \param url		Address to look for in hotlist
 */
void hotlist_remove_url(struct nsurl *url);

/**
 * Update given URL, e.g. new visited data
 *
 * \param url		Address to update entries for
 */
void hotlist_update_url(struct nsurl *url);

/**
 * Add an entry to the hotlist for given Title/URL.
 *
 * \param url		URL for entry to be added, or NULL
 * \param title		Title for entry being added (copied), or NULL
 * \param at_y		Iff true, insert at y-offest
 * \param y		Y-offset in px from top of hotlist.  Ignored if (!at_y).
 * \return NSERROR_OK on success, appropriate error otherwise
 */
nserror hotlist_add_entry(struct nsurl *url, const char *title, bool at_y, int y);

/**
 * Add a folder to the hotlist.
 *
 * \param title Title for folder being added, or NULL
 * \param at_y Iff true, insert at y-offest
 * \param y Y-offset in px from top of hotlist.  Ignored if (!at_y).
 * \return NSERROR_OK on success, appropriate error otherwise
 */
nserror hotlist_add_folder(const char *title, bool at_y, int y);

/**
 * Save hotlist to file
 *
 * \param path		The path to save hotlist to
 * \param title		The title to give the hotlist, or NULL for default
 * \return NSERROR_OK on success, or appropriate error otherwise
 */
nserror hotlist_export(const char *path, const char *title);

/**
 * Client callback for hotlist_iterate, reporting entry into folder
 *
 * \param ctx		Client context
 * \param title		The entered folder's title
 * \return NSERROR_OK on success, or appropriate error otherwise
 */
typedef nserror (*hotlist_folder_enter_cb)(void *ctx, const char *title);

/**
 * Client callback for hotlist_iterate, reporting a hotlist address
 *
 * \param ctx		Client context
 * \param url		The entry's address
 * \param title		The entry's title
 * \return NSERROR_OK on success, or appropriate error otherwise
 */
typedef nserror (*hotlist_address_cb)(void *ctx, struct nsurl *url, const char *title);

/**
 * Client callback for hotlist_iterate, reporting a hotlist folder departure
 *
 * \param ctx		Client context
 * \param title		The departed folder's title
 * \return NSERROR_OK on success, or appropriate error otherwise
 */
typedef nserror (*hotlist_folder_leave_cb)(void *ctx);


/**
 * Walk (depth first) the hotlist, calling callbacks on entering folders,
 * address nodes, and on leaving folders.
 *
 * \param ctx		Client context, passed back to callback function
 * \param enter_cb	Function to call on entering nodes, or NULL
 * \param address_cb	Function to call on address nodes, or NULL
 * \param leave_cb	Function to call on leaving nodes, or NULL
 * \return NSERROR_OK on success, or appropriate error otherwise
 *
 * Example usage: Generate a menu containing hotlist entries.  For flat list
 *                set enter_cb and leave_cb to NULL, or for hierarchical menu
 *                provide the folder callbacks.
 */
nserror hotlist_iterate(void *ctx,
		hotlist_folder_enter_cb enter_cb,
		hotlist_address_cb address_cb,
		hotlist_folder_leave_cb leave_cb);

/**
 * Redraw the hotlist.
 *
 * \param x		X coordinate to render treeview at
 * \param y		Y coordinate to render treeview at
 * \param clip		Current clip rectangle (wrt tree origin)
 * \param ctx		Current redraw context
 */
void hotlist_redraw(int x, int y, struct rect *clip,
		const struct redraw_context *ctx);

/**
 * Handles all kinds of mouse action
 *
 * \param mouse		The current mouse state
 * \param x		X coordinate
 * \param y		Y coordinate
 */
void hotlist_mouse_action(enum browser_mouse_state mouse, int x, int y);

/**
 * Key press handling.
 *
 * \param key The ucs4 character codepoint
 * \return true if the keypress is dealt with, false otherwise.
 */
bool hotlist_keypress(uint32_t key);

/**
 * Determine whether there is a selection
 *
 * \return true iff there is a selection
 */
bool hotlist_has_selection(void);

/**
 * Get the first selected node
 *
 * \param url		Updated to the selected entry's address, or NULL
 * \param title		Updated to the selected entry's title, or NULL
 * \return true iff hotlist has a selection
 */
bool hotlist_get_selection(struct nsurl **url, const char **title);

/**
 * Edit the first selected node
 */
void hotlist_edit_selection(void);

/**
 * Expand the treeview's nodes
 *
 * \param only_folders	Iff true, only folders are expanded.
 * \return NSERROR_OK on success, appropriate error otherwise
 */
nserror hotlist_expand(bool only_folders);

/**
 * Contract the treeview's nodes
 *
 * \param all		Iff false, only entries are contracted.
 * \return NSERROR_OK on success, appropriate error otherwise
 */
nserror hotlist_contract(bool all);

#endif
