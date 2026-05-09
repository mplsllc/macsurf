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

#include <stdlib.h>
#include <stdbool.h>

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>

#include "utils/messages.h"

#include "gtk/compat.h"
#include "gtk/menu.h"
#include "gtk/warn.h"
#include "gtk/accelerator.h"

/**
 * Adds image menu item to a menu.
 *
 * \param menu the menu to add the item to
 * \param item_out a pointer to the item's location in the menu struct
 * \param message the menu item I18n lookup value
 * \param group the 'global' in a gtk sense accelerator group
 * \return true if sucessful and \a item_out updated else false.
 */

static bool
nsgtk_menu_add_image_item(GtkMenu *menu,
			  GtkWidget **item_out,
			  const char *message,
			  GtkAccelGroup *group)
{
	unsigned int key;
	GdkModifierType mod;
	GtkWidget *item;
	const char *accelerator_desc; /* accelerator key description */

	item = nsgtk_image_menu_item_new_with_mnemonic(messages_get(message));
	if (item == NULL) {
		return false;
	}

	accelerator_desc = nsgtk_accelerator_get_desc(message);
	if (accelerator_desc != NULL) {
		gtk_accelerator_parse(accelerator_desc, &key, &mod);
		if (key > 0) {
			gtk_widget_add_accelerator(item,
						   "activate",
						   group,
						   key,
						   mod,
						   GTK_ACCEL_VISIBLE);
		}
	}
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	gtk_widget_show(item);

	*item_out = item;

	return true;
}

#define NEW_MENU(n, m)				\
	n = malloc(sizeof(*n));			\
	if (n == NULL) {			\
		return NULL;			\
	}					\
	n->m##_menu = GTK_MENU(gtk_menu_new())

#define IMAGE_ITEM(p, q, r, s, t)\
	nsgtk_menu_add_image_item(s->p##_menu, &(s->q##_menuitem), #r, t)

#define CHECK_ITEM(p, q, r, s)					\
	do {							\
		s->q##_menuitem = GTK_CHECK_MENU_ITEM(		\
			gtk_check_menu_item_new_with_mnemonic(\
			messages_get(#r)));\
		if ((s->q##_menuitem != NULL) && (s->p##_menu != NULL)) { \
			gtk_menu_shell_append(GTK_MENU_SHELL(s->p##_menu), \
					      GTK_WIDGET(s->q##_menuitem)); \
			gtk_widget_show(GTK_WIDGET(s->q##_menuitem));	\
		}							\
	} while(0)

#define SET_SUBMENU(q, r)					\
	do {							\
		r->q##_submenu = nsgtk_menu_##q##_submenu(group);	\
		if ((r->q##_submenu != NULL) &&				\
		    (r->q##_submenu->q##_menu != NULL) &&		\
		    (r->q##_menuitem != NULL)) {			\
			gtk_menu_item_set_submenu(GTK_MENU_ITEM(r->q##_menuitem), \
						  GTK_WIDGET(r->q##_submenu->q##_menu)); \
		}							\
	} while(0)

#define ADD_NAMED_SEP(q, r, s)						\
	do {								\
		s->r##_separator = gtk_separator_menu_item_new();	\
		if ((s->r##_separator != NULL) && (s->q##_menu != NULL)) { \
			gtk_menu_shell_append(GTK_MENU_SHELL(s->q##_menu), s->r##_separator); \
			gtk_widget_show(s->r##_separator);		\
		}							\
	} while(0)

#define ADD_SEP(q, r)							\
	do {								\
		GtkWidget *w = gtk_separator_menu_item_new();		\
		if ((w != NULL) && (r->q##_menu != NULL)) {		\
			gtk_menu_shell_append(GTK_MENU_SHELL(r->q##_menu), w); \
			gtk_widget_show(w);				\
		}							\
	} while(0)

#define ATTACH_PARENT(parent, msgname, menuv, group)			\
	do {								\
		/* create top level menu entry and attach to parent */	\
		menuv = GTK_MENU_ITEM(gtk_menu_item_new_with_mnemonic(messages_get(#msgname))); \
		gtk_menu_shell_append(parent, GTK_WIDGET(menuv));	\
		gtk_widget_show(GTK_WIDGET(menuv));			\
		/* attach submenu to parent */				\
		gtk_menu_item_set_submenu(menuv, GTK_WIDGET(menuv##_menu)); \
		gtk_menu_set_accel_group(menuv##_menu, group);		\
	} while(0)

/**
* creates an export submenu
* \param group the 'global' in a gtk sense accelerator reference
*/
static struct nsgtk_export_submenu *
nsgtk_menu_export_submenu(GtkAccelGroup *group)
{
	struct nsgtk_export_submenu *ret;
	ret = malloc(sizeof(struct nsgtk_export_submenu));

	if (ret == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		return NULL;
	}

	ret->export_menu = GTK_MENU(gtk_menu_new());
	if (ret->export_menu == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		free(ret);
		return NULL;
	}

	IMAGE_ITEM(export, savepage, gtkSavePage, ret, group);
	IMAGE_ITEM(export, plaintext, gtkPlainText, ret, group);
	IMAGE_ITEM(export, pdf, gtkPDF, ret, group);
	return ret;
}

/**
* creates a scaleview submenu
* \param group the 'global' in a gtk sense accelerator reference
*/

static struct nsgtk_scaleview_submenu *
nsgtk_menu_scaleview_submenu(GtkAccelGroup *group)
{
	struct nsgtk_scaleview_submenu *ret =
			malloc(sizeof(struct nsgtk_scaleview_submenu));
	if (ret == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		return NULL;
	}
	ret->scaleview_menu = GTK_MENU(gtk_menu_new());
	if (ret->scaleview_menu == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		free(ret);
		return NULL;
	}
	IMAGE_ITEM(scaleview, zoomplus, gtkZoomPlus, ret, group);
	IMAGE_ITEM(scaleview, zoomnormal, gtkZoomNormal, ret, group);
	IMAGE_ITEM(scaleview, zoomminus, gtkZoomMinus, ret, group);
	return ret;
}

/**
* creates a tab navigation submenu
* \param group the 'global' in a gtk sense accelerator reference
*/

static struct nsgtk_tabs_submenu *nsgtk_menu_tabs_submenu(GtkAccelGroup *group)
{
	struct nsgtk_tabs_submenu *ret;
	ret = malloc(sizeof(struct nsgtk_tabs_submenu));
	if (ret == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		return NULL;
	}
	ret->tabs_menu = GTK_MENU(gtk_menu_new());
	if (ret->tabs_menu == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		free(ret);
		return NULL;
	}
	IMAGE_ITEM(tabs, nexttab, gtkNextTab, ret, group);
	IMAGE_ITEM(tabs, prevtab, gtkPrevTab, ret, group);
	IMAGE_ITEM(tabs, closetab, gtkCloseTab, ret, group);

	return ret;
}


/**
 * creates a toolbars submenu
 *
 * \param group the 'global' in a gtk sense accelerator reference
 */
static struct nsgtk_toolbars_submenu *
nsgtk_menu_toolbars_submenu(GtkAccelGroup *group)
{
	struct nsgtk_toolbars_submenu *tmenu;

	tmenu = malloc(sizeof(struct nsgtk_toolbars_submenu));
	if (tmenu == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		return NULL;
	}

	tmenu->toolbars_menu = GTK_MENU(gtk_menu_new());
	if (tmenu->toolbars_menu == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		free(tmenu);
		return NULL;
	}

	CHECK_ITEM(toolbars, menubar, gtkMenuBar, tmenu);
	if (tmenu->menubar_menuitem != NULL) {
		gtk_check_menu_item_set_active(tmenu->menubar_menuitem, TRUE);
	}

	CHECK_ITEM(toolbars, toolbar, gtkToolBar, tmenu);
	if (tmenu->toolbar_menuitem != NULL) {
		gtk_check_menu_item_set_active(tmenu->toolbar_menuitem, TRUE);
	}

	ADD_SEP(toolbars, tmenu);

	IMAGE_ITEM(toolbars, customize, gtkCustomize, tmenu, group);

	return tmenu;
}

/**
* creates a debugging submenu
* \param group the 'global' in a gtk sense accelerator reference
*/

static struct nsgtk_developer_submenu *
nsgtk_menu_developer_submenu(GtkAccelGroup *group)
{
	struct nsgtk_developer_submenu *dmenu =
			malloc(sizeof(struct nsgtk_developer_submenu));
	if (dmenu == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		return NULL;
	}
	dmenu->developer_menu = GTK_MENU(gtk_menu_new());
	if (dmenu->developer_menu == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		free(dmenu);
		return NULL;
	}

	IMAGE_ITEM(developer, viewsource, gtkPageSource, dmenu, group);
	IMAGE_ITEM(developer, toggledebugging, gtkToggleDebugging, dmenu, group);
	IMAGE_ITEM(developer, debugboxtree, gtkDebugBoxTree, dmenu, group);
	IMAGE_ITEM(developer, debugdomtree, gtkDebugDomTree, dmenu, group);

	return dmenu;
}

/**
 * creates the file menu
 *
 * \param group The gtk 'global' accelerator reference
 * \return The new file menu or NULL on error
 */
static struct nsgtk_file_menu *nsgtk_menu_file_submenu(GtkAccelGroup *group)
{
	struct nsgtk_file_menu *fmenu;

	fmenu = malloc(sizeof(struct nsgtk_file_menu));
	if (fmenu == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		return NULL;
	}

	fmenu->file_menu = GTK_MENU(gtk_menu_new());
	if (fmenu->file_menu == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		free(fmenu);
		return NULL;
	}

	IMAGE_ITEM(file, newwindow, gtkNewWindow, fmenu, group);
	IMAGE_ITEM(file, newtab, gtkNewTab, fmenu, group);
	IMAGE_ITEM(file, openfile, gtkOpenFile, fmenu, group);
	IMAGE_ITEM(file, closewindow, gtkCloseWindow, fmenu, group);
	ADD_SEP(file, fmenu);
	IMAGE_ITEM(file, export, gtkExport, fmenu, group);
	ADD_SEP(file, fmenu);
	IMAGE_ITEM(file, printpreview, gtkPrintPreview, fmenu, group);
	IMAGE_ITEM(file, print, gtkPrint, fmenu, group);
	ADD_SEP(file, fmenu);
	IMAGE_ITEM(file, quit, gtkQuitMenu, fmenu, group);
	SET_SUBMENU(export, fmenu);

	return fmenu;
}

/**
* creates an edit menu
* \param group the 'global' in a gtk sense accelerator reference
*/

static struct nsgtk_edit_menu *nsgtk_menu_edit_submenu(GtkAccelGroup *group)
{
	struct nsgtk_edit_menu *ret = malloc(sizeof(struct nsgtk_edit_menu));
	if (ret == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		return NULL;
	}
	ret->edit_menu = GTK_MENU(gtk_menu_new());
	if (ret->edit_menu == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		free(ret);
		return NULL;
	}
	IMAGE_ITEM(edit, cut, gtkCut, ret, group);
	IMAGE_ITEM(edit, copy, gtkCopy, ret, group);
	IMAGE_ITEM(edit, paste, gtkPaste, ret, group);
	IMAGE_ITEM(edit, delete, gtkDelete, ret, group);
	ADD_SEP(edit, ret);
	IMAGE_ITEM(edit, selectall, gtkSelectAll, ret, group);
	ADD_SEP(edit, ret);
	IMAGE_ITEM(edit, find, gtkFind, ret, group);
	ADD_SEP(edit, ret);
	IMAGE_ITEM(edit, preferences, gtkPreferences, ret, group);

	return ret;
}

/**
* creates a view menu
* \param group the 'global' in a gtk sense accelerator reference
*/

static struct nsgtk_view_menu *nsgtk_menu_view_submenu(GtkAccelGroup *group)
{
	struct nsgtk_view_menu *ret = malloc(sizeof(struct nsgtk_view_menu));
	if (ret == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		return NULL;
	}
	ret->view_menu = GTK_MENU(gtk_menu_new());
	if (ret->view_menu == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		free(ret);
		return NULL;
	}
	IMAGE_ITEM(view, scaleview, gtkScaleView, ret, group);
	SET_SUBMENU(scaleview, ret);
	IMAGE_ITEM(view, fullscreen, gtkFullScreen, ret, group);
	ADD_SEP(view, ret);
	IMAGE_ITEM(view, toolbars, gtkToolbars, ret, group);
	SET_SUBMENU(toolbars, ret);
	IMAGE_ITEM(view, tabs, gtkTabs, ret, group);
	SET_SUBMENU(tabs, ret);
	ADD_SEP(view, ret);
	IMAGE_ITEM(view, savewindowsize, gtkSaveWindowSize, ret, group);

	return ret;
}

/**
* creates a nav menu
* \param group the 'global' in a gtk sense accelerator reference
*/

static struct nsgtk_nav_menu *nsgtk_menu_nav_submenu(GtkAccelGroup *group)
{
	struct nsgtk_nav_menu *ret = malloc(sizeof(struct nsgtk_nav_menu));
	if (ret == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		return NULL;
	}
	ret->nav_menu = GTK_MENU(gtk_menu_new());
	if (ret->nav_menu == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		free(ret);
		return NULL;
	}

	IMAGE_ITEM(nav, back, gtkBack, ret, group);
	IMAGE_ITEM(nav, forward, gtkForward, ret, group);
	IMAGE_ITEM(nav, stop, gtkStop, ret, group);
	IMAGE_ITEM(nav, reload, gtkReload, ret, group);
	IMAGE_ITEM(nav, home, gtkHome, ret, group);
	ADD_SEP(nav, ret);
	IMAGE_ITEM(nav, localhistory, gtkLocalHistory, ret, group);
	IMAGE_ITEM(nav, globalhistory, gtkGlobalHistory, ret, group);
	ADD_SEP(nav, ret);
	IMAGE_ITEM(nav, addbookmarks, gtkAddBookMarks, ret, group);
	IMAGE_ITEM(nav, showbookmarks, gtkShowBookMarks, ret, group);
	ADD_SEP(nav, ret);
	IMAGE_ITEM(nav, openlocation, gtkOpenLocation, ret, group);

	return ret;
}

/**
 * creates the tools menu
 * \param group the 'global' in a gtk sense accelerator reference
 */
static struct nsgtk_tools_menu *nsgtk_menu_tools_submenu(GtkAccelGroup *group)
{
	struct nsgtk_tools_menu *ret = malloc(sizeof(struct nsgtk_tools_menu));
	if (ret == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		return NULL;
	}
	ret->tools_menu = GTK_MENU(gtk_menu_new());
	if (ret->tools_menu == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		free(ret);
		return NULL;
	}

	IMAGE_ITEM(tools, downloads, gtkDownloads, ret, group);
	IMAGE_ITEM(tools, showcookies, gtkShowCookies, ret, group);
	IMAGE_ITEM(tools, developer, gtkDeveloper, ret, group);
	SET_SUBMENU(developer, ret);

	return ret;
}

/**
 * creates a help menu
 * \param group the 'global' in a gtk sense accelerator reference
 */
static struct nsgtk_help_menu *nsgtk_menu_help_submenu(GtkAccelGroup *group)
{
	struct nsgtk_help_menu *ret = malloc(sizeof(struct nsgtk_help_menu));
	if (ret == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		return NULL;
	}
	ret->help_menu = GTK_MENU(gtk_menu_new());
	if (ret->help_menu == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		free(ret);
		return NULL;
	}
	IMAGE_ITEM(help, contents, gtkContents, ret, group);
	IMAGE_ITEM(help, guide, gtkGuide, ret, group);
	IMAGE_ITEM(help, info, gtkUserInformation, ret, group);
	ADD_SEP(help, ret);
	IMAGE_ITEM(help, about, gtkAbout, ret, group);

	return ret;
}


/**
 * Generate menubar menus.
 *
 * Generate the main menu structure and attach it to a menubar widget.
 */
struct nsgtk_bar_submenu *
nsgtk_menu_bar_create(GtkMenuShell *menubar, GtkAccelGroup *group)
{
	struct nsgtk_bar_submenu *nmenu;

	nmenu = calloc(1, sizeof(struct nsgtk_bar_submenu));
	if (nmenu == NULL) {
		return NULL;
	}

	/* create sub menus */
	nmenu->file_submenu = nsgtk_menu_file_submenu(group);
	nmenu->edit_submenu = nsgtk_menu_edit_submenu(group);
	nmenu->view_submenu = nsgtk_menu_view_submenu(group);
	nmenu->nav_submenu = nsgtk_menu_nav_submenu(group);
	nmenu->tools_submenu = nsgtk_menu_tools_submenu(group);
	nmenu->help_submenu = nsgtk_menu_help_submenu(group);

	if (menubar != NULL) {
		nmenu->bar_menu = GTK_MENU_BAR(menubar);

		/* attach menus to menubar */
		ATTACH_PARENT(menubar, gtkFile, nmenu->file_submenu->file, group);
		ATTACH_PARENT(menubar, gtkEdit, nmenu->edit_submenu->edit, group);
		ATTACH_PARENT(menubar, gtkView, nmenu->view_submenu->view, group);
		ATTACH_PARENT(menubar, gtkNavigate, nmenu->nav_submenu->nav, group);
		ATTACH_PARENT(menubar, gtkTools, nmenu->tools_submenu->tools, group);
		ATTACH_PARENT(menubar, gtkHelp, nmenu->help_submenu->help, group);
	}

	return nmenu;
}


/* exported function documented in gtk/menu.h */
struct nsgtk_burger_menu *nsgtk_burger_menu_create(GtkAccelGroup *group)
{
	struct nsgtk_burger_menu *nmenu;

	NEW_MENU(nmenu, burger);

	IMAGE_ITEM(burger, file, gtkFile, nmenu, group);
	SET_SUBMENU(file, nmenu);

	IMAGE_ITEM(burger, edit, gtkEdit, nmenu, group);
	SET_SUBMENU(edit, nmenu);

	IMAGE_ITEM(burger, view, gtkView, nmenu, group);
	SET_SUBMENU(view, nmenu);

	IMAGE_ITEM(burger, nav, gtkNavigate, nmenu, group);
	SET_SUBMENU(nav, nmenu);

	IMAGE_ITEM(burger, tools, gtkTools, nmenu, group);
	SET_SUBMENU(tools, nmenu);

	IMAGE_ITEM(burger, help, gtkHelp, nmenu, group);
	SET_SUBMENU(help, nmenu);

	return nmenu;
}


/* exported function documented in gtk/menu.h */
struct nsgtk_popup_menu *nsgtk_popup_menu_create(GtkAccelGroup *group)
{
	struct nsgtk_popup_menu *nmenu;

	NEW_MENU(nmenu, popup);

	IMAGE_ITEM(popup, back, gtkBack, nmenu, group);
	IMAGE_ITEM(popup, forward, gtkForward, nmenu, group);
	IMAGE_ITEM(popup, stop, gtkStop, nmenu, group);
	IMAGE_ITEM(popup, reload, gtkReload, nmenu, group);

	ADD_NAMED_SEP(popup, first, nmenu);

	IMAGE_ITEM(popup, cut, gtkCut, nmenu, group);
	IMAGE_ITEM(popup, copy, gtkCopy, nmenu, group);
	IMAGE_ITEM(popup, paste, gtkPaste, nmenu, group);

	ADD_NAMED_SEP(popup, second, nmenu);

	IMAGE_ITEM(popup, toolbars, gtkToolbars, nmenu, group);
	SET_SUBMENU(toolbars, nmenu);

	IMAGE_ITEM(popup, tools, gtkTools, nmenu, group);
	SET_SUBMENU(tools, nmenu);

	return nmenu;
}


/* exported function documented in gtk/menu.h */
struct nsgtk_link_menu *
nsgtk_link_menu_create(GtkAccelGroup *group)
{
	struct nsgtk_link_menu *nmenu;

	NEW_MENU(nmenu, link);

	IMAGE_ITEM(link, opentab, gtkOpentab, nmenu, group);
	IMAGE_ITEM(link, openwin, gtkOpenwin, nmenu, group);

	ADD_SEP(link, nmenu);

	IMAGE_ITEM(link, save, gtkSavelink, nmenu, group);
	IMAGE_ITEM(link, bookmark, gtkBookmarklink, nmenu, group);
	IMAGE_ITEM(link, copy, gtkCopylink, nmenu, group);

	return nmenu;
}


/* exported function documented in gtk/menu.h */
nserror nsgtk_menu_bar_destroy(struct nsgtk_bar_submenu *menu)
{
	gtk_widget_destroy(GTK_WIDGET(menu->bar_menu));

	free(menu->file_submenu->export_submenu);
	free(menu->file_submenu);
	free(menu->edit_submenu);
	free(menu->view_submenu->tabs_submenu);
	free(menu->view_submenu->toolbars_submenu);
	free(menu->view_submenu->scaleview_submenu);
	free(menu->view_submenu);
	free(menu->nav_submenu);
	free(menu->tools_submenu->developer_submenu);
	free(menu->tools_submenu);
	free(menu->help_submenu);
	free(menu);

	return NSERROR_OK;
}

/* exported function documented in gtk/menu.h */
nserror nsgtk_burger_menu_destroy(struct nsgtk_burger_menu *menu)
{
	gtk_widget_destroy(GTK_WIDGET(menu->burger_menu));

	free(menu->file_submenu->export_submenu);
	free(menu->file_submenu);
	free(menu->edit_submenu);
	free(menu->view_submenu->tabs_submenu);
	free(menu->view_submenu->toolbars_submenu);
	free(menu->view_submenu->scaleview_submenu);
	free(menu->view_submenu);
	free(menu->nav_submenu);
	free(menu->tools_submenu->developer_submenu);
	free(menu->tools_submenu);
	free(menu->help_submenu);
	free(menu);

	return NSERROR_OK;
}


/* exported function documented in gtk/menu.h */
nserror nsgtk_popup_menu_destroy(struct nsgtk_popup_menu *menu)
{
	gtk_widget_destroy(GTK_WIDGET(menu->popup_menu));

	free(menu->toolbars_submenu);
	free(menu->tools_submenu->developer_submenu);
	free(menu->tools_submenu);
	free(menu);

	return NSERROR_OK;
}


/* exported function documented in gtk/menu.h */
nserror nsgtk_link_menu_destroy(struct nsgtk_link_menu *menu)
{
	gtk_widget_destroy(GTK_WIDGET(menu->link_menu));

	free(menu);

	return NSERROR_OK;
}
