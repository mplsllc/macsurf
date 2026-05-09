/*
 * Copyright 2010 Rob Kendrick <rjek@netsurf-browser.org>
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
 * Compatibility functions for older GTK versions implementation
 */

#include <stdint.h>

#include "gtk/compat.h"

#ifdef _SEXY_ICON_ENTRY_H_
#include "gtk/sexy_icon_entry.c"

/*
 * exported interface documented in gtk/compat.h
 *
 * Only required for the lib sexy interface before 2.16
 */
GtkStateType nsgtk_widget_get_state(GtkWidget *widget)
{
#if GTK_CHECK_VERSION(2,18,0)
	return gtk_widget_get_state(widget);
#else
	return GTK_WIDGET_STATE(widget);
#endif
}


#endif

void nsgtk_widget_set_can_focus(GtkWidget *widget, gboolean can_focus)
{
#if GTK_CHECK_VERSION(2,22,0)
	gtk_widget_set_can_focus(widget, can_focus);
#else
	if (can_focus == TRUE)
		GTK_WIDGET_SET_FLAGS(widget, GTK_CAN_FOCUS);
	else
		GTK_WIDGET_UNSET_FLAGS(widget, GTK_CAN_FOCUS);
#endif
}

gboolean nsgtk_widget_has_focus(GtkWidget *widget)
{
#if GTK_CHECK_VERSION(2,20,0)
	return gtk_widget_has_focus(widget);
#else
	return GTK_WIDGET_HAS_FOCUS(widget);
#endif
}

gboolean nsgtk_widget_get_visible(GtkWidget *widget)
{
#if GTK_CHECK_VERSION(2,20,0)
	return gtk_widget_get_visible(widget);
#else
	return GTK_WIDGET_VISIBLE(widget);
#endif
}

gboolean nsgtk_widget_get_realized(GtkWidget *widget)
{
#if GTK_CHECK_VERSION(2,20,0)
	return gtk_widget_get_realized(widget);
#else
	return GTK_WIDGET_REALIZED(widget);
#endif
}

gboolean nsgtk_widget_get_mapped(GtkWidget *widget)
{
#if GTK_CHECK_VERSION(2,20,0)
	return gtk_widget_get_mapped(widget);
#else
	return GTK_WIDGET_MAPPED(widget);
#endif
}

gboolean nsgtk_widget_is_drawable(GtkWidget *widget)
{
#if GTK_CHECK_VERSION(2,18,0)
	return gtk_widget_is_drawable(widget);
#else
	return GTK_WIDGET_DRAWABLE(widget);
#endif
}

void nsgtk_dialog_set_has_separator(GtkDialog *dialog, gboolean setting)
{
#if GTK_CHECK_VERSION(2,21,8)
	/* Deprecated */
#else
	gtk_dialog_set_has_separator(dialog, setting);
#endif
}

GtkWidget *nsgtk_combo_box_text_new(void)
{
#if GTK_CHECK_VERSION(2,24,0)
	return gtk_combo_box_text_new();
#else
	return gtk_combo_box_new_text();
#endif
}

void nsgtk_combo_box_text_append_text(GtkWidget *combo_box, const gchar *text)
{
#if GTK_CHECK_VERSION(2,24,0)
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_box), text);
#else
	gtk_combo_box_append_text(GTK_COMBO_BOX(combo_box), text);
#endif

}

gchar *nsgtk_combo_box_text_get_active_text(GtkWidget *combo_box)
{
#if GTK_CHECK_VERSION(2,24,0)
	return gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo_box));
#else
	return gtk_combo_box_get_active_text(GTK_COMBO_BOX(combo_box));
#endif
}

GtkWidget *nsgtk_entry_new(void)
{
#if GTK_CHECK_VERSION(2,16,0)
	return gtk_entry_new();
#else
	return GTK_WIDGET(sexy_icon_entry_new());
#endif
}

void nsgtk_entry_set_icon_from_pixbuf(GtkWidget *entry,
				      GtkEntryIconPosition icon_pos,
				      GdkPixbuf *pixbuf)
{
#if GTK_CHECK_VERSION(2,16,0)
	gtk_entry_set_icon_from_pixbuf(GTK_ENTRY(entry), icon_pos, pixbuf);
#else
	GtkImage *image = GTK_IMAGE(gtk_image_new_from_pixbuf(pixbuf));

	if (image != NULL) {
		sexy_icon_entry_set_icon(SEXY_ICON_ENTRY(entry),
					 (SexyIconEntryPosition)icon_pos,
					 image);

		g_object_unref(image);
	}

#endif
}


/* exported interface documented in gtk/compat.h */
void nsgtk_entry_set_icon_from_icon_name(GtkWidget *entry,
					 GtkEntryIconPosition icon_pos,
					 const gchar *id)
{
#ifdef NSGTK_USE_ICON_NAME
	gtk_entry_set_icon_from_icon_name(GTK_ENTRY(entry), icon_pos, id);
#else
#if GTK_CHECK_VERSION(2,16,0)
	gtk_entry_set_icon_from_icon_name(GTK_ENTRY(entry), icon_pos, id);
#else
	GtkImage *image;
	image = GTK_IMAGE(gtk_image_new_from_stock(id, GTK_ICON_SIZE_LARGE_TOOLBAR));

	if (image != NULL) {
		sexy_icon_entry_set_icon(SEXY_ICON_ENTRY(entry),
					 (SexyIconEntryPosition)icon_pos,
					 image);
		g_object_unref(image);
	}
#endif
#endif
}


/* exported interface documented in gtk/compat.h */
GtkWidget *nsgtk_image_new_from_stock(const gchar *id, GtkIconSize size)
{
#ifdef NSGTK_USE_ICON_NAME
	return gtk_image_new_from_icon_name(id, size);
#else
	return gtk_image_new_from_stock(id, size);
#endif
}


/* exported interface documented in gtk/compat.h */
GtkWidget *nsgtk_button_new_from_stock(const gchar *stock_id)
{
#ifdef NSGTK_USE_ICON_NAME
	return gtk_button_new_with_label(stock_id);
#else
	return gtk_button_new_from_stock(stock_id);
#endif
}

/* exported interface documented in gtk/compat.h */
void nsgtk_button_set_focus_on_click(GtkButton *button, gboolean focus_on_click)
{
#if GTK_CHECK_VERSION(3,20,0)
	gtk_widget_set_focus_on_click(GTK_WIDGET(button), focus_on_click);
#else
	gtk_button_set_focus_on_click(button, focus_on_click);
#endif
}


/* exported interface documented in gtk/compat.h */
gboolean nsgtk_stock_lookup(const gchar *stock_id, GtkStockItem *item)
{
#ifdef NSGTK_USE_ICON_NAME
	return FALSE;
#else
	return gtk_stock_lookup(stock_id, item);
#endif
}


void nsgtk_widget_override_background_color(GtkWidget *widget,
					    GtkStateFlags state,
					    uint16_t a,
					    uint16_t r,
					    uint16_t g,
					    uint16_t b)
{
#if GTK_CHECK_VERSION(3,0,0)
#if GTK_CHECK_VERSION(3,16,0)
	/* do nothing - deprecated - must use css styling */
	return;
#else
	GdkRGBA colour;
	colour.alpha = (double)a / 0xffff;
	colour.red = (double)r / 0xffff;
	colour.green = (double)g / 0xffff;
	colour.blue = (double)b / 0xffff;
	gtk_widget_override_background_color(widget, state, &colour);
#endif
#else
	GdkColor colour;
	colour.pixel = a;
	colour.red = r;
	colour.green = g;
	colour.blue = b;
	gtk_widget_modify_bg(widget, state, &colour );
#endif
}

GtkAdjustment *nsgtk_layout_get_vadjustment(GtkLayout *layout)
{
#if GTK_CHECK_VERSION(3,0,0)
	return gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(layout));
#else
	return gtk_layout_get_vadjustment(layout);
#endif
}

GtkAdjustment *nsgtk_layout_get_hadjustment(GtkLayout *layout)
{
#if GTK_CHECK_VERSION(3,0,0)
	return gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(layout));
#else
	return gtk_layout_get_hadjustment(layout);
#endif
}

static void nsgtk_layout_set_adjustment_step_increment(GtkAdjustment *adj,
						       int value)
{
#if GTK_CHECK_VERSION(2,14,0)
	gtk_adjustment_set_step_increment(adj, value);
#else
	adj->step_increment = value;
#endif
}

void nsgtk_layout_set_hadjustment(GtkLayout *layout, GtkAdjustment *adj)
{
#if GTK_CHECK_VERSION(3,0,0)
	gtk_scrollable_set_hadjustment(GTK_SCROLLABLE(layout), adj);
#else
	gtk_layout_set_hadjustment(layout, adj);
#endif
	nsgtk_layout_set_adjustment_step_increment(adj, 8);
}

void nsgtk_layout_set_vadjustment(GtkLayout *layout, GtkAdjustment *adj)
{
#if GTK_CHECK_VERSION(3,0,0)
	gtk_scrollable_set_vadjustment(GTK_SCROLLABLE(layout), adj);
#else
	gtk_layout_set_vadjustment(layout, adj);
#endif
	nsgtk_layout_set_adjustment_step_increment(adj, 8);
}

GtkWidget *nsgtk_hbox_new(gboolean homogeneous, gint spacing)
{
#if GTK_CHECK_VERSION(3,0,0)
	return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
#else
	return gtk_hbox_new(homogeneous, spacing);
#endif
}

GtkWidget *nsgtk_vbox_new(gboolean homogeneous, gint spacing)
{
#if GTK_CHECK_VERSION(3,0,0)
	return gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
#else
	return gtk_vbox_new(homogeneous, spacing);
#endif
}

GtkStateFlags nsgtk_widget_get_state_flags(GtkWidget *widget)
{
#if GTK_CHECK_VERSION(3,0,0)
	return gtk_widget_get_state_flags(widget);
#else
#if GTK_CHECK_VERSION(2,18,0)
	return gtk_widget_get_state(widget);
#else
	return 0; /* FIXME */
#endif
#endif
}

GtkStyleContext *nsgtk_widget_get_style_context(GtkWidget *widget)
{
#if GTK_CHECK_VERSION(3,0,0)
	return gtk_widget_get_style_context(widget);
#else
	return widget->style;
#endif
}

const PangoFontDescription* nsgtk_style_context_get_font(GtkStyleContext *style,
							 GtkStateFlags state)
{
#if GTK_CHECK_VERSION(3,8,0)
	const PangoFontDescription* fontdesc = NULL;
	gtk_style_context_get(style, state, GTK_STYLE_PROPERTY_FONT, &fontdesc, NULL);
	return fontdesc;
#else
#if GTK_CHECK_VERSION(3,0,0)
	return gtk_style_context_get_font(style, state);
#else
	return style->font_desc;
#endif
#endif
}

gulong nsgtk_connect_draw_event(GtkWidget *widget,
				GCallback callback,
				gpointer g)
{
#if GTK_CHECK_VERSION(3,0,0)
	return g_signal_connect(G_OBJECT(widget), "draw", callback, g);
#else
	return g_signal_connect(G_OBJECT(widget), "expose_event", callback, g);
#endif
}

void nsgdk_cursor_unref(GdkCursor *cursor)
{
#if GTK_CHECK_VERSION(3,0,0)
	g_object_unref(cursor);
#else
	gdk_cursor_unref(cursor);
#endif
}

void nsgtk_widget_modify_font(GtkWidget *widget,
			      PangoFontDescription *font_desc)
{
#if GTK_CHECK_VERSION(3,0,0)
/* FIXME */
	return;
#else
	gtk_widget_modify_font(widget, font_desc);
#endif
}

GdkWindow *nsgtk_widget_get_window(GtkWidget *widget)
{
#if GTK_CHECK_VERSION(2,14,0)
	return gtk_widget_get_window(widget);
#else
	return widget->window;
#endif
}

GtkWidget *nsgtk_dialog_get_content_area(GtkDialog *dialog)
{
#if GTK_CHECK_VERSION(2,14,0)
	return gtk_dialog_get_content_area(dialog);
#else
	return dialog->vbox;
#endif
}

#if GTK_CHECK_VERSION(3,22,0)
#include "gtk/scaffolding.h"
#endif

gboolean nsgtk_show_uri(GdkScreen *screen,
			const gchar *uri,
			guint32 timestamp,
			GError **error)
{
#if GTK_CHECK_VERSION(2,14,0)
#if GTK_CHECK_VERSION(3,22,0)
	GtkWindow *parent;
	parent = nsgtk_scaffolding_window(nsgtk_current_scaffolding());
	return gtk_show_uri_on_window(parent, uri, timestamp,error);
#else
	return gtk_show_uri(screen, uri, timestamp, error);
#endif
#else
	return FALSE; /** \todo add uri opening for before gtk 2.14 */
#endif
}

GdkWindow *nsgtk_layout_get_bin_window(GtkLayout *layout)
{
#if GTK_CHECK_VERSION(2,14,0)
	return gtk_layout_get_bin_window(layout);
#else
	return layout->bin_window;
#endif
}

gdouble nsgtk_adjustment_get_step_increment(GtkAdjustment *adjustment)
{
#if GTK_CHECK_VERSION(2,14,0)
	return gtk_adjustment_get_step_increment(adjustment);
#else
	return adjustment->step_increment;
#endif
}

gdouble nsgtk_adjustment_get_upper(GtkAdjustment *adjustment)
{
#if GTK_CHECK_VERSION(2,14,0)
	return gtk_adjustment_get_upper(adjustment);
#else
	return adjustment->upper;
#endif
}

gdouble nsgtk_adjustment_get_lower(GtkAdjustment *adjustment)
{
#if GTK_CHECK_VERSION(2,14,0)
	return gtk_adjustment_get_lower(adjustment);
#else
	return adjustment->lower;
#endif
}

gdouble nsgtk_adjustment_get_page_increment(GtkAdjustment *adjustment)
{
#if GTK_CHECK_VERSION(2,14,0)
	return gtk_adjustment_get_page_increment(adjustment);
#else
	return adjustment->page_increment;
#endif
}

void nsgtk_widget_get_allocation(GtkWidget *widget, GtkAllocation *allocation)
{
#if GTK_CHECK_VERSION(2,18,0)
	gtk_widget_get_allocation(widget, allocation);
#else
	allocation->x = widget->allocation.x;
	allocation->y = widget->allocation.y;
	allocation->width = widget->allocation.width;
	allocation->height = widget->allocation.height;
#endif
}

/* exported interface documented in gtk/compat.h */
GtkWidget *nsgtk_image_new_from_pixbuf_icon(GdkPixbuf *pixbuf, GtkIconSize size)
{
#if GTK_CHECK_VERSION(3,10,0)
	return gtk_image_new_from_pixbuf(pixbuf);
#else
	GtkIconSet *icon_set;
	GtkWidget *image;

	icon_set = gtk_icon_set_new_from_pixbuf(pixbuf);

	image = gtk_image_new_from_icon_set(icon_set, size);

	gtk_icon_set_unref(icon_set);

	return image;
#endif
}


/* exported interface documented in gtk/compat.h */
void nsgtk_window_set_opacity(GtkWindow *window, gdouble opacity)
{
#if GTK_CHECK_VERSION(3,8,0)
	gtk_widget_set_opacity(GTK_WIDGET(window), opacity);
#else
	gtk_window_set_opacity(window, opacity);
#endif
}

/* exported interface documented in gtk/compat.h */
void nsgtk_scrolled_window_add_with_viewport(GtkScrolledWindow *window,
		GtkWidget *child)
{
#if GTK_CHECK_VERSION(3,8,0)
	gtk_container_add(GTK_CONTAINER(window), child);
#else
	gtk_scrolled_window_add_with_viewport(window, child);
#endif
}

/* exported interface documented in gtk/compat.h */
GtkWidget *nsgtk_image_menu_item_new_with_mnemonic(const gchar *label)
{
#if GTK_CHECK_VERSION(3,10,0)
	return gtk_menu_item_new_with_mnemonic(label);
#else
	return gtk_image_menu_item_new_with_mnemonic(label);
#endif
}

/* exported interface documented in gtk/compat.h */
void nsgtk_image_menu_item_set_image(GtkWidget *image_menu_item, GtkWidget *image)
{
#if !GTK_CHECK_VERSION(3,10,0)
	gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(image_menu_item), image);
#endif
}

/* exported interface documented in gtk/compat.h */
void nsgtk_menu_popup_at_pointer(GtkMenu *menu, const GdkEvent *trigger_event)
{
#if GTK_CHECK_VERSION(3,22,0)
	gtk_menu_popup_at_pointer(menu, trigger_event);
#else
	gtk_menu_popup(menu, NULL, NULL, NULL, NULL, 0,
		       gtk_get_current_event_time());
#endif
}

/* exported interface documented in gtk/compat.h */
gboolean nsgtk_icon_size_lookup_for_settings(GtkSettings *settings,
					     GtkIconSize size,
					     gint *width,
					     gint *height)
{
#if GTK_CHECK_VERSION(3,10,0)
	return gtk_icon_size_lookup(size, width, height);
#else
	return gtk_icon_size_lookup_for_settings(settings, size, width, height);
#endif
}

/* exported interface documented in gtk/compat.h */
void nsgtk_widget_set_alignment(GtkWidget *widget, GtkAlign halign, GtkAlign valign)
{
#if GTK_CHECK_VERSION(3,0,0)
	gtk_widget_set_halign(widget, halign);
	gtk_widget_set_valign(widget, valign);
#else
	gfloat x, y;
	switch(halign) {
	case GTK_ALIGN_START:
		x = 0.0;
		break;

	case GTK_ALIGN_END:
		x = 1.0;
		break;

	default:
		x = 0.5;
		break;
	}

	switch(valign) {
	case GTK_ALIGN_START:
		y = 0.0;
		break;

	case GTK_ALIGN_END:
		y = 1.0;
		break;

	default:
		y = 0.5;
		break;
	}

	gtk_misc_set_alignment(GTK_MISC(widget), x, y);
#endif
}

/* exported interface documented in gtk/compat.h */
void nsgtk_widget_set_margins(GtkWidget *widget, gint hmargin, gint vmargin)
{
#if GTK_CHECK_VERSION(3,0,0)
#if GTK_CHECK_VERSION(3,12,0)
	gtk_widget_set_margin_start(widget, hmargin);
	gtk_widget_set_margin_end(widget, hmargin);
#else
	gtk_widget_set_margin_left(widget, hmargin);
	gtk_widget_set_margin_right(widget, hmargin);
#endif
	gtk_widget_set_margin_top(widget, vmargin);
	gtk_widget_set_margin_bottom(widget, vmargin);
#else
	if (GTK_IS_MISC(widget)) {
		gtk_misc_set_padding(GTK_MISC(widget), hmargin, vmargin);
	}
#endif
}

/* exported interface documented in gtk/compat.h */
guint
nsgtk_builder_add_from_resource(GtkBuilder *builder,
				const gchar *resource_path,
				GError **error)
{
	guint ret;

#ifdef WITH_GRESOURCE
#if GTK_CHECK_VERSION(3,4,0)
	ret = gtk_builder_add_from_resource(builder, resource_path, error);
#else
	GBytes *data;
	const gchar *buffer;
	gsize buffer_length;

	g_assert(error && *error == NULL);

	data = g_resources_lookup_data(resource_path, 0, error);
	if (data == NULL) {
		return 0;
	}

	buffer_length = 0;
	buffer = g_bytes_get_data(data, &buffer_length);
	g_assert(buffer != NULL);

	ret = gtk_builder_add_from_string(builder, buffer, buffer_length, error);

	g_bytes_unref(data);
#endif
#else
	ret = 0; /* return an error as GResource not supported before GLIB 2.32 */
#endif
	return ret;
}
