/*
 * Copyright 2008 Michael Lester <element3260@gmail.com>
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

#ifndef NETSURF_GTK_TABS_H_
#define NETSURF_GTK_TABS_H_

struct gui_window;

/**
 * create notebook
 *
 * creates a notebook for use inside a window, creates the special add
 *   page(tab) and attaches all signals.
 *
 * \param builder the gtk builder object to create notbook from
 * \param notebook_out reciveds the created notebook
 * \return NSERROR_OK and notebook_out updated else error code
 */
nserror nsgtk_notebook_create(GtkBuilder *builder, GtkNotebook **notebook_out);

/**
 * Add new gui window page to notebook.
 */
void nsgtk_tab_add(struct gui_window *window, GtkWidget *tab_contents, bool background, const char *title, GdkPixbuf *icon_pixbuf);

/**
 * Add new page to a notebook
 */
nserror nsgtk_tab_add_page(GtkNotebook *notebook, GtkWidget *tab_contents, bool background, const char *title, GdkPixbuf *icon_pixbuf);


/**
 * set the tab title
 *
 * The tab title will be set to the parameter
 *
 * \note currently only called from nsgtk_window_set_title()
 *
 * \param page The page widget that was added to the notebook
 * \param title The title text which may not be NULL.
 * \return NSERROR_OK on sucess else appropriate code.
 */
nserror nsgtk_tab_set_title(GtkWidget *page, const char *title);

/**
 * set the tab icon
 *
 * The tab icon will be set to the \a pixbuf parameter
 *
 * \param page The page widget that was added to the notebook
 * \param pixbuf The pixbuf to set the icon to.
 * \return NSERROR_OK on sucess else appropriate code.
 */
nserror nsgtk_tab_set_icon(GtkWidget *page, GdkPixbuf *pixbuf);

void nsgtk_tab_options_changed(GtkNotebook *notebook);
nserror nsgtk_tab_close_current(GtkNotebook *notebook);
nserror nsgtk_tab_prev(GtkNotebook *notebook);
nserror nsgtk_tab_next(GtkNotebook *notebook);

#endif
