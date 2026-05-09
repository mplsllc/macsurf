/*
 * Copyright 2009 Paul Blokus <paul_pl@users.sourceforge.net>
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
 * Implementation of GTK cookie manager.
 */

#include <stdint.h>
#include <stdlib.h>
#include <gtk/gtk.h>

#include "utils/log.h"
#include "netsurf/keypress.h"
#include "netsurf/plotters.h"
#include "desktop/cookie_manager.h"

#include "gtk/cookies.h"
#include "gtk/plotters.h"
#include "gtk/resources.h"
#include "gtk/corewindow.h"

struct nsgtk_cookie_window {
	struct nsgtk_corewindow core;
	GtkBuilder *builder;
	GtkWindow *wnd;
};

static struct nsgtk_cookie_window *cookie_window = NULL;

#define MENUPROTO(x) static gboolean nsgtk_on_##x##_activate(	\
		GtkMenuItem *widget, gpointer g)
#define MENUEVENT(x) { #x, G_CALLBACK(nsgtk_on_##x##_activate) }
#define MENUHANDLER(x) gboolean nsgtk_on_##x##_activate(GtkMenuItem *widget, \
		gpointer g)

struct menu_events {
	const char *widget;
	GCallback handler;
};

/* edit menu */
MENUPROTO(delete_selected);
MENUPROTO(delete_all);
MENUPROTO(select_all);
MENUPROTO(clear_selection);

/* view menu*/
MENUPROTO(expand_all);
MENUPROTO(expand_domains);
MENUPROTO(expand_cookies);
MENUPROTO(collapse_all);
MENUPROTO(collapse_domains);
MENUPROTO(collapse_cookies);


static struct menu_events menu_events[] = {

	/* edit menu */
	MENUEVENT(delete_selected),
	MENUEVENT(delete_all),
	MENUEVENT(select_all),
	MENUEVENT(clear_selection),

	/* view menu*/
	MENUEVENT(expand_all),
	MENUEVENT(expand_domains),
	MENUEVENT(expand_cookies),
	MENUEVENT(collapse_all),
	MENUEVENT(collapse_domains),
	MENUEVENT(collapse_cookies),

	{NULL, NULL}
};


/* edit menu */
MENUHANDLER(delete_selected)
{
	cookie_manager_keypress(NS_KEY_DELETE_LEFT);
	return TRUE;
}

MENUHANDLER(delete_all)
{
	cookie_manager_keypress(NS_KEY_ESCAPE);
	cookie_manager_keypress(NS_KEY_ESCAPE);
	cookie_manager_keypress(NS_KEY_SELECT_ALL);
	cookie_manager_keypress(NS_KEY_DELETE_LEFT);
	return TRUE;
}

MENUHANDLER(select_all)
{
	cookie_manager_keypress(NS_KEY_ESCAPE);
	cookie_manager_keypress(NS_KEY_ESCAPE);
	cookie_manager_keypress(NS_KEY_SELECT_ALL);
	return TRUE;
}

MENUHANDLER(clear_selection)
{
	cookie_manager_keypress(NS_KEY_CLEAR_SELECTION);
	return TRUE;
}

/* view menu*/
MENUHANDLER(expand_all)
{
	cookie_manager_expand(false);
	return TRUE;
}

MENUHANDLER(expand_domains)
{
	cookie_manager_expand(true);
	return TRUE;
}

MENUHANDLER(expand_cookies)
{
	cookie_manager_expand(false);
	return TRUE;
}

MENUHANDLER(collapse_all)
{
	cookie_manager_contract(true);
	return TRUE;
}

MENUHANDLER(collapse_domains)
{
	cookie_manager_contract(true);
	return TRUE;
}

MENUHANDLER(collapse_cookies)
{
	cookie_manager_contract(false);
	return TRUE;
}

/**
 * Connects menu events in the cookies window.
 */
static void nsgtk_cookies_init_menu(struct nsgtk_cookie_window *ncwin)
{
	struct menu_events *event = menu_events;
	GtkWidget *w;

	while (event->widget != NULL) {
		w = GTK_WIDGET(gtk_builder_get_object(ncwin->builder,
						      event->widget));
		if (w == NULL) {
			NSLOG(netsurf, INFO,
			      "Unable to connect menu widget ""%s""",
			      event->widget);
		} else {
			g_signal_connect(G_OBJECT(w),
					 "activate",
					 event->handler,
					 ncwin);
		}
		event++;
	}
}

/**
 * callback for mouse action on cookie window
 *
 * \param nsgtk_cw The nsgtk core window structure.
 * \param mouse_state netsurf mouse state on event
 * \param x location of event
 * \param y location of event
 * \return NSERROR_OK on success otherwise appropriate error code
 */
static nserror
nsgtk_cookies_mouse(struct nsgtk_corewindow *nsgtk_cw,
		    browser_mouse_state mouse_state,
		    int x, int y)
{
	cookie_manager_mouse_action(mouse_state, x, y);

	return NSERROR_OK;
}

/**
 * callback for keypress on cookie window
 *
 * \param nsgtk_cw The nsgtk core window structure.
 * \param nskey The netsurf key code
 * \return NSERROR_OK on success otherwise appropriate error code
 */
static nserror
nsgtk_cookies_key(struct nsgtk_corewindow *nsgtk_cw, uint32_t nskey)
{
	if (cookie_manager_keypress(nskey)) {
		return NSERROR_OK;
	}
	return NSERROR_NOT_IMPLEMENTED;
}

/**
 * callback on draw event for cookie window
 *
 * \param nsgtk_cw The nsgtk core window structure.
 * \param r The rectangle of the window that needs updating.
 * \return NSERROR_OK on success otherwise appropriate error code
 */
static nserror
nsgtk_cookies_draw(struct nsgtk_corewindow *nsgtk_cw, struct rect *r)
{
	struct redraw_context ctx = {
		.interactive = true,
		.background_images = true,
		.plot = &nsgtk_plotters
	};

	cookie_manager_redraw(0, 0, r, &ctx);

	return NSERROR_OK;
}

/**
 * Creates the window for the cookies tree.
 *
 * \return NSERROR_OK on success else appropriate error code on failure.
 */
static nserror nsgtk_cookies_init(void)
{
	struct nsgtk_cookie_window *ncwin;
	nserror res;

	if (cookie_window != NULL) {
		return NSERROR_OK;
	}

	ncwin = calloc(1, sizeof(*ncwin));
	if (ncwin == NULL) {
		return NSERROR_NOMEM;
	}

	res = nsgtk_builder_new_from_resname("cookies", &ncwin->builder);
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Cookie UI builder init failed");
		free(ncwin);
		return res;
	}

	gtk_builder_connect_signals(ncwin->builder, NULL);

	ncwin->wnd = GTK_WINDOW(gtk_builder_get_object(ncwin->builder,
						       "wndCookies"));

	ncwin->core.scrolled = GTK_SCROLLED_WINDOW(
		gtk_builder_get_object(ncwin->builder, "cookiesScrolled"));

	ncwin->core.drawing_area = GTK_DRAWING_AREA(
		gtk_builder_get_object(ncwin->builder, "cookiesDrawingArea"));

	/* make the delete event hide the window */
	g_signal_connect(G_OBJECT(ncwin->wnd),
			 "delete_event",
			 G_CALLBACK(gtk_widget_hide_on_delete),
			 NULL);

	nsgtk_cookies_init_menu(ncwin);

	ncwin->core.draw = nsgtk_cookies_draw;
	ncwin->core.key = nsgtk_cookies_key;
	ncwin->core.mouse = nsgtk_cookies_mouse;

	res = nsgtk_corewindow_init(&ncwin->core);
	if (res != NSERROR_OK) {
		free(ncwin);
		return res;
	}

	res = cookie_manager_init((struct core_window *)ncwin);
	if (res != NSERROR_OK) {
		free(ncwin);
		return res;
	}

	/* memoise window so it can be represented when necessary
	 * instead of recreating every time.
	 */
	cookie_window = ncwin;

	return NSERROR_OK;
}


/* exported function documented gtk/cookies.h */
nserror nsgtk_cookies_present(const char *search_term)
{
	nserror res;

	res = nsgtk_cookies_init();
	if (res == NSERROR_OK) {
		gtk_window_present(cookie_window->wnd);
		res = cookie_manager_set_search_string(search_term);
	}
	return res;
}


/* exported function documented gtk/cookies.h */
nserror nsgtk_cookies_destroy(void)
{
	nserror res;

	if (cookie_window == NULL) {
		return NSERROR_OK;
	}

	res = cookie_manager_fini();
	if (res == NSERROR_OK) {
		res = nsgtk_corewindow_fini(&cookie_window->core);
		gtk_widget_destroy(GTK_WIDGET(cookie_window->wnd));
		g_object_unref(G_OBJECT(cookie_window->builder));
		free(cookie_window);
		cookie_window = NULL;
	}

	return res;
}
