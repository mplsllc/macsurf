/*
 * Copyright 2005 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2003 Phil Mellor <monkeyson@users.sourceforge.net>
 * Copyright 2005 John M Bell <jmb202@ecs.soton.ac.uk>
 * Copyright 2006 Richard Wilson <info@tinct.net>
 * Copyright 2008 Michael Drake <tlsa@netsurf-browser.org>
 * Copyright 2020 Vincent Sanders <vince@netsurf-browser.org>
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
 * Implementation of special element handling conversion.
 */

#include <string.h>
#include <stdbool.h>
#include <dom/dom.h>

#include "utils/nsoption.h"
#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/talloc.h"
#include "utils/string.h"
#include "utils/ascii.h"
#include "utils/nsurl.h"
#include "netsurf/plot_style.h"
#include "css/hints.h"
#include "desktop/frame_types.h"
#include "content/content_factory.h"
#include "macsurf_debug.h"

#include "html/html.h"
#include "html/private.h"
#include "html/object.h"
#include "html/box.h"
#include "html/box_inspect.h"	/* fixes738: box_coords */
#include "html/box_manipulate.h"
#include "html/box_construct.h"
#include "html/box_special.h"
#include "html/box_textarea.h"
#include "html/form_internal.h"
#include "html/macsurf_dom_compat.h"


static const content_type image_types = CONTENT_IMAGE;


/**
 * determine if a box is the root node
 *
 * \param n node to check
 * \return true if node is root else false.
 */
static inline bool box_is_root(dom_node *n)
{
	dom_node *parent;
	dom_node_type type;
	dom_exception err;

	err = dom_node_get_parent_node(n, &parent);
	if (err != DOM_NO_ERR)
		return false;

	if (parent != NULL) {
		err = dom_node_get_node_type(parent, &type);

		dom_node_unref(parent);

		if (err != DOM_NO_ERR)
			return false;

		if (type != DOM_DOCUMENT_NODE)
			return false;
	}

	return true;
}


/**
 * Destructor for object_params, for &lt;object&gt; elements
 *
 * \param o  The object params being destroyed.
 * \return 0 to allow talloc to continue destroying the tree.
 */
static int box_object_talloc_destructor(struct object_params *o)
{
	if (o->codebase != NULL)
		nsurl_unref(o->codebase);
	if (o->classid != NULL)
		nsurl_unref(o->classid);
	if (o->data != NULL)
		nsurl_unref(o->data);

	return 0;
}


/**
 * Parse a multi-length-list, as defined by HTML 4.01.
 *
 * \param ds dom string to parse
 * \param count updated to number of entries
 * \return array of struct box_multi_length, or 0 on memory exhaustion
 */
static struct frame_dimension *
box_parse_multi_lengths(const dom_string *ds, unsigned int *count)
{
	char *end;
	unsigned int i, n;
	struct frame_dimension *length;
	const char *s;

	s = dom_string_data(ds);

	for (i = 0, n = 1; s[i]; i++)
		if (s[i] == ',')
			n++;

	length = calloc(n, sizeof(struct frame_dimension));
	if (!length)
		return NULL;

	for (i = 0; i != n; i++) {
		while (ascii_is_space(*s)) {
			s++;
		}
		length[i].value = strtof(s, &end);
		if (length[i].value <= 0) {
			length[i].value = 1;
		}
		s = end;
		switch (*s) {
			case '%':
				length[i].unit = FRAME_DIMENSION_PERCENT;
				break;
			case '*':
				length[i].unit = FRAME_DIMENSION_RELATIVE;
				break;
			default:
				length[i].unit = FRAME_DIMENSION_PIXELS;
				break;
		}
		while (*s && *s != ',') {
			s++;
		}
		if (*s == ',') {
			s++;
		}
	}

	*count = n;
	return length;
}


/**
 * Destructor for content_html_frames, for frame elements
 *
 * \param f  The frame params being destroyed.
 * \return 0 to allow talloc to continue destroying the tree.
 */
static int box_frames_talloc_destructor(struct content_html_frames *f)
{
	if (f->url != NULL) {
		nsurl_unref(f->url);
		f->url = NULL;
	}

	return 0;
}


/**
 * create a frameset box tree
 */
static bool
box_create_frameset(struct content_html_frames *f,
		    dom_node *n,
		    html_content *content)
{
	unsigned int row, col, index, i;
	unsigned int rows = 1, cols = 1;
	dom_string *s;
	dom_exception err;
	nsurl *url;
	struct frame_dimension *row_height = 0, *col_width = 0;
	dom_node *c, *next;
	struct content_html_frames *frame;
	bool default_border = true;
	colour default_border_colour = 0x000000;

	/* parse rows and columns */
	err = dom_element_get_attribute(n, corestring_dom_rows, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		row_height = box_parse_multi_lengths(s, &rows);
		dom_string_unref(s);
		if (row_height == NULL)
			return false;
	} else {
		row_height = calloc(1, sizeof(struct frame_dimension));
		if (row_height == NULL)
			return false;
		row_height->value = 100;
		row_height->unit = FRAME_DIMENSION_PERCENT;
	}

	err = dom_element_get_attribute(n, corestring_dom_cols, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		col_width = box_parse_multi_lengths(s, &cols);
		dom_string_unref(s);
		if (col_width == NULL) {
			free(row_height);
			return false;
		}
	} else {
		col_width = calloc(1, sizeof(struct frame_dimension));
		if (col_width == NULL) {
			free(row_height);
			return false;
		}
		col_width->value = 100;
		col_width->unit = FRAME_DIMENSION_PERCENT;
	}

	/* common extension: border="0|1" to control all children */
	err = dom_element_get_attribute(n, corestring_dom_border, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		if ((dom_string_data(s)[0] == '0') &&
				(dom_string_data(s)[1] == '\0'))
			default_border = false;
		dom_string_unref(s);
	}

	/* common extension: frameborder="yes|no" to control all children */
	err = dom_element_get_attribute(n, corestring_dom_frameborder, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		if (dom_string_caseless_lwc_isequal(s,
				corestring_lwc_no) == 0)
			default_border = false;
		dom_string_unref(s);
	}

	/* common extension: bordercolor="#RRGGBB|<named colour>" to control
	 *all children */
	err = dom_element_get_attribute(n, corestring_dom_bordercolor, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		css_color color;

		if (nscss_parse_colour(dom_string_data(s), &color))
			default_border_colour = nscss_color_to_ns(color);

		dom_string_unref(s);
	}

	/* update frameset and create default children */
	f->cols = cols;
	f->rows = rows;
	f->scrolling = BW_SCROLLING_NO;
	f->children = talloc_array(content->bctx, struct content_html_frames,
								(rows * cols));

	talloc_set_destructor(f->children, box_frames_talloc_destructor);

	for (row = 0; row < rows; row++) {
		for (col = 0; col < cols; col++) {
			index = (row * cols) + col;
			frame = &f->children[index];
			frame->cols = 0;
			frame->rows = 0;
			frame->width = col_width[col];
			frame->height = row_height[row];
			frame->margin_width = 0;
			frame->margin_height = 0;
			frame->name = NULL;
			frame->url = NULL;
			frame->no_resize = false;
			frame->scrolling = BW_SCROLLING_AUTO;
			frame->border = default_border;
			frame->border_colour = default_border_colour;
			frame->children = NULL;
		}
	}
	free(col_width);
	free(row_height);

	/* create the frameset windows */
	err = dom_node_get_first_child(n, &c);
	if (err != DOM_NO_ERR)
		return false;

	for (row = 0; c != NULL && row < rows; row++) {
		for (col = 0; c != NULL && col < cols; col++) {
			while (c != NULL) {
				dom_node_type type;
				dom_string *name;

				err = dom_node_get_node_type(c, &type);
				if (err != DOM_NO_ERR) {
					dom_node_unref(c);
					return false;
				}

				err = dom_node_get_node_name(c, &name);
				if (err != DOM_NO_ERR) {
					dom_node_unref(c);
					return false;
				}

				if (type != DOM_ELEMENT_NODE ||
					(!dom_string_caseless_lwc_isequal(
							name,
							corestring_lwc_frame) &&
					!dom_string_caseless_lwc_isequal(
							name,
							corestring_lwc_frameset
							))) {
					err = dom_node_get_next_sibling(c,
							&next);
					if (err != DOM_NO_ERR) {
						dom_string_unref(name);
						dom_node_unref(c);
						return false;
					}

					dom_string_unref(name);
					dom_node_unref(c);
					c = next;
				} else {
					/* Got a FRAME or FRAMESET element */
					dom_string_unref(name);
					break;
				}
			}

			if (c == NULL)
				break;

			/* get current frame */
			index = (row * cols) + col;
			frame = &f->children[index];

			/* nest framesets */
			err = dom_node_get_node_name(c, &s);
			if (err != DOM_NO_ERR) {
				dom_node_unref(c);
				return false;
			}

			if (dom_string_caseless_lwc_isequal(s,
					corestring_lwc_frameset)) {
				dom_string_unref(s);
				frame->border = 0;
				if (box_create_frameset(frame, c,
						content) == false) {
					dom_node_unref(c);
					return false;
				}

				err = dom_node_get_next_sibling(c, &next);
				if (err != DOM_NO_ERR) {
					dom_node_unref(c);
					return false;
				}

				dom_node_unref(c);
				c = next;
				continue;
			}

			dom_string_unref(s);

			/* get frame URL (not required) */
			url = NULL;
			err = dom_element_get_attribute(c, corestring_dom_src, &s);
			if (err == DOM_NO_ERR && s != NULL) {
				box_extract_link(content, s, content->base_url,
						 &url);
				dom_string_unref(s);
			}

			/* copy url */
			if (url != NULL) {
				/* no self-references */
				if (nsurl_compare(content->base_url, url,
						NSURL_COMPLETE) == false)
					frame->url = url;
				url = NULL;
			}

			/* fill in specified values */
			err = dom_element_get_attribute(c, corestring_dom_name, &s);
			if (err == DOM_NO_ERR && s != NULL) {
				frame->name = talloc_strdup(content->bctx,
						dom_string_data(s));
				dom_string_unref(s);
			}

			if (dom_element_has_attribute(c, corestring_dom_noresize,
						      &frame->no_resize) != DOM_NO_ERR) {
				/* If we can't read the attribute for some reason,
				 * assume we didn't have it.
				 */
				frame->no_resize = false;
			}

			err = dom_element_get_attribute(c, corestring_dom_frameborder,
					&s);
			if (err == DOM_NO_ERR && s != NULL) {
				i = atoi(dom_string_data(s));
				frame->border = (i != 0);
				dom_string_unref(s);
			}

			err = dom_element_get_attribute(c, corestring_dom_scrolling, &s);
			if (err == DOM_NO_ERR && s != NULL) {
				if (dom_string_caseless_lwc_isequal(s,
						corestring_lwc_yes))
					frame->scrolling = BW_SCROLLING_YES;
				else if (dom_string_caseless_lwc_isequal(s,
						corestring_lwc_no))
					frame->scrolling = BW_SCROLLING_NO;
				dom_string_unref(s);
			}

			err = dom_element_get_attribute(c, corestring_dom_marginwidth,
					&s);
			if (err == DOM_NO_ERR && s != NULL) {
				frame->margin_width = atoi(dom_string_data(s));
				dom_string_unref(s);
			}

			err = dom_element_get_attribute(c, corestring_dom_marginheight,
					&s);
			if (err == DOM_NO_ERR && s != NULL) {
				frame->margin_height = atoi(dom_string_data(s));
				dom_string_unref(s);
			}

			err = dom_element_get_attribute(c, corestring_dom_bordercolor,
					&s);
			if (err == DOM_NO_ERR && s != NULL) {
				css_color color;

				if (nscss_parse_colour(dom_string_data(s),
						&color))
					frame->border_colour =
						nscss_color_to_ns(color);

				dom_string_unref(s);
			}

			/* advance */
			err = dom_node_get_next_sibling(c, &next);
			if (err != DOM_NO_ERR) {
				dom_node_unref(c);
				return false;
			}

			dom_node_unref(c);
			c = next;
		}
	}

	/* If the last child wasn't a frame, we still need to unref it */
	if (c != NULL) {
		dom_node_unref(c);
	}

	return true;
}


/**
 * Destructor for content_html_iframe, for &lt;iframe&gt; elements
 *
 * \param f The iframe params being destroyed.
 * \return 0 to allow talloc to continue destroying the tree.
 */
static int box_iframes_talloc_destructor(struct content_html_iframe *f)
{
	if (f->url != NULL) {
		nsurl_unref(f->url);
		f->url = NULL;
	}

	return 0;
}


/**
 * Get the value of a dom node element's attribute.
 *
 * \param  n	      dom element node
 * \param  attribute  name of attribute
 * \param  context    talloc context for result buffer
 * \param  value      updated to value, if the attribute is present
 * \return  true on success, false if attribute present but memory exhausted
 *
 * \note returning true does not imply that the attribute was found. If the
 * attribute was not found, *value will be unchanged.
 */
static bool
box_get_attribute(dom_node *n,
		  const char *attribute,
		  void *context,
		  char **value)
{
	char *result;
	dom_string *attr, *attr_name;
	dom_exception err;

	err = dom_string_create_interned((const uint8_t *) attribute,
			strlen(attribute), &attr_name);
	if (err != DOM_NO_ERR)
		return false;

	err = dom_element_get_attribute(n, attr_name, &attr);
	if (err != DOM_NO_ERR) {
		dom_string_unref(attr_name);
		return false;
	}

	dom_string_unref(attr_name);

	if (attr != NULL) {
		result = talloc_strdup(context, dom_string_data(attr));

		dom_string_unref(attr);

		if (result == NULL)
			return false;

		*value = result;
	}

	return true;
}


/**
 * Helper function for adding textarea widget to box.
 *
 * This is a load of hacks to ensure boxes replaced with textareas
 * can be handled by the layout code.
 */
static bool
box_input_text(html_content *html, struct box *box, struct dom_node *node)
{
	struct box *inline_container, *inline_box;
	uint8_t display = css_computed_display_static(box->style);

	switch (display) {
	case CSS_DISPLAY_GRID:
	case CSS_DISPLAY_FLEX:
	case CSS_DISPLAY_BLOCK:
		box->type = BOX_BLOCK;
		break;
	default:
		box->type = BOX_INLINE_BLOCK;
		break;
	}

	inline_container = box_create(NULL, 0, false, 0, 0, 0, 0, html->bctx);
	if (!inline_container)
		return false;
	inline_container->type = BOX_INLINE_CONTAINER;
	inline_box = box_create(NULL, box->style, false, 0, 0, box->title, 0,
			html->bctx);
	if (!inline_box)
		return false;
	inline_box->type = BOX_TEXT;
	inline_box->text = talloc_strdup(html->bctx, "");

	box_add_child(inline_container, inline_box);
	box_add_child(box, inline_container);

	if (box_textarea_create_textarea(html, box, node) == false) {
		macsurf_debug_log_writef(
			"WORK form control: textarea widget creation failed"
			" gadget_type=%d box=%p node=%p",
			(int) box->gadget->type, (void *) box, (void *) node);
		return false;
	}

	return true;
}


/**
 * Add an option to a form select control (helper function for box_select()).
 *
 * \param  control  select containing the &lt;option&gt;
 * \param  n	    xml element node for &lt;option&gt;
 * \return  true on success, false on memory exhaustion
 */
static bool box_select_add_option(struct form_control *control, dom_node *n)
{
	char *value = NULL;
	char *text = NULL;
	char *text_nowrap = NULL;
	bool selected;
	dom_string *content, *s;
	dom_exception err;

	err = dom_node_get_text_content(n, &content);
	if (err != DOM_NO_ERR)
		return false;

	if (content != NULL) {
		text = squash_whitespace(dom_string_data(content));
		dom_string_unref(content);
	} else {
		text = strdup("");
	}

	if (text == NULL)
		goto no_memory;

	err = dom_element_get_attribute(n, corestring_dom_value, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		value = strdup(dom_string_data(s));
		dom_string_unref(s);
	} else {
		value = strdup(text);
	}

	if (value == NULL)
		goto no_memory;

	if (dom_element_has_attribute(n, corestring_dom_selected, &selected) != DOM_NO_ERR) {
		/* Assume not selected if we can't read the attribute presence */
		selected = false;
	}

	/* replace spaces/TABs with hard spaces to prevent line wrapping */
	text_nowrap = cnv_space2nbsp(text);
	if (text_nowrap == NULL)
		goto no_memory;

	if (form_add_option(control, value, text_nowrap, selected, n) == false)
		goto no_memory;

	free(text);

	return true;

no_memory:
	free(value);
	free(text);
	free(text_nowrap);
	return false;
}


/**
 * \name  Special case element handlers
 *
 * These functions are called by box_construct_element() when an element is
 * being converted, according to the entries in element_table.
 *
 * The parameters are the xmlNode, the content for the document, and a partly
 * filled in box structure for the element.
 *
 * Return true on success, false on memory exhaustion. Set *convert_children
 * to false if children of this element in the XML tree should be skipped (for
 * example, if they have been processed in some special way already).
 *
 * Elements ordered as in the HTML 4.01 specification. Section numbers in
 * brackets [] refer to the spec.
 *
 * \{
 */

/**
 * special element handler for Anchor [12.2].
 */
static bool
box_a(dom_node *n,
      html_content *content,
      struct box *box,
      bool *convert_children)
{
	bool ok;
	nsurl *url;
	dom_string *s;
	dom_exception err;

	err = dom_element_get_attribute(n, corestring_dom_href, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		ok = box_extract_link(content, s, content->base_url, &url);
		dom_string_unref(s);
		if (!ok)
			return false;
		if (url) {
			if (box->href != NULL)
				nsurl_unref(box->href);
			box->href = url;
		}
	}

	/* name and id share the same namespace */
	err = dom_element_get_attribute(n, corestring_dom_name, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		lwc_string *lwc_name;

		err = dom_string_intern(s, &lwc_name);

		dom_string_unref(s);

		if (err == DOM_NO_ERR) {
			/* name replaces existing id
			 * TODO: really? */
			lwc_string_unref(box->id);
			box->id = lwc_name;
		}
	}

	/* fixes1063 (#114) - HTML5 `download`: save the target rather than
	 * navigate to it. Recorded as a box flag here, beside href/target,
	 * because it has to reach the box the user actually CLICKS.
	 *
	 * Reading it from the DOM at click time does not work, and the box tree
	 * is why: for `<a>text</a>` the anchor's own BOX_INLINE is zero-width
	 * (so a click never lands on it) and the BOX_TEXT that IS clicked
	 * carries href but node == NULL. There is no DOM node to walk up from,
	 * and box->parent leads to the anonymous BOX_INLINE_CONTAINER, not the
	 * anchor -- text and the <a> are SIBLINGS in the box tree. An <img>
	 * inside the link works either way, which is exactly why fixes1060 and
	 * fixes1061 both looked right and both failed on plain text links.
	 *
	 * Carried down with href by box_construct_props, so every box that
	 * inherits the link inherits this too. */
	{
		dom_string *dl = NULL;
		err = dom_element_get_attribute(n, corestring_dom_download, &dl);
		if (err == DOM_NO_ERR && dl != NULL) {
			box->flags |= LINK_DOWNLOAD;
			dom_string_unref(dl);
		}
	}

	/* target frame [16.3] */
	err = dom_element_get_attribute(n, corestring_dom_target, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		if (dom_string_caseless_lwc_isequal(s,
				corestring_lwc__blank))
			box->target = "_blank";
		else if (dom_string_caseless_lwc_isequal(s,
				corestring_lwc__top))
			box->target = "_top";
		else if (dom_string_caseless_lwc_isequal(s,
				corestring_lwc__parent))
			box->target = "_parent";
		else if (dom_string_caseless_lwc_isequal(s,
				corestring_lwc__self))
			/* the default may have been overridden by a
			 * <base target=...>, so this is different to 0 */
			box->target = "_self";
		else {
			/* 6.16 says that frame names must begin with [a-zA-Z]
			 * This doesn't match reality, so just take anything */
			box->target = talloc_strdup(content->bctx,
					dom_string_data(s));
			if (!box->target) {
				dom_string_unref(s);
				return false;
			}
		}
		dom_string_unref(s);
	}

	return true;
}


/**
 * Document body special element handler [7.5.1].
 */
static bool
box_body(dom_node *n,
	 html_content *content,
	 struct box *box,
	 bool *convert_children)
{
	css_color color;

	/* Canvas (viewport) background per CSS 2.1 §14.2 background
	 * propagation: the ROOT element's (<html>) background wins for the
	 * page canvas; the <body> background propagates only when the root
	 * has none. box_html() runs first (DOM order) and already captured a
	 * non-transparent root background into content->background_colour, so
	 * only fall back to <body> here when the root left it transparent.
	 *
	 * Upstream NetSurf read <body> unconditionally and so cleared the
	 * canvas to the default white on pages that set the page colour on
	 * <html> (e.g. tinkerdifferent's `html{background:#2d3238}` with a
	 * transparent <body>) - the whole page rendered light instead of
	 * dark. The box-paint path already honours this precedence in
	 * html_redraw_find_bg_box; this mirrors it for the canvas clear. */
	if (content->background_colour != NS_TRANSPARENT) {
		return true;
	}

	css_computed_background_color(box->style, &color);
	if (nscss_color_is_transparent(color)) {
		content->background_colour = NS_TRANSPARENT;
	} else {
		content->background_colour = nscss_color_to_ns(color);
	}

	return true;
}


/**
 * Document root element handler [<html>].
 *
 * Capture the root background for the page canvas (CSS 2.1 §14.2 - the
 * root element's background propagates to the viewport and wins over the
 * <body> background). Runs before box_body in DOM order.
 */
static bool
box_html(dom_node *n,
	 html_content *content,
	 struct box *box,
	 bool *convert_children)
{
	css_color color;

	/* fixes628: base/canvas background - capture the FIRST non-transparent
	 * root <html> background and keep it; a later re-convert cannot revert
	 * it. On tinkerdifferent MacSurf re-converts the same content and the
	 * SECOND convert reads the root background as the light UA/system-
	 * default #EBEBEB instead of the author html{background:#2d3238} (a
	 * MacSurf re-convert cascade quirk on the ROOT element only -- the
	 * cards and stock NetSurf and the FIRST MacSurf convert all read
	 * #2d3238 correctly). Overwriting the good dark value with that light
	 * grey made the whole page base paint near-white while the cards
	 * stayed dark. Mirrors box_body's existing first-wins guard;
	 * content->background_colour persists across converts (only reset at
	 * content creation, html.c), so the correct value sticks. */
	if (content->background_colour != NS_TRANSPARENT) {
		return true;
	}

	if (box->style != NULL) {
		css_computed_background_color(box->style, &color);
		if (nscss_color_is_transparent(color) == false) {
			content->background_colour = nscss_color_to_ns(color);
			macsurf_debug_log_writef(
				"box_html: base bg captured %p",
				(void *)(unsigned long) color);
		}
	}

	return true;
}


/**
 * special element handler for forced line break [9.3.2].
 */
static bool
box_br(dom_node *n,
       html_content *content,
       struct box *box,
       bool *convert_children)
{
	box->type = BOX_BR;
	return true;
}


/**
 * special element handler for Push button [17.5].
 */
static bool
box_button(dom_node *n,
	   html_content *content,
	   struct box *box,
	   bool *convert_children)
{
	struct form_control *gadget;

	gadget = html_forms_get_control_for_node(content->forms, n);
	if (!gadget)
		return false;

	gadget->html = content;
	box->gadget = gadget;
	box->flags |= IS_REPLACED;
	gadget->box = box;

	box->type = BOX_INLINE_BLOCK;

	/* Just render the contents */

	return true;
}


/**
 * Canvas element
 */
static bool
box_canvas(dom_node *n,
	     html_content *content,
	     struct box *box,
	     bool *convert_children)
{
	/* If scripting is not enabled display the contents of canvas */
	if (!content->enable_scripting) {
		return true;
	}
	*convert_children = false;

	if (box->style && ns_computed_display(box->style,
			box_is_root(n)) == CSS_DISPLAY_NONE)
		return true;

	/* This is replaced content */
	box->flags |= IS_REPLACED | REPLACE_DIM;

	return true;
}


/**
 * Embedded object (not in any HTML specification:
 * see http://wp.netscape.com/assist/net_sites/new_html3_prop.html )
 */
static bool
box_embed(dom_node *n,
	  html_content *content,
	  struct box *box,
	  bool *convert_children)
{
	struct object_params *params;
	struct object_param *param;
	dom_namednodemap *attrs;
	unsigned long idx;
	uint32_t num_attrs;
	dom_string *src;
	dom_exception err;

	if (box->style && ns_computed_display(box->style,
			box_is_root(n)) == CSS_DISPLAY_NONE)
		return true;

	params = talloc(content->bctx, struct object_params);
	if (params == NULL)
		return false;

	talloc_set_destructor(params, box_object_talloc_destructor);

	params->data = NULL;
	params->type = NULL;
	params->codetype = NULL;
	params->codebase = NULL;
	params->classid = NULL;
	params->params = NULL;

	/* src is a URL */
	err = dom_element_get_attribute(n, corestring_dom_src, &src);
	if (err != DOM_NO_ERR || src == NULL)
		return true;
	if (box_extract_link(content, src, content->base_url,
			     &params->data) == false) {
		dom_string_unref(src);
		return false;
	}

	dom_string_unref(src);

	if (params->data == NULL)
		return true;

	/* Don't include ourself */
	if (nsurl_compare(content->base_url, params->data, NSURL_COMPLETE))
		return true;

	/* add attributes as parameters to linked list */
	err = dom_node_get_attributes(n, &attrs);
	if (err != DOM_NO_ERR)
		return false;

	err = dom_namednodemap_get_length(attrs, &num_attrs);
	if (err != DOM_NO_ERR) {
		dom_namednodemap_unref(attrs);
		return false;
	}

	for (idx = 0; idx < num_attrs; idx++) {
		dom_attr *attr;
		dom_string *name, *value;

		err = dom_namednodemap_item(attrs, idx, (void *) &attr);
		if (err != DOM_NO_ERR) {
			dom_namednodemap_unref(attrs);
			return false;
		}

		err = dom_attr_get_name(attr, &name);
		if (err != DOM_NO_ERR) {
			dom_node_unref(attr);
			dom_namednodemap_unref(attrs);
			return false;
		}

		if (dom_string_caseless_lwc_isequal(name, corestring_lwc_src)) {
			dom_node_unref(attr);
			dom_string_unref(name);
			continue;
		}

		err = dom_attr_get_value(attr, &value);
		if (err != DOM_NO_ERR) {
			dom_node_unref(attr);
			dom_string_unref(name);
			dom_namednodemap_unref(attrs);
			return false;
		}

		param = talloc(content->bctx, struct object_param);
		if (param == NULL) {
			dom_node_unref(attr);
			dom_string_unref(value);
			dom_string_unref(name);
			dom_namednodemap_unref(attrs);
			return false;
		}

		param->name = talloc_strdup(content->bctx, dom_string_data(name));
		param->value = talloc_strdup(content->bctx, dom_string_data(value));
		param->type = NULL;
		param->valuetype = talloc_strdup(content->bctx, "data");
		param->next = NULL;

		dom_string_unref(value);
		dom_string_unref(name);
		dom_node_unref(attr);

		if (param->name == NULL || param->value == NULL ||
				param->valuetype == NULL) {
			dom_namednodemap_unref(attrs);
			return false;
		}

		param->next = params->params;
		params->params = param;
	}

	dom_namednodemap_unref(attrs);

	box->object_params = params;

	/* start fetch */
	box->flags |= IS_REPLACED;
	return html_fetch_object(content, params->data, box, CONTENT_ANY, false);
}


/**
 * Window subdivision [16.2.1].
 */
static bool
box_frameset(dom_node *n,
	     html_content *content,
	     struct box *box,
	     bool *convert_children)
{
	bool ok;

	if (content->frameset) {
		NSLOG(netsurf, INFO, "Error: multiple framesets in document.");
		/* Don't convert children */
		if (convert_children)
			*convert_children = false;
		/* And ignore this spurious frameset */
		box->type = BOX_NONE;
		return true;
	}

	content->frameset = talloc_zero(content->bctx,
					struct content_html_frames);
	if (!content->frameset) {
		return false;
	}

	ok = box_create_frameset(content->frameset, n, content);
	if (ok) {
		box->type = BOX_NONE;
	}

	if (convert_children) {
		*convert_children = false;
	}
	return ok;
}


/**
 * Inline subwindow [16.5].
 */
static bool
box_iframe(dom_node *n,
	   html_content *content,
	   struct box *box,
	   bool *convert_children)
{
	nsurl *url;
	dom_string *s;
	dom_exception err;
	struct content_html_iframe *iframe;
	int i;

	if (box->style && ns_computed_display(box->style,
			box_is_root(n)) == CSS_DISPLAY_NONE)
		return true;

	if (box->style &&
	    css_computed_visibility(box->style) == CSS_VISIBILITY_HIDDEN) {
		/* Don't create iframe discriptors for invisible iframes
		 * TODO: handle hidden iframes at browser_window generation
		 * time instead? */
		return true;
	}

	/* get frame URL */
	err = dom_element_get_attribute(n, corestring_dom_src, &s);
	if (err != DOM_NO_ERR || s == NULL)
		return true;
	if (box_extract_link(content, s, content->base_url, &url) == false) {
		dom_string_unref(s);
		return false;
	}
	dom_string_unref(s);
	if (url == NULL)
		return true;

	/* don't include ourself */
	if (nsurl_compare(content->base_url, url, NSURL_COMPLETE)) {
		nsurl_unref(url);
		return true;
	}

	/* fixes451: skip video-embed domains MacSurf cannot render.
	 * These iframes waste connections and delay page conversion. */
	{
		lwc_string *host;
		const char *hd;
		int hl;
		host = nsurl_get_component(url, NSURL_HOST);
		if (host != NULL) {
			hd = lwc_string_data(host);
			hl = (int)lwc_string_length(host);
			if ((hl == 11 && strncmp(hd, "youtube.com", 11) == 0) ||
			    (hl > 12 && strncmp(hd + hl - 12, ".youtube.com", 12) == 0) ||
			    (hl == 8  && strncmp(hd, "youtu.be",  8)  == 0) ||
			    (hl == 9  && strncmp(hd, "vimeo.com", 9)  == 0) ||
			    (hl > 10 && strncmp(hd + hl - 10, ".vimeo.com", 10) == 0)) {
				lwc_string_unref(host);
				nsurl_unref(url);
				return true;
			}
			lwc_string_unref(host);
		}
	}

	/* create a new iframe */
	iframe = talloc(content->bctx, struct content_html_iframe);
	if (iframe == NULL) {
		nsurl_unref(url);
		return false;
	}

	talloc_set_destructor(iframe, box_iframes_talloc_destructor);

	iframe->box = box;
	iframe->margin_width = 0;
	iframe->margin_height = 0;
	iframe->name = NULL;
	iframe->url = url;
	iframe->scrolling = BW_SCROLLING_AUTO;
	iframe->border = true;

	/* Add this iframe to the linked list of iframes */
	iframe->next = content->iframe;
	content->iframe = iframe;

	/* fill in specified values */
	err = dom_element_get_attribute(n, corestring_dom_name, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		iframe->name = talloc_strdup(content->bctx, dom_string_data(s));
		dom_string_unref(s);
	}

	err = dom_element_get_attribute(n, corestring_dom_frameborder, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		i = atoi(dom_string_data(s));
		iframe->border = (i != 0);
		dom_string_unref(s);
	}

	err = dom_element_get_attribute(n, corestring_dom_bordercolor, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		css_color color;

		if (nscss_parse_colour(dom_string_data(s), &color))
			iframe->border_colour = nscss_color_to_ns(color);

		dom_string_unref(s);
	}

	err = dom_element_get_attribute(n, corestring_dom_scrolling, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		if (dom_string_caseless_lwc_isequal(s,
				corestring_lwc_yes))
			iframe->scrolling = BW_SCROLLING_YES;
		else if (dom_string_caseless_lwc_isequal(s,
				corestring_lwc_no))
			iframe->scrolling = BW_SCROLLING_NO;
		dom_string_unref(s);
	}

	err = dom_element_get_attribute(n, corestring_dom_marginwidth, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		iframe->margin_width = atoi(dom_string_data(s));
		dom_string_unref(s);
	}

	err = dom_element_get_attribute(n, corestring_dom_marginheight, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		iframe->margin_height = atoi(dom_string_data(s));
		dom_string_unref(s);
	}

	/* box */
	assert(box->style);
	box->flags |= IFRAME;
	box->flags |= IS_REPLACED;

	/* Showing iframe, so don't show alternate content */
	if (convert_children)
		*convert_children = false;
	return true;
}


/* fixes168b - Lazy-image source resolver. Modern sites routinely ship
 * <img src="placeholder"> or <img src=""> and store the real URL on
 * data-src / data-original / data-lazy-src / srcset, expecting JS to
 * promote it at runtime. MacSurf has no such JS, so we promote those
 * URLs at box-construct time.
 *
 * Returns true if `*out` is a non-NULL char* talloc'd in content->bctx
 * containing a usable URL string. Returns true with *out=NULL when no
 * candidate exists (caller falls through to "no image"). Returns false
 * only on hard allocation failure. */

static bool
box_image_src_looks_placeholder(const char *s)
{
	size_t n;
	const char *p;

	if (s == NULL) return true;
	if (s[0] == '\0') return true;
	/* data: URIs under ~256 bytes are usually 1x1 transparent
	 * spacers; longer ones may be real inline images, which we
	 * don't decode either way, but treat as "real" and fall
	 * through to normal handling. */
	if (s[0] == 'd' && s[1] == 'a' && s[2] == 't' && s[3] == 'a' &&
	    s[4] == ':') {
		n = strlen(s);
		if (n < 256) return true;
		return false;
	}
	/* "#" is sometimes used as a placeholder src; same for "about:blank". */
	if (s[0] == '#' && s[1] == '\0') return true;
	if (strcmp(s, "about:blank") == 0) return true;
	/* Common lazy-load placeholder filenames. */
	for (p = s; *p != '\0'; p++) {
		if (p[0] == '/' || p == s) {
			const char *q = (p == s) ? p : p + 1;
			if (strncmp(q, "spacer.", 7) == 0) return true;
			if (strncmp(q, "blank.", 6) == 0) return true;
			if (strncmp(q, "placeholder.", 12) == 0)
				return true;
			if (strncmp(q, "1x1.", 4) == 0) return true;
			if (strncmp(q, "pixel.", 6) == 0) return true;
		}
	}
	return false;
}

/* Extract the first URL token from an srcset value. srcset syntax is
 *   "url1 480w, url2 800w" or "url1 1x, url2 2x"
 * Picking the first URL is good enough for survival rendering; we are
 * not going to honour the descriptor anyway.
 *
 * Writes a freshly-talloc'd copy of the first URL into *out. Returns
 * true even when no URL was found (then *out == NULL). False only on
 * allocation failure. */
static bool
box_image_pick_srcset(const char *srcset, void *talloc_ctx, char **out)
{
	const char *p;
	const char *url_start;
	size_t url_len;
	char *copy;

	*out = NULL;
	if (srcset == NULL) return true;

	p = srcset;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
	       *p == ',')
		p++;
	if (*p == '\0') return true;

	url_start = p;
	while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' &&
	       *p != '\r' && *p != ',')
		p++;
	url_len = (size_t)(p - url_start);
	if (url_len == 0) return true;

	copy = talloc_size(talloc_ctx, url_len + 1);
	if (copy == NULL) return false;
	memcpy(copy, url_start, url_len);
	copy[url_len] = '\0';
	*out = copy;
	return true;
}

/* Is `type` (a <source type="..."> MIME string) something MacSurf can
 * actually decode? NULL/empty is "no declared type" and returns true -
 * per the HTML picture-source algorithm, a <source> with no type is NOT
 * skipped; a real browser attempts to load it. A DECLARED type is checked
 * against the content factory's actual registered handlers (the same
 * authority box_special.c's own <object>/<embed> codetype/type gating
 * already asks, above) rather than a hardcoded MIME list or a file-
 * extension guess, so this stays correct automatically as decoders are
 * added or removed - no per-format special case to maintain here. */
static bool
box_image_type_supported(const char *type)
{
	const char *p;
	const char *end;
	lwc_string *itype;
	lwc_error lerror;
	bool supported;

	if (type == NULL)
		return true;
	p = type;
	while (ascii_is_space(*p))
		p++;
	if (*p == '\0')
		return true;
	end = p;
	while (*end != '\0' && *end != ';' && !ascii_is_space(*end))
		end++;

	lerror = lwc_intern_string(p, (size_t)(end - p), &itype);
	if (lerror != lwc_error_ok)
		return true; /* fail open: don't block a source on an alloc hiccup */

	supported = (content_factory_type_from_mime_type(itype) != CONTENT_NONE);
	lwc_string_unref(itype);
	return supported;
}

/* The HTML parser knows about <picture>/<source>, but the old special-element
 * path only ever looked at the IMG's own attributes.  That silently selected
 * the JPEG/PNG fallback on every responsive-image site, despite MacSurf now
 * having an image/webp handler.  Keep the check deliberately narrow: select
 * only an explicitly declared image/webp source and otherwise retain the IMG
 * fallback exactly as before. */
static bool
box_image_text_is(const char *value, const char *wanted)
{
	char a;
	char b;

	if (value == NULL || wanted == NULL)
		return false;
	while (*wanted != '\0') {
		a = *value++;
		b = *wanted++;
		if (a == '\0')
			return false;
		if (a >= 'A' && a <= 'Z')
			a = (char)(a + ('a' - 'A'));
		if (b >= 'A' && b <= 'Z')
			b = (char)(b + ('a' - 'A'));
		if (a != b)
			return false;
	}
	return (*value == '\0');
}

static bool
box_image_node_is(dom_node *node, const char *tag)
{
	dom_string *name = NULL;
	dom_exception err;
	bool result;

	err = dom_element_get_tag_name((dom_element *)node, &name);
	if (err != DOM_NO_ERR || name == NULL)
		return false;
	result = box_image_text_is(dom_string_data(name), tag);
	dom_string_unref(name);
	return result;
}

/* fixes1318: proper <picture>/<source> selection, replacing the old
 * "only an explicit type=image/webp source, else fall through to IMG"
 * special case. Per the HTML picture-source algorithm: walk SOURCE
 * siblings in document order and use the first one MacSurf can actually
 * decode - a source with NO type attribute is not skipped (a real
 * browser attempts to load it), and a DECLARED type is accepted whenever
 * box_image_type_supported() says the content factory has a handler for
 * it (JPEG/PNG/GIF/BMP/TIFF/ICO/SVG/WebP today), not just image/webp
 * specifically. A source whose declared type names a format MacSurf
 * genuinely cannot decode (e.g. image/avif) is skipped, same as before. */
static bool
box_image_resolve_picture_url(dom_node *img, html_content *content,
		nsurl **out_url)
{
	dom_node *parent = NULL;
	dom_node *child = NULL;
	dom_node *next = NULL;
	dom_string *candidate_string = NULL;
	dom_exception err;
	char *type = NULL;
	char *raw = NULL;
	char *candidate = NULL;
	bool ok = true;

	*out_url = NULL;
	err = dom_node_get_parent_node(img, &parent);
	if (err != DOM_NO_ERR || parent == NULL)
		return true;
	if (!box_image_node_is(parent, "picture"))
		goto done;

	err = dom_node_get_first_child(parent, &child);
	if (err != DOM_NO_ERR)
		goto done;

	while (child != NULL) {
		if (child == img)
			break;
		if (box_image_node_is(child, "source")) {
			type = NULL;
			if (!box_get_attribute(child, "type", content->bctx,
					&type)) {
				ok = false;
				goto done;
			}
			if (box_image_type_supported(type)) {
				raw = NULL;
				if (!box_get_attribute(child, "srcset", content->bctx,
						&raw)) {
					ok = false;
					goto done;
				}
				if (raw == NULL || raw[0] == '\0') {
					raw = NULL;
					if (!box_get_attribute(child, "src", content->bctx,
							&raw)) {
						ok = false;
						goto done;
					}
				}
				if (raw != NULL && raw[0] != '\0') {
					if (!box_image_pick_srcset(raw, content->bctx,
							&candidate)) {
						ok = false;
						goto done;
					}
					if (candidate == NULL)
						candidate = raw;
					if (candidate[0] != '\0') {
						err = dom_string_create_interned(
							(const uint8_t *)candidate,
							strlen(candidate), &candidate_string);
						if (err != DOM_NO_ERR ||
								candidate_string == NULL)
							goto done;
						ok = box_extract_link(content, candidate_string,
								content->base_url, out_url);
						/* fixes1319: per-source selection trace removed -
						 * object.c's macsurf_log_final_image() reports
						 * every loaded image from one place. */
						goto done;
					}
				}
			}
		}
		err = dom_node_get_next_sibling(child, &next);
		if (err != DOM_NO_ERR) {
			ok = false;
			goto done;
		}
		dom_node_unref(child);
		child = next;
		next = NULL;
	}

done:
	if (candidate_string != NULL)
		dom_string_unref(candidate_string);
	if (child != NULL)
		dom_node_unref(child);
	if (next != NULL)
		dom_node_unref(next);
	if (parent != NULL)
		dom_node_unref(parent);
	return ok;
}

/* Choose the best image URL for the given <img>/<source>. srcset (falling
 * back to data-srcset) is the PREFERRED source whenever src is a real,
 * usable pointer, not just a rescue for a missing one - fixes1318 (see
 * box_image_resolve_url's history: fixes168b wrote src-wins-if-usable back
 * when MacSurf's decoder only covered JPEG/PNG/GIF/BMP, so treating srcset
 * as a last resort was a deliberate "stick to what we can decode" survival
 * policy. Now that box_image_type_supported() (used by the <picture> path
 * above) reflects whatever the content factory actually has registered,
 * there is no format reason left to prefer src; this matches how srcset
 * behaves in real browsers regardless. box_image_pick_srcset only ever
 * takes the first candidate (no width/density matching, no format
 * filtering) - deliberately no .webp/format string-matching here, the
 * fetch+mimesniff pipeline decides what a candidate actually is once
 * fetched, same as it already does for a plain src.
 *
 * data-src / data-original / data-lazy-src are a SEPARATE concern (lazy-
 * load placeholder promotion, fixes168b) and keep their own precedence:
 * only consulted when src itself is missing/empty/a placeholder. */
static bool
box_image_resolve_url(dom_node *n, html_content *content, nsurl **out_url)
{
	dom_string *s = NULL;
	dom_exception err;
	char *src_str = NULL;
	char *fallback = NULL;
	char *srcset_url = NULL;
	bool ok;

	*out_url = NULL;
	if (!box_image_resolve_picture_url(n, content, out_url))
		return false;
	if (*out_url != NULL)
		return true;

	err = dom_element_get_attribute(n, corestring_dom_src, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		const char *raw = dom_string_data(s);
		if (!box_image_src_looks_placeholder(raw)) {
			char *srcset_raw = NULL;

			if (!box_get_attribute(n, "data-srcset",
					content->bctx, &srcset_raw)) {
				dom_string_unref(s);
				return false;
			}
			if (srcset_raw == NULL || srcset_raw[0] == '\0') {
				srcset_raw = NULL;
				if (!box_get_attribute(n, "srcset",
						content->bctx, &srcset_raw)) {
					dom_string_unref(s);
					return false;
				}
			}
			if (srcset_raw != NULL && srcset_raw[0] != '\0') {
				if (!box_image_pick_srcset(srcset_raw,
						content->bctx, &srcset_url)) {
					dom_string_unref(s);
					return false;
				}
			}
			if (srcset_url != NULL) {
				dom_string *promoted = NULL;
				dom_exception err2;
				dom_string_unref(s);
				err2 = dom_string_create_interned(
						(const uint8_t *)srcset_url,
						strlen(srcset_url), &promoted);
				if (err2 != DOM_NO_ERR || promoted == NULL)
					return true;
				ok = box_extract_link(content, promoted,
						content->base_url, out_url);
				dom_string_unref(promoted);
				return ok;
			}
			ok = box_extract_link(content, s,
					content->base_url, out_url);
			dom_string_unref(s);
			return ok;
		}
		dom_string_unref(s);
		s = NULL;
	}

	/* src missing / empty / placeholder - try data-* attributes. */
	if (!box_get_attribute(n, "data-src", content->bctx, &fallback))
		return false;
	if (fallback == NULL || fallback[0] == '\0' ||
	    box_image_src_looks_placeholder(fallback)) {
		fallback = NULL;
		if (!box_get_attribute(n, "data-original",
				content->bctx, &fallback))
			return false;
	}
	if (fallback == NULL || fallback[0] == '\0' ||
	    box_image_src_looks_placeholder(fallback)) {
		fallback = NULL;
		if (!box_get_attribute(n, "data-lazy-src",
				content->bctx, &fallback))
			return false;
	}

	if (fallback == NULL || fallback[0] == '\0') {
		/* Try data-srcset, then srcset. */
		char *srcset_raw = NULL;
		if (!box_get_attribute(n, "data-srcset",
				content->bctx, &srcset_raw))
			return false;
		if (srcset_raw == NULL || srcset_raw[0] == '\0') {
			srcset_raw = NULL;
			if (!box_get_attribute(n, "srcset",
					content->bctx, &srcset_raw))
				return false;
		}
		if (srcset_raw != NULL && srcset_raw[0] != '\0') {
			if (!box_image_pick_srcset(srcset_raw,
					content->bctx, &srcset_url))
				return false;
			fallback = srcset_url;
		}
	}

	if (fallback == NULL || fallback[0] == '\0' ||
	    box_image_src_looks_placeholder(fallback))
		return true; /* no candidate; *out_url stays NULL */

	src_str = fallback;
	{
		dom_string *promoted = NULL;
		err = dom_string_create_interned(
				(const uint8_t *)src_str,
				strlen(src_str), &promoted);
		if (err != DOM_NO_ERR || promoted == NULL)
			return true;
		ok = box_extract_link(content, promoted,
				content->base_url, out_url);
		dom_string_unref(promoted);
		return ok;
	}
}


/* fixes738 (perf): viewport-gated deferred image loading - re-adds & EXTENDS the
 * reverted fixes673-674c. Originally only loading="lazy" images were deferred;
 * now ALL <img> objects are queued at box-construct time and fetched only once
 * their box is within (viewport + margin), driven from the frontend paint path
 * (macsurf_lazyimg_viewport_changed, after each browser_window_redraw). On a big
 * forum page (68kmla: 6.7MB + 1.6MB inline attachments, dozens of below-fold
 * avatars) this stops the off-screen images from blocking the load - the page
 * reaches DONE on text, images stream in on paint/scroll, never-seen images are
 * never fetched.
 *
 * CRASH SAFETY (the prior version was reverted for a G3 crash - treat with care):
 *  - Hold the DOM node (refcounted, stable), NOT a raw box* (fixes674b: box* went
 *    stale via normalisation → box_coords faulted). Re-resolve via box_for_node
 *    every paint.
 *  - Only walk the box tree when the content is CONTENT_STATUS_DONE (fixes674c:
 *    a paint mid-construction walked a renormalising parent chain → UAF).
 *  - Registry-guarded liveness (macos9_content_is_live) so a navigation before a
 *    fetch cannot deref freed content (is_live is pointer-membership only, no
 *    deref). Dead entries are dropped on the next paint.
 *  - RECON RX-style logging so if it DOES still crash on hardware, the last
 *    "RECON LAZY ..." line names the node/box/coords it faulted on. */
extern int macos9_content_is_live(struct content *c);

struct lazyimg_entry {
	html_content        *content;
	struct dom_node     *node;
	nsurl               *url;
	struct lazyimg_entry *next;
};

/*
 * fixes929 - URL -> intrinsic pixel size memo.
 *
 * THE PROBLEM. content_get_width(box->object) was the only source of an
 * image's natural size anywhere in the engine, so a box whose object was not
 * linked *right now* had no size at all: lh__box_is_object does not test
 * IS_REPLACED, so the <img> stopped being a replaced element and layout sized
 * it as inline text -- height = line_height, width = the measured alt string.
 * That is the squish. Three ordinary situations produce it:
 *
 *   1. Lazy loading. box_image NEVER fetches; it always defers to the queue
 *      below, which drains only from a paint. So at first layout EVERY image
 *      on the page has box->object == NULL.
 *   2. A reconvert. It rebuilds the box tree and its relink pass retires any
 *      object it cannot re-key, releasing the handle; the reformat then runs
 *      on the SAME stack, before anything can be re-fetched.
 *   3. A revisit. hlcache_clean destroys an image content the moment its last
 *      user goes away, and llcache_handle_drop_source_data has already freed
 *      the bytes, so nothing anywhere remembers the size -- images are not
 *      disk-cached either. The second visit starts from zero knowledge.
 *
 * A window resize "fixed" it only by forcing invalidate -> paint -> lazy drain
 * -> re-fetch -> completion -> reflow, i.e. by walking the whole chain again.
 *
 * THE FIX. Remember the size against the URL and hand it to the box at
 * construct time, so the box knows how big the image will be BEFORE the fetch
 * starts. That is what width/height attributes do, what REPLACE_DIM already
 * does when an author supplies them, and what every real browser does with its
 * image cache. It makes the squish unreachable regardless of fetch timing
 * instead of racing it.
 *
 * Direct-mapped, fixed 512 entries, no allocation and no eviction policy --
 * this must be cheap on a G3 and must never fail. A hash collision costs a
 * wrong FIRST size that the real object corrects on arrival, so exactness is
 * not required; a stored length check makes collisions rarer still.
 */
#define IMGDIMS_SLOTS 512

struct imgdims_slot {
	unsigned long hash;
	unsigned long len;
	int w;
	int h;
};

static struct imgdims_slot g_imgdims[IMGDIMS_SLOTS];

/* fixes930 - fixes929 shipped with NO observability, so a "still squished"
 * report could not distinguish (a) the memo never filling, (b) box_image never
 * consulting it, (c) it working and something downstream overriding the size.
 * Three counters, read once per layout. No per-image logging: that is the
 * fixes911 mistake (a FlushVol per entry) which had to be undone twice. */
static int g_imgdims_stored = 0;
static int g_imgdims_hit = 0;
static int g_imgdims_miss = 0;

/* fixes934 - LIFE img construct census. How does each <img> enter the fetch
 * system: eager (loads with the document) vs lazy (deferred to the paint-gated
 * queue), and how many arrive already sized from CSS (REPLACE_DIM). Plus the
 * lazy-drain "already had an object, dropped without fetching" counter, the
 * highest-signal dedupe-vs-reconvert probe. Session-cumulative; emitted once
 * per reformat from html_reformat via macsurf_img_ctor_stats, only when moved.
 * Coarse by design (the fixes911 per-image-FlushVol lesson). */
static long g_img_ctor_total = 0;
static long g_img_ctor_eager = 0;
static long g_img_ctor_lazy = 0;
static long g_img_ctor_replacedim = 0;
static long g_img_lazy_dropobj = 0; /* drain drop: box->object already set */

void macsurf_img_ctor_stats(long *total, long *eager, long *lazy,
		long *rdim, long *dropobj);
void macsurf_img_ctor_stats(long *total, long *eager, long *lazy,
		long *rdim, long *dropobj)
{
	*total = g_img_ctor_total;
	*eager = g_img_ctor_eager;
	*lazy = g_img_ctor_lazy;
	*rdim = g_img_ctor_replacedim;
	*dropobj = g_img_lazy_dropobj;
}

void macsurf_imgdims_stats(int *stored, int *hit, int *miss);
void macsurf_imgdims_stats(int *stored, int *hit, int *miss)
{
	*stored = g_imgdims_stored;
	*hit = g_imgdims_hit;
	*miss = g_imgdims_miss;
}

static unsigned long imgdims_hash(const char *sp, unsigned long *len_out)
{
	unsigned long h = 2166136261UL;	/* FNV-1a */
	unsigned long n = 0;

	while (sp[n] != '\0') {
		h ^= (unsigned long) (unsigned char) sp[n];
		h *= 16777619UL;
		n++;
	}
	*len_out = n;
	return h;
}

void macsurf_imgdims_remember(struct nsurl *url, int w, int h);
void macsurf_imgdims_remember(struct nsurl *url, int w, int h)
{
	const char *sp;
	unsigned long hash, len;
	struct imgdims_slot *sl;

	if (url == NULL || w <= 0 || h <= 0) {
		return;
	}
	sp = nsurl_access(url);
	if (sp == NULL) {
		return;
	}
	hash = imgdims_hash(sp, &len);
	sl = &g_imgdims[hash % IMGDIMS_SLOTS];
	sl->hash = hash;
	sl->len = len;
	sl->w = w;
	sl->h = h;
	g_imgdims_stored++;
}

int macsurf_imgdims_lookup(struct nsurl *url, int *w, int *h);
int macsurf_imgdims_lookup(struct nsurl *url, int *w, int *h)
{
	const char *sp;
	unsigned long hash, len;
	struct imgdims_slot *sl;

	if (url == NULL) {
		return 0;
	}
	sp = nsurl_access(url);
	if (sp == NULL) {
		return 0;
	}
	hash = imgdims_hash(sp, &len);
	sl = &g_imgdims[hash % IMGDIMS_SLOTS];
	if (sl->w <= 0 || sl->hash != hash || sl->len != len) {
		g_imgdims_miss++;
		return 0;
	}
	g_imgdims_hit++;
	*w = sl->w;
	*h = sl->h;
	return 1;
}

static struct lazyimg_entry *g_lazyimg_head = NULL;

#define LAZYIMG_MARGIN 600   /* preload band above/below viewport (px) */

static bool macsurf_lazyimg_defer(html_content *content, struct dom_node *node,
		nsurl *url)
{
	struct lazyimg_entry *e;

	/*
	 * fixes920 - DEDUPE by (content, node). Without this the same image is
	 * queued twice and fetched twice.
	 *
	 * g_lazyimg_head is a file-scope global that NOTHING clears on a
	 * reconvert: entries are only removed when the drain fetches them, or
	 * when their content stops being live. A reconvert rebuilds the BOX tree
	 * but leaves the DOM alone, so box_image runs again over the very same
	 * dom_nodes while the previous entries for those nodes are still queued.
	 * Both then survive to the next paint, both re-resolve the same node via
	 * box_for_node, and both call html_fetch_object - which has no URL dedupe
	 * of its own (it allocates a fresh content_html_object every call), so
	 * one <img> ends up with two object entries and two base.active counts.
	 *
	 * The node pointer is a sound key precisely because the DOM persists
	 * across a reconvert; that is the same property the queue already relies
	 * on to re-resolve boxes (fixes674b).
	 *
	 * O(queue) per deferred image, and the queue only holds images not yet
	 * fetched, so this is a pointer compare over a short list - cheaper than
	 * the duplicate fetch it prevents.
	 */
	for (e = g_lazyimg_head; e != NULL; e = e->next) {
		if (e->content == content && e->node == node) {
			/* Same element re-queued. Keep ONE entry, but adopt the
			 * newer URL: JS may have changed src between passes.
			 * Ref before unref in case they are the same object. */
			if (e->url != url) {
				nsurl_ref(url);
				nsurl_unref(e->url);
				e->url = url;
			}
			return true;
		}
	}

	e = malloc(sizeof(*e));
	if (e == NULL) return false;
	e->content = content;
	e->node = node;
	dom_node_ref(node);
	e->url = url;
	nsurl_ref(url);
	e->next = g_lazyimg_head;
	g_lazyimg_head = e;
	return true;
}

/* Frontend hook (main.c update handler, after browser_window_redraw): fetch any
 * queued image whose box is within (viewport + margin), drop dead ones, leave
 * off-screen images queued for a later scroll. scroll_y / viewport_h are the
 * content-area document scroll offset and pixel height. */
void macsurf_lazyimg_viewport_changed(int scroll_y, int viewport_h)
{
	struct lazyimg_entry *keep = NULL;
	struct lazyimg_entry *e;
	int n_fetched = 0, n_kept = 0;
	int vp_top = scroll_y - LAZYIMG_MARGIN;
	int vp_bot = scroll_y + viewport_h + LAZYIMG_MARGIN;

	e = g_lazyimg_head;
	g_lazyimg_head = NULL;
	while (e != NULL) {
		struct lazyimg_entry *next = e->next;
		struct box *box;

		/* liveness: pointer-membership only, never derefs freed content */
		if (macos9_content_is_live((struct content *)e->content) == 0) {
			dom_node_unref(e->node);
			nsurl_unref(e->url);
			free(e);
			e = next;
			continue;
		}
		/* only touch the box tree once the content is DONE (stable tree) */
		if (e->content->base.status != CONTENT_STATUS_DONE) {
			e->next = keep; keep = e; n_kept++; e = next;
			continue;
		}
		box = box_for_node(e->node);
		if (box == NULL) {
			e->next = keep; keep = e; n_kept++;
		} else {
			int bx = 0, by = 0;
			int bh = box->height;

			/* fixes921 - this node's box already has its object, so
			 * there is nothing to fetch. Happens after a reconvert:
			 * the rebuild re-queues every <img> via box_image, but
			 * html_object_relink_after_reconvert has already
			 * re-attached the surviving object to the new box. Without
			 * this the drain would fetch it a second time and build a
			 * duplicate content_html_object (html_fetch_object has no
			 * URL dedupe of its own), which is the very churn the
			 * re-link exists to remove. Drop the entry, do not fetch. */
			if (box->object != NULL) {
				g_img_lazy_dropobj++; /* fixes934 */
				dom_node_unref(e->node);
				nsurl_unref(e->url);
				free(e);
				e = next;
				continue;
			}

			box_coords(box, &bx, &by);
			if (bh < 0) bh = 0;
			if (by + bh >= vp_top && by <= vp_bot) {
				(void) html_fetch_object(e->content, e->url,
						box, image_types, false);
				dom_node_unref(e->node);
				nsurl_unref(e->url);
				free(e);
				n_fetched++;
			} else {
				e->next = keep; keep = e; n_kept++;
			}
		}
		e = next;
	}
	g_lazyimg_head = keep;
	/* fixes1032 - only when something was actually FETCHED. Logging on
	 * `kept` too meant one flushed write per scroll notch: 107 lines in one
	 * session saying nothing happened. */
	if (n_fetched) {
		/* fixes934 - retagged RECON LAZY -> LIFE img lazy so the drain is
		 * visible on a default build (RECON LAZY was not on the crash-only
		 * whitelist, so it never reached disk). */
		macsurf_debug_log_writef(
			"LIFE img lazy fetched=%d kept=%d scroll=%d vh=%d",
			n_fetched, n_kept, scroll_y, viewport_h);
	}
}

/**
 * Embedded image [13.2].
 */
static bool
box_image(dom_node *n,
	  html_content *content,
	  struct box *box,
	  bool *convert_children)
{
	bool ok;
	dom_string *s;
	dom_exception err;
	nsurl *url;
	enum css_width_e wtype;
	enum css_height_e htype;
	css_fixed value = 0;
	css_unit wunit = CSS_UNIT_PX;
	css_unit hunit = CSS_UNIT_PX;

	if (box->style && ns_computed_display(box->style,
			box_is_root(n)) == CSS_DISPLAY_NONE)
		return true;

	/* handle alt text */
	err = dom_element_get_attribute(n, corestring_dom_alt, &s);
	if (err == DOM_NO_ERR && s != NULL) {
		char *alt = squash_whitespace(dom_string_data(s));
		dom_string_unref(s);
		if (alt == NULL)
			return false;
		box->text = talloc_strdup(content->bctx, alt);
		free(alt);
		if (box->text == NULL)
			return false;
		box->length = strlen(box->text);
	}

	if (nsoption_bool(foreground_images) == false) {
		return true;
	}

	/* imagemap associated with this image */
	if (!box_get_attribute(n, "usemap", content->bctx, &box->usemap))
		return false;
	if (box->usemap && box->usemap[0] == '#')
		box->usemap++;

	/* fixes168b - resolve src with data-src / data-original /
	 * data-lazy-src / data-srcset / srcset fallback chain so lazy-
	 * loaded modern images become real URLs at box-construct time. */
	if (box_image_resolve_url(n, content, &url) == false)
		return false;

	if (url == NULL)
		return true;

	/* start fetch */
	box->flags |= IS_REPLACED;

	/* fixes929 - if this URL has been loaded before (this page, an earlier
	 * visit, or the tree we are rebuilding), we already know how big it is.
	 * Give the box that size NOW, before the fetch is even queued, so the
	 * first layout is correct and stays correct however the fetch goes. */
	{
		int mw = 0, mh = 0;
		if (macsurf_imgdims_lookup(url, &mw, &mh)) {
			box->obj_w = mw;
			box->obj_h = mh;
		}
	}
	/* fixes738 - viewport-gated deferral: queue the image; the paint-path
	 * hook (macsurf_lazyimg_viewport_changed) fetches it once its box is
	 * on-screen. Fall back to an immediate fetch if the queue alloc fails.
	 *
	 * fixes932 - but EAGERLY fetch the first N images in DOM order (the
	 * above-the-fold approximation, since layout has not run yet so the box
	 * position is unknown). Those load with the document and are sized by
	 * the page's own completion reflow, instead of waiting for a paint to
	 * drain the queue. The long tail still defers, keeping the fixes738 win
	 * for galleries and below-fold thumbnails. */
	g_img_ctor_total++; /* fixes934 - construct census */
	if (content->img_eager_budget > 0) {
		content->img_eager_budget--;
		g_img_ctor_eager++;
		ok = html_fetch_object(content, url, box, image_types, false);
	} else if (macsurf_lazyimg_defer(content, n, url)) {
		g_img_ctor_lazy++;
		ok = true;
	} else {
		g_img_ctor_eager++; /* alloc-fail fallback fetches immediately */
		ok = html_fetch_object(content, url, box, image_types, false);
	}
	nsurl_unref(url);

	wtype = css_computed_width(box->style, &value, &wunit);
	htype = css_computed_height(box->style, &value, &hunit);

	if (wtype == CSS_WIDTH_SET &&
	    wunit != CSS_UNIT_PCT &&
	    htype == CSS_HEIGHT_SET &&
	    hunit != CSS_UNIT_PCT) {
		/* We know the dimensions the image will be shown at
		 * before it's fetched. */
		box->flags |= REPLACE_DIM;
		g_img_ctor_replacedim++; /* fixes934 */
	}

	return ok;
}


/**
 * Form control [17.4].
 */
static bool
box_input(dom_node *n,
	  html_content *content,
	  struct box *box,
	  bool *convert_children)
{
	struct form_control *gadget;
	dom_string *type = NULL;
	dom_exception err;
	nsurl *url;
	nserror error;

	gadget = html_forms_get_control_for_node(content->forms, n);
	if (gadget == NULL) {
		return false;
	}

	box->gadget = gadget;
	box->flags |= IS_REPLACED;
	gadget->box = box;
	gadget->html = content;

	/* get entry type */
	err = dom_element_get_attribute(n, corestring_dom_type, &type);
	if ((err != DOM_NO_ERR) || (type == NULL)) {
		/* no type so "text" is assumed */
		if (box_input_text(content, box, n) == false) {
			return false;
		}
		*convert_children = false;
		return true;
	}

	if (dom_string_caseless_lwc_isequal(type, corestring_lwc_password)) {
		if (box_input_text(content, box, n) == false)
			goto no_memory;

	} else if (dom_string_caseless_lwc_isequal(type, corestring_lwc_file)) {
		box->type = BOX_INLINE_BLOCK;

	} else if (dom_string_caseless_lwc_isequal(type,
			corestring_lwc_hidden)) {
		/* no box for hidden inputs */
		box->type = BOX_NONE;

	} else if ((dom_string_caseless_lwc_isequal(type,
				corestring_lwc_checkbox) ||
			dom_string_caseless_lwc_isequal(type,
				corestring_lwc_radio))) {

	} else if (dom_string_caseless_lwc_isequal(type,
				corestring_lwc_submit) ||
			dom_string_caseless_lwc_isequal(type,
				corestring_lwc_reset) ||
			dom_string_caseless_lwc_isequal(type,
				corestring_lwc_button)) {
		struct box *inline_container, *inline_box;

		if (box_button(n, content, box, 0) == false)
			goto no_memory;

		inline_container = box_create(NULL, 0, false, 0, 0, 0, 0,
				content->bctx);
		if (inline_container == NULL)
			goto no_memory;

		inline_container->type = BOX_INLINE_CONTAINER;

		inline_box = box_create(NULL, box->style, false, 0, 0,
				box->title, 0, content->bctx);
		if (inline_box == NULL)
			goto no_memory;

		inline_box->type = BOX_TEXT;

		if (box->gadget->value != NULL)
			inline_box->text = talloc_strdup(content->bctx,
					box->gadget->value);
		else if (box->gadget->type == GADGET_SUBMIT)
			inline_box->text = talloc_strdup(content->bctx,
					messages_get("Form_Submit"));
		else if (box->gadget->type == GADGET_RESET)
			inline_box->text = talloc_strdup(content->bctx,
					messages_get("Form_Reset"));
		else
			inline_box->text = talloc_strdup(content->bctx,
							 "Button");

		if (inline_box->text == NULL)
			goto no_memory;

		inline_box->length = strlen(inline_box->text);

		box_add_child(inline_container, inline_box);

		box_add_child(box, inline_container);

	} else if (dom_string_caseless_lwc_isequal(type,
						   corestring_lwc_image)) {
		gadget->type = GADGET_IMAGE;

		if (box->style && ns_computed_display(box->style,
				box_is_root(n)) != CSS_DISPLAY_NONE &&
				nsoption_bool(foreground_images) == true) {
			dom_string *s;

			err = dom_element_get_attribute(n, corestring_dom_src, &s);
			if (err == DOM_NO_ERR && s != NULL) {
				error = nsurl_join(content->base_url,
						dom_string_data(s), &url);
				dom_string_unref(s);
				if (error != NSERROR_OK)
					goto no_memory;

				/* if url is equivalent to the parent's url,
				 * we've got infinite inclusion. stop it here
				 */
				if (nsurl_compare(url, content->base_url,
						NSURL_COMPLETE) == false) {
					if (!html_fetch_object(content,
							       url,
							       box,
							       image_types,
							       false)) {
						nsurl_unref(url);
						goto no_memory;
					}
				}
				nsurl_unref(url);
			}
		}
	} else {
		/* unhandled type the default is "text" */
		if (box_input_text(content, box, n) == false)
			goto no_memory;
	}

	dom_string_unref(type);

	*convert_children = false;

	return true;

no_memory:
	dom_string_unref(type);

	return false;
}


/**
 * Noscript element
 */
static bool
box_noscript(dom_node *n,
	     html_content *content,
	     struct box *box,
	     bool *convert_children)
{
	/* If scripting is enabled, do not display the contents of noscript */
	if (content->enable_scripting) {
		*convert_children = false;
	}

	return true;
}


/**
 * Generic embedded object [13.3].
 */
static bool
box_object(dom_node *n,
	   html_content *content,
	   struct box *box,
	   bool *convert_children)
{
	struct object_params *params;
	struct object_param *param;
	dom_string *codebase, *classid, *data;
	dom_node *c;
	dom_exception err;

	if (box->style && ns_computed_display(box->style,
			box_is_root(n)) == CSS_DISPLAY_NONE)
		return true;

	if (box_get_attribute(n, "usemap", content->bctx, &box->usemap) ==
			false)
		return false;
	if (box->usemap && box->usemap[0] == '#')
		box->usemap++;

	params = talloc(content->bctx, struct object_params);
	if (params == NULL)
		return false;

	talloc_set_destructor(params, box_object_talloc_destructor);

	params->data = NULL;
	params->type = NULL;
	params->codetype = NULL;
	params->codebase = NULL;
	params->classid = NULL;
	params->params = NULL;

	/* codebase, classid, and data are URLs
	 * (codebase is the base for the other two) */
	err = dom_element_get_attribute(n, corestring_dom_codebase, &codebase);
	if (err == DOM_NO_ERR && codebase != NULL) {
		if (box_extract_link(content, codebase,	content->base_url,
				&params->codebase) == false) {
			dom_string_unref(codebase);
			return false;
		}
		dom_string_unref(codebase);
	}
	if (params->codebase == NULL)
		params->codebase = nsurl_ref(content->base_url);

	err = dom_element_get_attribute(n, corestring_dom_classid, &classid);
	if (err == DOM_NO_ERR && classid != NULL) {
		if (box_extract_link(content, classid,
				params->codebase, &params->classid) == false) {
			dom_string_unref(classid);
			return false;
		}
		dom_string_unref(classid);
	}

	err = dom_element_get_attribute(n, corestring_dom_data, &data);
	if (err == DOM_NO_ERR && data != NULL) {
		if (box_extract_link(content, data,
				params->codebase, &params->data) == false) {
			dom_string_unref(data);
			return false;
		}
		dom_string_unref(data);
	}

	if (params->classid == NULL && params->data == NULL)
		/* nothing to embed; ignore */
		return true;

	/* Don't include ourself */
	if (params->classid != NULL && nsurl_compare(content->base_url,
			params->classid, NSURL_COMPLETE))
		return true;

	if (params->data != NULL && nsurl_compare(content->base_url,
			params->data, NSURL_COMPLETE))
		return true;

	/* codetype and type are MIME types */
	if (box_get_attribute(n, "codetype", params,
			&params->codetype) == false)
		return false;
	if (box_get_attribute(n, "type", params, &params->type) == false)
		return false;

	/* classid && !data => classid is used (consult codetype)
	 * (classid || !classid) && data => data is used (consult type)
	 * !classid && !data => invalid; ignored */

	if (params->classid != NULL && params->data == NULL &&
			params->codetype != NULL) {
		lwc_string *icodetype;
		lwc_error lerror;

		lerror = lwc_intern_string(params->codetype,
				strlen(params->codetype), &icodetype);
		if (lerror != lwc_error_ok)
			return false;

		if (content_factory_type_from_mime_type(icodetype) ==
				CONTENT_NONE) {
			/* can't handle this MIME type */
			lwc_string_unref(icodetype);
			return true;
		}

		lwc_string_unref(icodetype);
	}

	if (params->data != NULL && params->type != NULL) {
		lwc_string *itype;
		lwc_error lerror;

		lerror = lwc_intern_string(params->type, strlen(params->type),
				&itype);
		if (lerror != lwc_error_ok)
			return false;

		if (content_factory_type_from_mime_type(itype) ==
				CONTENT_NONE) {
			/* can't handle this MIME type */
			lwc_string_unref(itype);
			return true;
		}

		lwc_string_unref(itype);
	}

	/* add parameters to linked list */
	err = dom_node_get_first_child(n, &c);
	if (err != DOM_NO_ERR)
		return false;

	while (c != NULL) {
		dom_node *next;
		dom_node_type type;

		err = dom_node_get_node_type(c, &type);
		if (err != DOM_NO_ERR) {
			dom_node_unref(c);
			return false;
		}

		if (type == DOM_ELEMENT_NODE) {
			dom_string *name;

			err = dom_node_get_node_name(c, &name);
			if (err != DOM_NO_ERR) {
				dom_node_unref(c);
				return false;
			}

			if (!dom_string_caseless_lwc_isequal(name,
					corestring_lwc_param)) {
				/* The first non-param child is the start of
				 * the alt html. Therefore, we should break
				 * out of this loop. */
				dom_string_unref(name);
				dom_node_unref(c);
				break;
			}
			dom_string_unref(name);

			param = talloc(params, struct object_param);
			if (param == NULL) {
				dom_node_unref(c);
				return false;
			}
			param->name = NULL;
			param->value = NULL;
			param->type = NULL;
			param->valuetype = NULL;
			param->next = NULL;

			if (box_get_attribute(c, "name", param,
					&param->name) == false) {
				dom_node_unref(c);
				return false;
			}

			if (box_get_attribute(c, "value", param,
					&param->value) == false) {
				dom_node_unref(c);
				return false;
			}

			if (box_get_attribute(c, "type", param,
					&param->type) == false) {
				dom_node_unref(c);
				return false;
			}

			if (box_get_attribute(c, "valuetype", param,
					&param->valuetype) == false) {
				dom_node_unref(c);
				return false;
			}

			if (param->valuetype == NULL) {
				param->valuetype = talloc_strdup(param, "data");
				if (param->valuetype == NULL) {
					dom_node_unref(c);
					return false;
				}
			}

			param->next = params->params;
			params->params = param;
		}

		err = dom_node_get_next_sibling(c, &next);
		if (err != DOM_NO_ERR) {
			dom_node_unref(c);
			return false;
		}

		dom_node_unref(c);
		c = next;
	}

	box->object_params = params;

	/* start fetch (MIME type is ok or not specified) */
	box->flags |= IS_REPLACED;
	if (!html_fetch_object(content,
			       params->data ? params->data : params->classid,
			       box,
			       CONTENT_ANY,
			       false))
		return false;

	*convert_children = false;
	return true;
}


/**
 * Preformatted text [9.3.4].
 */
static bool
box_pre(dom_node *n,
	html_content *content,
	struct box *box,
	bool *convert_children)
{
	box->flags |= PRE_STRIP;
	return true;
}


/**
 * Option selector [17.6].
 */
static bool
box_select(dom_node *n,
	   html_content *content,
	   struct box *box,
	   bool *convert_children)
{
	struct box *inline_container;
	struct box *inline_box;
	struct form_control *gadget;
	dom_node *c, *c2;
	dom_node *next, *next2;
	dom_exception err;

	gadget = html_forms_get_control_for_node(content->forms, n);
	if (gadget == NULL)
		return false;

	gadget->html = content;
	err = dom_node_get_first_child(n, &c);
	if (err != DOM_NO_ERR) {
		form_free_control(gadget);
		return false;
	}

	while (c != NULL) {
		dom_string *name;

		err = dom_node_get_node_name(c, &name);
		if (err != DOM_NO_ERR) {
			dom_node_unref(c);
			form_free_control(gadget);
			return false;
		}

		if (dom_string_caseless_lwc_isequal(name,
				corestring_lwc_option)) {
			dom_string_unref(name);

			if (box_select_add_option(gadget, c) == false) {
				dom_node_unref(c);
				form_free_control(gadget);
				return false;
			}
		} else if (dom_string_caseless_lwc_isequal(name,
				corestring_lwc_optgroup)) {
			dom_string_unref(name);

			err = dom_node_get_first_child(c, &c2);
			if (err != DOM_NO_ERR) {
				dom_node_unref(c);
				form_free_control(gadget);
				return false;
			}

			while (c2 != NULL) {
				dom_string *c2_name;

				err = dom_node_get_node_name(c2, &c2_name);
				if (err != DOM_NO_ERR) {
					dom_node_unref(c2);
					dom_node_unref(c);
					form_free_control(gadget);
					return false;
				}

				if (dom_string_caseless_lwc_isequal(c2_name,
						corestring_lwc_option)) {
					dom_string_unref(c2_name);

					if (box_select_add_option(gadget,
							c2) == false) {
						dom_node_unref(c2);
						dom_node_unref(c);
						form_free_control(gadget);
						return false;
					}
				} else {
					dom_string_unref(c2_name);
				}

				err = dom_node_get_next_sibling(c2, &next2);
				if (err != DOM_NO_ERR) {
					dom_node_unref(c2);
					dom_node_unref(c);
					form_free_control(gadget);
					return false;
				}

				dom_node_unref(c2);
				c2 = next2;
			}
		} else {
			dom_string_unref(name);
		}

		err = dom_node_get_next_sibling(c, &next);
		if (err != DOM_NO_ERR) {
			dom_node_unref(c);
			form_free_control(gadget);
			return false;
		}

		dom_node_unref(c);
		c = next;
	}

	if (gadget->data.select.num_items == 0) {
		/* no options: ignore entire select */
		form_free_control(gadget);
		return true;
	}

	box->type = BOX_INLINE_BLOCK;
	box->gadget = gadget;
	box->flags |= IS_REPLACED;
	gadget->box = box;

	inline_container = box_create(NULL, 0, false, 0, 0, 0, 0, content->bctx);
	if (inline_container == NULL)
		goto no_memory;
	inline_container->type = BOX_INLINE_CONTAINER;
	inline_box = box_create(NULL, box->style, false, 0, 0, box->title, 0,
			content->bctx);
	if (inline_box == NULL)
		goto no_memory;
	inline_box->type = BOX_TEXT;
	box_add_child(inline_container, inline_box);
	box_add_child(box, inline_container);

	if (gadget->data.select.multiple == false &&
			gadget->data.select.num_selected == 0) {
		gadget->data.select.current = gadget->data.select.items;
		gadget->data.select.current->initial_selected =
			gadget->data.select.current->selected = true;
		gadget->data.select.num_selected = 1;
		dom_html_option_element_set_selected(
				gadget->data.select.current->node, true);
	}

	if (gadget->data.select.num_selected == 0)
		inline_box->text = talloc_strdup(content->bctx,
				messages_get("Form_None"));
	else if (gadget->data.select.num_selected == 1)
		inline_box->text = talloc_strdup(content->bctx,
				gadget->data.select.current->text);
	else
		inline_box->text = talloc_strdup(content->bctx,
				messages_get("Form_Many"));
	if (inline_box->text == NULL)
		goto no_memory;

	inline_box->length = strlen(inline_box->text);

	*convert_children = false;
	return true;

no_memory:
	return false;
}


/**
 * Multi-line text field [17.7].
 */
static bool box_textarea(dom_node *n,
			html_content *content,
			struct box *box,
			bool *convert_children)
{
	/* Get the form_control for the DOM node */
	box->gadget = html_forms_get_control_for_node(content->forms, n);
	if (box->gadget == NULL) {
		macsurf_debug_log_writef(
			"WORK form control: textarea has no gadget node=%p", (void *) n);
		return false;
	}

	box->flags |= IS_REPLACED;
	box->gadget->html = content;
	box->gadget->box = box;

	if (!box_input_text(content, box, n)) {
		macsurf_debug_log_writef(
			"WORK form control: textarea box setup failed"
			" gadget_type=%d box=%p node=%p",
			(int) box->gadget->type, (void *) box, (void *) n);
		return false;
	}

	*convert_children = false;
	return true;
}


/**
 * \}
 */


/* exported interface documented in html/box_special.h */
bool
convert_special_elements(dom_node *node,
			 html_content *content,
			 struct box *box,
			 bool *convert_children)
{
	dom_exception exc;
	dom_html_element_type tag_type;
	bool res;

	exc = macsurf_html_element_get_tag_type(node, &tag_type);
	if (exc != DOM_NO_ERR) {
		tag_type = DOM_HTML_ELEMENT_TYPE__UNKNOWN;
	}

	switch (tag_type) {
	case DOM_HTML_ELEMENT_TYPE_A:
		res =  box_a(node, content, box, convert_children);
		break;

	case DOM_HTML_ELEMENT_TYPE_BODY:
		res = box_body(node, content, box, convert_children);
		break;

	case DOM_HTML_ELEMENT_TYPE_HTML:
		res = box_html(node, content, box, convert_children);
		break;

	case DOM_HTML_ELEMENT_TYPE_BR:
		res = box_br(node, content, box, convert_children);
		break;

	case DOM_HTML_ELEMENT_TYPE_BUTTON:
		res = box_button(node, content, box, convert_children);
		break;

	case DOM_HTML_ELEMENT_TYPE_CANVAS:
		res = box_canvas(node, content, box, convert_children);
		break;

	case DOM_HTML_ELEMENT_TYPE_EMBED:
		res = box_embed(node, content, box, convert_children);
		break;

	case DOM_HTML_ELEMENT_TYPE_FRAMESET:
		res = box_frameset(node, content, box, convert_children);
		break;

	case DOM_HTML_ELEMENT_TYPE_IFRAME:
		res = box_iframe(node, content, box, convert_children);
		break;

	case DOM_HTML_ELEMENT_TYPE_IMG:
		res = box_image(node, content, box, convert_children);
		break;

	case DOM_HTML_ELEMENT_TYPE_INPUT:
		res = box_input(node, content, box, convert_children);
		break;

	case DOM_HTML_ELEMENT_TYPE_NOSCRIPT:
		res = box_noscript(node, content, box, convert_children);
		break;

	case DOM_HTML_ELEMENT_TYPE_OBJECT:
		res = box_object(node, content, box, convert_children);
		break;

	case DOM_HTML_ELEMENT_TYPE_PRE:
		res = box_pre(node, content, box, convert_children);
		break;

	case DOM_HTML_ELEMENT_TYPE_SELECT:
		res = box_select(node, content, box, convert_children);
		break;

	case DOM_HTML_ELEMENT_TYPE_TEXTAREA:
		res = box_textarea(node, content, box, convert_children);
		break;

	default:
		res = true;
	}

	return res;
}
