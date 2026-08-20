/*
 * Copyright 2004, 2005 Richard Wilson <info@tinct.net>
 * Copyright 2011-2014 Stephen Fryatt <stevef@netsurf-browser.org>
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
 * RISC OS URL bar implementation.
 *
 * The treeview resources are retrieved from resource url necessitating
 * the use of the hlcache content interface.
 */

#include <stddef.h>
#include "oslib/wimp.h"

#include "utils/log.h"
#include "utils/messages.h"
#include "netsurf/browser_window.h"
#include "netsurf/plotters.h"
#include "netsurf/content.h"
#include "content/hlcache.h"

#include "riscos/gui.h"
#include "riscos/hotlist.h"
#include "riscos/url_suggest.h"
#include "riscos/wimp.h"
#include "riscos/wimp_event.h"
#include "riscos/wimputils.h"
#include "riscos/window.h"
#include "riscos/ucstables.h"
#include "riscos/filetype.h"
#include "riscos/gui/url_bar.h"

#define URLBAR_HEIGHT 52
#define URLBAR_FAVICON_SIZE 16
#define URLBAR_HOTLIST_SIZE 17
#define URLBAR_PGINFO_WIDTH ((26) * 2)
#define URLBAR_FAVICON_WIDTH ((5 + URLBAR_FAVICON_SIZE + 5) * 2)
#define URLBAR_HOTLIST_WIDTH ((5 + URLBAR_HOTLIST_SIZE + 5) * 2)
#define URLBAR_MIN_WIDTH 52
#define URLBAR_GRIGHT_GUTTER 8

#define URLBAR_PGINFO_NAME_LENGTH 12
#define URLBAR_FAVICON_NAME_LENGTH 12

struct url_bar {
	/** The applied theme (or NULL to use the default) */
	struct theme_descriptor	*theme;

	/** The widget dimensions. */
	int			x_min, y_min;

	/** The window and icon details. */
	wimp_w			window;
	os_box			extent;
	osspriteop_area		*sprites;

	wimp_i			container_icon;

	bool			hidden;
	bool			display;
	bool			shaded;

	struct {
		char		sprite[URLBAR_PGINFO_NAME_LENGTH];
		os_box		extent;
	} pginfo;

	struct {
		char		sprite[URLBAR_FAVICON_NAME_LENGTH];
		int		type;
		struct hlcache_handle *content;
		os_box		extent;
		os_coord	offset;
		int		width;
		int		height;
	} favicon;

	struct {
		wimp_i		icon;
		char		*buffer;
		size_t		size;
		char		*buffer_utf8;
	} text;

	struct {
		wimp_i		icon;
		int		x;
		int		y;
	} suggest;

	struct {
		bool			set;
		os_box			extent;
		os_coord		offset;
	} hotlist;
};

static char text_validation[] = "Pptr_write;KN";
static char suggest_icon[] = "gright";
static char suggest_validation[] = "R5;Sgright,pgright";
static char null_text_string[] = "";

/** Treeview content resource data */
struct url_bar_resource {
	const char *url;
	struct hlcache_handle *c;
	int height;
	bool ready;
};

enum url_bar_resource_id {
	URLBAR_RES_HOTLIST_ADD = 0,
	URLBAR_RES_HOTLIST_REMOVE,
	URLBAR_RES_LAST
};

/** Treeview content resources */
static struct url_bar_resource url_bar_res[URLBAR_RES_LAST] = {
	{ "resource:icons/hotlist-add.png", NULL, 0, false },
	{ "resource:icons/hotlist-rmv.png", NULL, 0, false }
};


/**
 * Position the icons in the URL bar to take account of the currently
 * configured extent.
 *
 * \param *url_bar The URL bar to update.
 * \param full true to resize everything;
 *             false to move only the right-hand end of the bar.
 * \return true if successful; else false.
 */
static bool ro_gui_url_bar_icon_resize(struct url_bar *url_bar, bool full)
{
	int x0, y0, x1, y1;
	int centre;
	os_error *error;
	os_coord eig = {1, 1};
	wimp_caret caret;

	if ((url_bar == NULL) ||
	    (url_bar->window == NULL)) {
		return false;
	}

	/* calculate 1px in OS units */
	ro_convert_pixels_to_os_units(&eig, (os_mode) -1);

	/* The vertical centre line of the widget's extent. */
	centre = url_bar->extent.y0 +
		(url_bar->extent.y1 - url_bar->extent.y0) / 2;

	/* Position the container icon. */
	if (url_bar->container_icon != -1) {
		x0 = url_bar->extent.x0;
		x1 = url_bar->extent.x1 -
			url_bar->suggest.x - URLBAR_GRIGHT_GUTTER;

		y0 = centre - (URLBAR_HEIGHT / 2);
		y1 = y0 + URLBAR_HEIGHT;

		error = xwimp_resize_icon(url_bar->window,
					  url_bar->container_icon,
					  x0, y0, x1, y1);
		if (error != NULL) {
			NSLOG(netsurf, INFO,
			      "xwimp_resize_icon: 0x%x: %s",
			      error->errnum, error->errmess);
			ro_warn_user("WimpError", error->errmess);
			url_bar->container_icon = -1;
			return false;
		}
	}

	/* Position the URL Suggest icon. */
	if (url_bar->suggest.icon != -1) {
		x0 = url_bar->extent.x1 - url_bar->suggest.x;
		x1 = url_bar->extent.x1;

		y0 = centre - (url_bar->suggest.y / 2);
		y1 = y0 + url_bar->suggest.y;

		error = xwimp_resize_icon(url_bar->window,
					  url_bar->suggest.icon,
					  x0, y0, x1, y1);
		if (error != NULL) {
			NSLOG(netsurf, INFO,
			      "xwimp_resize_icon: 0x%x: %s",
			      error->errnum, error->errmess);
			ro_warn_user("WimpError", error->errmess);
			url_bar->suggest.icon = -1;
			return false;
		}
	}

	/* Position the Text icon. */
	if (url_bar->text.icon != -1) {
		x0 = url_bar->extent.x0 + URLBAR_PGINFO_WIDTH + URLBAR_FAVICON_WIDTH;
		x1 = url_bar->extent.x1 - eig.x - URLBAR_HOTLIST_WIDTH -
			url_bar->suggest.x - URLBAR_GRIGHT_GUTTER;

		y0 = centre - (URLBAR_HEIGHT / 2) + eig.y;
		y1 = y0 + URLBAR_HEIGHT - 2 * eig.y;

		error = xwimp_resize_icon(url_bar->window,
					  url_bar->text.icon,
					  x0, y0, x1, y1);
		if (error != NULL) {
			NSLOG(netsurf, INFO,
			      "xwimp_resize_icon: 0x%x: %s",
			      error->errnum, error->errmess);
			ro_warn_user("WimpError", error->errmess);
			url_bar->text.icon = -1;
			return false;
		}

		if (xwimp_get_caret_position(&caret) == NULL) {
			if ((caret.w == url_bar->window) &&
			    (caret.i == url_bar->text.icon)) {
				xwimp_set_caret_position(url_bar->window,
							 url_bar->text.icon,
							 caret.pos.x,
							 caret.pos.y,
							 -1,
							 caret.index);
			}
		}
	}

	/* Position the page info icon. */
	url_bar->pginfo.extent.x0 = url_bar->extent.x0 + eig.x;
	url_bar->pginfo.extent.x1 = url_bar->extent.x0 + URLBAR_PGINFO_WIDTH;
	url_bar->pginfo.extent.y0 = centre - (URLBAR_HEIGHT / 2) + eig.y;
	url_bar->pginfo.extent.y1 = url_bar->pginfo.extent.y0 + URLBAR_HEIGHT
		- 2 * eig.y;

	/* Position the Favicon icon. */
	url_bar->favicon.extent.x0 = url_bar->extent.x0 + URLBAR_PGINFO_WIDTH ;
	url_bar->favicon.extent.x1 = url_bar->extent.x0 + URLBAR_PGINFO_WIDTH + URLBAR_FAVICON_WIDTH;
	url_bar->favicon.extent.y0 = centre - (URLBAR_HEIGHT / 2) + eig.y;
	url_bar->favicon.extent.y1 = url_bar->favicon.extent.y0 + URLBAR_HEIGHT
		- 2 * eig.y;

	/* Position the Hotlist icon. */
	url_bar->hotlist.extent.x0 = url_bar->extent.x1 - eig.x -
		URLBAR_HOTLIST_WIDTH - url_bar->suggest.x -
		URLBAR_GRIGHT_GUTTER;
	url_bar->hotlist.extent.x1 = url_bar->hotlist.extent.x0 +
		URLBAR_HOTLIST_WIDTH;
	url_bar->hotlist.extent.y0 = centre - (URLBAR_HEIGHT / 2) + eig.y;
	url_bar->hotlist.extent.y1 = url_bar->hotlist.extent.y0 + URLBAR_HEIGHT
		- 2 * eig.y;

	url_bar->hotlist.offset.x = ((url_bar->hotlist.extent.x1 -
				      url_bar->hotlist.extent.x0) -
				     (URLBAR_HOTLIST_SIZE * 2)) / 2;
	url_bar->hotlist.offset.y = ((url_bar->hotlist.extent.y1 -
				      url_bar->hotlist.extent.y0) -
				     (URLBAR_HOTLIST_SIZE * 2)) / 2 - 1;

	return true;
}


/**
 * Create or delete a URL bar's icons if required to bring it into sync with
 * the current hidden setting.
 *
 * \param *url_bar The URL bar to update.
 * \return true if successful; else false.
 */
static bool ro_gui_url_bar_icon_update(struct url_bar *url_bar)
{
	wimp_icon_create icon;
	os_error *error;
	bool resize;

	if ((url_bar == NULL) ||
	    (url_bar->window == NULL)) {
		return false;
	}

	icon.w = url_bar->window;
	icon.icon.extent.x0 = 0;
	icon.icon.extent.y0 = 0;
	icon.icon.extent.x1 = 0;
	icon.icon.extent.y1 = 0;

	resize = false;

	/* Create or delete the container icon. */

	if (!url_bar->hidden && url_bar->container_icon == -1) {
		icon.icon.flags = wimp_ICON_BORDER |
			(wimp_COLOUR_BLACK << wimp_ICON_FG_COLOUR_SHIFT) |
			(wimp_BUTTON_DOUBLE_CLICK_DRAG << wimp_ICON_BUTTON_TYPE_SHIFT);
		error = xwimp_create_icon(&icon, &url_bar->container_icon);
		if (error != NULL) {
			NSLOG(netsurf, INFO,
			      "xwimp_create_icon: 0x%x: %s",
			      error->errnum, error->errmess);
			ro_warn_user("WimpError", error->errmess);
			url_bar->container_icon = -1;
			return false;
		}

		resize = true;
	} else if ((url_bar->hidden) &&
		   (url_bar->container_icon != -1)) {
		error = xwimp_delete_icon(url_bar->window,
					  url_bar->container_icon);
		if (error != NULL) {
			NSLOG(netsurf, INFO,
			      "xwimp_delete_icon: 0x%x: %s",
			      error->errnum, error->errmess);
			ro_warn_user("WimpError", error->errmess);
			return false;
		}

		url_bar->container_icon = -1;
	}

	/* Create or delete the text icon. */
	if (!url_bar->hidden &&
	    url_bar->text.icon == -1) {
		icon.icon.data.indirected_text.text = url_bar->text.buffer;
		icon.icon.data.indirected_text.validation = text_validation;
		icon.icon.data.indirected_text.size = url_bar->text.size;
		icon.icon.flags = wimp_ICON_TEXT |
			wimp_ICON_INDIRECTED |
			wimp_ICON_VCENTRED |
			wimp_ICON_FILLED |
			(wimp_COLOUR_BLACK << wimp_ICON_FG_COLOUR_SHIFT);

		if (url_bar->display) {
			icon.icon.flags |= (wimp_BUTTON_NEVER <<
					    wimp_ICON_BUTTON_TYPE_SHIFT);
		} else if (!ns_wimp_has_text_selection()) {
			icon.icon.flags |= (wimp_BUTTON_WRITE_CLICK_DRAG <<
					    wimp_ICON_BUTTON_TYPE_SHIFT);
		} else {
			icon.icon.flags |= (wimp_BUTTON_WRITABLE <<
					    wimp_ICON_BUTTON_TYPE_SHIFT);
		}
		error = xwimp_create_icon(&icon, &url_bar->text.icon);
		if (error) {
			NSLOG(netsurf, INFO,
			      "xwimp_create_icon: 0x%x: %s",
			      error->errnum, error->errmess);
			ro_warn_user("WimpError", error->errmess);
			url_bar->text.icon = -1;
			return false;
		}

		resize = true;

	} else if (url_bar->hidden &&
		   url_bar->text.icon != -1) {
		error = xwimp_delete_icon(url_bar->window,
					  url_bar->text.icon);
		if (error != NULL) {
			NSLOG(netsurf, INFO,
			      "xwimp_delete_icon: 0x%x: %s",
			      error->errnum, error->errmess);
			ro_warn_user("WimpError", error->errmess);
			return false;
		}

		url_bar->text.icon = -1;
	}

	/* Create or delete the suggest icon. */
	if (!url_bar->hidden &&
	    url_bar->suggest.icon == -1) {
		icon.icon.data.indirected_text.text = null_text_string;
		icon.icon.data.indirected_text.size = 1;
		icon.icon.data.indirected_text.validation = suggest_validation;
		icon.icon.flags = wimp_ICON_TEXT |
			wimp_ICON_SPRITE |
			wimp_ICON_INDIRECTED |
			wimp_ICON_HCENTRED |
			wimp_ICON_VCENTRED |
			(wimp_BUTTON_CLICK << wimp_ICON_BUTTON_TYPE_SHIFT);

		error = xwimp_create_icon(&icon, &url_bar->suggest.icon);
		if (error) {
			NSLOG(netsurf, INFO, "xwimp_create_icon: 0x%x: %s",
			      error->errnum, error->errmess);
			ro_warn_user("WimpError", error->errmess);
			return false;
		}

		if (!url_bar->display)
			ro_gui_wimp_event_register_menu_gright(url_bar->window,
							       wimp_ICON_WINDOW,
							       url_bar->suggest.icon,
							       ro_gui_url_suggest_menu);

		if (!ro_gui_url_bar_update_urlsuggest(url_bar)) {
			return false;
		}

		resize = true;

	} else if (url_bar->hidden &&
		   url_bar->suggest.icon != -1) {
		ro_gui_wimp_event_deregister(url_bar->window,
					     url_bar->suggest.icon);
		error = xwimp_delete_icon(url_bar->window,
					  url_bar->suggest.icon);
		if (error != NULL) {
			NSLOG(netsurf, INFO,
			      "xwimp_delete_icon: 0x%x: %s",
			      error->errnum, error->errmess);
			ro_warn_user("WimpError", error->errmess);
			return false;
		}

		url_bar->suggest.icon = -1;
	}

	/* If any icons were created, resize the bar. */
	if (resize && !ro_gui_url_bar_icon_resize(url_bar, true)) {
		return false;
	}

	/* If there are any icons, apply shading as necessary. */
	if (url_bar->container_icon != -1) {
		ro_gui_set_icon_shaded_state(url_bar->window,
					     url_bar->container_icon,
					     url_bar->shaded);
	}

	if (url_bar->text.icon != -1) {
		ro_gui_set_icon_shaded_state(url_bar->window,
					     url_bar->text.icon,
					     url_bar->shaded);
	}

	if (url_bar->suggest.icon != -1) {
		ro_gui_set_icon_shaded_state(url_bar->window,
					     url_bar->suggest.icon,
					     url_bar->shaded);
	}

	return true;
}


/**
 * Set the state of a URL Bar's hotlist icon.
 *
 * \param *url_bar The URL Bar to update.
 * \param set TRUE to set the hotlist icon; FALSE to clear it.
 */
static void ro_gui_url_bar_set_hotlist(struct url_bar *url_bar, bool set)
{
	if (url_bar == NULL ||
	    set == url_bar->hotlist.set) {
		return;
	}

	url_bar->hotlist.set = set;

	if (!url_bar->hidden) {
		xwimp_force_redraw(url_bar->window,
				   url_bar->hotlist.extent.x0,
				   url_bar->hotlist.extent.y0,
				   url_bar->hotlist.extent.x1,
				   url_bar->hotlist.extent.y1);
	}
}


/**
 * Callback for hlcache.
 */
static nserror
ro_gui_url_bar_res_cb(hlcache_handle *handle,
		      const hlcache_event *event, void *pw)
{
	struct url_bar_resource *r = pw;

	switch (event->type) {
	case CONTENT_MSG_READY:
	case CONTENT_MSG_DONE:
		r->ready = true;
		r->height = content_get_height(handle);
		break;

	default:
		break;
	}

	return NSERROR_OK;
}


/* This is an exported interface documented in url_bar.h */
struct url_bar *ro_gui_url_bar_create(struct theme_descriptor *theme)
{
	struct url_bar *url_bar;

	/* Allocate memory. */

	url_bar = malloc(sizeof(struct url_bar));
	if (url_bar == NULL) {
		NSLOG(netsurf, INFO, "No memory for malloc()");
		return NULL;
	}

	/* Set up default parameters. */

	url_bar->theme = theme;
	url_bar->sprites = ro_gui_theme_get_sprites(theme);

	url_bar->display = false;
	url_bar->shaded = false;

	url_bar->x_min = URLBAR_FAVICON_WIDTH + URLBAR_MIN_WIDTH +
		URLBAR_HOTLIST_WIDTH;
	url_bar->y_min = URLBAR_HEIGHT;

	url_bar->extent.x0 = 0;
	url_bar->extent.y0 = 0;
	url_bar->extent.x1 = 0;
	url_bar->extent.y1 = 0;

	url_bar->window = NULL;
	url_bar->container_icon = -1;
	url_bar->text.icon = -1;
	url_bar->suggest.icon = -1;

	url_bar->pginfo.extent.x0 = 0;
	url_bar->pginfo.extent.y0 = 0;
	url_bar->pginfo.extent.x1 = 0;
	url_bar->pginfo.extent.y1 = 0;
	strncpy(url_bar->pginfo.sprite,
		"pgiinternal",
		URLBAR_PGINFO_NAME_LENGTH);

	url_bar->favicon.extent.x0 = 0;
	url_bar->favicon.extent.y0 = 0;
	url_bar->favicon.extent.x1 = 0;
	url_bar->favicon.extent.y1 = 0;
	url_bar->favicon.width = 0;
	url_bar->favicon.height = 0;
	url_bar->favicon.content = NULL;
	url_bar->favicon.type = 0;
	strncpy(url_bar->favicon.sprite,
		"Ssmall_xxx",
		URLBAR_FAVICON_NAME_LENGTH);

	url_bar->hotlist.set = false;
	url_bar->hotlist.extent.x0 = 0;
	url_bar->hotlist.extent.y0 = 0;
	url_bar->hotlist.extent.x1 = 0;
	url_bar->hotlist.extent.y1 = 0;

	url_bar->text.size = RO_GUI_MAX_URL_SIZE;
	url_bar->text.buffer = malloc(url_bar->text.size);
	if (url_bar->text.buffer == NULL) {
		free(url_bar);
		return NULL;
	}
	url_bar->text.buffer[0] = 0;
	url_bar->text.buffer_utf8 = NULL;

	url_bar->hidden = false;

	return url_bar;
}


/* This is an exported interface documented in url_bar.h */
bool
ro_gui_url_bar_rebuild(struct url_bar *url_bar,
		       struct theme_descriptor *theme,
		       theme_style style,
		       wimp_w window,
		       bool display,
		       bool shaded)
{
	if (url_bar == NULL) {
		return false;
	}

	url_bar->theme = theme;
	url_bar->window = window;

	url_bar->display = display;
	url_bar->shaded = shaded;

	url_bar->container_icon = -1;
	url_bar->text.icon = -1;
	url_bar->suggest.icon = -1;

	ro_gui_wimp_get_sprite_dimensions((osspriteop_area *) -1,
					  suggest_icon,
					  &url_bar->suggest.x,
					  &url_bar->suggest.y);

	url_bar->x_min = URLBAR_PGINFO_WIDTH +
		URLBAR_FAVICON_WIDTH +
		URLBAR_MIN_WIDTH +
		URLBAR_HOTLIST_WIDTH +
		URLBAR_GRIGHT_GUTTER +
		url_bar->suggest.x;

	url_bar->y_min = (url_bar->suggest.y > URLBAR_HEIGHT) ?
		url_bar->suggest.y : URLBAR_HEIGHT;

	return ro_gui_url_bar_icon_update(url_bar);
}


/* This is an exported interface documented in url_bar.h */
void ro_gui_url_bar_destroy(struct url_bar *url_bar)
{
	if (url_bar == NULL) {
		return;
	}

	if (url_bar->text.buffer_utf8 != NULL) {
		free(url_bar->text.buffer_utf8);
	}

	if (url_bar->text.buffer != NULL) {
		free(url_bar->text.buffer);
	}

	free(url_bar);
}


/* This is an exported interface documented in url_bar.h */
bool ro_gui_url_bar_get_dims(struct url_bar *url_bar, int *width, int *height)
{
	if (url_bar == NULL) {
		return false;
	}

	if (url_bar->x_min != -1 &&
	    url_bar->y_min != -1) {
		if (width != NULL) {
			*width = url_bar->x_min;
		}
		if (height != NULL) {
			*height = url_bar->y_min;
		}

		return true;
	}

	return false;
}


/* This is an exported interface documented in url_bar.h */
bool
ro_gui_url_bar_set_extent(struct url_bar *url_bar,
			  int x0, int y0, int x1, int y1)
{
	bool stretch;

	if (url_bar == NULL) {
		return false;
	}

	if ((x1 - x0) < url_bar->x_min ||
	    (y1 - y0) < url_bar->y_min) {
		return false;
	}

	if (url_bar->extent.x0 == x0 &&
	    url_bar->extent.y0 == y0 &&
	    url_bar->extent.x1 == x1 &&
	    url_bar->extent.y1 == y1) {
		return true;
	}

	/* If it's only the length that changes, less needs to be updated. */
	stretch = (url_bar->extent.x0 == x0 &&
		   url_bar->extent.y0 == y0 &&
		   url_bar->extent.y1 == y1) ? true : false;

	/* Redraw the relevant bits of the toolbar. */
	if (url_bar->window != NULL &&
	    !url_bar->hidden) {
		if (stretch) {
			xwimp_force_redraw(url_bar->window,
					   x0 + URLBAR_PGINFO_WIDTH + URLBAR_FAVICON_WIDTH, y0,
					   (x1 > url_bar->extent.x1) ?
					   x1 : url_bar->extent.x1, y1);
		} else {
			xwimp_force_redraw(url_bar->window,
					   url_bar->extent.x0,
					   url_bar->extent.y0,
					   url_bar->extent.x1,
					   url_bar->extent.y1);
			xwimp_force_redraw(url_bar->window, x0, y0, x1, y1);
		}
	}

	/* Reposition the URL bar icons. */
	url_bar->extent.x0 = x0;
	url_bar->extent.y0 = y0;
	url_bar->extent.x1 = x1;
	url_bar->extent.y1 = y1;

	return ro_gui_url_bar_icon_resize(url_bar, !stretch);
}


/* This is an exported interface documented in url_bar.h */
bool ro_gui_url_bar_hide(struct url_bar *url_bar, bool hide)
{
	if (url_bar == NULL || url_bar->hidden == hide)
		return (url_bar == NULL) ? false : true;

	url_bar->hidden = hide;

	return ro_gui_url_bar_icon_update(url_bar);
}


/* This is an exported interface documented in url_bar.h */
void ro_gui_url_bar_redraw(struct url_bar *url_bar, wimp_draw *redraw)
{
	wimp_icon icon;
	struct rect clip;
	bool draw_pginfo = true;
	bool draw_favicon = true;
	bool draw_hotlist = true;

	/* Test for a valid URL bar */
	if (url_bar == NULL || url_bar->hidden)
		return;

	if ((redraw->clip.x0 - (redraw->box.x0 - redraw->xscroll)) >
	    (url_bar->pginfo.extent.x1) ||
	    (redraw->clip.y0 - (redraw->box.y1 - redraw->yscroll)) >
	    (url_bar->pginfo.extent.y1) ||
	    (redraw->clip.x1 - (redraw->box.x0 - redraw->xscroll)) <
	    (url_bar->pginfo.extent.x0) ||
	    (redraw->clip.y1 - (redraw->box.y1 - redraw->yscroll)) <
	    (url_bar->pginfo.extent.y0)) {
		/* page info not in redraw area */
		draw_pginfo = false;
	}

	if ((redraw->clip.x0 - (redraw->box.x0 - redraw->xscroll)) >
	    (url_bar->favicon.extent.x1) ||
	    (redraw->clip.y0 - (redraw->box.y1 - redraw->yscroll)) >
	    (url_bar->favicon.extent.y1) ||
	    (redraw->clip.x1 - (redraw->box.x0 - redraw->xscroll)) <
	    (url_bar->favicon.extent.x0) ||
	    (redraw->clip.y1 - (redraw->box.y1 - redraw->yscroll)) <
	    (url_bar->favicon.extent.y0)) {
		/* Favicon not in redraw area */
		draw_favicon = false;
	}

	if ((redraw->clip.x0 - (redraw->box.x0 - redraw->xscroll)) >
	    (url_bar->hotlist.extent.x1) ||
	    (redraw->clip.y0 - (redraw->box.y1 - redraw->yscroll)) >
	    (url_bar->hotlist.extent.y1) ||
	    (redraw->clip.x1 - (redraw->box.x0 - redraw->xscroll)) <
	    (url_bar->hotlist.extent.x0) ||
	    (redraw->clip.y1 - (redraw->box.y1 - redraw->yscroll)) <
	    (url_bar->hotlist.extent.y0)) {
		/* Hotlist icon not in redraw area */
		draw_hotlist = false;
	}

	if (draw_pginfo) {
		icon.flags = wimp_ICON_SPRITE |
			wimp_ICON_INDIRECTED |
			wimp_ICON_FILLED |
			wimp_ICON_HCENTRED |
			wimp_ICON_VCENTRED |
			(wimp_COLOUR_WHITE << wimp_ICON_BG_COLOUR_SHIFT);
		icon.data.indirected_sprite.id = (osspriteop_id)url_bar->pginfo.sprite;
		icon.data.indirected_sprite.area = url_bar->sprites;
		icon.data.indirected_sprite.size = 12;

		icon.extent.x0 = url_bar->pginfo.extent.x0;
		icon.extent.x1 = url_bar->pginfo.extent.x1;
		icon.extent.y0 = url_bar->pginfo.extent.y0;
		icon.extent.y1 = url_bar->pginfo.extent.y1;

		xwimp_plot_icon(&icon);
	}

	if (draw_favicon) {
		if (url_bar->favicon.content == NULL) {
			icon.data.indirected_text.text = null_text_string;
			icon.data.indirected_text.validation =
				url_bar->favicon.sprite;
			icon.data.indirected_text.size = 1;
			icon.flags = wimp_ICON_TEXT | wimp_ICON_SPRITE |
				wimp_ICON_INDIRECTED |
				wimp_ICON_FILLED |
				wimp_ICON_HCENTRED |
				wimp_ICON_VCENTRED;
			icon.extent.x0 = url_bar->favicon.extent.x0;
			icon.extent.x1 = url_bar->favicon.extent.x1;
			icon.extent.y0 = url_bar->favicon.extent.y0;
			icon.extent.y1 = url_bar->favicon.extent.y1;

			xwimp_plot_icon(&icon);
		} else {
			struct content_redraw_data data;
			struct redraw_context ctx = {
				.interactive = true,
				.background_images = true,
				.plot = &ro_plotters
			};

			xwimp_set_colour(wimp_COLOUR_WHITE);
			xos_plot(os_MOVE_TO,
				 (redraw->box.x0 - redraw->xscroll) +
				 url_bar->favicon.extent.x0,
				 (redraw->box.y1 - redraw->yscroll) +
				 url_bar->favicon.extent.y0);
			xos_plot(os_PLOT_TO | os_PLOT_RECTANGLE,
				 (redraw->box.x0 - redraw->xscroll) +
				 url_bar->favicon.extent.x1,
				 (redraw->box.y1 - redraw->yscroll) +
				 url_bar->favicon.extent.y1);

			clip.x0 = (redraw->clip.x0 - ro_plot_origin_x) / 2;
			clip.y0 = (ro_plot_origin_y - redraw->clip.y0) / 2;
			clip.x1 = (redraw->clip.x1 - ro_plot_origin_x) / 2;
			clip.y1 = (ro_plot_origin_y - redraw->clip.y1) / 2;

			data.x = (url_bar->favicon.extent.x0 +
				  url_bar->favicon.offset.x) / 2;
			data.y = (url_bar->favicon.offset.y -
				  url_bar->favicon.extent.y1) / 2;
			data.width = url_bar->favicon.width;
			data.height = url_bar->favicon.height;
			data.background_colour = 0xFFFFFF;
			data.scale = 1;
			data.repeat_x = false;
			data.repeat_y = false;
			data.background_blend_mode = 0;

			content_redraw(url_bar->favicon.content,
				       &data, &clip, &ctx);
		}
	}

	if (draw_hotlist) {
		struct content_redraw_data data;
		struct redraw_context ctx = {
			.interactive = true,
			.background_images = true,
			.plot = &ro_plotters
		};
		struct url_bar_resource *hotlist_icon = url_bar->hotlist.set ?
			&(url_bar_res[URLBAR_RES_HOTLIST_REMOVE]) :
			&(url_bar_res[URLBAR_RES_HOTLIST_ADD]);

		xwimp_set_colour(wimp_COLOUR_WHITE);
		xos_plot(os_MOVE_TO,
			 (redraw->box.x0 - redraw->xscroll) +
			 url_bar->hotlist.extent.x0,
			 (redraw->box.y1 - redraw->yscroll) +
			 url_bar->hotlist.extent.y0);
		xos_plot(os_PLOT_TO | os_PLOT_RECTANGLE,
			 (redraw->box.x0 - redraw->xscroll) +
			 url_bar->hotlist.extent.x1,
			 (redraw->box.y1 - redraw->yscroll) +
			 url_bar->hotlist.extent.y1);

		if (hotlist_icon->ready == false) {
			return;
		}

		clip.x0 = (redraw->clip.x0 - ro_plot_origin_x) / 2;
		clip.y0 = (ro_plot_origin_y - redraw->clip.y0) / 2;
		clip.x1 = (redraw->clip.x1 - ro_plot_origin_x) / 2;
		clip.y1 = (ro_plot_origin_y - redraw->clip.y1) / 2;

		data.x = (url_bar->hotlist.extent.x0 +
			  url_bar->hotlist.offset.x) / 2;
		data.y = (url_bar->hotlist.offset.y -
			  url_bar->hotlist.extent.y1) / 2;
		data.width = URLBAR_HOTLIST_SIZE;
		data.height = URLBAR_HOTLIST_SIZE;
		data.background_colour = 0xFFFFFF;
		data.scale = 1;
		data.repeat_x = false;
		data.repeat_y = false;
		data.background_blend_mode = 0;

		content_redraw(hotlist_icon->c, &data, &clip, &ctx);
	}
}

/**
 * check if os point is inside an os box
 *
 * \param pos The coordinate of the point
 * \param box The box to check against
 * \return true if point is inside the box else false
 */
static inline bool is_point_in_box(os_coord *pos, os_box *box)
{
	if (pos->x < box->x0 ||
	    pos->x > box->x1 ||
	    pos->y < box->y0 ||
	    pos->y > box->y1) {
		return false;
	}
	return true;
}

/* This is an exported interface documented in url_bar.h */
bool
ro_gui_url_bar_click(struct url_bar *url_bar,
		     wimp_pointer *pointer,
		     wimp_window_state *state,
		     url_bar_action *action)
{
	os_coord pos;

	if (url_bar == NULL || url_bar->hidden ||
	    url_bar->display || url_bar->shaded) {
		return false;
	}

	/* Check that the click was within our part of the window. */
	pos.x = pointer->pos.x - state->visible.x0 + state->xscroll;
	pos.y = pointer->pos.y - state->visible.y1 + state->yscroll;

	if (!is_point_in_box(&pos, &url_bar->extent)) {
		return false;
	}

	/* If we have a Select or Adjust click, check if it originated on the
	 * hotlist icon; if it did, return an event.
	 */
	if (pointer->buttons == wimp_SINGLE_SELECT ||
	    pointer->buttons == wimp_SINGLE_ADJUST) {
		if (is_point_in_box(&pos, &url_bar->hotlist.extent)) {
			if (pointer->buttons == wimp_SINGLE_SELECT &&
			    action != NULL) {
				*action = TOOLBAR_URL_SELECT_HOTLIST;
			} else if (pointer->buttons == wimp_SINGLE_ADJUST &&
				   action != NULL) {
				*action = TOOLBAR_URL_ADJUST_HOTLIST;
			}
			return true;
		}

		if (is_point_in_box(&pos, &url_bar->pginfo.extent)) {
			if (pointer->buttons == wimp_SINGLE_SELECT &&
			    action != NULL) {
				*action = TOOLBAR_URL_SELECT_PGINFO;
			} else if (pointer->buttons == wimp_SINGLE_ADJUST &&
				   action != NULL) {
				*action = TOOLBAR_URL_ADJUST_PGINFO;
			}
			return true;
		}

	}

	/* If we find a Select or Adjust drag, check if it originated on the
	 * URL bar or over the favicon.  If either, then return an event.
	 */
	if (pointer->buttons == wimp_DRAG_SELECT ||
	    pointer->buttons == wimp_DRAG_ADJUST) {
		if (!ns_wimp_has_text_selection()) {
			if (pointer->i == url_bar->text.icon) {
				if (action != NULL) {
					*action = TOOLBAR_URL_DRAG_URL;
				}
				return true;
			}
		}

		if (is_point_in_box(&pos, &url_bar->favicon.extent)) {
			if (action != NULL) {
				*action = TOOLBAR_URL_DRAG_FAVICON;
			}
			return true;
		}
	}

	return false;
}


/* This is an exported interface documented in url_bar.h */
bool
ro_gui_url_bar_menu_prepare(struct url_bar *url_bar,
			    wimp_i i,
			    wimp_menu *menu,
			    wimp_pointer *pointer)
{
	if (url_bar == NULL ||
	    url_bar->suggest.icon != i ||
	    menu != ro_gui_url_suggest_menu) {
		return false;
	}

	if (pointer != NULL) {
		return ro_gui_url_suggest_prepare_menu();
	}

	return true;
}


/* This is an exported interface documented in url_bar.h */
bool
ro_gui_url_bar_menu_select(struct url_bar *url_bar,
			   wimp_i i,
			   wimp_menu *menu,
			   wimp_selection *selection,
			   menu_action action)
{
	const char *urltxt;
	struct gui_window *g;

	if (url_bar == NULL ||
	    url_bar->suggest.icon != i ||
	    menu != ro_gui_url_suggest_menu) {
		return false;
	}

	urltxt = ro_gui_url_suggest_get_selection(selection);
	g = ro_gui_toolbar_lookup(url_bar->window);

	if (urltxt != NULL &&
	    g != NULL &&
	    g->bw != NULL) {
		nsurl *url;
		nserror error;

		error = nsurl_create(urltxt, &url);
		if (error != NSERROR_OK) {
			ro_warn_user(messages_get_errorcode(error), 0);
		} else {
			ro_gui_window_set_url(g, url);

			browser_window_navigate(g->bw,
						url,
						NULL,
						BW_NAVIGATE_HISTORY,
						NULL,
						NULL,
						NULL);
			nsurl_unref(url);
		}
	}

	return true;
}


/* This is an exported interface documented in url_bar.h */
bool
ro_gui_url_bar_help_suffix(struct url_bar *url_bar,
			   wimp_i i,
			   os_coord *mouse,
			   wimp_window_state *state,
			   wimp_mouse_state buttons,
			   const char **suffix)
{
	os_coord pos;

	if (url_bar == NULL || url_bar->hidden) {
		return false;
	}

	/* Check that the click was within our part of the window. */

	pos.x = mouse->x - state->visible.x0 + state->xscroll;
	pos.y = mouse->y - state->visible.y1 + state->yscroll;

	if (pos.x < url_bar->extent.x0 ||
	    pos.x > url_bar->extent.x1 ||
	    pos.y < url_bar->extent.y0 ||
	    pos.y > url_bar->extent.y1) {
		return false;
	}

	/* Return hard-coded icon numbers that match the ones that were
	 * always allocated to the URL bar in a previous implementation.
	 * If Messages can be updated, this could be changed.
	 */

	if (i == url_bar->text.icon) {
		*suffix = "14";
	} else if (i == url_bar->suggest.icon) {
		*suffix = "15";
	} else if (pos.x >= url_bar->hotlist.extent.x0 &&
		   pos.x <= url_bar->hotlist.extent.x1 &&
		   pos.y >= url_bar->hotlist.extent.y0 &&
		   pos.y <= url_bar->hotlist.extent.y1) {
		*suffix = "Hot";
	} else if (pos.x >= url_bar->favicon.extent.x0 &&
		   pos.x <= url_bar->favicon.extent.x1 &&
		   pos.y >= url_bar->favicon.extent.y0 &&
		   pos.y <= url_bar->favicon.extent.y1) {
		*suffix = "Fav";
	} else {
		*suffix = "";
	}

	return true;
}


/* This is an exported interface documented in url_bar.h */
bool ro_gui_url_bar_take_caret(struct url_bar *url_bar)
{
	os_error *error;

	if (url_bar == NULL || url_bar->hidden) {
		return false;
	}

	error = xwimp_set_caret_position(url_bar->window,
					 url_bar->text.icon,
					 -1, -1, -1, 0);
	if (error) {
		NSLOG(netsurf, INFO,
		      "xwimp_set_caret_position: 0x%x: %s",
		      error->errnum, error->errmess);
		ro_warn_user("WimpError", error->errmess);

		return false;
	}

	return true;
}


/* This is an exported interface documented in url_bar.h */
void
ro_gui_url_bar_set_url(struct url_bar *url_bar,
		       const char *url,
		       bool is_utf8,
		       bool set_caret)
{
	wimp_caret caret;
	os_error *error;
	char *local_text = NULL;
	const char *local_url;
	nsurl *n;

	if (url_bar == NULL ||
	    url_bar->text.buffer == NULL ||
	    url == NULL) {
		return;
	}

	/* Before we do anything with the URL, get it into local encoding so
	 * that behaviour is consistent with the rest of the URL Bar module
	 * (which will act on the icon's text buffer, which is always in local
	 * encoding).
	 */

	if (is_utf8) {
		nserror err;

		err = utf8_to_local_encoding(url, 0, &local_text);
		if (err != NSERROR_OK) {
			/* A bad encoding should never happen, so assert this */
			assert(err != NSERROR_BAD_ENCODING);
			NSLOG(netsurf, INFO, "utf8_to_enc failed");
			/* Paranoia */
			local_text = NULL;
		}
		local_url = (local_text != NULL) ? local_text : url;
	} else {
		local_url = url;
	}

	/* Copy the text into the icon buffer. If the text is too long, truncate
	 * for URL bar and log the full URL.
	 */
	if (strlen(local_url) >= url_bar->text.size) {
		NSLOG(netsurf, WARNING,
		      "URL too long to show in URL bar (%zu chars): %s",
		      strlen(url), url);
	}

	strncpy(url_bar->text.buffer, local_url, url_bar->text.size - 1);
	url_bar->text.buffer[url_bar->text.size - 1] = '\0';

	if (local_text != NULL) {
		free(local_text);
	}

	/* Set the hotlist flag. */
	if (nsurl_create(url, &n) == NSERROR_OK) {
		ro_gui_url_bar_set_hotlist(url_bar, ro_gui_hotlist_has_page(n));
		nsurl_unref(n);
	}

	/* If there's no icon, then there's nothing else to do... */
	if (url_bar->text.icon == -1) {
		return;
	}

	/* ...if there is, redraw the icon and fix the caret's position. */
	ro_gui_redraw_icon(url_bar->window, url_bar->text.icon);

	error = xwimp_get_caret_position(&caret);
	if (error) {
		NSLOG(netsurf, INFO,
		      "xwimp_get_caret_position: 0x%x: %s",
		      error->errnum, error->errmess);
		ro_warn_user("WimpError", error->errmess);
		return;
	}

	if (set_caret ||
	    (caret.w == url_bar->window &&
	     caret.i == url_bar->text.icon)) {
		const char *set_url;
		set_url = ro_gui_get_icon_string(url_bar->window,
						 url_bar->text.icon);

		error = xwimp_set_caret_position(url_bar->window,
						 url_bar->text.icon,
						 0, 0, -1,
						 strlen(set_url));
		if (error) {
			NSLOG(netsurf, INFO,
			      "xwimp_set_caret_position: 0x%x: %s",
			      error->errnum,
			      error->errmess);
			ro_warn_user("WimpError", error->errmess);
		}
	}
}


/* This is an exported interface documented in url_bar.h */
void ro_gui_url_bar_update_hotlist(struct url_bar *url_bar)
{
	const char *url;
	nsurl *n;

	if (url_bar == NULL) {
		return;
	}

	url = (const char *) url_bar->text.buffer;
	if (url != NULL &&
	    nsurl_create(url, &n) == NSERROR_OK) {
		ro_gui_url_bar_set_hotlist(url_bar, ro_gui_hotlist_has_page(n));
		nsurl_unref(n);
	}
}


/* This is an exported interface documented in url_bar.h */
const char *ro_gui_url_bar_get_url(struct url_bar *url_bar)
{
	nserror res;

	if ((url_bar == NULL) ||
	    (url_bar->text.buffer == NULL)) {
		return NULL;
	}

	if (url_bar->text.buffer_utf8 != NULL) {
		free(url_bar->text.buffer_utf8);
		url_bar->text.buffer_utf8 = NULL;
	}

	if (url_bar->text.buffer[0] == '\0') {
		return (const char *) url_bar->text.buffer;
	}

	res = utf8_from_local_encoding(url_bar->text.buffer, 0,
				       &url_bar->text.buffer_utf8);
	if (res == NSERROR_OK) {
		return (const char *)url_bar->text.buffer_utf8;
	}

	return (const char *) url_bar->text.buffer;
}


/* This is an exported interface documented in url_bar.h */
bool ro_gui_url_bar_get_url_extent(struct url_bar *url_bar, os_box *extent)
{
	wimp_icon_state state;
	os_error *error;

	if (url_bar == NULL || url_bar->hidden) {
		return false;
	}

	if (extent == NULL) {
		return true;
	}

	state.w = url_bar->window;
	state.i = url_bar->container_icon;
	error = xwimp_get_icon_state(&state);
	if (error) {
		NSLOG(netsurf, INFO,
		      "xwimp_get_icon_state: 0x%x: %s",
		      error->errnum, error->errmess);
		ro_warn_user("WimpError", error->errmess);
		return false;
	}

	extent->x0 = state.icon.extent.x0;
	extent->y0 = state.icon.extent.y0;
	extent->x1 = state.icon.extent.x1;
	extent->y1 = state.icon.extent.y1;

	return true;
}


/* This is an exported interface documented in url_bar.h */
bool
ro_gui_url_bar_test_for_text_field_click(struct url_bar *url_bar,
					 wimp_pointer *pointer)
{
	if (url_bar == NULL ||
	    url_bar->hidden ||
	    pointer == NULL) {
		return false;
	}

	return (pointer->w == url_bar->window &&
		pointer->i == url_bar->text.icon) ? true : false;
}


/* This is an exported interface documented in url_bar.h */
bool
ro_gui_url_bar_test_for_text_field_keypress(struct url_bar *url_bar,
					    wimp_key *key)
{
	const char *url;
	nsurl *n;

	if (url_bar == NULL ||
	    url_bar->hidden ||
	    key == NULL) {
		return false;
	}

	if (key->w != url_bar->window ||
	    key->i != url_bar->text.icon) {
		return false;
	}

	/* Update hotlist indicator */

	url = (const char *) url_bar->text.buffer;
	if (url != NULL &&
	    nsurl_create(url, &n) == NSERROR_OK) {
		ro_gui_url_bar_set_hotlist(url_bar, ro_gui_hotlist_has_page(n));
		nsurl_unref(n);
	} else if (url_bar->hotlist.set) {
		ro_gui_url_bar_set_hotlist(url_bar, false);
	}

	return true;
}


/* This is an exported interface documented in url_bar.h */
bool
ro_gui_url_bar_set_site_favicon(struct url_bar *url_bar,
				struct hlcache_handle *h)
{
	content_type type = CONTENT_NONE;

	if (url_bar == NULL) {
		return false;
	}

	if (h != NULL) {
		type = content_get_type(h);
	}

	// \TODO -- Maybe test for CONTENT_ICO ???

	if (type == CONTENT_IMAGE) {
		url_bar->favicon.content = h;
		url_bar->favicon.width = content_get_width(h);
		url_bar->favicon.height = content_get_height(h);

		if (url_bar->favicon.width > URLBAR_FAVICON_SIZE) {
			url_bar->favicon.width = URLBAR_FAVICON_SIZE;
		}

		if (url_bar->favicon.height > URLBAR_FAVICON_SIZE) {
			url_bar->favicon.height = URLBAR_FAVICON_SIZE;
		}

		url_bar->favicon.offset.x = ((url_bar->favicon.extent.x1 -
					      url_bar->favicon.extent.x0) -
					     (url_bar->favicon.width * 2)) / 2;
		url_bar->favicon.offset.y = ((url_bar->favicon.extent.y1 -
					      url_bar->favicon.extent.y0) -
					     (url_bar->favicon.height * 2)) / 2;
	} else {
		url_bar->favicon.content = NULL;

		if (url_bar->favicon.type != 0) {
			snprintf(url_bar->favicon.sprite,
				 URLBAR_FAVICON_NAME_LENGTH,
				 "Ssmall_%.3x", url_bar->favicon.type);
		} else {
			snprintf(url_bar->favicon.sprite,
				 URLBAR_FAVICON_NAME_LENGTH,
				 "Ssmall_xxx");
		}
	}

	if (!url_bar->hidden) {
		xwimp_force_redraw(url_bar->window,
				   url_bar->favicon.extent.x0,
				   url_bar->favicon.extent.y0,
				   url_bar->favicon.extent.x1,
				   url_bar->favicon.extent.y1);
	}

	return true;
}


/* This is an exported interface documented in url_bar.h */
bool ro_gui_url_bar_page_info_change(struct url_bar *url_bar)
{
	browser_window_page_info_state pistate;
	const char *icon_name;
	struct gui_window *g;

	g = ro_gui_toolbar_lookup(url_bar->window);

	pistate = browser_window_get_page_info_state(g->bw);

	switch (pistate) {
	case PAGE_STATE_LOCAL:
		icon_name = "pgilocal";
		break;

	case PAGE_STATE_INSECURE:
		icon_name = "pgiinsecure";
		break;

	case PAGE_STATE_SECURE_OVERRIDE:
		icon_name = "pgiwarning";
		break;

	case PAGE_STATE_SECURE_ISSUES:
		icon_name = "pgiwarning";
		break;

	case PAGE_STATE_SECURE:
		icon_name = "pgisecure";
		break;

	case PAGE_STATE_INTERNAL:
	default:
		icon_name = "pgiinternal";
		break;
	}

	strncpy(url_bar->pginfo.sprite,	icon_name, URLBAR_PGINFO_NAME_LENGTH);

	if (!url_bar->hidden) {
		xwimp_force_redraw(url_bar->window,
				   url_bar->pginfo.extent.x0,
				   url_bar->pginfo.extent.y0,
				   url_bar->pginfo.extent.x1,
				   url_bar->pginfo.extent.y1);
	}

	return true;
}


/* This is an exported interface documented in url_bar.h */
bool
ro_gui_url_bar_set_content_favicon(struct url_bar *url_bar,
				   struct gui_window *g)
{
	int type = 0;
	char sprite[URLBAR_FAVICON_NAME_LENGTH-1];
	struct hlcache_handle *h;

	if (url_bar == NULL ||
	    g == NULL) {
		return false;
	}

	h = browser_window_get_content(g->bw);
	if (h != NULL) {
		type = ro_content_filetype(h);
	}

	if (type != 0) {
		snprintf(sprite, URLBAR_FAVICON_NAME_LENGTH,
			 "small_%.3x", type);

		if (!ro_gui_wimp_sprite_exists(sprite)) {
			type = 0;
		}
	}

	url_bar->favicon.type = type;

	if (url_bar->favicon.content == NULL) {
		if (type == 0) {
			snprintf(url_bar->favicon.sprite,
				 URLBAR_FAVICON_NAME_LENGTH, "Ssmall_xxx");
		} else {
			snprintf(url_bar->favicon.sprite,
				 URLBAR_FAVICON_NAME_LENGTH, "S%s", sprite);
		}

		if (!url_bar->hidden) {
			xwimp_force_redraw(url_bar->window,
					   url_bar->favicon.extent.x0,
					   url_bar->favicon.extent.y0,
					   url_bar->favicon.extent.x1,
					   url_bar->favicon.extent.y1);
		}
	}

	return true;
}


/* This is an exported interface documented in url_bar.h */
bool ro_gui_url_bar_update_urlsuggest(struct url_bar *url_bar)
{
	if (url_bar == NULL ||
	    url_bar->hidden) {
		return (url_bar == NULL) ? false : true;
	}

	if (url_bar->window != NULL &&
	    url_bar->suggest.icon != -1) {
		ro_gui_set_icon_shaded_state(url_bar->window,
					     url_bar->suggest.icon,
					     !ro_gui_url_suggest_get_menu_available());
	}

	return true;
}


/* Exported interface, documented in url_bar.h */
bool ro_gui_url_bar_init(void)
{
	int i;

	for (i = 0; i < URLBAR_RES_LAST; i++) {
		nsurl *url;
		if (nsurl_create(url_bar_res[i].url, &url) == NSERROR_OK) {
			hlcache_handle_retrieve(url,
						0,
						NULL,
						NULL,
						ro_gui_url_bar_res_cb,
						&(url_bar_res[i]),
						NULL,
						CONTENT_IMAGE,
						&(url_bar_res[i].c));
			nsurl_unref(url);
		}
	}

	return true;
}


/* Exported interface, documented in url_bar.h */
void ro_gui_url_bar_fini(void)
{
	int i;

	for (i = 0; i < URLBAR_RES_LAST; i++) {
		hlcache_handle_release(url_bar_res[i].c);
	}
}
