/*
 * Copyright 2021 Vincemt Sanders <vince@netsurf-browser.org>
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
 * Implementation of netsurf miscellaneous operations table
 */

#include <string.h>
#include <stdbool.h>
#include <gtk/gtk.h>

#include "utils/config.h"
#include "utils/errors.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsurl.h"
#include "netsurf/misc.h"
#include "desktop/save_pdf.h"

#include "gtk/compat.h"
#include "gtk/warn.h"
#include "gtk/schedule.h"
#include "gtk/resources.h"
#include "gtk/cookies.h"
#include "gtk/misc.h"


static nserror gui_launch_url(struct nsurl *url)
{
	gboolean ok;
	GError *error = NULL;

	ok = nsgtk_show_uri(NULL, nsurl_access(url), GDK_CURRENT_TIME, &error);
	if (ok == TRUE) {
		return NSERROR_OK;
	}

	if (error) {
		nsgtk_warning(messages_get("URIOpenError"), error->message);
		g_error_free(error);
	}
	return NSERROR_NO_FETCH_HANDLER;
}

static void nsgtk_PDF_set_pass(GtkButton *w, gpointer data)
{
	char **owner_pass = ((void **)data)[0];
	char **user_pass = ((void **)data)[1];
	GtkWindow *wnd = ((void **)data)[2];
	GtkBuilder *password_builder = ((void **)data)[3];
	char *path = ((void **)data)[4];

	char *op, *op1;
	char *up, *up1;

	op = strdup(gtk_entry_get_text(
			GTK_ENTRY(gtk_builder_get_object(password_builder,
					"entryPDFOwnerPassword"))));
	op1 = strdup(gtk_entry_get_text(
			GTK_ENTRY(gtk_builder_get_object(password_builder,
					"entryPDFOwnerPassword1"))));
	up = strdup(gtk_entry_get_text(
			GTK_ENTRY(gtk_builder_get_object(password_builder,
					"entryPDFUserPassword"))));
	up1 = strdup(gtk_entry_get_text(
			GTK_ENTRY(gtk_builder_get_object(password_builder,
					"entryPDFUserPassword1"))));


	if (op[0] == '\0') {
		gtk_label_set_text(GTK_LABEL(gtk_builder_get_object(password_builder,
				"labelInfo")),
				"Owner password must be at least 1 character long:");
		free(op);
		free(up);
	} else if (!strcmp(op, up)) {
		gtk_label_set_text(GTK_LABEL(gtk_builder_get_object(password_builder,
				"labelInfo")),
				"User and owner passwords must be different:");
		free(op);
		free(up);
	} else if (!strcmp(op, op1) && !strcmp(up, up1)) {

		*owner_pass = op;
		if (up[0] == '\0')
			free(up);
		else
			*user_pass = up;

		free(data);
		gtk_widget_destroy(GTK_WIDGET(wnd));
		g_object_unref(G_OBJECT(password_builder));

		save_pdf(path);

		free(path);
	} else {
		gtk_label_set_text(GTK_LABEL(gtk_builder_get_object(password_builder,
				"labelInfo")), "Passwords not confirmed:");
		free(op);
		free(up);
	}

	free(op1);
	free(up1);
}

static void nsgtk_PDF_no_pass(GtkButton *w, gpointer data)
{
	GtkWindow *wnd = ((void **)data)[2];
	GtkBuilder *password_builder = ((void **)data)[3];
	char *path = ((void **)data)[4];

	free(data);

	gtk_widget_destroy(GTK_WIDGET(wnd));
	g_object_unref(G_OBJECT(password_builder));

	save_pdf(path);

	free(path);
}

static void nsgtk_pdf_password(char **owner_pass, char **user_pass, char *path)
{
	GtkButton *ok, *no;
	GtkWindow *wnd;
	void **data;
	GtkBuilder *password_builder;
	nserror res;

	res = nsgtk_builder_new_from_resname("password", &password_builder);
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Password UI builder init failed");
		return;
	}

	gtk_builder_connect_signals(password_builder, NULL);

	wnd = GTK_WINDOW(gtk_builder_get_object(password_builder,
						"wndPDFPassword"));

	data = malloc(5 * sizeof(void *));

	*owner_pass = NULL;
	*user_pass = NULL;

	data[0] = owner_pass;
	data[1] = user_pass;
	data[2] = wnd;
	data[3] = password_builder;
	data[4] = path;

	ok = GTK_BUTTON(gtk_builder_get_object(password_builder,
					       "buttonPDFSetPassword"));
	no = GTK_BUTTON(gtk_builder_get_object(password_builder,
					       "buttonPDFNoPassword"));

	g_signal_connect(G_OBJECT(ok), "clicked",
			 G_CALLBACK(nsgtk_PDF_set_pass), (gpointer)data);
	g_signal_connect(G_OBJECT(no), "clicked",
			 G_CALLBACK(nsgtk_PDF_no_pass), (gpointer)data);

	gtk_widget_show(GTK_WIDGET(wnd));
}


static struct gui_misc_table misc_table = {
	.schedule = nsgtk_schedule,

	.launch_url = gui_launch_url,
	.pdf_password = nsgtk_pdf_password,
	.present_cookies = nsgtk_cookies_present,
};

struct gui_misc_table *nsgtk_misc_table = &misc_table;
