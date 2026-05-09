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
 * Implementation of GTK global history manager.
 */

#include <stdint.h>
#include <stdlib.h>
#include <gtk/gtk.h>

#include "utils/log.h"
#include "netsurf/keypress.h"
#include "netsurf/plotters.h"
#include "desktop/global_history.h"

#include "gtk/compat.h"
#include "gtk/plotters.h"
#include "gtk/resources.h"
#include "gtk/corewindow.h"
#include "gtk/global_history.h"

struct nsgtk_global_history_window {
	struct nsgtk_corewindow core;
	GtkBuilder *builder;
	GtkWindow *wnd;
};

static struct nsgtk_global_history_window *global_history_window = NULL;

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

/* edit menu */
MENUPROTO(delete_selected);
MENUPROTO(delete_all);
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

	/* edit menu */
	MENUEVENT(delete_selected),
	MENUEVENT(delete_all),
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

/* edit menu */
MENUHANDLER(delete_selected)
{
	global_history_keypress(NS_KEY_DELETE_LEFT);
	return TRUE;
}

MENUHANDLER(delete_all)
{
	global_history_keypress(NS_KEY_ESCAPE);
	global_history_keypress(NS_KEY_ESCAPE);
	global_history_keypress(NS_KEY_SELECT_ALL);
	global_history_keypress(NS_KEY_DELETE_LEFT);
	return TRUE;
}

MENUHANDLER(select_all)
{
	global_history_keypress(NS_KEY_ESCAPE);
	global_history_keypress(NS_KEY_ESCAPE);
	global_history_keypress(NS_KEY_SELECT_ALL);
	return TRUE;
}

MENUHANDLER(clear_selection)
{
	global_history_keypress(NS_KEY_ESCAPE);
	global_history_keypress(NS_KEY_ESCAPE);
	global_history_keypress(NS_KEY_CLEAR_SELECTION);
	return TRUE;
}

/* view menu*/
MENUHANDLER(expand_all)
{
	global_history_expand(false);
	return TRUE;
}

MENUHANDLER(expand_directories)
{
	global_history_expand(true);
	return TRUE;
}

MENUHANDLER(expand_addresses)
{
	global_history_expand(false);
	return TRUE;
}

MENUHANDLER(collapse_all)
{
	global_history_contract(true);
	return TRUE;
}

MENUHANDLER(collapse_directories)
{
	global_history_contract(true);
	return TRUE;
}

MENUHANDLER(collapse_addresses)
{
	global_history_contract(false);
	return TRUE;
}

MENUHANDLER(launch)
{
	global_history_keypress(NS_KEY_CR);
	return TRUE;
}

/* file menu */
MENUHANDLER(export)
{
	struct nsgtk_global_history_window *ghwin;
	GtkWidget *save_dialog;

	ghwin = (struct nsgtk_global_history_window *)g;

	save_dialog = gtk_file_chooser_dialog_new("Save File",
			ghwin->wnd,
			GTK_FILE_CHOOSER_ACTION_SAVE,
			NSGTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
			NSGTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
			NULL);

	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(save_dialog),
			getenv("HOME") ? getenv("HOME") : "/");

	gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(save_dialog),
			"history.html");

	if (gtk_dialog_run(GTK_DIALOG(save_dialog)) == GTK_RESPONSE_ACCEPT) {
		gchar *filename = gtk_file_chooser_get_filename(
				GTK_FILE_CHOOSER(save_dialog));

		global_history_export(filename, NULL);
		g_free(filename);
	}

	gtk_widget_destroy(save_dialog);

	return TRUE;
}

/**
 * Connects menu events in the global history window.
 */
static void
nsgtk_global_history_init_menu(struct nsgtk_global_history_window *ghwin)
{
	struct menu_events *event = menu_events;
	GtkWidget *w;

	while (event->widget != NULL) {
		w = GTK_WIDGET(gtk_builder_get_object(ghwin->builder,
						      event->widget));
		if (w == NULL) {
			NSLOG(netsurf, INFO,
			      "Unable to connect menu widget ""%s""",
			      event->widget);
		} else {
			g_signal_connect(G_OBJECT(w),
					 "activate",
					 event->handler,
					 ghwin);
		}
		event++;
	}
}


/**
 * callback for mouse action on global history window
 *
 * \param nsgtk_cw The nsgtk core window structure.
 * \param mouse_state netsurf mouse state on event
 * \param x location of event
 * \param y location of event
 * \return NSERROR_OK on success otherwise apropriate error code
 */
static nserror
nsgtk_global_history_mouse(struct nsgtk_corewindow *nsgtk_cw,
		    browser_mouse_state mouse_state,
		    int x, int y)
{
	global_history_mouse_action(mouse_state, x, y);

	return NSERROR_OK;
}


/**
 * callback for keypress on global history window
 *
 * \param nsgtk_cw The nsgtk core window structure.
 * \param nskey The netsurf key code
 * \return NSERROR_OK on success otherwise apropriate error code
 */
static nserror
nsgtk_global_history_key(struct nsgtk_corewindow *nsgtk_cw, uint32_t nskey)
{
	if (global_history_keypress(nskey)) {
		return NSERROR_OK;
	}
	return NSERROR_NOT_IMPLEMENTED;
}


/**
 * callback on draw event for global history window
 *
 * \param nsgtk_cw The nsgtk core window structure.
 * \param r The rectangle of the window that needs updating.
 * \return NSERROR_OK on success otherwise apropriate error code
 */
static nserror
nsgtk_global_history_draw(struct nsgtk_corewindow *nsgtk_cw, struct rect *r)
{
	struct redraw_context ctx = {
		.interactive = true,
		.background_images = true,
		.plot = &nsgtk_plotters
	};

	global_history_redraw(0, 0, r, &ctx);

	return NSERROR_OK;
}

/**
 * Creates the window for the global history tree.
 *
 * \return NSERROR_OK on success else appropriate error code on faliure.
 */
static nserror nsgtk_global_history_init(void)
{
	struct nsgtk_global_history_window *ncwin;
	nserror res;

	if (global_history_window != NULL) {
		return NSERROR_OK;
	}

	ncwin = calloc(1, sizeof(*ncwin));
	if (ncwin == NULL) {
		return NSERROR_NOMEM;
	}

	res = nsgtk_builder_new_from_resname("globalhistory", &ncwin->builder);
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "History UI builder init failed");
		free(ncwin);
		return res;
	}

	gtk_builder_connect_signals(ncwin->builder, NULL);

	ncwin->wnd = GTK_WINDOW(gtk_builder_get_object(ncwin->builder,
						       "wndHistory"));

	ncwin->core.scrolled = GTK_SCROLLED_WINDOW(
		gtk_builder_get_object(ncwin->builder,
				       "globalHistoryScrolled"));

	ncwin->core.drawing_area = GTK_DRAWING_AREA(
		gtk_builder_get_object(ncwin->builder,
				       "globalHistoryDrawingArea"));

	/* make the delete event hide the window */
	g_signal_connect(G_OBJECT(ncwin->wnd),
			 "delete_event",
			 G_CALLBACK(gtk_widget_hide_on_delete),
			 NULL);

	nsgtk_global_history_init_menu(ncwin);

	ncwin->core.draw = nsgtk_global_history_draw;
	ncwin->core.key = nsgtk_global_history_key;
	ncwin->core.mouse = nsgtk_global_history_mouse;

	res = nsgtk_corewindow_init(&ncwin->core);
	if (res != NSERROR_OK) {
		free(ncwin);
		return res;
	}

	res = global_history_init((struct core_window *)ncwin);
	if (res != NSERROR_OK) {
		free(ncwin);
		return res;
	}

	/* memoise window so it can be represented when necessary
	 * instead of recreating every time.
	 */
	global_history_window = ncwin;

	return NSERROR_OK;
}


/* exported function documented gtk/history.h */
nserror nsgtk_global_history_present(void)
{
	nserror res;

	res = nsgtk_global_history_init();
	if (res == NSERROR_OK) {
		gtk_window_present(global_history_window->wnd);
	}
	return res;
}


/* exported function documented gtk/history.h */
nserror nsgtk_global_history_destroy(void)
{
	nserror res;

	if (global_history_window == NULL) {
		return NSERROR_OK;
	}

	res = global_history_fini();
	if (res == NSERROR_OK) {
		res = nsgtk_corewindow_fini(&global_history_window->core);
		gtk_widget_destroy(GTK_WIDGET(global_history_window->wnd));
		g_object_unref(G_OBJECT(global_history_window->builder));
		free(global_history_window);
		global_history_window = NULL;
	}

	return res;

}



