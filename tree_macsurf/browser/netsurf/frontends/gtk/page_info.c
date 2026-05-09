/*
 * Copyright 2015 Vincent Sanders <vince@netsurf-browser.org>
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
 * Implementation of gtk certificate viewing using gtk core windows.
 */

#include <stdint.h>
#include <stdlib.h>
#include <gtk/gtk.h>

#include "utils/log.h"
#include "utils/messages.h"
#include "netsurf/keypress.h"
#include "netsurf/plotters.h"
#include "netsurf/misc.h"
#include "netsurf/browser_window.h"
#include "desktop/page-info.h"
#include "desktop/gui_internal.h"

#include "gtk/plotters.h"
#include "gtk/scaffolding.h"
#include "gtk/resources.h"
#include "gtk/page_info.h"
#include "gtk/corewindow.h"


/**
 * GTK certificate viewing window context
 */
struct nsgtk_pi_window {
	/** GTK core window context */
	struct nsgtk_corewindow core;
	/** GTK builder for window */
	GtkBuilder *builder;
	/** GTK window being shown */
	GtkWindow *dlg;
	/** Core page-info window */
	struct page_info *pi;
};


/**
 * destroy a previously created page information window
 */
static gboolean
nsgtk_pi_delete_event(GtkWidget *w, GdkEvent  *event, gpointer data)
{
	struct nsgtk_pi_window *pi_win;
	pi_win = (struct nsgtk_pi_window *)data;

	page_info_destroy(pi_win->pi);

	nsgtk_corewindow_fini(&pi_win->core);
	gtk_widget_destroy(GTK_WIDGET(pi_win->dlg));
	g_object_unref(G_OBJECT(pi_win->builder));
	free(pi_win);

	return FALSE;
}

/**
 * Called to cause the page-info window to close cleanly
 */
static void
nsgtk_pi_close_callback(void *pw)
{
	nsgtk_pi_delete_event(NULL, NULL, pw);
}

/**
 * callback for mouse action for certificate verify on core window
 *
 * \param nsgtk_cw The nsgtk core window structure.
 * \param mouse_state netsurf mouse state on event
 * \param x location of event
 * \param y location of event
 * \return NSERROR_OK on success otherwise appropriate error code
 */
static nserror
nsgtk_pi_mouse(struct nsgtk_corewindow *nsgtk_cw,
		    browser_mouse_state mouse_state,
		    int x, int y)
{
	struct nsgtk_pi_window *pi_win;
	bool did_something = false;
	/* technically degenerate container of */
	pi_win = (struct nsgtk_pi_window *)nsgtk_cw;

	if (page_info_mouse_action(pi_win->pi, mouse_state, x, y, &did_something) == NSERROR_OK) {
		if (did_something == true) {
			/* Something happened so we need to close ourselves */
			guit->misc->schedule(0, nsgtk_pi_close_callback, pi_win);
		}
	}

	return NSERROR_OK;
}

/**
 * callback for keypress for certificate verify on core window
 *
 * \param nsgtk_cw The nsgtk core window structure.
 * \param nskey The netsurf key code
 * \return NSERROR_OK on success otherwise appropriate error code
 */
static nserror
nsgtk_pi_key(struct nsgtk_corewindow *nsgtk_cw, uint32_t nskey)
{
	struct nsgtk_pi_window *pi_win;

	/* technically degenerate container of */
	pi_win = (struct nsgtk_pi_window *)nsgtk_cw;

	if (page_info_keypress(pi_win->pi, nskey)) {
		return NSERROR_OK;
	}
	return NSERROR_NOT_IMPLEMENTED;
}

/**
 * callback on draw event for certificate verify on core window
 *
 * \param nsgtk_cw The nsgtk core window structure.
 * \param r The rectangle of the window that needs updating.
 * \return NSERROR_OK on success otherwise appropriate error code
 */
static nserror
nsgtk_pi_draw(struct nsgtk_corewindow *nsgtk_cw, struct rect *r)
{
	struct redraw_context ctx = {
		.interactive = true,
		.background_images = true,
		.plot = &nsgtk_plotters
	};
	struct nsgtk_pi_window *pi_win;

	/* technically degenerate container of */
	pi_win = (struct nsgtk_pi_window *)nsgtk_cw;

	page_info_redraw(pi_win->pi, 0, 0, r, &ctx);

	return NSERROR_OK;
}

/* exported interface documented in gtk/page_info.h */
nserror nsgtk_page_info(struct browser_window *bw)
{
	struct nsgtk_pi_window *ncwin;
	nserror res;
	GtkWindow *scaffwin = nsgtk_scaffolding_window(nsgtk_current_scaffolding());

	ncwin = calloc(1, sizeof(struct nsgtk_pi_window));
	if (ncwin == NULL) {
		return NSERROR_NOMEM;
	}

	res = nsgtk_builder_new_from_resname("pageinfo", &ncwin->builder);
	if (res != NSERROR_OK) {
		NSLOG(netsurf, CRITICAL, "Page Info UI builder init failed %s", messages_get_errorcode(res));
		free(ncwin);
		return res;
	}

	gtk_builder_connect_signals(ncwin->builder, NULL);

	ncwin->dlg = GTK_WINDOW(gtk_builder_get_object(ncwin->builder,
						       "PGIWindow"));

	/* Configure for transient behaviour */
	gtk_window_set_type_hint(GTK_WINDOW(ncwin->dlg),
				 GDK_WINDOW_TYPE_HINT_DROPDOWN_MENU);

	gtk_window_set_modal(GTK_WINDOW(ncwin->dlg), TRUE);

	gtk_window_group_add_window(gtk_window_get_group(scaffwin),
				    GTK_WINDOW(ncwin->dlg));

	gtk_window_set_transient_for(GTK_WINDOW(ncwin->dlg), scaffwin);

	gtk_window_set_screen(GTK_WINDOW(ncwin->dlg),
			      gtk_widget_get_screen(GTK_WIDGET(scaffwin)));

	/* Attempt to place the window in the right place */
	nsgtk_scaffolding_position_page_info(nsgtk_current_scaffolding(),
					     ncwin);

	ncwin->core.drawing_area = GTK_DRAWING_AREA(
		gtk_builder_get_object(ncwin->builder, "PGIDrawingArea"));

	/* make the delete event call our destructor */
	g_signal_connect(G_OBJECT(ncwin->dlg),
			 "delete_event",
			 G_CALLBACK(nsgtk_pi_delete_event),
			 ncwin);
	/* Ditto if we lose the grab */
	g_signal_connect(G_OBJECT(ncwin->dlg),
			 "grab-broken-event",
			 G_CALLBACK(nsgtk_pi_delete_event),
			 ncwin);
	/* Handle button press events */
	g_signal_connect(G_OBJECT(ncwin->dlg),
			 "button-press-event",
			 G_CALLBACK(nsgtk_pi_delete_event),
			 ncwin);

	/* initialise GTK core window */
	ncwin->core.draw = nsgtk_pi_draw;
	ncwin->core.key = nsgtk_pi_key;
	ncwin->core.mouse = nsgtk_pi_mouse;

	res = nsgtk_corewindow_init(&ncwin->core);
	if (res != NSERROR_OK) {
		g_object_unref(G_OBJECT(ncwin->dlg));
		free(ncwin);
		return res;
	}

	res = page_info_create((struct core_window *)ncwin, bw, &ncwin->pi);
	if (res != NSERROR_OK) {
		g_object_unref(G_OBJECT(ncwin->dlg));
		free(ncwin);
		return res;
	}

	gtk_widget_show(GTK_WIDGET(ncwin->dlg));

	gtk_widget_grab_focus(GTK_WIDGET(ncwin->dlg));

	return NSERROR_OK;
}

/* exported interface documented in gtk/page_info.h */
void
nsgtk_page_info_set_position(struct nsgtk_pi_window *win, int x, int y)
{
	NSLOG(netsurf, INFO, "win=%p x=%d y=%d", win, x, y);

	gtk_window_move(GTK_WINDOW(win->dlg), x, y);
}
