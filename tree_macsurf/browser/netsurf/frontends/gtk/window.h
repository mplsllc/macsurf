/*
 * Copyright 2006 Daniel Silverstone <dsilvers@digital-scurf.org>
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

#ifndef NETSURF_GTK_WINDOW_H
#define NETSURF_GTK_WINDOW_H 1

struct nsgtk_pi_window;

extern struct gui_window_table *nsgtk_window_table;
extern struct gui_search_web_table *nsgtk_search_web_table;

extern struct gui_window *window_list;

/**
 * get core browsing context from gui window handle
 *
 * \param gw gui window handle
 */
struct browser_window *nsgtk_get_browser_window(struct gui_window *gw);

/**
 * get containing nsgtk scaffolding handle from gui window handle
 *
 * \param gw gui window handle
 */
struct nsgtk_scaffolding *nsgtk_get_scaffold(struct gui_window *gw);

/**
 * Every window will have its tab, toolbar and drawing area updated
 *
 * The update will ensure the correct tab options are used, the
 * toolbar size and style is changed and the browser window contents
 * redrawn.
 */
nserror nsgtk_window_update_all(void);

/**
 * every window will have its toolbar updated to reflect user settings
 */
nserror nsgtk_window_toolbar_update(void);

/**
 * Windows associated with a scaffold will have their toolbar show state set
 */
nserror nsgtk_window_toolbar_show(struct nsgtk_scaffolding *gs, bool show);

/**
 * update targets
 *
 * \param gw gui window handle
 */
int nsgtk_gui_window_update_targets(struct gui_window *gw);

/**
 * destroy browsing context
 *
 * \param gw gui window handle
 */
void nsgtk_window_destroy_browser(struct gui_window *gw);


/**
 * toggle search visibility
 *
 * \param gw gui window handle
 */
nserror nsgtk_window_search_toggle(struct gui_window *gw);

/**
 * get gtk layout from gui handle
 *
 * \param gw gui window handle
 */
GtkLayout *nsgtk_window_get_layout(struct gui_window *gw);


/**
 * activate the handler for a item in a toolbar of a gui window
 *
 * \param gw The gui window handle
 * \param itemid The id of the item to activate
 */
nserror nsgtk_window_item_activate(struct gui_window *gw, nsgtk_toolbar_button itemid);

/**
 * position page_info appropriately
 *
 * \param gw The gui window handle to position relative to
 * \param win The page-info window to position
 */
nserror nsgtk_window_position_page_info(struct gui_window *gw,
					struct nsgtk_pi_window *win);

/**
 * position local_history appropriately
 *
 * \param gw The gui window handle to position relative to
 */
nserror nsgtk_window_position_local_history(struct gui_window *gw);

#endif /* NETSURF_GTK_WINDOW_H */
