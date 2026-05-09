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

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "utils/config.h"
#include "utils/errors.h"
#include "utils/file.h"
#include "utils/inet.h"
#include "netsurf/bitmap.h"
#include "content/hlcache.h"
#include "content/backing_store.h"

#include "desktop/save_pdf.h"
#include "desktop/download.h"
#include "desktop/searchweb.h"
#include "netsurf/download.h"
#include "netsurf/fetch.h"
#include "netsurf/misc.h"
#include "netsurf/window.h"
#include "netsurf/core_window.h"
#include "netsurf/search.h"
#include "netsurf/clipboard.h"
#include "netsurf/utf8.h"
#include "netsurf/layout.h"
#include "netsurf/netsurf.h"

/**
 * The global interface table.
 */
struct netsurf_table *guit = NULL;


static void gui_default_window_set_title(struct gui_window *g, const char *title)
{
}

static nserror gui_default_window_set_url(struct gui_window *g, struct nsurl *url)
{
	return NSERROR_OK;
}

static bool gui_default_window_drag_start(struct gui_window *g,
					  gui_drag_type type,
					  const struct rect *rect)
{
	return true;
}

static nserror gui_default_window_save_link(struct gui_window *g,
					 nsurl *url,
					 const char *title)
{
	return NSERROR_OK;
}

static void gui_default_window_set_icon(struct gui_window *g,
					hlcache_handle *icon)
{
}

static void gui_default_window_set_pointer(struct gui_window *g,
					   gui_pointer_shape shape)
{
}

static void gui_default_window_set_status(struct gui_window *g,
					  const char *text)
{
}

static void gui_default_window_place_caret(struct gui_window *g,
					   int x, int y, int height,
					   const struct rect *clip)
{
}

static void gui_default_window_create_form_select_menu(struct gui_window *g,
						struct form_control *control)
{
}

static void gui_default_window_file_gadget_open(struct gui_window *g,
						hlcache_handle *hl,
						struct form_control *gadget)
{
}

static void gui_default_window_drag_save_object(struct gui_window *g,
						hlcache_handle *c,
						gui_save_type type)
{
}

static void gui_default_window_drag_save_selection(struct gui_window *g,
						   const char *selection)
{
}


static void
gui_default_console_log(struct gui_window *gw,
			browser_window_console_source src,
			const char *msg,
			size_t msglen,
			browser_window_console_flags flags)
{
}


/** verify window table is valid */
static nserror verify_window_register(struct gui_window_table *gwt)
{
	/* check table is present */
	if (gwt == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* check the mandantory fields are set */
	if (gwt->create == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (gwt->destroy == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (gwt->invalidate == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (gwt->get_scroll == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (gwt->set_scroll == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (gwt->get_dimensions == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (gwt->event == NULL) {
		return NSERROR_BAD_PARAMETER;
	}


	/* fill in the optional entries with defaults */
	if (gwt->set_title == NULL) {
		gwt->set_title = gui_default_window_set_title;
	}
	if (gwt->set_url == NULL) {
		gwt->set_url = gui_default_window_set_url;
	}
	if (gwt->set_icon == NULL) {
		gwt->set_icon = gui_default_window_set_icon;
	}
	if (gwt->set_status == NULL) {
		gwt->set_status = gui_default_window_set_status;
	}
	if (gwt->set_pointer == NULL) {
		gwt->set_pointer = gui_default_window_set_pointer;
	}
	if (gwt->place_caret == NULL) {
		gwt->place_caret = gui_default_window_place_caret;
	}
	if (gwt->drag_start == NULL) {
		gwt->drag_start = gui_default_window_drag_start;
	}
	if (gwt->save_link == NULL) {
		gwt->save_link = gui_default_window_save_link;
	}
	if (gwt->create_form_select_menu == NULL) {
		gwt->create_form_select_menu =
				gui_default_window_create_form_select_menu;
	}
	if (gwt->file_gadget_open == NULL) {
		gwt->file_gadget_open = gui_default_window_file_gadget_open;
	}
	if (gwt->drag_save_object == NULL) {
		gwt->drag_save_object = gui_default_window_drag_save_object;
	}
	if (gwt->drag_save_selection == NULL) {
		gwt->drag_save_selection = gui_default_window_drag_save_selection;
	}
	if (gwt->console_log == NULL) {
		gwt->console_log = gui_default_console_log;
	}

	return NSERROR_OK;
}


static nserror gui_default_corewindow_invalidate(struct core_window *cw, const struct rect *rect)
{
	return NSERROR_OK;
}

static nserror gui_default_corewindow_set_extent(struct core_window *cw, int width, int height)
{
	return NSERROR_OK;
}

static nserror gui_default_corewindow_set_scroll(struct core_window *cw, int x, int y)
{
	return NSERROR_OK;
}

static nserror gui_default_corewindow_get_scroll(const struct core_window *cw, int *x, int *y)
{
	return NSERROR_OK;
}

static nserror gui_default_corewindow_get_dimensions(const struct core_window *cw, int *width, int *height)
{
	return NSERROR_OK;
}

static nserror gui_default_corewindow_dragstatus(struct core_window *cw, core_window_drag_status ds)
{
	return NSERROR_OK;
}

static struct core_window_table default_corewindow_table = {
	gui_default_corewindow_invalidate,
	gui_default_corewindow_set_extent,
	gui_default_corewindow_set_scroll,
	gui_default_corewindow_get_scroll,
	gui_default_corewindow_get_dimensions,
	gui_default_corewindow_dragstatus
};

/** verify corewindow window table is valid */
static nserror verify_corewindow_register(struct core_window_table *cwt)
{
	/* check table is present */
	if (cwt == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* all enties are mandantory */
	if (cwt->invalidate == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (cwt->set_extent == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (cwt->set_scroll == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (cwt->get_scroll == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (cwt->get_dimensions == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (cwt->drag_status == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	return NSERROR_OK;
}

static struct gui_download_window *
gui_default_download_create(download_context *ctx, struct gui_window *parent)
{
	return NULL;
}

static nserror gui_default_download_data(struct gui_download_window *dw,
				  const char *data, unsigned int size)
{
	return NSERROR_OK;
}

static void gui_default_download_error(struct gui_download_window *dw,
				const char *error_msg)
{
}

static void gui_default_download_done(struct gui_download_window *dw)
{
}

static struct gui_download_table default_download_table = {
	gui_default_download_create,
	gui_default_download_data,
	gui_default_download_error,
	gui_default_download_done
};

/** verify download window table is valid */
static nserror verify_download_register(struct gui_download_table *gdt)
{
	/* check table is present */
	if (gdt == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* all enties are mandantory */
	if (gdt->create == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (gdt->data == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (gdt->error == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (gdt->done == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	return NSERROR_OK;
}

static void gui_default_get_clipboard(char **buffer, size_t *length)
{
	*buffer = NULL;
	*length = 0;
}

static void gui_default_set_clipboard(const char *buffer, size_t length,
		nsclipboard_styles styles[], int n_styles)
{
}

static struct gui_clipboard_table default_clipboard_table = {
	gui_default_get_clipboard,
	gui_default_set_clipboard
};

/** verify clipboard table is valid */
static nserror verify_clipboard_register(struct gui_clipboard_table *gct)
{
	/* check table is present */
	if (gct == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* optional operations */
	if (gct->get == NULL) {
		gct->get = gui_default_get_clipboard;
	}
	if (gct->set == NULL) {
		gct->set = gui_default_set_clipboard;
	}
	return NSERROR_OK;
}

/**
 * The default utf8 conversion implementation.
 *
 * The default implementation assumes the local encoding is utf8
 * allowing the conversion to be a simple copy.
 *
 * @param [in] string The source string.
 * @param [in] len The \a string length or 0 to compute it.
 * @param [out] result A pointer to the converted string.
 * @result NSERROR_OK or NSERROR_NOMEM if memory could not be allocated.
 */
static nserror gui_default_utf8(const char *string, size_t len, char **result)
{
	assert(string && result);

	if (len == 0)
		len = strlen(string);

	*result = strndup(string, len);
	if (!(*result))
		return NSERROR_NOMEM;

	return NSERROR_OK;
}

static struct gui_utf8_table default_utf8_table = {
	gui_default_utf8,
	gui_default_utf8
};

/** verify clipboard table is valid */
static nserror verify_utf8_register(struct gui_utf8_table *gut)
{
	/* check table is present */
	if (gut == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* mandantory operations */
	if (gut->utf8_to_local == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (gut->local_to_utf8 == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	return NSERROR_OK;
}

static void gui_default_status(bool found, void *p)
{
}

static void gui_default_hourglass(bool active, void *p)
{
}

static void gui_default_add_recent(const char *string, void *p)
{
}

static void gui_default_forward_state(bool active, void *p)
{
}

static void gui_default_back_state(bool active, void *p)
{
}

static struct gui_search_table default_search_table = {
	gui_default_status,
	gui_default_hourglass,
	gui_default_add_recent,
	gui_default_forward_state,
	gui_default_back_state
};

/** verify search table is valid */
static nserror verify_search_register(struct gui_search_table *gst)
{
	/* check table is present */
	if (gst == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* fill in the optional entries with defaults */
	if (gst->status == NULL) {
		gst->status = default_search_table.status;
	}
	if (gst->hourglass == NULL) {
		gst->hourglass = default_search_table.hourglass;
	}
	if (gst->add_recent == NULL) {
		gst->add_recent = default_search_table.add_recent;
	}
	if (gst->forward_state == NULL) {
		gst->forward_state = default_search_table.forward_state;
	}
	if (gst->back_state == NULL) {
		gst->back_state = default_search_table.back_state;
	}

	return NSERROR_OK;
}

static nserror
gui_default_provider_update(const char *provider_name,
			    struct bitmap *provider_bitmap)
{
	return NSERROR_OK;
}

static struct gui_search_web_table default_search_web_table = {
	gui_default_provider_update
};

/** verify search table is valid */
static nserror verify_search_web_register(struct gui_search_web_table *gswt)
{
	/* check table is present */
	if (gswt == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* mandantory operations */
	if (gswt->provider_update == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	return NSERROR_OK;
}

/** verify low level cache persistant backing store table is valid */
static nserror verify_llcache_register(struct gui_llcache_table *glt)
{
	/* check table is present */
	if (glt == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* mandantory operations */
	if (glt->store == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (glt->fetch == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (glt->invalidate == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (glt->release == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (glt->initialise == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (glt->finalise == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	return NSERROR_OK;
}

static nsurl *gui_default_get_resource_url(const char *path)
{
	return NULL;
}

static nserror gui_default_get_resource_data(const char *path, const uint8_t **data, size_t *data_len)
{
	return NSERROR_NOT_FOUND;
}

static nserror gui_default_release_resource_data(const uint8_t *data)
{
	return NSERROR_OK;
}

static char *gui_default_mimetype(const char *path)
{
	return strdup(guit->fetch->filetype(path));
}

static int gui_default_socket_open(int domain, int type, int protocol)
{
	return (int) socket(domain, type, protocol);
}

static int gui_default_socket_close(int fd)
{
	return (int) ns_close_socket(fd);
}

/** verify fetch table is valid */
static nserror verify_fetch_register(struct gui_fetch_table *gft)
{
	/* check table is present */
	if (gft == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* check the mandantory fields are set */
	if (gft->filetype == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* fill in the optional entries with defaults */
	if (gft->get_resource_url == NULL) {
		gft->get_resource_url = gui_default_get_resource_url;
	}
	if (gft->get_resource_data == NULL) {
		gft->get_resource_data = gui_default_get_resource_data;
	}
	if (gft->release_resource_data == NULL) {
		gft->release_resource_data = gui_default_release_resource_data;
	}
	if (gft->mimetype == NULL) {
		gft->mimetype = gui_default_mimetype;
	}
	if (gft->socket_open == NULL) {
		gft->socket_open = gui_default_socket_open;
	}
	if (gft->socket_close == NULL) {
		gft->socket_close = gui_default_socket_close;
	}

	return NSERROR_OK;
}

/** verify file table is valid */
static nserror verify_file_register(struct gui_file_table *gft)
{
	/* check table is present */
	if (gft == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* check the mandantory fields are set */
	if (gft->mkpath == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (gft->basename == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (gft->nsurl_to_path == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (gft->path_to_nsurl == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (gft->mkdir_all == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	return NSERROR_OK;
}

/**
 * verify bitmap table is valid
 *
 * \param gbt The bitmap table to verify.
 * \return NSERROR_OK if the table is valid else NSERROR_BAD_PARAMETER.
 */
static nserror verify_bitmap_register(struct gui_bitmap_table *gbt)
{
	/* check table is present */
	if (gbt == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* check the mandantory fields are set */
	if (gbt->create == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	if (gbt->destroy == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	if (gbt->set_opaque == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	if (gbt->get_opaque == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	if (gbt->get_buffer == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	if (gbt->get_rowstride == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	if (gbt->get_width == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	if (gbt->get_height == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	if (gbt->modified == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	if (gbt->render == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	return NSERROR_OK;
}

/**
 * verify layout table is valid
 *
 * \param glt The layout table to verify.
 * \return NSERROR_OK if the table is valid else NSERROR_BAD_PARAMETER.
 */
static nserror verify_layout_register(struct gui_layout_table *glt)
{
	/* check table is present */
	if (glt == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* check the mandantory fields are set */
	if (glt->width == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	if (glt->position == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	if (glt->split == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	return NSERROR_OK;
}

static void gui_default_quit(void)
{
}


static nserror gui_default_launch_url(struct nsurl *url)
{
	return NSERROR_NO_FETCH_HANDLER;
}


static nserror gui_default_401login_open(
	nsurl *url, const char *realm,
	const char *username, const char *password,
	nserror (*cb)(nsurl *url, const char * realm,
		      const char *username,
		      const char *password,
		      void *pw),
	void *cbpw)
{
	return NSERROR_NOT_IMPLEMENTED;
}

static void
gui_default_pdf_password(char **owner_pass, char **user_pass, char *path)
{
	*owner_pass = NULL;
	save_pdf(path);
}

static nserror
gui_default_present_cookies(const char *search_term)
{
	return NSERROR_NOT_IMPLEMENTED;
}

/** verify misc table is valid */
static nserror verify_misc_register(struct gui_misc_table *gmt)
{
	/* check table is present */
	if (gmt == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* check the mandantory fields are set */
	if (gmt->schedule == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* fill in the optional entries with defaults */
	if (gmt->quit == NULL) {
		gmt->quit = gui_default_quit;
	}
	if (gmt->launch_url == NULL) {
		gmt->launch_url = gui_default_launch_url;
	}
	if (gmt->login == NULL) {
		gmt->login = gui_default_401login_open;
	}
	if (gmt->pdf_password == NULL) {
		gmt->pdf_password = gui_default_pdf_password;
	}
	if (gmt->present_cookies == NULL) {
		gmt->present_cookies = gui_default_present_cookies;
	}
	return NSERROR_OK;
}


/* exported interface documented in netsurf/netsurf.h */
nserror netsurf_register(struct netsurf_table *gt)
{
	nserror err;

	/* ensure not already initialised */
	if (guit != NULL) {
		return NSERROR_INIT_FAILED;
	}

	/* check table is present */
	if (gt == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	/* mandantory tables */

	/* miscellaneous table */
	err = verify_misc_register(gt->misc);
	if (err != NSERROR_OK) {
		return err;
	}

	/* window table */
	err = verify_window_register(gt->window);
	if (err != NSERROR_OK) {
		return err;
	}

	/* fetch table */
	err = verify_fetch_register(gt->fetch);
	if (err != NSERROR_OK) {
		return err;
	}

	/* bitmap table */
	err = verify_bitmap_register(gt->bitmap);
	if (err != NSERROR_OK) {
		return err;
	}

	/* layout table */
	err = verify_layout_register(gt->layout);
	if (err != NSERROR_OK) {
		return err;
	}

	/* optional tables */

	/* core window table */
	if (gt->corewindow == NULL) {
		/* set default core window table */
		gt->corewindow = &default_corewindow_table;
	}
	err = verify_corewindow_register(gt->corewindow);
	if (err != NSERROR_OK) {
		return err;
	}


	/* file table */
	if (gt->file == NULL) {
		gt->file = default_file_table;
	}
	err = verify_file_register(gt->file);
	if (err != NSERROR_OK) {
		return err;
	}

	/* download table */
	if (gt->download == NULL) {
		/* set default download table */
		gt->download = &default_download_table;
	}
	err = verify_download_register(gt->download);
	if (err != NSERROR_OK) {
		return err;
	}

	/* clipboard table */
	if (gt->clipboard == NULL) {
		/* set default clipboard table */
		gt->clipboard = &default_clipboard_table;
	}
	err = verify_clipboard_register(gt->clipboard);
	if (err != NSERROR_OK) {
		return err;
	}

	/* utf8 table */
	if (gt->utf8 == NULL) {
		/* set default utf8 table */
		gt->utf8 = &default_utf8_table;
	}
	err = verify_utf8_register(gt->utf8);
	if (err != NSERROR_OK) {
		return err;
	}

	/* search table */
	if (gt->search == NULL) {
		/* set default search table */
		gt->search = &default_search_table;
	}
	err = verify_search_register(gt->search);
	if (err != NSERROR_OK) {
		return err;
	}

	/* web search table */
	if (gt->search_web == NULL) {
		/* set default search table */
		gt->search_web = &default_search_web_table;
	}
	err = verify_search_web_register(gt->search_web);
	if (err != NSERROR_OK) {
		return err;
	}

	/* llcache table */
	if (gt->llcache == NULL) {
		/* set default backing store table */
		gt->llcache = null_llcache_table;
	}
	err = verify_llcache_register(gt->llcache);
	if (err != NSERROR_OK) {
		return err;
	}

	guit = gt;

	return NSERROR_OK;
}
