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
#ifndef NETSURF_GTK_MENU_H_
#define NETSURF_GTK_MENU_H_

#include <gtk/gtk.h>

/**
 * File menu item on menubar
 */
struct nsgtk_file_menu {
	GtkMenuItem *file;
	GtkMenu *file_menu;
	GtkWidget *newwindow_menuitem;
	GtkWidget *newtab_menuitem;
	GtkWidget *openfile_menuitem;
	GtkWidget *closewindow_menuitem;
	GtkWidget *export_menuitem;
	struct nsgtk_export_submenu *export_submenu;
	GtkWidget *printpreview_menuitem;
	GtkWidget *print_menuitem;
	GtkWidget *quit_menuitem;
};

struct nsgtk_edit_menu {
	GtkMenuItem *edit; /* Edit menu item on menubar */
	GtkMenu *edit_menu;
	GtkWidget *cut_menuitem;
	GtkWidget *copy_menuitem;
	GtkWidget *paste_menuitem;
	GtkWidget *delete_menuitem;
	GtkWidget *selectall_menuitem;
	GtkWidget *find_menuitem;
	GtkWidget *preferences_menuitem;
};

struct nsgtk_view_menu {
	GtkMenuItem *view; /* View menu item on menubar */
	GtkMenu *view_menu; /* gtk menu attached to menu item */

	GtkWidget *scaleview_menuitem;
	struct nsgtk_scaleview_submenu	*scaleview_submenu;
	GtkWidget *fullscreen_menuitem;
	GtkWidget *toolbars_menuitem;
	struct nsgtk_toolbars_submenu *toolbars_submenu;
	GtkWidget *tabs_menuitem;
	struct nsgtk_tabs_submenu *tabs_submenu;
	GtkWidget *savewindowsize_menuitem;
};

struct nsgtk_nav_menu {
	GtkMenuItem *nav; /* Nav menu item on menubar */
	GtkMenu *nav_menu;

	GtkWidget *back_menuitem;
	GtkWidget *forward_menuitem;
	GtkWidget *stop_menuitem;
	GtkWidget *reload_menuitem;
	GtkWidget *home_menuitem;
	GtkWidget *localhistory_menuitem;
	GtkWidget *globalhistory_menuitem;
	GtkWidget *addbookmarks_menuitem;
	GtkWidget *showbookmarks_menuitem;
	GtkWidget *openlocation_menuitem;
};

struct nsgtk_tools_menu {
	GtkMenuItem  *tools; /* Tools menu item on menubar */
	GtkMenu	*tools_menu;

	GtkWidget *showcookies_menuitem;
	GtkWidget *downloads_menuitem;
	GtkWidget *developer_menuitem;
	struct nsgtk_developer_submenu *developer_submenu;
};

struct nsgtk_help_menu {
	GtkMenuItem *help; /* Help menu item on menubar */
	GtkMenu *help_menu;
	GtkWidget *contents_menuitem;
	GtkWidget *guide_menuitem;
	GtkWidget *info_menuitem;
	GtkWidget *about_menuitem;
};


struct nsgtk_export_submenu {
	GtkMenu *export_menu;
	GtkWidget *savepage_menuitem;
	GtkWidget *plaintext_menuitem;
	GtkWidget *pdf_menuitem;
};

struct nsgtk_scaleview_submenu {
	GtkMenu *scaleview_menu;
	GtkWidget *zoomplus_menuitem;
	GtkWidget *zoomminus_menuitem;
	GtkWidget *zoomnormal_menuitem;
};

struct nsgtk_tabs_submenu {
	GtkMenu *tabs_menu;
	GtkWidget *nexttab_menuitem;
	GtkWidget *prevtab_menuitem;
	GtkWidget *closetab_menuitem;
};

struct nsgtk_toolbars_submenu {
	GtkMenu *toolbars_menu;
	GtkCheckMenuItem *menubar_menuitem;
	GtkCheckMenuItem *toolbar_menuitem;
	GtkWidget *customize_menuitem;
};

struct nsgtk_developer_submenu {
	GtkMenu   *developer_menu;

	GtkWidget *viewsource_menuitem;
	GtkWidget *toggledebugging_menuitem;
	GtkWidget *debugboxtree_menuitem;
	GtkWidget *debugdomtree_menuitem;
};

/**
 * main menubar
 */
struct nsgtk_bar_submenu {
	GtkMenuBar		*bar_menu;
	struct nsgtk_file_menu	*file_submenu;
	struct nsgtk_edit_menu	*edit_submenu;
	struct nsgtk_view_menu	*view_submenu;
	struct nsgtk_nav_menu	*nav_submenu;
	struct nsgtk_tools_menu	*tools_submenu;
	struct nsgtk_help_menu	*help_submenu;
};

struct nsgtk_burger_menu {
	GtkMenu	*burger_menu;

	GtkWidget *file_menuitem;
	struct nsgtk_file_menu *file_submenu;

	GtkWidget *edit_menuitem;
	struct nsgtk_edit_menu *edit_submenu;

	GtkWidget *view_menuitem;
	struct nsgtk_view_menu *view_submenu;

	GtkWidget *nav_menuitem;
	struct nsgtk_nav_menu *nav_submenu;

	GtkWidget *tools_menuitem;
	struct nsgtk_tools_menu *tools_submenu;

	GtkWidget *help_menuitem;
	struct nsgtk_help_menu *help_submenu;
};

struct nsgtk_popup_menu {
	GtkMenu	*popup_menu;

	/* navigation entries */
	GtkWidget *back_menuitem;
	GtkWidget *forward_menuitem;
	GtkWidget *stop_menuitem;
	GtkWidget *reload_menuitem;

	GtkWidget *first_separator;

	/* edit entries */
	GtkWidget *cut_menuitem;
	GtkWidget *copy_menuitem;
	GtkWidget *paste_menuitem;

	GtkWidget *second_separator;

	GtkWidget *toolbars_menuitem;
	struct nsgtk_toolbars_submenu *toolbars_submenu;
	GtkWidget *tools_menuitem;
	struct nsgtk_tools_menu *tools_submenu;
};

struct nsgtk_link_menu {
	GtkMenu	*link_menu;

	GtkWidget *opentab_menuitem;
	GtkWidget *openwin_menuitem;

	GtkWidget *save_menuitem;
	GtkWidget *bookmark_menuitem;
	GtkWidget *copy_menuitem;
};


/**
 * Create main menubar menu.
 */
struct nsgtk_bar_submenu *nsgtk_menu_bar_create(GtkMenuShell *menubar, GtkAccelGroup *group);


/**
 * Generate burger menu.
 *
 * \param accelerator group to use with menu
 * \return new menu structure or NULL on error
 */
struct nsgtk_burger_menu *nsgtk_burger_menu_create(GtkAccelGroup *group);


/**
 * Generate right click popup menu.
 *
 * \param accelerator group to use with menu
 * \return new menu structure or NULL on error
 */
struct nsgtk_popup_menu *nsgtk_popup_menu_create(GtkAccelGroup *group);


/**
 * Generate context sensitive link popup menu.
 *
 * \param accelerator group to use with menu
 * \return new menu structure or NULL on error
 */
struct nsgtk_link_menu *nsgtk_link_menu_create(GtkAccelGroup *group);


/**
 * destroy bar menu
 *
 * destroys the gtk widgets associated with menu and frees all storage.
 *
 * \param menu menu to destroy
 * \return NSERROR_OK and menu destroyed on success else error code
 */
nserror nsgtk_menu_bar_destroy(struct nsgtk_bar_submenu *menu);


/**
 * destroy burger menu
 *
 * destroys the gtk widgets associated with menu and frees all storage.
 *
 * \param menu menu to destroy
 * \return NSERROR_OK and menu destroyed on success else error code
 */
nserror nsgtk_burger_menu_destroy(struct nsgtk_burger_menu *menu);


/**
 * destroy popup menu
 *
 * destroys the gtk widgets associated with menu and frees all storage.
 *
 * \param menu menu to destroy
 * \return NSERROR_OK and menu destroyed on success else error code
 */
nserror nsgtk_popup_menu_destroy(struct nsgtk_popup_menu *menu);


/**
 * destroy link menu
 *
 * destroys the gtk widgets associated with menu and frees all storage.
 *
 * \param menu menu to destroy
 * \return NSERROR_OK and menu destroyed on success else error code
 */
nserror nsgtk_link_menu_destroy(struct nsgtk_link_menu *menu);


#endif
