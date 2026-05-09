/*
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * Implementation of url entry completion.
 */

#include <stdlib.h>

#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "netsurf/url_db.h"
#include "netsurf/browser_window.h"
#include "desktop/searchweb.h"

#include "gtk/compat.h"
#include "gtk/warn.h"
#include "gtk/scaffolding.h"
#include "gtk/toolbar_items.h"
#include "gtk/window.h"
#include "gtk/completion.h"

GtkListStore *nsgtk_completion_list;

struct nsgtk_completion_ctx {
	/**
	 * callback to obtain a browser window for navigation
	 */
	struct browser_window *(*get_bw)(void *ctx);

	/**
	 * context passed to get_bw function
	 */
	void *get_bw_ctx;
};

/**
 * completion row matcher
 */
static gboolean nsgtk_completion_match(GtkEntryCompletion *completion,
				const gchar *key,
				GtkTreeIter *iter,
				gpointer user_data)
{
	/* the completion list is modified to only contain valid
	 * entries so this simply returns TRUE to indicate all rows
	 * are in the list should be shown.
	 */
	return TRUE;
}


/**
 * callback for each entry to add to completion list
 */
static bool
nsgtk_completion_udb_callback(nsurl *url, const struct url_data *data)
{
	GtkTreeIter iter;

	if (data->visits != 0) {
		gtk_list_store_append(nsgtk_completion_list, &iter);
		gtk_list_store_set(nsgtk_completion_list, &iter, 0,
				nsurl_access(url), -1);
	}
	return true;
}

/**
 * event handler for when a completion suggestion is selected.
 */
static gboolean
nsgtk_completion_match_select(GtkEntryCompletion *widget,
			      GtkTreeModel *model,
			      GtkTreeIter *iter,
			      gpointer data)
{
	struct nsgtk_completion_ctx *cb_ctx;
	GValue value = G_VALUE_INIT;
	struct browser_window *bw;
	nserror ret;
	nsurl *url;

	cb_ctx = data;
	bw = cb_ctx->get_bw(cb_ctx->get_bw_ctx);

	gtk_tree_model_get_value(model, iter, 0, &value);

	ret = search_web_omni(g_value_get_string(&value),
			      SEARCH_WEB_OMNI_NONE,
			      &url);

	g_value_unset(&value);

	if (ret == NSERROR_OK) {
		ret = browser_window_navigate(bw,
					      url, NULL, BW_NAVIGATE_HISTORY,
					      NULL, NULL, NULL);
		nsurl_unref(url);
	}
	if (ret != NSERROR_OK) {
		nsgtk_warning(messages_get_errorcode(ret), 0);
	}

	return TRUE;
}

/* exported interface documented in completion.h */
void nsgtk_completion_init(void)
{
	nsgtk_completion_list = gtk_list_store_new(1, G_TYPE_STRING);

}

/* exported interface documented in completion.h */
gboolean nsgtk_completion_update(GtkEntry *entry)
{
	gtk_list_store_clear(nsgtk_completion_list);

	if (nsoption_bool(url_suggestion) == true) {
		urldb_iterate_partial(gtk_entry_get_text(entry),
				      nsgtk_completion_udb_callback);
	}

	return TRUE;
}

/* exported interface documented in completion.h */
nserror
nsgtk_completion_connect_signals(GtkEntry *entry,
				 struct browser_window *(*get_bw)(void *ctx),
				 void *get_bw_ctx)
{
	GtkEntryCompletion *completion;
	struct nsgtk_completion_ctx *cb_ctx;

	cb_ctx = calloc(1, sizeof(struct nsgtk_completion_ctx));
	if (cb_ctx == NULL) {
		return NSERROR_NOMEM;
	}

	cb_ctx->get_bw = get_bw;
	cb_ctx->get_bw_ctx = get_bw_ctx;

	completion = gtk_entry_get_completion(entry);

	gtk_entry_completion_set_match_func(completion,
			nsgtk_completion_match, NULL, NULL);

	gtk_entry_completion_set_model(completion,
			GTK_TREE_MODEL(nsgtk_completion_list));

	gtk_entry_completion_set_text_column(completion, 0);

	gtk_entry_completion_set_minimum_key_length(completion, 1);

	/* enable popup for completion */
	gtk_entry_completion_set_popup_completion(completion, TRUE);

	/* when selected callback */
	g_signal_connect(G_OBJECT(completion),
			 "match-selected",
			 G_CALLBACK(nsgtk_completion_match_select),
			 cb_ctx);

	g_object_set(G_OBJECT(completion),
		     "popup-set-width", TRUE,
		     "popup-single-match", TRUE,
		     NULL);

	return NSERROR_OK;
}
