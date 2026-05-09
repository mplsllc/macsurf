/*
 * Copyright 2014 Vincent Sanders <vince@netsurf-browser.org>
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
 * \file gtk/about.c
 *
 * Implementation of gtk about dialog.
 */

#include <stdlib.h>
#include <stdint.h>

#include "utils/messages.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "netsurf/browser_window.h"
#include "desktop/version.h"

#include "gtk/warn.h"
#include "gtk/compat.h"
#include "gtk/about.h"

#define ABOUT_RESPONSE_ID_LICENCE 1
#define ABOUT_RESPONSE_ID_CREDITS 2


/**
 * Open a url and a browser window/tab
 *
 * \param url_text The text of the url to open
 */
static void about_open(const char *url_text)
{
	nsurl *url;
	nserror ret;
	enum browser_window_create_flags flags = BW_CREATE_HISTORY;

	if (nsoption_bool(show_single_tab) == true) {
		flags |= BW_CREATE_TAB;
	}

	ret = nsurl_create(url_text, &url);
	if (ret == NSERROR_OK) {
		ret = browser_window_create(flags, url, NULL, NULL, NULL);
		nsurl_unref(url);
	}

	if (ret != NSERROR_OK) {
		nsgtk_warning(messages_get_errorcode(ret), 0);
	}
}

/**
 * About dialog response handling.
 *
 * \param dialog The dialog widget
 * \param response_id The response ID from the user clicking.
 * \param user_data The value from the signal connection.
 */
static void
nsgtk_about_dialog_response(GtkDialog *dialog,
			    gint response_id,
			    gpointer user_data)
{
	switch (response_id) {

	case ABOUT_RESPONSE_ID_LICENCE:
		about_open("about:licence");
		break;

	case ABOUT_RESPONSE_ID_CREDITS:
		about_open("about:credits");
		break;
	}

	/* close about dialog */
	gtk_widget_destroy(GTK_WIDGET(dialog));
}

void nsgtk_about_dialog_init(GtkWindow *parent)
{
	GtkWidget *dialog, *vbox, *label;
	gchar *name_string;
	GList *pixbufs;

	/* Create the dialog */
	dialog = gtk_dialog_new_with_buttons("About NetSurf",
					     parent,
					     GTK_DIALOG_DESTROY_WITH_PARENT,
					     "Licence", ABOUT_RESPONSE_ID_LICENCE,
					     "Credits", ABOUT_RESPONSE_ID_CREDITS,
					     "Close", GTK_RESPONSE_CANCEL,
					     NULL, NULL);

	vbox = nsgtk_vbox_new(FALSE, 8);

	gtk_box_pack_start(GTK_BOX(nsgtk_dialog_get_content_area(GTK_DIALOG(dialog))), vbox, TRUE, TRUE, 0);

	/* NetSurf icon */
	pixbufs = gtk_window_get_default_icon_list();
	if (pixbufs != NULL) {
		GtkWidget *image;

		image = nsgtk_image_new_from_pixbuf_icon(GDK_PIXBUF(g_list_nth_data(pixbufs, 0)),
							 GTK_ICON_SIZE_DIALOG);
		g_list_free(pixbufs);

		gtk_box_pack_start(GTK_BOX(vbox), image, FALSE, FALSE, 0);
	}

	/* version string */
	label = gtk_label_new (NULL);
	name_string = g_markup_printf_escaped("<span size=\"xx-large\" weight=\"bold\">NetSurf %s</span>", netsurf_version);
	gtk_label_set_markup (GTK_LABEL (label), name_string);
	g_free(name_string);
	gtk_label_set_selectable (GTK_LABEL (label), TRUE);
	gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
	gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);

	label = gtk_label_new(messages_get("AboutDesc"));
	gtk_label_set_selectable(GTK_LABEL (label), TRUE);
	gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);

	label = gtk_label_new(messages_get("NetSurfCopyright"));
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);
	gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
	gtk_box_pack_start(GTK_BOX (vbox), label, FALSE, FALSE, 0);

	/* Remove separator */
	nsgtk_dialog_set_has_separator(GTK_DIALOG (dialog), FALSE);

	/* Ensure that the dialog box response is processed. */
	g_signal_connect_swapped(dialog,
				  "response",
				  G_CALLBACK(nsgtk_about_dialog_response),
				  dialog);

	/* Add the label, and show everything we've added to the dialog. */
	gtk_widget_show_all(dialog);
}
