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
 * \file
 * generic data viewer implementation.
 *
 * This viewer can be used for utf-8 encoded chunk of data. Thie data
 * might be page source or the debugging of dom or box trees. It will
 * show the data in a tab, window or editor as per user configuration.
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define _WITH_GETLINE /* necessary for FreeBSD */
#include <stdio.h>
#include <unistd.h>
#include <gtk/gtk.h>

#include "utils/log.h"
#include "utils/nsoption.h"
#include "utils/utf8.h"
#include "utils/messages.h"
#include "utils/utils.h"
#include "utils/file.h"
#include "utils/filepath.h"
#include "utils/nsurl.h"
#include "netsurf/browser_window.h"

#include "gtk/warn.h"
#include "gtk/about.h"
#include "gtk/fetch.h"
#include "gtk/compat.h"
#include "gtk/resources.h"
#include "gtk/viewdata.h"

struct nsgtk_viewdata_ctx {
	char *data;
	size_t data_len;
	char *filename;

	GtkBuilder *builder; /**< The gtk builder that built the widgets. */
	GtkWindow *window; /**< handle to gtk window (builder holds reference) */
	GtkTextView *gv; /**< handle to gtk text view (builder holds reference) */

	struct nsgtk_viewdata_ctx *next;
	struct nsgtk_viewdata_ctx *prev;
};

struct menu_events {
	const char *widget;
	GCallback handler;
};

static struct nsgtk_viewdata_ctx *nsgtk_viewdata_list = NULL;
static char viewdata_zoomlevel = 10;

#define MENUEVENT(x) { #x, G_CALLBACK(nsgtk_on_##x##_activate) }
#define MENUPROTO(x) static gboolean nsgtk_on_##x##_activate(	\
		GtkMenuItem *widget, gpointer g)

MENUPROTO(viewdata_save_as);
MENUPROTO(viewdata_print);
MENUPROTO(viewdata_close);
MENUPROTO(viewdata_select_all);
MENUPROTO(viewdata_cut);
MENUPROTO(viewdata_copy);
MENUPROTO(viewdata_paste);
MENUPROTO(viewdata_delete);
MENUPROTO(viewdata_zoom_in);
MENUPROTO(viewdata_zoom_out);
MENUPROTO(viewdata_zoom_normal);
MENUPROTO(viewdata_about);

static struct menu_events viewdata_menu_events[] = {
	MENUEVENT(viewdata_save_as),
	MENUEVENT(viewdata_print),
	MENUEVENT(viewdata_close),
	MENUEVENT(viewdata_select_all),
	MENUEVENT(viewdata_cut),
	MENUEVENT(viewdata_copy),
	MENUEVENT(viewdata_paste),
	MENUEVENT(viewdata_delete),
	MENUEVENT(viewdata_zoom_in),
	MENUEVENT(viewdata_zoom_out),
	MENUEVENT(viewdata_zoom_normal),
	MENUEVENT(viewdata_about),
	{NULL, NULL}
};

static void nsgtk_attach_viewdata_menu_handlers(GtkBuilder *xml, gpointer g)
{
	struct menu_events *event = viewdata_menu_events;

	while (event->widget != NULL)
	{
		GtkWidget *w = GTK_WIDGET(gtk_builder_get_object(xml, event->widget));
		g_signal_connect(G_OBJECT(w), "activate", event->handler, g);
		event++;
	}
}

static gboolean nsgtk_viewdata_destroy_event(GtkBuilder *window, gpointer g)
{
	struct nsgtk_viewdata_ctx *vdctx = (struct nsgtk_viewdata_ctx *)g;

	if (vdctx->next != NULL) {
		vdctx->next->prev = vdctx->prev;
	}

	if (vdctx->prev != NULL) {
		vdctx->prev->next = vdctx->next;
	} else {
		nsgtk_viewdata_list = vdctx->next;
	}

	/* release the data */
	free(vdctx->data);

	/* free the builder */
	g_object_unref(G_OBJECT(vdctx->builder));

	/* free the context structure */
	free(vdctx);

	return FALSE;
}

static gboolean nsgtk_viewdata_delete_event(GtkWindow * window, gpointer g)
{
	return FALSE;
}



static void nsgtk_viewdata_file_save(GtkWindow *parent, const char *filename,
				     const char *data, size_t data_size)
{
	FILE *f;
	GtkWidget *notif;
	GtkWidget *label;

	f = fopen(filename, "w+");
	if (f != NULL) {
		fwrite(data, data_size, 1, f);
		fclose(f);
		return;
	}

	/* inform user of faliure */
	notif = gtk_dialog_new_with_buttons(messages_get("gtkSaveFailedTitle"),
					    parent,
					    GTK_DIALOG_MODAL,
					    NSGTK_STOCK_OK,
					    GTK_RESPONSE_NONE,
					    NULL);

	g_signal_connect_swapped(notif, "response",
				 G_CALLBACK(gtk_widget_destroy), notif);

	label = gtk_label_new(messages_get("gtkSaveFailed"));
	gtk_container_add(GTK_CONTAINER(nsgtk_dialog_get_content_area(GTK_DIALOG(notif))), label);
	gtk_widget_show_all(notif);

}


gboolean nsgtk_on_viewdata_save_as_activate(GtkMenuItem *widget, gpointer g)
{
	struct nsgtk_viewdata_ctx *nsg = (struct nsgtk_viewdata_ctx *) g;
	GtkWidget *fc;

	fc = gtk_file_chooser_dialog_new(messages_get("gtkSaveFile"),
					 nsg->window,
					 GTK_FILE_CHOOSER_ACTION_SAVE,
					 NSGTK_STOCK_CANCEL,
					 GTK_RESPONSE_CANCEL,
					 NSGTK_STOCK_SAVE,
					 GTK_RESPONSE_ACCEPT,
					 NULL);

	gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(fc), nsg->filename);

	gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(fc),
						       TRUE);

	if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT) {
		char *filename;
		filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
		nsgtk_viewdata_file_save(nsg->window, filename, nsg->data, nsg->data_len);
		g_free(filename);
	}

	gtk_widget_destroy(fc);

	return TRUE;
}


gboolean nsgtk_on_viewdata_print_activate( GtkMenuItem *widget, gpointer g)
{
	/* correct printing */

	return TRUE;
}

gboolean nsgtk_on_viewdata_close_activate( GtkMenuItem *widget, gpointer g)
{
	struct nsgtk_viewdata_ctx *nsg = (struct nsgtk_viewdata_ctx *) g;

	gtk_widget_destroy(GTK_WIDGET(nsg->window));

	return TRUE;
}



gboolean nsgtk_on_viewdata_select_all_activate (GtkMenuItem *widget, gpointer g)
{
	struct nsgtk_viewdata_ctx *nsg = (struct nsgtk_viewdata_ctx *) g;
	GtkTextBuffer *buf = gtk_text_view_get_buffer(nsg->gv);
	GtkTextIter start, end;

	gtk_text_buffer_get_bounds(buf, &start, &end);

	gtk_text_buffer_select_range(buf, &start, &end);

	return TRUE;
}

gboolean nsgtk_on_viewdata_cut_activate(GtkMenuItem *widget, gpointer g)
{
	return TRUE;
}

gboolean nsgtk_on_viewdata_copy_activate(GtkMenuItem *widget, gpointer g)
{
	struct nsgtk_viewdata_ctx *nsg = (struct nsgtk_viewdata_ctx *) g;
	GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(nsg->gv));

	gtk_text_buffer_copy_clipboard(buf,
		gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));

	return TRUE;
}

gboolean nsgtk_on_viewdata_paste_activate(GtkMenuItem *widget, gpointer g)
{
	return TRUE;
}

gboolean nsgtk_on_viewdata_delete_activate(GtkMenuItem *widget, gpointer g)
{
	return TRUE;
}

static void nsgtk_viewdata_update_zoomlevel(gpointer g)
{
	struct nsgtk_viewdata_ctx *nsg;
	GtkTextBuffer *buf;
	GtkTextTagTable *tab;
	GtkTextTag *tag;

	nsg = nsgtk_viewdata_list;
	while (nsg) {
		if (nsg->gv) {
			buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(nsg->gv));

			tab = gtk_text_buffer_get_tag_table(
				GTK_TEXT_BUFFER(buf));

			tag = gtk_text_tag_table_lookup(tab, "zoomlevel");
			if (!tag) {
				tag = gtk_text_tag_new("zoomlevel");
				gtk_text_tag_table_add(tab, GTK_TEXT_TAG(tag));
			}

			gdouble fscale = ((gdouble) viewdata_zoomlevel) / 10;

			g_object_set(GTK_TEXT_TAG(tag), "scale", fscale, NULL);

			GtkTextIter start, end;

			gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(buf),
						   &start, &end);
			gtk_text_buffer_remove_all_tags(GTK_TEXT_BUFFER(buf),
							&start,	&end);
			gtk_text_buffer_apply_tag(GTK_TEXT_BUFFER(buf),
						  GTK_TEXT_TAG(tag), &start, &end);
		}
		nsg = nsg->next;
	}
}

gboolean nsgtk_on_viewdata_zoom_in_activate(GtkMenuItem *widget, gpointer g)
{
	viewdata_zoomlevel++;
	nsgtk_viewdata_update_zoomlevel(g);

	return TRUE;
}

gboolean nsgtk_on_viewdata_zoom_out_activate(GtkMenuItem *widget, gpointer g)
{
	if (viewdata_zoomlevel > 1) {
		viewdata_zoomlevel--;
		nsgtk_viewdata_update_zoomlevel(g);
	}

	return TRUE;
}


gboolean nsgtk_on_viewdata_zoom_normal_activate(GtkMenuItem *widget, gpointer g)
{
	viewdata_zoomlevel = 10;
	nsgtk_viewdata_update_zoomlevel(g);

	return TRUE;
}

gboolean nsgtk_on_viewdata_about_activate(GtkMenuItem *widget, gpointer g)
{
	struct nsgtk_viewdata_ctx *nsg = (struct nsgtk_viewdata_ctx *) g;

	nsgtk_about_dialog_init(nsg->window);

	return TRUE;
}

/**
 * View the data in a gtk text window.
 */
static nserror
window_init(const char *title,
	    const char *filename,
	    char *ndata,
	    size_t ndata_len)
{
	GtkWindow *window;
	GtkWidget *cutbutton;
	GtkWidget *pastebutton;
	GtkWidget *deletebutton;
	GtkWidget *printbutton;
	GtkTextView *dataview;
	PangoFontDescription *fontdesc;
	GtkTextBuffer *tb;
	struct nsgtk_viewdata_ctx *newctx;
	nserror res;

	newctx = malloc(sizeof(struct nsgtk_viewdata_ctx));
	if (newctx == NULL) {
		return NSERROR_NOMEM;
	}

	res = nsgtk_builder_new_from_resname("viewdata", &newctx->builder);
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Viewdata UI builder init failed");
		free(newctx);
		return res;
	}

	gtk_builder_connect_signals(newctx->builder, NULL);

	window = GTK_WINDOW(gtk_builder_get_object(newctx->builder,
						   "ViewDataWindow"));
	if (window == NULL) {
		NSLOG(netsurf, INFO, "Unable to find window in builder ");

		/* free the builder */
		g_object_unref(G_OBJECT(newctx->builder));

		/* free the context structure */
		free(newctx);

		return NSERROR_INIT_FAILED;
	}

	cutbutton = GTK_WIDGET(gtk_builder_get_object(newctx->builder, "viewdata_cut"));
	pastebutton = GTK_WIDGET(gtk_builder_get_object(newctx->builder, "viewdata_paste"));
	deletebutton = GTK_WIDGET(gtk_builder_get_object(newctx->builder, "viewdata_delete"));
	printbutton = GTK_WIDGET(gtk_builder_get_object(newctx->builder, "viewdata_print"));
	gtk_widget_set_sensitive(cutbutton, FALSE);
	gtk_widget_set_sensitive(pastebutton, FALSE);
	gtk_widget_set_sensitive(deletebutton, FALSE);
	/* for now */
	gtk_widget_set_sensitive(printbutton, FALSE);


	newctx->filename = strdup(filename);

	newctx->data = ndata;
	newctx->data_len = ndata_len;

	newctx->window = window;

	newctx->next = nsgtk_viewdata_list;
	newctx->prev = NULL;
	if (nsgtk_viewdata_list != NULL) {
		nsgtk_viewdata_list->prev = newctx;
	}
	nsgtk_viewdata_list = newctx;

	nsgtk_attach_viewdata_menu_handlers(newctx->builder, newctx);

	gtk_window_set_title(window, title);

	g_signal_connect(G_OBJECT(window), "destroy",
			 G_CALLBACK(nsgtk_viewdata_destroy_event),
			 newctx);
	g_signal_connect(G_OBJECT(window), "delete-event",
			 G_CALLBACK(nsgtk_viewdata_delete_event),
			 newctx);

	dataview = GTK_TEXT_VIEW(gtk_builder_get_object(newctx->builder,
							"viewdata_view"));

	fontdesc = pango_font_description_from_string("Monospace 8");

	newctx->gv = dataview;
	nsgtk_widget_modify_font(GTK_WIDGET(dataview), fontdesc);

	tb = gtk_text_view_get_buffer(dataview);
	gtk_text_buffer_set_text(tb, newctx->data, -1);

	gtk_widget_show(GTK_WIDGET(window));

	return NSERROR_OK;
}

/**
 * open a window to dispaly an existing file.
 */
static nserror
window_init_fname(const char *title,
	       const char *leafname,
	       const char *filename)
{
	nserror ret;
	FILE *f;
	char *ndata;
	long tell_len;
	size_t ndata_len;

	f = fopen(filename, "r");
	if (f == NULL) {
		return NSERROR_NOT_FOUND;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NSERROR_BAD_SIZE;
	}

	tell_len = ftell(f);
	if (tell_len == -1) {
		fclose(f);
		return NSERROR_BAD_SIZE;		
	}

	if (fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NSERROR_BAD_SIZE;
	}

	ndata = malloc(tell_len);

	ndata_len = fread(ndata, 1, tell_len, f);
	
	fclose(f);

	/* window init takes ownership of the ndata if there is no error */
	ret = window_init(title, leafname, ndata, ndata_len);
	if (ret != NSERROR_OK) {
		free(ndata);
	}

	return ret;
}

/**
 * open a new tab from an existing file.
 */
static nserror
tab_init_fname(const char *title,
	       const char *leafname,
	       const char *fname)
{
	nsurl *url;
	nserror ret;

	/* Open tab on temporary file */
	ret = netsurf_path_to_nsurl(fname, &url);
	if (ret != NSERROR_OK) {
		return ret;
	}

	/* open tab on temportary file */
	ret = browser_window_create(BW_CREATE_TAB | BW_CREATE_HISTORY, url, NULL, NULL, NULL);
	nsurl_unref(url);
	if (ret != NSERROR_OK) {
		return ret;
	}

	return NSERROR_OK;
}

/**
 * create a new tab from data.
 */
static nserror
tab_init(const char *title,
	 const char *leafname,
	 char *ndata,
	 size_t ndata_len)
{
	nserror ret;
	gchar *fname;
	gint handle;
	FILE *f;

	handle = g_file_open_tmp("nsgtkdataXXXXXX", &fname, NULL);
	if ((handle == -1) || (fname == NULL)) {
		return NSERROR_SAVE_FAILED;
	}
	close(handle); /* in case it was binary mode */

	/* save data to temporary file */
	f = fopen(fname, "w");
	if (f == NULL) {
		nsgtk_warning(messages_get("gtkSourceTabError"), 0);
		g_free(fname);
		return NSERROR_SAVE_FAILED;
	}
	fprintf(f, "%s", ndata);
	fclose(f);

	ret = tab_init_fname(title, leafname, fname);
	if (ret == NSERROR_OK) {
		free(ndata);
	}

	g_free(fname);

	return ret;
}


/**
 * Build string vector of search path.
 *
 * ${XDG_DATA_HOME:-$HOME/.local/share}:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}
 *
 * $XDG_DATA_HOME if empty use $HOME/.local/share
 *
 * XDG_DATA_DIRS if empty use /usr/local/share/:/usr/share/
 *
 * \return string vector of search pathnames or NULL on error.
 */
static char** xdg_data_strvec(void)
{
	const char *xdg_data_dirs;
	const char *xdg_data_home;
	const char *home_dir;
	char *xdg_data_path;
	int xdg_data_size;
	char **svec;

	xdg_data_dirs = getenv("XDG_DATA_DIRS");
	if ((xdg_data_dirs == NULL) ||
	    (*xdg_data_dirs == 0) ||
	    (strlen(xdg_data_dirs) > 4096)) {
		xdg_data_dirs = "/usr/local/share/:/usr/share/";
	}

	xdg_data_home = getenv("XDG_DATA_HOME");
	if ((xdg_data_home == NULL) ||
	    (*xdg_data_home == 0) ||
	    (strlen(xdg_data_home) > 4096)) {
		/* $XDG_DATA_HOME is empty use $HOME/.local/share */

		home_dir = getenv("HOME");
		if ((home_dir == NULL) ||
		    (*home_dir == 0) ||
		    (strlen(home_dir) > 4096)) {
			xdg_data_path = strdup(xdg_data_dirs);
		} else {
			xdg_data_size = strlen(home_dir) +
				SLEN("/.local/share:") +
				strlen(xdg_data_dirs) + 1;
			xdg_data_path = malloc(xdg_data_size);
			snprintf(xdg_data_path, xdg_data_size ,
				 "%s/.local/share/:%s",
				 home_dir, xdg_data_dirs);
		}
	} else {
		xdg_data_size = strlen(xdg_data_home) +
			strlen(xdg_data_dirs) + 2;
		xdg_data_path = malloc(xdg_data_size);
		snprintf(xdg_data_path, xdg_data_size , "%s:%s",
			 xdg_data_home, xdg_data_dirs);
	}

	NSLOG(netsurf, INFO, "%s", xdg_data_path);

	svec = filepath_path_to_strvec(xdg_data_path);
	free(xdg_data_path);

	return svec;
}

/**
 * Search application defaults file for matching mime type.
 *
 * create filename form path and applications/defaults.list
 *
 * look for [Default Applications]
 * search lines looking like mime/type=Desktop
 *
 * \param path The base path.
 * \param mimetype The mimetype to search for.
 * \return The desktop file associated with the mime type or NULL if not found.
 */
static char *xdg_get_default_app(const char *path, const char *mimetype)
{
	FILE *fp;
	char *line = NULL;
	size_t len = 0;
	ssize_t rd;
	int fname_len;
	char *fname;
	int mimetype_len;
	char *ret = NULL;

	fname_len = strlen(path) + SLEN("/applications/defaults.list") + 1;
	fname = malloc(fname_len);
	snprintf(fname, fname_len, "%s/applications/defaults.list", path);

	NSLOG(netsurf, INFO, "Checking %s", fname);

	fp = fopen(fname, "r");
	free(fname);
	if (fp == NULL) {
		return NULL;
	}

	mimetype_len = strlen(mimetype);
	while ((rd = getline(&line, &len, fp)) != -1) {
		/* line includes line endings if present, remove them */
		while ((line[rd - 1] == '\n') || (line[rd - 1] == '\r')) {
			rd--;
		}
		line[rd] = 0;

		/* look for mimetype */
		if ((rd > mimetype_len) &&
		    (line[mimetype_len] == '=') &&
		    (strncmp(line, mimetype, mimetype_len) == 0)) {

			ret = strdup(line + mimetype_len + 1);

			NSLOG(netsurf, INFO,
			      "Found line match for %s length %zu\n",
			      mimetype,
			      rd);
			NSLOG(netsurf, INFO, "Result %s", ret);

			break;
		}
	}

	free(line);
	fclose(fp);

	return ret;
}

/**
 * Search desktop file for an Exec line.
 *
 * search path is combined with applications/application.desktop to
 * create a filename.
 *
 * Desktop file format http://standards.freedesktop.org/desktop-entry-spec/desktop-entry-spec-latest.html
 *
 * \todo The parsing of the desktop file is badly incomplete and needs
 * improving. For example the handling of the = delimiter is wrong and
 * selection from the "Desktop Entry" group is completely absent.
 *
 */
static char *xdg_get_exec_cmd(const char *path, const char *desktop)
{
	FILE *fp;
	char *line = NULL;
	size_t len = 0;
	ssize_t rd;
	int fname_len;
	char *fname;
	char *ret = NULL;

	fname_len = strlen(path) + SLEN("/applications/") + strlen(desktop) + 1;
	fname = malloc(fname_len);
	snprintf(fname, fname_len, "%s/applications/%s", path, desktop);

	NSLOG(netsurf, INFO, "Checking %s", fname);

	fp = fopen(fname, "r");
	free(fname);
	if (fp == NULL) {
		return NULL;
	}

	while ((rd = getline(&line, &len, fp)) != -1) {
		/* line includes line endings if present, remove them */
		while ((line[rd - 1] == '\n') || (line[rd - 1] == '\r')) {
			rd--;
		}
		line[rd] = 0;

		/* look for mimetype */
		if ((rd > (ssize_t)SLEN("Exec=")) &&
		    (strncmp(line, "Exec=", SLEN("Exec=")) == 0)) {

			ret = strdup(line + SLEN("Exec="));

			NSLOG(netsurf, INFO, "Found Exec length %zu", rd);
			NSLOG(netsurf, INFO, "Result %s", ret);

			break;
		}
	}

	free(line);
	fclose(fp);

	return ret;
}

static char *exec_arg(const char *arg, int len, const char *fname)
{
	char *res = NULL;

	if (*arg == '%') {
		arg++;
		if ((*arg == 'f') || (*arg == 'F') ||
		    (*arg == 'u') || (*arg == 'U')) {
			res = strdup(fname);
		}
	} else {
		res = calloc(1, len + 1);
		if (res != NULL) {
			memcpy(res, arg, len);
		}
	}

	return res;
}

/**
 * Build vector for executing app.
 */
static char **build_exec_argv(const char *fname, const char *exec_cmd)
{
	char **argv;
	const char *start; /* current arguments start */
	const char *cur; /* current ptr within exec cmd */
	int aidx = 0; /* argv index */

	argv = calloc(10, sizeof(char *));
	if (argv == NULL) {
		return NULL;
	}

	cur = exec_cmd;
	while (*cur != 0) {
		/* skip whitespace */
		while ((*cur != 0) && (*cur == ' ')) {
			cur++;
		}
		if (*cur == 0) {
			break;
		}
		start = cur;

		/* find end of element */
		while ((*cur != 0) && (*cur != ' ')) {
			cur++;
		}

		argv[aidx] = exec_arg(start, cur - start, fname);
		if (argv[aidx] != NULL) {
			NSLOG(netsurf, INFO, "adding \"%s\"", argv[aidx]);
			aidx++;
		}
	}

	/* if no arguments were found there was nothing to execute */
	if (aidx == 0) {
		free(argv);
		return NULL;
	}

	return argv;
}


/**
 * open an editor from an existing file.
 */
static nserror
editor_init_fname(const char *title,
	       const char *leafname,
	       const char *fname)
{
	char **xdg_data_vec;
	int veci;
	/* desktop file of default app for mimetype */
	char *def_app_desktop = NULL;
	char *exec_cmd = NULL;
	char **argv;

	/* build string vector of search path */
	xdg_data_vec = xdg_data_strvec();

	/* find user configured app for opening text/plain */
	veci = 0;
	while (xdg_data_vec[veci] != NULL) {
		def_app_desktop = xdg_get_default_app(xdg_data_vec[veci],
						  "text/plain");
		if (def_app_desktop != NULL) {
			break;
		}
		veci++;
	}

	if (def_app_desktop == NULL) {
		/* no default app */
		filepath_free_strvec(xdg_data_vec);
		return NSERROR_NOT_FOUND;
	}

	/* find app to execute */
	veci = 0;
	while (xdg_data_vec[veci] != NULL) {
		exec_cmd = xdg_get_exec_cmd(xdg_data_vec[veci], def_app_desktop);
		if (exec_cmd != NULL) {
			break;
		}
		veci++;
	}
	free(def_app_desktop);
	filepath_free_strvec(xdg_data_vec);

	if (exec_cmd == NULL) {
		/* no exec entry */
		return NSERROR_NOT_FOUND;
	}

	/* build exec vector */
	argv = build_exec_argv(fname, exec_cmd);
	free(exec_cmd);

	/* execute target app on saved data */
	if (g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
			  NULL, NULL) != TRUE) {
		return NSERROR_NOT_FOUND;
	}
	filepath_free_strvec(argv);

	return NSERROR_OK;
}

/**
 * open an editor with data.
 */
static nserror
editor_init(const char *title,
	    const char *leafname,
	    char *ndata,
	    size_t ndata_len)
{

	nserror ret;
	gchar *fname;
	gint handle;
	FILE *f;

	handle = g_file_open_tmp("nsgtkdataXXXXXX", &fname, NULL);
	if ((handle == -1) || (fname == NULL)) {
		return NSERROR_SAVE_FAILED;
	}
	close(handle); /* in case it was binary mode */

	/* save data to temporary file */
	f = fopen(fname, "w");
	if (f == NULL) {
		nsgtk_warning(messages_get("gtkSourceTabError"), 0);
		g_free(fname);
		return NSERROR_SAVE_FAILED;
	}
	fprintf(f, "%s", ndata);
	fclose(f);

	ret = editor_init_fname(title, leafname, fname);
	if (ret == NSERROR_OK) {
		free(ndata);
	}

	g_free(fname);

	return ret;
}

/* exported interface documented in gtk/viewdata.h */
nserror
nsgtk_viewdata(const char *title,
	       const char *filename,
	       char *ndata,
	       size_t ndata_len)
{
	nserror ret;

	switch (nsoption_int(developer_view)) {
	case 0:
		ret = window_init(title, filename, ndata, ndata_len);
		break;

	case 1:
		ret = tab_init(title, filename, ndata, ndata_len);
		break;

	case 2:
		ret = editor_init(title, filename, ndata, ndata_len);
		break;

	default:
		ret = NSERROR_BAD_PARAMETER;
		break;
	}
	if (ret != NSERROR_OK) {
		/* release the data */
		free(ndata);
	}


	return ret;
}

/* exported interface documented in gtk/viewdata.h */
nserror
nsgtk_viewfile(const char *title,
	       const char *leafname,
	       const char *filename)
{
	nserror ret;

	switch (nsoption_int(developer_view)) {
	case 0:
		ret = window_init_fname(title, leafname, filename);
		break;

	case 1:
		ret = tab_init_fname(title, leafname, filename);
		break;

	case 2:
		ret = editor_init_fname(title, leafname, filename);
		break;

	default:
		ret = NSERROR_BAD_PARAMETER;
		break;
	}

	return ret;
}
