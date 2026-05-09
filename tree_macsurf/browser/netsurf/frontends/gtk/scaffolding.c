/*
 * Copyright 2019 Vincent Sanders <vince@netsurf-browser.org>
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

#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "utils/utils.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsurl.h"
#include "utils/nsoption.h"
#include "netsurf/browser_window.h"
#include "desktop/browser_history.h"
#include "desktop/hotlist.h"

#include "gtk/compat.h"
#include "gtk/toolbar_items.h"
#include "gtk/menu.h"
#include "gtk/local_history.h"
#include "gtk/gui.h"
#include "gtk/download.h"
#include "gtk/window.h"
#include "gtk/warn.h"
#include "gtk/tabs.h"
#include "gtk/resources.h"
#include "gtk/scaffolding.h"


/**
 * menu entry context
 */
struct nsgtk_menu {
	GtkWidget *main; /* main menu entry */
	GtkWidget *burger; /* right click menu */
	GtkWidget *popup; /* popup menu entry */
	/**
	 * menu item handler
	 */
	gboolean (*mhandler)(GtkMenuItem *widget, gpointer data);
	const char *iconname; /* name of the icon to use */
	bool sensitivity; /* menu item is sensitive */
};

/**
 * Core scaffolding structure.
 */
struct nsgtk_scaffolding {
	/** global linked list of scaffolding for gui interface adjustments */
	struct nsgtk_scaffolding *next, *prev;

	/** currently active gui browsing context */
	struct gui_window *top_level;

	/** Builder object scaffold was created from */
	GtkBuilder *builder;

	/** scaffold container window */
	GtkWindow *window;

	/** tab widget holding displayed pages */
	GtkNotebook *notebook;

	/** handler id for tabs remove callback */
	gulong tabs_remove_handler_id;

	/** menu bar hierarchy */
	struct nsgtk_bar_submenu *menu_bar;

	/** burger menu hierarchy */
	struct nsgtk_burger_menu *burger_menu;

	/** right click popup menu hierarchy */
	struct nsgtk_popup_menu *popup_menu;

	/** link popup menu */
	struct nsgtk_link_menu *link_menu;

	/** menu entries widgets for sensitivity adjustment */
	struct nsgtk_menu menus[PLACEHOLDER_BUTTON];
};

/**
 * current scaffold for model dialogue use
 */
static struct nsgtk_scaffolding *scaf_current;

/**
 * global list for interface changes
 */
static struct nsgtk_scaffolding *scaf_list = NULL;

/**
 * holds the context data for what's under the pointer, when the
 *  contextual menu is opened.
 */
static struct browser_window_features current_menu_features;


/**
 * Helper to hide popup menu entries by grouping.
 *
 * \param menu The popup menu to modify.
 * \param nav flag to indicate if navigation entries should be hidden.
 * \param cnp flag to indicate if cut and paste entries should be hidden.
 * \param custom flag to indicate if menu customisation is hidden.
 */
static void
popup_menu_hide(struct nsgtk_popup_menu *menu, bool nav, bool cnp)
{
	if (nav) {
		gtk_widget_hide(GTK_WIDGET(menu->back_menuitem));
		gtk_widget_hide(GTK_WIDGET(menu->forward_menuitem));
		gtk_widget_hide(GTK_WIDGET(menu->stop_menuitem));
		gtk_widget_hide(GTK_WIDGET(menu->reload_menuitem));

		gtk_widget_hide(menu->first_separator);
	}

	if (cnp) {
		gtk_widget_hide(GTK_WIDGET(menu->cut_menuitem));
		gtk_widget_hide(GTK_WIDGET(menu->copy_menuitem));
		gtk_widget_hide(GTK_WIDGET(menu->paste_menuitem));

		gtk_widget_hide(menu->second_separator);
	}


}


/**
 * Helper to show popup menu entries by grouping.
 *
 * \param menu The popup menu to modify.
 * \param nav flag to indicate if navigation entries should be visible.
 * \param cnp flag to indicate if cut and paste entries should be visible.
 * \param custom flag to indicate if menu customisation is visible.
 */
static void
popup_menu_show(struct nsgtk_popup_menu *menu, bool nav, bool cnp)
{
	if (nav) {
		gtk_widget_show(GTK_WIDGET(menu->back_menuitem));
		gtk_widget_show(GTK_WIDGET(menu->forward_menuitem));
		gtk_widget_show(GTK_WIDGET(menu->stop_menuitem));
		gtk_widget_show(GTK_WIDGET(menu->reload_menuitem));

		gtk_widget_show(menu->first_separator);
	}

	if (cnp) {
		gtk_widget_show(GTK_WIDGET(menu->cut_menuitem));
		gtk_widget_show(GTK_WIDGET(menu->copy_menuitem));
		gtk_widget_show(GTK_WIDGET(menu->paste_menuitem));

		gtk_widget_show(menu->second_separator);
	}

}


/**
 * resource cleanup function for window destruction.
 *
 * gtk event called when window is being destroyed. Need to free any
 * resources associated with this scaffold,
 *
 * \param widget the widget being destroyed
 * \param data The context pointer passed when the connection was made.
 */
static void scaffolding_window_destroy(GtkWidget *widget, gpointer data)
{
	struct nsgtk_scaffolding *gs = data;

	NSLOG(netsurf, INFO, "scaffold:%p", gs);

	nsgtk_local_history_hide();

	/* ensure scaffolding being destroyed is not current */
	if (scaf_current == gs) {
		scaf_current = NULL;
		/* attempt to select nearest scaffold instead of just selecting the first */
		if (gs->prev != NULL) {
			scaf_current = gs->prev;
		} else if (gs->next != NULL) {
			scaf_current = gs->next;
		}
	}

	/* remove scaffolding from list */
	if (gs->prev != NULL) {
		gs->prev->next = gs->next;
	} else {
		scaf_list = gs->next;
	}
	if (gs->next != NULL) {
		gs->next->prev = gs->prev;
	}

	NSLOG(netsurf, INFO, "scaffold list head: %p", scaf_list);

	/* ensure menu resources are freed */
	nsgtk_menu_bar_destroy(gs->menu_bar);
	nsgtk_burger_menu_destroy(gs->burger_menu);
	nsgtk_popup_menu_destroy(gs->popup_menu);
	nsgtk_link_menu_destroy(gs->link_menu);

	g_signal_handler_disconnect(gs->notebook, gs->tabs_remove_handler_id);

	free(gs);

	if (scaf_list == NULL) {
		/* no more open windows - stop the browser */
		nsgtk_complete = true;
	}
}


/**
 * gtk event callback on window delete event.
 *
 * prevent window close if download is in progress
 *
 * \param widget The widget receiving the delete event
 * \param event The event
 * \param data The context pointer passed when the connection was made.
 * \return TRUE to indicate message handled.
 */
static gboolean
scaffolding_window_delete_event(GtkWidget *widget,
				GdkEvent *event,
				gpointer data)
{
	struct nsgtk_scaffolding *g = data;

	if (nsgtk_check_for_downloads(GTK_WINDOW(widget)) == false) {
		gtk_widget_destroy(GTK_WIDGET(g->window));
	}
	return TRUE;
}


/**
 * Update the scaffolding controls
 *
 * The button sensitivity, url bar and local history visibility are updated
 *
 * \param g The scaffolding context to update
 */
static void scaffolding_update_context(struct nsgtk_scaffolding *g)
{
	struct browser_window *bw = nsgtk_get_browser_window(g->top_level);

	g->menus[BACK_BUTTON].sensitivity =
		browser_window_history_back_available(bw);
	g->menus[FORWARD_BUTTON].sensitivity =
		browser_window_history_forward_available(bw);

	nsgtk_scaffolding_set_sensitivity(g);

	nsgtk_local_history_hide();
}


/**
 * edit the sensitivity of focused widget
 *
 * \todo this needs to update toolbar sensitivity
 *
 * \param g The scaffolding context.
 */
static guint
nsgtk_scaffolding_update_edit_actions_sensitivity(struct nsgtk_scaffolding *g)
{
	GtkWidget *widget = gtk_window_get_focus(g->window);

	if (GTK_IS_EDITABLE(widget)) {
		gboolean has_selection;
		has_selection = gtk_editable_get_selection_bounds(
					GTK_EDITABLE(widget), NULL, NULL);
		g->menus[COPY_BUTTON].sensitivity = has_selection;
		g->menus[CUT_BUTTON].sensitivity = has_selection;
		g->menus[PASTE_BUTTON].sensitivity = true;
	} else {
		struct browser_window *bw =
			nsgtk_get_browser_window(g->top_level);
		browser_editor_flags edit_f =
			browser_window_get_editor_flags(bw);

		g->menus[COPY_BUTTON].sensitivity =
			edit_f & BW_EDITOR_CAN_COPY;
		g->menus[CUT_BUTTON].sensitivity =
			edit_f & BW_EDITOR_CAN_CUT;
		g->menus[PASTE_BUTTON].sensitivity =
			edit_f & BW_EDITOR_CAN_PASTE;
	}

	nsgtk_scaffolding_set_sensitivity(g);

	return ((g->menus[COPY_BUTTON].sensitivity) |
		(g->menus[CUT_BUTTON].sensitivity) |
		(g->menus[PASTE_BUTTON].sensitivity));
}


/**
 * make edit actions sensitive
 *
 * \todo toolbar sensitivity
 *
 * \param g The scaffolding context.
 */
static void
nsgtk_scaffolding_enable_edit_actions_sensitivity(struct nsgtk_scaffolding *g)
{
	g->menus[PASTE_BUTTON].sensitivity = true;
	g->menus[COPY_BUTTON].sensitivity = true;
	g->menus[CUT_BUTTON].sensitivity = true;
	nsgtk_scaffolding_set_sensitivity(g);

	popup_menu_show(g->popup_menu, false, true);
}

/* signal handling functions for the toolbar, URL bar, and menu bar */

/**
 * gtk event for edit menu being show
 *
 * \param widget The menu widget
 * \param g scaffolding handle
 * \return TRUE to indicate event handled
 */
static gboolean
nsgtk_window_edit_menu_shown(GtkWidget *widget,
			     struct nsgtk_scaffolding *g)
{
	nsgtk_scaffolding_update_edit_actions_sensitivity(g);

	return TRUE;
}

/**
 * gtk event handler for edit menu being hidden
 *
 * \param widget The menu widget
 * \param g scaffolding handle
 * \return TRUE to indicate event handled
 */
static gboolean
nsgtk_window_edit_menu_hidden(GtkWidget *widget,
			      struct nsgtk_scaffolding *g)
{
	nsgtk_scaffolding_enable_edit_actions_sensitivity(g);

	return TRUE;
}


/**
 * gtk event handler for popup menu being hidden.
 *
 * \param widget The menu widget
 * \param g scaffolding handle
 * \return TRUE to indicate event handled
 */
static gboolean
nsgtk_window_popup_menu_hidden(GtkWidget *widget, struct nsgtk_scaffolding *g)
{
	nsgtk_scaffolding_enable_edit_actions_sensitivity(g);
	return TRUE;
}


/**
 * Update the menus when the number of tabs changes.
 *
 * \todo toolbar sensitivity
 * \todo next/previous tab ought to only be visible if there is such a tab
 *
 * \param notebook The notebook all the tabs are in
 * \param page The newly added page container widget
 * \param page_num The index of the newly added page
 * \param g The scaffolding context containing the notebook
 */
static void
nsgtk_window_tabs_add(GtkNotebook *notebook,
		      GtkWidget *page,
		      guint page_num,
		      struct nsgtk_scaffolding *g)
{
	gboolean visible = gtk_notebook_get_show_tabs(g->notebook);
	g_object_set(g->menu_bar->view_submenu->tabs_menuitem,
		     "visible",
		     visible,
		     NULL);
	g_object_set(g->burger_menu->view_submenu->tabs_menuitem,
		     "visible",
		     visible,
		     NULL);

	g->menus[NEXTTAB_BUTTON].sensitivity = visible;
	g->menus[PREVTAB_BUTTON].sensitivity = visible;
	g->menus[CLOSETAB_BUTTON].sensitivity = visible;

	nsgtk_scaffolding_set_sensitivity(g);
}


/**
 * Update the menus when the number of tabs changes.
 *
 * \todo toolbar sensitivity
 *
 * \param notebook The notebook all the tabs are in
 * \param page The page container widget being removed
 * \param page_num The index of the removed page
 * \param gs The scaffolding context containing the notebook
 */
static void
nsgtk_window_tabs_remove(GtkNotebook *notebook,
			 GtkWidget *page,
			 guint page_num,
			 struct nsgtk_scaffolding *gs)
{
	gboolean visible;

	/* if the scaffold is being destroyed it is not useful to
	 * update the state, further many of the widgets may have
	 * already been destroyed.
	 */
	if (gtk_widget_in_destruction(GTK_WIDGET(gs->window)) == TRUE) {
		return;
	}

	/* if this is the last tab destroy the scaffold in addition */
	if (gtk_notebook_get_n_pages(notebook) == 1) {
		gtk_widget_destroy(GTK_WIDGET(gs->window));
		return;
	}

	visible = gtk_notebook_get_show_tabs(gs->notebook);
	g_object_set(gs->menu_bar->view_submenu->tabs_menuitem,
		     "visible", visible, NULL);
	g_object_set(gs->burger_menu->view_submenu->tabs_menuitem,
		     "visible", visible, NULL);

	gs->menus[NEXTTAB_BUTTON].sensitivity = visible;
	gs->menus[PREVTAB_BUTTON].sensitivity = visible;
	gs->menus[CLOSETAB_BUTTON].sensitivity = visible;

	nsgtk_scaffolding_set_sensitivity(gs);
}


/* signal handlers for menu entries */

/**
 * handle menu activate signals by calling toolbar item activation
 */
#define TOOLBAR_ITEM_p(identifier, name)				\
	static gboolean							\
nsgtk_on_##name##_activate_menu(GtkMenuItem *widget, gpointer data)	\
{									\
	struct nsgtk_scaffolding *gs = (struct nsgtk_scaffolding *)data;\
	nsgtk_window_item_activate(gs->top_level, identifier);		\
	return TRUE;							\
}
#define TOOLBAR_ITEM_y(identifier, name)
#define TOOLBAR_ITEM_n(identifier, name)
#define TOOLBAR_ITEM(identifier, name, sensitivity, clicked, activate, label, iconame) \
	TOOLBAR_ITEM_ ## activate(identifier, name)
#include "gtk/toolbar_items.h"
#undef TOOLBAR_ITEM_y
#undef TOOLBAR_ITEM_n
#undef TOOLBAR_ITEM_p
#undef TOOLBAR_ITEM


static gboolean
nsgtk_on_savelink_activate_menu(GtkMenuItem *widget, gpointer data)
{
	struct nsgtk_scaffolding *g = (struct nsgtk_scaffolding *) data;
	struct gui_window *gui = g->top_level;
	struct browser_window *bw = nsgtk_get_browser_window(gui);
	nserror err;

	if (current_menu_features.link == NULL)
		return FALSE;

	err = browser_window_navigate(bw,
				current_menu_features.link,
				NULL,
				BW_NAVIGATE_DOWNLOAD,
				NULL,
				NULL,
				NULL);
	if (err != NSERROR_OK) {
		nsgtk_warning(messages_get_errorcode(err), 0);
	}

	return TRUE;
}


/**
 * Handler for opening new window from a link. attached to the popup menu.
 */
static gboolean
nsgtk_on_link_openwin_activate_menu(GtkMenuItem *widget, gpointer data)
{
	struct nsgtk_scaffolding *g = (struct nsgtk_scaffolding *) data;
	struct gui_window *gui = g->top_level;
	struct browser_window *bw = nsgtk_get_browser_window(gui);
	nserror err;

	if (current_menu_features.link == NULL)
		return FALSE;

	err = browser_window_create(BW_CREATE_CLONE | BW_CREATE_HISTORY,
				current_menu_features.link, NULL, bw, NULL);
	if (err != NSERROR_OK) {
		nsgtk_warning(messages_get_errorcode(err), 0);
	}

	return TRUE;
}


/**
 * Handler for opening new tab from a link. attached to the popup menu.
 */
static gboolean
nsgtk_on_link_opentab_activate_menu(GtkMenuItem *widget, gpointer data)
{
	struct nsgtk_scaffolding *g = (struct nsgtk_scaffolding *) data;
	struct gui_window *gui = g->top_level;
	struct browser_window *bw = nsgtk_get_browser_window(gui);
	nserror err;

	if (current_menu_features.link == NULL)
		return FALSE;

	err = browser_window_create(BW_CREATE_CLONE |
				    BW_CREATE_HISTORY |
				    BW_CREATE_TAB,
				    current_menu_features.link, NULL, bw, NULL);
	if (err != NSERROR_OK) {
		nsgtk_warning(messages_get_errorcode(err), 0);
	}

	return TRUE;
}


/**
 * Handler for bookmarking a link. attached to the popup menu.
 */
static gboolean
nsgtk_on_link_bookmark_activate_menu(GtkMenuItem *widget, gpointer data)
{
	if (current_menu_features.link == NULL)
		return FALSE;

	hotlist_add_url(current_menu_features.link);

	return TRUE;
}


/**
 * Handler for copying a link. attached to the popup menu.
 */
static gboolean
nsgtk_on_link_copy_activate_menu(GtkMenuItem *widget, gpointer data)
{
	GtkClipboard *clipboard;

	if (current_menu_features.link == NULL)
		return FALSE;

	clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	gtk_clipboard_set_text(clipboard,
			       nsurl_access(current_menu_features.link), -1);

	return TRUE;
}


static gboolean nsgtk_on_find_activate_menu(GtkMenuItem *widget, gpointer data)
{
	struct nsgtk_scaffolding *g = (struct nsgtk_scaffolding *)data;

	nsgtk_window_search_toggle(g->top_level);

	return TRUE;
}

static nserror get_bar_show(bool *menu, bool *tool)
{
	const char *cur_bar_show;

	*menu = false;
	*tool = false;

	cur_bar_show = nsoption_charp(bar_show);
	if (cur_bar_show != NULL) {
		if (strcmp(cur_bar_show, "menu/tool") == 0) {
			*menu = true;
			*tool = true;
		} else if (strcmp(cur_bar_show, "menu") == 0) {
			*menu = true;
		} else if (strcmp(cur_bar_show, "tool") == 0) {
			*tool = true;
		}
	}

	return NSERROR_OK;
}

static nserror set_bar_show(const char *bar, bool show)
{
	bool menu;
	bool tool;
	const char *new_bar_show;

	get_bar_show(&menu, &tool);

	if (strcmp(bar, "menu") == 0) {
		menu = show;
	} else if (strcmp(bar, "tool") == 0) {
		tool = show;
	}

	if ((menu == true) && (tool == true)) {
		new_bar_show = "menu/tool";
	} else if (menu == true) {
		new_bar_show = "menu";
	} else if (tool == true) {
		new_bar_show = "tool";
	} else {
		new_bar_show = "none";
	}
	nsoption_set_charp(bar_show, strdup(new_bar_show));

	return NSERROR_OK;
}

static gboolean
nsgtk_on_menubar_activate_menu(GtkMenuItem *widget, gpointer data)
{
	struct nsgtk_scaffolding *gs = (struct nsgtk_scaffolding *)data;
	GtkCheckMenuItem *bmcmi; /* burger menu check */
	GtkCheckMenuItem *mbcmi; /* menu bar check */
	GtkCheckMenuItem *tbcmi; /* popup menu check */

	bmcmi = GTK_CHECK_MENU_ITEM(gs->burger_menu->view_submenu->toolbars_submenu->menubar_menuitem);
	mbcmi = GTK_CHECK_MENU_ITEM(gs->menu_bar->view_submenu->toolbars_submenu->menubar_menuitem);
	tbcmi = GTK_CHECK_MENU_ITEM(gs->popup_menu->toolbars_submenu->menubar_menuitem);

	/* ensure menubar and burger menu checkboxes are both updated */
	if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(widget))) {
		if (gtk_check_menu_item_get_active(bmcmi) == FALSE) {
			gtk_check_menu_item_set_active(bmcmi, TRUE);
		}

		if (gtk_check_menu_item_get_active(mbcmi) == FALSE) {
			gtk_check_menu_item_set_active(mbcmi, TRUE);
		}

		if (gtk_check_menu_item_get_active(tbcmi) == FALSE) {
			gtk_check_menu_item_set_active(tbcmi, TRUE);
		}

		gtk_widget_show(GTK_WIDGET(gs->menu_bar->bar_menu));
		set_bar_show("menu", true);
	} else {
		if (gtk_check_menu_item_get_active(bmcmi) == TRUE) {
			gtk_check_menu_item_set_active(bmcmi, FALSE);
		}

		if (gtk_check_menu_item_get_active(mbcmi) == TRUE) {
			gtk_check_menu_item_set_active(mbcmi, FALSE);
		}

		if (gtk_check_menu_item_get_active(tbcmi) == TRUE) {
			gtk_check_menu_item_set_active(tbcmi, FALSE);
		}

		gtk_widget_hide(GTK_WIDGET(gs->menu_bar->bar_menu));
		set_bar_show("menu", false);
	}
	return TRUE;
}


static gboolean
nsgtk_on_toolbar_activate_menu(GtkMenuItem *widget, gpointer data)
{
	struct nsgtk_scaffolding *gs = (struct nsgtk_scaffolding *)data;
	GtkCheckMenuItem *bmcmi; /* burger menu check */
	GtkCheckMenuItem *mbcmi; /* menu bar check */
	GtkCheckMenuItem *tbcmi; /* popup menu check */

	bmcmi = GTK_CHECK_MENU_ITEM(gs->burger_menu->view_submenu->toolbars_submenu->toolbar_menuitem);
	mbcmi = GTK_CHECK_MENU_ITEM(gs->menu_bar->view_submenu->toolbars_submenu->toolbar_menuitem);
	tbcmi = GTK_CHECK_MENU_ITEM(gs->popup_menu->toolbars_submenu->toolbar_menuitem);

	/* ensure menubar and burger menu checkboxes are both updated */
	if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(widget))) {
		if (gtk_check_menu_item_get_active(bmcmi) == FALSE) {
			gtk_check_menu_item_set_active(bmcmi, TRUE);
		}

		if (gtk_check_menu_item_get_active(mbcmi) == FALSE) {
			gtk_check_menu_item_set_active(mbcmi, TRUE);
		}

		if (gtk_check_menu_item_get_active(tbcmi) == FALSE) {
			gtk_check_menu_item_set_active(tbcmi, TRUE);
		}

		nsgtk_window_toolbar_show(gs, true);
		set_bar_show("tool", true);
	} else {
		if (gtk_check_menu_item_get_active(bmcmi) == TRUE) {
			gtk_check_menu_item_set_active(bmcmi, FALSE);
		}

		if (gtk_check_menu_item_get_active(mbcmi) == TRUE) {
			gtk_check_menu_item_set_active(mbcmi, FALSE);
		}

		if (gtk_check_menu_item_get_active(tbcmi) == TRUE) {
			gtk_check_menu_item_set_active(tbcmi, FALSE);
		}

		nsgtk_window_toolbar_show(gs, false);
		set_bar_show("tool", false);
	}
	return TRUE;
}


static gboolean
nsgtk_on_nexttab_activate_menu(GtkMenuItem *widget, gpointer data)
{
	struct nsgtk_scaffolding *g = (struct nsgtk_scaffolding *)data;

	nsgtk_tab_next(g->notebook);

	return TRUE;
}


static gboolean
nsgtk_on_prevtab_activate_menu(GtkMenuItem *widget, gpointer data)
{
	struct nsgtk_scaffolding *g = (struct nsgtk_scaffolding *)data;

	nsgtk_tab_prev(g->notebook);

	return TRUE;
}


/**
 * menu signal handler for activation on close tab item
 */
static gboolean
nsgtk_on_closetab_activate_menu(GtkMenuItem *widget, gpointer data)
{
	struct nsgtk_scaffolding *g = (struct nsgtk_scaffolding *)data;

	nsgtk_tab_close_current(g->notebook);

	return TRUE;
}

/* end of menu callback handlers */

/**
 * attach gtk signal handlers for menus
 */
static void nsgtk_menu_connect_signals(struct nsgtk_scaffolding *g)
{
	int idx; /* item index */
	for (idx = BACK_BUTTON; idx < PLACEHOLDER_BUTTON; idx++) {
		if (g->menus[idx].main != NULL) {
			g_signal_connect(g->menus[idx].main,
					 "activate",
					 G_CALLBACK(g->menus[idx].mhandler),
					 g);
		}
		if (g->menus[idx].burger != NULL) {
			g_signal_connect(g->menus[idx].burger,
					 "activate",
					 G_CALLBACK(g->menus[idx].mhandler),
					 g);
		}
		if (g->menus[idx].popup != NULL) {
			g_signal_connect(g->menus[idx].popup,
					 "activate",
					G_CALLBACK(g->menus[idx].mhandler),
					 g);
		}
	}
}


/**
 * Create and connect handlers to bar menu.
 *
 * \param gs scaffolding to attach popup menu to.
 * \param group The accelerator group to use for the popup.
 * \param showmenu if the bar menu should be shown
 * \param showtool if the toolabar should be shown
 * \return menu structure on success or NULL on error.
 */
static struct nsgtk_bar_submenu *
create_scaffolding_bar_menu(struct nsgtk_scaffolding *gs,
			    GtkAccelGroup *group,
			    bool showmenu,
			    bool showtool)
{
	GtkMenuShell *menushell;
	struct nsgtk_bar_submenu *nmenu;

	menushell = GTK_MENU_SHELL(gtk_builder_get_object(gs->builder,
							  "menubar"));

	nmenu = nsgtk_menu_bar_create(menushell, group);
	if (nmenu == NULL) {
		return NULL;
	}

	/* set menu bar visibility */
	if (showmenu) {
		gtk_widget_show(GTK_WIDGET(nmenu->bar_menu));
	} else {
		gtk_widget_hide(GTK_WIDGET(nmenu->bar_menu));
	}

	/* set checks correct way on toolbar submenu */
	gtk_check_menu_item_set_active(nmenu->view_submenu->toolbars_submenu->menubar_menuitem, showmenu);
	gtk_check_menu_item_set_active(nmenu->view_submenu->toolbars_submenu->toolbar_menuitem, showtool);

	/* bar menu signal handlers for edit controls */
	g_signal_connect(nmenu->edit_submenu->edit,
			 "show",
			 G_CALLBACK(nsgtk_window_edit_menu_shown),
			 gs);

	g_signal_connect(nmenu->edit_submenu->edit,
			 "hide",
			 G_CALLBACK(nsgtk_window_edit_menu_hidden),
			 gs);

	/*
	 * attach signal handlers for menubar and toolbar visibility toggling
	 */
	g_signal_connect(nmenu->view_submenu->toolbars_submenu->menubar_menuitem,
			 "toggled",
			 G_CALLBACK(nsgtk_on_menubar_activate_menu),
			 gs);

	g_signal_connect(nmenu->view_submenu->toolbars_submenu->toolbar_menuitem,
			 "toggled",
			 G_CALLBACK(nsgtk_on_toolbar_activate_menu),
			 gs);


	return nmenu;
}


/**
 * Create and connect handlers to burger menu.
 *
 * \param g scaffolding to attach popup menu to.
 * \param group The accelerator group to use for the popup.
 * \param showbar if the bar menu should be shown
 * \param showtool if the toolabar should be shown
 * \return menu structure on success or NULL on error.
 */
static struct nsgtk_burger_menu *
create_scaffolding_burger_menu(struct nsgtk_scaffolding *gs,
			       GtkAccelGroup *group,
			       bool showbar,
			       bool showtool)
{
	struct nsgtk_burger_menu *nmenu;

	nmenu = nsgtk_burger_menu_create(group);

	if (nmenu == NULL) {
		return NULL;
	}

	/* set checks correct way on toolbar submenu */
	gtk_check_menu_item_set_active(nmenu->view_submenu->toolbars_submenu->menubar_menuitem, showbar);
	gtk_check_menu_item_set_active(nmenu->view_submenu->toolbars_submenu->toolbar_menuitem, showtool);

	g_signal_connect(nmenu->view_submenu->toolbars_submenu->menubar_menuitem,
			 "toggled",
			 G_CALLBACK(nsgtk_on_menubar_activate_menu),
			 gs);
	g_signal_connect(nmenu->view_submenu->toolbars_submenu->toolbar_menuitem,
			 "toggled",
			 G_CALLBACK(nsgtk_on_toolbar_activate_menu),
			 gs);
	return nmenu;
}


/**
 * Create and connect handlers to popup menu.
 *
 * \param gs scaffolding to attach popup menu to.
 * \param group The accelerator group to use for the popup.
 * \param showbar if the bar menu should be shown
 * \param showtool if the toolabar should be shown
 * \return menu structure on success or NULL on error.
 */
static struct nsgtk_popup_menu *
create_scaffolding_popup_menu(struct nsgtk_scaffolding *gs,
			      GtkAccelGroup *group,
			      bool showbar,
			      bool showtool)
{
	struct nsgtk_popup_menu *nmenu;

	nmenu = nsgtk_popup_menu_create(group);

	if (nmenu == NULL) {
		return NULL;
	}
	/* set checks correct way on toolbar submenu */
	gtk_check_menu_item_set_active(nmenu->toolbars_submenu->menubar_menuitem, showbar);
	gtk_check_menu_item_set_active(nmenu->toolbars_submenu->toolbar_menuitem, showtool);

	g_signal_connect(nmenu->popup_menu,
			 "hide",
			 G_CALLBACK(nsgtk_window_popup_menu_hidden),
			 gs);

	g_signal_connect(nmenu->toolbars_submenu->menubar_menuitem,
			 "toggled",
			 G_CALLBACK(nsgtk_on_menubar_activate_menu),
			 gs);
	g_signal_connect(nmenu->toolbars_submenu->toolbar_menuitem,
			 "toggled",
			 G_CALLBACK(nsgtk_on_toolbar_activate_menu),
			 gs);

	/* set initial popup menu visibility */
	popup_menu_hide(nmenu, false, false);

	return nmenu;
}


/**
 * Create and connect handlers to link popup menu.
 *
 * \param g scaffolding to attach popup menu to.
 * \param group The accelerator group to use for the popup.
 * \return true on success or false on error.
 */
static struct nsgtk_link_menu *
create_scaffolding_link_menu(struct nsgtk_scaffolding *g, GtkAccelGroup *group)
{
	struct nsgtk_link_menu *nmenu;

	nmenu = nsgtk_link_menu_create(group);

	if (nmenu == NULL) {
		return NULL;
	}

	g_signal_connect(nmenu->save_menuitem,
			 "activate",
			 G_CALLBACK(nsgtk_on_savelink_activate_menu),
			 g);

	g_signal_connect(nmenu->opentab_menuitem,
			 "activate",
			 G_CALLBACK(nsgtk_on_link_opentab_activate_menu),
			 g);

	g_signal_connect(nmenu->openwin_menuitem,
			 "activate",
			 G_CALLBACK(nsgtk_on_link_openwin_activate_menu),
			 g);

	g_signal_connect(nmenu->bookmark_menuitem,
			 "activate",
			 G_CALLBACK(nsgtk_on_link_bookmark_activate_menu),
			 g);

	g_signal_connect(nmenu->copy_menuitem,
			 "activate",
			 G_CALLBACK(nsgtk_on_link_copy_activate_menu),
			 g);

	return nmenu;
}


/**
 * initialiase the menu signal handlers ready for connection
 */
static nserror nsgtk_menu_initialise(struct nsgtk_scaffolding *g)
{
#define TOOLBAR_ITEM_p(identifier, name, iconame)			\
	g->menus[identifier].mhandler = nsgtk_on_##name##_activate_menu; \
	g->menus[identifier].iconname = iconame;
#define TOOLBAR_ITEM_y(identifier, name, iconame)			\
	g->menus[identifier].mhandler = nsgtk_on_##name##_activate_menu; \
	g->menus[identifier].iconname = iconame;
#define TOOLBAR_ITEM_n(identifier, name, iconame)			\
	g->menus[identifier].mhandler = NULL;				\
	g->menus[identifier].iconname = iconame;
#define TOOLBAR_ITEM(identifier, name, snstvty, clicked, activate, label, iconame) \
	g->menus[identifier].sensitivity = snstvty;			\
	TOOLBAR_ITEM_ ## activate(identifier, name, iconame)
#include "gtk/toolbar_items.h"
#undef TOOLBAR_ITEM_y
#undef TOOLBAR_ITEM_n
#undef TOOLBAR_ITEM

	/* items on menubar, burger */
#define ITEM_MB(p, q, r)						\
	g->menus[p##_BUTTON].main = g->menu_bar->r##_submenu->q##_menuitem; \
	g->menus[p##_BUTTON].burger = g->burger_menu->r##_submenu->q##_menuitem

	/* items on menubar, burger and context popup submenu */
#define ITEM_MBP(p, q, r)						\
	g->menus[p##_BUTTON].main = g->menu_bar->r##_submenu->q##_menuitem; \
	g->menus[p##_BUTTON].burger = g->burger_menu->r##_submenu->q##_menuitem; \
	g->menus[p##_BUTTON].popup = g->popup_menu->r##_submenu->q##_menuitem

	/* items on menubar, burger and context popup */
#define ITEM_MBp(p, q, r)						\
	g->menus[p##_BUTTON].main = g->menu_bar->r##_submenu->q##_menuitem; \
	g->menus[p##_BUTTON].burger = g->burger_menu->r##_submenu->q##_menuitem; \
	g->menus[p##_BUTTON].popup = g->popup_menu->q##_menuitem


	/* file menu */
	ITEM_MB(NEWWINDOW, newwindow, file);
	ITEM_MB(NEWTAB, newtab, file);
	ITEM_MB(OPENFILE, openfile, file);
	ITEM_MB(CLOSEWINDOW, closewindow, file);
	ITEM_MB(PRINTPREVIEW, printpreview, file);
	ITEM_MB(PRINT, print, file);
	ITEM_MB(QUIT, quit, file);
	/* file - export submenu */
	ITEM_MB(SAVEPAGE, savepage, file_submenu->export);
	ITEM_MB(PLAINTEXT, plaintext, file_submenu->export);
	ITEM_MB(PDF, pdf, file_submenu->export);

	/* edit menu */
	ITEM_MBp(CUT, cut, edit);
	ITEM_MBp(COPY, copy, edit);
	ITEM_MBp(PASTE, paste, edit);
	ITEM_MB(DELETE, delete, edit);
	ITEM_MB(SELECTALL, selectall, edit);
	ITEM_MB(FIND, find, edit);
	ITEM_MB(PREFERENCES, preferences, edit);

	/* view menu */
	ITEM_MB(FULLSCREEN, fullscreen, view);
	ITEM_MB(SAVEWINDOWSIZE, savewindowsize, view);
	/* view - scale submenu */
	ITEM_MB(ZOOMPLUS, zoomplus, view_submenu->scaleview);
	ITEM_MB(ZOOMMINUS, zoomminus, view_submenu->scaleview);
	ITEM_MB(ZOOMNORMAL, zoomnormal, view_submenu->scaleview);
	/* view - tabs submenu */
	ITEM_MB(NEXTTAB, nexttab, view_submenu->tabs);
	ITEM_MB(PREVTAB, prevtab, view_submenu->tabs);
	ITEM_MB(CLOSETAB, closetab, view_submenu->tabs);
	/* view - toolbars submenu */
	ITEM_MB(CUSTOMIZE, customize, view_submenu->toolbars);
	g->menus[CUSTOMIZE_BUTTON].popup = g->popup_menu->toolbars_submenu->customize_menuitem;

	/* navigation menu */
	ITEM_MBp(BACK, back, nav);
	ITEM_MBp(FORWARD, forward, nav);
	ITEM_MBp(STOP, stop, nav);
	ITEM_MBp(RELOAD, reload, nav);
	ITEM_MB(HOME, home, nav);
	ITEM_MB(LOCALHISTORY, localhistory, nav);
	ITEM_MB(GLOBALHISTORY, globalhistory, nav);
	ITEM_MB(ADDBOOKMARKS, addbookmarks, nav);
	ITEM_MB(SHOWBOOKMARKS, showbookmarks, nav);
	ITEM_MB(OPENLOCATION, openlocation, nav);

	/* tools menu */
	ITEM_MBP(DOWNLOADS, downloads, tools);
	ITEM_MBP(SHOWCOOKIES, showcookies, tools);
	/* tools > developer submenu */
	ITEM_MBP(VIEWSOURCE, viewsource, tools_submenu->developer);
	ITEM_MBP(TOGGLEDEBUGGING, toggledebugging, tools_submenu->developer);
	ITEM_MBP(SAVEBOXTREE, debugboxtree, tools_submenu->developer);
	ITEM_MBP(SAVEDOMTREE, debugdomtree, tools_submenu->developer);

	/* help menu */
	ITEM_MB(CONTENTS, contents, help);
	ITEM_MB(GUIDE, guide, help);
	ITEM_MB(INFO, info, help);
	ITEM_MB(ABOUT, about, help);


#undef ITEM_MB
#undef ITEM_MBp
#undef ITEM_MBP

	return NSERROR_OK;
}


static void nsgtk_menu_set_sensitivity(struct nsgtk_scaffolding *g)
{

	for (int i = BACK_BUTTON; i < PLACEHOLDER_BUTTON; i++) {
		if (g->menus[i].main != NULL) {
			gtk_widget_set_sensitive(GTK_WIDGET(
					g->menus[i].main),
					g->menus[i].sensitivity);
		}
		if (g->menus[i].burger != NULL) {
			gtk_widget_set_sensitive(GTK_WIDGET(
					g->menus[i].burger),
					g->menus[i].sensitivity);
		}
		if (g->menus[i].popup != NULL) {
			gtk_widget_set_sensitive(GTK_WIDGET(
					g->menus[i].popup),
					g->menus[i].sensitivity);
		}
	}
}


/* set menu items to have icons */
static void nsgtk_menu_set_icons(struct nsgtk_scaffolding *g)
{
	GtkWidget *img;
	for (int i = BACK_BUTTON; i < PLACEHOLDER_BUTTON; i++) {
		/* ensure there is an icon name */
		if (g->menus[i].iconname == NULL) {
			continue;
		}

		if (g->menus[i].main != NULL) {
		img = gtk_image_new_from_icon_name(g->menus[i].iconname,
						   GTK_ICON_SIZE_MENU);
			nsgtk_image_menu_item_set_image(GTK_WIDGET(g->menus[i].main), img);
		}
		if (g->menus[i].burger != NULL) {
		img = gtk_image_new_from_icon_name(g->menus[i].iconname,
						   GTK_ICON_SIZE_MENU);
			nsgtk_image_menu_item_set_image(GTK_WIDGET(g->menus[i].burger), img);
		}
		if (g->menus[i].popup != NULL) {
		img = gtk_image_new_from_icon_name(g->menus[i].iconname,
						   GTK_ICON_SIZE_MENU);
			nsgtk_image_menu_item_set_image(GTK_WIDGET(g->menus[i].popup), img);
		}
	}
}


/**
 * create and initialise menus
 *
 * There are four menus held by the scaffolding:
 *
 *  1. Main menubar menu.
 *     This can be hidden which causes the right click popup context menu
 *       to use the burger menu.
 *  2. Burger menu.
 *     This can be opened from a burger icon on the toolbar.
 *  3. popup context menu.
 *     This is opened by right mouse clicking on the toolbar or browser area
 *  4. link context menu
 *     Opened like the other popup menu when the mouse is over a link in the
 *        browser area
 *
 * The cut, copy, paste, delete and back, forwards, stop, reload groups of
 *   menu entries are context sensitive and must be updated as appropriate
 *   when a menu is opened which contains those groups.
 */
static nserror nsgtk_menus_create(struct nsgtk_scaffolding *gs)
{
	GtkAccelGroup *group;
	bool showmenu; /* show menubar */
	bool showtool; /* show toolbar */

	get_bar_show(&showmenu, &showtool);

	group = gtk_accel_group_new();

	gtk_window_add_accel_group(gs->window, group);

	gs->menu_bar = create_scaffolding_bar_menu(gs, group, showmenu, showtool);
	gs->burger_menu = create_scaffolding_burger_menu(gs, group, showmenu, showtool);
	gs->popup_menu = create_scaffolding_popup_menu(gs, group, showmenu, showtool);
	gs->link_menu = create_scaffolding_link_menu(gs, group);

	/* set up the menu signal handlers */
	nsgtk_menu_initialise(gs);
	nsgtk_menu_set_icons(gs);
	nsgtk_menu_connect_signals(gs);
	nsgtk_menu_set_sensitivity(gs);

	return NSERROR_OK;
}


/* exported function documented in gtk/scaffolding.h */
void nsgtk_scaffolding_set_title(struct gui_window *gw, const char *title)
{
	struct nsgtk_scaffolding *gs = nsgtk_get_scaffold(gw);
	int title_len;
	char *newtitle;

	/* only set window title if top level window */
	if (gs->top_level != gw) {
		return;
	}

	if (title == NULL || title[0] == '\0') {
		gtk_window_set_title(gs->window, "NetSurf");
		return;
	}

	title_len = strlen(title) + SLEN(" - NetSurf") + 1;
	newtitle = malloc(title_len);
	if (newtitle == NULL) {
		return;
	}

	snprintf(newtitle, title_len, "%s - NetSurf", title);

	gtk_window_set_title(gs->window, newtitle);

	free(newtitle);

}


/* exported interface documented in scaffolding.h */
nserror nsgtk_scaffolding_throbber(struct gui_window* gw, bool active)
{
	struct nsgtk_scaffolding *gs = nsgtk_get_scaffold(gw);
	if (active) {
		gs->menus[STOP_BUTTON].sensitivity = true;
		gs->menus[RELOAD_BUTTON].sensitivity = false;
	} else {
		gs->menus[STOP_BUTTON].sensitivity = false;
		gs->menus[RELOAD_BUTTON].sensitivity = true;
	}
	scaffolding_update_context(gs);

	return NSERROR_OK;
}


/* exported interface documented in gtk/scaffolding.h */
nserror nsgtk_scaffolding_destroy_all(void)
{
	struct nsgtk_scaffolding *gs;

	gs = scaf_list;
	assert(gs != NULL);

	if (nsgtk_check_for_downloads(gs->window) == true) {
		return NSERROR_INVALID;
	}

	/* iterate all scaffolding windows and destroy them */
	while (gs != NULL) {
		struct nsgtk_scaffolding *next = gs->next;
		gtk_widget_destroy(GTK_WIDGET(gs->window));
		gs = next;
	}
	return NSERROR_OK;
}


/* exported interface documented in gtk/scaffolding.h */
GtkWindow* nsgtk_scaffolding_window(struct nsgtk_scaffolding *g)
{
	return g->window;
}

/* exported interface documented in gtk/scaffolding.h */
GtkNotebook* nsgtk_scaffolding_notebook(struct nsgtk_scaffolding *g)
{
	return g->notebook;
}


/* exported interface documented in gtk/scaffolding.h */
GtkMenuBar *nsgtk_scaffolding_menu_bar(struct nsgtk_scaffolding *gs)
{
	if (gs == NULL) {
		return NULL;
	}
	return gs->menu_bar->bar_menu;
}

/* exported interface documented in gtk/scaffolding.h */
struct nsgtk_scaffolding *nsgtk_scaffolding_iterate(struct nsgtk_scaffolding *g)
{
	if (g == NULL) {
		return scaf_list;
	}
	return g->next;
}


/* exported interface documented in gtk/scaffolding.h */
struct gui_window *nsgtk_scaffolding_top_level(struct nsgtk_scaffolding *g)
{
	return g->top_level;
}


/* exported interface documented in gtk/scaffolding.h */
void nsgtk_scaffolding_set_top_level(struct gui_window *gw)
{
	struct browser_window *bw;
	struct nsgtk_scaffolding *sc;

	assert(gw != NULL);

	bw = nsgtk_get_browser_window(gw);

	assert(bw != NULL);

	sc = nsgtk_get_scaffold(gw);
	assert(sc != NULL);

	scaf_current = sc;

	sc->top_level = gw;

	/* Synchronise the history */
	scaffolding_update_context(sc);

	/* Ensure the window's title bar is updated */
	nsgtk_scaffolding_set_title(gw, browser_window_get_title(bw));
}


/* exported interface documented in scaffolding.h */
void nsgtk_scaffolding_set_sensitivity(struct nsgtk_scaffolding *g)
{
	int i;
#define SENSITIVITY(q)							\
	i = q##_BUTTON;							\
	if (g->menus[i].main != NULL)					\
		gtk_widget_set_sensitive(GTK_WIDGET(g->menus[i].main),	\
					 g->menus[i].sensitivity);	\
	if (g->menus[i].burger != NULL)					\
		gtk_widget_set_sensitive(GTK_WIDGET(g->menus[i].burger), \
					 g->menus[i].sensitivity);	\
	if (g->menus[i].popup != NULL)					\
		gtk_widget_set_sensitive(GTK_WIDGET(g->menus[i].popup), \
					 g->menus[i].sensitivity);

	SENSITIVITY(STOP)
	SENSITIVITY(RELOAD)
	SENSITIVITY(CUT)
	SENSITIVITY(COPY)
	SENSITIVITY(PASTE)
	SENSITIVITY(BACK)
	SENSITIVITY(FORWARD)
	SENSITIVITY(NEXTTAB)
	SENSITIVITY(PREVTAB)
	SENSITIVITY(CLOSETAB)
#undef SENSITIVITY

}


/* exported interface documented in gtk/scaffolding.h */
nserror nsgtk_scaffolding_toolbar_context_menu(struct nsgtk_scaffolding *gs)
{
	/* set visibility for right-click popup menu */
	popup_menu_hide(gs->popup_menu, false, true);

	nsgtk_menu_popup_at_pointer(gs->popup_menu->popup_menu, NULL);

	return NSERROR_OK;
}


/* exported interface documented in gtk/scaffolding.h */
nserror nsgtk_scaffolding_burger_menu(struct nsgtk_scaffolding *gs)
{
	nsgtk_menu_popup_at_pointer(gs->burger_menu->burger_menu, NULL);

	return NSERROR_OK;
}


/* exported interface documented in gtk/scaffolding.h */
void
nsgtk_scaffolding_context_menu(struct nsgtk_scaffolding *g,
			       gdouble x,
			       gdouble y)
{
	GtkMenu	*gtkmenu;
	struct browser_window *bw;

	bw = nsgtk_get_browser_window(g->top_level);

	/* update the global context menu features */
	browser_window_get_features(bw,	x, y, &current_menu_features);

	if (current_menu_features.link != NULL) {
		/* menu is opening over a link */
		gtkmenu = g->link_menu->link_menu;
	} else {
		gtkmenu = g->popup_menu->popup_menu;

		nsgtk_scaffolding_update_edit_actions_sensitivity(g);

		if (!(g->menus[COPY_BUTTON].sensitivity)) {
			gtk_widget_hide(GTK_WIDGET(g->popup_menu->copy_menuitem));
		} else {
			gtk_widget_show(GTK_WIDGET(g->popup_menu->copy_menuitem));
		}

		if (!(g->menus[CUT_BUTTON].sensitivity)) {
			gtk_widget_hide(GTK_WIDGET(g->popup_menu->cut_menuitem));
		} else {
			gtk_widget_show(GTK_WIDGET(g->popup_menu->cut_menuitem));
		}

		if (!(g->menus[PASTE_BUTTON].sensitivity)) {
			gtk_widget_hide(GTK_WIDGET(g->popup_menu->paste_menuitem));
		} else {
			gtk_widget_show(GTK_WIDGET(g->popup_menu->paste_menuitem));
		}

	}

	nsgtk_menu_popup_at_pointer(gtkmenu, NULL);
}

/* exported interface documented in gtk/scaffolding.h */
struct nsgtk_scaffolding *nsgtk_current_scaffolding(void)
{
	if (scaf_current == NULL) {
		scaf_current = scaf_list;
	}
	return scaf_current;
}

/* exported interface documented in gtk/scaffolding.h */
struct nsgtk_scaffolding *nsgtk_scaffolding_from_notebook(GtkNotebook *notebook)
{
	struct nsgtk_scaffolding *gs;
	gs = scaf_list;
	while (gs != NULL) {
		if (gs->notebook == notebook) {
			break;
		}
		gs = gs->next;
	}
	return gs;
}

/* exported interface documented in gtk/scaffolding.h */
struct nsgtk_scaffolding *nsgtk_new_scaffolding(struct gui_window *toplevel)
{
	nserror res;
	struct nsgtk_scaffolding *gs;

	gs = calloc(1, sizeof(*gs));
	if (gs == NULL) {
		return NULL;
	}

	NSLOG(netsurf, INFO,
	      "Constructing a scaffold of %p for gui_window %p", gs, toplevel);

	gs->top_level = toplevel;

	/* Construct UI widgets */
	if (nsgtk_builder_new_from_resname("netsurf", &gs->builder) != NSERROR_OK) {
		free(gs);
		return NULL;
	}

	gtk_builder_connect_signals(gs->builder, NULL);

	/* containing window setup */
	gs->window = GTK_WINDOW(gtk_builder_get_object(gs->builder,
						       "wndBrowser"));

	/**
	 * set this window's size and position to what's in the options, or
	 *   some sensible default if they are not set yet.
	 */
	if (nsoption_int(window_width) > 0) {
		gtk_window_move(gs->window,
				nsoption_int(window_x),
				nsoption_int(window_y));
		gtk_window_resize(gs->window,
				  nsoption_int(window_width),
				  nsoption_int(window_height));
	} else {
		/* Set to 1000x700, so we're very likely to fit even on
		 * 1024x768 displays, not being able to take into account
		 * window furniture or panels.
		 */
		gtk_window_set_default_size(gs->window, 1000, 700);
	}

	g_signal_connect(gs->window,
			 "delete-event",
			 G_CALLBACK(scaffolding_window_delete_event),
			 gs);

	g_signal_connect(gs->window,
			 "destroy",
			 G_CALLBACK(scaffolding_window_destroy),
			 gs);


	/* notebook */
	res = nsgtk_notebook_create(gs->builder, &gs->notebook);
	if (res != NSERROR_OK) {
		free(gs);
		return NULL;
	}

	g_signal_connect_after(gs->notebook,
			       "page-added",
			       G_CALLBACK(nsgtk_window_tabs_add),
			       gs);
	gs->tabs_remove_handler_id = g_signal_connect_after(gs->notebook,
			       "page-removed",
			       G_CALLBACK(nsgtk_window_tabs_remove),
			       gs);


	res = nsgtk_menus_create(gs);
	if (res != NSERROR_OK) {
		free(gs);
		return NULL;
	}

	/* attach to the list */
	if (scaf_list) {
		scaf_list->prev = gs;
	}
	gs->next = scaf_list;
	gs->prev = NULL;
	scaf_list = gs;

	/* finally, show the window. */
	gtk_widget_show(GTK_WIDGET(gs->window));

	NSLOG(netsurf, INFO, "creation complete");

	return gs;
}

/* exported interface documented in gtk/scaffolding.h */
nserror nsgtk_scaffolding_position_page_info(struct nsgtk_scaffolding *gs,
					     struct nsgtk_pi_window *win)
{
	return nsgtk_window_position_page_info(gs->top_level, win);
}

/* exported interface documented in gtk/scaffolding.h */
nserror nsgtk_scaffolding_position_local_history(struct nsgtk_scaffolding *gs)
{
	return nsgtk_window_position_local_history(gs->top_level);
}
