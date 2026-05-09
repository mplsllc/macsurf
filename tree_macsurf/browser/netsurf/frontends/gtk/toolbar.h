/*
 * Copyright 2009 Mark Benjamin <netsurf-browser.org.MarkBenjamin@dfgh.net>
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

#ifndef NETSURF_GTK_TOOLBAR_H_
#define NETSURF_GTK_TOOLBAR_H_

/**
 * control toolbar context
 */
struct nsgtk_toolbar;
struct nsgtk_scaffolding;

/**
 * create a control toolbar
 *
 * \param[in] builder The gtk builder object the toolbar is being created from
 * \param[out] toolbar a pointer to receive the result.
 * \return NSERROR_OK and toolbar updated on success else error code
 */
nserror nsgtk_toolbar_create(GtkBuilder *builder,
			     struct browser_window *(*get_bw)(void *ctx),
			     void *get_bw_ctx,
			     bool want_location_focus,
			     struct nsgtk_toolbar **toolbar);


/**
 * Update the toolbar items being shown based on current settings
 *
 * \param toolbar A toolbar returned from a creation
 * \return NSERROR_OK on success
 */
nserror nsgtk_toolbar_update(struct nsgtk_toolbar *tb);


/**
 * Update toolbar style and size based on current settings
 *
 * \param toolbar A toolbar returned from a creation
 * \return NSERROR_OK on success
 */
nserror nsgtk_toolbar_restyle(struct nsgtk_toolbar *tb);


/**
 * Start or stop a throbber in a toolbar
 *
 * \param toolbar A toolbar returned from a creation
 * \param active True if the throbber animation should play.
 * \return NSERROR_OK on success
 */
nserror nsgtk_toolbar_throbber(struct nsgtk_toolbar *tb, bool active);


/**
 * Page info has changed state
 *
 * \param toolbar A toolbar returned from a creation
 * \return NSERROR_OK on success
 */
nserror nsgtk_toolbar_page_info_change(struct nsgtk_toolbar *tb);


/**
 * Update the toolbar url entry
 *
 * \param toolbar A toolbar returned from a creation
 * \param url The URL to set
 * \return NSERROR_OK on success
 */
nserror nsgtk_toolbar_set_url(struct nsgtk_toolbar *tb, nsurl *url);


/**
 * set the websearch image
 *
 * \param toolbar A toolbar returned from a creation
 * \param pixbuf The pixel buffer data to use to set the web search icon
 * \return NSERROR_OK on success
 */
nserror nsgtk_toolbar_set_websearch_image(struct nsgtk_toolbar *tb, GdkPixbuf *pixbuf);


/**
 * activate the handler for a toolbar item
 *
 * This allows the same action to be performed for menu enties as if
 *  the user had clicked the toolbar widget.
 *
 * \param toolbar A toolbar returned from a creation
 * \param itemid the id of the item to activate
 * \return NSERROR_OK on success
 */
nserror nsgtk_toolbar_item_activate(struct nsgtk_toolbar *tb, nsgtk_toolbar_button itemid);

/**
 * set the toolbar to be shown or hidden
 *
 * \param toolbar A toolbar returned from a creation
 * \param show true to show the toolbar and false to hide it.
 * \return NSERROR_OK on success
 */
nserror nsgtk_toolbar_show(struct nsgtk_toolbar *tb, bool show);

/**
 * position the page info window appropriately
 *
 * \param tb The toolbar to position relative to
 * \param win The page-info window to position
 */
nserror nsgtk_toolbar_position_page_info(struct nsgtk_toolbar *tb,
					 struct nsgtk_pi_window *win);

/**
 * position the local history window appropriately
 *
 * \param tb The toolbar to position relative to
 */
nserror nsgtk_toolbar_position_local_history(struct nsgtk_toolbar *tb);

/**
 * Initialise customization of toolbar entries
 */
void nsgtk_toolbar_customization_init(struct nsgtk_scaffolding *g);


#endif
