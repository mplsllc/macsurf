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

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <gtk/gtk.h>
#include <glib/gstdio.h>

#include "utils/log.h"
#include "utils/utils.h"
#include "utils/nsurl.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "utils/string.h"
#include "desktop/download.h"
#include "netsurf/download.h"

#include "gtk/warn.h"
#include "gtk/scaffolding.h"
#include "gtk/toolbar_items.h"
#include "gtk/window.h"
#include "gtk/compat.h"
#include "gtk/resources.h"
#include "gtk/download.h"

#define UPDATE_RATE 500 /* In milliseconds */

struct download_context;

enum {
	NSGTK_DOWNLOAD_PROGRESS,
	NSGTK_DOWNLOAD_INFO,
	NSGTK_DOWNLOAD_REMAINING,
	NSGTK_DOWNLOAD_SPEED,
	NSGTK_DOWNLOAD_PULSE,
	NSGTK_DOWNLOAD_STATUS,
	NSGTK_DOWNLOAD,

	NSGTK_DOWNLOAD_N_COLUMNS
};

typedef enum {
	NSGTK_DOWNLOAD_NONE,
	NSGTK_DOWNLOAD_WORKING,
	NSGTK_DOWNLOAD_ERROR,
	NSGTK_DOWNLOAD_COMPLETE,
	NSGTK_DOWNLOAD_CANCELED
} nsgtk_download_status;

typedef enum {
	NSGTK_DOWNLOAD_PAUSE	= 1 << 0,
	NSGTK_DOWNLOAD_RESUME	= 1 << 1,
	NSGTK_DOWNLOAD_CANCEL	= 1 << 2,
	NSGTK_DOWNLOAD_CLEAR	= 1 << 3
} nsgtk_download_actions;

static const gchar* status_messages[] =	{
	NULL,
	"gtkWorking",
	"gtkError",
	"gtkComplete",
	"gtkCanceled"
};

/**
 * context for each download.
 */
struct gui_download_window {
	struct download_context *ctx;
	nsgtk_download_actions sensitivity;
	nsgtk_download_status status;

	GString *name;
	GString *time_left;
	unsigned long long int size_total;
	unsigned long long int size_downloaded;
	gint progress;
	gfloat time_remaining;
	gfloat start_time;
	gfloat speed;

	GtkTreeRowReference *row;
	GIOChannel *write;
	GError *error;
};

typedef	void (*nsgtk_download_selection_action)(struct gui_download_window *dl,
						void *user_data);

/**
 * context for a nsgtk download window.
 */
struct download_window_ctx {
	GtkWindow *window;
	GtkWindow *parent;

	GtkProgressBar *progress;

	GtkTreeView *tree;
	GtkListStore *store;
	GtkTreeSelection *selection;
	GtkTreeIter iter;

	GTimer *timer;
	GList *list;
	GtkButton *pause;
	GtkButton *clear;
	GtkButton *cancel;
	GtkButton *resume;

	gint num_active;
};

/**
 * global instance of the download window
 */
static struct download_window_ctx dl_ctx;


static GtkTreeView* nsgtk_download_tree_view_new(GtkBuilder *gladeFile)
{
	GtkTreeView *treeview;
	GtkCellRenderer *renderer;

	treeview = GTK_TREE_VIEW(gtk_builder_get_object(gladeFile,
							"treeDownloads"));

	/* Progress column */
	renderer = gtk_cell_renderer_progress_new();
	gtk_tree_view_insert_column_with_attributes(treeview,
						    -1,
						    messages_get("gtkProgress"),
						    renderer,
						    "value",
						    NSGTK_DOWNLOAD_PROGRESS,
						    "pulse",
						    NSGTK_DOWNLOAD_PULSE,
						    "text",
						    NSGTK_DOWNLOAD_STATUS,
						    NULL);

	/* Information column */
	renderer = gtk_cell_renderer_text_new();
	g_object_set(G_OBJECT(renderer),
		     "wrap-mode",
		     PANGO_WRAP_WORD_CHAR,
		     "wrap-width",
		     300,
		     NULL);
	gtk_tree_view_insert_column_with_attributes(treeview,
						    -1,
						    messages_get("gtkDetails"),
						    renderer,
						    "text",
						    NSGTK_DOWNLOAD_INFO,
						    NULL);
	gtk_tree_view_column_set_expand(
			gtk_tree_view_get_column(treeview,
						 NSGTK_DOWNLOAD_INFO), TRUE);

	/* Time remaining column */
	renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_insert_column_with_attributes(treeview,
						    -1,
						    messages_get("gtkRemaining"),
						    renderer,
						    "text",
						    NSGTK_DOWNLOAD_REMAINING,
						    NULL);

	/* Speed column */
	renderer = gtk_cell_renderer_text_new();
	gtk_tree_view_insert_column_with_attributes(treeview,
						    -1,
						    messages_get("gtkSpeed"),
						    renderer,
						    "text",
						    NSGTK_DOWNLOAD_SPEED,
						    NULL);

	return treeview;
}


static gint
nsgtk_download_sort(GtkTreeModel *model,
		    GtkTreeIter *a,
		    GtkTreeIter *b,
		    gpointer userdata)
{
	struct gui_download_window *dl1, *dl2;

	gtk_tree_model_get(model, a, NSGTK_DOWNLOAD, &dl1, -1);
	gtk_tree_model_get(model, b, NSGTK_DOWNLOAD, &dl2, -1);

	return dl1->status - dl2->status;
}


static void
nsgtk_download_sensitivity_update_buttons(nsgtk_download_actions sensitivity)
{
	/* Glade seems to pack the buttons in an arbitrary order */
	enum { PAUSE_BUTTON, CLEAR_BUTTON, CANCEL_BUTTON, RESUME_BUTTON };

	gtk_widget_set_sensitive(GTK_WIDGET(dl_ctx.pause),
				 sensitivity & NSGTK_DOWNLOAD_PAUSE);
	gtk_widget_set_sensitive(GTK_WIDGET(dl_ctx.clear),
				 sensitivity & NSGTK_DOWNLOAD_CLEAR);
	gtk_widget_set_sensitive(GTK_WIDGET(dl_ctx.cancel),
				 sensitivity & NSGTK_DOWNLOAD_CANCEL);
	gtk_widget_set_sensitive(GTK_WIDGET(dl_ctx.resume),
				 sensitivity & NSGTK_DOWNLOAD_RESUME);
}


static void nsgtk_download_sensitivity_evaluate(GtkTreeSelection *selection)
{
	GtkTreeIter iter;
	GList *rows;
	gboolean selected;
	GtkTreeModel *model;
	nsgtk_download_actions sensitivity = 0;
	struct gui_download_window *dl;

	model = GTK_TREE_MODEL(dl_ctx.store);

	selected = gtk_tree_selection_count_selected_rows(selection);
	if (selected) {
		rows = gtk_tree_selection_get_selected_rows(selection, &model);
		while (rows != NULL) {
			gtk_tree_model_get_iter(model,
						&iter,
						(GtkTreePath*)rows->data);
			gtk_tree_model_get(model,
					   &iter,
					   NSGTK_DOWNLOAD,
					   &dl,
					   -1);
			sensitivity |= dl->sensitivity;
			rows = rows->next;
		}
	} else {
		rows = dl_ctx.list;
		while (rows != NULL) {
			dl = rows->data;
			sensitivity |= (dl->sensitivity & NSGTK_DOWNLOAD_CLEAR);
			rows = rows->next;
		}
	}

	nsgtk_download_sensitivity_update_buttons(sensitivity);
}


/**
 * Wrapper to GFunc-ify gtk_tree_path_free for g_list_foreach.
 */
static void
nsgtk_download_gfunc__gtk_tree_path_free(gpointer data, gpointer user_data)
{
	gtk_tree_path_free(data);
}


/**
 * Wrapper to GFunc-ify g_free for g_list_foreach.
 */
static void
nsgtk_download_gfunc__g_free(gpointer data, gpointer user_data)
{
	g_free(data);
}


static void nsgtk_download_do(nsgtk_download_selection_action action)
{
	GList *rows, *dls = NULL;
	GtkTreeModel *model;

	if (gtk_tree_selection_count_selected_rows(dl_ctx.selection)) {
		model = GTK_TREE_MODEL(dl_ctx.store);

		rows = gtk_tree_selection_get_selected_rows(dl_ctx.selection,
							    &model);
		while (rows != NULL) {
			struct gui_download_window *dl;

			gtk_tree_model_get_iter(GTK_TREE_MODEL(dl_ctx.store),
						&dl_ctx.iter,
						(GtkTreePath*)rows->data);

			gtk_tree_model_get(GTK_TREE_MODEL(dl_ctx.store),
					   &dl_ctx.iter,
					   NSGTK_DOWNLOAD,
					   &dl,
					   -1);

			dls = g_list_prepend(dls, dl);

			rows = rows->next;
		}
		g_list_foreach(rows,
			       nsgtk_download_gfunc__gtk_tree_path_free,
			       NULL);
		g_list_foreach(rows,
			       nsgtk_download_gfunc__g_free,
			       NULL);
		g_list_free(rows);
	} else {
		dls = g_list_copy(dl_ctx.list);
	}

	g_list_foreach(dls, (GFunc)action, NULL);
	g_list_free(dls);
}


static gchar* nsgtk_download_info_to_string(struct gui_download_window *dl)
{
	gchar *size_info;
	gchar *r;

	size_info = g_strdup_printf(messages_get("gtkSizeInfo"),
				    human_friendly_bytesize(dl->size_downloaded),
				    dl->size_total == 0 ?
				    messages_get("gtkUnknownSize") :
				    human_friendly_bytesize(dl->size_total));

	if (dl->status != NSGTK_DOWNLOAD_ERROR) {
		r = g_strdup_printf("%s\n%s", dl->name->str, size_info);
	} else {
		r = g_strdup_printf("%s\n%s", dl->name->str, dl->error->message);
	}

	g_free(size_info);

	return r;
}


static gchar* nsgtk_download_time_to_string(gint seconds)
{
	gint hours, minutes;

	if (seconds < 0) {
		return g_strdup("-");
	}

	hours = seconds / 3600;
	seconds -= hours * 3600;
	minutes = seconds / 60;
	seconds -= minutes * 60;

	if (hours > 0) {
		return g_strdup_printf("%u:%02u:%02u",
				       hours,
				       minutes,
				       seconds);
	} else {
		return g_strdup_printf("%u:%02u", minutes, seconds);
	}
}


static void nsgtk_download_store_update_item(struct gui_download_window *dl)
{
	gchar *info;
	char *speed;
	gchar *time;
	gboolean pulse;

	info = nsgtk_download_info_to_string(dl);

	/* setup the speed text */
	if (dl->speed == 0) {
		speed = strdup("-");
	} else {
		size_t speedsize;
		char *human;
		human = human_friendly_bytesize(dl->speed);
		speedsize = strlen(human) + SLEN("/s") + 1;
		speed = malloc(speedsize);
		snprintf(speed, speedsize, "%s/s", human);
	}
	time = nsgtk_download_time_to_string(dl->time_remaining);
	pulse = dl->status == NSGTK_DOWNLOAD_WORKING;

	/* Updates iter (which is needed to set and get data) with the dl row */
	gtk_tree_model_get_iter(GTK_TREE_MODEL(dl_ctx.store),
				&dl_ctx.iter,
				gtk_tree_row_reference_get_path(dl->row));

	gtk_list_store_set(dl_ctx.store, &dl_ctx.iter,
			   NSGTK_DOWNLOAD_PULSE, pulse ? dl->progress : -1,
			   NSGTK_DOWNLOAD_PROGRESS, pulse ? 0 : dl->progress,
			   NSGTK_DOWNLOAD_INFO, info,
			   NSGTK_DOWNLOAD_SPEED, speed,
			   NSGTK_DOWNLOAD_REMAINING, time,
			   NSGTK_DOWNLOAD, dl,
			   -1);

	g_free(info);
	g_free(time);
	free(speed);
}


static gboolean nsgtk_download_update(gboolean force_update)
{
	/* Be sure we need to update */
	if (!nsgtk_widget_get_visible(GTK_WIDGET(dl_ctx.window))) {
		return TRUE;
	}

	GList *list;
	gchar *text;
	gboolean update, pulse_mode = FALSE;
	unsigned long long int downloaded = 0;
	unsigned long long int total = 0;
	gint dls = 0;
	gfloat percent, elapsed = g_timer_elapsed(dl_ctx.timer, NULL);

	dl_ctx.num_active = 0;

	for (list = dl_ctx.list; list != NULL; list = list->next) {
		struct gui_download_window *dl = list->data;
		update = force_update;

		switch (dl->status) {
		case NSGTK_DOWNLOAD_WORKING:
			pulse_mode = TRUE;
			fallthrough;

		case NSGTK_DOWNLOAD_NONE:
			dl->speed = dl->size_downloaded /
				(elapsed - dl->start_time);
			if (dl->status == NSGTK_DOWNLOAD_NONE) {
				dl->time_remaining = (dl->size_total -
						      dl->size_downloaded)/
					dl->speed;
				dl->progress = (double)dl->size_downloaded /
					(double)dl->size_total * 100;
			} else {
				dl->progress++;
			}

			dl_ctx.num_active++;
			update = TRUE;
			fallthrough;

		case NSGTK_DOWNLOAD_COMPLETE:
			downloaded += dl->size_downloaded;
			total += dl->size_total;
			dls++;
			fallthrough;

		default:
			;//Do nothing

		}
		if (update) {
			nsgtk_download_store_update_item(dl);
		}
	}

	if (pulse_mode) {
		text = g_strdup_printf(
			messages_get(dl_ctx.num_active > 1 ?
				     "gtkProgressBarPulse" :
				     "gtkProgressBarPulseSingle"),
			dl_ctx.num_active);
		gtk_progress_bar_pulse(dl_ctx.progress);
		gtk_progress_bar_set_text(dl_ctx.progress, text);
	} else {
		percent = total != 0 ? (double)downloaded / (double)total : 0;
		text = g_strdup_printf(messages_get("gtkProgressBar"),
				       floor(percent * 100), dls);
		gtk_progress_bar_set_fraction(dl_ctx.progress,
					      percent);
		gtk_progress_bar_set_text(dl_ctx.progress, text);
	}

	g_free(text);

	if (dl_ctx.num_active == 0) {
		return FALSE; /* Returning FALSE here cancels the g_timeout */
	} else {
		return TRUE;
	}
}


static void
nsgtk_download_store_clear_item(struct gui_download_window *dl,	void *user_data)
{
	if (dl->sensitivity & NSGTK_DOWNLOAD_CLEAR) {
		dl_ctx.list = g_list_remove(dl_ctx.list, dl);

		gtk_tree_model_get_iter(GTK_TREE_MODEL(dl_ctx.store),
					&dl_ctx.iter,
					gtk_tree_row_reference_get_path(dl->row));
		gtk_list_store_remove(dl_ctx.store,
				      &dl_ctx.iter);

		download_context_destroy(dl->ctx);
		g_string_free(dl->name, TRUE);
		g_string_free(dl->time_left, TRUE);
		g_free(dl);

		nsgtk_download_sensitivity_evaluate(dl_ctx.selection);
		nsgtk_download_update(FALSE);
	}
}


static void
nsgtk_download_tree_view_row_activated(GtkTreeView *tree,
				       GtkTreePath *path,
				       GtkTreeViewColumn *column,
				       gpointer data)
{
	GtkTreeModel *model;
	GtkTreeIter iter;

	model = gtk_tree_view_get_model(tree);

	if (gtk_tree_model_get_iter(model, &iter, path)) {
		/* TODO: This will be a context action (pause, start, clear) */
		nsgtk_download_do(nsgtk_download_store_clear_item);
	}
}


static void
nsgtk_download_change_sensitivity(struct gui_download_window *dl,
				  nsgtk_download_actions sensitivity)
{
	dl->sensitivity = sensitivity;
	nsgtk_download_sensitivity_evaluate(dl_ctx.selection);
}


static void
nsgtk_download_change_status(struct gui_download_window *dl,
			     nsgtk_download_status status)
{
	dl->status = status;
	if (status != NSGTK_DOWNLOAD_NONE) {
		gtk_tree_model_get_iter(GTK_TREE_MODEL(dl_ctx.store),
					&dl_ctx.iter,
					gtk_tree_row_reference_get_path(dl->row));

		gtk_list_store_set(dl_ctx.store, &dl_ctx.iter,
				   NSGTK_DOWNLOAD_STATUS,
				   messages_get(status_messages[status]), -1);
	}
}


static void
nsgtk_download_store_cancel_item(struct gui_download_window *dl,
				 void *user_data)
{
	if (dl->sensitivity & NSGTK_DOWNLOAD_CANCEL) {
		dl->speed = 0;
		dl->size_downloaded = 0;
		dl->progress = 0;
		dl->time_remaining = -1;
		nsgtk_download_change_sensitivity(dl, NSGTK_DOWNLOAD_CLEAR);
		nsgtk_download_change_status(dl, NSGTK_DOWNLOAD_CANCELED);

		download_context_abort(dl->ctx);

		g_unlink(download_context_get_filename(dl->ctx));

		nsgtk_download_update(TRUE);
	}
}


static gboolean nsgtk_download_hide(GtkWidget *window)
{
	gtk_widget_hide(window);
	return TRUE;
}


/**
 * Prompt user for downloaded file name
 *
 * \param filename The original name of the file
 * \param domain the domain the file is being downloaded from
 * \param size The size of the file being downloaded
 */
static gchar*
nsgtk_download_dialog_show(const gchar *filename,
			   const gchar *domain,
			   const gchar *size)
{
	enum { GTK_RESPONSE_DOWNLOAD, GTK_RESPONSE_SAVE_AS };
	GtkWidget *dialog;
	char *destination = NULL;
	gchar *message;
	gchar *info;

	message = g_strdup(messages_get("gtkStartDownload"));
	info = g_strdup_printf(messages_get("gtkInfo"), filename, domain, size);

	dialog = gtk_message_dialog_new_with_markup(
		dl_ctx.parent,
		GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
		"<span size=\"x-large\" weight=\"ultrabold\">%s</span>"
		"\n\n<small>%s</small>",
		message,
		info);

	gtk_dialog_add_buttons(GTK_DIALOG(dialog),
			       NSGTK_STOCK_SAVE, GTK_RESPONSE_DOWNLOAD,
			       NSGTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
			       NSGTK_STOCK_SAVE_AS, GTK_RESPONSE_SAVE_AS,
			       NULL);

	gint result = gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
	g_free(message);
	g_free(info);

	switch (result) {
	case GTK_RESPONSE_SAVE_AS: {
		dialog = gtk_file_chooser_dialog_new(
			messages_get("gtkSave"),
			dl_ctx.parent,
			GTK_FILE_CHOOSER_ACTION_SAVE,
			NSGTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
			NSGTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
			NULL);
		gtk_file_chooser_set_current_name
			(GTK_FILE_CHOOSER(dialog), filename);
		gtk_file_chooser_set_current_folder
			(GTK_FILE_CHOOSER(dialog),
			 nsoption_charp(downloads_directory));
		gtk_file_chooser_set_do_overwrite_confirmation
			(GTK_FILE_CHOOSER(dialog),
			 nsoption_bool(request_overwrite));

		gint result = gtk_dialog_run(GTK_DIALOG(dialog));
		if (result == GTK_RESPONSE_ACCEPT)
			destination = gtk_file_chooser_get_filename
				(GTK_FILE_CHOOSER(dialog));
		gtk_widget_destroy(dialog);
		break;
	}
	case GTK_RESPONSE_DOWNLOAD: {
		size_t destsize;
		destsize = strlen(nsoption_charp(downloads_directory)) +
			SLEN("/") + strlen(filename) + 1;
		destination = malloc(destsize);
		if (destination == NULL) {
			nsgtk_warning(messages_get("NoMemory"), 0);
			break;
		}
		snprintf(destination, destsize, "%s/%s",
			 nsoption_charp(downloads_directory), filename);
		/* Test if file already exists and display overwrite
		 * confirmation if needed */
		if (g_file_test(destination, G_FILE_TEST_EXISTS) &&
		    nsoption_bool(request_overwrite)) {
			GtkWidget *button;

			message = g_strdup_printf(messages_get("gtkOverwrite"),
						  filename);
			info = g_strdup_printf(messages_get("gtkOverwriteInfo"),
					       nsoption_charp(downloads_directory));

			dialog = gtk_message_dialog_new_with_markup(
				dl_ctx.parent,
				GTK_DIALOG_DESTROY_WITH_PARENT,
				GTK_MESSAGE_QUESTION,
				GTK_BUTTONS_CANCEL,
				"<b>%s</b>",
				message);
			gtk_message_dialog_format_secondary_markup(
				GTK_MESSAGE_DIALOG(dialog),
				"%s",
				info);

			button = gtk_dialog_add_button(GTK_DIALOG(dialog),
						       "_Replace",
						       GTK_RESPONSE_DOWNLOAD);
			gtk_button_set_image(GTK_BUTTON(button),
					     nsgtk_image_new_from_stock(
						     NSGTK_STOCK_SAVE,
						     GTK_ICON_SIZE_BUTTON));

			gint result = gtk_dialog_run(GTK_DIALOG(dialog));
			if (result == GTK_RESPONSE_CANCEL) {
				free(destination);
				destination = NULL;
			}

			gtk_widget_destroy(dialog);
			g_free(message);
			g_free(info);
		}
		break;
	}
	}
	return destination;
}


static gboolean nsgtk_download_handle_error(GError *error)
{
	GtkWidget*dialog;
	gchar *message;

	if (error != NULL) {
		message = g_strdup_printf(messages_get("gtkFileError"),
					  error->message);

		dialog = gtk_message_dialog_new_with_markup(
					dl_ctx.parent,
					GTK_DIALOG_MODAL,
					GTK_MESSAGE_ERROR,
					GTK_BUTTONS_OK,
					"<big><b>%s</b></big>\n\n"
					"<small>%s</small>",
					messages_get("gtkFailed"),
					message);

		gtk_dialog_run(GTK_DIALOG(dialog));
		gtk_widget_destroy(dialog);
		return TRUE;
	}
	return FALSE;
}


static void nsgtk_download_store_create_item(struct gui_download_window *dl)
{
	nsgtk_download_store_update_item(dl);
	/* The iter has already been updated to this row */
	gtk_list_store_set(dl_ctx.store,
			   &dl_ctx.iter,
			   NSGTK_DOWNLOAD,
			   dl,
			   -1);
}


/**
 * Wrapper to GSourceFunc-ify nsgtk_download_update.
 */
static gboolean
nsgtk_download_gsourcefunc__nsgtk_download_update(gpointer user_data)
{
	return nsgtk_download_update(FALSE);
}


/**
 * core callback on creating a new download
 */
static struct gui_download_window *
gui_download_window_create(download_context *ctx, struct gui_window *gui)
{
	nsurl *url;
	unsigned long long int total_size;
	gchar *domain;
	gchar *destination;
	gboolean unknown_size;
	struct gui_download_window *download;
	const char *size;

	url = download_context_get_url(ctx);
	total_size = download_context_get_total_length(ctx);
	unknown_size = total_size == 0;
	size = (total_size == 0 ?
		messages_get("gtkUnknownSize") :
		human_friendly_bytesize(total_size));

	dl_ctx.parent =	nsgtk_scaffolding_window(nsgtk_get_scaffold(gui));

	download = malloc(sizeof *download);
	if (download == NULL) {
		return NULL;
	}

	/* set the domain to the host component of the url if it exists */
	if (nsurl_has_component(url, NSURL_HOST)) {
		domain = g_strdup(lwc_string_data(nsurl_get_component(url, NSURL_HOST)));
	} else {
		domain = g_strdup(messages_get("gtkUnknownHost"));
	}
	if (domain == NULL) {
		free(download);
		return NULL;
	}

	/* show the dialog */
	destination = nsgtk_download_dialog_show(
		download_context_get_filename(ctx), domain, size);
	if (destination == NULL) {
		g_free(domain);
		free(download);
		return NULL;
	}

	/* Add the new row and store the reference to it (which keeps track of
	 * the tree changes) */
	gtk_list_store_prepend(dl_ctx.store, &dl_ctx.iter);
	download->row = gtk_tree_row_reference_new(
					GTK_TREE_MODEL(dl_ctx.store),
					gtk_tree_model_get_path(
						GTK_TREE_MODEL(dl_ctx.store),
						&dl_ctx.iter));

	download->ctx = ctx;
	download->name = g_string_new(download_context_get_filename(ctx));
	download->time_left = g_string_new("");
	download->size_total = total_size;
	download->size_downloaded = 0;
	download->speed = 0;
	download->start_time = g_timer_elapsed(dl_ctx.timer, NULL);
	download->time_remaining = -1;
	download->status = NSGTK_DOWNLOAD_NONE;
	download->progress = 0;
	download->error = NULL;
	download->write = g_io_channel_new_file(destination,
						"w",
						&download->error);

	if (nsgtk_download_handle_error(download->error)) {
		g_string_free(download->name, TRUE);
		g_string_free(download->time_left, TRUE);
		free(download);
		return NULL;
	}
	g_io_channel_set_encoding(download->write, NULL, &download->error);

	nsgtk_download_change_sensitivity(download, NSGTK_DOWNLOAD_CANCEL);

	nsgtk_download_store_create_item(download);
	nsgtk_download_show(dl_ctx.parent);

	if (unknown_size) {
		nsgtk_download_change_status(download, NSGTK_DOWNLOAD_WORKING);
	}

	if (dl_ctx.num_active == 0) {
		g_timeout_add(
			UPDATE_RATE,
			nsgtk_download_gsourcefunc__nsgtk_download_update,
			NULL);
	}

	dl_ctx.list = g_list_prepend(dl_ctx.list, download);

	return download;
}


/**
 * core callback on receipt of data
 */
static nserror
gui_download_window_data(struct gui_download_window *dw,
			 const char *data,
			 unsigned int size)
{
	GIOStatus status;
	status = g_io_channel_write_chars(dw->write, data, size, NULL, &dw->error);
	if (status != G_IO_STATUS_NORMAL || dw->error != NULL) {
		dw->speed = 0;
		dw->time_remaining = -1;

		nsgtk_download_change_sensitivity(dw, NSGTK_DOWNLOAD_CLEAR);
		nsgtk_download_change_status(dw, NSGTK_DOWNLOAD_ERROR);

		nsgtk_download_update(TRUE);

		gtk_window_present(dl_ctx.window);

		return NSERROR_SAVE_FAILED;
	}
	dw->size_downloaded += size;

	return NSERROR_OK;
}


/**
 * core callback on error
 */
static void
gui_download_window_error(struct gui_download_window *dw, const char *error_msg)
{
}


/**
 * core callback when core download is complete
 */
static void gui_download_window_done(struct gui_download_window *dw)
{
	g_io_channel_shutdown(dw->write, TRUE, &dw->error);
	g_io_channel_unref(dw->write);

	dw->speed = 0;
	dw->time_remaining = -1;
	dw->progress = 100;
	dw->size_total = dw->size_downloaded;
	nsgtk_download_change_sensitivity(dw, NSGTK_DOWNLOAD_CLEAR);
	nsgtk_download_change_status(dw, NSGTK_DOWNLOAD_COMPLETE);

	if (nsoption_bool(downloads_clear)) {
		nsgtk_download_store_clear_item(dw, NULL);
	} else {
		nsgtk_download_update(TRUE);
	}
}


static struct gui_download_table download_table = {
	.create = gui_download_window_create,
	.data = gui_download_window_data,
	.error = gui_download_window_error,
	.done = gui_download_window_done,
};

struct gui_download_table *nsgtk_download_table = &download_table;


/* exported interface documented in gtk/download.h */
nserror nsgtk_download_init(void)
{
	GtkBuilder* builder;
	nserror res;

	res = nsgtk_builder_new_from_resname("downloads", &builder);
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Download UI builder init failed");
		return res;
	}

	gtk_builder_connect_signals(builder, NULL);

	dl_ctx.pause = GTK_BUTTON(gtk_builder_get_object(builder,
							 "buttonPause"));
	dl_ctx.clear = GTK_BUTTON(gtk_builder_get_object(builder,
							 "buttonClear"));
	dl_ctx.cancel = GTK_BUTTON(gtk_builder_get_object(builder,
							  "buttonCancel"));
	dl_ctx.resume = GTK_BUTTON(gtk_builder_get_object(builder,
							  "buttonPlay"));

	dl_ctx.progress = GTK_PROGRESS_BAR(gtk_builder_get_object(builder,
								  "progressBar"));
	dl_ctx.window = GTK_WINDOW(gtk_builder_get_object(builder,
							  "wndDownloads"));
	dl_ctx.parent = NULL;

	gtk_window_set_transient_for(GTK_WINDOW(dl_ctx.window),
				     dl_ctx.parent);
	gtk_window_set_destroy_with_parent(GTK_WINDOW(dl_ctx.window),
					   FALSE);

	dl_ctx.timer = g_timer_new();

	dl_ctx.tree = nsgtk_download_tree_view_new(builder);

	dl_ctx.store = gtk_list_store_new(NSGTK_DOWNLOAD_N_COLUMNS,
					  G_TYPE_INT, /* % complete */
					  G_TYPE_STRING, /* Description */
					  G_TYPE_STRING, /* Time remaining */
					  G_TYPE_STRING, /* Speed */
					  G_TYPE_INT,	/* Pulse */
					  G_TYPE_STRING, /* Status */
					  G_TYPE_POINTER /* Download structure */
					  );


	gtk_tree_view_set_model(dl_ctx.tree, GTK_TREE_MODEL(dl_ctx.store));

	gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(dl_ctx.store),
					NSGTK_DOWNLOAD_STATUS,
					(GtkTreeIterCompareFunc)nsgtk_download_sort, NULL, NULL);
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(dl_ctx.store),
					     NSGTK_DOWNLOAD_STATUS,
					     GTK_SORT_ASCENDING);

	g_object_unref(dl_ctx.store);

	dl_ctx.selection = gtk_tree_view_get_selection(dl_ctx.tree);
	gtk_tree_selection_set_mode(dl_ctx.selection, GTK_SELECTION_MULTIPLE);

	g_signal_connect(G_OBJECT(dl_ctx.selection),
			 "changed",
			 G_CALLBACK(nsgtk_download_sensitivity_evaluate),
			 NULL);

	g_signal_connect(dl_ctx.tree,
			 "row-activated",
			 G_CALLBACK(nsgtk_download_tree_view_row_activated),
			 NULL);

	g_signal_connect_swapped(gtk_builder_get_object(builder, "buttonClear"),
				 "clicked",
				 G_CALLBACK(nsgtk_download_do),
				 nsgtk_download_store_clear_item);

	g_signal_connect_swapped(gtk_builder_get_object(builder, "buttonCancel"),
				 "clicked",
				 G_CALLBACK(nsgtk_download_do),
				 nsgtk_download_store_cancel_item);

	g_signal_connect(G_OBJECT(dl_ctx.window),
			 "delete-event",
			 G_CALLBACK(nsgtk_download_hide),
			 NULL);

	return NSERROR_OK;
}


/* exported interface documented in gtk/download.h */
void nsgtk_download_destroy(void)
{
	nsgtk_download_do(nsgtk_download_store_cancel_item);
}


/* exported interface documented in gtk/download.h */
bool nsgtk_check_for_downloads(GtkWindow *parent)
{
	GtkWidget *dialog;
	gint response;

	if (dl_ctx.num_active == 0) {
		return false;
	}

	dialog = gtk_message_dialog_new_with_markup(
				parent,
				GTK_DIALOG_MODAL,
				GTK_MESSAGE_WARNING,
				GTK_BUTTONS_NONE,
				"<big><b>%s</b></big>\n\n"
				"<small>%s</small>",
				messages_get("gtkQuit"),
				messages_get("gtkDownloadsRunning"));

	gtk_dialog_add_buttons(GTK_DIALOG(dialog),
			       "gtk-cancel", GTK_RESPONSE_CANCEL,
			       "gtk-quit", GTK_RESPONSE_CLOSE,
			       NULL);

	response = gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);

	if (response == GTK_RESPONSE_CANCEL) {
		return true;
	}

	return false;
}


/* exported interface documented in gtk/download.h */
void nsgtk_download_show(GtkWindow *parent)
{
	gtk_window_set_transient_for(dl_ctx.window, dl_ctx.parent);
	gtk_window_present(dl_ctx.window);
}
