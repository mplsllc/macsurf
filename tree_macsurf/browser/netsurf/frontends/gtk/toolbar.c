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
 * implementation of toolbar to control browsing context
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <gtk/gtk.h>

#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "utils/file.h"
#include "utils/nsurl.h"
#include "utils/corestrings.h"
#include "desktop/browser_history.h"
#include "desktop/searchweb.h"
#include "desktop/search.h"
#include "desktop/save_complete.h"
#include "desktop/save_text.h"
#include "desktop/print.h"
#include "desktop/hotlist.h"
#include "netsurf/content.h"
#include "netsurf/browser_window.h"
#include "netsurf/keypress.h"

#include "gtk/toolbar_items.h"
#include "gtk/completion.h"
#include "gtk/gui.h"
#include "gtk/warn.h"
#include "gtk/search.h"
#include "gtk/throbber.h"
#include "gtk/scaffolding.h"
#include "gtk/window.h"
#include "gtk/compat.h"
#include "gtk/resources.h"
#include "gtk/schedule.h"
#include "gtk/local_history.h"
#include "gtk/global_history.h"
#include "gtk/viewsource.h"
#include "gtk/download.h"
#include "gtk/viewdata.h"
#include "gtk/tabs.h"
#include "gtk/print.h"
#include "gtk/layout_pango.h"
#include "gtk/preferences.h"
#include "gtk/hotlist.h"
#include "gtk/cookies.h"
#include "gtk/about.h"
#include "gtk/gdk.h"
#include "gtk/bitmap.h"
#include "gtk/page_info.h"
#include "gtk/toolbar.h"

/**
 * button location indicating button is not to be shown
 */
#define INACTIVE_LOCATION (-1)

/**
 * time (in ms) between throbber animation frame updates
 */
#define THROBBER_FRAME_TIME (100)

/**
 * the minimum number of columns in the tool store
 */
#define NSGTK_MIN_STORE_COLUMNS 4

/**
 * the 'standard' width of a button that makes sufficient of its label visible
 */
#define NSGTK_BUTTON_WIDTH 120

/**
 * the 'standard' height of a button that fits as many toolbars as
 *   possible into the store
 */
#define NSGTK_BUTTON_HEIGHT 70

/**
 * the 'normal' width of the websearch bar
 */
#define NSGTK_WEBSEARCH_WIDTH 150

/**
 * toolbar item context
 */
struct nsgtk_toolbar_item {

	/**
	 * GTK widget in the toolbar
	 */
	GtkToolItem *button;

	/**
	 * location index in toolbar
	 */
	int location;

	/**
	 * if the item is currently sensitive in the toolbar
	 */
	bool sensitivity;

	/**
	 * textural name used in serialising items
	 */
	const char *name;

	/**
	 * button clicked on toolbar handler
	 */
	gboolean (*clicked)(GtkWidget *widget, gpointer data);

	/**
	 * handler when dragging from customisation toolbox to toolbar
	 */
	void *dataplus;

	/**
	 * handler when dragging from toolbar to customisation toolbox
	 */
	void *dataminus;
};

/**
 * Location focus state machine
 *
 * 1. If we don't care, we're in LFS_IDLE
 * 2. When we create a new toolbar, we can put it into
 *    LFS_WANT which means that we want the url bar to focus
 * 3. When we start throbbing if we're in LFS_WANT we move to LFS_THROB
 * 4. When we stop throbbing, if we're in LFS_THROB we move to LFS_LAST
 *
 * While not in LFS_IDLE, if the url bar is updated and we previously had it
 * fully selected then we reselect it all.  If we're in LFS_LAST we move to
 * LFS_IDLE at that point.
 */
typedef enum {
	LFS_IDLE, /**< Nothing to do */
	LFS_WANT, /**< Want focus, will apply */
	LFS_THROB, /**< Want focus, we have started throbbing */
	LFS_LAST, /**< Last chance for a focus update */
} nsgtk_toolbar_location_focus_state;

/**
 * control toolbar context
 */
struct nsgtk_toolbar {
	/** gtk toolbar widget */
	GtkToolbar *widget;

	/* toolbar size allocation context */
	int offset;
	int toolbarmem;
	int toolbarbase;
	int historybase;

	/**
	 * Toolbar item contexts
	 */
	struct nsgtk_toolbar_item items[PLACEHOLDER_BUTTON];

	/**
	 * Current frame of throbber animation
	 */
	int throb_frame;

	/**
	 * Web search widget
	 */
	GtkWidget *webSearchEntry;

	/**
	 * callback to obtain a browser window for navigation
	 */
	struct browser_window *(*get_bw)(void *ctx);

	/**
	 * context passed to get_bw function
	 */
	void *get_ctx;

	/**
	 * Location focus state machine, current state
	 */
	nsgtk_toolbar_location_focus_state loc_focus;
};


/**
 * toolbar cusomisation context
 */
struct nsgtk_toolbar_customisation {
	/**
	 * first entry is a toolbar widget so a customisation widget
	 *   can be cast to toolbar and back.
	 */
	struct nsgtk_toolbar toolbar;

	/**
	 * The top level container (tabBox)
	 */
	GtkWidget *container;

	/**
	 * The vertical box into which the available tools are shown
	 */
	GtkBox *toolbox;

	/**
	 * widget handles for items in the customisation toolbox area
	 */
	GtkToolItem *items[PLACEHOLDER_BUTTON];

	/**
	 * which item is being dragged
	 */
	int dragitem; /* currentbutton */
	/**
	 * true if item being dragged onto toolbar, false if from toolbar
	 */
	bool dragfrom; /*fromstore */

};


/* forward declaration */
static nserror toolbar_item_create(nsgtk_toolbar_button id,
				   struct nsgtk_toolbar_item *item_out);


/**
 * returns a string without its underscores
 *
 * \param s The string to change.
 * \param replacespace true to insert a space where there was an underscore
 * \return The altered string
 */
static char *remove_underscores(const char *s, bool replacespace)
{
	size_t i, ii, len;
	char *ret;
	len = strlen(s);
	ret = malloc(len + 1);
	if (ret == NULL) {
		return NULL;
	}
	for (i = 0, ii = 0; i < len; i++) {
		if (s[i] != '_') {
			ret[ii++] = s[i];
		} else if (replacespace) {
			ret[ii++] = ' ';
		}
	}
	ret[ii] = '\0';
	return ret;
}


/**
 * create throbber toolbar item widget
 *
 * create a gtk entry widget with a completion attached
 */
static GtkToolItem *
make_toolbar_item_throbber(bool sensitivity, bool edit)
{
	nserror res;
	GtkToolItem *item;
	GdkPixbuf *pixbuf;
	GtkWidget *image;

	res = nsgtk_throbber_get_frame(0, &pixbuf);
	if (res != NSERROR_OK) {
		return NULL;
	}

	if (edit) {
		const char *msg;
		msg = messages_get("ToolThrob");
		item = gtk_tool_button_new(
				GTK_WIDGET(gtk_image_new_from_pixbuf(pixbuf)),
				msg);
	} else {
		item = gtk_tool_item_new();

		image = gtk_image_new_from_pixbuf(pixbuf);
		if (image != NULL) {
			nsgtk_widget_set_alignment(image,
						   GTK_ALIGN_CENTER,
						   GTK_ALIGN_CENTER);
			nsgtk_widget_set_margins(image, 3, 0);

			gtk_container_add(GTK_CONTAINER(item), image);
		}
	}
	gtk_widget_set_sensitive(GTK_WIDGET(item), sensitivity);

	return item;
}


/**
 * create url bar toolbar item widget
 *
 * create a gtk entry widget with a completion attached
 *
 * \param sensitivity if the entry should be created sensitive to input
 * \param edit if the entry should be editable
 */
static GtkToolItem *
make_toolbar_item_url_bar(bool sensitivity, bool edit)
{
	GtkToolItem *item;
	GtkWidget *entry;
	GtkEntryCompletion *completion;

	entry = nsgtk_entry_new();

	if (entry == NULL) {
		return NULL;
	}
	nsgtk_entry_set_icon_from_icon_name(entry,
					    GTK_ENTRY_ICON_PRIMARY,
					    "page-info-internal");

	if (edit) {
		gtk_entry_set_width_chars(GTK_ENTRY(entry), 9);

		item = gtk_tool_button_new(NULL, "URL");
		gtk_tool_button_set_icon_widget(GTK_TOOL_BUTTON(item), entry);
	} else {
		completion = gtk_entry_completion_new();
		if (completion != NULL) {
			gtk_entry_set_completion(GTK_ENTRY(entry), completion);
		}

		item = gtk_tool_item_new();
		if (item == NULL) {
			return NULL;
		}

		gtk_container_add(GTK_CONTAINER(item), entry);
		gtk_tool_item_set_expand(item, TRUE);

	}
	gtk_widget_set_sensitive(GTK_WIDGET(item), TRUE);
	gtk_widget_set_sensitive(GTK_WIDGET(entry), sensitivity);

	return item;
}


/**
 * create web search toolbar item widget
 */
static GtkToolItem *
make_toolbar_item_websearch(bool sensitivity, bool edit)
{
	GtkToolItem *item;
	nserror res;
	GtkWidget *entry;
	struct bitmap *bitmap;
	GdkPixbuf *pixbuf = NULL;

	res = search_web_get_provider_bitmap(&bitmap);
	if ((res == NSERROR_OK) && (bitmap != NULL)) {
		pixbuf = nsgdk_pixbuf_get_from_surface(bitmap->surface, 32, 32);
	}

	entry = nsgtk_entry_new();

	if (entry == NULL) {
		return NULL;
	}

	if (pixbuf != NULL) {
		nsgtk_entry_set_icon_from_pixbuf(entry,
						 GTK_ENTRY_ICON_PRIMARY,
						 pixbuf);
		g_object_unref(pixbuf);
	} else {
		nsgtk_entry_set_icon_from_icon_name(entry,
						    GTK_ENTRY_ICON_PRIMARY,
						    NSGTK_STOCK_INFO);
	}

	if (edit) {
		gtk_entry_set_width_chars(GTK_ENTRY(entry), 9);

		item = gtk_tool_button_new(NULL, "Web Search");
		gtk_tool_button_set_icon_widget(GTK_TOOL_BUTTON(item),
						entry);
	} else {
		gtk_widget_set_size_request(entry, NSGTK_WEBSEARCH_WIDTH, -1);

		item = gtk_tool_item_new();
		if (item == NULL) {
			return NULL;
		}

		gtk_container_add(GTK_CONTAINER(item), entry);
	}
	gtk_widget_set_sensitive(GTK_WIDGET(item), TRUE);
	gtk_widget_set_sensitive(GTK_WIDGET(entry), sensitivity);

	return item;
}


/**
 * create local history toolbar item widget
 */
static GtkToolItem *
make_toolbar_item_history(bool sensitivity, bool edit)
{
	GtkToolItem *item;
	const char *msg = "H";
	char *label = NULL;

	if (edit) {
		msg = messages_get("gtkLocalHistory");
	}
	label = remove_underscores(msg, false);
	item = gtk_tool_button_new(NULL, label);
	if (label != NULL) {
		free(label);
	}
	gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(item), "local-history");

	/* set history widget minimum width */
	gtk_widget_set_size_request(GTK_WIDGET(item), 20, -1);
	gtk_widget_set_sensitive(GTK_WIDGET(item), sensitivity);

	return item;
}


/**
 * create generic button toolbar item widget
 */
static GtkToolItem *
make_toolbar_item_button(const char *labelmsg,
			 const char *iconname,
			 bool sensitivity,
			 bool edit)
{
	GtkToolItem *item;
	char *label = NULL;

	label = remove_underscores(messages_get(labelmsg), false);

	item = gtk_tool_button_new(NULL, label);
	if (label != NULL) {
		free(label);
	}

	if (item != NULL) {
		gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(item), iconname);

		gtk_widget_set_sensitive(GTK_WIDGET(item), sensitivity);
		if (edit) {
			nsgtk_widget_set_margins(GTK_WIDGET(item), 0, 0);
		}
	}

	return item;
}


/**
 * widget factory for creation of toolbar item widgets
 *
 * \param i the id of the widget
 * \param theme the theme to make the widgets from
 * \return gtk widget
 */
static GtkToolItem *
make_toolbar_item(nsgtk_toolbar_button itemid, bool sensitivity)
{
	GtkToolItem *toolitem = NULL;

	switch(itemid) {
#define TOOLBAR_ITEM_y(identifier, label, iconame)
#define TOOLBAR_ITEM_n(identifier, label, iconame)
#define TOOLBAR_ITEM_t(identifier, label, iconame)		\
	case identifier:					\
		toolitem = make_toolbar_item_button(#label, iconame, sensitivity, false); \
		break;
#define TOOLBAR_ITEM_b(identifier, label, iconame)		\
	case identifier:					\
		toolitem = make_toolbar_item_button(#label, iconame, sensitivity, false); \
		break;
#define TOOLBAR_ITEM(identifier, name, snstvty, clicked, activate, label, iconame) \
		TOOLBAR_ITEM_ ## clicked(identifier, label, iconame)

#include "gtk/toolbar_items.h"

#undef TOOLBAR_ITEM_t
#undef TOOLBAR_ITEM_b
#undef TOOLBAR_ITEM_n
#undef TOOLBAR_ITEM_y
#undef TOOLBAR_ITEM

	case HISTORY_BUTTON:
		toolitem = make_toolbar_item_history(sensitivity, false);
		break;

	case URL_BAR_ITEM:
		toolitem = make_toolbar_item_url_bar(sensitivity, false);
		break;

	case THROBBER_ITEM:
		toolitem = make_toolbar_item_throbber(sensitivity, false);
		break;

	case WEBSEARCH_ITEM:
		toolitem = make_toolbar_item_websearch(sensitivity, false);
		break;

	default:
		break;

	}
	return toolitem;
}


/**
 * widget factory for creation of toolbar item widgets for the toolbox
 *
 * \param itemid the id of the widget
 * \return gtk tool item widget
 */
static GtkToolItem *
make_toolbox_item(nsgtk_toolbar_button itemid, bool bar)
{
	GtkToolItem *toolitem = NULL;

	switch(itemid) {
#define TOOLBAR_ITEM_y(identifier, label, iconame)
#define TOOLBAR_ITEM_n(identifier, label, iconame)
#define TOOLBAR_ITEM_t(identifier, label, iconame)		\
	case identifier:					\
		if (bar) {						\
			toolitem = make_toolbar_item_button(#label, iconame, true, true); \
		}							\
		break;
#define TOOLBAR_ITEM_b(identifier, label, iconame)		\
	case identifier:					\
		toolitem = make_toolbar_item_button(#label, iconame, true, true); \
		break;
#define TOOLBAR_ITEM(identifier, name, snstvty, clicked, activate, label, iconame) \
		TOOLBAR_ITEM_ ## clicked(identifier, label, iconame)

#include "gtk/toolbar_items.h"

#undef TOOLBAR_ITEM_t
#undef TOOLBAR_ITEM_b
#undef TOOLBAR_ITEM_n
#undef TOOLBAR_ITEM_y
#undef TOOLBAR_ITEM

	case HISTORY_BUTTON:
		toolitem = make_toolbar_item_history(true, true);
		break;

	case URL_BAR_ITEM:
		toolitem = make_toolbar_item_url_bar(false, true);
		break;

	case THROBBER_ITEM:
		toolitem = make_toolbar_item_throbber(true, true);
		break;

	case WEBSEARCH_ITEM:
		toolitem = make_toolbar_item_websearch(false, true);
		break;

	default:
		break;

	}
	return toolitem;
}


/**
 * target entry for drag source
 */
static GtkTargetEntry target_entry = {
	 (char *)"nsgtk_button_data",
	 GTK_TARGET_SAME_APP,
	 0
};


/**
 * find the toolbar item with a given location.
 *
 * \param tb the toolbar instance
 * \param locaction the location to search for
 * \return the item id for a location
 */
static nsgtk_toolbar_button
itemid_from_location(struct nsgtk_toolbar *tb, int location)
{
	int iidx;
	for (iidx = BACK_BUTTON; iidx < PLACEHOLDER_BUTTON; iidx++) {
		if (tb->items[iidx].location == location) {
			break;
		}
	}
	return iidx;
}


/**
 * save toolbar settings to file
 */
static nserror
nsgtk_toolbar_customisation_save(struct nsgtk_toolbar *tb)
{
	int iidx; /* item index */
	char *order; /* item ordering */
	char *start; /* start of next item name to be output */
	int orderlen = 0; /* length of item ordering */
	nsgtk_toolbar_button itemid;
	int location;
	char *choices = NULL;

	for (iidx = BACK_BUTTON; iidx < PLACEHOLDER_BUTTON; iidx++) {
		if (tb->items[iidx].location != INACTIVE_LOCATION) {
			orderlen += strlen(tb->items[iidx].name);
			orderlen++; /* allow for separator */
		}
	}

	/* ensure there are some items to store */
	if (orderlen == 0) {
		return NSERROR_INVALID;
	}

	order = malloc(orderlen);
	if (order == NULL) {
		return NSERROR_NOMEM;
	}

	start = order;

	for (location = BACK_BUTTON;
	     location < PLACEHOLDER_BUTTON;
	     location++) {
		int written;
		itemid = itemid_from_location(tb, location);
		if (itemid == PLACEHOLDER_BUTTON) {
			/* no more filled locations */
			break;
		}
		written = snprintf(start,
				orderlen - (start - order),
				"%s/",
				tb->items[itemid].name);
		if ((written < 0) ||
		    (written >= orderlen - (start - order))) {
			free(order);
			return NSERROR_UNKNOWN;
		}
		start += written;

		if ((start - order) >= orderlen) {
			break;
		}
	}

	order[orderlen - 1] = 0;

	nsoption_set_charp(toolbar_items, order);

	/* ensure choices are saved */
	netsurf_mkpath(&choices, NULL, 2, nsgtk_config_home, "Choices");
	if (choices != NULL) {
		nsoption_write(choices, NULL, NULL);
		free(choices);
	}

	return NSERROR_OK;
}


/**
 * connect signals to a toolbar item in a customisation toolbar
 *
 * \param tb The toolbar
 * \param itemid The item id within to toolbar to connect
 * \param NSERROR_OK on success
 */
static nserror
toolbar_item_connect_signals(struct nsgtk_toolbar *tb, int itemid)
{
	/* set toolbar items to be a drag source */
	gtk_tool_item_set_use_drag_window(tb->items[itemid].button, TRUE);
	gtk_drag_source_set(GTK_WIDGET(tb->items[itemid].button),
			    GDK_BUTTON1_MASK,
			    &target_entry,
			    1,
			    GDK_ACTION_COPY);
	g_signal_connect(tb->items[itemid].button,
			 "drag-data-get",
			 G_CALLBACK(tb->items[itemid].dataminus),
			 tb);
	return NSERROR_OK;
}


/**
 * customisation container handler for drag drop signal
 *
 * called when a widget is dropped onto the store window
 */
static gboolean
customisation_container_drag_drop_cb(GtkWidget *widget,
				     GdkDragContext *gdc,
				     gint x, gint y,
				     guint time,
				     gpointer data)
{
	struct nsgtk_toolbar_customisation *tbc;
	tbc = (struct nsgtk_toolbar_customisation *)data;
	int location;
	int itemid;

	if ((tbc->dragfrom) || (tbc->dragitem == -1)) {
		tbc->dragitem = -1;
		return FALSE;
	}

	if (tbc->toolbar.items[tbc->dragitem].location == INACTIVE_LOCATION) {
		tbc->dragitem = -1;
		gtk_drag_finish(gdc, TRUE, TRUE, time);
		return FALSE;

	}

	/* update the locations for all the subsequent toolbar items */
	for (location = tbc->toolbar.items[tbc->dragitem].location;
	     location < PLACEHOLDER_BUTTON;
	     location++) {
		itemid = itemid_from_location(&tbc->toolbar, location);
		if (itemid == PLACEHOLDER_BUTTON) {
			break;
		}
		tbc->toolbar.items[itemid].location--;
	}

	/* remove existing item */
	tbc->toolbar.items[tbc->dragitem].location = -1;
	gtk_container_remove(GTK_CONTAINER(tbc->toolbar.widget),
			     GTK_WIDGET(tbc->toolbar.items[tbc->dragitem].button));

	tbc->dragitem = -1;
	gtk_drag_finish(gdc, TRUE, TRUE, time);
	return FALSE;
}


/**
 * customisation container handler for drag motion signal
 *
 * called when hovering above the store
 */
static gboolean
customisation_container_drag_motion_cb(GtkWidget *widget,
				       GdkDragContext *gdc,
				       gint x, gint y,
				       guint time,
				       gpointer data)
{
	return FALSE;
}


/**
 * customisation toolbar handler for drag drop signal
 *
 * called when a widget is dropped onto the toolbar
 */
static gboolean
customisation_toolbar_drag_drop_cb(GtkWidget *widget,
				   GdkDragContext *gdc,
				   gint x,
				   gint y,
				   guint time,
				   gpointer data)
{
	struct nsgtk_toolbar_customisation *tbc;
	tbc = (struct nsgtk_toolbar_customisation *)data;
	gint position; /* drop position in toolbar */
	int location;
	int itemid;
	struct nsgtk_toolbar_item *dragitem; /* toolbar item being dragged */

	position = gtk_toolbar_get_drop_index(tbc->toolbar.widget, x, y);
	if (tbc->dragitem == -1) {
		return TRUE;
	}

	/* pure conveiance variable */
	dragitem = &tbc->toolbar.items[tbc->dragitem];

	/* deal with replacing existing item in toolbar */
	if (dragitem->location != INACTIVE_LOCATION) {
		if (dragitem->location < position) {
			position--;
		}

		/* update the locations for all the subsequent toolbar items */
		for (location = dragitem->location;
		     location < PLACEHOLDER_BUTTON;
		     location++) {
			itemid = itemid_from_location(&tbc->toolbar, location);
			if (itemid == PLACEHOLDER_BUTTON) {
				break;
			}
			tbc->toolbar.items[itemid].location--;
		}

		/* remove existing item */
		dragitem->location = INACTIVE_LOCATION;
		gtk_container_remove(GTK_CONTAINER(tbc->toolbar.widget),
				     GTK_WIDGET(dragitem->button));
	}


	dragitem->button = make_toolbox_item(tbc->dragitem, true);

	if (dragitem->button == NULL) {
		nsgtk_warning("NoMemory", 0);
		return TRUE;
	}

	/* update locations */
	for (location = PLACEHOLDER_BUTTON; location >= position; location--) {
		itemid = itemid_from_location(&tbc->toolbar, location);
		if (itemid != PLACEHOLDER_BUTTON) {
			tbc->toolbar.items[itemid].location++;
		}
	}
	dragitem->location = position;

	gtk_toolbar_insert(tbc->toolbar.widget,
			   dragitem->button,
			   dragitem->location);

	toolbar_item_connect_signals(&tbc->toolbar, tbc->dragitem);
	gtk_widget_show_all(GTK_WIDGET(dragitem->button));
	tbc->dragitem = -1;
	return TRUE;
}


/**
 * customisation toolbar handler for drag data received signal
 *
 * connected to toolbutton drop; perhaps one day it'll work properly
 * so it may replace the global current_button
 */
static gboolean
customisation_toolbar_drag_data_received_cb(GtkWidget *widget,
			    GdkDragContext *gdc,
			    gint x,
			    gint y,
			    GtkSelectionData *selection,
			    guint info,
			    guint time,
			    gpointer data)
{
	return FALSE;
}


/**
 * customisation toolbar handler for drag motion signal
 *
 * called when hovering an item above the toolbar
 */
static gboolean
customisation_toolbar_drag_motion_cb(GtkWidget *widget,
				     GdkDragContext *gdc,
				     gint x,
				     gint y,
				     guint time,
				     gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	GtkToolItem *item;
	gint position; /* position in toolbar */

	item = gtk_tool_button_new(NULL, NULL);
	position = gtk_toolbar_get_drop_index(tb->widget, x, y);

	gtk_toolbar_set_drop_highlight_item(tb->widget, item, position);

	return FALSE; /* drag not in drop zone */
}


/**
 * customisation toolbar handler for drag leave signal
 *
 * called when hovering stops
 */
static void
customisation_toolbar_drag_leave_cb(GtkWidget *widget,
				    GdkDragContext *gdc,
				    guint time,
				    gpointer data)
{
	gtk_toolbar_set_drop_highlight_item(GTK_TOOLBAR(widget), NULL, 0);
}


/**
 * create a new browser window
 *
 * creates a browser window with default url depending on user choices.
 *
 * \param bw The browser window to pass for existing window/
 * \param intab true if the new window should be in a tab else false
 *                for new window.
 * \return NSERROR_OK on success else error code.
 */
static nserror
nsgtk_browser_window_create(struct browser_window *bw, bool intab)
{
	nserror res = NSERROR_OK;
	nsurl *url = NULL;
	int flags = BW_CREATE_HISTORY | BW_CREATE_FOREGROUND | BW_CREATE_FOCUS_LOCATION;

	if (intab) {
		flags |= BW_CREATE_TAB;
	}

	if (!nsoption_bool(new_blank)) {
		const char *addr;
		if (nsoption_charp(homepage_url) != NULL) {
			addr = nsoption_charp(homepage_url);
		} else {
			addr = NETSURF_HOMEPAGE;
		}
		res = nsurl_create(addr, &url);
	}

	if (res == NSERROR_OK) {
		res = browser_window_create(flags, url, NULL, bw, NULL);
	}

	if (url != NULL) {
		nsurl_unref(url);
	}

	return res;
}


/**
 * Apply the user toolbar button settings from configuration
 *
 * GTK specific user option string is a set of fields arranged as
 * [itemreference];[itemlocation]|[itemreference];[itemlocation]| etc
 *
 * \param tb The toolbar to apply customisation to
 * \param NSERROR_OK on success else error code.
 */
static nserror
apply_user_button_customisation(struct nsgtk_toolbar *tb)
{
	const char *tbitems; /* item order user config */
	const char *start;
	const char *end;
	int iidx; /* item index */
	int location = 0; /* location index */

	/* set all button locations to inactive */
	for (iidx = BACK_BUTTON; iidx < PLACEHOLDER_BUTTON; iidx++) {
		tb->items[iidx].location = INACTIVE_LOCATION;
	}

	tbitems = nsoption_charp(toolbar_items);
	if (tbitems == NULL) {
		tbitems = "";
	}

	end = tbitems;
	while (*end != 0) {
		start = end;
		while ((*end != 0) && (*end !='/')) {
			end++;
		}

		for (iidx = BACK_BUTTON; iidx < PLACEHOLDER_BUTTON; iidx++) {
			if (((ssize_t)strlen(tb->items[iidx].name) == (end - start)) &&
			    (strncmp(tb->items[iidx].name, start, end - start) == 0)) {
				tb->items[iidx].location = location++;
				break;
			}
		}

		if (*end == '/') {
			end++;
		}
	}

	if (location == 0) {
		/* completely failed to create any buttons so use defaults */
		tb->items[BACK_BUTTON].location = location++;
		tb->items[HISTORY_BUTTON].location = location++;
		tb->items[FORWARD_BUTTON].location = location++;
		tb->items[RELOADSTOP_BUTTON].location = location++;
		tb->items[URL_BAR_ITEM].location = location++;
		tb->items[WEBSEARCH_ITEM].location = location++;
		tb->items[OPENMENU_BUTTON].location = location++;
		tb->items[THROBBER_ITEM].location = location++;
	}


	return NSERROR_OK;
}


/**
 * callback function to remove a widget from a container
 */
static void container_remove_widget(GtkWidget *widget, gpointer data)
{
	GtkContainer *container = GTK_CONTAINER(data);
	gtk_container_remove(container, widget);
}


/**
 * populates a toolbar with widgets in correct order
 *
 * \param tb toolbar
 * \return NSERROR_OK on success else error code.
 */
static nserror populate_gtk_toolbar_widget(struct nsgtk_toolbar *tb)
{
	int location; /* location index */
	int itemid;

	/* clear the toolbar container of all widgets */
	gtk_container_foreach(GTK_CONTAINER(tb->widget),
			      container_remove_widget,
			      tb->widget);

	/* add widgets to toolbar */
	for (location = 0; location < PLACEHOLDER_BUTTON; location++) {
		itemid = itemid_from_location(tb, location);
		if (itemid == PLACEHOLDER_BUTTON) {
			break;
		}
		tb->items[itemid].button =
			make_toolbar_item(itemid,
					  tb->items[itemid].sensitivity);

		gtk_toolbar_insert(tb->widget,
				   tb->items[itemid].button,
				   location);
	}

	gtk_widget_show_all(GTK_WIDGET(tb->widget));

	return NSERROR_OK;
}


/**
 * populates the customization toolbar with widgets in correct order
 *
 * \param tb toolbar
 * \return NSERROR_OK on success else error code.
 */
static nserror customisation_toolbar_populate(struct nsgtk_toolbar *tb)
{
	int location; /* location index */
	int itemid;

	/* clear the toolbar container of all widgets */
	gtk_container_foreach(GTK_CONTAINER(tb->widget),
			      container_remove_widget,
			      tb->widget);

	/* add widgets to toolbar */
	for (location = 0; location < PLACEHOLDER_BUTTON; location++) {
		itemid = itemid_from_location(tb, location);
		if (itemid == PLACEHOLDER_BUTTON) {
			break;
		}
		tb->items[itemid].button = make_toolbox_item(itemid, true);

		gtk_toolbar_insert(tb->widget,
				   tb->items[itemid].button,
				   location);
	}

	gtk_widget_show_all(GTK_WIDGET(tb->widget));

	return NSERROR_OK;
}


/**
 * find the toolbar item with a given gtk widget.
 *
 * \param tb the toolbar instance
 * \param toolitem the tool item widget to search for
 * \return the item id matching the widget
 */
static nsgtk_toolbar_button
itemid_from_gtktoolitem(struct nsgtk_toolbar *tb, GtkToolItem *toolitem)
{
	int iidx;
	for (iidx = BACK_BUTTON; iidx < PLACEHOLDER_BUTTON; iidx++) {
		if ((tb->items[iidx].location != INACTIVE_LOCATION) &&
		    (tb->items[iidx].button == toolitem)) {
			break;
		}
	}
	return iidx;
}


/**
 * set a toolbar items sensitivity
 *
 * note this does not set menu items sensitivity
 */
static nserror
set_item_sensitivity(struct nsgtk_toolbar_item *item, bool sensitivity)
{
	if (item->sensitivity != sensitivity) {
		/* item requires sensitivity changing */
		item->sensitivity = sensitivity;

		if ((item->location != -1) && (item->button != NULL)) {
			gtk_widget_set_sensitive(GTK_WIDGET(item->button),
						 item->sensitivity);
		}
	}

	return NSERROR_OK;
}


/**
 * set an item to its alternative action
 *
 * this is currently only used for the stop/reload button where we
 *   also reuse the item sensitivity for the state indicator.
 *
 * \param tb the toolbar instance
 */
static nserror set_item_action(struct nsgtk_toolbar *tb, int itemid, bool alt)
{
	const char *iconname;
	char *label = NULL;

	if (itemid != RELOADSTOP_BUTTON) {
		return NSERROR_INVALID;
	}
	if (tb->items[itemid].location == -1) {
		return NSERROR_OK;
	}
	tb->items[itemid].sensitivity = alt;

	if (tb->items[itemid].button == NULL) {
		return NSERROR_INVALID;
	}

	if (tb->items[itemid].sensitivity) {
		iconname = NSGTK_STOCK_REFRESH;
		label = remove_underscores(messages_get("Reload"), false);

	} else {
		iconname = NSGTK_STOCK_STOP;
		label = remove_underscores(messages_get("gtkStop"), false);

	}
	gtk_tool_button_set_label(GTK_TOOL_BUTTON(tb->items[itemid].button),
				  label);

	gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(tb->items[itemid].button),
				      iconname);

	gtk_widget_set_sensitive(GTK_WIDGET(tb->items[itemid].button), TRUE);

	if (label != NULL) {
		free(label);
	}

	return NSERROR_OK;
}


/**
 * cause the toolbar browsing context to navigate to a new url.
 *
 * \param tb the toolbar context.
 * \param urltxt The url string.
 * \return NSERROR_OK on success else appropriate error code.
 */
static nserror
toolbar_navigate_to_url(struct nsgtk_toolbar *tb, const char *urltxt)
{
	struct browser_window *bw;
	nsurl *url;
	nserror res;

	res = nsurl_create(urltxt, &url);
	if (res != NSERROR_OK) {
		return res;
	}

	bw = tb->get_bw(tb->get_ctx);

	res = browser_window_navigate(bw,
				      url,
				      NULL,
				      BW_NAVIGATE_HISTORY,
				      NULL,
				      NULL,
				      NULL);
	nsurl_unref(url);

	return res;
}


/**
 * run a gtk file chooser as a save dialog to obtain a path
 */
static nserror
nsgtk_saveas_dialog(struct browser_window *bw,
		    const char *title,
		    GtkWindow *parent,
		    bool folder,
		    gchar **path_out)
{
	nserror res;
	GtkWidget *fc; /* file chooser widget */
	GtkFileChooserAction action;
	char *path; /* proposed path */

	if (!browser_window_has_content(bw)) {
		/* cannot save a page with no content */
		return NSERROR_INVALID;
	}

	if (folder) {
		action = GTK_FILE_CHOOSER_ACTION_CREATE_FOLDER;
	} else {
		action = GTK_FILE_CHOOSER_ACTION_SAVE;
	}

	fc = gtk_file_chooser_dialog_new(title,
					 parent,
					 action,
					 NSGTK_STOCK_CANCEL,
					 GTK_RESPONSE_CANCEL,
					 NSGTK_STOCK_SAVE,
					 GTK_RESPONSE_ACCEPT,
					 NULL);

	/* set a default file name */
	res = nsurl_nice(browser_window_access_url(bw), &path, false);
	if (res != NSERROR_OK) {
		path = strdup(messages_get("SaveText"));
		if (path == NULL) {
			gtk_widget_destroy(fc);
			return NSERROR_NOMEM;
		}
	}

	if ((!folder) || (access(path, F_OK) != 0)) {
		gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(fc), path);
	}
	free(path);

	/* confirm overwriting */
	gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(fc), TRUE);

	/* run the dialog to let user select path */
	if (gtk_dialog_run(GTK_DIALOG(fc)) != GTK_RESPONSE_ACCEPT) {
		gtk_widget_destroy(fc);
		return NSERROR_NOT_FOUND;
	}

	*path_out = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));

	gtk_widget_destroy(fc);

	return NSERROR_OK;
}


/**
 * connect all signals to widgets in a customisation
 */
static nserror
toolbar_customisation_connect_signals(struct nsgtk_toolbar *tb)
{
	int iidx;

	for (iidx = BACK_BUTTON; iidx < PLACEHOLDER_BUTTON; iidx++) {
		/* skip inactive items in toolbar */
		if (tb->items[iidx].location != INACTIVE_LOCATION) {
			toolbar_item_connect_signals(tb, iidx);
		}
	}

	/* add move button listeners */
	g_signal_connect(tb->widget,
			"drag-drop",
			 G_CALLBACK(customisation_toolbar_drag_drop_cb),
			 tb);
	g_signal_connect(tb->widget,
			"drag-data-received",
			 G_CALLBACK(customisation_toolbar_drag_data_received_cb),
			 tb);
	g_signal_connect(tb->widget,
			"drag-motion",
			 G_CALLBACK(customisation_toolbar_drag_motion_cb),
			 tb);
	g_signal_connect(tb->widget,
			"drag-leave",
			 G_CALLBACK(customisation_toolbar_drag_leave_cb),
			 tb);

	/* set data types */
	gtk_drag_dest_set(GTK_WIDGET(tb->widget),
			  GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_DROP,
			  &target_entry,
			  1,
			  GDK_ACTION_COPY);

	return NSERROR_OK;
}


static void
item_size_allocate_cb(GtkWidget *widget,
		      GdkRectangle *alloc,
		      gpointer user_data)
{
	if (alloc->width > NSGTK_BUTTON_WIDTH) {
		alloc->width = NSGTK_BUTTON_WIDTH;
	}
	if (alloc->height > NSGTK_BUTTON_HEIGHT) {
		alloc->height = NSGTK_BUTTON_HEIGHT;
	}
	gtk_widget_set_allocation(widget, alloc);
}


/**
 * add a row to a toolbar customisation toolbox
 *
 * \param tbc The toolbar customisation context
 * \param startitem The item index of the beginning of the row
 * \param enditem The item index of the beginning of the next row
 * \return NSERROR_OK on successs else error
 */
static nserror
add_toolbox_row(struct nsgtk_toolbar_customisation *tbc,
		int startitem,
		int enditem)
{
	GtkToolbar *rowbar;
	int iidx;

	rowbar = GTK_TOOLBAR(gtk_toolbar_new());
	if (rowbar == NULL) {
		return NSERROR_NOMEM;
	}

	gtk_toolbar_set_style(rowbar, GTK_TOOLBAR_BOTH);
	gtk_toolbar_set_icon_size(rowbar, GTK_ICON_SIZE_LARGE_TOOLBAR);
	gtk_box_pack_start(tbc->toolbox, GTK_WIDGET(rowbar), FALSE, FALSE, 0);

	for (iidx = startitem; iidx < enditem; iidx++) {
		if (tbc->items[iidx] == NULL) {
			/* skip any widgets that failed to initialise */
			continue;
		}
		gtk_widget_set_size_request(GTK_WIDGET(tbc->items[iidx]),
					    NSGTK_BUTTON_WIDTH,
					    NSGTK_BUTTON_HEIGHT);
		gtk_tool_item_set_use_drag_window(tbc->items[iidx], TRUE);
		gtk_drag_source_set(GTK_WIDGET(tbc->items[iidx]),
				    GDK_BUTTON1_MASK,
				    &target_entry,
				    1,
				    GDK_ACTION_COPY);
		g_signal_connect(tbc->items[iidx],
				 "drag-data-get",
				 G_CALLBACK(tbc->toolbar.items[iidx].dataplus),
				 &tbc->toolbar);
		g_signal_connect(tbc->items[iidx],
				 "size-allocate",
				 G_CALLBACK(item_size_allocate_cb),
				 NULL);
		gtk_toolbar_insert(rowbar, tbc->items[iidx], -1);
	}
	return NSERROR_OK;
}


/**
 * creates widgets in customisation toolbox
 *
 * \param tbc The toolbar customisation context
 * \param width The width to layout the toolbox to
 * \return NSERROR_OK on success else error code.
 */
static nserror
toolbar_customisation_create_toolbox(struct nsgtk_toolbar_customisation *tbc,
				     int width)
{
	int columns; /* number of items in a single row */
	int curcol; /* current column in creation */
	int iidx; /* item index */
	int startidx; /* index of item at start of row */

	/* ensure there are a minimum number of items per row */
	columns = width / NSGTK_BUTTON_WIDTH;
	if (columns < NSGTK_MIN_STORE_COLUMNS) {
		columns = NSGTK_MIN_STORE_COLUMNS;
	}

	curcol = 0;
	for (iidx = startidx = BACK_BUTTON; iidx < PLACEHOLDER_BUTTON; iidx++) {
		if (curcol >= columns) {
			add_toolbox_row(tbc, startidx, iidx);
			curcol = 0;
			startidx = iidx;
		}
		tbc->items[iidx] = make_toolbox_item(iidx, false);
		if (tbc->items[iidx] != NULL) {
			curcol++;
		}
	}
	if (curcol > 0) {
		add_toolbox_row(tbc, startidx, iidx);
	}

	return NSERROR_OK;
}


/**
 * update toolbar in customisation to user settings
 */
static nserror
customisation_toolbar_update(struct nsgtk_toolbar_customisation *tbc)
{
	nserror res;

	res = apply_user_button_customisation(&tbc->toolbar);
	if (res != NSERROR_OK) {
		return res;
	}

	/* populate toolbar widget */
	res = customisation_toolbar_populate(&tbc->toolbar);
	if (res != NSERROR_OK) {
		return res;
	}

	/* ensure icon sizes and text labels on toolbar are set */
	res = nsgtk_toolbar_restyle(&tbc->toolbar);
	if (res != NSERROR_OK) {
		return res;
	}

	/* attach handlers to toolbar widgets */
	res = toolbar_customisation_connect_signals(&tbc->toolbar);
	if (res != NSERROR_OK) {
		return res;
	}

	return NSERROR_OK;
}


/**
 * customisation apply handler for clicked signal
 *
 * when 'save settings' button is clicked
 */
static gboolean
customisation_apply_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar_customisation *tbc;
	tbc = (struct nsgtk_toolbar_customisation *)data;

	/* save state to file, update toolbars for all windows */
	nsgtk_toolbar_customisation_save(&tbc->toolbar);
	nsgtk_window_toolbar_update();
	gtk_widget_destroy(tbc->container);

	return TRUE;
}


/**
 * customisation reset handler for clicked signal
 *
 * when 'reload defaults' button is clicked
 */
static gboolean
customisation_reset_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar_customisation *tbc;
	tbc = (struct nsgtk_toolbar_customisation *)data;

	customisation_toolbar_update(tbc);

	return TRUE;
}


/**
 * customisation container destroy handler
 */
static void customisation_container_destroy_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar_customisation *tbc;
	tbc = (struct nsgtk_toolbar_customisation *)data;

	free(tbc);
}

/*
 * Toolbar button clicked handlers
 */

/**
 * create a toolbar customisation tab
 *
 * this is completely different approach to previous implementation. it
 *  is not modal and the toolbar configuration is performed completely
 *  within the tab. once the user is happy they can apply the change or
 *  cancel as they see fit while continuing to use the browser as usual.
 */
static gboolean cutomize_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar_customisation *tbc;
	nserror res;
	GtkBuilder *builder;
	GtkNotebook *notebook; /* notebook containing widget */
	GtkAllocation notebook_alloc; /* notebook size allocation */
	int iidx; /* item index */

	/* obtain the notebook being added to */
	notebook = GTK_NOTEBOOK(gtk_widget_get_ancestor(widget,
							GTK_TYPE_NOTEBOOK));
	if (notebook == NULL) {
		return TRUE;
	}

	/* create builder */
	res = nsgtk_builder_new_from_resname("toolbar", &builder);
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Toolbar UI builder init failed");
		return TRUE;
	}
	gtk_builder_connect_signals(builder, NULL);

	/* create nsgtk_toolbar_customisation which has nsgtk_toolbar
	 * at the front so we can reuse functions that take
	 * nsgtk_toolbar
	 */
	tbc = calloc(1, sizeof(struct nsgtk_toolbar_customisation));
	if (tbc == NULL) {
		g_object_unref(builder);
		return TRUE;
	}

	/* get container box widget which forms a page of the tabs */
	tbc->container = GTK_WIDGET(gtk_builder_get_object(builder, "customisation"));
	if (tbc->container == NULL) {
		goto cutomize_button_clicked_cb_error;
	}

	/* vertical box for the toolbox to drag items into and out of */
	tbc->toolbox = GTK_BOX(gtk_builder_get_object(builder, "toolbox"));
	if (tbc->toolbox == NULL) {
		goto cutomize_button_clicked_cb_error;
	}

	/* customisation toolbar container */
	tbc->toolbar.widget = GTK_TOOLBAR(gtk_builder_get_object(builder, "toolbar"));
	if (tbc->toolbar.widget == NULL) {
		goto cutomize_button_clicked_cb_error;
	}

	/* build customisation toolbar */
	gtk_toolbar_set_show_arrow(tbc->toolbar.widget, TRUE);

	for (iidx = BACK_BUTTON; iidx < PLACEHOLDER_BUTTON; iidx++) {
		res = toolbar_item_create(iidx, &tbc->toolbar.items[iidx]);
		if (res != NSERROR_OK) {
			goto cutomize_button_clicked_cb_error;
		}
	}

	res = customisation_toolbar_update(tbc);
	if (res != NSERROR_OK) {
		goto cutomize_button_clicked_cb_error;
	}

	/* use toolbox for widgets to drag to/from */
	gtk_widget_get_allocation(GTK_WIDGET(notebook), &notebook_alloc);

	res = toolbar_customisation_create_toolbox(tbc, notebook_alloc.width);
	if (res != NSERROR_OK) {
		goto cutomize_button_clicked_cb_error;
	}

	/* configure the container */
	gtk_drag_dest_set(GTK_WIDGET(tbc->container),
			  GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_DROP,
			  &target_entry,
			  1,
			  GDK_ACTION_COPY);

	/* discard button calls destroy */
	g_signal_connect_swapped(GTK_WIDGET(gtk_builder_get_object(builder,
								   "discard")),
				 "clicked",
				 G_CALLBACK(gtk_widget_destroy),
				 tbc->container);

	/* save and update on apply button  */
	g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "apply")),
			 "clicked",
			 G_CALLBACK(customisation_apply_clicked_cb),
			 tbc);

	g_signal_connect(GTK_WIDGET(gtk_builder_get_object(builder, "reset")),
			 "clicked",
			 G_CALLBACK(customisation_reset_clicked_cb),
			 tbc);

	/* close and cleanup on delete signal */
	g_signal_connect(tbc->container,
			 "destroy",
			 G_CALLBACK(customisation_container_destroy_cb),
			 tbc);


	g_signal_connect(tbc->container,
			 "drag-drop",
			 G_CALLBACK(customisation_container_drag_drop_cb),
			 tbc);

	g_signal_connect(tbc->container,
			 "drag-motion",
			 G_CALLBACK(customisation_container_drag_motion_cb),
			 tbc);


	nsgtk_tab_add_page(notebook,
			   tbc->container,
			   false,
			   messages_get("gtkCustomizeToolbarTitle"),
			   favicon_pixbuf);


	/* safe to drop the reference to the builder as the container is
	 * referenced by the notebook now.
	 */
	g_object_unref(builder);

	return TRUE;

 cutomize_button_clicked_cb_error:
	free(tbc);
	g_object_unref(builder);
	return TRUE;

}


/**
 * callback for all toolbar items widget size allocation
 *
 * handler connected to all toolbar items for the size-allocate signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param alloc The size allocation being set.
 * \param data The toolbar context passed when the signal was connected
 */
static void
toolbar_item_size_allocate_cb(GtkWidget *widget,
			      GtkAllocation *alloc,
			      gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	nsgtk_toolbar_button itemid;

	itemid = itemid_from_gtktoolitem(tb, GTK_TOOL_ITEM(widget));

	if ((tb->toolbarmem == alloc->x) ||
	    (tb->items[itemid].location < tb->items[HISTORY_BUTTON].location)) {
		/*
		 * no reallocation after first adjustment,
		 * no reallocation for buttons left of history button
		 */
		return;
	}

	if (itemid == HISTORY_BUTTON) {
		if (alloc->width == 20) {
			return;
		}

		tb->toolbarbase = alloc->y + alloc->height;
		tb->historybase = alloc->x + 20;
		if (tb->offset == 0) {
			tb->offset = alloc->width - 20;
		}
		alloc->width = 20;
	} else if (tb->items[itemid].location <= tb->items[URL_BAR_ITEM].location) {
		alloc->x -= tb->offset;
		if (itemid == URL_BAR_ITEM) {
			alloc->width += tb->offset;
		}
	}
	tb->toolbarmem = alloc->x;

	gtk_widget_size_allocate(widget, alloc);
}


/**
 * handler for back tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
back_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;

	bw = tb->get_bw(tb->get_ctx);

	if ((bw != NULL) && browser_window_history_back_available(bw)) {
		/* clear potential search effects */
		browser_window_search_clear(bw);

		browser_window_history_back(bw, false);

		set_item_sensitivity(&tb->items[BACK_BUTTON],
				browser_window_history_back_available(bw));
		set_item_sensitivity(&tb->items[FORWARD_BUTTON],
				browser_window_history_forward_available(bw));

		nsgtk_local_history_hide();
	}
	return TRUE;
}


/**
 * handler for forward tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
forward_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;

	bw = tb->get_bw(tb->get_ctx);

	if ((bw != NULL) && browser_window_history_forward_available(bw)) {
		/* clear potential search effects */
		browser_window_search_clear(bw);

		browser_window_history_forward(bw, false);

		set_item_sensitivity(&tb->items[BACK_BUTTON],
				browser_window_history_back_available(bw));
		set_item_sensitivity(&tb->items[FORWARD_BUTTON],
				browser_window_history_forward_available(bw));
		nsgtk_local_history_hide();
	}
	return TRUE;
}


/**
 * handler for stop tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
stop_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;

	browser_window_stop(tb->get_bw(tb->get_ctx));

	return TRUE;
}


/**
 * handler for reload tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
reload_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;

	bw = tb->get_bw(tb->get_ctx);

	/* clear potential search effects */
	browser_window_search_clear(bw);

	browser_window_reload(bw, true);

	return TRUE;
}


/**
 * handler for reload/stop tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
reloadstop_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;

	bw = tb->get_bw(tb->get_ctx);

	/* clear potential search effects */
	browser_window_search_clear(bw);

	if (tb->items[RELOADSTOP_BUTTON].sensitivity) {
		browser_window_reload(bw, true);
	} else {
		browser_window_stop(tb->get_bw(tb->get_ctx));
	}

	return TRUE;
}


/**
 * handler for home tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
home_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	nserror res;
	const char *addr;

	if (nsoption_charp(homepage_url) != NULL) {
		addr = nsoption_charp(homepage_url);
	} else {
		addr = NETSURF_HOMEPAGE;
	}

	res = toolbar_navigate_to_url(tb, addr);
	if (res != NSERROR_OK) {
		nsgtk_warning(messages_get_errorcode(res), 0);
	}

	return TRUE;
}


/**
 * callback for url entry widget activation
 *
 * handler connected to url entry widget for the activate signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE to allow activation.
 */
static gboolean url_entry_activate_cb(GtkWidget *widget, gpointer data)
{
	nserror res;
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;
	nsurl *url;

	res = search_web_omni(gtk_entry_get_text(GTK_ENTRY(widget)),
			      SEARCH_WEB_OMNI_NONE,
			      &url);
	if (res == NSERROR_OK) {
		bw = tb->get_bw(tb->get_ctx);
		res = browser_window_navigate(
			bw, url, NULL, BW_NAVIGATE_HISTORY, NULL, NULL, NULL);
		nsurl_unref(url);
	}
	if (res != NSERROR_OK) {
		nsgtk_warning(messages_get_errorcode(res), 0);
	}

	return TRUE;
}


/**
 * callback for url entry widget changing
 *
 * handler connected to url entry widget for the change signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param event The key change event that changed the entry.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE to allow activation.
 */
static gboolean
url_entry_changed_cb(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
	return nsgtk_completion_update(GTK_ENTRY(widget));
}


/**
 * callback for url entry widget icon button release
 *
 * handler connected to url entry widget for the icon release signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param event The key change event that changed the entry.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE to allow activation.
 */
static void
url_entry_icon_release_cb(GtkEntry *entry,
			   GtkEntryIconPosition icon_pos,
			   GdkEvent *event,
			   gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;

	bw = tb->get_bw(tb->get_ctx);

	nsgtk_page_info(bw);
}


/**
 * handler for web search tool bar entry item activate signal
 *
 * handler connected to web search entry widget for the activate signal
 *
 * \todo make this user selectable to switch between opening in new
 *   and navigating current window. Possibly improve core search_web interfaces
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean websearch_entry_activate_cb(GtkWidget *widget, gpointer data)
{
	nserror res;
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;
	nsurl *url;

	res = search_web_omni(gtk_entry_get_text(GTK_ENTRY(widget)),
			      SEARCH_WEB_OMNI_SEARCHONLY,
			      &url);
	if (res == NSERROR_OK) {
		bw = tb->get_bw(tb->get_ctx);

		res = browser_window_create(
			BW_CREATE_HISTORY | BW_CREATE_TAB | BW_CREATE_FOREGROUND,
			url,
			NULL,
			bw,
			NULL);
		nsurl_unref(url);
	}
	if (res != NSERROR_OK) {
		nsgtk_warning(messages_get_errorcode(res), 0);
	}

	return TRUE;
}

/**
 * handler for web search tool bar item button press signal
 *
 * allows a click in the websearch entry field to clear the name of the
 * provider.
 *
 * \todo this does not work well, different behaviour wanted perhaps?
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
websearch_entry_button_press_cb(GtkWidget *widget,
			     GdkEventFocus *f,
			     gpointer data)
{
	gtk_editable_select_region(GTK_EDITABLE(widget), 0, -1);
	gtk_widget_grab_focus(GTK_WIDGET(widget));

	return TRUE;
}


/**
 * handler for new window tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
newwindow_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	nserror res;
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;

	res = nsgtk_browser_window_create(tb->get_bw(tb->get_ctx), false);
	if (res != NSERROR_OK) {
		nsgtk_warning(messages_get_errorcode(res), 0);
	}

	return TRUE;
}


/**
 * handler for new tab tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
newtab_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	nserror res;
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;

	res = nsgtk_browser_window_create(tb->get_bw(tb->get_ctx), true);
	if (res != NSERROR_OK) {
		nsgtk_warning(messages_get_errorcode(res), 0);
	}
	return TRUE;
}


/**
 * handler for open file tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
openfile_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	GtkWidget *dlgOpen;
	gint response;
	GtkWidget *toplevel;
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;

	toplevel = gtk_widget_get_ancestor(widget, GTK_TYPE_WINDOW);

	dlgOpen = gtk_file_chooser_dialog_new("Open File",
					      GTK_WINDOW(toplevel),
			GTK_FILE_CHOOSER_ACTION_OPEN,
			NSGTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
			NSGTK_STOCK_OPEN, GTK_RESPONSE_OK,
			NULL, NULL);

	response = gtk_dialog_run(GTK_DIALOG(dlgOpen));
	if (response == GTK_RESPONSE_OK) {
		size_t urltxt_size;
		char *urltxt;
		gchar *filename;
		nserror res;
		nsurl *url;

		filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlgOpen));

		urltxt_size = strlen(filename) + FILE_SCHEME_PREFIX_LEN + 1;
		urltxt = malloc(urltxt_size);
		if (urltxt != NULL) {
			snprintf(urltxt, urltxt_size, FILE_SCHEME_PREFIX"%s", filename);

			res = nsurl_create(urltxt, &url);
			if (res == NSERROR_OK) {
				bw = tb->get_bw(tb->get_ctx);
				res = browser_window_navigate(bw,
							url,
							NULL,
							BW_NAVIGATE_HISTORY,
							NULL,
							NULL,
							NULL);
				nsurl_unref(url);
			}
			if (res != NSERROR_OK) {
				nsgtk_warning(messages_get_errorcode(res), 0);
			}
			free(urltxt);
		}


		g_free(filename);
	}

	gtk_widget_destroy(dlgOpen);

	return TRUE;
}


/**
 * handler for close window tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
closewindow_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	GtkWidget *toplevel;
	toplevel = gtk_widget_get_ancestor(widget, GTK_TYPE_WINDOW);
	gtk_widget_destroy(toplevel);
	return TRUE;
}


/**
 * handler for full save export tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
savepage_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;
	DIR *d;
	gchar *path;
	nserror res;
	GtkWidget *toplevel;

	bw = tb->get_bw(tb->get_ctx);
	toplevel = gtk_widget_get_ancestor(widget, GTK_TYPE_WINDOW);

	res = nsgtk_saveas_dialog(bw,
				  messages_get("gtkcompleteSave"),
				  GTK_WINDOW(toplevel),
				  true,
				  &path);
	if (res != NSERROR_OK) {
		return FALSE;
	}

	d = opendir(path);
	if (d == NULL) {
		NSLOG(netsurf, INFO,
		      "Unable to open directory %s for complete save: %s",
		      path,
		      strerror(errno));
		if (errno == ENOTDIR) {
			nsgtk_warning("NoDirError", path);
		} else {
			nsgtk_warning("gtkFileError", path);
		}
		g_free(path);
		return TRUE;
	}
	closedir(d);

	save_complete(browser_window_get_content(bw), path, NULL);
	g_free(path);

	return TRUE;
}


/**
 * handler for pdf export tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
pdf_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;
	GtkWidget *toplevel;
	gchar *filename;
	nserror res;

	bw = tb->get_bw(tb->get_ctx);

	toplevel = gtk_widget_get_ancestor(widget, GTK_TYPE_WINDOW);

	res = nsgtk_saveas_dialog(bw,
				  "Export to PDF",
				  GTK_WINDOW(toplevel),
				  false,
				  &filename);
	if (res != NSERROR_OK) {
		return FALSE;
	}

#ifdef WITH_PDF_EXPORT
	struct print_settings *settings;

	/* this way the scale used by PDF functions is synchronised with that
	 * used by the all-purpose print interface
	 */
	haru_nsfont_set_scale((float)option_export_scale / 100);

	settings = print_make_settings(PRINT_OPTIONS,
				       (const char *) filename,
				       &haru_nsfont);
	g_free(filename);
	if (settings == NULL) {
		return TRUE;
	}
	/* This will clean up the print_settings object for us */
	print_basic_run(browser_window_get_content(bw),	&pdf_printer, settings);
#endif
	return TRUE;

}


/**
 * handler for plain text export tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
plaintext_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;
	GtkWidget *toplevel;
	gchar *filename;
	nserror res;

	bw = tb->get_bw(tb->get_ctx);

	toplevel = gtk_widget_get_ancestor(widget, GTK_TYPE_WINDOW);

	res = nsgtk_saveas_dialog(bw,
				  messages_get("gtkplainSave"),
				  GTK_WINDOW(toplevel),
				  false,
				  &filename);
	if (res != NSERROR_OK) {
		return FALSE;
	}


	save_as_text(browser_window_get_content(bw), filename);
	g_free(filename);

	return TRUE;
}


/**
 * handler for print tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
print_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;
	GtkPrintOperation *print_op;
	GtkPageSetup *page_setup;
	GtkPrintSettings *print_settings;
	GtkPrintOperationResult res = GTK_PRINT_OPERATION_RESULT_ERROR;
	struct print_settings *nssettings;
	char *settings_fname = NULL;
	GtkWidget *toplevel;

	bw = tb->get_bw(tb->get_ctx);

	toplevel = gtk_widget_get_ancestor(widget, GTK_TYPE_WINDOW);

	print_op = gtk_print_operation_new();
	if (print_op == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		return TRUE;
	}

	/* use previously saved settings if any */
	netsurf_mkpath(&settings_fname, NULL, 2, nsgtk_config_home, "Print");
	if (settings_fname != NULL) {
		print_settings = gtk_print_settings_new_from_file(settings_fname, NULL);
		if (print_settings != NULL) {
			gtk_print_operation_set_print_settings(print_op,
							       print_settings);

			/* We're not interested in the settings any more */
			g_object_unref(print_settings);
		}
	}

	content_to_print = browser_window_get_content(bw);

	page_setup = gtk_print_run_page_setup_dialog(GTK_WINDOW(toplevel),
						     NULL,
						     NULL);
	if (page_setup == NULL) {
		nsgtk_warning(messages_get("NoMemory"), 0);
		free(settings_fname);
		g_object_unref(print_op);
		return TRUE;
	}
	gtk_print_operation_set_default_page_setup(print_op, page_setup);

	nssettings = print_make_settings(PRINT_DEFAULT,
					 NULL,
					 nsgtk_layout_table);

	g_signal_connect(print_op,
			 "begin_print",
			 G_CALLBACK(gtk_print_signal_begin_print),
			 nssettings);
	g_signal_connect(print_op,
			 "draw_page",
			 G_CALLBACK(gtk_print_signal_draw_page),
			 NULL);
	g_signal_connect(print_op,
			 "end_print",
			 G_CALLBACK(gtk_print_signal_end_print),
			 nssettings);

	if (content_get_type(browser_window_get_content(bw)) != CONTENT_TEXTPLAIN) {
		res = gtk_print_operation_run(print_op,
					      GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
					      GTK_WINDOW(toplevel),
					      NULL);
	}

	/* if the settings were used save them for future use */
	if (settings_fname != NULL) {
		if (res == GTK_PRINT_OPERATION_RESULT_APPLY) {
			/* Do not increment the settings reference */
			print_settings = gtk_print_operation_get_print_settings(print_op);

			gtk_print_settings_to_file(print_settings,
						   settings_fname,
						   NULL);
		}
		free(settings_fname);
	}

	/* Our print_settings object is destroyed by the end print handler */
	g_object_unref(page_setup);
	g_object_unref(print_op);

	return TRUE;
}

/**
 * handler for quit tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
quit_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	nsgtk_scaffolding_destroy_all();
	return TRUE;
}


/**
 * handler for cut tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
cut_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;
	GtkWidget *focused;
	GtkWidget *toplevel;

	toplevel = gtk_widget_get_ancestor(widget, GTK_TYPE_WINDOW);

	focused = gtk_window_get_focus(GTK_WINDOW(toplevel));

	/* let gtk handle it if focused widget is an editable */
	if (GTK_IS_EDITABLE(focused)) {
		gtk_editable_cut_clipboard(GTK_EDITABLE(focused));
	} else {
		bw = tb->get_bw(tb->get_ctx);
		browser_window_key_press(bw, NS_KEY_CUT_SELECTION);
	}

	return TRUE;
}


/**
 * handler for copy tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
copy_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;
	GtkWidget *focused;
	GtkWidget *toplevel;

	toplevel = gtk_widget_get_ancestor(widget, GTK_TYPE_WINDOW);

	focused = gtk_window_get_focus(GTK_WINDOW(toplevel));

	/* let gtk handle it if focused widget is an editable */
	if (GTK_IS_EDITABLE(focused)) {
		gtk_editable_copy_clipboard(GTK_EDITABLE(focused));
	} else {
		bw = tb->get_bw(tb->get_ctx);
		browser_window_key_press(bw, NS_KEY_COPY_SELECTION);
	}

	return TRUE;
}


/**
 * handler for paste tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
paste_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;
	GtkWidget *focused;
	GtkWidget *toplevel;

	toplevel = gtk_widget_get_ancestor(widget, GTK_TYPE_WINDOW);

	focused = gtk_window_get_focus(GTK_WINDOW(toplevel));

	/* let gtk handle it if focused widget is an editable */
	if (GTK_IS_EDITABLE(focused)) {
		gtk_editable_paste_clipboard(GTK_EDITABLE(focused));
	} else {
		bw = tb->get_bw(tb->get_ctx);
		browser_window_key_press(bw, NS_KEY_PASTE);
	}

	return TRUE;
}


/**
 * handler for delete tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
delete_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;
	GtkWidget *focused;
	GtkWidget *toplevel;

	toplevel = gtk_widget_get_ancestor(widget, GTK_TYPE_WINDOW);

	focused = gtk_window_get_focus(GTK_WINDOW(toplevel));

	/* let gtk handle it if focused widget is an editable */
	if (GTK_IS_EDITABLE(focused)) {
		gtk_editable_delete_selection(GTK_EDITABLE(focused));
	} else {
		bw = tb->get_bw(tb->get_ctx);
		browser_window_key_press(bw, NS_KEY_CLEAR_SELECTION);
	}

	return TRUE;
}


/**
 * handler for select all tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
selectall_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;
	GtkWidget *focused;
	GtkWidget *toplevel;

	toplevel = gtk_widget_get_ancestor(widget, GTK_TYPE_WINDOW);

	focused = gtk_window_get_focus(GTK_WINDOW(toplevel));

	/* let gtk handle it if focused widget is an editable */
	if (GTK_IS_EDITABLE(focused)) {
		gtk_editable_select_region(GTK_EDITABLE(focused), 0, -1);
	} else {
		bw = tb->get_bw(tb->get_ctx);
		browser_window_key_press(bw, NS_KEY_SELECT_ALL);
	}

	return TRUE;
}


/**
 * handler for preferences tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
preferences_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;
	GtkWidget *toplevel;
	GtkWidget *wndpreferences;

	bw = tb->get_bw(tb->get_ctx);

	toplevel = gtk_widget_get_ancestor(widget, GTK_TYPE_WINDOW);

	wndpreferences = nsgtk_preferences(bw, GTK_WINDOW(toplevel));
	if (wndpreferences != NULL) {
		gtk_widget_show(wndpreferences);
	}

	return TRUE;
}


/**
 * handler for zoom plus tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
zoomplus_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;

	bw = tb->get_bw(tb->get_ctx);

	browser_window_set_scale(bw, 0.05, false);

	return TRUE;
}


/**
 * handler for zoom minus tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
zoomminus_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;

	bw = tb->get_bw(tb->get_ctx);

	browser_window_set_scale(bw, -0.05, false);

	return TRUE;

}


/**
 * handler for zoom normal tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
zoomnormal_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;

	bw = tb->get_bw(tb->get_ctx);

	browser_window_set_scale(bw, 1.0, true);

	return TRUE;
}


/**
 * handler for full screen tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
fullscreen_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	GtkWindow *gtkwindow; /* gtk window widget is in */
	GdkWindow *gdkwindow;
	GdkWindowState state;

	gtkwindow = GTK_WINDOW(gtk_widget_get_ancestor(widget,GTK_TYPE_WINDOW));
	gdkwindow = gtk_widget_get_window(GTK_WIDGET(gtkwindow));
	state = gdk_window_get_state(gdkwindow);

	if (state & GDK_WINDOW_STATE_FULLSCREEN) {
		gtk_window_unfullscreen(gtkwindow);
	} else {
		gtk_window_fullscreen(gtkwindow);
	}
	return TRUE;
}


/**
 * handler for view source tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
viewsource_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	nserror res;
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;
	GtkWindow *gtkwindow; /* gtk window widget is in */

	bw = tb->get_bw(tb->get_ctx);

	gtkwindow = GTK_WINDOW(gtk_widget_get_ancestor(widget,GTK_TYPE_WINDOW));

	res = nsgtk_viewsource(gtkwindow, bw);
	if (res != NSERROR_OK) {
		nsgtk_warning(messages_get_errorcode(res), 0);
	}

	return TRUE;
}


/**
 * handler for show downloads tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
downloads_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	GtkWindow *gtkwindow; /* gtk window widget is in */
	gtkwindow = GTK_WINDOW(gtk_widget_get_ancestor(widget,GTK_TYPE_WINDOW));
	nsgtk_download_show(gtkwindow);
	return TRUE;
}


/**
 * handler for show downloads tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
savewindowsize_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	GtkWindow *gtkwindow; /* gtk window widget is in */
	int x,y,w,h;
	char *choices = NULL;

	gtkwindow = GTK_WINDOW(gtk_widget_get_ancestor(widget,GTK_TYPE_WINDOW));

	gtk_window_get_position(gtkwindow, &x, &y);
	gtk_window_get_size(gtkwindow, &w, &h);

	nsoption_set_int(window_width, w);
	nsoption_set_int(window_height, h);
	nsoption_set_int(window_x, x);
	nsoption_set_int(window_y, y);

	netsurf_mkpath(&choices, NULL, 2, nsgtk_config_home, "Choices");
	if (choices != NULL) {
		nsoption_write(choices, NULL, NULL);
		free(choices);
	}

	return TRUE;
}


/**
 * handler for show downloads tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
toggledebugging_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;

	bw = tb->get_bw(tb->get_ctx);

	browser_window_debug(bw, CONTENT_DEBUG_REDRAW);

	nsgtk_window_update_all();

	return TRUE;
}


/**
 * handler for debug box tree tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
debugboxtree_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;
	gchar *fname;
	gint handle;
	FILE *f;

	handle = g_file_open_tmp("nsgtkboxtreeXXXXXX", &fname, NULL);
	if ((handle == -1) || (fname == NULL)) {
		return TRUE;
	}
	close(handle); /* in case it was binary mode */

	/* save data to temporary file */
	f = fopen(fname, "w");
	if (f == NULL) {
		nsgtk_warning("Error saving box tree dump.",
			      "Unable to open file for writing.");
		unlink(fname);

	} else {
		bw = tb->get_bw(tb->get_ctx);

		browser_window_debug_dump(bw, f, CONTENT_DEBUG_RENDER);

		fclose(f);

		nsgtk_viewfile("Box Tree Debug", "boxtree", fname);
	}
	g_free(fname);

	return TRUE;
}


/**
 * handler for debug dom tree tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
debugdomtree_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;
	gchar *fname;
	gint handle;
	FILE *f;

	handle = g_file_open_tmp("nsgtkdomtreeXXXXXX", &fname, NULL);
	if ((handle == -1) || (fname == NULL)) {
		return TRUE;
	}
	close(handle); /* in case it was binary mode */

	/* save data to temporary file */
	f = fopen(fname, "w");
	if (f == NULL) {
		nsgtk_warning("Error saving box tree dump.",
			      "Unable to open file for writing.");
		unlink(fname);

	} else {
		bw = tb->get_bw(tb->get_ctx);

		browser_window_debug_dump(bw, f, CONTENT_DEBUG_DOM);

		fclose(f);

		nsgtk_viewfile("DOM Tree Debug", "domtree", fname);
	}
	g_free(fname);

	return TRUE;

}


/**
 * handler for local history tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
localhistory_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	nserror res;
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;
	GtkWidget *toplevel;

	toplevel = gtk_widget_get_ancestor(widget, GTK_TYPE_WINDOW);
	if (toplevel != NULL) {
		bw = tb->get_bw(tb->get_ctx);

		res = nsgtk_local_history_present(GTK_WINDOW(toplevel), bw);
		if (res != NSERROR_OK) {
			NSLOG(netsurf, INFO,
			      "Unable to present local history window.");
		}
	}
	return TRUE;
}

/**
 * handler for history tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
history_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	return localhistory_button_clicked_cb(widget, data);
}


/**
 * handler for global history tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
globalhistory_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	nserror res;
	res = nsgtk_global_history_present();
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO,
		      "Unable to initialise global history window.");
	}
	return TRUE;
}


/**
 * handler for add bookmark tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
addbookmarks_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct browser_window *bw;

	bw = tb->get_bw(tb->get_ctx);
	if (browser_window_has_content(bw)) {
		hotlist_add_url(browser_window_access_url(bw));
	}
	return TRUE;
}


/**
 * handler for show bookmark tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
showbookmarks_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	nserror res;
	res = nsgtk_hotlist_present();
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Unable to initialise bookmark window.");
	}
	return TRUE;
}


/**
 * handler for show cookies tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
showcookies_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	nserror res;
	res = nsgtk_cookies_present(NULL);
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Unable to initialise cookies window.");
	}
	return TRUE;
}


/**
 * handler for open location tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
openlocation_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	GtkToolItem *urltitem;

	urltitem = tb->items[URL_BAR_ITEM].button;
	if (urltitem != NULL) {
		GtkEntry *entry;
		entry = GTK_ENTRY(gtk_bin_get_child(GTK_BIN(urltitem)));
		gtk_widget_grab_focus(GTK_WIDGET(entry));
	}
	return TRUE;
}


/**
 * handler for contents tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
contents_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	nserror res;

	res = toolbar_navigate_to_url(tb, "https://www.netsurf-browser.org/documentation/");
	if (res != NSERROR_OK) {
		nsgtk_warning(messages_get_errorcode(res), 0);
	}

	return TRUE;
}

/**
 * handler for contents tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
guide_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	nserror res;

	res = toolbar_navigate_to_url(tb, "https://www.netsurf-browser.org/documentation/guide");
	if (res != NSERROR_OK) {
		nsgtk_warning(messages_get_errorcode(res), 0);
	}

	return TRUE;
}


/**
 * handler for contents tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean
info_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	nserror res;

	res = toolbar_navigate_to_url(tb, "https://www.netsurf-browser.org/documentation/info");
	if (res != NSERROR_OK) {
		nsgtk_warning(messages_get_errorcode(res), 0);
	}

	return TRUE;
}


/**
 * handler for contents tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE
 */
static gboolean about_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	GtkWindow *parent; /* gtk window widget is in */

	parent = GTK_WINDOW(gtk_widget_get_ancestor(widget, GTK_TYPE_WINDOW));

	nsgtk_about_dialog_init(parent);
	return TRUE;
}

/**
 * handler for openmenu tool bar item clicked signal
 *
 * \param widget The widget the signal is being delivered to.
 * \param data The toolbar context passed when the signal was connected
 * \return TRUE to indicate signal handled.
 */
static gboolean openmenu_button_clicked_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct gui_window *gw;
	struct nsgtk_scaffolding *gs;

	gw = tb->get_ctx; /** \todo stop assuming the context is a gui window */

	gs = nsgtk_get_scaffold(gw);

	nsgtk_scaffolding_burger_menu(gs);

	return TRUE;
}


/* define data plus and data minus handlers */
#define TOOLBAR_ITEM(identifier, name, snstvty, clicked, activate, label, iconame) \
static gboolean								\
nsgtk_toolbar_##name##_data_plus(GtkWidget *widget,			\
				 GdkDragContext *cont,			\
				 GtkSelectionData *selection,		\
				 guint info,				\
				 guint time,				\
				 gpointer data)				\
{									\
	struct nsgtk_toolbar_customisation *tbc;			\
	tbc = (struct nsgtk_toolbar_customisation *)data;		\
	tbc->dragitem = identifier;					\
	tbc->dragfrom = true;						\
	return TRUE;							\
}									\
static gboolean								\
nsgtk_toolbar_##name##_data_minus(GtkWidget *widget,			\
				  GdkDragContext *cont,			\
				  GtkSelectionData *selection,		\
				  guint info,				\
				  guint time,				\
				  gpointer data)			\
{									\
	struct nsgtk_toolbar_customisation *tbc;			\
	tbc = (struct nsgtk_toolbar_customisation *)data;		\
	tbc->dragitem = identifier;					\
	tbc->dragfrom = false;						\
	return TRUE;							\
}

#include "gtk/toolbar_items.h"

#undef TOOLBAR_ITEM


/**
 * create a toolbar item
 *
 * create a toolbar item and set up its default handlers
 */
static nserror
toolbar_item_create(nsgtk_toolbar_button id, struct nsgtk_toolbar_item *item)
{
	item->location = INACTIVE_LOCATION;

	/* set item defaults from macro */
	switch (id) {
#define TOOLBAR_ITEM_t(name)						\
		item->clicked = name##_button_clicked_cb;
#define TOOLBAR_ITEM_b(name)						\
		item->clicked = name##_button_clicked_cb;
#define TOOLBAR_ITEM_y(name)						\
		item->clicked = name##_button_clicked_cb;
#define TOOLBAR_ITEM_n(name)						\
		item->clicked = NULL;
#define TOOLBAR_ITEM(identifier, iname, snstvty, clicked, activate, label, iconame) \
	case identifier:						\
		item->name = #iname;					\
		item->sensitivity = snstvty;				\
		item->dataplus = nsgtk_toolbar_##iname##_data_plus;	\
		item->dataminus = nsgtk_toolbar_##iname##_data_minus;	\
		TOOLBAR_ITEM_ ## clicked(iname)				\
		break;

#include "gtk/toolbar_items.h"

#undef TOOLBAR_ITEM_t
#undef TOOLBAR_ITEM_y
#undef TOOLBAR_ITEM_n
#undef TOOLBAR_ITEM

	case PLACEHOLDER_BUTTON:
		return NSERROR_INVALID;
	}

	return NSERROR_OK;
}


/**
 * set a toolbar item to a throbber frame number
 *
 * \param toolbar_item The toolbar item to update
 * \param frame The animation frame number to update to
 * \return NSERROR_OK on success,
 *         NSERROR_INVALID if the toolbar item does not contain an image,
 *         NSERROR_BAD_SIZE if the frame is out of range.
 */
static nserror set_throbber_frame(GtkToolItem *toolbar_item, int frame)
{
	nserror res;
	GdkPixbuf *pixbuf;
	GtkImage *throbber;

	if (toolbar_item == NULL) {
		/* no toolbar item */
		return NSERROR_INVALID;
	}

	res = nsgtk_throbber_get_frame(frame, &pixbuf);
	if (res != NSERROR_OK) {
		return res;
	}

	throbber = GTK_IMAGE(gtk_bin_get_child(GTK_BIN(toolbar_item)));

	gtk_image_set_from_pixbuf(throbber, pixbuf);

	return NSERROR_OK;
}


/**
 * Make the throbber run.
 *
 * scheduled callback to update the throbber
 *
 * \param p The context passed when scheduled.
 */
static void next_throbber_frame(void *p)
{
	struct nsgtk_toolbar *tb = p;
	nserror res;

	tb->throb_frame++; /* advance to next frame */

	res = set_throbber_frame(tb->items[THROBBER_ITEM].button,
				 tb->throb_frame);
	if (res == NSERROR_BAD_SIZE) {
		tb->throb_frame = 1;
		res = set_throbber_frame(tb->items[THROBBER_ITEM].button,
					 tb->throb_frame);
	}

	/* only schedule next frame if there are no errors */
	if (res == NSERROR_OK) {
		nsgtk_schedule(THROBBER_FRAME_TIME, next_throbber_frame, p);
	}
}


/**
 * connect signal handlers to a gtk toolbar item
 */
static nserror
toolbar_connect_signal(struct nsgtk_toolbar *tb, nsgtk_toolbar_button itemid)
{
	struct nsgtk_toolbar_item *item;
	GtkEntry *entry;

	item = &tb->items[itemid];

	if (item->button != NULL) {
		g_signal_connect(item->button,
				 "size-allocate",
				 G_CALLBACK(toolbar_item_size_allocate_cb),
				 tb);
	}

	switch (itemid) {
	case URL_BAR_ITEM:
		entry = GTK_ENTRY(gtk_bin_get_child(GTK_BIN(item->button)));

		g_signal_connect(GTK_WIDGET(entry),
				 "activate",
				 G_CALLBACK(url_entry_activate_cb),
				 tb);
		g_signal_connect(GTK_WIDGET(entry),
				 "changed",
				 G_CALLBACK(url_entry_changed_cb),
				 tb);
		g_signal_connect(GTK_WIDGET(entry),
				 "icon-release",
				 G_CALLBACK(url_entry_icon_release_cb),
				 tb);

		nsgtk_completion_connect_signals(entry,
						 tb->get_bw,
						 tb->get_ctx);
		break;


	case WEBSEARCH_ITEM:
		entry = GTK_ENTRY(gtk_bin_get_child(GTK_BIN(item->button)));

		g_signal_connect(GTK_WIDGET(entry),
				 "activate",
				 G_CALLBACK(websearch_entry_activate_cb),
				 tb);
		g_signal_connect(GTK_WIDGET(entry),
				 "button-press-event",
				 G_CALLBACK(websearch_entry_button_press_cb),
				 tb);
		break;

	default:
		if ((item->clicked != NULL) && (item->button != NULL)) {
			g_signal_connect(item->button,
					 "clicked",
					 G_CALLBACK(item->clicked),
					 tb);
		}
		break;

	}

	return NSERROR_OK;
}

/**
 * connect all signals to widgets in a toolbar
 */
static nserror toolbar_connect_signals(struct nsgtk_toolbar *tb)
{
	int location; /* location index */
	nsgtk_toolbar_button itemid; /* item id */

	for (location = BACK_BUTTON; location < PLACEHOLDER_BUTTON; location++) {
		itemid = itemid_from_location(tb, location);
		if (itemid == PLACEHOLDER_BUTTON) {
			/* no more filled locations */
			break;
		}
		toolbar_connect_signal(tb, itemid);
	}

	return NSERROR_OK;
}


/**
 * signal handler for toolbar context menu
 *
 * \param toolbar The toolbar event is being delivered to
 * \param x The x coordinate where the click happened
 * \param y The x coordinate where the click happened
 * \param button the buttons being pressed
 * \param data The context pointer passed when the connection was made.
 * \return TRUE to indicate signal handled.
 */
static gboolean
toolbar_popup_context_menu_cb(GtkToolbar *toolbar,
			      gint x,
			      gint y,
			      gint button,
			      gpointer data)
{
	struct nsgtk_toolbar *tb = (struct nsgtk_toolbar *)data;
	struct gui_window *gw;
	struct nsgtk_scaffolding *gs;

	gw = tb->get_ctx; /** \todo stop assuming the context is a gui window */

	gs = nsgtk_get_scaffold(gw);

	nsgtk_scaffolding_toolbar_context_menu(gs);

	return TRUE;
}


/**
 * toolbar delete signal handler
 */
static void toolbar_destroy_cb(GtkWidget *widget, gpointer data)
{
	struct nsgtk_toolbar *tb;
	tb = (struct nsgtk_toolbar *)data;

	/* ensure any throbber scheduled is stopped */
	nsgtk_schedule(-1, next_throbber_frame, tb);

	free(tb);
}


/* exported interface documented in toolbar.h */
nserror
nsgtk_toolbar_create(GtkBuilder *builder,
		     struct browser_window *(*get_bw)(void *ctx),
		     void *get_ctx,
		     bool want_location_focus,
		     struct nsgtk_toolbar **tb_out)
{
	nserror res;
	struct nsgtk_toolbar *tb;
	int bidx; /* button index */

	tb = calloc(1, sizeof(struct nsgtk_toolbar));
	if (tb == NULL) {
		return NSERROR_NOMEM;
	}

	tb->get_bw = get_bw;
	tb->get_ctx = get_ctx;
	/* set the throbber start frame. */
	tb->throb_frame = 0;
	if (want_location_focus) {
		tb->loc_focus = LFS_WANT;
	} else {
		tb->loc_focus = LFS_IDLE;
	}

	tb->widget = GTK_TOOLBAR(gtk_builder_get_object(builder, "toolbar"));
	gtk_toolbar_set_show_arrow(tb->widget, TRUE);

	g_signal_connect(tb->widget,
			 "popup-context-menu",
			 G_CALLBACK(toolbar_popup_context_menu_cb),
			 tb);

	/* close and cleanup on delete signal */
	g_signal_connect(tb->widget,
			 "destroy",
			 G_CALLBACK(toolbar_destroy_cb),
			 tb);

	/* allocate button contexts */
	for (bidx = BACK_BUTTON; bidx < PLACEHOLDER_BUTTON; bidx++) {
		res = toolbar_item_create(bidx, &tb->items[bidx]);
		if (res != NSERROR_OK) {
			return res;
		}
	}

	res = nsgtk_toolbar_update(tb);
	if (res != NSERROR_OK) {
		return res;
	}

	*tb_out = tb;
	return NSERROR_OK;
}


/* exported interface documented in toolbar.h */
nserror nsgtk_toolbar_restyle(struct nsgtk_toolbar *tb)
{
	/*
	 * reset toolbar size allocation so icon size change affects
	 *  allocated widths.
	 */
	tb->offset = 0;

	switch (nsoption_int(button_type)) {

	case 1: /* Small icons */
		gtk_toolbar_set_style(GTK_TOOLBAR(tb->widget),
				      GTK_TOOLBAR_ICONS);
		gtk_toolbar_set_icon_size(GTK_TOOLBAR(tb->widget),
					  GTK_ICON_SIZE_SMALL_TOOLBAR);
		break;

	case 2: /* Large icons */
		gtk_toolbar_set_style(GTK_TOOLBAR(tb->widget),
				      GTK_TOOLBAR_ICONS);
		gtk_toolbar_set_icon_size(GTK_TOOLBAR(tb->widget),
					  GTK_ICON_SIZE_LARGE_TOOLBAR);
		break;

	case 3: /* Large icons with text */
		gtk_toolbar_set_style(GTK_TOOLBAR(tb->widget),
				      GTK_TOOLBAR_BOTH);
		gtk_toolbar_set_icon_size(GTK_TOOLBAR(tb->widget),
					  GTK_ICON_SIZE_LARGE_TOOLBAR);
		break;

	case 4: /* Text icons only */
		gtk_toolbar_set_style(GTK_TOOLBAR(tb->widget),
				      GTK_TOOLBAR_TEXT);
		break;

	default:
		break;
	}

	return NSERROR_OK;
}


/* exported interface documented in toolbar.h */
nserror nsgtk_toolbar_throbber(struct nsgtk_toolbar *tb, bool active)
{
	nserror res;
	struct browser_window *bw;

	/* Manage the location focus state */
	switch (tb->loc_focus) {
	case LFS_IDLE:
		break;
	case LFS_WANT:
		if (active) {
			tb->loc_focus = LFS_THROB;
		}
		break;
	case LFS_THROB:
		if (!active) {
			tb->loc_focus = LFS_LAST;
		}
		break;
	case LFS_LAST:
		break;
	}

	/* when activating the throbber simply schedule the next frame update */
	if (active) {
		nsgtk_schedule(THROBBER_FRAME_TIME, next_throbber_frame, tb);

		set_item_sensitivity(&tb->items[STOP_BUTTON], true);
		set_item_sensitivity(&tb->items[RELOAD_BUTTON], false);
		set_item_action(tb, RELOADSTOP_BUTTON, false);

		return NSERROR_OK;
	}

	/* stopping the throbber */
	nsgtk_schedule(-1, next_throbber_frame, tb);
	tb->throb_frame = 0;
	res =  set_throbber_frame(tb->items[THROBBER_ITEM].button,
				  tb->throb_frame);

	bw = tb->get_bw(tb->get_ctx);

	/* adjust sensitivity of other items */
	set_item_sensitivity(&tb->items[STOP_BUTTON], false);
	set_item_sensitivity(&tb->items[RELOAD_BUTTON], true);
	set_item_action(tb, RELOADSTOP_BUTTON, true);
	set_item_sensitivity(&tb->items[BACK_BUTTON],
			     browser_window_history_back_available(bw));
	set_item_sensitivity(&tb->items[FORWARD_BUTTON],
			     browser_window_history_forward_available(bw));
	nsgtk_local_history_hide();

	return res;
}


/* exported interface documented in toolbar.h */
nserror nsgtk_toolbar_page_info_change(struct nsgtk_toolbar *tb)
{
	GtkEntry *url_entry;
	browser_window_page_info_state pistate;
	struct browser_window *bw;
	const char *icon_name;

	if (tb->items[URL_BAR_ITEM].button == NULL) {
		/* no toolbar item */
		return NSERROR_INVALID;
	}
	url_entry = GTK_ENTRY(gtk_bin_get_child(GTK_BIN(tb->items[URL_BAR_ITEM].button)));

	bw = tb->get_bw(tb->get_ctx);

	pistate = browser_window_get_page_info_state(bw);

	switch (pistate) {
	case PAGE_STATE_INTERNAL:
		icon_name = "page-info-internal";
		break;

	case PAGE_STATE_LOCAL:
		icon_name = "page-info-local";
		break;

	case PAGE_STATE_INSECURE:
		icon_name = "page-info-insecure";
		break;

	case PAGE_STATE_SECURE_OVERRIDE:
		icon_name = "page-info-warning";
		break;

	case PAGE_STATE_SECURE_ISSUES:
		icon_name = "page-info-warning";
		break;

	case PAGE_STATE_SECURE:
		icon_name = "page-info-secure";
		break;

	default:
		icon_name = "page-info-internal";
		break;
	}

	nsgtk_entry_set_icon_from_icon_name(GTK_WIDGET(url_entry),
					    GTK_ENTRY_ICON_PRIMARY,
					    icon_name);
	return NSERROR_OK;
}


/* exported interface documented in toolbar.h */
nserror nsgtk_toolbar_set_url(struct nsgtk_toolbar *tb, nsurl *url)
{
	size_t idn_url_l;
	char *idn_url_s = NULL;
	const char *url_text = NULL;
	GtkEntry *url_entry;

	if (tb->items[URL_BAR_ITEM].button == NULL) {
		/* no toolbar item */
		return NSERROR_INVALID;
	}
	url_entry = GTK_ENTRY(gtk_bin_get_child(GTK_BIN(tb->items[URL_BAR_ITEM].button)));

	if (nsoption_bool(display_decoded_idn) == true) {
		if (nsurl_get_utf8(url, &idn_url_s, &idn_url_l) != NSERROR_OK) {
			idn_url_s = NULL;
		}
		url_text = idn_url_s;
	}
	if (url_text == NULL) {
		url_text = nsurl_access(url);
	}

	if (strcmp(url_text, gtk_entry_get_text(url_entry)) != 0) {
		/* The URL bar content has changed, we need to update it */
		gint startpos, endpos;
		bool was_selected;
		gtk_editable_get_selection_bounds(GTK_EDITABLE(url_entry),
						  &startpos, &endpos);
		was_selected = gtk_widget_is_focus(GTK_WIDGET(url_entry)) &&
			startpos == 0 &&
			endpos == gtk_entry_get_text_length(url_entry);
		gtk_entry_set_text(url_entry, url_text);
		if (was_selected && tb->loc_focus != LFS_IDLE) {
			gtk_widget_grab_focus(GTK_WIDGET(url_entry));
			if (tb->loc_focus == LFS_LAST) {
				tb->loc_focus = LFS_IDLE;
			}
		}
	}

	if (idn_url_s != NULL) {
		free(idn_url_s);
	}

	return NSERROR_OK;
}


/* exported interface documented in toolbar.h */
nserror
nsgtk_toolbar_set_websearch_image(struct nsgtk_toolbar *tb, GdkPixbuf *pixbuf)
{
	GtkWidget *entry;

	if (tb->items[WEBSEARCH_ITEM].button == NULL) {
		/* no toolbar item */
		return NSERROR_INVALID;
	}

	entry = gtk_bin_get_child(GTK_BIN(tb->items[WEBSEARCH_ITEM].button));

	if (pixbuf != NULL) {
		nsgtk_entry_set_icon_from_pixbuf(entry,
						 GTK_ENTRY_ICON_PRIMARY,
						 pixbuf);
	} else {
		nsgtk_entry_set_icon_from_icon_name(entry,
						    GTK_ENTRY_ICON_PRIMARY,
						    NSGTK_STOCK_INFO);
	}

	return NSERROR_OK;
}


/* exported interface documented in toolbar.h */
nserror
nsgtk_toolbar_item_activate(struct nsgtk_toolbar *tb,
			    nsgtk_toolbar_button itemid)
{
	GtkWidget *widget;

	/* ensure item id in range */
	if ((itemid < BACK_BUTTON) || (itemid >= PLACEHOLDER_BUTTON)) {
		return NSERROR_BAD_PARAMETER;
	}

	if (tb->items[itemid].clicked == NULL) {
		return NSERROR_INVALID;
	}

	/*
	 * if item has a widget in the current toolbar use that as the
	 *   signal source otherwise use the toolbar widget itself.
	 */
	if (tb->items[itemid].button != NULL) {
		widget = GTK_WIDGET(tb->items[itemid].button);
	} else {
		widget = GTK_WIDGET(tb->widget);
	}

	tb->items[itemid].clicked(widget, tb);

	return NSERROR_OK;
}


/* exported interface documented in toolbar.h */
nserror nsgtk_toolbar_show(struct nsgtk_toolbar *tb, bool show)
{
	if (show) {
		gtk_widget_show(GTK_WIDGET(tb->widget));
	} else {
		gtk_widget_hide(GTK_WIDGET(tb->widget));
	}
	return NSERROR_OK;
}


/* exported interface documented in toolbar.h */
nserror nsgtk_toolbar_update(struct nsgtk_toolbar *tb)
{
	nserror res;

	/* setup item locations based on user config */
	res = apply_user_button_customisation(tb);
	if (res != NSERROR_OK) {
		return res;
	}

	/* populate toolbar widget */
	res = populate_gtk_toolbar_widget(tb);
	if (res != NSERROR_OK) {
		return res;
	}

	/* ensure icon sizes and text labels on toolbar are set */
	res = nsgtk_toolbar_restyle(tb);
	if (res != NSERROR_OK) {
		return res;
	}

	res = toolbar_connect_signals(tb);

	return res;
}

/**
 * Find the correct location for popping up a window for the chosen item.
 *
 * \param tb The toolbar to select from
 * \param item_idx The toolbar item to select from
 * \param out_x Filled with an appropriate X coordinate
 * \param out_y Filled with an appropriate Y coordinate
 */
static nserror
nsgtk_toolbar_get_icon_window_position(struct nsgtk_toolbar *tb,
				       int item_idx,
				       int *out_x,
				       int *out_y)
{
	struct nsgtk_toolbar_item *item = &tb->items[item_idx];
	GtkWidget *widget = GTK_WIDGET(item->button);
	GtkAllocation alloc;
	gint rootx, rooty, x, y;

	switch (item_idx) {
	case URL_BAR_ITEM:
		widget = GTK_WIDGET(gtk_bin_get_child(GTK_BIN(item->button)));
		break;
	default:
		/* Nothing to do here */
		break;
	}

	nsgtk_widget_get_allocation(widget, &alloc);

	if (gtk_widget_translate_coordinates(widget,
					     gtk_widget_get_toplevel(widget),
					     0,
					     alloc.height - 1,
					     &x, &y) != TRUE) {
		return NSERROR_UNKNOWN;
	}

	gtk_window_get_position(GTK_WINDOW(gtk_widget_get_toplevel(widget)),
				&rootx, &rooty);

	*out_x = rootx + x + 4;
	*out_y = rooty + y + 4;

	return NSERROR_OK;
}

nserror nsgtk_toolbar_position_page_info(struct nsgtk_toolbar *tb,
					 struct nsgtk_pi_window *win)
{
	nserror res;
	int x, y;

	res = nsgtk_toolbar_get_icon_window_position(tb, URL_BAR_ITEM, &x, &y);
	if (res != NSERROR_OK) {
		return res;
	}

	nsgtk_page_info_set_position(win, x, y);

	return NSERROR_OK;
}

/* exported interface documented in toolbar.h */
nserror nsgtk_toolbar_position_local_history(struct nsgtk_toolbar *tb)
{
	nserror res;
	int x, y;

	res = nsgtk_toolbar_get_icon_window_position(tb, HISTORY_BUTTON, &x, &y);
	if (res != NSERROR_OK) {
		return res;
	}

	nsgtk_local_history_set_position(x, y);

	return NSERROR_OK;
}
