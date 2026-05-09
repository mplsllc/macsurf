/*
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
 * GTK generic core window interface.
 *
 * Provides interface for core renderers to the gtk toolkit drawable area.
 * \todo should the interface really be called coredrawable?
 *
 * This module is an object that must be encapsulated. Client users
 * should embed a struct nsgtk_corewindow at the beginning of their
 * context for this display surface, fill in relevant data and then
 * call nsgtk_corewindow_init()
 *
 * The nsgtk core window structure requires the drawing area and
 * scrollable widgets are present and the callback for draw, key and
 * mouse operations.
 */

#include <assert.h>
#include <string.h>
#include <math.h>
#include <gtk/gtk.h>

#include "utils/log.h"
#include "utils/messages.h"
#include "utils/utf8.h"
#include "netsurf/types.h"
#include "netsurf/keypress.h"
#include "netsurf/mouse.h"

#include "gtk/compat.h"
#include "gtk/gui.h" /* just for gtk_gui_gdkkey_to_nskey */
#include "gtk/plotters.h"
#include "gtk/corewindow.h"

/**
 * Convert GDK mouse event to netsurf mouse state
 *
 * \param event The GDK mouse event to convert.
 * \return The netsurf mouse state.
 */
static browser_mouse_state nsgtk_cw_gdkbutton_to_nsstate(GdkEventButton *event)
{
	browser_mouse_state ms;

	if (event->type == GDK_2BUTTON_PRESS) {
		ms = BROWSER_MOUSE_DOUBLE_CLICK;
	} else {
		ms = BROWSER_MOUSE_HOVER;
	}

	/* button state */
	switch (event->button) {
	case 1:
		ms |= BROWSER_MOUSE_PRESS_1;
		break;

	case 2:
		ms |= BROWSER_MOUSE_PRESS_2;
		break;
	}

	/* Handle the modifiers too */
	if (event->state & GDK_SHIFT_MASK) {
		ms |= BROWSER_MOUSE_MOD_1;
	}

	if (event->state & GDK_CONTROL_MASK) {
		ms |= BROWSER_MOUSE_MOD_2;
	}

	if (event->state & GDK_MOD1_MASK) {
		/* usually alt */
		ms |= BROWSER_MOUSE_MOD_3;
	}

	return ms;
}


/**
 * gtk event on mouse button press.
 *
 * Service gtk event for a mouse button transition to pressed from
 * released state.
 *
 * \param widget The gtk widget the event occurred for.
 * \param event The event that occurred.
 * \param g The context pointer passed when the event was registered.
 */
static gboolean
nsgtk_cw_button_press_event(GtkWidget *widget,
			    GdkEventButton *event,
			    gpointer g)
{
	struct nsgtk_corewindow *nsgtk_cw = (struct nsgtk_corewindow *)g;
	struct nsgtk_corewindow_mouse *mouse = &nsgtk_cw->mouse_state;

	gtk_im_context_reset(nsgtk_cw->input_method);
	gtk_widget_grab_focus(GTK_WIDGET(nsgtk_cw->drawing_area));

	/* record event information for potentially starting a drag. */
	mouse->pressed_x = mouse->last_x = event->x;
	mouse->pressed_y = mouse->last_y = event->y;
	mouse->pressed = true;

	mouse->state = nsgtk_cw_gdkbutton_to_nsstate(event);

	nsgtk_cw->mouse(nsgtk_cw, mouse->state, event->x, event->y);

	return TRUE;
}


/**
 * gtk event on mouse button release.
 *
 * Service gtk event for a mouse button transition from pressed to
 * released state.
 *
 * \param widget The gtk widget the event occurred for.
 * \param event The event that occurred.
 * \param g The context pointer passed when the event was registered.
 */
static gboolean
nsgtk_cw_button_release_event(GtkWidget *widget,
			      GdkEventButton *event,
			      gpointer g)
{
	struct nsgtk_corewindow *nsgtk_cw = (struct nsgtk_corewindow *)g;
	struct nsgtk_corewindow_mouse *mouse = &nsgtk_cw->mouse_state;
	bool was_drag = false;

	/* only button 1 clicks are considered double clicks. If the
	 * mouse state is PRESS then we are waiting for a release to
	 * emit a click event, otherwise just reset the state to nothing.
	 */
	if (mouse->state & BROWSER_MOUSE_DOUBLE_CLICK) {
		if (mouse->state & BROWSER_MOUSE_PRESS_1) {
			mouse->state ^= BROWSER_MOUSE_PRESS_1 |
					BROWSER_MOUSE_CLICK_1;
		} else if (mouse->state & BROWSER_MOUSE_PRESS_2) {
			mouse->state ^= (BROWSER_MOUSE_PRESS_2 |
					BROWSER_MOUSE_CLICK_2 |
					BROWSER_MOUSE_DOUBLE_CLICK);
		}
	} else if (mouse->state & BROWSER_MOUSE_PRESS_1) {
		mouse->state ^= (BROWSER_MOUSE_PRESS_1 |
				 BROWSER_MOUSE_CLICK_1);
	} else if (mouse->state & BROWSER_MOUSE_PRESS_2) {
		mouse->state ^= (BROWSER_MOUSE_PRESS_2 |
				 BROWSER_MOUSE_CLICK_2);
	} else if (mouse->state & BROWSER_MOUSE_HOLDING_1) {
		mouse->state ^= (BROWSER_MOUSE_HOLDING_1 |
				 BROWSER_MOUSE_DRAG_ON);
		was_drag = true;
	} else if (mouse->state & BROWSER_MOUSE_HOLDING_2) {
		mouse->state ^= (BROWSER_MOUSE_HOLDING_2 |
				 BROWSER_MOUSE_DRAG_ON);
		was_drag = true;
	}

	/* Handle modifiers being removed */
	if ((mouse->state & BROWSER_MOUSE_MOD_1) &&
	    !(event->state & GDK_SHIFT_MASK)) {
		mouse->state ^= BROWSER_MOUSE_MOD_1;
	}
	if ((mouse->state & BROWSER_MOUSE_MOD_2) &&
	    !(event->state & GDK_CONTROL_MASK)) {
		mouse->state ^= BROWSER_MOUSE_MOD_2;
	}
	if ((mouse->state & BROWSER_MOUSE_MOD_3) &&
	    !(event->state & GDK_MOD1_MASK)) {
		mouse->state ^= BROWSER_MOUSE_MOD_3;
	}

	/* end drag with modifiers */
	if (was_drag && (mouse->state & (
			BROWSER_MOUSE_MOD_1 |
			BROWSER_MOUSE_MOD_2 |
			BROWSER_MOUSE_MOD_3))) {
		mouse->state = BROWSER_MOUSE_HOVER;
	}

	nsgtk_cw->mouse(nsgtk_cw, mouse->state, event->x, event->y);

	mouse->pressed = false;

	return TRUE;
}


/**
 * gtk event on mouse movement.
 *
 * Service gtk motion-notify-event for mouse movement above a widget.
 *
 * \param widget The gtk widget the event occurred for.
 * \param event The motion event that occurred.
 * \param g The context pointer passed when the event was registered.
 */
static gboolean
nsgtk_cw_motion_notify_event(GtkWidget *widget,
			     GdkEventMotion *event,
			     gpointer g)
{
	struct nsgtk_corewindow *nsgtk_cw = (struct nsgtk_corewindow *)g;
	struct nsgtk_corewindow_mouse *mouse = &nsgtk_cw->mouse_state;

	if (mouse->pressed == false) {
		nsgtk_cw->mouse(nsgtk_cw,
				BROWSER_MOUSE_HOVER,
				event->x,
				event->y);
		return TRUE;
	}

	if ((fabs(event->x - mouse->last_x) < 5.0) &&
	    (fabs(event->y - mouse->last_y) < 5.0)) {
		/* Mouse hasn't moved far enough from press coordinate
		 * for this to be considered a drag.
		 */
		return FALSE;
	}

	/* This is a drag, ensure it's always treated as such, even if
	 * we drag back over the press location.
	 */
	mouse->last_x = INT_MIN;
	mouse->last_y = INT_MIN;


	if (mouse->state & BROWSER_MOUSE_PRESS_1) {
		/* Start button 1 drag */
		nsgtk_cw->mouse(nsgtk_cw,
				BROWSER_MOUSE_DRAG_1,
				mouse->pressed_x,
				mouse->pressed_y);

		/* Replace PRESS with HOLDING and declare drag in progress */
		mouse->state ^= (BROWSER_MOUSE_PRESS_1 |
				 BROWSER_MOUSE_HOLDING_1);
		mouse->state |= BROWSER_MOUSE_DRAG_ON;

	} else if (mouse->state & BROWSER_MOUSE_PRESS_2) {
		/* Start button 2s drag */
		nsgtk_cw->mouse(nsgtk_cw,
				BROWSER_MOUSE_DRAG_2,
				mouse->pressed_x,
				mouse->pressed_y);

		/* Replace PRESS with HOLDING and declare drag in progress */
		mouse->state ^= (BROWSER_MOUSE_PRESS_2 |
				 BROWSER_MOUSE_HOLDING_2);
		mouse->state |= BROWSER_MOUSE_DRAG_ON;

	} else {
		/* continue drag */

		/* Handle modifiers being removed */
		if ((mouse->state & BROWSER_MOUSE_MOD_1) &&
		    !(event->state & GDK_SHIFT_MASK)) {
			mouse->state ^= BROWSER_MOUSE_MOD_1;
		}
		if ((mouse->state & BROWSER_MOUSE_MOD_2) &&
		    !(event->state & GDK_CONTROL_MASK)) {
			mouse->state ^= BROWSER_MOUSE_MOD_2;
		}
		if ((mouse->state & BROWSER_MOUSE_MOD_3) &&
		    !(event->state & GDK_MOD1_MASK)) {
			mouse->state ^= BROWSER_MOUSE_MOD_3;
		}

		if (mouse->state &
		    (BROWSER_MOUSE_HOLDING_1 | BROWSER_MOUSE_HOLDING_2)) {
			nsgtk_cw->mouse(nsgtk_cw,
					mouse->state,
					event->x, event->y);
		}
	}

	return TRUE;
}


/**
 * Deal with keypress events not handled buy input method or callback
 *
 * \param nsgtk_cw nsgtk core window key event happened in.
 * \param nskey The netsurf keycode of the event.
 * \return NSERROR_OK on success otherwise an error code.
 */
static nserror nsgtk_cw_key(struct nsgtk_corewindow *nsgtk_cw, uint32_t nskey)
{
	double value = 0;
	GtkAdjustment *vscroll;
	GtkAdjustment *hscroll;
	GtkAdjustment *scroll = NULL;
	gdouble hpage, vpage;

	vscroll = gtk_scrolled_window_get_vadjustment(nsgtk_cw->scrolled);
	hscroll = gtk_scrolled_window_get_hadjustment(nsgtk_cw->scrolled);
	g_object_get(vscroll, "page-size", &vpage, NULL);
	g_object_get(hscroll, "page-size", &hpage, NULL);

	switch(nskey) {
	case NS_KEY_TEXT_START:
		scroll = vscroll;
		value = nsgtk_adjustment_get_lower(scroll);
		break;

	case NS_KEY_TEXT_END:
		scroll = vscroll;
		value = nsgtk_adjustment_get_upper(scroll) - vpage;
		if (value < nsgtk_adjustment_get_lower(scroll))
			value = nsgtk_adjustment_get_lower(scroll);
		break;

	case NS_KEY_LEFT:
		scroll = hscroll;
		value = gtk_adjustment_get_value(scroll) -
			nsgtk_adjustment_get_step_increment(scroll);
		if (value < nsgtk_adjustment_get_lower(scroll))
			value = nsgtk_adjustment_get_lower(scroll);
		break;

	case NS_KEY_RIGHT:
		scroll = hscroll;
		value = gtk_adjustment_get_value(scroll) +
			nsgtk_adjustment_get_step_increment(scroll);
		if (value > nsgtk_adjustment_get_upper(scroll) - hpage)
			value = nsgtk_adjustment_get_upper(scroll) - hpage;
		break;
	case NS_KEY_UP:
		scroll = vscroll;
		value = gtk_adjustment_get_value(scroll) -
			nsgtk_adjustment_get_step_increment(scroll);
		if (value < nsgtk_adjustment_get_lower(scroll))
			value = nsgtk_adjustment_get_lower(scroll);
		break;

	case NS_KEY_DOWN:
		scroll = vscroll;
		value = gtk_adjustment_get_value(scroll) +
			nsgtk_adjustment_get_step_increment(scroll);
		if (value > nsgtk_adjustment_get_upper(scroll) - vpage)
			value = nsgtk_adjustment_get_upper(scroll) - vpage;
		break;

	case NS_KEY_PAGE_UP:
		scroll = vscroll;
		value = gtk_adjustment_get_value(scroll) -
			nsgtk_adjustment_get_page_increment(scroll);

		if (value < nsgtk_adjustment_get_lower(scroll))
			value = nsgtk_adjustment_get_lower(scroll);

		break;

	case NS_KEY_PAGE_DOWN:
		scroll = vscroll;
		value = gtk_adjustment_get_value(scroll) +
			nsgtk_adjustment_get_page_increment(scroll);

		if (value > nsgtk_adjustment_get_upper(scroll) - vpage)
			value = nsgtk_adjustment_get_upper(scroll) - vpage;
		break;

	}

	if (scroll != NULL) {
		gtk_adjustment_set_value(scroll, value);
	}

	return NSERROR_OK;
}

/**
 * gtk event on key press.
 *
 * Service gtk key-press-event for key transition on a widget from
 * released to pressed.
 *
 * \param widget The gtk widget the event occurred for.
 * \param event The event that occurred.
 * \param g The context pointer passed when the event was registered.
 */
static gboolean
nsgtk_cw_keypress_event(GtkWidget *widget, GdkEventKey *event, gpointer g)
{
	struct nsgtk_corewindow *nsgtk_cw = (struct nsgtk_corewindow *)g;
	nserror res;
	uint32_t nskey;

	/* check to see if gtk input method swallowed the keypress */
	if (gtk_im_context_filter_keypress(nsgtk_cw->input_method, event)) {
		return TRUE;
	}

	/* convert gtk event to nskey */
	nskey = gtk_gui_gdkkey_to_nskey(event);

	/* attempt to handle keypress in caller */
	res = nsgtk_cw->key(nsgtk_cw, nskey);
	if (res == NSERROR_OK) {
		return TRUE;
	} else if (res != NSERROR_NOT_IMPLEMENTED) {
		NSLOG(netsurf, INFO, "%s", messages_get_errorcode(res));
		return FALSE;
	}

	/* deal with unprocessed keypress */
	res = nsgtk_cw_key(nsgtk_cw, nskey);
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "%s", messages_get_errorcode(res));
		return FALSE;
	}
	return TRUE;
}


/**
 * gtk event on key release.
 *
 * Service gtk key-release-event for key transition on a widget from
 * pressed to released.
 *
 * \param widget The gtk widget the event occurred for.
 * \param event The event that occurred.
 * \param g The context pointer passed when the event was registered.
 */
static gboolean
nsgtk_cw_keyrelease_event(GtkWidget *widget, GdkEventKey *event, gpointer g)
{
	struct nsgtk_corewindow *nsgtk_cw = (struct nsgtk_corewindow *)g;

	return gtk_im_context_filter_keypress(nsgtk_cw->input_method, event);
}


/**
 * gtk event handler for input method commit.
 *
 * Service gtk commit for input method commit action.
 *
 * \param ctx The gtk input method context the event occurred for.
 * \param str The resulting string from the input method.
 * \param g The context pointer passed when the event was registered.
 */
static void
nsgtk_cw_input_method_commit(GtkIMContext *ctx, const gchar *str, gpointer g)
{
	struct nsgtk_corewindow *nsgtk_cw = (struct nsgtk_corewindow *)g;
	size_t len;
	size_t offset = 0;
	uint32_t nskey;

	len = strlen(str);

	while (offset < len) {
		nskey = utf8_to_ucs4(str + offset, len - offset);

		nsgtk_cw->key(nsgtk_cw, nskey);

		offset = utf8_next(str, len, offset);
	}
}


#if GTK_CHECK_VERSION(3,0,0)


/**
 * handler for gtk draw event on a nsgtk core window for GTK 3
 *
 * \param widget The GTK widget to redraw.
 * \param cr The cairo drawing context of the widget
 * \param data The context pointer passed when the event was registered.
 * \return FALSE indicating no error.
 */
static gboolean
nsgtk_cw_draw_event(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	struct nsgtk_corewindow *nsgtk_cw = (struct nsgtk_corewindow *)data;
	double x1;
	double y1;
	double x2;
	double y2;
	struct rect clip;

	current_cr = cr;

	cairo_clip_extents(cr, &x1, &y1, &x2, &y2);

	clip.x0 = x1;
	clip.y0 = y1;
	clip.x1 = x2;
	clip.y1 = y2;

	nsgtk_cw->draw(nsgtk_cw, &clip);

	return FALSE;
}

#else


/**
 * handler for gtk draw event on a nsgtk core window for GTK 2
 *
 * \param widget The GTK widget to redraw.
 * \param event The GDK expose event
 * \param g The context pointer passed when the event was registered.
 * \return FALSE indicating no error.
 */
static gboolean
nsgtk_cw_draw_event(GtkWidget *widget,
		    GdkEventExpose *event,
		    gpointer g)
{
	struct nsgtk_corewindow *nsgtk_cw = (struct nsgtk_corewindow *)g;
	struct rect clip;

	clip.x0 = event->area.x;
	clip.y0 = event->area.y;
	clip.x1 = event->area.x + event->area.width;
	clip.y1 = event->area.y + event->area.height;

	current_cr = gdk_cairo_create(nsgtk_widget_get_window(widget));

	nsgtk_cw->draw(nsgtk_cw, &clip);

	cairo_destroy(current_cr);

	return FALSE;
}

#endif


/**
 * callback from core to request an invalidation of a GTK core window area.
 *
 * The specified area of the window should now be considered
 *  out of date. If the area is NULL the entire window must be
 *  invalidated.
 *
 * \param[in] cw The core window to invalidate.
 * \param[in] rect area to redraw or NULL for the entire window area.
 * \return NSERROR_OK on success or appropriate error code.
 */
static nserror
nsgtk_cw_invalidate_area(struct core_window *cw, const struct rect *rect)
{
	struct nsgtk_corewindow *nsgtk_cw = (struct nsgtk_corewindow *)cw;

	if (rect == NULL) {
		gtk_widget_queue_draw(GTK_WIDGET(nsgtk_cw->drawing_area));
		return NSERROR_OK;
	}

	gtk_widget_queue_draw_area(GTK_WIDGET(nsgtk_cw->drawing_area),
				   rect->x0,
				   rect->y0,
				   rect->x1 - rect->x0,
				   rect->y1 - rect->y0);

	return NSERROR_OK;
}


/**
 * update window size core window callback
 *
 * \param cw core window handle.
 * \param width New widget width.
 * \param height New widget height.
 */
static nserror
nsgtk_cw_update_size(struct core_window *cw, int width, int height)
{
	struct nsgtk_corewindow *nsgtk_cw = (struct nsgtk_corewindow *)cw;

	gtk_widget_set_size_request(GTK_WIDGET(nsgtk_cw->drawing_area),
				    width, height);

	return NSERROR_OK;
}


/**
 * scroll window core window callback
 *
 * \param cw core window handle.
 * \param r rectangle that needs scrolling.
 */
static nserror
nsgtk_cw_set_scroll(struct core_window *cw, int x, int y)
{
	struct nsgtk_corewindow *nsgtk_cw = (struct nsgtk_corewindow *)cw;

	if (nsgtk_cw->scrolled != NULL) {
		GtkAdjustment *vadj;
		GtkAdjustment *hadj;

		vadj = gtk_scrolled_window_get_vadjustment(nsgtk_cw->scrolled);
		hadj = gtk_scrolled_window_get_hadjustment(nsgtk_cw->scrolled);

		assert(vadj != NULL);
		assert(hadj != NULL);

		gtk_adjustment_set_value(vadj, y);
		gtk_adjustment_set_value(hadj, x);
	}
	return NSERROR_OK;
}


/**
 * scroll window core window callback
 *
 * \param cw core window handle.
 * \param r rectangle that needs scrolling.
 */
static nserror
nsgtk_cw_get_scroll(const struct core_window *cw, int *x, int *y)
{
	struct nsgtk_corewindow *nsgtk_cw = (struct nsgtk_corewindow *)cw;

	if (nsgtk_cw->scrolled != NULL) {
		GtkAdjustment *vadj;
		GtkAdjustment *hadj;

		vadj = gtk_scrolled_window_get_vadjustment(nsgtk_cw->scrolled);
		hadj = gtk_scrolled_window_get_hadjustment(nsgtk_cw->scrolled);

		assert(vadj != NULL);
		assert(hadj != NULL);

		*y = (int)(gtk_adjustment_get_value(vadj));
		*x = (int)(gtk_adjustment_get_value(hadj));
	} else {
		*x = 0;
		*y = 0;
	}
	return NSERROR_OK;
}


/**
 * Callback from the core to obtain the window viewport dimensions
 *
 * \param[in] cw the core window object
 * \param[out] width to be set to viewport width in px
 * \param[out] height to be set to viewport height in px
 */
static nserror
nsgtk_cw_get_window_dimensions(const struct core_window *cw,
		int *width, int *height)
{
	struct nsgtk_corewindow *nsgtk_cw = (struct nsgtk_corewindow *)cw;
	if (nsgtk_cw->scrolled != NULL) {
		GtkAdjustment *vadj;
		GtkAdjustment *hadj;
		gdouble page;

		hadj = gtk_scrolled_window_get_hadjustment(nsgtk_cw->scrolled);
		g_object_get(hadj, "page-size", &page, NULL);
		*width = page;

		vadj = gtk_scrolled_window_get_vadjustment(nsgtk_cw->scrolled);
		g_object_get(vadj, "page-size", &page, NULL);
		*height = page;
	} else {
		GtkAllocation allocation;
		gtk_widget_get_allocation(GTK_WIDGET(nsgtk_cw->drawing_area),
					  &allocation);
		*width = allocation.width;
		*height = allocation.height;
	}
	return NSERROR_OK;
}


/**
 * update window drag status core window callback
 *
 * \param cw core window handle.
 * \param ds The new drag status.
 */
static nserror
nsgtk_cw_drag_status(struct core_window *cw, core_window_drag_status ds)
{
	struct nsgtk_corewindow *nsgtk_cw = (struct nsgtk_corewindow *)cw;
	nsgtk_cw->drag_status = ds;

	return NSERROR_OK;
}


/**
 * core window callback table for nsgtk
 */
static struct core_window_table nsgtk_cw_cb_table = {
	.invalidate = nsgtk_cw_invalidate_area,
	.set_extent = nsgtk_cw_update_size,
	.set_scroll = nsgtk_cw_set_scroll,
	.get_scroll = nsgtk_cw_get_scroll,
	.get_dimensions = nsgtk_cw_get_window_dimensions,
	.drag_status = nsgtk_cw_drag_status
};

struct core_window_table *nsgtk_core_window_table = &nsgtk_cw_cb_table;

/* exported function documented gtk/corewindow.h */
nserror nsgtk_corewindow_init(struct nsgtk_corewindow *nsgtk_cw)
{
	nsgtk_cw->drag_status = CORE_WINDOW_DRAG_NONE;

	/* input method setup */
	nsgtk_cw->input_method = gtk_im_multicontext_new();
	gtk_im_context_set_client_window(nsgtk_cw->input_method,
		gtk_widget_get_parent_window(GTK_WIDGET(nsgtk_cw->drawing_area)));
	gtk_im_context_set_use_preedit(nsgtk_cw->input_method, FALSE);

	g_signal_connect(G_OBJECT(nsgtk_cw->input_method),
			 "commit",
			 G_CALLBACK(nsgtk_cw_input_method_commit),
			 nsgtk_cw);

	nsgtk_connect_draw_event(GTK_WIDGET(nsgtk_cw->drawing_area),
				 G_CALLBACK(nsgtk_cw_draw_event),
				 nsgtk_cw);

	g_signal_connect(G_OBJECT(nsgtk_cw->drawing_area),
			 "button-press-event",
			 G_CALLBACK(nsgtk_cw_button_press_event),
			 nsgtk_cw);

	g_signal_connect(G_OBJECT(nsgtk_cw->drawing_area),
			 "button-release-event",
			 G_CALLBACK(nsgtk_cw_button_release_event),
			 nsgtk_cw);

	g_signal_connect(G_OBJECT(nsgtk_cw->drawing_area),
			 "motion-notify-event",
			 G_CALLBACK(nsgtk_cw_motion_notify_event),
			 nsgtk_cw);

	g_signal_connect(G_OBJECT(nsgtk_cw->drawing_area),
			 "key-press-event",
			 G_CALLBACK(nsgtk_cw_keypress_event),
			 nsgtk_cw);

	g_signal_connect(G_OBJECT(nsgtk_cw->drawing_area),
			 "key-release-event",
			 G_CALLBACK(nsgtk_cw_keyrelease_event),
			 nsgtk_cw);

	nsgtk_widget_override_background_color(
		GTK_WIDGET(nsgtk_cw->drawing_area),
		GTK_STATE_FLAG_NORMAL,
		0, 0xffff, 0xffff, 0xffff);

	return NSERROR_OK;
}

/* exported interface documented in gtk/corewindow.h */
nserror nsgtk_corewindow_fini(struct nsgtk_corewindow *nsgtk_cw)
{
	g_object_unref(nsgtk_cw->input_method);

	return NSERROR_OK;
}
