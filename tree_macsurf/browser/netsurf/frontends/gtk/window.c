/*
 * Copyright 2006 Daniel Silverstone <dsilvers@digital-scurf.org>
 * Copyright 2006 Rob Kendrick <rjek@rjek.com>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * Implementation of gtk windowing.
 */

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <math.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gdk-pixbuf/gdk-pixdata.h>

#include "utils/utils.h"
#include "utils/log.h"
#include "utils/utf8.h"
#include "utils/nsoption.h"
#include "utils/messages.h"
#include "utils/nsurl.h"
#include "netsurf/inttypes.h"
#include "netsurf/content.h"
#include "netsurf/browser_window.h"
#include "netsurf/mouse.h"
#include "netsurf/window.h"
#include "netsurf/plotters.h"
#include "netsurf/form.h"
#include "netsurf/keypress.h"
#include "desktop/searchweb.h"
#include "desktop/textinput.h"

#include "gtk/selection.h"
#include "gtk/warn.h"
#include "gtk/compat.h"
#include "gtk/gui.h"
#include "gtk/scaffolding.h"
#include "gtk/toolbar_items.h"
#include "gtk/toolbar.h"
#include "gtk/local_history.h"
#include "gtk/plotters.h"
#include "gtk/schedule.h"
#include "gtk/tabs.h"
#include "gtk/bitmap.h"
#include "gtk/gdk.h"
#include "gtk/resources.h"
#include "gtk/search.h"
#include "gtk/throbber.h"
#include "gtk/window.h"

/**
 * time (in ms) between throbber animation frame updates
 */
#define THROBBER_FRAME_TIME (100)

static GtkWidget *select_menu;
static struct form_control *select_menu_control;

struct gui_window {
	/**
	 * The gtk scaffold object containing menu, buttons, url bar, [tabs],
	 * drawing area, etc that may contain one or more gui_windows.
	 */
	struct nsgtk_scaffolding *scaffold;

	/** The 'content' window that is rendered in the gui_window */
	struct browser_window	*bw;

	/** mouse state and events. */
	struct {
		struct gui_window *gui;

		gdouble pressed_x;
		gdouble pressed_y;
		gboolean waiting;
		browser_mouse_state state;
	} mouse;

	/** caret dimension and location for rendering */
	int caretx, carety, careth;

	/** caret shape for rendering */
	gui_pointer_shape current_pointer;

	/** previous event location */
	int last_x, last_y;

	/** controls toolbar context */
	struct nsgtk_toolbar *toolbar;

	/** search toolbar context */
	struct gtk_search *search;

	/** The top level container (tabBox) */
	GtkWidget *container;

	/** display widget for this page or frame */
	GtkLayout *layout;

	/** The container for the layout etc */
	GtkWidget *grid;

	/** handle to the the visible tab */
	GtkWidget *tab;

	/** statusbar */
	GtkLabel *status_bar;

	/** status pane */
	GtkPaned *paned;

	/** has the status pane had its first size operation yet? */
	bool paned_sized;

	/** The icon this window should have */
	GdkPixbuf *icon;

	/** The input method to use with this window */
	GtkIMContext *input_method;

	/** current frame of throbber */
	int throb_frame;

	/** list for cleanup */
	struct gui_window *next, *prev;
};

/**< first entry in window list */
struct gui_window *window_list = NULL;

static void
nsgtk_select_menu_clicked(GtkCheckMenuItem *checkmenuitem,
			  gpointer user_data)
{
	form_select_process_selection(select_menu_control,
				      (intptr_t)user_data);
}

#if GTK_CHECK_VERSION(3,0,0)

static gboolean
nsgtk_window_draw_event(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	struct gui_window *gw = data;
	struct gui_window *z;
	struct rect clip;
	struct redraw_context ctx = {
		.interactive = true,
		.background_images = true,
		.plot = &nsgtk_plotters
	};

	double x1;
	double y1;
	double x2;
	double y2;

	assert(gw);
	assert(gw->bw);

	for (z = window_list; z && z != gw; z = z->next)
		continue;
	assert(z);
	assert(GTK_WIDGET(gw->layout) == widget);

	current_cr = cr;

	GtkAdjustment *vscroll = nsgtk_layout_get_vadjustment(gw->layout);
	GtkAdjustment *hscroll = nsgtk_layout_get_hadjustment(gw->layout);

	cairo_clip_extents(cr, &x1, &y1, &x2, &y2);

	clip.x0 = x1;
	clip.y0 = y1;
	clip.x1 = x2;
	clip.y1 = y2;

	browser_window_redraw(gw->bw,
			      -gtk_adjustment_get_value(hscroll),
			      -gtk_adjustment_get_value(vscroll),
			      &clip,
			      &ctx);

	if (gw->careth != 0) {
		nsgtk_plot_caret(gw->caretx, gw->carety, gw->careth);
	}

	return FALSE;
}

#else

static gboolean
nsgtk_window_draw_event(GtkWidget *widget, GdkEventExpose *event, gpointer data)
{
	struct gui_window *gw = data;
	struct gui_window *z;
	struct rect clip;
	struct redraw_context ctx = {
		.interactive = true,
		.background_images = true,
		.plot = &nsgtk_plotters
	};

	assert(gw);
	assert(gw->bw);

	for (z = window_list; z && z != gw; z = z->next)
		continue;
	assert(z);
	assert(GTK_WIDGET(gw->layout) == widget);

	current_cr = gdk_cairo_create(nsgtk_layout_get_bin_window(gw->layout));

	clip.x0 = event->area.x;
	clip.y0 = event->area.y;
	clip.x1 = event->area.x + event->area.width;
	clip.y1 = event->area.y + event->area.height;

	browser_window_redraw(gw->bw, 0, 0, &clip, &ctx);

	if (gw->careth != 0) {
		nsgtk_plot_caret(gw->caretx, gw->carety, gw->careth);
	}

	cairo_destroy(current_cr);

	return FALSE;
}

#endif

static gboolean
nsgtk_window_motion_notify_event(GtkWidget *widget,
				 GdkEventMotion *event,
				 gpointer data)
{
	struct gui_window *g = data;
	bool shift = event->state & GDK_SHIFT_MASK;
	bool ctrl = event->state & GDK_CONTROL_MASK;

	if ((fabs(event->x - g->last_x) < 5.0) &&
	    (fabs(event->y - g->last_y) < 5.0)) {
		/* Mouse hasn't moved far enough from press coordinate
		 * for this to be considered a drag.
		 */
		return FALSE;
	} else {
		/* This is a drag, ensure it's always treated as such,
		 * even if we drag back over the press location.
		 */
		g->last_x = INT_MIN;
		g->last_y = INT_MIN;
	}

	if (g->mouse.state & BROWSER_MOUSE_PRESS_1) {
		/* Start button 1 drag */
		browser_window_mouse_click(g->bw, BROWSER_MOUSE_DRAG_1,
				g->mouse.pressed_x, g->mouse.pressed_y);

		/* Replace PRESS with HOLDING and declare drag in progress */
		g->mouse.state ^= (BROWSER_MOUSE_PRESS_1 |
				BROWSER_MOUSE_HOLDING_1);
		g->mouse.state |= BROWSER_MOUSE_DRAG_ON;
	} else if (g->mouse.state & BROWSER_MOUSE_PRESS_2) {
		/* Start button 2 drag */
		browser_window_mouse_click(g->bw, BROWSER_MOUSE_DRAG_2,
				g->mouse.pressed_x, g->mouse.pressed_y);

		/* Replace PRESS with HOLDING and declare drag in progress */
		g->mouse.state ^= (BROWSER_MOUSE_PRESS_2 |
				BROWSER_MOUSE_HOLDING_2);
		g->mouse.state |= BROWSER_MOUSE_DRAG_ON;
	}

	/* Handle modifiers being removed */
	if (g->mouse.state & BROWSER_MOUSE_MOD_1 && !shift)
		g->mouse.state ^= BROWSER_MOUSE_MOD_1;
	if (g->mouse.state & BROWSER_MOUSE_MOD_2 && !ctrl)
		g->mouse.state ^= BROWSER_MOUSE_MOD_2;

	browser_window_mouse_track(g->bw, g->mouse.state, event->x, event->y);

	return TRUE;
}

/**
 * GTK signal handler for focus-out-event on layout
 *
 * when focus leaves the layout widget ensure the caret is cleared
 */
static gboolean
nsgtk_window_focus_out_event(GtkWidget *widget,
			     GdkEvent *event,
			     gpointer data)
{
	struct gui_window *g = data;

	browser_window_remove_caret(g->bw, true);
	return FALSE;
}

/**
 * GTK signal handler for button-press-event on layout
 */
static gboolean
nsgtk_window_button_press_event(GtkWidget *widget,
				GdkEventButton *event,
				gpointer data)
{
	struct gui_window *g = data;

	gtk_im_context_reset(g->input_method);
	gtk_widget_grab_focus(GTK_WIDGET(g->layout));
	nsgtk_local_history_hide();

	g->mouse.pressed_x = event->x;
	g->mouse.pressed_y = event->y;

	switch (event->button) {
	case 1:	/* Left button, usually. Pass to core as BUTTON 1. */
		g->mouse.state = BROWSER_MOUSE_PRESS_1;
		break;

	case 2:	/* Middle button, usually. Pass to core as BUTTON 2 */
		g->mouse.state = BROWSER_MOUSE_PRESS_2;
		break;

	case 3:	/* Right button, usually. Action button, context menu. */
		/** \todo determine if hiding the caret here is necessary */
		browser_window_remove_caret(g->bw, true);
		nsgtk_scaffolding_context_menu(g->scaffold,
					       g->mouse.pressed_x,
					       g->mouse.pressed_y);
		return TRUE;

	case 8: /* Back button */
		g->mouse.state = BROWSER_MOUSE_PRESS_4;
		break;

	case 9: /* Forward button */
		g->mouse.state = BROWSER_MOUSE_PRESS_5;
		break;

	default:
		return FALSE;
	}

	/* Modify for double & triple clicks */
	if (event->type == GDK_3BUTTON_PRESS)
		g->mouse.state |= BROWSER_MOUSE_TRIPLE_CLICK;
	else if (event->type == GDK_2BUTTON_PRESS)
		g->mouse.state |= BROWSER_MOUSE_DOUBLE_CLICK;

	/* Handle the modifiers too */
	if (event->state & GDK_SHIFT_MASK)
		g->mouse.state |= BROWSER_MOUSE_MOD_1;
	if (event->state & GDK_CONTROL_MASK)
		g->mouse.state |= BROWSER_MOUSE_MOD_2;
	if (event->state & GDK_MOD1_MASK)
		g->mouse.state |= BROWSER_MOUSE_MOD_3;

	/* Record where we pressed, for use when determining whether to start
	 * a drag in motion notify events. */
	g->last_x = event->x;
	g->last_y = event->y;

	browser_window_mouse_click(g->bw,
				   g->mouse.state,
				   g->mouse.pressed_x,
				   g->mouse.pressed_y);

	return TRUE;
}


static gboolean
nsgtk_window_button_release_event(GtkWidget *widget,
				  GdkEventButton *event,
				  gpointer data)
{
	struct gui_window *g = data;

	/* If the mouse state is PRESS then we are waiting for a release to emit
	 * a click event, otherwise just reset the state to nothing */
	if (g->mouse.state & BROWSER_MOUSE_PRESS_1) {
		g->mouse.state ^= (BROWSER_MOUSE_PRESS_1 | BROWSER_MOUSE_CLICK_1);
	} else if (g->mouse.state & BROWSER_MOUSE_PRESS_2) {
		g->mouse.state ^= (BROWSER_MOUSE_PRESS_2 | BROWSER_MOUSE_CLICK_2);
	} else if (g->mouse.state & BROWSER_MOUSE_PRESS_3) {
		g->mouse.state ^= (BROWSER_MOUSE_PRESS_3 | BROWSER_MOUSE_CLICK_3);
	} else if (g->mouse.state & BROWSER_MOUSE_PRESS_4) {
		g->mouse.state ^= (BROWSER_MOUSE_PRESS_4 | BROWSER_MOUSE_CLICK_4);
	} else if (g->mouse.state & BROWSER_MOUSE_PRESS_5) {
		g->mouse.state ^= (BROWSER_MOUSE_PRESS_5 | BROWSER_MOUSE_CLICK_5);
	}

	/* Handle modifiers being removed */
	if (g->mouse.state & BROWSER_MOUSE_MOD_1 &&
	    !(event->state & GDK_SHIFT_MASK)) {
		g->mouse.state ^= BROWSER_MOUSE_MOD_1;
	}
	if (g->mouse.state & BROWSER_MOUSE_MOD_2 &&
	    !(event->state & GDK_CONTROL_MASK)) {
		g->mouse.state ^= BROWSER_MOUSE_MOD_2;
	}
	if (g->mouse.state & BROWSER_MOUSE_MOD_3 &&
	    !(event->state & GDK_MOD1_MASK)) {
		g->mouse.state ^= BROWSER_MOUSE_MOD_3;
	}

	if (g->mouse.state & (BROWSER_MOUSE_CLICK_1 |
			      BROWSER_MOUSE_CLICK_2 |
			      BROWSER_MOUSE_CLICK_3 |
			      BROWSER_MOUSE_CLICK_4 |
			      BROWSER_MOUSE_CLICK_5)) {
		browser_window_mouse_click(g->bw, g->mouse.state, event->x, event->y);
	} else {
		browser_window_mouse_track(g->bw, 0, event->x, event->y);
	}

	g->mouse.state = 0;
	return TRUE;
}


static gboolean
nsgtk_window_scroll_event(GtkWidget *widget,
			  GdkEventScroll *event,
			  gpointer data)
{
	struct gui_window *g = data;
	double value;
	double deltax = 0;
	double deltay = 0;
	GtkAdjustment *vscroll = nsgtk_layout_get_vadjustment(g->layout);
	GtkAdjustment *hscroll = nsgtk_layout_get_hadjustment(g->layout);
	GtkAllocation alloc;

	switch (event->direction) {
	case GDK_SCROLL_LEFT:
		deltax = -1.0;
		break;

	case GDK_SCROLL_UP:
		deltay = -1.0;
		break;

	case GDK_SCROLL_RIGHT:
		deltax = 1.0;
		break;

	case GDK_SCROLL_DOWN:
		deltay = 1.0;
		break;

#if GTK_CHECK_VERSION(3,4,0)
	case GDK_SCROLL_SMOOTH:
		gdk_event_get_scroll_deltas((GdkEvent *)event, &deltax, &deltay);
		break;
#endif
	default:
		NSLOG(netsurf, INFO, "Unhandled mouse scroll direction");
		return TRUE;
	}

	deltax *= nsgtk_adjustment_get_step_increment(hscroll);
	deltay *= nsgtk_adjustment_get_step_increment(vscroll);

	if (browser_window_scroll_at_point(g->bw,
					   event->x, event->y,
					   deltax, deltay) != true) {

		/* core did not handle event so change adjustments */

		/* Horizontal */
		if (deltax != 0) {
			value = gtk_adjustment_get_value(hscroll) + deltax;

			/* @todo consider gtk_widget_get_allocated_width() */
			nsgtk_widget_get_allocation(GTK_WIDGET(g->layout), &alloc);

			if (value > nsgtk_adjustment_get_upper(hscroll) - alloc.width) {
				value = nsgtk_adjustment_get_upper(hscroll) - alloc.width;
			}
			if (value < nsgtk_adjustment_get_lower(hscroll)) {
				value = nsgtk_adjustment_get_lower(hscroll);
			}

			gtk_adjustment_set_value(hscroll, value);
		}

		/* Vertical */
		if (deltay != 0) {
			value = gtk_adjustment_get_value(vscroll) + deltay;

			/* @todo consider gtk_widget_get_allocated_height */
			nsgtk_widget_get_allocation(GTK_WIDGET(g->layout), &alloc);

			if (value > (nsgtk_adjustment_get_upper(vscroll) - alloc.height)) {
				value = nsgtk_adjustment_get_upper(vscroll) - alloc.height;
			}
			if (value < nsgtk_adjustment_get_lower(vscroll)) {
				value = nsgtk_adjustment_get_lower(vscroll);
			}

			gtk_adjustment_set_value(vscroll, value);
		}
	}

	return TRUE;
}


static gboolean
nsgtk_window_keypress_event(GtkWidget *widget,
			    GdkEventKey *event,
			    gpointer data)
{
	struct gui_window *g = data;
	uint32_t nskey;

	if (gtk_im_context_filter_keypress(g->input_method, event))
		return TRUE;

	nskey = gtk_gui_gdkkey_to_nskey(event);

	if (browser_window_key_press(g->bw, nskey))
		return TRUE;

	if ((event->state & 0x7) != 0)
		return TRUE;

	double value;
	GtkAdjustment *vscroll = nsgtk_layout_get_vadjustment(g->layout);
	GtkAdjustment *hscroll = nsgtk_layout_get_hadjustment(g->layout);
	GtkAllocation alloc;

	/* @todo consider gtk_widget_get_allocated_width() */
	nsgtk_widget_get_allocation(GTK_WIDGET(g->layout), &alloc);

	switch (event->keyval) {

	case GDK_KEY(Home):
	case GDK_KEY(KP_Home):
		value = nsgtk_adjustment_get_lower(vscroll);
		gtk_adjustment_set_value(vscroll, value);
		break;

	case GDK_KEY(End):
	case GDK_KEY(KP_End):
		value = nsgtk_adjustment_get_upper(vscroll) - alloc.height;

		if (value < nsgtk_adjustment_get_lower(vscroll))
			value = nsgtk_adjustment_get_lower(vscroll);

		gtk_adjustment_set_value(vscroll, value);
		break;

	case GDK_KEY(Left):
	case GDK_KEY(KP_Left):
		value = gtk_adjustment_get_value(hscroll) -
			nsgtk_adjustment_get_step_increment(hscroll);

		if (value < nsgtk_adjustment_get_lower(hscroll))
			value = nsgtk_adjustment_get_lower(hscroll);

		gtk_adjustment_set_value(hscroll, value);
		break;

	case GDK_KEY(Up):
	case GDK_KEY(KP_Up):
		value = gtk_adjustment_get_value(vscroll) -
			nsgtk_adjustment_get_step_increment(vscroll);

		if (value < nsgtk_adjustment_get_lower(vscroll))
			value = nsgtk_adjustment_get_lower(vscroll);

		gtk_adjustment_set_value(vscroll, value);
		break;

	case GDK_KEY(Right):
	case GDK_KEY(KP_Right):
		value = gtk_adjustment_get_value(hscroll) +
			nsgtk_adjustment_get_step_increment(hscroll);

		if (value > nsgtk_adjustment_get_upper(hscroll) - alloc.width)
			value = nsgtk_adjustment_get_upper(hscroll) - alloc.width;

		gtk_adjustment_set_value(hscroll, value);
		break;

	case GDK_KEY(Down):
	case GDK_KEY(KP_Down):
		value = gtk_adjustment_get_value(vscroll) +
			nsgtk_adjustment_get_step_increment(vscroll);

		if (value > nsgtk_adjustment_get_upper(vscroll) - alloc.height)
			value = nsgtk_adjustment_get_upper(vscroll) - alloc.height;

		gtk_adjustment_set_value(vscroll, value);
		break;

	case GDK_KEY(Page_Up):
	case GDK_KEY(KP_Page_Up):
		value = gtk_adjustment_get_value(vscroll) -
			nsgtk_adjustment_get_page_increment(vscroll);

		if (value < nsgtk_adjustment_get_lower(vscroll))
			value = nsgtk_adjustment_get_lower(vscroll);

		gtk_adjustment_set_value(vscroll, value);
		break;

	case GDK_KEY(Page_Down):
	case GDK_KEY(KP_Page_Down):
		value = gtk_adjustment_get_value(vscroll) +
			nsgtk_adjustment_get_page_increment(vscroll);

		if (value > nsgtk_adjustment_get_upper(vscroll) - alloc.height)
			value = nsgtk_adjustment_get_upper(vscroll) - alloc.height;

		gtk_adjustment_set_value(vscroll, value);
		break;

	default:
		break;

	}

	return TRUE;
}


static gboolean
nsgtk_window_keyrelease_event(GtkWidget *widget,
			      GdkEventKey *event,
			      gpointer data)
{
	struct gui_window *g = data;

	return gtk_im_context_filter_keypress(g->input_method, event);
}


static void
nsgtk_window_input_method_commit(GtkIMContext *ctx,
				 const gchar *str,
				 gpointer data)
{
	struct gui_window *g = data;
	size_t len = strlen(str), offset = 0;

	while (offset < len) {
		uint32_t nskey = utf8_to_ucs4(str + offset, len - offset);

		browser_window_key_press(g->bw, nskey);

		offset = utf8_next(str, len, offset);
	}
}


static gboolean
nsgtk_window_size_allocate_event(GtkWidget *widget,
				 GtkAllocation *allocation,
				 gpointer data)
{
	struct gui_window *g = data;

	browser_window_schedule_reformat(g->bw);

	return TRUE;
}


/**
 * when the pane position is changed update the user option
 *
 * The slightly awkward implementation with the first allocation flag
 * is necessary because the initial window creation does not cause an
 * allocate-event signal so the position value in the pane is incorrect
 * and we cannot know what it should be until after the allocation
 * (which did not generate a signal) is done as the user position is a
 * percentage of pane total width not an absolute value.
 */
static void
nsgtk_paned_notify__position(GObject *gobject, GParamSpec *pspec, gpointer data)
{
	struct gui_window *g = data;
	GtkAllocation pane_alloc;

	gtk_widget_get_allocation(GTK_WIDGET(g->paned), &pane_alloc);

	if (g->paned_sized == false)
	{
		g->paned_sized = true;
		gtk_paned_set_position(g->paned,
		(nsoption_int(toolbar_status_size) * pane_alloc.width) / 10000);
		return;
	}

	nsoption_set_int(toolbar_status_size,
	 ((gtk_paned_get_position(g->paned) * 10000) / (pane_alloc.width - 1)));
}


/**
 * Set status bar / scroll bar proportion according to user option
 *   when pane is resized.
 */
static gboolean
nsgtk_paned_size_allocate_event(GtkWidget *widget,
				GtkAllocation *allocation,
				gpointer data)
{
	gtk_paned_set_position(GTK_PANED(widget),
	       (nsoption_int(toolbar_status_size) * allocation->width) / 10000);

	return TRUE;
}


/**
 * handler for gtk destroy signal on window container
 *
 * destroy the browsing context as there is will be nothing to display it now
 */
static void window_destroy(GtkWidget *widget, gpointer data)
{
	struct gui_window *gw = data;

	browser_window_destroy(gw->bw);

	g_object_unref(gw->input_method);

	/* free any existing icon */
	if (gw->icon != NULL) {
		g_object_unref(gw->icon);
		gw->icon = NULL;
	}

	free(gw);
}


static struct browser_window *bw_from_gw(void *data)
{
	struct gui_window *gw = data;
	return gw->bw;
}


static bool get_tool_bar_show(void)
{
	const char *cur_bar_show;

	cur_bar_show = nsoption_charp(bar_show);
	if (cur_bar_show != NULL) {
		if (strcmp(cur_bar_show, "menu/tool") == 0) {
			return true;
		} else if (strcmp(cur_bar_show, "tool") == 0) {
			return true;
		}
	}
	return false;
}


/**
 * Make the throbber advance to next frame.
 *
 * scheduled callback to update the throbber
 *
 * \param p The context passed when scheduled.
 */
static void next_throbber_frame(void *p)
{
	struct gui_window *gw = p;
	nserror res;
	GdkPixbuf *pixbuf;

	gw->throb_frame++; /* advance to next frame */

	res = nsgtk_throbber_get_frame(gw->throb_frame, &pixbuf);
	if (res == NSERROR_BAD_SIZE) {
		gw->throb_frame = 1;
		res = nsgtk_throbber_get_frame(gw->throb_frame, &pixbuf);
	}

	if (res == NSERROR_OK) {
		nsgtk_tab_set_icon(gw->container, pixbuf);
		/* only schedule next frame if there are no errors */
		nsgtk_schedule(THROBBER_FRAME_TIME, next_throbber_frame, p);
	}
}


/**
 * Create and open a gtk container (window or tab) for a browsing context.
 *
 * \param bw The browsing context to create gui_window for.
 * \param existing An existing gui_window, may be NULL
 * \param flags	flags to control the container creation
 * \return gui window, or NULL on error
 *
 * If GW_CREATE_CLONE flag is set existing is non-NULL.
 *
 * Front end's gui_window must include a reference to the
 * browser window passed in the bw param.
 */
static struct gui_window *
gui_window_create(struct browser_window *bw,
		  struct gui_window *existing,
		  gui_window_create_flags flags)
{
	struct gui_window *g; /* what is being created to return */
	bool open_in_background = !(flags & GW_CREATE_FOREGROUND);
	GtkBuilder* tab_builder;
	nserror res;

	res = nsgtk_builder_new_from_resname("tabcontents", &tab_builder);
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Tab contents UI builder init failed");
		return NULL;
	}

	gtk_builder_connect_signals(tab_builder, NULL);

	g = calloc(1, sizeof(*g));
	if (!g) {
		nsgtk_warning("NoMemory", 0);
		g_object_unref(tab_builder);
		return NULL;
	}

	NSLOG(netsurf, INFO, "Creating gui window %p for browser window %p",
	      g, bw);

	g->bw = bw;
	g->mouse.state = 0;
	g->current_pointer = GUI_POINTER_DEFAULT;

	/* attach scaffold */
	if (flags & GW_CREATE_TAB) {
		/* open in new tab, attach to existing scaffold */
		if (existing != NULL) {
			g->scaffold = existing->scaffold;
		} else {
			g->scaffold = nsgtk_current_scaffolding();
		}
	} else {
		/* open in new window, create and attach to scaffold */
		g->scaffold = nsgtk_new_scaffolding(g);
	}
	if (g->scaffold == NULL) {
		nsgtk_warning("NoMemory", 0);
		free(g);
		g_object_unref(tab_builder);
		return NULL;
	}

	/* Construct our primary elements */
	g->container = GTK_WIDGET(gtk_builder_get_object(tab_builder, "tabBox"));
	g->layout = GTK_LAYOUT(gtk_builder_get_object(tab_builder, "layout"));
	g->grid = GTK_WIDGET(gtk_builder_get_object(tab_builder, "tabContents"));
	g->status_bar = GTK_LABEL(gtk_builder_get_object(tab_builder, "status_bar"));
	g->paned = GTK_PANED(gtk_builder_get_object(tab_builder, "hpaned1"));
	g->input_method = gtk_im_multicontext_new();


	/* create toolbar */
	res = nsgtk_toolbar_create(tab_builder, bw_from_gw, g,
				   !!(flags & GW_CREATE_FOCUS_LOCATION),
				   &g->toolbar);
	if (res != NSERROR_OK) {
		free(g);
		g_object_unref(tab_builder);
		return NULL;
	}

	/* local page text search toolbar */
	res = nsgtk_search_create(tab_builder, g->bw, &g->search);
	if (res != NSERROR_OK) {
		free(g);
		g_object_unref(tab_builder);
		return NULL;
	}

	/* set a default favicon */
	g_object_ref(favicon_pixbuf);
	g->icon = favicon_pixbuf;

	/* add new gui window to global list (push_top) */
	if (window_list) {
		window_list->prev = g;
	}
	g->next = window_list;
	g->prev = NULL;
	window_list = g;

	/* set the events we're interested in receiving from the browser's
	 * drawing area.
	 */
	gtk_widget_add_events(GTK_WIDGET(g->layout),
				GDK_EXPOSURE_MASK |
				GDK_LEAVE_NOTIFY_MASK |
				GDK_BUTTON_PRESS_MASK |
				GDK_BUTTON_RELEASE_MASK |
				GDK_POINTER_MOTION_MASK |
				GDK_POINTER_MOTION_HINT_MASK |
				GDK_KEY_PRESS_MASK |
				GDK_KEY_RELEASE_MASK |
				GDK_SCROLL_MASK);
	nsgtk_widget_set_can_focus(GTK_WIDGET(g->layout), TRUE);

	/* set the default background colour of the drawing area to white. */
	nsgtk_widget_override_background_color(GTK_WIDGET(g->layout),
					       GTK_STATE_FLAG_NORMAL,
					       0, 0xffff, 0xffff, 0xffff);

	nsgtk_connect_draw_event(GTK_WIDGET(g->layout),
				G_CALLBACK(nsgtk_window_draw_event), g);

	/* helper macro to conect signals to callbacks */
#define CONNECT(obj, sig, callback, ptr)				\
	g_signal_connect(G_OBJECT(obj), (sig), G_CALLBACK(callback), (ptr))

	/* layout signals */
	CONNECT(g->layout, "motion-notify-event",
			nsgtk_window_motion_notify_event, g);
	CONNECT(g->layout, "button-press-event",
			nsgtk_window_button_press_event, g);
	CONNECT(g->layout, "button-release-event",
			nsgtk_window_button_release_event, g);
	CONNECT(g->layout, "key-press-event",
			nsgtk_window_keypress_event, g);
	CONNECT(g->layout, "key-release-event",
			nsgtk_window_keyrelease_event, g);
	CONNECT(g->layout, "size-allocate",
			nsgtk_window_size_allocate_event, g);
	CONNECT(g->layout, "scroll-event",
			nsgtk_window_scroll_event, g);
	CONNECT(g->layout, "focus-out-event",
			nsgtk_window_focus_out_event, g);

	/* status pane signals */
	CONNECT(g->paned, "size-allocate",
		nsgtk_paned_size_allocate_event, g);

	CONNECT(g->paned, "notify::position",
		nsgtk_paned_notify__position, g);

	/* gtk container destructor */
	CONNECT(g->container, "destroy", window_destroy, g);

	/* input method */
	gtk_im_context_set_client_window(g->input_method,
			nsgtk_layout_get_bin_window(g->layout));
	gtk_im_context_set_use_preedit(g->input_method, FALSE);

	/* input method signals */
	CONNECT(g->input_method, "commit",
		nsgtk_window_input_method_commit, g);

	/* add the tab container to the scaffold notebook */
	nsgtk_tab_add(g, g->container,
		      open_in_background,
		      messages_get("NewTab"), g->icon);

	/* initialy should not be visible */
	nsgtk_search_toggle_visibility(g->search);

	/* set toolbar visibility from user option */
	nsgtk_toolbar_show(g->toolbar, get_tool_bar_show());

	/* safe to drop the reference to the tab_builder as the container is
	 * referenced by the notebook now.
	 */
	g_object_unref(tab_builder);

	/* Finally we need to focus the location bar if requested */
	if (flags & GW_CREATE_FOCUS_LOCATION) {
		if (nsgtk_window_item_activate(g, OPENLOCATION_BUTTON) != NSERROR_OK) {
			NSLOG(netsurf, WARNING, "Unable to focus location input");
		}
	}

	return g;
}


static void gui_window_destroy(struct gui_window *gw)
{
	NSLOG(netsurf, INFO, "gui_window: %p", gw);
	assert(gw != NULL);
	assert(gw->bw != NULL);
	NSLOG(netsurf, INFO, "scaffolding: %p", gw->scaffold);

	/* kill off any throbber that might be running */
	nsgtk_schedule(-1, next_throbber_frame, gw);

	/* remove from window list */
	if (gw->prev) {
		gw->prev->next = gw->next;
	} else {
		window_list = gw->next;
	}

	if (gw->next) {
		gw->next->prev = gw->prev;
	}

	NSLOG(netsurf, INFO, "window list head: %p", window_list);
}


/**
 * favicon setting for gtk gui window.
 *
 * \param gw gtk gui window to set favicon on.
 * \param icon A handle to the new favicon content.
 */
static void
gui_window_set_icon(struct gui_window *gw, struct hlcache_handle *icon)
{
	struct bitmap *icon_bitmap = NULL;

	/* free any existing icon */
	if (gw->icon != NULL) {
		g_object_unref(gw->icon);
		gw->icon = NULL;
	}

	if (icon != NULL) {
		icon_bitmap = content_get_bitmap(icon);
		if (icon_bitmap != NULL) {
			NSLOG(netsurf, INFO, "Using %p bitmap", icon_bitmap);
			gw->icon = nsgdk_pixbuf_get_from_surface(icon_bitmap->surface, 16, 16);
		}
	}

	if (gw->icon == NULL) {
		NSLOG(netsurf, INFO, "Using default favicon");
		g_object_ref(favicon_pixbuf);
		gw->icon = favicon_pixbuf;
	}

	/* only set icon if throbber not running */
	if (gw->throb_frame == 0) {
		nsgtk_tab_set_icon(gw->container, gw->icon);
	}
}


static bool gui_window_get_scroll(struct gui_window *g, int *sx, int *sy)
{
	GtkAdjustment *vadj = nsgtk_layout_get_vadjustment(g->layout);
	GtkAdjustment *hadj = nsgtk_layout_get_hadjustment(g->layout);

	assert(vadj);
	assert(hadj);

	*sy = (int)(gtk_adjustment_get_value(vadj));
	*sx = (int)(gtk_adjustment_get_value(hadj));

	return true;
}


static void nsgtk_redraw_caret(struct gui_window *g)
{
	int sx, sy;

	if (g->careth == 0)
		return;

	gui_window_get_scroll(g, &sx, &sy);

	gtk_widget_queue_draw_area(GTK_WIDGET(g->layout),
			g->caretx - sx, g->carety - sy, 1, g->careth + 1);

}


static void gui_window_remove_caret(struct gui_window *g)
{
	int sx, sy;
	int oh = g->careth;

	if (oh == 0)
		return;

	g->careth = 0;

	gui_window_get_scroll(g, &sx, &sy);

	gtk_widget_queue_draw_area(GTK_WIDGET(g->layout),
			g->caretx - sx, g->carety - sy, 1, oh + 1);

}


/**
 * Invalidates an area of a GTK browser window
 *
 * \param g gui_window
 * \param rect area to redraw or NULL for the entire window area
 * \return NSERROR_OK on success or appropriate error code
 */
static nserror
nsgtk_window_invalidate_area(struct gui_window *g, const struct rect *rect)
{
	int sx, sy;

	if (rect == NULL) {
		gtk_widget_queue_draw(GTK_WIDGET(g->layout));
		return NSERROR_OK;
	}

	if (!browser_window_has_content(g->bw)) {
		return NSERROR_OK;
	}

	gui_window_get_scroll(g, &sx, &sy);

	gtk_widget_queue_draw_area(GTK_WIDGET(g->layout),
				   rect->x0 - sx,
				   rect->y0 - sy,
				   rect->x1 - rect->x0,
				   rect->y1 - rect->y0);

	return NSERROR_OK;
}


static void gui_window_set_status(struct gui_window *g, const char *text)
{
	assert(g);
	assert(g->status_bar);
	gtk_label_set_text(g->status_bar, text);
}


/**
 * Set the scroll position of a gtk browser window.
 *
 * Scrolls the viewport to ensure the specified rectangle of the
 *   content is shown. The GTK implementation scrolls the contents so
 *   the specified point in the content is at the top of the viewport.
 *
 * \param g gui window to scroll
 * \param rect The rectangle to ensure is shown.
 * \return NSERROR_OK on success or apropriate error code.
 */
static nserror
gui_window_set_scroll(struct gui_window *g, const struct rect *rect)
{
	GtkAdjustment *vadj = nsgtk_layout_get_vadjustment(g->layout);
	GtkAdjustment *hadj = nsgtk_layout_get_hadjustment(g->layout);
	gdouble vlower, vpage, vupper, hlower, hpage, hupper;
	gdouble x = (gdouble)rect->x0;
	gdouble y = (gdouble)rect->y0;

	assert(vadj);
	assert(hadj);

	g_object_get(vadj, "page-size", &vpage, "lower", &vlower, "upper", &vupper, NULL);
	g_object_get(hadj, "page-size", &hpage, "lower", &hlower, "upper", &hupper, NULL);

	if (x < hlower) {
		x = hlower;
	}
	if (x > (hupper - hpage)) {
		x = hupper - hpage;
	}
	if (y < vlower) {
		y = vlower;
	}
	if (y > (vupper - vpage)) {
		y = vupper - vpage;
	}

	gtk_adjustment_set_value(vadj, y);
	gtk_adjustment_set_value(hadj, x);

	return NSERROR_OK;
}


static void gui_window_update_extent(struct gui_window *g)
{
	int w, h;

	if (browser_window_get_extents(g->bw, true, &w, &h) == NSERROR_OK) {
		gtk_layout_set_size(g->layout, w, h);
		gtk_widget_queue_resize(g->grid);
	}
}


static void
gui_window_set_pointer(struct gui_window *g, gui_pointer_shape shape)
{
	GdkCursor *cursor = NULL;
	GdkCursorType cursortype;
	bool nullcursor = false;

	if (g->current_pointer == shape)
		return;

	g->current_pointer = shape;

	switch (shape) {
	case GUI_POINTER_POINT:
		cursortype = GDK_HAND2;
		break;
	case GUI_POINTER_CARET:
		cursortype = GDK_XTERM;
		break;
	case GUI_POINTER_UP:
		cursortype = GDK_TOP_SIDE;
		break;
	case GUI_POINTER_DOWN:
		cursortype = GDK_BOTTOM_SIDE;
		break;
	case GUI_POINTER_LEFT:
		cursortype = GDK_LEFT_SIDE;
		break;
	case GUI_POINTER_RIGHT:
		cursortype = GDK_RIGHT_SIDE;
		break;
	case GUI_POINTER_LD:
		cursortype = GDK_BOTTOM_LEFT_CORNER;
		break;
	case GUI_POINTER_RD:
		cursortype = GDK_BOTTOM_RIGHT_CORNER;
		break;
	case GUI_POINTER_LU:
		cursortype = GDK_TOP_LEFT_CORNER;
		break;
	case GUI_POINTER_RU:
		cursortype = GDK_TOP_RIGHT_CORNER;
		break;
	case GUI_POINTER_CROSS:
		cursortype = GDK_CROSS;
		break;
	case GUI_POINTER_MOVE:
		cursortype = GDK_FLEUR;
		break;
	case GUI_POINTER_WAIT:
		cursortype = GDK_WATCH;
		break;
	case GUI_POINTER_HELP:
		cursortype = GDK_QUESTION_ARROW;
		break;
	case GUI_POINTER_MENU:
		cursor = nsgtk_create_menu_cursor();
		nullcursor = true;
		break;
	case GUI_POINTER_PROGRESS:
		/* In reality, this needs to be the funky left_ptr_watch
		 * which we can't do easily yet.
		 */
		cursortype = GDK_WATCH;
		break;
	/* The following we're not sure about */
	case GUI_POINTER_NO_DROP:
	case GUI_POINTER_NOT_ALLOWED:
	case GUI_POINTER_DEFAULT:
	default:
	      nullcursor = true;
	}

	if (!nullcursor)
		cursor = gdk_cursor_new_for_display(
				gtk_widget_get_display(
					GTK_WIDGET(g->layout)),
					cursortype);
	gdk_window_set_cursor(nsgtk_widget_get_window(GTK_WIDGET(g->layout)),
			      cursor);

	if (!nullcursor)
		nsgdk_cursor_unref(cursor);
}


static void
gui_window_place_caret(struct gui_window *g,
		       int x, int y, int height,
		       const struct rect *clip)
{
	nsgtk_redraw_caret(g);

	y += 1;
	height -= 1;

	if (y < clip->y0) {
		height -= clip->y0 - y;
		y = clip->y0;
	}

	if (y + height > clip->y1) {
		height = clip->y1 - y + 1;
	}

	g->caretx = x;
	g->carety = y;
	g->careth = height;

	nsgtk_redraw_caret(g);

	gtk_widget_grab_focus(GTK_WIDGET(g->layout));
}


/**
 * Find the current dimensions of a GTK browser window content area.
 *
 * \param gw The gui window to measure content area of.
 * \param width receives width of window
 * \param height receives height of window
 * \return NSERROR_OK on sucess and width and height updated
 *          else error code.
 */
static nserror
gui_window_get_dimensions(struct gui_window *gw, int *width, int *height)
{
	GtkAllocation alloc;

	/** @todo consider gtk_widget_get_allocated_width() */
	nsgtk_widget_get_allocation(GTK_WIDGET(gw->layout), &alloc);

	*width = alloc.width;
	*height = alloc.height;

	return NSERROR_OK;
}


static void gui_window_start_selection(struct gui_window *g)
{
	gtk_widget_grab_focus(GTK_WIDGET(g->layout));
}


static void
gui_window_create_form_select_menu(struct gui_window *g,
				   struct form_control *control)
{
	intptr_t item;
	struct form_option *option;

	GtkWidget *menu_item;

	/* control->data.select.multiple is true if multiple selections
	 * are allowable.  We ignore this, as the core handles it for us.
	 * Yay. \o/
	 */

	if (select_menu != NULL) {
		gtk_widget_destroy(select_menu);
	}

	select_menu = gtk_menu_new();
	select_menu_control = control;

	item = 0;
	option = form_select_get_option(control, item);
	while (option != NULL) {
		NSLOG(netsurf, INFO, "Item %"PRIdPTR" option %p text %s",
		      item, option, option->text);
		menu_item = gtk_check_menu_item_new_with_label(option->text);
		if (option->selected) {
			gtk_check_menu_item_set_active(
				GTK_CHECK_MENU_ITEM(menu_item), TRUE);
		}

		/*
		 * This casts the item index integer into an integer
		 * the size of a pointer. This allows the callback
		 * parameter to be passed avoiding allocating memory
		 * for a context with a single integer in it.
		 */
		g_signal_connect(menu_item, "toggled",
			G_CALLBACK(nsgtk_select_menu_clicked), (gpointer)item);

		gtk_menu_shell_append(GTK_MENU_SHELL(select_menu), menu_item);

		item++;
		option = form_select_get_option(control, item);
	}

	gtk_widget_show_all(select_menu);

	nsgtk_menu_popup_at_pointer(GTK_MENU(select_menu), NULL);
}


/**
 * GTK window UI callback when core needs a file selection gadget
 *
 * \param g The gui window on which the gadget has been requested
 */
static void
gui_window_file_gadget_open(struct gui_window *g,
			    struct hlcache_handle *hl,
			    struct form_control *gadget)
{
	GtkWidget *dialog;

	dialog = gtk_file_chooser_dialog_new("Select File",
			nsgtk_scaffolding_window(g->scaffold),
			GTK_FILE_CHOOSER_ACTION_OPEN,
			NSGTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
			NSGTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
			NULL);

	NSLOG(netsurf, INFO, "*** open dialog: %p", dialog);

	int ret = gtk_dialog_run(GTK_DIALOG(dialog));
	NSLOG(netsurf, INFO, "*** return value: %d", ret);
	if (ret == GTK_RESPONSE_ACCEPT) {
		char *filename;

		filename = gtk_file_chooser_get_filename(
			GTK_FILE_CHOOSER(dialog));

		browser_window_set_gadget_filename(g->bw, gadget, filename);

		g_free(filename);
	}

	gtk_widget_destroy(dialog);
}


/**
 * handle throbber changing state
 */
static nserror throbber(struct gui_window *gw, bool active)
{
	nsgtk_toolbar_throbber(gw->toolbar, active);
	nsgtk_scaffolding_throbber(gw, active);
	if (active) {
		nsgtk_schedule(THROBBER_FRAME_TIME, next_throbber_frame, gw);
	} else {
		nsgtk_schedule(-1, next_throbber_frame, gw);
		gw->throb_frame = 0;
		/* set tab back to favicon */
		nsgtk_tab_set_icon(gw->container, gw->icon);
	}
	return NSERROR_OK;
}


/**
 * handle page info changing
 */
static nserror page_info_change(struct gui_window *gw)
{
	nsgtk_toolbar_page_info_change(gw->toolbar);
	return NSERROR_OK;
}

/**
 * GTK window UI callback to process miscellaneous events
 *
 * \param gw The window receiving the event.
 * \param event The event code.
 * \return NSERROR_OK when processed ok
 */
static nserror
gui_window_event(struct gui_window *gw, enum gui_window_event event)
{
	switch (event) {
	case GW_EVENT_UPDATE_EXTENT:
		gui_window_update_extent(gw);
		break;

	case GW_EVENT_REMOVE_CARET:
		gui_window_remove_caret(gw);
		break;

	case GW_EVENT_START_SELECTION:
		gui_window_start_selection(gw);
		break;

	case GW_EVENT_START_THROBBER:
		throbber(gw, true);
		break;

	case GW_EVENT_STOP_THROBBER:
		throbber(gw, false);
		break;

	case GW_EVENT_PAGE_INFO_CHANGE:
		page_info_change(gw);
		break;

	default:
		break;
	}
	return NSERROR_OK;
}


/**
 * GTK window UI callback when core changes the current url
 *
 * \param gw The gui window on which the url has been set.
 * \param url The new url.
 */
static nserror gui_window_set_url(struct gui_window *gw, nsurl *url)
{
	return nsgtk_toolbar_set_url(gw->toolbar, url);
}


/**
 * GTK window UI callback when core changes the current title
 *
 * \param gw The gui window on which the url has been set.
 * \param url The new url.
 */
static void gui_window_set_title(struct gui_window *gw, const char *title)
{

	if ((title != NULL) && (title[0] != '\0')) {
		nsgtk_tab_set_title(gw->container, title);
	}
	nsgtk_scaffolding_set_title(gw, title);
}


/**
 * GTK UI callback when search provider details are updated.
 *
 * \param name The providers name.
 * \param bitmap The bitmap representing the provider.
 * \return NSERROR_OK on success else error code.
 */
static nserror
gui_search_web_provider_update(const char *name, struct bitmap *bitmap)
{
	struct gui_window *gw;
	GdkPixbuf *pixbuf = NULL;

	if (bitmap != NULL) {
		pixbuf = nsgdk_pixbuf_get_from_surface(bitmap->surface, 32, 32);
	}

	for (gw = window_list; gw != NULL; gw = gw->next) {
		nsgtk_toolbar_set_websearch_image(gw->toolbar, pixbuf);
	}

	if (pixbuf != NULL) {
		g_object_unref(pixbuf);
	}

	return NSERROR_OK;
}


/**
 * GTK frontend web search operation table
 */
static struct gui_search_web_table search_web_table = {
	.provider_update = gui_search_web_provider_update,
};

struct gui_search_web_table *nsgtk_search_web_table = &search_web_table;


/**
 * GTK frontend browser window operation table
 */
static struct gui_window_table window_table = {
	.create = gui_window_create,
	.destroy = gui_window_destroy,
	.invalidate = nsgtk_window_invalidate_area,
	.get_scroll = gui_window_get_scroll,
	.set_scroll = gui_window_set_scroll,
	.get_dimensions = gui_window_get_dimensions,
	.event = gui_window_event,

	.set_icon = gui_window_set_icon,
	.set_title = gui_window_set_title,
	.set_status = gui_window_set_status,
	.set_pointer = gui_window_set_pointer,
	.place_caret = gui_window_place_caret,
	.create_form_select_menu = gui_window_create_form_select_menu,
	.file_gadget_open = gui_window_file_gadget_open,
	.set_url = gui_window_set_url,


};

struct gui_window_table *nsgtk_window_table = &window_table;


/* exported interface documented in window.h */
struct nsgtk_scaffolding *nsgtk_get_scaffold(struct gui_window *g)
{
	return g->scaffold;
}


/* exported interface documented in window.h */
struct browser_window *nsgtk_get_browser_window(struct gui_window *g)
{
	return g->bw;
}


/* exported interface documented in window.h */
GtkLayout *nsgtk_window_get_layout(struct gui_window *g)
{
	return g->layout;
}


/* exported interface documented in window.h */
nserror
nsgtk_window_search_toggle(struct gui_window *gw)
{
	return nsgtk_search_toggle_visibility(gw->search);
}


/* exported interface documented in window.h */
nserror
nsgtk_window_item_activate(struct gui_window *gw, nsgtk_toolbar_button itemid)
{
	return nsgtk_toolbar_item_activate(gw->toolbar, itemid);
}


/* exported interface documented in window.h */
void nsgtk_window_destroy_browser(struct gui_window *gw)
{
	/* remove tab */
	gtk_widget_destroy(gw->container);
}


/* exported interface documented in window.h */
nserror nsgtk_window_update_all(void)
{
	struct gui_window *gw;
	for (gw = window_list; gw != NULL; gw = gw->next) {
		nsgtk_tab_options_changed(nsgtk_scaffolding_notebook(gw->scaffold));
		nsgtk_toolbar_restyle(gw->toolbar);
		nsgtk_search_restyle(gw->search);
		browser_window_schedule_reformat(gw->bw);
	}
	return NSERROR_OK;
}


/* exported interface documented in window.h */
nserror nsgtk_window_toolbar_show(struct nsgtk_scaffolding *gs, bool show)
{
	struct gui_window *gw;
	for (gw = window_list; gw != NULL; gw = gw->next) {
		if (gw->scaffold == gs) {
			nsgtk_toolbar_show(gw->toolbar, show);
		}
	}
	return NSERROR_OK;
}


/* exported interface documented in window.h */
nserror nsgtk_window_toolbar_update(void)
{
	struct gui_window *gw;
	for (gw = window_list; gw != NULL; gw = gw->next) {
		nsgtk_toolbar_update(gw->toolbar);

	}
	return NSERROR_OK;
}

/* exported interface documented in window.h */
nserror nsgtk_window_position_page_info(struct gui_window *gw,
					struct nsgtk_pi_window *win)
{
	return nsgtk_toolbar_position_page_info(gw->toolbar, win);
}

/* exported interface documented in window.h */
nserror nsgtk_window_position_local_history(struct gui_window *gw)
{
	return nsgtk_toolbar_position_local_history(gw->toolbar);
}
