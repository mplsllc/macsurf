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

#include <stdint.h>
#include <string.h>

#include "utils/nsoption.h"
#include "utils/log.h"
#include "netsurf/browser_window.h"
#include "desktop/search.h"

#include "gtk/compat.h"
#include "gtk/toolbar_items.h"
#include "gtk/scaffolding.h"
#include "gtk/window.h"
#include "gtk/search.h"
#include "gtk/tabs.h"

#define TAB_WIDTH_N_CHARS 15

static gint srcpagenum;

/**
 * callback to update sizes when style-set gtk signal
 */
static void
nsgtk_tab_update_size(GtkWidget *hbox,
		      GtkStyle *previous_style,
		      GtkWidget *close_button)
{
	PangoFontMetrics *metrics;
	PangoContext *context;
	int char_width, h, w;
	GtkStyleContext *style;
	GtkStateFlags state;

	state = nsgtk_widget_get_state_flags(hbox);
	style = nsgtk_widget_get_style_context(hbox);

	context = gtk_widget_get_pango_context(hbox);
	metrics = pango_context_get_metrics(context,
				nsgtk_style_context_get_font(style, state),
				pango_context_get_language(context));

	char_width = pango_font_metrics_get_approximate_digit_width(metrics);
	pango_font_metrics_unref(metrics);

	nsgtk_icon_size_lookup_for_settings(gtk_widget_get_settings (hbox),
			GTK_ICON_SIZE_MENU, &w, &h);

	gtk_widget_set_size_request(hbox,
			TAB_WIDTH_N_CHARS * PANGO_PIXELS(char_width) + 2 * w,
			-1);

	gtk_widget_set_size_request(close_button, w + 4, h + 4);
}


/**
 * gtk event handler for button release on tab hbox
 */
static gboolean
nsgtk_tab_button_release(GtkWidget *widget,
			 GdkEventButton *event,
			 gpointer user_data)
{
	GtkWidget *page;

	if ((event->type == GDK_BUTTON_RELEASE) && (event->button == 2)) {
		page = (GtkWidget *)user_data;
		gtk_widget_destroy(page);
		return TRUE;
	}
	return FALSE;
}


/**
 * Create a notebook tab label
 *
 * \param page The page content widget
 * \param title The title of the page
 * \param icon_pixbuf The icon of the page
 */
static GtkWidget *
nsgtk_tab_label_setup(GtkWidget *page,
		      const char *title,
		      GdkPixbuf *icon_pixbuf)
{
	GtkWidget *ebox, *hbox, *favicon, *label, *button, *close;

	/* horizontal box */
	hbox = nsgtk_hbox_new(FALSE, 3);

	/* event box */
	ebox = gtk_event_box_new();
	gtk_widget_set_events(ebox, GDK_BUTTON_PRESS_MASK);
	gtk_container_add(GTK_CONTAINER(ebox), hbox);

	/* construct a favicon */
	favicon = gtk_image_new();
	if (icon_pixbuf != NULL) {
		gtk_image_set_from_pixbuf(GTK_IMAGE(favicon), icon_pixbuf);
	}

	/* construct a label */
	label = gtk_label_new(title);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
	nsgtk_widget_set_alignment(label, GTK_ALIGN_START, GTK_ALIGN_CENTER);
	nsgtk_widget_set_margins(label, 0, 0);
	gtk_widget_show(label);

	/* construct a close button  */
	button = gtk_button_new();

	close = nsgtk_image_new_from_stock(NSGTK_STOCK_CLOSE,
					   GTK_ICON_SIZE_LARGE_TOOLBAR);
	gtk_container_add(GTK_CONTAINER(button), close);
	nsgtk_button_set_focus_on_click(GTK_BUTTON(button), FALSE);
	gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(button, "Close this tab.");

	/* pack the widgets into the label box */
	gtk_box_pack_start(GTK_BOX(hbox), favicon, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);

	/* make the icon and label widgets findable by name */
	g_object_set_data(G_OBJECT(ebox), "favicon", favicon);
	g_object_set_data(G_OBJECT(ebox), "label", label);

	/* attach signal handlers */
	g_signal_connect_swapped(button,
				 "clicked",
				 G_CALLBACK(gtk_widget_destroy), page);

	g_signal_connect(hbox,
			 "style-set",
			 G_CALLBACK(nsgtk_tab_update_size),
			 button);

	g_signal_connect(ebox,
			 "button-release-event",
			 G_CALLBACK(nsgtk_tab_button_release),
			 page);


	gtk_widget_show_all(ebox);

	return ebox;
}


/**
 * The before switch-page gtk signal handler
 *
 * This signal is handled both before and after delivery to work round
 * issue that setting the selected tab during the switch-page signal
 * fails
 *
 * \param notebook The notebook being changed
 * \param page The notebook page being switched to
 * \param selpagenum The currently selected page number
 * \param user_data Unused
 */
static void
nsgtk_tab_switch_page(GtkNotebook *notebook,
		      GtkWidget *page,
		      guint selpagenum,
		      gpointer user_data)
{
	srcpagenum = gtk_notebook_get_current_page(notebook);
}


/**
 * The after switch-page gtk signal handler
 *
 * \param notebook The notebook being changed
 * \param selpage The notebook page selected
 * \param selpagenum The currently selected page number
 * \param user_data Unused
 */
static void
nsgtk_tab_switch_page_after(GtkNotebook *notebook,
			    GtkWidget *selpage,
			    guint selpagenum,
			    gpointer user_data)
{
	GtkWidget *srcpage;
	GtkWidget *addpage;
	GtkMenuBar *menubar;
	struct gui_window *gw = NULL;
	nserror res = NSERROR_INVALID;

	addpage = g_object_get_data(G_OBJECT(notebook), "addtab");

	/* check if trying to select the "add page" tab */
	if (selpage != addpage) {
		NSLOG(netsurf, INFO, "sel %d", selpagenum);
		menubar = nsgtk_scaffolding_menu_bar(nsgtk_scaffolding_from_notebook(notebook));
		gw = g_object_get_data(G_OBJECT(selpage), "gui_window");
		if (gw != NULL) {
			/* tab with web page in it */
			nsgtk_scaffolding_set_top_level(gw);
			gtk_widget_show(GTK_WIDGET(addpage));
			gtk_widget_set_sensitive(GTK_WIDGET(menubar), true);
		} else {
			/* tab with non browser content (e.g. tb customize) */
			gtk_widget_hide(GTK_WIDGET(addpage));
			gtk_widget_set_sensitive(GTK_WIDGET(menubar), false);
		}
		return;
	}

	NSLOG(netsurf, INFO, "src %d sel %d", srcpagenum, selpagenum);

	/* ensure the add tab is not already selected */
	if ((srcpagenum == -1) || (srcpagenum == (gint)selpagenum)) {
		return;
	}

	srcpage = gtk_notebook_get_nth_page(notebook, srcpagenum);

	gw = g_object_get_data(G_OBJECT(srcpage), "gui_window");

	if (gw != NULL) {
		res = nsgtk_window_item_activate(gw, NEWTAB_BUTTON);
	}
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Failed to open new tab.");
	}
}


/**
 * The tab reordered gtk signal handler
 *
 * \param notebook The notebook being changed
 * \param page_num The currently selected page number
 * \param user_data Unused
 */
static void
nsgtk_tab_page_reordered(GtkNotebook *notebook,
			 GtkWidget *child,
			 guint page_num,
			 gpointer user_data)
{
	gint pages;
	GtkWidget *addpage;

	pages = gtk_notebook_get_n_pages(notebook);
	addpage = g_object_get_data(G_OBJECT(notebook), "addtab");

	if (((gint)page_num == (pages - 1)) &&
	    (child != addpage)) {
		/* moved tab to end */
		gtk_notebook_reorder_child(notebook, addpage, -1);
	}
}

/**
 * The tab orientation signal handler
 *
 * \param notebook The notebook being changed
 * \param page_num The currently selected page number
 * \param user_data Unused
 */
static void
nsgtk_tab_orientation(GtkNotebook *notebook)
{
	switch (nsoption_int(position_tab)) {
	case 0:
		gtk_notebook_set_tab_pos(notebook, GTK_POS_TOP);
		break;

	case 1:
		gtk_notebook_set_tab_pos(notebook, GTK_POS_LEFT);
		break;

	case 2:
		gtk_notebook_set_tab_pos(notebook, GTK_POS_RIGHT);
		break;

	case 3:
		gtk_notebook_set_tab_pos(notebook, GTK_POS_BOTTOM);
		break;

	}
}

/**
 * adds a "new tab" tab
 */
static GtkWidget *
nsgtk_tab_add_newtab(GtkNotebook *notebook)
{
	GtkWidget *tablabel;
	GtkWidget *tabcontents;
	GtkWidget *add;

	tablabel = nsgtk_hbox_new(FALSE, 1);
	tabcontents = nsgtk_hbox_new(FALSE, 1);

	add = gtk_image_new_from_icon_name(NSGTK_STOCK_ADD,
					   GTK_ICON_SIZE_LARGE_TOOLBAR);
	gtk_widget_set_tooltip_text(add, "New Tab");

	gtk_box_pack_start(GTK_BOX(tablabel), add, FALSE, FALSE, 0);

	gtk_widget_show_all(tablabel);

	gtk_notebook_append_page(notebook, tabcontents, tablabel);

	gtk_notebook_set_tab_reorderable(notebook, tabcontents, false);

	gtk_widget_show_all(tabcontents);

	g_object_set_data(G_OBJECT(notebook), "addtab", tabcontents);

	return tablabel;
}


/**
 * callback to alter tab visibility when pages are added or removed
 */
static void
nsgtk_tab_visibility_update(GtkNotebook *notebook, GtkWidget *child, guint page)
{
	gint pagec;
	GtkWidget *addpage;

	pagec = gtk_notebook_get_n_pages(notebook);
	if (pagec > 1) {
		addpage = g_object_get_data(G_OBJECT(notebook), "addtab");
		if (addpage != NULL) {
			pagec--; /* skip the add tab */
			if ((gint)page == pagec) {
				/* ensure the add new tab cannot be current */
				gtk_notebook_set_current_page(notebook,
							      page - 1);
			}
		}
	}

	if ((nsoption_bool(show_single_tab) == true) || (pagec > 1)) {
		gtk_notebook_set_show_tabs(notebook, TRUE);
	} else {
		gtk_notebook_set_show_tabs(notebook, FALSE);
	}
}


/* exported interface documented in gtk/tabs.h */
void nsgtk_tab_options_changed(GtkNotebook *notebook)
{
	nsgtk_tab_orientation(notebook);
	nsgtk_tab_visibility_update(notebook, NULL, 0);
}


/* exported interface documented in gtk/tabs.h */
nserror nsgtk_notebook_create(GtkBuilder *builder, GtkNotebook **notebook_out)
{
	GtkNotebook *notebook;

	notebook = GTK_NOTEBOOK(gtk_builder_get_object(builder, "notebook"));

	nsgtk_tab_add_newtab(notebook);

	g_signal_connect(notebook,
			 "switch-page",
			 G_CALLBACK(nsgtk_tab_switch_page),
			 NULL);
	g_signal_connect_after(notebook,
			       "switch-page",
			       G_CALLBACK(nsgtk_tab_switch_page_after),
			       NULL);
	g_signal_connect(notebook,
			 "page-removed",
			 G_CALLBACK(nsgtk_tab_visibility_update),
			 NULL);
	g_signal_connect(notebook,
			 "page-added",
			 G_CALLBACK(nsgtk_tab_visibility_update),
			 NULL);
	g_signal_connect(notebook,
			 "page-reordered",
			 G_CALLBACK(nsgtk_tab_page_reordered),
			 NULL);

	nsgtk_tab_options_changed(notebook);

	*notebook_out = notebook;

	return NSERROR_OK;
}

/* exported interface documented in gtk/tabs.h */
nserror
nsgtk_tab_add_page(GtkNotebook *notebook,
		   GtkWidget *tab_contents,
		   bool background,
		   const char *title,
		   GdkPixbuf *icon_pixbuf)
{
	GtkWidget *tabBox;
	gint remember;
	gint pages;
	gint newpage;

	tabBox = nsgtk_tab_label_setup(tab_contents, title, icon_pixbuf);

	remember = gtk_notebook_get_current_page(notebook);

	pages = gtk_notebook_get_n_pages(notebook);

	newpage = gtk_notebook_insert_page(notebook, tab_contents, tabBox, pages - 1);

	gtk_notebook_set_tab_reorderable(notebook, tab_contents, true);

	gtk_widget_show_all(tab_contents);

	if (background) {
		gtk_notebook_set_current_page(notebook, remember);
	} else {
		gtk_notebook_set_current_page(notebook, newpage);
	}

	return NSERROR_OK;
}


/* exported interface documented in gtk/tabs.h */
void nsgtk_tab_add(struct gui_window *gw,
		   GtkWidget *tab_contents,
		   bool background,
		   const char *title,
		   GdkPixbuf *icon_pixbuf)
{
	GtkNotebook *notebook;

	g_object_set_data(G_OBJECT(tab_contents), "gui_window", gw);

	notebook = nsgtk_scaffolding_notebook(nsgtk_get_scaffold(gw));

	nsgtk_tab_add_page(notebook, tab_contents, background, title, icon_pixbuf);

}


/* exported interface documented in gtk/tabs.h */
nserror nsgtk_tab_set_icon(GtkWidget *page, GdkPixbuf *pixbuf)
{
	GtkImage *favicon;
	GtkWidget *tab_label;
	GtkNotebook *notebook;

	if (pixbuf == NULL) {
		return NSERROR_INVALID;
	}
	notebook = GTK_NOTEBOOK(gtk_widget_get_ancestor(page, GTK_TYPE_NOTEBOOK));
	if (notebook == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	tab_label = gtk_notebook_get_tab_label(notebook, page);
	if (tab_label == NULL) {
		return NSERROR_INVALID;
	}

	favicon = GTK_IMAGE(g_object_get_data(G_OBJECT(tab_label), "favicon"));

	gtk_image_set_from_pixbuf(favicon, pixbuf);

	return NSERROR_OK;
}


/* exported interface documented in gtk/tabs.h */
nserror nsgtk_tab_set_title(GtkWidget *page, const char *title)
{
	GtkLabel *label;
	GtkWidget *tab_label;
	GtkNotebook *notebook;

	if (title == NULL) {
		return NSERROR_INVALID;
	}

	notebook = GTK_NOTEBOOK(gtk_widget_get_ancestor(page, GTK_TYPE_NOTEBOOK));
	if (notebook == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	tab_label = gtk_notebook_get_tab_label(notebook, page);
	if (tab_label == NULL) {
		return NSERROR_INVALID;
	}

	label = GTK_LABEL(g_object_get_data(G_OBJECT(tab_label), "label"));

	gtk_label_set_text(label, title);
	gtk_widget_set_tooltip_text(tab_label, title);

	return NSERROR_OK;
}

/* exported interface documented in gtk/tabs.h */
nserror nsgtk_tab_close_current(GtkNotebook *notebook)
{
	gint pagen;
	GtkWidget *page;
	struct gui_window *gw;
	GtkWidget *addpage;

	pagen = gtk_notebook_get_current_page(notebook);
	if (pagen == -1) {
		return NSERROR_OK;
	}

	page = gtk_notebook_get_nth_page(notebook, pagen);
	if (page == NULL) {
		return NSERROR_OK;
	}

	addpage = g_object_get_data(G_OBJECT(notebook), "addtab");
	if (page == addpage) {
		/* the add new tab page is current, cannot close that */
		return NSERROR_OK;
	}

	gw = g_object_get_data(G_OBJECT(page), "gui_window");
	if (gw == NULL) {
		return NSERROR_OK;
	}

	nsgtk_window_destroy_browser(gw);

	return NSERROR_OK;
}

nserror nsgtk_tab_prev(GtkNotebook *notebook)
{
	gtk_notebook_prev_page(notebook);

	return NSERROR_OK;

}

nserror nsgtk_tab_next(GtkNotebook *notebook)
{
	gint pagen;
	GtkWidget *page;
	GtkWidget *addpage;

	pagen = gtk_notebook_get_current_page(notebook);
	if (pagen == -1) {
		return NSERROR_OK;
	}

	page = gtk_notebook_get_nth_page(notebook, pagen + 1);
	if (page == NULL) {
		return NSERROR_OK;
	}

	addpage = g_object_get_data(G_OBJECT(notebook), "addtab");
	if (page == addpage) {
		/* cannot make add new tab page current */
		return NSERROR_OK;
	}

	gtk_notebook_set_current_page(notebook, pagen + 1);

	return NSERROR_OK;
}
