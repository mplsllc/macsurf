/*
 * Copyright 2012 John-Mark Bell <jmb@netsurf-browser.org>
 * Copyright 2013 Michael Drake <tlsa@netsurf-browser.org>
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

#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <dom/dom.h>
#include <dom/bindings/hubbub/parser.h>

#include "utils/corestrings.h"
#include "utils/messages.h"
#include "utils/utils.h"
#include "utils/utf8.h"
#include "utils/libdom.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "content/urldb.h"

#include "netsurf/misc.h"
#include "desktop/gui_internal.h"
#include "desktop/hotlist.h"
#include "desktop/treeview.h"
#include "netsurf/browser_window.h"

#define N_DAYS 28
#define N_SEC_PER_DAY (60 * 60 * 24)

enum hotlist_fields {
	HL_TITLE,
	HL_URL,
	HL_LAST_VISIT,
	HL_VISITS,
	HL_FOLDER,
	HL_N_FIELDS
};

struct hotlist_folder {
	treeview_node *folder;
	struct treeview_field_data data;
};

struct hotlist_ctx {
	treeview *tree;
	struct treeview_field_desc fields[HL_N_FIELDS];
	bool built;
	struct hotlist_folder *default_folder;
	char *save_path;
	bool save_scheduled;
};
struct hotlist_ctx hl_ctx;

struct hotlist_entry {
	nsurl *url;
	treeview_node *entry;

	struct treeview_field_data data[HL_N_FIELDS - 1];
};


/*
 * Get path for writing hotlist to
 *
 * \param path		The final path of the hotlist
 * \param loaded	Updated to the path to write the holist to
 * \return NSERROR_OK on success, or appropriate error otherwise
 */
static nserror hotlist_get_temp_path(const char *path, char **temp_path)
{
	const char *extension = "-bk";
	char *joined;
	int len;

	len = strlen(path) + strlen(extension);

	joined = malloc(len + 1);
	if (joined == NULL) {
		return NSERROR_NOMEM;
	}

	if (snprintf(joined, len + 1, "%s%s", path, extension) != len) {
		free(joined);
		return NSERROR_UNKNOWN;
	}

	*temp_path = joined;
	return NSERROR_OK;
}


/* Save the hotlist to to a file at the given path
 *
 * \param path  Path to save hotlist file to.  NULL path is a no-op.
 * \return NSERROR_OK on success, or appropriate error otherwise
 */
static nserror hotlist_save(const char *path)
{
	nserror res = NSERROR_OK;
	char *temp_path;

	/* NULL path is a no-op. */
	if (path == NULL) {
		return NSERROR_OK;
	}

	/* Get path to export to */
	res = hotlist_get_temp_path(path, &temp_path);
	if (res != NSERROR_OK) {
		return res;
	}

	/* Export to temp path */
	res = hotlist_export(temp_path, NULL);
	if (res != NSERROR_OK) {
		goto cleanup;
	}

	/* Remove old hotlist to handle non-POSIX rename() implementations. */
	(void)remove(path);

	/* Replace any old hotlist file with the one we just saved */
	if (rename(temp_path, path) != 0) {
		res = NSERROR_SAVE_FAILED;
		NSLOG(netsurf, INFO, "Error renaming hotlist: %s.",
		      strerror(errno));
		goto cleanup;
	}

cleanup:
	free(temp_path);

	return res;
}


/**
 * Scheduler callback for saving the hotlist.
 *
 * \param p  Unused user data.
 */
static void hotlist_schedule_save_cb(void *p)
{
	hl_ctx.save_scheduled = false;
	hotlist_save(hl_ctx.save_path);
}


/**
 * Schedule a hotlist save.
 *
 * \return NSERROR_OK on success, or appropriate error otherwise
 */
static nserror hotlist_schedule_save(void)
{
	if (hl_ctx.save_scheduled == false && hl_ctx.save_path != NULL) {
		nserror err = guit->misc->schedule(10 * 1000,
				hotlist_schedule_save_cb, NULL);
		if (err != NSERROR_OK) {
			return err;
		}
		hl_ctx.save_scheduled = true;
	}

	return NSERROR_OK;
}


/**
 * Set a hotlist entry's data from the url_data.
 *
 * \param e    hotlist entry to set up.
 * \param data Data associated with entry's URL.
 * \return NSERROR_OK on success, appropriate error otherwise.
 */
static nserror hotlist_create_treeview_field_visits_data(
		struct hotlist_entry *e, const struct url_data *data)
{
	char buffer[16];
	char *last_visited = NULL;
	size_t len = 0;

	/* Last visited */
	if (data->visits != 0) {
		const size_t lvsize = 256;
		struct tm *lvtime;

		if ((lvtime = localtime(&data->last_visit)) != NULL) {
			last_visited = malloc(lvsize);
			if (last_visited != NULL) {
				len = strftime(last_visited, lvsize,
						"%a %b %e %H:%M:%S %Y",
						lvtime);
			}
		}
	} else {
		last_visited = strdup("-");
		len = 1;
	}
	if (last_visited == NULL) {
		return NSERROR_NOMEM;
	}

	e->data[HL_LAST_VISIT].field = hl_ctx.fields[HL_LAST_VISIT].field;
	e->data[HL_LAST_VISIT].value = last_visited;
	e->data[HL_LAST_VISIT].value_len = len;

	/* Visits */
	len = snprintf(buffer, 16, "%u", data->visits);
	if (len == 16) {
		len--;
		buffer[len] = '\0';
	}

	e->data[HL_VISITS].field = hl_ctx.fields[HL_VISITS].field;
	e->data[HL_VISITS].value = strdup(buffer);
	if (e->data[HL_VISITS].value == NULL) {
		free((void*)e->data[HL_LAST_VISIT].value);
		return NSERROR_NOMEM;
	}
	e->data[HL_VISITS].value_len = len;

	return NSERROR_OK;
}


/**
 * Set a hotlist entry's data from the url_data.
 *
 * \param e     hotlist entry to set up
 * \param title Title for entry, or NULL if using title from data
 * \param data  Data associated with entry's URL
 * \return NSERROR_OK on success, appropriate error otherwise
 */
static nserror hotlist_create_treeview_field_data(
		struct hotlist_entry *e, const char *title,
		const struct url_data *data)
{
	nserror err;

	/* "URL" field */
	e->data[HL_URL].field = hl_ctx.fields[HL_URL].field;
	e->data[HL_URL].value = nsurl_access(e->url);
	e->data[HL_URL].value_len = nsurl_length(e->url);

	/* "Title" field */
	if (title == NULL) {
		/* Title not provided; use one from URL data */
		title = (data->title != NULL) ?
				data->title : nsurl_access(e->url);
	}

	e->data[HL_TITLE].field = hl_ctx.fields[HL_TITLE].field;
	e->data[HL_TITLE].value = strdup(title);
	if (e->data[HL_TITLE].value == NULL) {
		return NSERROR_NOMEM;
	}
	e->data[HL_TITLE].value_len = (e->data[HL_TITLE].value != NULL) ?
			strlen(title) : 0;

	/* "Last visited" and "Visits" fields */
	err = hotlist_create_treeview_field_visits_data(e, data);
	if (err != NSERROR_OK) {
		free((void*)e->data[HL_TITLE].value);
		return NSERROR_OK;
	}

	return NSERROR_OK;
}


/**
 * Add a hotlist entry to the treeview
 *
 * \param e		Entry to add to treeview
 * \param relation	Existing node to insert as relation of, or NULL
 * \param rel		Folder's relationship to relation
 * \return NSERROR_OK on success, or appropriate error otherwise
 *
 * It is assumed that the entry is unique (for its URL) in the global
 * hotlist table
 */
static nserror hotlist_entry_insert(struct hotlist_entry *e,
		treeview_node *relation, enum treeview_relationship rel)
{
	nserror err;

	err = treeview_create_node_entry(hl_ctx.tree, &(e->entry),
			relation, rel, e->data, e, hl_ctx.built ?
			TREE_OPTION_NONE : TREE_OPTION_SUPPRESS_RESIZE |
					TREE_OPTION_SUPPRESS_REDRAW);
	if (err != NSERROR_OK) {
		return err;
	}

	return NSERROR_OK;
}


/**
 * Delete a hotlist entry
 *
 * This does not delete the treeview node, rather it should only be called from
 * the treeview node delete event message.
 *
 * \param e		Entry to delete
 */
static void hotlist_delete_entry_internal(struct hotlist_entry *e)
{
	assert(e != NULL);
	assert(e->entry == NULL);

	/* Destroy fields */
	free((void *)e->data[HL_TITLE].value); /* Eww */
	free((void *)e->data[HL_LAST_VISIT].value); /* Eww */
	free((void *)e->data[HL_VISITS].value); /* Eww */
	nsurl_unref(e->url);

	/* Destroy entry */
	free(e);
}


/**
 * Create hotlist entry data for URL.
 *
 * \param url		URL for entry to add to hotlist.
 * \param title		Title for entry, or NULL if using title from data
 * \param data		URL data for the entry, or NULL
 * \param entry		Updated to new hotlist entry data
 * \return NSERROR_OK on success, or appropriate error otherwise
 */
static nserror hotlist_create_entry(nsurl *url, const char *title,
		const struct url_data *data, struct hotlist_entry **entry)
{
	nserror err;
	struct hotlist_entry *e;

	assert(url != NULL);

	*entry = NULL;

	if (data == NULL) {
		/* Get the URL data */
		data = urldb_get_url_data(url);
		if (data == NULL) {
			/* No entry in database, so add one */
			urldb_add_url(url);
			/* now attempt to get url data */
			data = urldb_get_url_data(url);
		}
		if (data == NULL) {
			return NSERROR_NOMEM;
		}
	}

	/* Create new local hotlist entry */
	e = malloc(sizeof(struct hotlist_entry));
	if (e == NULL) {
		return NSERROR_NOMEM;
	}

	e->url = nsurl_ref(url);
	e->entry = NULL;

	err = hotlist_create_treeview_field_data(e, title, data);
	if (err != NSERROR_OK) {
		nsurl_unref(e->url);
		free(e);
		return err;
	}

	*entry = e;

	return NSERROR_OK;
}


/**
 * Add an entry to the hotlist (creates the entry).
 *
 * \param url		URL for entry to add to hotlist.
 * \param title		Title for entry, or NULL if using title from data
 * \param data		URL data for the entry, or NULL
 * \param relation	Existing node to insert as relation of, or NULL
 * \param rel		Entry's relationship to relation
 * \param entry		Updated to new treeview entry node
 * \return NSERROR_OK on success, or appropriate error otherwise
 */
static nserror hotlist_add_entry_internal(nsurl *url, const char *title,
		const struct url_data *data, treeview_node *relation,
		enum treeview_relationship rel, treeview_node **entry)
{
	nserror err;
	struct hotlist_entry *e;

	err = hotlist_create_entry(url, title, data, &e);
	if (err != NSERROR_OK) {
		return err;
	}

	err = hotlist_entry_insert(e, relation, rel);
	if (err != NSERROR_OK) {
		hotlist_delete_entry_internal(e);
		return err;
	}

	/* Make this URL persistent */
	urldb_set_url_persistence(url, true);

	*entry = e->entry;

	return NSERROR_OK;
}


/**
 * Add folder to the hotlist (creates the folder).
 *
 * \param title		Title for folder, or NULL if using title from data
 * \param relation	Existing node to insert as relation of, or NULL
 * \param rel		Folder's relationship to relation
 * \param folder	Updated to new hotlist folder data
 * \param default_folder Add to the default folder.
 * \return NSERROR_OK on success, or appropriate error otherwise
 */
static nserror hotlist_add_folder_internal(
		const char *title, treeview_node *relation,
		enum treeview_relationship rel, struct hotlist_folder **folder,
		bool default_folder)
{
	struct hotlist_folder *f;
	treeview_node_options_flags flags = TREE_OPTION_NONE;
	treeview_node *n;
	nserror err;

	if (title == NULL) {
		title = messages_get("NewFolderName");
	}

	/* Create the title field */
	f = malloc(sizeof(struct hotlist_folder));
	if (f == NULL) {
		return NSERROR_NOMEM;
	}
	f->data.field = hl_ctx.fields[HL_FOLDER].field;
	f->data.value = strdup(title);
	if (f->data.value == NULL) {
		free(f);
		return NSERROR_NOMEM;
	}
	f->data.value_len = strlen(title);

	if (!hl_ctx.built)
		flags |= TREE_OPTION_SUPPRESS_RESIZE |
				TREE_OPTION_SUPPRESS_REDRAW;
	if (default_folder)
		flags |= TREE_OPTION_SPECIAL_DIR;

	err = treeview_create_node_folder(hl_ctx.tree,
			&n, relation, rel, &f->data, f, flags);
	if (err != NSERROR_OK) {
		free((void*)f->data.value); /* Eww */
		free(f);
		return err;
	}

	f->folder = n;
	*folder = f;

	return NSERROR_OK;
}


static nserror hotlist_tree_node_folder_cb(
		struct treeview_node_msg msg, void *data)
{
	struct hotlist_folder *f = data;
	const char *old_text;
	bool match;

	switch (msg.msg) {
	case TREE_MSG_NODE_DELETE:
		if (f == hl_ctx.default_folder)
			hl_ctx.default_folder = NULL;
		free((void*)f->data.value); /* Eww */
		free(f);
		break;

	case TREE_MSG_NODE_EDIT:
		if (lwc_string_isequal(hl_ctx.fields[HL_FOLDER].field,
				msg.data.node_edit.field, &match) ==
				lwc_error_ok && match == true &&
				msg.data.node_edit.text != NULL &&
				msg.data.node_edit.text[0] != '\0') {
			/* Requst to change the folder title text */
			old_text = f->data.value;
			f->data.value = strdup(msg.data.node_edit.text);

			if (f->data.value == NULL) {
				f->data.value = old_text;
			} else {
				f->data.value_len = strlen(f->data.value);
				treeview_update_node_folder(hl_ctx.tree,
						f->folder, &f->data, f);
				free((void *)old_text);
			}
		}
		break;

	case TREE_MSG_NODE_LAUNCH:
		break;
	}

	return NSERROR_OK;
}

/**
 * callback for hotlist treeview entry manipulation.
 */
static nserror
hotlist_tree_node_entry_cb(struct treeview_node_msg msg, void *data)
{
	struct hotlist_entry *e = data;
	const char *old_text;
	nsurl *old_url;
	nsurl *url;
	nserror err = NSERROR_OK;
	bool match;

	switch (msg.msg) {
	case TREE_MSG_NODE_DELETE:
		e->entry = NULL;
		hotlist_delete_entry_internal(e);

		err = hotlist_schedule_save();
		break;

	case TREE_MSG_NODE_EDIT:
		if (lwc_string_isequal(hl_ctx.fields[HL_TITLE].field,
				msg.data.node_edit.field, &match) ==
				lwc_error_ok && match == true &&
				msg.data.node_edit.text != NULL &&
				msg.data.node_edit.text[0] != '\0') {
			/* Requst to change the entry title text */
			old_text = e->data[HL_TITLE].value;
			e->data[HL_TITLE].value =
					strdup(msg.data.node_edit.text);

			if (e->data[HL_TITLE].value == NULL) {
				e->data[HL_TITLE].value = old_text;
			} else {
				e->data[HL_TITLE].value_len =
						strlen(e->data[HL_TITLE].value);
				treeview_update_node_entry(hl_ctx.tree,
						e->entry, e->data, e);
				free((void *)old_text);
			}

			err = hotlist_schedule_save();

		} else if (lwc_string_isequal(hl_ctx.fields[HL_URL].field,
				msg.data.node_edit.field, &match) ==
				lwc_error_ok && match == true &&
				msg.data.node_edit.text != NULL &&
				msg.data.node_edit.text[0] != '\0') {
			/* Requst to change the entry URL text */
			err = nsurl_create(msg.data.node_edit.text, &url);
			if (err == NSERROR_OK) {
				old_url = e->url;

				e->url = url;
				e->data[HL_URL].value = nsurl_access(url);
				e->data[HL_URL].value_len = nsurl_length(e->url);

				treeview_update_node_entry(hl_ctx.tree,
						   e->entry, e->data, e);
				nsurl_unref(old_url);

				err = hotlist_schedule_save();
			}
		}
		break;

	case TREE_MSG_NODE_LAUNCH:
	{
		struct browser_window *existing = NULL;
		enum browser_window_create_flags flags = BW_CREATE_HISTORY;

		/* TODO: Set existing to window that new tab appears in */

		if (msg.data.node_launch.mouse &
				(BROWSER_MOUSE_MOD_1 | BROWSER_MOUSE_MOD_2) ||
				existing == NULL) {
			/* Shift or Ctrl launch, open in new window rather
			 * than tab. */
			/* TODO: flags ^= BW_CREATE_TAB; */
		}

		err = browser_window_create(flags, e->url, NULL,
				existing, NULL);
	}
		break;
	}
	return err;
}

struct treeview_callback_table hl_tree_cb_t = {
	.folder = hotlist_tree_node_folder_cb,
	.entry = hotlist_tree_node_entry_cb
};



typedef struct {
	treeview *tree;
	treeview_node *rel;
	enum treeview_relationship relshp;
	bool last_was_h4;
	dom_string *title;
} hotlist_load_ctx;


/**
 * Parse an entry represented as a li.
 *
 * \param li DOM node for parsed li
 * \param ctx Our hotlist loading context.
 * \return NSERROR_OK on success, or appropriate error otherwise
 */
static nserror hotlist_load_entry(dom_node *li, hotlist_load_ctx *ctx)
{
	dom_node *a;
	dom_string *title1, *url1;
	const char *title;
	nsurl *url;
	dom_exception derror;
	nserror err;

	/* The li must contain an "a" element */
	a = libdom_find_first_element(li, corestring_lwc_a);
	if (a == NULL) {
		NSLOG(netsurf, INFO, "Missing <a> in <li>");
		return NSERROR_INVALID;
	}

	derror = dom_node_get_text_content(a, &title1);
	if (derror != DOM_NO_ERR) {
		NSLOG(netsurf, INFO, "No title");
		dom_node_unref(a);
		return NSERROR_INVALID;
	}

	derror = dom_element_get_attribute(a, corestring_dom_href, &url1);
	if (derror != DOM_NO_ERR || url1 == NULL) {
		NSLOG(netsurf, INFO, "No URL");
		dom_string_unref(title1);
		dom_node_unref(a);
		return NSERROR_INVALID;
	}
	dom_node_unref(a);

	if (title1 != NULL) {
		title = dom_string_data(title1);
	} else {
		title = messages_get("NoTitle");
	}

	/* Need to get URL as a nsurl object */
	err = nsurl_create(dom_string_data(url1), &url);
	dom_string_unref(url1);

	if (err != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Failed normalising '%s'",
		      dom_string_data(url1));

		if (title1 != NULL) {
			dom_string_unref(title1);
		}

		return err;
	}

	/* Add the entry */
	err = hotlist_add_entry_internal(url, title, NULL, ctx->rel,
			ctx->relshp, &ctx->rel);
	nsurl_unref(url);
	if (title1 != NULL) {
		dom_string_unref(title1);
	}
	ctx->relshp = TREE_REL_NEXT_SIBLING;

	if (err != NSERROR_OK) {
		return err;
	}

	return NSERROR_OK;
}


/*
 * Callback for libdom_iterate_child_elements, which despite the namespace is
 * a NetSurf function.
 *
 * \param node		Node that is a child of the directory UL node
 * \param ctx		Our hotlist loading context.
 */
static nserror hotlist_load_directory_cb(dom_node *node, void *ctx);

/**
 * Parse a directory represented as a ul.
 *
 * \param ul DOM node for parsed ul.
 * \param ctx The hotlist context.
 * \return NSERROR_OK on success, or appropriate error otherwise
 */
static nserror hotlist_load_directory(dom_node *ul, hotlist_load_ctx *ctx)
{
	assert(ul != NULL);
	assert(ctx != NULL);

	return libdom_iterate_child_elements(ul,
			hotlist_load_directory_cb, ctx);
}


/* Documented above, in forward declaration */
nserror hotlist_load_directory_cb(dom_node *node, void *ctx)
{
	/* TODO: return appropriate errors */
	hotlist_load_ctx *current_ctx = ctx;
	dom_string *name;
	dom_exception error;
	nserror err;

	/* The ul may contain entries as a li, or directories as
	 * an h4 followed by a ul. Non-element nodes may be present
	 * (eg. text, comments), and are ignored. */

	error = dom_node_get_node_name(node, &name);
	if (error != DOM_NO_ERR || name == NULL)
		return NSERROR_DOM;

	if (dom_string_caseless_lwc_isequal(name, corestring_lwc_li)) {
		/* Entry handling */
		hotlist_load_entry(node, current_ctx);
		current_ctx->last_was_h4 = false;

	} else if (dom_string_caseless_lwc_isequal(name, corestring_lwc_h4)) {
		/* Directory handling, part 1: Get title from H4 */
		dom_string *title;

		error = dom_node_get_text_content(node, &title);
		if (error != DOM_NO_ERR || title == NULL) {
			NSLOG(netsurf, INFO,
			      "Empty <h4> or memory exhausted.");
			dom_string_unref(name);
			return NSERROR_DOM;
		}

		if (current_ctx->title != NULL)
			dom_string_unref(current_ctx->title);
		current_ctx->title = title;
		current_ctx->last_was_h4 = true;

	} else if (current_ctx->last_was_h4 &&
			dom_string_caseless_lwc_isequal(name, 
					corestring_lwc_ul)) {
		/* Directory handling, part 2: Make node, and handle children */
		const char *title;
		dom_string *id;
		struct hotlist_folder *f;
		hotlist_load_ctx new_ctx;
		treeview_node *rel;
		bool default_folder = false;

		/* Check if folder should be default folder */
		error = dom_element_get_attribute(node, corestring_dom_id, &id);
		if (error != DOM_NO_ERR) {
			dom_string_unref(name);
			return NSERROR_NOMEM;
		}
		if (id != NULL) {
			if (dom_string_lwc_isequal(id, corestring_lwc_default))
				default_folder = true;

			dom_string_unref(id);
		}

		title = dom_string_data(current_ctx->title);

		/* Add folder node */
		err = hotlist_add_folder_internal(title, current_ctx->rel,
				current_ctx->relshp, &f, default_folder);
		if (err != NSERROR_OK) {
			dom_string_unref(name);
			return NSERROR_NOMEM;
		}

		if (default_folder)
			hl_ctx.default_folder = f;

		rel = f->folder;
		current_ctx->rel = rel;
		current_ctx->relshp = TREE_REL_NEXT_SIBLING;

		new_ctx.tree = current_ctx->tree;
		new_ctx.rel = rel;
		new_ctx.relshp = TREE_REL_FIRST_CHILD;
		new_ctx.last_was_h4 = false;
		new_ctx.title = NULL;

		/* And load its contents */
		err = hotlist_load_directory(node, &new_ctx);
		if (err != NSERROR_OK) {
			dom_string_unref(name);
			return NSERROR_NOMEM;
		}

		if (new_ctx.title != NULL) {
			dom_string_unref(new_ctx.title);
			new_ctx.title = NULL;
		}
		current_ctx->last_was_h4 = false;
	} else {
		current_ctx->last_was_h4 = false;
	}

	dom_string_unref(name);

	return NSERROR_OK;
}


/*
 * Load the hotlist data from file
 *
 * \param path		The path to load the hotlist file from, or NULL
 * \param loaded	Updated to true iff hotlist file loaded, else set false
 * \return NSERROR_OK on success, or appropriate error otherwise
 */
static nserror hotlist_load(const char *path, bool *loaded)
{
	dom_document *document;
	dom_node *html, *body, *ul;
	hotlist_load_ctx ctx;
	char *temp_path;
	nserror err;

	*loaded = false;

	/* Handle no path */
	if (path == NULL) {
		NSLOG(netsurf, INFO, "No hotlist file path provided.");
		return NSERROR_OK;
	}

	/* Get temp hotlist write path */
	err = hotlist_get_temp_path(path, &temp_path);
	if (err != NSERROR_OK) {
		return err;
	}

	/* Load hotlist file */
	err = libdom_parse_file(path, "iso-8859-1", &document);
	if (err != NSERROR_OK) {
		err = libdom_parse_file(temp_path, "iso-8859-1", &document);
		if (err != NSERROR_OK) {
			free(temp_path);
			return err;
		}
	}
	free(temp_path);

	/* Find HTML element */
	html = libdom_find_first_element((dom_node *) document,
			corestring_lwc_html);
	if (html == NULL) {
		dom_node_unref(document);
		NSLOG(netsurf, WARNING,
		      "%s (<html> not found)",
		      messages_get("TreeLoadError"));
		return NSERROR_OK;
	}

	/* Find BODY element */
	body = libdom_find_first_element(html, corestring_lwc_body);
	if (body == NULL) {
		dom_node_unref(html);
		dom_node_unref(document);
		NSLOG(netsurf, WARNING,
		      "%s (<html>...<body> not found)",
		      messages_get("TreeLoadError"));
		return NSERROR_OK;
	}

	/* Find UL element */
	ul = libdom_find_first_element(body, corestring_lwc_ul);
	if (ul == NULL) {
		dom_node_unref(body);
		dom_node_unref(html);
		dom_node_unref(document);
		NSLOG(netsurf, WARNING,
		      "%s (<html>...<body>...<ul> not found.)",
		      messages_get("TreeLoadError"));
		return NSERROR_OK;
	}

	/* Set up the hotlist loading context */
	ctx.tree = hl_ctx.tree;
	ctx.rel = NULL;
	ctx.relshp = TREE_REL_FIRST_CHILD;
	ctx.last_was_h4 = false;
	ctx.title = NULL;

	err = hotlist_load_directory(ul, &ctx);

	if (ctx.title != NULL) {
		dom_string_unref(ctx.title);
		ctx.title = NULL;
	}

	dom_node_unref(ul);
	dom_node_unref(body);
	dom_node_unref(html);
	dom_node_unref(document);

	if (err != NSERROR_OK) {
		NSLOG(netsurf, WARNING,
		      "%s (Failed building tree.)",
		      messages_get("TreeLoadError"));
		return NSERROR_OK;
	}

	*loaded = true;

	return NSERROR_OK;
}


/*
 * Generate default hotlist
 *
 * \return NSERROR_OK on success, or appropriate error otherwise
 */
static nserror hotlist_generate(void)
{
	int i;
	struct hotlist_folder *f;
	treeview_node *e;
	const char *title;
	nserror err;
	nsurl *url;
	static const struct {
		const char *url;
		const char *msg_key;
	} default_entries[] = {
		{ "https://www.netsurf-browser.org/",
				"HotlistHomepage" },
		{ "https://www.netsurf-browser.org/downloads/",
				"HotlistDownloads" },
		{ "https://www.netsurf-browser.org/documentation",
				"HotlistDocumentation" },
		{ "https://www.netsurf-browser.org/contact",
				"HotlistContact" }
	};
	const int n_entries = sizeof(default_entries) /
			sizeof(default_entries[0]);

	/* First make "NetSurf" folder for defualt entries */
	title = "NetSurf";
	err = hotlist_add_folder_internal(title, NULL,
			TREE_REL_FIRST_CHILD, &f, false);
	if (err != NSERROR_OK) {
		return err;
	}

	/* And add entries as children of folder node */
	for (i = n_entries - 1; i >= 0; i--) {
		/* Get URL as nsurl object */
		err = nsurl_create(default_entries[i].url, &url);
		if (err != NSERROR_OK) {
			return NSERROR_NOMEM;
		}

		title = messages_get(default_entries[i].msg_key);

		/* Build the node */
		err = hotlist_add_entry_internal(url, title,
				NULL, f->folder, TREE_REL_FIRST_CHILD, &e);
		nsurl_unref(url);

		if (err != NSERROR_OK) {
			return NSERROR_NOMEM;
		}
	}

	return NSERROR_OK;
}


struct treeview_export_walk_ctx {
	FILE *fp;
};

/** Callback for treeview_walk node entering */
static nserror hotlist_export_enter_cb(void *ctx, void *node_data,
		enum treeview_node_type type, bool *abort)
{
	struct treeview_export_walk_ctx *tw = ctx;
	nserror ret;

	if (type == TREE_NODE_ENTRY) {
		struct hotlist_entry *e = node_data;
		char *t_text;
		char *u_text;

		ret = utf8_to_html(e->data[HL_TITLE].value, "iso-8859-1",
				e->data[HL_TITLE].value_len, &t_text);
		if (ret != NSERROR_OK)
			return NSERROR_SAVE_FAILED;

		ret = utf8_to_html(e->data[HL_URL].value, "iso-8859-1",
				e->data[HL_URL].value_len, &u_text);
		if (ret != NSERROR_OK) {
			free(t_text);
			return NSERROR_SAVE_FAILED;
		}

		fprintf(tw->fp, "<li><a href=\"%s\">%s</a></li>\n",
			u_text, t_text);

		free(t_text);
		free(u_text);

	} else if (type == TREE_NODE_FOLDER) {
		struct hotlist_folder *f = node_data;
		char *f_text;

		ret = utf8_to_html(f->data.value, "iso-8859-1",
				f->data.value_len, &f_text);
		if (ret != NSERROR_OK)
			return NSERROR_SAVE_FAILED;

		if (f == hl_ctx.default_folder) {
			fprintf(tw->fp, "<h4>%s</h4>\n<ul id=\"default\">\n",
					f_text);
		} else {
			fprintf(tw->fp, "<h4>%s</h4>\n<ul>\n", f_text);
		}

		free(f_text);
	}

	return NSERROR_OK;
}
/** Callback for treeview_walk node leaving */
static nserror hotlist_export_leave_cb(void *ctx, void *node_data,
		enum treeview_node_type type, bool *abort)
{
	struct treeview_export_walk_ctx *tw = ctx;

	if (type == TREE_NODE_FOLDER) {
		fputs("</ul>\n", tw->fp);
	}

	return NSERROR_OK;
}
/* Exported interface, documented in hotlist.h */
nserror hotlist_export(const char *path, const char *title)
{
	struct treeview_export_walk_ctx tw;
	nserror err;
	FILE *fp;

	fp = fopen(path, "w");
	if (fp == NULL)
		return NSERROR_SAVE_FAILED;

	if (title == NULL)
		title = "NetSurf hotlist";

	/* The Acorn Browse Hotlist format, which we mimic[*], is invalid HTML
	 * claming to be valid.
	 * [*] Why? */
	fputs("<!DOCTYPE html "
		"PUBLIC \"//W3C/DTD HTML 4.01//EN\" "
		"\"http://www.w3.org/TR/html4/strict.dtd\">\n", fp);
	fputs("<html>\n<head>\n", fp);
	fputs("<meta http-equiv=\"Content-Type\" "
		"content=\"text/html; charset=iso-8859-1\">\n", fp);
	fprintf(fp, "<title>%s</title>\n", title);
	fputs("</head>\n<body>\n<ul>\n", fp);

	tw.fp = fp;
	err = treeview_walk(hl_ctx.tree, NULL,
			hotlist_export_enter_cb,
			hotlist_export_leave_cb,
			&tw, TREE_NODE_ENTRY | TREE_NODE_FOLDER);
	if (err != NSERROR_OK)
		return err;

	fputs("</ul>\n</body>\n</html>\n", fp);

	fclose(fp);

	return NSERROR_OK;
}


struct hotlist_iterate_ctx {
	hotlist_folder_enter_cb enter_cb;
	hotlist_address_cb address_cb;
	hotlist_folder_leave_cb leave_cb;
	void *ctx;
};
/** Callback for hotlist_iterate node entering */
static nserror hotlist_iterate_enter_cb(void *ctx, void *node_data,
		enum treeview_node_type type, bool *abort)
{
	struct hotlist_iterate_ctx *data = ctx;

	if (type == TREE_NODE_ENTRY && data->address_cb != NULL) {
		struct hotlist_entry *e = node_data;
		data->address_cb(data->ctx, e->url,
				e->data[HL_TITLE].value);

	} else if (type == TREE_NODE_FOLDER && data->enter_cb != NULL) {
		struct hotlist_folder *f = node_data;
		data->enter_cb(data->ctx, f->data.value);
	}

	return NSERROR_OK;
}
/** Callback for hotlist_iterate node leaving */
static nserror hotlist_iterate_leave_cb(void *ctx, void *node_data,
		enum treeview_node_type type, bool *abort)
{
	struct hotlist_iterate_ctx *data = ctx;

	if (type == TREE_NODE_FOLDER && data->leave_cb != NULL) {
		data->leave_cb(data->ctx);
	}

	return NSERROR_OK;
}
/* Exported interface, documented in hotlist.h */
nserror hotlist_iterate(void *ctx,
		hotlist_folder_enter_cb enter_cb,
		hotlist_address_cb address_cb,
		hotlist_folder_leave_cb leave_cb)
{
	struct hotlist_iterate_ctx data;
	nserror err;

	data.enter_cb = enter_cb;
	data.address_cb = address_cb;
	data.leave_cb = leave_cb;
	data.ctx = ctx;

	err = treeview_walk(hl_ctx.tree, NULL,
			hotlist_iterate_enter_cb,
			hotlist_iterate_leave_cb,
			&data, TREE_NODE_ENTRY | TREE_NODE_FOLDER);
	if (err != NSERROR_OK)
		return err;

	return NSERROR_OK;
}


/**
 * Initialise the treeview entry feilds
 *
 * \return NSERROR_OK on success, or appropriate error otherwise
 */
static nserror hotlist_initialise_entry_fields(void)
{
	int i;
	const char *label;

	for (i = 0; i < HL_N_FIELDS; i++)
		hl_ctx.fields[i].field = NULL;

	hl_ctx.fields[HL_TITLE].flags = TREE_FLAG_DEFAULT | 
			TREE_FLAG_ALLOW_EDIT;
	label = "TreeviewLabelTitle";
	label = messages_get(label);
	if (lwc_intern_string(label, strlen(label),
			&hl_ctx.fields[HL_TITLE].field) !=
			lwc_error_ok) {
		goto error;
	}

	hl_ctx.fields[HL_URL].flags =
			TREE_FLAG_ALLOW_EDIT |
			TREE_FLAG_COPY_TEXT |
			TREE_FLAG_SEARCHABLE;
	label = "TreeviewLabelURL";
	label = messages_get(label);
	if (lwc_intern_string(label, strlen(label),
			&hl_ctx.fields[HL_URL].field) !=
			lwc_error_ok) {
		goto error;
	}

	hl_ctx.fields[HL_LAST_VISIT].flags = TREE_FLAG_SHOW_NAME;
	label = "TreeviewLabelLastVisit";
	label = messages_get(label);
	if (lwc_intern_string(label, strlen(label),
			&hl_ctx.fields[HL_LAST_VISIT].field) !=
			lwc_error_ok) {
		goto error;
	}

	hl_ctx.fields[HL_VISITS].flags = TREE_FLAG_SHOW_NAME;
	label = "TreeviewLabelVisits";
	label = messages_get(label);
	if (lwc_intern_string(label, strlen(label),
			&hl_ctx.fields[HL_VISITS].field) !=
			lwc_error_ok) {
		goto error;
	}

	hl_ctx.fields[HL_FOLDER].flags = TREE_FLAG_DEFAULT | 
			TREE_FLAG_ALLOW_EDIT;
	label = "TreeviewLabelFolder";
	label = messages_get(label);
	if (lwc_intern_string(label, strlen(label),
			&hl_ctx.fields[HL_FOLDER].field) !=
			lwc_error_ok) {
		return false;
	}

	return NSERROR_OK;

error:
	for (i = 0; i < HL_N_FIELDS; i++)
		lwc_string_unref(hl_ctx.fields[i].field);

	return NSERROR_UNKNOWN;
}


/*
 * Populate the hotlist from file, or generate default hotlist if no file
 *
 * \param path		The path to load the hotlist file from, or NULL
 * \return NSERROR_OK on success, or appropriate error otherwise
 */
static nserror hotlist_populate(const char *path)
{
	nserror err;
	bool loaded;

	/* Load hotlist file */
	err = hotlist_load(path, &loaded);

	if (loaded && err == NSERROR_OK)
		return err;

	/* Couldn't load hotlist, generate a default one */
	err = hotlist_generate();
	if (err != NSERROR_OK) {
		return err;
	}

	return NSERROR_OK;
}


/* Exported interface, documented in hotlist.h */
nserror hotlist_init(
		const char *load_path,
		const char *save_path)
{
	nserror err;

	err = treeview_init();
	if (err != NSERROR_OK) {
		return err;
	}

	NSLOG(netsurf, INFO, "Loading hotlist");

	hl_ctx.tree = NULL;
	hl_ctx.built = false;
	hl_ctx.default_folder = NULL;

	/* Store the save path */
	if (save_path != NULL) {
		hl_ctx.save_path = strdup(save_path);
		if (hl_ctx.save_path == NULL) {
			return NSERROR_NOMEM;
		}
	} else {
		hl_ctx.save_path = NULL;
	}

	/* Init. hotlist treeview entry fields */
	err = hotlist_initialise_entry_fields();
	if (err != NSERROR_OK) {
		free(hl_ctx.save_path);
		hl_ctx.tree = NULL;
		return err;
	}

	/* Create the hotlist treeview */
	err = treeview_create(&hl_ctx.tree, &hl_tree_cb_t,
			HL_N_FIELDS, hl_ctx.fields, NULL,
			TREEVIEW_SEARCHABLE);
	if (err != NSERROR_OK) {
		free(hl_ctx.save_path);
		hl_ctx.tree = NULL;
		return err;
	}

	/* Populate the hotlist */
	err = hotlist_populate(load_path);
	if (err != NSERROR_OK) {
		free(hl_ctx.save_path);
		return err;
	}

	/* Hotlist tree is built
	 * We suppress the treeview height callback on entry insertion before
	 * the treeview is built. */
	hl_ctx.built = true;

	NSLOG(netsurf, INFO, "Loaded hotlist");

	return NSERROR_OK;
}


/* Exported interface, documented in hotlist.h */
nserror hotlist_manager_init(void *core_window_handle)
{
	nserror err;

	/* Create the hotlist treeview */
	err = treeview_cw_attach(hl_ctx.tree, core_window_handle);
	if (err != NSERROR_OK) {
		return err;
	}

	/* Inform client of window height */
	treeview_get_height(hl_ctx.tree);

	return NSERROR_OK;
}


/* Exported interface, documented in hotlist.h */
nserror hotlist_manager_fini(void)
{
	nserror err;

	/* Create the hotlist treeview */
	err = treeview_cw_detach(hl_ctx.tree);
	if (err != NSERROR_OK) {
		return err;
	}

	return NSERROR_OK;
}


/* Exported interface, documented in hotlist.h */
nserror hotlist_fini(void)
{
	int i;
	nserror err;

	NSLOG(netsurf, INFO, "Finalising hotlist");

	/* Remove any existing scheduled save callback */
	guit->misc->schedule(-1, hotlist_schedule_save_cb, NULL);
	hl_ctx.save_scheduled = false;

	/* Save the hotlist */
	err = hotlist_save(hl_ctx.save_path);
	if (err != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Problem saving the hotlist.");
	}

	free(hl_ctx.save_path);

	/* Destroy the hotlist treeview */
	err = treeview_destroy(hl_ctx.tree);
	if (err != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Problem destroying the hotlist treeview.");
	}
	hl_ctx.built = false;

	/* Free hotlist treeview entry fields */
	for (i = 0; i < HL_N_FIELDS; i++)
		lwc_string_unref(hl_ctx.fields[i].field);

	err = treeview_fini();
	if (err != NSERROR_OK) {
		return err;
	}

	NSLOG(netsurf, INFO, "Finalised hotlist");

	return err;
}


/* Exported interface, documented in hotlist.h */
nserror hotlist_add_url(nsurl *url)
{
	treeview_node *entry;
	nserror err;

	/* If we don't have a hotlist at the moment, just return OK */
	if (hl_ctx.tree == NULL)
		return NSERROR_OK;

	/* Make the default folder, if we don't have one */
	if (hl_ctx.default_folder == NULL) {
		const char *temp = messages_get("HotlistDefaultFolderName");
		struct hotlist_folder *f;
		err = hotlist_add_folder_internal(temp, NULL,
				TREE_REL_FIRST_CHILD, &f, true);
		if (err != NSERROR_OK)
			return err;

		if (f == NULL)
			return NSERROR_NOMEM;

		hl_ctx.default_folder = f;
	}

	/* Add new entry to default folder */
	err = hotlist_add_entry_internal(url, NULL, NULL,
			hl_ctx.default_folder->folder,
			TREE_REL_FIRST_CHILD, &entry);
	if (err != NSERROR_OK)
		return err;

	/* Ensure default folder is expanded */
	err = treeview_node_expand(hl_ctx.tree, hl_ctx.default_folder->folder);
	if (err != NSERROR_OK)
		return err;

	return hotlist_schedule_save();
}


struct treeview_has_url_walk_ctx {
	nsurl *url;
	bool found;
};
/** Callback for treeview_walk */
static nserror hotlist_has_url_walk_cb(void *ctx, void *node_data,
		enum treeview_node_type type, bool *abort)
{
	struct treeview_has_url_walk_ctx *tw = ctx;

	if (type == TREE_NODE_ENTRY) {
		struct hotlist_entry *e = node_data;

		if (nsurl_compare(e->url, tw->url, NSURL_COMPLETE) == true) {
			/* Found what we're looking for */
			tw->found = true;
			*abort = true;
		}
	}

	return NSERROR_OK;
}
/* Exported interface, documented in hotlist.h */
bool hotlist_has_url(nsurl *url)
{
	nserror err;
	struct treeview_has_url_walk_ctx tw = {
		.url = url,
		.found = false
	};

	if (hl_ctx.built == false)
		return false;

	err = treeview_walk(hl_ctx.tree, NULL, hotlist_has_url_walk_cb, NULL,
			&tw, TREE_NODE_ENTRY);
	if (err != NSERROR_OK)
		return false;

	return tw.found;
}


struct treeview_remove_url_walk_ctx {
	nsurl *url;
};
/** Callback for treeview_walk */
static nserror hotlist_remove_url_walk_cb(void *ctx, void *node_data,
		enum treeview_node_type type, bool *abort)
{
	struct treeview_remove_url_walk_ctx *tw = ctx;

	if (type == TREE_NODE_ENTRY) {
		struct hotlist_entry *e = node_data;

		if (nsurl_compare(e->url, tw->url, NSURL_COMPLETE) == true) {
			/* Found what we're looking for: delete it */
			treeview_delete_node(hl_ctx.tree, e->entry,
					TREE_OPTION_NONE);
		}
	}

	return NSERROR_OK;
}
/* Exported interface, documented in hotlist.h */
void hotlist_remove_url(nsurl *url)
{
	nserror err;
	struct treeview_remove_url_walk_ctx tw = {
		.url = url
	};

	if (hl_ctx.built == false)
		return;

	err = treeview_walk(hl_ctx.tree, NULL, NULL, hotlist_remove_url_walk_cb,
			&tw, TREE_NODE_ENTRY);
	if (err != NSERROR_OK)
		return;

	return;
}


struct treeview_update_url_walk_ctx {
	nsurl *url;
	const struct url_data *data;
};
/** Callback for treeview_walk */
static nserror hotlist_update_url_walk_cb(void *ctx, void *node_data,
		enum treeview_node_type type, bool *abort)
{
	struct treeview_update_url_walk_ctx *tw = ctx;
	struct hotlist_entry *e = node_data;
	nserror err;

	if (type != TREE_NODE_ENTRY) {
		return NSERROR_OK;
	}

	if (nsurl_compare(e->url, tw->url, NSURL_COMPLETE) == true) {
		/* Found match: Update the entry data */
		free((void *)e->data[HL_LAST_VISIT].value); /* Eww */
		free((void *)e->data[HL_VISITS].value); /* Eww */

		if (tw->data == NULL) {
			/* Get the URL data */
			tw->data = urldb_get_url_data(tw->url);
			if (tw->data == NULL) {
				/* No entry in database, so add one */
				urldb_add_url(tw->url);
				/* now attempt to get url data */
				tw->data = urldb_get_url_data(tw->url);
			}
			if (tw->data == NULL) {
				return NSERROR_NOMEM;
			}
		}

		err = hotlist_create_treeview_field_visits_data(e, tw->data);
		if (err != NSERROR_OK)
			return err;

		err = treeview_update_node_entry(hl_ctx.tree,
				e->entry, e->data, e);
		if (err != NSERROR_OK)
			return err;
	}

	return NSERROR_OK;
}
/* Exported interface, documented in hotlist.h */
void hotlist_update_url(nsurl *url)
{
	nserror err;
	struct treeview_update_url_walk_ctx tw = {
		.url = url,
		.data = NULL
	};

	if (hl_ctx.built == false)
		return;

	err = treeview_walk(hl_ctx.tree, NULL, hotlist_update_url_walk_cb, NULL,
			&tw, TREE_NODE_ENTRY);
	if (err != NSERROR_OK)
		return;

	return;
}


/* Exported interface, documented in hotlist.h */
nserror hotlist_add_entry(nsurl *url, const char *title, bool at_y, int y)
{
	nserror err;
	treeview_node *entry;
	treeview_node *relation;
	enum treeview_relationship rel;

	if (url == NULL) {
		err = nsurl_create("https://netsurf-browser.org/", &url);
		if (err != NSERROR_OK) {
			return err;
		}
		assert(url != NULL);
	} else {
		nsurl_ref(url);
	}

	err = treeview_get_relation(hl_ctx.tree, &relation, &rel, at_y, y);
	if (err != NSERROR_OK) {
		nsurl_unref(url);
		return err;
	}

	err = hotlist_add_entry_internal(url, title, NULL,
			relation, rel, &entry);
	if (err != NSERROR_OK) {
		nsurl_unref(url);
		return err;
	}

	nsurl_unref(url);

	return NSERROR_OK;
}


/* Exported interface, documented in hotlist.h */
nserror hotlist_add_folder(const char *title, bool at_y, int y)
{
	nserror err;
	struct hotlist_folder *f;
	treeview_node *relation;
	enum treeview_relationship rel;

	err = treeview_get_relation(hl_ctx.tree, &relation, &rel, at_y, y);
	if (err != NSERROR_OK) {
		return err;
	}

	err = hotlist_add_folder_internal(title, relation, rel, &f, false);
	if (err != NSERROR_OK) {
		return err;
	}

	return NSERROR_OK;
}


/* Exported interface, documented in hotlist.h */
void hotlist_redraw(int x, int y, struct rect *clip,
		const struct redraw_context *ctx)
{
	treeview_redraw(hl_ctx.tree, x, y, clip, ctx);
}


/* Exported interface, documented in hotlist.h */
void hotlist_mouse_action(browser_mouse_state mouse, int x, int y)
{
	treeview_mouse_action(hl_ctx.tree, mouse, x, y);
}


/* Exported interface, documented in hotlist.h */
bool hotlist_keypress(uint32_t key)
{
	return treeview_keypress(hl_ctx.tree, key);
}


/* Exported interface, documented in hotlist.h */
bool hotlist_has_selection(void)
{
	return treeview_has_selection(hl_ctx.tree);
}


/* Exported interface, documented in hotlist.h */
bool hotlist_get_selection(nsurl **url, const char **title)
{
	struct hotlist_entry *e;
	enum treeview_node_type type;
	void *v;

	type = treeview_get_selection(hl_ctx.tree, &v);
	if (type != TREE_NODE_ENTRY || v == NULL) {
		*url = NULL;
		*title = NULL;
		return false;
	}

	e = (struct hotlist_entry *)v;

	*url = e->url;
	*title = e->data[HL_TITLE].value;
	return true;
}


/* Exported interface, documented in hotlist.h */
void hotlist_edit_selection(void)
{
	treeview_edit_selection(hl_ctx.tree);
}


/* Exported interface, documented in hotlist.h */
nserror hotlist_expand(bool only_folders)
{
	return treeview_expand(hl_ctx.tree, only_folders);
}


/* Exported interface, documented in hotlist.h */
nserror hotlist_contract(bool all)
{
	return treeview_contract(hl_ctx.tree, all);
}

