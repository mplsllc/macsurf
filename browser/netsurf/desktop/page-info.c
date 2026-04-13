/*
 * Copyright 2020 Michael Drake <tlsa@netsurf-browser.org>
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
 * Pave info viewer window implementation
 */

#include <stdlib.h>
#include <string.h>

#include "css/utils.h"

#include "utils/nsurl.h"
#include "utils/nscolour.h"

#include "netsurf/mouse.h"
#include "netsurf/layout.h"
#include "netsurf/keypress.h"
#include "netsurf/plotters.h"
#include "netsurf/core_window.h"
#include "netsurf/browser_window.h"

#include "desktop/knockout.h"
#include "desktop/page-info.h"
#include "desktop/gui_internal.h"
#include "desktop/system_colour.h"

/**
 * Plot style for heading font.
 */
static plot_font_style_t pi__heading[PAGE_STATE__COUNT] = {
	[PAGE_STATE_UNKNOWN] = {
		.family = PLOT_FONT_FAMILY_SANS_SERIF,
		.size = 14 * PLOT_STYLE_SCALE,
		.flags = FONTF_NONE,
		.weight = 400,
	},
	[PAGE_STATE_INTERNAL] = {
		.family = PLOT_FONT_FAMILY_SANS_SERIF,
		.size = 14 * PLOT_STYLE_SCALE,
		.flags = FONTF_NONE,
		.weight = 400,
	},
	[PAGE_STATE_LOCAL] = {
		.family = PLOT_FONT_FAMILY_SANS_SERIF,
		.size = 14 * PLOT_STYLE_SCALE,
		.flags = FONTF_NONE,
		.weight = 400,
	},
	[PAGE_STATE_INSECURE] = {
		.family = PLOT_FONT_FAMILY_SANS_SERIF,
		.size = 14 * PLOT_STYLE_SCALE,
		.flags = FONTF_NONE,
		.weight = 400,
	},
	[PAGE_STATE_SECURE_OVERRIDE] = {
		.family = PLOT_FONT_FAMILY_SANS_SERIF,
		.size = 14 * PLOT_STYLE_SCALE,
		.flags = FONTF_NONE,
		.weight = 400,
	},
	[PAGE_STATE_SECURE_ISSUES] = {
		.family = PLOT_FONT_FAMILY_SANS_SERIF,
		.size = 14 * PLOT_STYLE_SCALE,
		.flags = FONTF_NONE,
		.weight = 400,
	},
	[PAGE_STATE_SECURE] = {
		.family = PLOT_FONT_FAMILY_SANS_SERIF,
		.size = 14 * PLOT_STYLE_SCALE,
		.flags = FONTF_NONE,
		.weight = 400,
	},
};

/**
 * Plot style for domain font.
 */
static plot_font_style_t pi__domain = {
	.family = PLOT_FONT_FAMILY_SANS_SERIF,
	.size = 8 * PLOT_STYLE_SCALE,
	.flags = FONTF_NONE,
	.weight = 700,
};

/**
 * Plot style for item font.
 */
static plot_font_style_t pi__item = {
	.family = PLOT_FONT_FAMILY_SANS_SERIF,
	.size = 11 * PLOT_STYLE_SCALE,
	.flags = FONTF_NONE,
	.weight = 400,
};

/**
 * Plot style for item detail font.
 */
static plot_font_style_t pi__item_detail = {
	.family = PLOT_FONT_FAMILY_SANS_SERIF,
	.size = 11 * PLOT_STYLE_SCALE,
	.flags = FONTF_NONE,
	.weight = 400,
};

/**
 * Plot style for window background.
 */
static plot_style_t pi__bg = {
	.fill_type = PLOT_OP_TYPE_SOLID,
};

/**
 * Plot style for hover background.
 */
static plot_style_t pi__hover = {
	.fill_type = PLOT_OP_TYPE_SOLID,
};

/**
 * An "text" type page info entry.
 */
struct page_info_text {
	const char *text;
	const plot_font_style_t *style;
	int width;
	int height;
	int padding_bottom;
};

/**
 * An "item" type page info entry.
 */
struct page_info_item {
	struct page_info_text item;
	struct page_info_text detail;
	const plot_style_t *hover_bg;
	int padding_bottom;
	int padding_top;
	bool hover;
};

/**
 * List of page info window entries.
 */
enum pi_entry {
	PI_ENTRY_HEADER,
	PI_ENTRY_DOMAIN,
	PI_ENTRY_CERT,
	PI_ENTRY_COOKIES,
	PI_ENTRY__COUNT,
};

/**
 * An entry on a page info window.
 */
struct page_info_entry {
	/**
	 * List of page info entry types.
	 */
	enum page_info_entry_type {
		PAGE_INFO_ENTRY_TYPE_TEXT,
		PAGE_INFO_ENTRY_TYPE_ITEM,
	} type;
	/**
	 * Type-specific page info entry data.
	 */
	union {
		struct page_info_text text;
		struct page_info_item item;
	} u;
};

/**
 * The default page info window data.
 */
struct page_info_entry pi__entries[PI_ENTRY__COUNT] = {
	[PI_ENTRY_HEADER] = {
		.type = PAGE_INFO_ENTRY_TYPE_TEXT,
	},
	[PI_ENTRY_DOMAIN] = {
		.type = PAGE_INFO_ENTRY_TYPE_TEXT,
		.u = {
			.text = {
				.style = &pi__domain,
			},
		},
	},
	[PI_ENTRY_CERT] = {
		.type = PAGE_INFO_ENTRY_TYPE_ITEM,
		.u = {
			.item = {
				.item = {
					.style = &pi__item,
				},
				.detail = {
					.style = &pi__item_detail,
				},
				.hover_bg = &pi__hover,
			},
		},
	},
	[PI_ENTRY_COOKIES] = {
		.type = PAGE_INFO_ENTRY_TYPE_ITEM,
		.u = {
			.item = {
				.item = {
					.style = &pi__item,
				},
				.detail = {
					.style = &pi__item_detail,
				},
				.hover_bg = &pi__hover,
			},
		},
	},
};

/**
 * The page info window structure.
 */
struct page_info {
	struct core_window *cw_h;

	struct browser_window *bw;
	lwc_string *domain;
	enum nsurl_scheme_type scheme;

	browser_window_page_info_state state;
	unsigned cookies;

	char cookie_text[64];
	struct page_info_entry entries[PI_ENTRY__COUNT];

	int width;
	int height;

	int window_padding;
};

/* Exported interface documented in desktop/page_info.h */
nserror page_info_init(void)
{
	pi__bg.fill_colour = nscolours[NSCOLOUR_WIN_EVEN_BG];
	pi__hover.fill_colour = nscolours[NSCOLOUR_WIN_EVEN_BG_HOVER];

	pi__domain.background = nscolours[NSCOLOUR_WIN_EVEN_BG];
	pi__domain.foreground = nscolours[NSCOLOUR_WIN_EVEN_FG];

	pi__item.background = nscolours[NSCOLOUR_WIN_EVEN_BG];
	pi__item.foreground = nscolours[NSCOLOUR_WIN_EVEN_FG];

	pi__item_detail.background = nscolours[NSCOLOUR_WIN_EVEN_BG];
	pi__item_detail.foreground = nscolours[NSCOLOUR_WIN_EVEN_FG_FADED];

	pi__heading[PAGE_STATE_UNKNOWN].background = nscolours[NSCOLOUR_WIN_EVEN_BG];
	pi__heading[PAGE_STATE_UNKNOWN].foreground = nscolours[NSCOLOUR_WIN_EVEN_FG_BAD];
	pi__heading[PAGE_STATE_INTERNAL].background = nscolours[NSCOLOUR_WIN_EVEN_BG];
	pi__heading[PAGE_STATE_INTERNAL].foreground = nscolours[NSCOLOUR_WIN_EVEN_FG];
	pi__heading[PAGE_STATE_LOCAL].background = nscolours[NSCOLOUR_WIN_EVEN_BG];
	pi__heading[PAGE_STATE_LOCAL].foreground = nscolours[NSCOLOUR_WIN_EVEN_FG];
	pi__heading[PAGE_STATE_INSECURE].background = nscolours[NSCOLOUR_WIN_EVEN_BG];
	pi__heading[PAGE_STATE_INSECURE].foreground = nscolours[NSCOLOUR_WIN_EVEN_FG_BAD];
	pi__heading[PAGE_STATE_SECURE_OVERRIDE].background = nscolours[NSCOLOUR_WIN_EVEN_BG];
	pi__heading[PAGE_STATE_SECURE_OVERRIDE].foreground = nscolours[NSCOLOUR_WIN_EVEN_FG_BAD];
	pi__heading[PAGE_STATE_SECURE_ISSUES].background = nscolours[NSCOLOUR_WIN_EVEN_BG];
	pi__heading[PAGE_STATE_SECURE_ISSUES].foreground = nscolours[NSCOLOUR_WIN_EVEN_FG_BAD];
	pi__heading[PAGE_STATE_SECURE].background = nscolours[NSCOLOUR_WIN_EVEN_BG];
	pi__heading[PAGE_STATE_SECURE].foreground = nscolours[NSCOLOUR_WIN_EVEN_FG_GOOD];

	return NSERROR_OK;
}

/* Exported interface documented in desktop/page_info.h */
nserror page_info_fini(void)
{
	return NSERROR_OK;
}

/**
 * Measure the text in the page_info window.
 *
 * \param[in] pi  The page info window handle.
 * \return NSERROR_OK on success, appropriate error code otherwise.
 */
static nserror page_info__measure_text_entry(
		struct page_info_text *pit)
{
	nserror err;
	int height_px;

	err = guit->layout->width(pit->style,
			pit->text, strlen(pit->text),
			&pit->width);
	if (err != NSERROR_OK) {
		return err;
	}

	/* \todo: This needs to be a helper in plot style or in nscss. */
	height_px = ((pit->style->size / PLOT_STYLE_SCALE) *
			FIXTOINT(nscss_screen_dpi) + 36) / 72;

	pit->height = (height_px * 8 + 3) / 6;

	return NSERROR_OK;
}

/**
 * Measure the text in the page_info window.
 *
 * \param[in] pi  The page info window handle.
 * \return NSERROR_OK on success, appropriate error code otherwise.
 */
static nserror page_info__measure_text(
		struct page_info *pi)
{
	nserror err;
	unsigned i;

	for (i = 0; i < PI_ENTRY__COUNT; i++) {
		struct page_info_entry *entry = pi->entries + i;
		int padding;

		switch (entry->type) {
		case PAGE_INFO_ENTRY_TYPE_TEXT:
			err = page_info__measure_text_entry(
					&entry->u.text);
			if (err != NSERROR_OK) {
				return err;
			}
			if (i == PI_ENTRY_DOMAIN) {
				entry->u.text.padding_bottom =
						entry->u.text.height * 3 / 2;
			}
			break;

		case PAGE_INFO_ENTRY_TYPE_ITEM:
			err = page_info__measure_text_entry(
					&entry->u.item.item);
			if (err != NSERROR_OK) {
				return err;
			}
			err = page_info__measure_text_entry(
					&entry->u.item.detail);
			if (err != NSERROR_OK) {
				return err;
			}
			padding = entry->u.item.item.height / 4;
			entry->u.item.padding_top = padding;
			entry->u.item.padding_bottom = padding;

			break;
		}
	}

	pi->window_padding = pi->entries[PI_ENTRY_DOMAIN]
			.u.item.item.height / 2;

	return NSERROR_OK;
}

/**
 * Set the text for the page_info window.
 *
 * \todo Use messages for internationalisation.
 *
 * \param[in] pi  The page info window handle.
 * \return NSERROR_OK on success, appropriate error code otherwise.
 */
static nserror page_info__set_text(
		struct page_info *pi)
{
	int printed;
	static const char *header[PAGE_STATE__COUNT] = {
		[PAGE_STATE_UNKNOWN]         = "Provenance unknown",
		[PAGE_STATE_INTERNAL]        = "NetSurf data",
		[PAGE_STATE_LOCAL]           = "Local data",
		[PAGE_STATE_INSECURE]        = "Connection not secure",
		[PAGE_STATE_SECURE_OVERRIDE] = "Connection not secure",
		[PAGE_STATE_SECURE_ISSUES]   = "Connection not secure",
		[PAGE_STATE_SECURE]          = "Connection is secure",
	};
	static const char *certificate[PAGE_STATE__COUNT] = {
		[PAGE_STATE_UNKNOWN]         = "Missing",
		[PAGE_STATE_INTERNAL]        = "None",
		[PAGE_STATE_LOCAL]           = "None",
		[PAGE_STATE_INSECURE]        = "Not valid",
		[PAGE_STATE_SECURE_OVERRIDE] = "Not valid",
		[PAGE_STATE_SECURE_ISSUES]   = "Not valid",
		[PAGE_STATE_SECURE]          = "Valid",
	};

	assert(pi != NULL);
	assert(pi->state < PAGE_STATE__COUNT);

	pi->entries[PI_ENTRY_HEADER].u.text.style = &pi__heading[pi->state];
	pi->entries[PI_ENTRY_HEADER].u.text.text = header[pi->state];
	pi->entries[PI_ENTRY_DOMAIN].u.text.text = (pi->domain) ?
			lwc_string_data(pi->domain) : "<No domain>";

	pi->entries[PI_ENTRY_CERT].u.item.item.text = "Certificate: ";
	pi->entries[PI_ENTRY_CERT].u.item.detail.text = certificate[pi->state];

	printed = snprintf(pi->cookie_text, sizeof(pi->cookie_text),
			"(%u in use)", pi->cookies);
	if (printed < 0) {
		return NSERROR_UNKNOWN;

	} else if ((unsigned) printed >= sizeof(pi->cookie_text)) {
		return NSERROR_NOSPACE;
	}
	pi->entries[PI_ENTRY_COOKIES].u.item.item.text = "Cookies: ";
	pi->entries[PI_ENTRY_COOKIES].u.item.detail.text = pi->cookie_text;

	return page_info__measure_text(pi);
}

/**
 * Create page info from a browser window.
 *
 * \param[in] pi  The page info window handle.
 * \param[in] bw  Browser window to show page info for.
 * \return NSERROR_OK on success, appropriate error code otherwise.
 */
static nserror page_info__create_from_bw(
		struct page_info *pi,
		struct browser_window *bw)
{
	nsurl *url = browser_window_access_url(bw);

	pi->bw = bw;
	pi->state = browser_window_get_page_info_state(bw);
	pi->cookies = browser_window_get_cookie_count(bw);
	pi->domain = nsurl_get_component(url, NSURL_HOST);
	pi->scheme = nsurl_get_scheme_type(url);

	return page_info__set_text(pi);
}

/**
 * Check whether an entry is relevant.
 *
 * \param[in] entry   The page info entry to consider.
 * \param[in] scheme  URL scheme that the page info is for.
 * \return true if the entry should be hidden, otherwise false.
 */
static inline bool page_info__hide_entry(
		enum pi_entry entry,
		enum nsurl_scheme_type scheme)
{
	switch (entry) {
	case PI_ENTRY_CERT:
		if (scheme != NSURL_SCHEME_HTTPS) {
			return true;
		}
		break;
	case PI_ENTRY_COOKIES:
		if (scheme != NSURL_SCHEME_HTTP &&
		    scheme != NSURL_SCHEME_HTTPS) {
			return true;
		}
		break;
	default:
		break;
	}

	return false;
}

/**
 * Lay out the page info window.
 *
 * \param[in] pi  The page info window handle.
 * \return NSERROR_OK on success, appropriate error code otherwise.
 */
static nserror page_info__layout(
		struct page_info *pi)
{
	int cur_y = 0;
	int max_x = 0;
	unsigned i;

	cur_y += pi->window_padding;
	for (i = 0; i < PI_ENTRY__COUNT; i++) {
		struct page_info_entry *entry = pi->entries + i;

		if (page_info__hide_entry(i, pi->scheme)) {
			continue;
		}

		switch (entry->type) {
		case PAGE_INFO_ENTRY_TYPE_TEXT:
			cur_y += entry->u.text.height;
			if (max_x < entry->u.text.width) {
				max_x = entry->u.text.width;
			}
			cur_y += entry->u.text.padding_bottom;
			break;

		case PAGE_INFO_ENTRY_TYPE_ITEM:
		{
			int full_width = entry->u.item.item.width +
					entry->u.item.detail.width;
			cur_y += entry->u.item.padding_top;
			cur_y += entry->u.item.item.height;
			if (max_x < full_width) {
				max_x = full_width;
			}
			cur_y += entry->u.item.padding_bottom;
		}
			break;
		}
	}
	cur_y += pi->window_padding;
	max_x += pi->window_padding * 2;

	pi->width = max_x;
	pi->height = cur_y;
	return guit->corewindow->set_extent(pi->cw_h, max_x, cur_y);
}

/* Exported interface documented in desktop/page_info.h */
nserror page_info_create(struct core_window *cw_h,
			 struct browser_window *bw,
			 struct page_info **pi_out)
{
	struct page_info *pi;
	nserror err;

	pi = calloc(1, sizeof(*pi));
	if (pi == NULL) {
		return NSERROR_NOMEM;
	}

	pi->cw_h = cw_h;

	memcpy(pi->entries, pi__entries, sizeof(pi__entries));

	err = page_info__create_from_bw(pi, bw);
	if (err != NSERROR_OK) {
		page_info_destroy(pi);
		return err;
	}

	err = page_info__layout(pi);
	if (err != NSERROR_OK) {
		page_info_destroy(pi);
		return err;
	}

	*pi_out = pi;
	return NSERROR_OK;
}

/* Exported interface documented in desktop/page_info.h */
nserror page_info_destroy(struct page_info *pi)
{
	lwc_string_unref(pi->domain);
	free(pi);
	return NSERROR_OK;
}

/* Exported interface documented in desktop/page_info.h */
nserror page_info_set(struct page_info *pgi, struct browser_window *bw)
{
	nserror res;

	lwc_string_unref(pgi->domain);

	res = page_info__create_from_bw(pgi, bw);
	if (res == NSERROR_OK) {
		res = page_info__layout(pgi);
	}

	return res;
}

/**
 * Render a text entry.
 *
 * \param[in] pit  The page info window handle.
 * \param[in] x    X-coordinate to plot at.
 * \param[in] y    Y-coordinate to plot at.
 * \param[in] ctx  Current redraw context.
 * \return NSERROR_OK on success, appropriate error code otherwise.
 */
static nserror page_info__redraw_text_entry(
		const struct page_info_text *pit,
		int x,
		int y,
		const struct redraw_context *ctx)
{
	int baseline = (pit->height * 3 + 2) / 4;

	ctx->plot->text(ctx, pit->style, x, y + baseline,
			pit->text, strlen(pit->text));

	return NSERROR_OK;
}

/* Exported interface documented in desktop/page_info.h */
nserror page_info_redraw(
		const struct page_info *pi,
		int x,
		int y,
		const struct rect *clip,
		const struct redraw_context *ctx)
{
	struct redraw_context new_ctx = *ctx;
	struct rect r;
	int cur_y = y;
	nserror err;
	unsigned i;
	r.x0 = clip->x0 + x;
	r.y0 = clip->y0 + y;
	r.x1 = clip->x1 + x;
	r.y1 = clip->y1 + y;

	/* Start knockout rendering if it's available for this plotter. */
	if (ctx->plot->option_knockout) {
		bool res = knockout_plot_start(ctx, &new_ctx);
		if (res == false) {
			return NSERROR_UNKNOWN;
		}
	}

	/* Set up clip rectangle and draw background. */
	new_ctx.plot->clip(&new_ctx, &r);
	new_ctx.plot->rectangle(&new_ctx, &pi__bg, &r);

	cur_y += pi->window_padding;
	for (i = 0; i < PI_ENTRY__COUNT; i++) {
		const struct page_info_entry *entry = pi->entries + i;
		int cur_x = x + pi->window_padding;

		if (page_info__hide_entry(i, pi->scheme)) {
			continue;
		}

		switch (entry->type) {
		case PAGE_INFO_ENTRY_TYPE_TEXT:
			err = page_info__redraw_text_entry(
					&entry->u.text,
					cur_x, cur_y,
					&new_ctx);
			if (err != NSERROR_OK) {
				goto cleanup;
			}
			cur_y += entry->u.text.height;
			cur_y += entry->u.text.padding_bottom;
			break;

		case PAGE_INFO_ENTRY_TYPE_ITEM:
			if (entry->u.item.hover) {
				r.y0 = cur_y;
				r.y1 = cur_y + entry->u.item.padding_top +
						entry->u.item.item.height +
						entry->u.item.padding_bottom;
				new_ctx.plot->rectangle(&new_ctx,
						&pi__hover, &r);
			}
			cur_y += entry->u.item.padding_top;
			err = page_info__redraw_text_entry(
					&entry->u.item.item,
					cur_x, cur_y,
					&new_ctx);
			if (err != NSERROR_OK) {
				goto cleanup;
			}
			cur_x += entry->u.item.item.width;
			err = page_info__redraw_text_entry(
					&entry->u.item.detail,
					cur_x, cur_y,
					&new_ctx);
			if (err != NSERROR_OK) {
				goto cleanup;
			}
			cur_y += entry->u.item.item.height;
			cur_y += entry->u.item.padding_bottom;
			break;
		}
	}

cleanup:
	/* Rendering complete */
	if (ctx->plot->option_knockout) {
		bool res = knockout_plot_end(ctx);
		if (res == false) {
			return NSERROR_UNKNOWN;
		}
	}

	return NSERROR_OK;
}

/**
 * Handle any clicks on an item.
 *
 * \param[in] pi       The page info window handle.
 * \param[in] mouse    The current mouse state.
 * \param[in] clicked  The page info window entry to consider clicks on.
 * \param[out] did_something Set to true if this click did something
 * \return NSERROR_OK on success, appropriate error code otherwise.
 */
static nserror page_info__handle_item_click(
		struct page_info *pi,
		enum browser_mouse_state mouse,
		enum pi_entry clicked,
		bool *did_something)
{
	nserror err;

	if (!(mouse & BROWSER_MOUSE_CLICK_1)) {
		return NSERROR_OK;
	}

	switch (clicked) {
	case PI_ENTRY_CERT:
		err = browser_window_show_certificates(pi->bw);
		*did_something = true;
		break;
	case PI_ENTRY_COOKIES:
		err = browser_window_show_cookies(pi->bw);
		*did_something = true;
		break;
	default:
		err = NSERROR_OK;
		break;
	}

	return err;
}

/* Exported interface documented in desktop/page_info.h */
nserror page_info_mouse_action(
		struct page_info *pi,
		enum browser_mouse_state mouse,
		int x,
		int y,
		bool *did_something)
{
	int cur_y = 0;
	nserror err;
	unsigned i;

	cur_y += pi->window_padding;
	for (i = 0; i < PI_ENTRY__COUNT; i++) {
		struct page_info_entry *entry = pi->entries + i;
		bool hovering = false;
		int height;

		if (page_info__hide_entry(i, pi->scheme)) {
			continue;
		}

		switch (entry->type) {
		case PAGE_INFO_ENTRY_TYPE_TEXT:
			cur_y += entry->u.text.height;
			cur_y += entry->u.text.padding_bottom;
			break;

		case PAGE_INFO_ENTRY_TYPE_ITEM:
			height = entry->u.item.padding_top +
			         entry->u.item.item.height +
			         entry->u.item.padding_bottom;

			if (y >= cur_y && y < cur_y + height) {
				hovering = true;
				err = page_info__handle_item_click(
						pi, mouse, i, did_something);
				if (err != NSERROR_OK) {
					return err;
				}
			}
			if (entry->u.item.hover != hovering) {
				int w, h;
				struct rect r = {
					.x0 = 0,
					.y0 = cur_y,
					.y1 = cur_y + height,
				};
				guit->corewindow->get_dimensions(pi->cw_h, &w, &h);
				r.x1 = (pi->width > w) ? pi->width : w;

				guit->corewindow->invalidate(pi->cw_h, &r);
			}
			entry->u.item.hover = hovering;
			cur_y += height;
			break;
		}
	}

	return NSERROR_OK;
}

/* Exported interface documented in desktop/page_info.h */
bool page_info_keypress(
		struct page_info *pi,
		int32_t key)
{
	return NSERROR_OK;
}

/* Exported interface documented in desktop/page_info.h */
nserror page_info_get_size(
		struct page_info *pi,
		int *width,
		int *height)
{
	*width = pi->width;
	*height = pi->height;

	return NSERROR_OK;
}
