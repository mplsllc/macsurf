/*
 * Copyright 2010 John Mark Bell <jmb@netsurf-browser.org>
 * Copyright 2016 Vincent Sanders <vince@netsurf-browser.org>
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
 * Implementation of GTK bookmark (hotlist) manager.
 */

#include <stdint.h>
#include <stdlib.h>
#include <gtk/gtk.h>

#include "utils/log.h"
#include "utils/nsoption.h"
#include "netsurf/keypress.h"
#include "netsurf/plotters.h"
#include "desktop/hotlist.h"

#include "gtk/compat.h"
#include "gtk/plotters.h"
#include "gtk/resources.h"
#include "gtk/corewindow.h"
#include "gtk/hotlist.h"

/**
 * hotlist window container for gtk.
 */
struct nsgtk_hotlist_window {
	struct nsgtk_corewindow core;
	GtkBuilder *builder;
	GtkWindow *wnd;
};

static struct nsgtk_hotlist_window *hotlist_window = NULL;

#define MENUPROTO(x) static gboolean nsgtk_on_##x##_activate( \
		GtkMenuItem *widget, gpointer g)
#define MENUEVENT(x) { #x, G_CALLBACK(nsgtk_on_##x##_activate) }
#define MENUHANDLER(x) gboolean nsgtk_on_##x##_activate(GtkMenuItem *widget, \
		gpointer g)

struct menu_events {
	const char *widget;
	GCallback handler;
};


/* file menu*/
MENUPROTO(export);
MENUPROTO(new_folder);
MENUPROTO(new_entry);

/* edit menu */
MENUPROTO(edit_selected);
MENUPROTO(delete_selected);
MENUPROTO(select_all);
MENUPROTO(clear_selection);

/* view menu*/
MENUPROTO(expand_all);
MENUPROTO(expand_directories);
MENUPROTO(expand_addresses);
MENUPROTO(collapse_all);
MENUPROTO(collapse_directories);
MENUPROTO(collapse_addresses);

MENUPROTO(launch);

static struct menu_events menu_events[] = {

	/* file menu*/
	MENUEVENT(export),
	MENUEVENT(new_folder),
	MENUEVENT(new_entry),

	/* edit menu */
	MENUEVENT(edit_selected),
	MENUEVENT(delete_selected),
	MENUEVENT(select_all),
	MENUEVENT(clear_selection),

	/* view menu*/
	MENUEVENT(expand_all),
	MENUEVENT(expand_directories),
	MENUEVENT(expand_addresses),
	MENUEVENT(collapse_all),
	MENUEVENT(collapse_directories),
	MENUEVENT(collapse_addresses),

	MENUEVENT(launch),

	{NULL, NULL}
};


/* file menu*/
MENUHANDLER(export)
{
	struct nsgtk_hotlist_window *hlwin;
	GtkWidget *save_dialog;

	hlwin = (struct nsgtk_hotlist_window *)g;

	save_dialog = gtk_file_chooser_dialog_new("Save File",
			hlwin->wnd,
			GTK_FILE_CHOOSER_ACTION_SAVE,
			NSGTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
			NSGTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
			NULL);

	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(save_dialog),
			getenv("HOME") ? getenv("HOME") : "/");

	gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(save_dialog),
			"hotlist.html");

	if (gtk_dialog_run(GTK_DIALOG(save_dialog)) == GTK_RESPONSE_ACCEPT) {
		gchar *filename = gtk_file_chooser_get_filename(
				GTK_FILE_CHOOSER(save_dialog));

		hotlist_export(filename, NULL);
		g_free(filename);
	}

	gtk_widget_destroy(save_dialog);

	return TRUE;
}

MENUHANDLER(new_folder)
{
	hotlist_add_folder(NULL, false, 0);
	return TRUE;
}

MENUHANDLER(new_entry)
{
	hotlist_add_entry(NULL, NULL, false, 0);
	return TRUE;
}

/* edit menu */
MENUHANDLER(edit_selected)
{
	hotlist_edit_selection();
	return TRUE;
}

MENUHANDLER(delete_selected)
{
	hotlist_keypress(NS_KEY_DELETE_LEFT);
	return TRUE;
}

MENUHANDLER(select_all)
{
	hotlist_keypress(NS_KEY_ESCAPE);
	hotlist_keypress(NS_KEY_ESCAPE);
	hotlist_keypress(NS_KEY_SELECT_ALL);
	return TRUE;
}

MENUHANDLER(clear_selection)
{
	hotlist_keypress(NS_KEY_CLEAR_SELECTION);
	return TRUE;
}

/* view menu*/
MENUHANDLER(expand_all)
{
	hotlist_expand(false);
	return TRUE;
}

MENUHANDLER(expand_directories)
{
	hotlist_expand(true);
	return TRUE;
}

MENUHANDLER(expand_addresses)
{
	hotlist_expand(false);
	return TRUE;
}

MENUHANDLER(collapse_all)
{
	hotlist_contract(true);
	return TRUE;
}

MENUHANDLER(collapse_directories)
{
	hotlist_contract(true);
	return TRUE;
}

MENUHANDLER(collapse_addresses)
{
	hotlist_contract(false);
	return TRUE;
}

MENUHANDLER(launch)
{
	hotlist_keypress(NS_KEY_CR);
	return TRUE;
}


/**
 * Connects menu events in the hotlist window.
 */
static void nsgtk_hotlist_init_menu(struct nsgtk_hotlist_window *hlwin)
{
	struct menu_events *event = menu_events;
	GtkWidget *w;

	while (event->widget != NULL) {
		w = GTK_WIDGET(gtk_builder_get_object(hlwin->builder,
						      event->widget));
		if (w == NULL) {
			NSLOG(netsurf, INFO,
			      "Unable to connect menu widget ""%s""",
			      event->widget);
		} else {
			g_signal_connect(G_OBJECT(w),
					 "activate",
					 event->handler,
					 hlwin);
		}
		event++;
	}
}


/**
 * callback for mouse action on hotlist window
 *
 * \param nsgtk_cw The nsgtk core window structure.
 * \param mouse_state netsurf mouse state on event
 * \param x location of event
 * \param y location of event
 * \return NSERROR_OK on success otherwise apropriate error code
 */
static nserror
nsgtk_hotlist_mouse(struct nsgtk_corewindow *nsgtk_cw,
		    browser_mouse_state mouse_state,
		    int x, int y)
{
	hotlist_mouse_action(mouse_state, x, y);

	return NSERROR_OK;
}

/**
 * callback for keypress on hotlist window
 *
 * \param nsgtk_cw The nsgtk core window structure.
 * \param nskey The netsurf key code
 * \return NSERROR_OK on success otherwise apropriate error code
 */
static nserror
nsgtk_hotlist_key(struct nsgtk_corewindow *nsgtk_cw, uint32_t nskey)
{
	if (hotlist_keypress(nskey)) {
		return NSERROR_OK;
	}
	return NSERROR_NOT_IMPLEMENTED;
}

/**
 * callback on draw event for hotlist window
 *
 * \param nsgtk_cw The nsgtk core window structure.
 * \param r The rectangle of the window that needs updating.
 * \return NSERROR_OK on success otherwise apropriate error code
 */
static nserror
nsgtk_hotlist_draw(struct nsgtk_corewindow *nsgtk_cw, struct rect *r)
{
	struct redraw_context ctx = {
		.interactive = true,
		.background_images = true,
		.plot = &nsgtk_plotters
	};

	hotlist_redraw(0, 0, r, &ctx);

	return NSERROR_OK;
}

/**
 * Creates the window for the hotlist tree.
 *
 * \return NSERROR_OK on success else appropriate error code on faliure.
 */
static nserror nsgtk_hotlist_init(void)
{
	struct nsgtk_hotlist_window *ncwin;
	nserror res;

	if (hotlist_window != NULL) {
		return NSERROR_OK;
	}

	ncwin = calloc(1, sizeof(*ncwin));
	if (ncwin == NULL) {
		return NSERROR_NOMEM;
	}

	res = nsgtk_builder_new_from_resname("hotlist", &ncwin->builder);
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Hotlist UI builder init failed");
		free(ncwin);
		return res;
	}

	gtk_builder_connect_signals(ncwin->builder, NULL);

	ncwin->wnd = GTK_WINDOW(gtk_builder_get_object(ncwin->builder,
						       "wndHotlist"));

	ncwin->core.scrolled = GTK_SCROLLED_WINDOW(
		gtk_builder_get_object(ncwin->builder, "hotlistScrolled"));

	ncwin->core.drawing_area = GTK_DRAWING_AREA(
		gtk_builder_get_object(ncwin->builder, "hotlistDrawingArea"));

	/* make the delete event hide the window */
	g_signal_connect(G_OBJECT(ncwin->wnd),
			 "delete_event",
			 G_CALLBACK(gtk_widget_hide_on_delete),
			 NULL);

	nsgtk_hotlist_init_menu(ncwin);

	ncwin->core.draw = nsgtk_hotlist_draw;
	ncwin->core.key = nsgtk_hotlist_key;
	ncwin->core.mouse = nsgtk_hotlist_mouse;

	res = nsgtk_corewindow_init(&ncwin->core);
	if (res != NSERROR_OK) {
		free(ncwin);
		return res;
	}

	res = hotlist_manager_init((struct core_window *)ncwin);
	if (res != NSERROR_OK) {
		free(ncwin);
		return res;
	}

	/* memoise window so it can be represented when necessary
	 * instead of recreating every time.
	 */
	hotlist_window = ncwin;

	return NSERROR_OK;
}


/* exported function documented gtk/hotlist.h */
nserror nsgtk_hotlist_present(void)
{
	nserror res;

	res = nsgtk_hotlist_init();
	if (res == NSERROR_OK) {
		gtk_window_present(hotlist_window->wnd);
	}
	return res;
}


/* exported function documented gtk/hotlist.h */
nserror nsgtk_hotlist_destroy(void)
{
	nserror res;

	if (hotlist_window == NULL) {
		return NSERROR_OK;
	}

	res = hotlist_manager_fini();
	if (res == NSERROR_OK) {
		res = nsgtk_corewindow_fini(&hotlist_window->core);
		gtk_widget_destroy(GTK_WIDGET(hotlist_window->wnd));
		g_object_unref(G_OBJECT(hotlist_window->builder));
		free(hotlist_window);
		hotlist_window = NULL;
	}

	return res;
}
