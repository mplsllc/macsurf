/*
 * Copyright 2010 Michael Drake <tlsa@netsurf-browser.org>
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
 * Implementation of HTML content DOM event handling.
 */

#include <string.h>

#include "utils/config.h"
#include "utils/utils.h"
#include "utils/corestrings.h"
#include "utils/nsoption.h"
#include "utils/log.h"
#include "utils/ascii.h"
#include "utils/string.h"
#include "utils/nsurl.h"
#include "content/content.h"
#include "javascript/js.h"

#include "netsurf/bitmap.h"

#include "html/private.h"
#include "html/object.h"
#include "html/css.h"
#include "html/box.h"
#include "html/box_construct.h"
#include "html/form_internal.h"
#include "html/dom_event.h"


/**
 * process a base element being inserted into the DOM
 *
 * \param htmlc The html content containing the DOM
 * \param node The DOM node being inserted
 * \return NSERROR_OK on success else appropriate error code
 */
static bool html_process_inserted_base(html_content *htmlc, dom_node *node)
{
	dom_exception exc; /* returned by libdom functions */
	dom_string *atr_string;

	/* get href attribute if present */
	exc = dom_element_get_attribute(node, corestring_dom_href, &atr_string);
	if ((exc == DOM_NO_ERR) && (atr_string != NULL)) {
		nsurl *url;
		nserror error;

		/* get url from string */
		error = nsurl_create(dom_string_data(atr_string), &url);
		dom_string_unref(atr_string);
		if (error == NSERROR_OK) {
			if (htmlc->base_url != NULL) {
				nsurl_unref(htmlc->base_url);
			}
			htmlc->base_url = url;
		}
	}


	/* get target attribute if present and not already set */
	if (htmlc->base_target != NULL) {
		return true;
	}

	exc = dom_element_get_attribute(node,
					corestring_dom_target,
					&atr_string);
	if ((exc == DOM_NO_ERR) && (atr_string != NULL)) {
		/* Validation rules from the HTML5 spec for the base element:
		 *  The target must be one of _blank, _self, _parent, or
		 *  _top or any identifier which does not begin with an
		 *  underscore
		 */
		if (*dom_string_data(atr_string) != '_' ||
				dom_string_caseless_lwc_isequal(atr_string,
						corestring_lwc__blank) ||
				dom_string_caseless_lwc_isequal(atr_string,
						corestring_lwc__self) ||
				dom_string_caseless_lwc_isequal(atr_string,
						corestring_lwc__parent) ||
				dom_string_caseless_lwc_isequal(atr_string,
						corestring_lwc__top)) {
			htmlc->base_target = strdup(dom_string_data(atr_string));
		}
		dom_string_unref(atr_string);
	}

	return true;
}



/**
 * Process img element being inserted into the DOM.
 *
 * \param htmlc The html content containing the DOM
 * \param node The DOM node being inserted
 * \return NSERROR_OK on success else appropriate error code
 */
static bool html_process_inserted_img(html_content *htmlc, dom_node *node)
{
	dom_string *src;
	nsurl *url;
	nserror err;
	dom_exception exc;
	bool success;

	/* Do nothing if foreground images are disabled */
	if (nsoption_bool(foreground_images) == false) {
		return true;
	}

	exc = dom_element_get_attribute(node, corestring_dom_src, &src);
	if (exc != DOM_NO_ERR || src == NULL) {
		return true;
	}

	err = nsurl_join(htmlc->base_url, dom_string_data(src), &url);
	if (err != NSERROR_OK) {
		dom_string_unref(src);
		return false;
	}
	dom_string_unref(src);

	/* Speculatively fetch the image */
	success = html_fetch_object(htmlc, url, NULL, CONTENT_IMAGE, false);
	nsurl_unref(url);

	return success;
}


/**
 * process a LINK element being inserted into the DOM
 *
 * \note only the http-equiv attribute for refresh is currently considered
 *
 * \param htmlc The html content containing the DOM
 * \param n The DOM node being inserted
 * \return NSERROR_OK on success else appropriate error code
 */
static bool html_process_inserted_link(html_content *c, dom_node *node)
{
	struct content_rfc5988_link link; /* the link added to the content */
	dom_exception exc; /* returned by libdom functions */
	dom_string *atr_string;
	nserror error;

	/* Handle stylesheet loading */
	html_css_process_link(c, (dom_node *)node);

	/* try Generic link handling */

	memset(&link, 0, sizeof(struct content_rfc5988_link));

	/* check that the relation exists - w3c spec says must be present */
	exc = dom_element_get_attribute(node, corestring_dom_rel, &atr_string);
	if ((exc != DOM_NO_ERR) || (atr_string == NULL)) {
		return false;
	}
	/* get a lwc string containing the link relation */
	exc = dom_string_intern(atr_string, &link.rel);
	dom_string_unref(atr_string);
	if (exc != DOM_NO_ERR) {
		return false;
	}

	/* check that the href exists - w3c spec says must be present */
	exc = dom_element_get_attribute(node, corestring_dom_href, &atr_string);
	if ((exc != DOM_NO_ERR) || (atr_string == NULL)) {
		lwc_string_unref(link.rel);
		return false;
	}

	/* get nsurl */
	error = nsurl_join(c->base_url, dom_string_data(atr_string),
			&link.href);
	dom_string_unref(atr_string);
	if (error != NSERROR_OK) {
		lwc_string_unref(link.rel);
		return false;
	}

	/* look for optional properties -- we don't care if internment fails */

	exc = dom_element_get_attribute(node,
			corestring_dom_hreflang, &atr_string);
	if ((exc == DOM_NO_ERR) && (atr_string != NULL)) {
		/* get a lwc string containing the href lang */
		(void)dom_string_intern(atr_string, &link.hreflang);
		dom_string_unref(atr_string);
	}

	exc = dom_element_get_attribute(node,
			corestring_dom_type, &atr_string);
	if ((exc == DOM_NO_ERR) && (atr_string != NULL)) {
		/* get a lwc string containing the type */
		(void)dom_string_intern(atr_string, &link.type);
		dom_string_unref(atr_string);
	}

	exc = dom_element_get_attribute(node,
			corestring_dom_media, &atr_string);
	if ((exc == DOM_NO_ERR) && (atr_string != NULL)) {
		/* get a lwc string containing the media */
		(void)dom_string_intern(atr_string, &link.media);
		dom_string_unref(atr_string);
	}

	exc = dom_element_get_attribute(node,
			corestring_dom_sizes, &atr_string);
	if ((exc == DOM_NO_ERR) && (atr_string != NULL)) {
		/* get a lwc string containing the sizes */
		(void)dom_string_intern(atr_string, &link.sizes);
		dom_string_unref(atr_string);
	}

	/* add to content */
	content__add_rfc5988_link(&c->base, &link);

	lwc_string_unref(link.sizes);
	lwc_string_unref(link.media);
	lwc_string_unref(link.type);
	lwc_string_unref(link.hreflang);

	nsurl_unref(link.href);
	lwc_string_unref(link.rel);

	return true;
}


/* handler for a SCRIPT which has been added to a tree */
static void
dom_SCRIPT_showed_up(html_content *htmlc, dom_html_script_element *script)
{
	dom_exception exc;
	dom_html_script_element_flags flags;
	dom_hubbub_error res;
	bool within;

	if (!htmlc->enable_scripting) {
		NSLOG(netsurf, INFO, "Encountered a script, but scripting is off, ignoring");
		return;
	}

	NSLOG(netsurf, DEEPDEBUG, "Encountered a script, node %p showed up", script);

	exc = dom_html_script_element_get_flags(script, &flags);
	if (exc != DOM_NO_ERR) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to retrieve flags, giving up");
		return;
	}

	if (flags & DOM_HTML_SCRIPT_ELEMENT_FLAG_PARSER_INSERTED) {
		NSLOG(netsurf, DEBUG, "Script was parser inserted, skipping");
		return;
	}

	exc = dom_node_contains(htmlc->document, script, &within);
	if (exc != DOM_NO_ERR) {
		NSLOG(netsurf, DEBUG, "Unable to determine if script was within document, ignoring");
		return;
	}

	if (!within) {
		NSLOG(netsurf, DEBUG, "Script was not within the document, ignoring for now");
		return;
	}

	res = html_process_script(htmlc, (dom_node *) script);
	if (res == DOM_HUBBUB_OK) {
		NSLOG(netsurf, DEEPDEBUG, "Inserted script has finished running");
	} else {
		if (res == (DOM_HUBBUB_HUBBUB_ERR | HUBBUB_PAUSED)) {
			NSLOG(netsurf, DEEPDEBUG, "Inserted script has launced asynchronously");
		} else {
			NSLOG(netsurf, DEEPDEBUG, "Failure starting script");
		}
	}
}


/**
 * process a META element being inserted into the DOM
 *
 * \note only the http-equiv attribute for refresh is currently considered
 *
 * \param htmlc The html content containing the DOM
 * \param n The DOM node being inserted
 * \return NSERROR_OK on success else appropriate error code
 */
static nserror html_process_inserted_meta(html_content *c, dom_node *n)
{
	union content_msg_data msg_data;
	const char *url, *end, *refresh = NULL;
	char *new_url;
	char quote = '\0';
	dom_string *equiv, *content;
	dom_exception exc;
	nsurl *nsurl;
	nserror error = NSERROR_OK;

	if (c->refresh) {
		/* refresh already delt with */
		return NSERROR_OK;
	}

	exc = dom_element_get_attribute(n, corestring_dom_http_equiv, &equiv);
	if (exc != DOM_NO_ERR) {
		return NSERROR_DOM;
	}

	if (equiv == NULL) {
		return NSERROR_OK;
	}

	if (!dom_string_caseless_lwc_isequal(equiv, corestring_lwc_refresh)) {
		dom_string_unref(equiv);
		return NSERROR_OK;
	}

	dom_string_unref(equiv);

	exc = dom_element_get_attribute(n, corestring_dom_content, &content);
	if (exc != DOM_NO_ERR) {
		return NSERROR_DOM;
	}

	if (content == NULL) {
		return NSERROR_OK;
	}

	end = dom_string_data(content) + dom_string_byte_length(content);

	/* content  := *LWS intpart fracpart? *LWS [';' *LWS *1url *LWS]
	 * intpart  := 1*DIGIT
	 * fracpart := 1*('.' | DIGIT)
	 * url      := "url" *LWS '=' *LWS (url-nq | url-sq | url-dq)
	 * url-nq   := *urlchar
	 * url-sq   := "'" *(urlchar | '"') "'"
	 * url-dq   := '"' *(urlchar | "'") '"'
	 * urlchar  := [#x9#x21#x23-#x26#x28-#x7E] | nonascii
	 * nonascii := [#x80-#xD7FF#xE000-#xFFFD#x10000-#x10FFFF]
	 */

	url = dom_string_data(content);

	/* *LWS */
	while (url < end && ascii_is_space(*url)) {
		url++;
	}

	/* intpart */
	if (url == end || (*url < '0' || '9' < *url)) {
		/* Empty content, or invalid timeval */
		dom_string_unref(content);
		return NSERROR_OK;
	}

	msg_data.delay = (int) strtol(url, &new_url, 10);
	/* a very small delay and self-referencing URL can cause a loop
	 * that grinds machines to a halt. To prevent this we set a
	 * minimum refresh delay of 1s. */
	if (msg_data.delay < 1) {
		msg_data.delay = 1;
	}

	url = new_url;

	/* fracpart? (ignored, as delay is integer only) */
	while (url < end && (('0' <= *url && *url <= '9') ||
			*url == '.')) {
		url++;
	}

	/* *LWS */
	while (url < end && ascii_is_space(*url)) {
		url++;
	}

	/* ';' */
	if (url < end && *url == ';')
		url++;

	/* *LWS */
	while (url < end && ascii_is_space(*url)) {
		url++;
	}

	if (url == end) {
		/* Just delay specified, so refresh current page */
		dom_string_unref(content);

		c->base.refresh = nsurl_ref(content_get_url(&c->base));

		content_broadcast(&c->base, CONTENT_MSG_REFRESH, &msg_data);

		return NSERROR_OK;
	}

	/* "url" */
	if (url <= end - 3) {
		if (strncasecmp(url, "url", 3) == 0) {
			url += 3;
		} else {
			/* Unexpected input, ignore this header */
			dom_string_unref(content);
			return NSERROR_OK;
		}
	} else {
		/* Insufficient input, ignore this header */
		dom_string_unref(content);
		return NSERROR_OK;
	}

	/* *LWS */
	while (url < end && ascii_is_space(*url)) {
		url++;
	}

	/* '=' */
	if (url < end) {
		if (*url == '=') {
			url++;
		} else {
			/* Unexpected input, ignore this header */
			dom_string_unref(content);
			return NSERROR_OK;
		}
	} else {
		/* Insufficient input, ignore this header */
		dom_string_unref(content);
		return NSERROR_OK;
	}

	/* *LWS */
	while (url < end && ascii_is_space(*url)) {
		url++;
	}

	/* '"' or "'" */
	if (url < end && (*url == '"' || *url == '\'')) {
		quote = *url;
		url++;
	}

	/* Start of URL */
	refresh = url;

	if (quote != 0) {
		/* url-sq | url-dq */
		while (url < end && *url != quote)
			url++;
	} else {
		/* url-nq */
		while (url < end && !ascii_is_space(*url))
			url++;
	}

	/* '"' or "'" or *LWS (we don't care) */
	if (url > refresh) {
		/* There's a URL */
		new_url = strndup(refresh, url - refresh);
		if (new_url == NULL) {
			dom_string_unref(content);
			return NSERROR_NOMEM;
		}

		error = nsurl_join(c->base_url, new_url, &nsurl);
		if (error == NSERROR_OK) {
			/* broadcast valid refresh url */

			c->base.refresh = nsurl;

			content_broadcast(&c->base,
					  CONTENT_MSG_REFRESH,
					  &msg_data);
			c->refresh = true;
		}

		free(new_url);

	}

	dom_string_unref(content);

	return error;
}


/**
 * Process title element being inserted into the DOM.
 *
 * https://html.spec.whatwg.org/multipage/semantics.html#the-title-element
 *
 * \param htmlc The html content containing the DOM
 * \param node The DOM node being inserted
 * \return NSERROR_OK on success else appropriate error code
 */
static nserror html_process_inserted_title(html_content *htmlc, dom_node *node)
{
	if (htmlc->title == NULL) {
		/* only the first title is considered */
		htmlc->title = dom_node_ref(node);
	}
	return NSERROR_OK;
}


/** process title node */
static bool html_process_title(html_content *c, dom_node *node)
{
	dom_exception exc; /* returned by libdom functions */
	dom_string *title;
	char *title_str;
	bool success;

	exc = dom_node_get_text_content(node, &title);
	if ((exc != DOM_NO_ERR) || (title == NULL)) {
		return false;
	}

	title_str = squash_whitespace(dom_string_data(title));
	dom_string_unref(title);

	if (title_str == NULL) {
		return false;
	}

	success = content__set_title(&c->base, title_str);

	free(title_str);

	return success;
}


/**
 * Deal with input elements being modified by resyncing their gadget
 * if they have one.
 */
static void html_texty_element_update(html_content *htmlc, dom_node *node)
{
	struct box *box = box_for_node(node);
	if (box == NULL) {
		return; /* No Box (yet?) so no gadget to update */
	}
	if (box->gadget == NULL) {
		return; /* No gadget yet (under construction perhaps?) */
	}
	form_gadget_sync_with_dom(box->gadget);
	/* And schedule a redraw for the box */
	html__redraw_a_box(htmlc, box);
}


/**
 * callback for DOMNodeInserted end type
 */
static void
dom_default_action_DOMNodeInserted_cb(struct dom_event *evt, void *pw)
{
	dom_event_target *node;
	dom_node_type type;
	dom_exception exc;
	html_content *htmlc = pw;

	exc = dom_event_get_target(evt, &node);
	if ((exc != DOM_NO_ERR) || (node == NULL)) {
		/* failed to obtain the event target node */
		return;
	}

	exc = dom_node_get_node_type(node, &type);
	if ((exc == DOM_NO_ERR) && (type == DOM_ELEMENT_NODE)) {
		/* an element node has been inserted */
		dom_html_element_type tag_type;

		exc = dom_html_element_get_tag_type(node, &tag_type);
		if (exc != DOM_NO_ERR) {
			tag_type = DOM_HTML_ELEMENT_TYPE__UNKNOWN;
		}

		switch (tag_type) {
		case DOM_HTML_ELEMENT_TYPE_BASE:
			html_process_inserted_base(htmlc, (dom_node *)node);
			break;

		case DOM_HTML_ELEMENT_TYPE_IMG:
			html_process_inserted_img(htmlc, (dom_node *)node);
			break;

		case DOM_HTML_ELEMENT_TYPE_LINK:
			html_process_inserted_link(htmlc, (dom_node *)node);
			break;

		case DOM_HTML_ELEMENT_TYPE_META:
			html_process_inserted_meta(htmlc, (dom_node *)node);
			break;

		case DOM_HTML_ELEMENT_TYPE_STYLE:
			if (nsoption_bool(author_level_css)) {
				html_css_process_style(htmlc, (dom_node *)node);
			}
			break;

		case DOM_HTML_ELEMENT_TYPE_SCRIPT:
			dom_SCRIPT_showed_up(htmlc,
					     (dom_html_script_element *)node);
			break;

		case DOM_HTML_ELEMENT_TYPE_TITLE:
			html_process_inserted_title(htmlc, (dom_node *)node);
			break;

		default:
			break;
		}

		if (htmlc->enable_scripting) {
			/* ensure javascript context is available */
			if (htmlc->js_thread == NULL) {
				union content_msg_data msg_data;

				msg_data.jsthread = &htmlc->js_thread;
				content_broadcast(&htmlc->base,
						  CONTENT_MSG_GETTHREAD,
						  &msg_data);
				NSLOG(netsurf, INFO,
				      "javascript context: %p (htmlc: %p)",
				      htmlc->js_thread,
				      htmlc);
			}
			if (htmlc->js_thread != NULL) {
				js_handle_new_element(htmlc->js_thread,
						      (dom_element *) node);
			}
		}
	}
	dom_node_unref(node);
}


/**
 * callback for DOMNodeInsertedIntoDocument end type
 */
static void
dom_default_action_DOMNodeInsertedIntoDocument_cb(struct dom_event *evt,
						  void *pw)
{
	html_content *htmlc = pw;
	dom_event_target *node;
	dom_node_type type;
	dom_exception exc;

	exc = dom_event_get_target(evt, &node);
	if ((exc == DOM_NO_ERR) && (node != NULL)) {
		exc = dom_node_get_node_type(node, &type);
		if ((exc == DOM_NO_ERR) && (type == DOM_ELEMENT_NODE)) {
			/* an element node has been modified */
			dom_html_element_type tag_type;

			exc = dom_html_element_get_tag_type(node, &tag_type);
			if (exc != DOM_NO_ERR) {
				tag_type = DOM_HTML_ELEMENT_TYPE__UNKNOWN;
			}

			switch (tag_type) {
			case DOM_HTML_ELEMENT_TYPE_SCRIPT:
				dom_SCRIPT_showed_up(htmlc, (dom_html_script_element *) node);
				fallthrough;
			default:
				break;
			}
		}
		dom_node_unref(node);
	}
}


/**
 * callback for DOMSubtreeModified end type
 */
static void
dom_default_action_DOMSubtreeModified_cb(struct dom_event *evt, void *pw)
{
	dom_event_target *node;
	dom_node_type type;
	dom_exception exc;
	html_content *htmlc = pw;

	exc = dom_event_get_target(evt, &node);
	if ((exc == DOM_NO_ERR) && (node != NULL)) {
		if (htmlc->title == (dom_node *)node) {
			/* Node is our title node */
			html_process_title(htmlc, (dom_node *)node);
			dom_node_unref(node);
			return;
		}

		exc = dom_node_get_node_type(node, &type);
		if ((exc == DOM_NO_ERR) && (type == DOM_ELEMENT_NODE)) {
			/* an element node has been modified */
			dom_html_element_type tag_type;

			exc = dom_html_element_get_tag_type(node, &tag_type);
			if (exc != DOM_NO_ERR) {
				tag_type = DOM_HTML_ELEMENT_TYPE__UNKNOWN;
			}

			switch (tag_type) {
			case DOM_HTML_ELEMENT_TYPE_STYLE:
				if (nsoption_bool(author_level_css)) {
					html_css_update_style(htmlc,
							(dom_node *)node);
				}
				break;
			case DOM_HTML_ELEMENT_TYPE_TEXTAREA:
			case DOM_HTML_ELEMENT_TYPE_INPUT:
				html_texty_element_update(htmlc, (dom_node *)node);
				fallthrough;
			default:
				break;
			}
		}
		dom_node_unref(node);
	}
}


/**
 * callback for default action finished
 */
static void
dom_default_action_finished_cb(struct dom_event *evt, void *pw)
{
	html_content *htmlc = pw;

	if (htmlc->js_thread != NULL)
		js_event_cleanup(htmlc->js_thread, evt);
}


/* exported interface documented in html/dom_event.c */
dom_default_action_callback
html_dom_event_fetcher(dom_string *type,
		  dom_default_action_phase phase,
		  void **pw)
{
	NSLOG(netsurf, DEEPDEBUG,
	      "phase:%d type:%s", phase, dom_string_data(type));

	if (phase == DOM_DEFAULT_ACTION_END) {
		if (dom_string_isequal(type, corestring_dom_DOMNodeInserted)) {
			return dom_default_action_DOMNodeInserted_cb;
		} else if (dom_string_isequal(type, corestring_dom_DOMNodeInsertedIntoDocument)) {
			return dom_default_action_DOMNodeInsertedIntoDocument_cb;
		} else if (dom_string_isequal(type, corestring_dom_DOMSubtreeModified)) {
			return dom_default_action_DOMSubtreeModified_cb;
		}
	} else if (phase == DOM_DEFAULT_ACTION_FINISHED) {
		return dom_default_action_finished_cb;
	}
	return NULL;
}
