/*
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

/**
 * \file
 * Box tree treeview box replacement (implementation).
 */

#include <string.h>
#include <dom/dom.h>

#include "utils/config.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "netsurf/keypress.h"
#include "netsurf/misc.h"
#include "desktop/textarea.h"

#include "html/private.h"
#include "html/interaction.h"
#include "html/box.h"
#include "html/box_inspect.h"
#include "html/box_textarea.h"
#include "html/font.h"
#include "html/form_internal.h"


nserror box_textarea_keypress(html_content *html, struct box *box, uint32_t key)
{
	struct form_control *gadget = box->gadget;
	struct textarea *ta = gadget->data.text.ta;
	struct form* form = box->gadget->form;
	struct content *c = (struct content *)html;
	nserror res = NSERROR_OK;

	assert(ta != NULL);

	if (gadget->type == GADGET_TEXTAREA) {
		if (textarea_keypress(ta, key)) {
			return NSERROR_OK;
		} else {
			return NSERROR_INVALID;
		}
	}

	/* non textarea input */
	switch (key) {
	case NS_KEY_NL:
	case NS_KEY_CR:
		if (form) {
			res = form_submit(content_get_url(c),
					  html->bw,
					  form,
					  NULL);
		}
		break;

	case NS_KEY_TAB:
		{
			struct form_control *next_input;
			/* Find next text entry field that is actually
			 * displayed (i.e. has an associated box) */
			for (next_input = gadget->next;
			     next_input &&
				     ((next_input->type != GADGET_TEXTBOX &&
				       next_input->type != GADGET_TEXTAREA &&
				       next_input->type != GADGET_PASSWORD) ||
				      !next_input->box);
			     next_input = next_input->next)
				;

			if (next_input != NULL) {
				textarea_set_caret(ta, -1);
				textarea_set_caret(next_input->data.text.ta, 0);
			}
		}
		break;

	case NS_KEY_SHIFT_TAB:
		{
			struct form_control *prev_input;
			/* Find previous text entry field that is actually
			 * displayed (i.e. has an associated box) */
			for (prev_input = gadget->prev;
			     prev_input &&
				     ((prev_input->type != GADGET_TEXTBOX &&
				       prev_input->type != GADGET_TEXTAREA &&
				       prev_input->type != GADGET_PASSWORD) ||
				      !prev_input->box);
			     prev_input = prev_input->prev)
				;

			if (prev_input != NULL) {
				textarea_set_caret(ta, -1);
				textarea_set_caret(prev_input->data.text.ta, 0);
			}
		}
		break;

	default:
		/* Pass to textarea widget */
		if (!textarea_keypress(ta, key)) {
			res = NSERROR_INVALID;
		}
		break;
	}

	return res;
}


/**
 * Callback for html form textareas.
 */
static void box_textarea_callback(void *data, struct textarea_msg *msg)
{
	struct form_textarea_data *d = data;
	struct form_control *gadget = d->gadget;
	struct html_content *html = d->gadget->html;
	struct box *box = gadget->box;

	switch (msg->type) {
	case TEXTAREA_MSG_DRAG_REPORT:
		if (msg->data.drag == TEXTAREA_DRAG_NONE) {
			/* Textarea drag finished */
			html_drag_type drag_type = HTML_DRAG_NONE;
			union html_drag_owner drag_owner;
			drag_owner.no_owner = true;

			html_set_drag_type(html, drag_type, drag_owner,
					NULL);
		} else {
			/* Textarea drag started */
			struct rect rect;
			union html_drag_owner drag_owner;
			rect.x0 = INT_MIN;
			rect.y0 = INT_MIN;
			rect.x1 = INT_MAX;
			rect.y1 = INT_MAX;
			drag_owner.textarea = box;

			switch (msg->data.drag) {
			case TEXTAREA_DRAG_SCROLLBAR:
				html_set_drag_type(html,
						   HTML_DRAG_TEXTAREA_SCROLLBAR,
						   drag_owner,
						   &rect);
				break;

			case TEXTAREA_DRAG_SELECTION:
				html_set_drag_type(html,
						   HTML_DRAG_TEXTAREA_SELECTION,
						   drag_owner,
						   &rect);
				break;

			default:
				NSLOG(netsurf, INFO,
				      "Drag type %d not handled.",
				      msg->data.drag);
				/* This is a logic faliure in the
				 * front end code so abort.
				 */
				assert(0);
				break;
			}
		}
		break;

	case TEXTAREA_MSG_REDRAW_REQUEST:
	{
		/* Request redraw of the required textarea rectangle */
		int x, y;

		if (html->reflowing == true) {
			/* Can't redraw during layout, and it will
			 * be redrawn after layout anyway. */
			break;
		}

		box_coords(box, &x, &y);

		content__request_redraw((struct content *)html,
				x + msg->data.redraw.x0,
				y + msg->data.redraw.y0,
				msg->data.redraw.x1 - msg->data.redraw.x0,
				msg->data.redraw.y1 - msg->data.redraw.y0);
	}
		break;

	case TEXTAREA_MSG_SELECTION_REPORT:
		if (msg->data.selection.have_selection) {
			/* Textarea now has a selection */
			union html_selection_owner sel_owner;
			sel_owner.textarea = box;

			html_set_selection(html, HTML_SELECTION_TEXTAREA,
					sel_owner,
					msg->data.selection.read_only);
		} else {
			/* The textarea now has no selection */
			union html_selection_owner sel_owner;
			sel_owner.none = true;

			html_set_selection(html, HTML_SELECTION_NONE,
					sel_owner, true);
		}
		break;

	case TEXTAREA_MSG_CARET_UPDATE:
		if (html->bw == NULL)
			break;

		if (msg->data.caret.type == TEXTAREA_CARET_HIDE) {
			union html_focus_owner focus_owner;
			focus_owner.textarea = box;
			html_set_focus(html, HTML_FOCUS_TEXTAREA,
					focus_owner, true, 0, 0, 0, NULL);
		} else {
			union html_focus_owner focus_owner;
			focus_owner.textarea = box;
			html_set_focus(html, HTML_FOCUS_TEXTAREA,
					focus_owner, false,
					msg->data.caret.pos.x,
					msg->data.caret.pos.y,
					msg->data.caret.pos.height,
					msg->data.caret.pos.clip);
		}
		break;

	case TEXTAREA_MSG_TEXT_MODIFIED:
		form_gadget_update_value(gadget,
					 strndup(msg->data.modified.text,
						 msg->data.modified.len));
		break;
	}
}


/* Exported interface, documented in box_textarea.h */
bool box_textarea_create_textarea(html_content *html,
		struct box *box, struct dom_node *node)
{
	dom_string *dom_text = NULL;
	dom_exception err;
	textarea_setup ta_setup;
	textarea_flags ta_flags;
	/* MacSurf: positional init for CW8 C89.
	 * plot_font_style_t order: families, family, size, weight, flags,
	 *                          background, foreground. */
	plot_font_style_t fstyle = {
		NULL,
		PLOT_FONT_FAMILY_SANS_SERIF,
		10 * PLOT_STYLE_SCALE,
		400,
		FONTF_NONE,
		0,
		0
	};
	bool read_only = false;
	bool disabled = false;
	struct form_control *gadget = box->gadget;
	const char *text;

	assert(gadget != NULL);
	assert(gadget->type == GADGET_TEXTAREA ||
			gadget->type == GADGET_TEXTBOX ||
			gadget->type == GADGET_PASSWORD);

	if (gadget->type == GADGET_TEXTAREA) {
		dom_html_text_area_element *textarea =
				(dom_html_text_area_element *) node;
		ta_flags = TEXTAREA_MULTILINE;

		err = dom_html_text_area_element_get_read_only(
				textarea, &read_only);
		if (err != DOM_NO_ERR)
			return false;

		err = dom_html_text_area_element_get_disabled(
				textarea, &disabled);
		if (err != DOM_NO_ERR)
			return false;

		/* Get the textarea's initial content */
		err = dom_html_text_area_element_get_value(textarea, &dom_text);
		if (err != DOM_NO_ERR)
			return false;

	} else {
		dom_html_input_element *input = (dom_html_input_element *) node;

		err = dom_html_input_element_get_read_only(
				input, &read_only);
		if (err != DOM_NO_ERR)
			return false;

		err = dom_html_input_element_get_disabled(
				input, &disabled);
		if (err != DOM_NO_ERR)
			return false;

		if (gadget->type == GADGET_PASSWORD)
			ta_flags = TEXTAREA_PASSWORD;
		else
			ta_flags = TEXTAREA_DEFAULT;

		/* Get initial text */
		err = dom_html_input_element_get_value(input, &dom_text);
		if (err != DOM_NO_ERR)
			return false;
	}

	if (dom_text != NULL) {
		text = dom_string_data(dom_text);
	} else {
		/* No initial text, or failed reading it;
		 * use a blank string */
		text = "";
	}

	if (read_only || disabled)
		ta_flags |= TEXTAREA_READONLY;

	gadget->data.text.data.gadget = gadget;

	/* Reset to correct values by layout */
	ta_setup.width = 200;
	ta_setup.height = 20;
	ta_setup.pad_top = 4;
	ta_setup.pad_right = 4;
	ta_setup.pad_bottom = 4;
	ta_setup.pad_left = 4;

	/* Set remaining data */
	ta_setup.border_width = 0;
	ta_setup.border_col = 0x000000;
	ta_setup.text = fstyle;
	ta_setup.text.background = NS_TRANSPARENT;
	/* Make selected text either black or white, as gives greatest contrast
	 * with background colour. */
	ta_setup.selected_bg = fstyle.foreground;
	ta_setup.selected_text = colour_to_bw_furthest(ta_setup.selected_bg);

	/* Hand reference to dom text over to gadget */
	gadget->data.text.initial = dom_text;

	gadget->data.text.ta = textarea_create(ta_flags, &ta_setup,
			box_textarea_callback, &gadget->data.text.data);

	if (gadget->data.text.ta == NULL) {
		return false;
	}

	if (!textarea_set_text(gadget->data.text.ta, text))
		return false;

	return true;
}
