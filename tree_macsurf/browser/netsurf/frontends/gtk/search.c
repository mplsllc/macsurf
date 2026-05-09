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


/**
 * \file
 * find in page gtk frontend implementation
 *
 * \todo this whole thing should be named find rather than search as
 *         that generally means web search and is confusing.
 */

#include <stdlib.h>
#include <stdbool.h>
#include <gtk/gtk.h>

#include "utils/nsoption.h"
#include "netsurf/search.h"
#include "desktop/search.h"

#include "gtk/compat.h"
#include "gtk/toolbar_items.h"
#include "gtk/window.h"
#include "gtk/search.h"


struct gtk_search {
	GtkToolbar *bar;
	GtkEntry *entry;
	GtkToolButton *back;
	GtkToolButton *forward;
	GtkToolButton *close;
	GtkCheckButton *checkAll;
	GtkCheckButton *caseSens;

	struct browser_window *bw;
};

/**
 * activate search forwards button in gui.
 *
 * \param active activate/inactivate
 * \param search the gtk search context
 */
static void nsgtk_search_set_forward_state(bool active, struct gtk_search *search)
{
	gtk_widget_set_sensitive(GTK_WIDGET(search->forward), active);
}


/**
 * activate search back button in gui.
 *
 * \param active activate/inactivate
 * \param search the gtk search context
 */
static void nsgtk_search_set_back_state(bool active, struct gtk_search *search)
{
	gtk_widget_set_sensitive(GTK_WIDGET(search->back), active);
}


/**
 * connected to the search forward button
 */
static gboolean
nsgtk_search_forward_button_clicked(GtkWidget *widget, gpointer data)
{
	struct gtk_search *search;
	search_flags_t flags;

	search = (struct gtk_search *)data;

	flags = SEARCH_FLAG_FORWARDS;

	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(search->caseSens))) {
		flags |= SEARCH_FLAG_CASE_SENSITIVE;
	}

	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(search->checkAll))) {
		flags |= SEARCH_FLAG_SHOWALL;
	}

	browser_window_search(search->bw, search, flags,
			      gtk_entry_get_text(search->entry));

	return TRUE;
}

/**
 * connected to the search back button
 */
static gboolean
nsgtk_search_back_button_clicked(GtkWidget *widget, gpointer data)
{
	struct gtk_search *search;
	search_flags_t flags;

	search = (struct gtk_search *)data;

	flags = 0;

	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(search->caseSens))) {
		flags |= SEARCH_FLAG_CASE_SENSITIVE;
	}

	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(search->checkAll))) {
		flags |= SEARCH_FLAG_SHOWALL;
	}

	browser_window_search(search->bw, search, flags,
			      gtk_entry_get_text(search->entry));

	return TRUE;
}

/**
 * connected to the search close button
 */
static gboolean
nsgtk_search_close_button_clicked(GtkWidget *widget, gpointer data)
{
	struct gtk_search *search;

	search = (struct gtk_search *)data;

	nsgtk_search_toggle_visibility(search);

	return TRUE;
}


/**
 * connected to the search entry [typing]
 */
static gboolean nsgtk_search_entry_changed(GtkWidget *widget, gpointer data)
{
	struct gtk_search *search;
	search_flags_t flags;

	search = (struct gtk_search *)data;

	flags = 0;

	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(search->caseSens))) {
		flags |= SEARCH_FLAG_CASE_SENSITIVE;
	}

	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(search->checkAll))) {
		flags |= SEARCH_FLAG_SHOWALL;
	}

	browser_window_search(search->bw, search, flags,
			      gtk_entry_get_text(search->entry));

	return TRUE;
}

/**
 * connected to the search entry [return key]
 */
static gboolean nsgtk_search_entry_activate(GtkWidget *widget, gpointer data)
{
	struct gtk_search *search;
	search_flags_t flags;

	search = (struct gtk_search *)data;

	flags = SEARCH_FLAG_FORWARDS;

	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(search->caseSens))) {
		flags |= SEARCH_FLAG_CASE_SENSITIVE;
	}

	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(search->checkAll))) {
		flags |= SEARCH_FLAG_SHOWALL;
	}

	browser_window_search(search->bw, search, flags,
			      gtk_entry_get_text(search->entry));

	return FALSE;
}

/**
 * allows escape key to close search bar too
 */
static gboolean
nsgtk_search_entry_key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
	if (event->keyval == GDK_KEY(Escape)) {
		struct gtk_search *search;
		search = (struct gtk_search *)data;

		nsgtk_search_toggle_visibility(search);
	}
	return FALSE;
}


static struct gui_search_table search_table = {
	.forward_state = (void *)nsgtk_search_set_forward_state,
	.back_state = (void *)nsgtk_search_set_back_state,
};

struct gui_search_table *nsgtk_search_table = &search_table;


/* exported interface documented in gtk/scaffolding.h */
nserror nsgtk_search_toggle_visibility(struct gtk_search *search)
{
	gboolean vis;

	browser_window_search_clear(search->bw);

	g_object_get(G_OBJECT(search->bar), "visible", &vis, NULL);
	if (vis) {
		gtk_widget_hide(GTK_WIDGET(search->bar));
	} else {
		gtk_widget_show(GTK_WIDGET(search->bar));
		gtk_widget_grab_focus(GTK_WIDGET(search->entry));
		nsgtk_search_entry_changed(GTK_WIDGET(search->entry), search);
	}

	return NSERROR_OK;
}


/* exported interface documented in gtk/search.h */
nserror nsgtk_search_restyle(struct gtk_search *search)
{
	switch (nsoption_int(button_type)) {

	case 1: /* Small icons */
		gtk_toolbar_set_style(GTK_TOOLBAR(search->bar),
				      GTK_TOOLBAR_ICONS);
		gtk_toolbar_set_icon_size(GTK_TOOLBAR(search->bar),
					  GTK_ICON_SIZE_SMALL_TOOLBAR);
		break;

	case 2: /* Large icons */
		gtk_toolbar_set_style(GTK_TOOLBAR(search->bar),
				      GTK_TOOLBAR_ICONS);
		gtk_toolbar_set_icon_size(GTK_TOOLBAR(search->bar),
					  GTK_ICON_SIZE_LARGE_TOOLBAR);
		break;

	case 3: /* Large icons with text */
		gtk_toolbar_set_style(GTK_TOOLBAR(search->bar),
				      GTK_TOOLBAR_BOTH);
		gtk_toolbar_set_icon_size(GTK_TOOLBAR(search->bar),
					  GTK_ICON_SIZE_LARGE_TOOLBAR);
		break;

	case 4: /* Text icons only */
		gtk_toolbar_set_style(GTK_TOOLBAR(search->bar),
				      GTK_TOOLBAR_TEXT);
		break;

	default:
		break;
	}
	return NSERROR_OK;
}


/* exported interface documented in gtk/search.h */
nserror
nsgtk_search_create(GtkBuilder *builder,
		    struct browser_window *bw,
		    struct gtk_search **search_out)
{
	struct gtk_search *search;

	search = malloc(sizeof(struct gtk_search));
	if (search == NULL) {
		return NSERROR_NOMEM;
	}

	search->bw = bw;

	search->bar = GTK_TOOLBAR(gtk_builder_get_object(builder, "findbar"));
	search->entry = GTK_ENTRY(gtk_builder_get_object(builder, "Find"));
	search->back = GTK_TOOL_BUTTON(gtk_builder_get_object(builder,
							"FindBack"));
	search->forward = GTK_TOOL_BUTTON(gtk_builder_get_object(builder,
							"FindForward"));
	search->close = GTK_TOOL_BUTTON(gtk_builder_get_object(builder,
							"FindClose"));
	search->checkAll = GTK_CHECK_BUTTON(gtk_builder_get_object(builder,
							"FindHighlightAll"));
	search->caseSens = GTK_CHECK_BUTTON(gtk_builder_get_object(builder,
							"FindMatchCase"));

	g_signal_connect(search->forward,
			 "clicked",
			 G_CALLBACK(nsgtk_search_forward_button_clicked),
			 search);

	g_signal_connect(search->back,
			 "clicked",
			 G_CALLBACK(nsgtk_search_back_button_clicked),
			 search);

	g_signal_connect(search->entry,
			 "changed",
			 G_CALLBACK(nsgtk_search_entry_changed),
			 search);

	g_signal_connect(search->entry,
			 "activate",
			 G_CALLBACK(nsgtk_search_entry_activate),
			 search);

	g_signal_connect(search->entry,
			 "key-press-event",
			 G_CALLBACK(nsgtk_search_entry_key),
			 search);

	g_signal_connect(search->close,
			 "clicked",
			 G_CALLBACK(nsgtk_search_close_button_clicked),
			 search);

	g_signal_connect(search->caseSens,
			 "toggled",
			 G_CALLBACK(nsgtk_search_entry_changed),
			 search);

	g_signal_connect(search->checkAll,
			 "toggled",
			 G_CALLBACK(nsgtk_search_entry_changed),
			 search);

	nsgtk_search_restyle(search);


	*search_out = search;

	return NSERROR_OK;
}
