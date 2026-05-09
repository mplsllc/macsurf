/*
 * Copyright 2017 Vincent Sanders <vince@netsurf-browser.org>
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
 * Implementation of GTK local history manager.
 */

#include <stdint.h>
#include <stdlib.h>
#include <gtk/gtk.h>

#include "utils/log.h"
#include "netsurf/keypress.h"
#include "netsurf/plotters.h"
#include "desktop/local_history.h"

#include "gtk/compat.h"
#include "gtk/plotters.h"
#include "gtk/resources.h"
#include "gtk/corewindow.h"
#include "gtk/local_history.h"
#include "gtk/scaffolding.h"

struct nsgtk_local_history_window {
	struct nsgtk_corewindow core;

	GtkBuilder *builder;

	GtkWindow *wnd;

	struct local_history_session *session;
};

static struct nsgtk_local_history_window *local_history_window = NULL;



/**
 * callback for mouse action on local history window
 *
 * \param nsgtk_cw The nsgtk core window structure.
 * \param mouse_state netsurf mouse state on event
 * \param x location of event
 * \param y location of event
 * \return NSERROR_OK on success otherwise apropriate error code
 */
static nserror
nsgtk_local_history_mouse(struct nsgtk_corewindow *nsgtk_cw,
		    browser_mouse_state mouse_state,
		    int x, int y)
{
	struct nsgtk_local_history_window *lhw;
	/* technically degenerate container of */
	lhw = (struct nsgtk_local_history_window *)nsgtk_cw;

	local_history_mouse_action(lhw->session, mouse_state, x, y);

	return NSERROR_OK;
}


/**
 * callback for keypress on local history window
 *
 * \param nsgtk_cw The nsgtk core window structure.
 * \param nskey The netsurf key code
 * \return NSERROR_OK on success otherwise apropriate error code
 */
static nserror
nsgtk_local_history_key(struct nsgtk_corewindow *nsgtk_cw, uint32_t nskey)
{
	struct nsgtk_local_history_window *lhw;
	/* technically degenerate container of */
	lhw = (struct nsgtk_local_history_window *)nsgtk_cw;

	if (local_history_keypress(lhw->session, nskey)) {
		return NSERROR_OK;
	}
	return NSERROR_NOT_IMPLEMENTED;
}


/**
 * callback on draw event for local history window
 *
 * \param nsgtk_cw The nsgtk core window structure.
 * \param r The rectangle of the window that needs updating.
 * \return NSERROR_OK on success otherwise apropriate error code
 */
static nserror
nsgtk_local_history_draw(struct nsgtk_corewindow *nsgtk_cw, struct rect *r)
{
	struct redraw_context ctx = {
		.interactive = true,
		.background_images = true,
		.plot = &nsgtk_plotters
	};
	struct nsgtk_local_history_window *lhw;

	/* technically degenerate container of */
	lhw = (struct nsgtk_local_history_window *)nsgtk_cw;

	ctx.plot->clip(&ctx, r);
	local_history_redraw(lhw->session, 0, 0, r, &ctx);

	return NSERROR_OK;
}

/**
 * Creates the window for the local history view.
 *
 * \return NSERROR_OK on success else appropriate error code on faliure.
 */
static nserror
nsgtk_local_history_init(struct browser_window *bw,
			 struct nsgtk_local_history_window **win_out)
{
	struct nsgtk_local_history_window *ncwin;
	nserror res;

	/* memoise window so it can be represented when necessary
	 * instead of recreating every time.
	 */
	if ((*win_out) != NULL) {
		res = local_history_set((*win_out)->session, bw);
		return res;
	}

	ncwin = calloc(1, sizeof(*ncwin));
	if (ncwin == NULL) {
		return NSERROR_NOMEM;
	}

	res = nsgtk_builder_new_from_resname("localhistory", &ncwin->builder);
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Local history UI builder init failed");
		free(ncwin);
		return res;
	}

	gtk_builder_connect_signals(ncwin->builder, NULL);

	ncwin->wnd = GTK_WINDOW(gtk_builder_get_object(ncwin->builder,
						       "wndHistory"));

	/* Configure for transient behaviour */
	gtk_window_set_type_hint(GTK_WINDOW(ncwin->wnd),
				 GDK_WINDOW_TYPE_HINT_DROPDOWN_MENU);
	gtk_window_set_modal(GTK_WINDOW(ncwin->wnd), TRUE);


	ncwin->core.scrolled = GTK_SCROLLED_WINDOW(
		gtk_builder_get_object(ncwin->builder,
				       "HistoryScrolled"));

	ncwin->core.drawing_area = GTK_DRAWING_AREA(
		gtk_builder_get_object(ncwin->builder,
				       "HistoryDrawingArea"));

	/* make the delete event hide the window */
	g_signal_connect(G_OBJECT(ncwin->wnd),
			 "delete_event",
			 G_CALLBACK(gtk_widget_hide_on_delete),
			 NULL);
	/* Ditto if we lose the grab */
	g_signal_connect(G_OBJECT(ncwin->wnd),
			 "grab-broken-event",
			 G_CALLBACK(gtk_widget_hide_on_delete),
			 ncwin);
	/* Handle button press events */
	g_signal_connect(G_OBJECT(ncwin->wnd),
			 "button-press-event",
			 G_CALLBACK(gtk_widget_hide_on_delete),
			 ncwin);

	ncwin->core.draw = nsgtk_local_history_draw;
	ncwin->core.key = nsgtk_local_history_key;
	ncwin->core.mouse = nsgtk_local_history_mouse;

	res = nsgtk_corewindow_init(&ncwin->core);
	if (res != NSERROR_OK) {
		free(ncwin);
		return res;
	}

	res = local_history_init((struct core_window *)ncwin,
				 bw,
				 &ncwin->session);
	if (res != NSERROR_OK) {
		free(ncwin);
		return res;
	}

	*win_out = ncwin;

	return NSERROR_OK;
}


/* exported function documented gtk/history.h */
nserror nsgtk_local_history_present(GtkWindow *parent,
				    struct browser_window *bw)
{
	nserror res;
	int prnt_width, prnt_height;
	int width, height;
	res = nsgtk_local_history_init(bw, &local_history_window);
	if (res == NSERROR_OK) {
		gtk_window_group_add_window(gtk_window_get_group(parent),
					    local_history_window->wnd);
		gtk_window_set_transient_for(local_history_window->wnd, parent);
		gtk_window_set_screen(local_history_window->wnd,
				      gtk_widget_get_screen(GTK_WIDGET(parent)));

		gtk_window_get_size(parent, &prnt_width, &prnt_height);

		/* resize history widget ensureing the drawing area is
		 * no larger than parent window
		 */
		res = local_history_get_size(local_history_window->session,
					     &width,
					     &height);
		if (width > prnt_width) {
			width = prnt_width;
		}
		if (height > prnt_height) {
			height = prnt_height;
		}
		gtk_window_resize(local_history_window->wnd, width, height);

		/* Attempt to place the window in the right place */
		nsgtk_scaffolding_position_local_history(nsgtk_current_scaffolding());

		gtk_widget_show(GTK_WIDGET(local_history_window->wnd));
		gtk_widget_grab_focus(GTK_WIDGET(local_history_window->wnd));

		local_history_scroll_to_cursor(local_history_window->session);
	}

	return res;
}


/* exported function documented gtk/history.h */
nserror nsgtk_local_history_hide(void)
{
	nserror res = NSERROR_OK;

	if (local_history_window != NULL) {
		gtk_widget_hide(GTK_WIDGET(local_history_window->wnd));

		res = local_history_set(local_history_window->session, NULL);
	}

	return res;
}


/* exported function documented gtk/history.h */
nserror nsgtk_local_history_destroy(void)
{
	nserror res;

	if (local_history_window == NULL) {
		return NSERROR_OK;
	}

	res = local_history_fini(local_history_window->session);
	if (res == NSERROR_OK) {
		res = nsgtk_corewindow_fini(&local_history_window->core);
		gtk_widget_destroy(GTK_WIDGET(local_history_window->wnd));
		g_object_unref(G_OBJECT(local_history_window->builder));
		free(local_history_window);
		local_history_window = NULL;
	}

	return res;

}

/* exported function documented gtk/history.h */
void nsgtk_local_history_set_position(int x, int y)
{
	NSLOG(netsurf, INFO, "x=%d y=%d", x, y);

	gtk_window_move(local_history_window->wnd, x, y);
}
