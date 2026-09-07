/*
 * MacSurf - Mac OS 9 frontend for NetSurf
 * macsurf_dom_compat.c - CW8 CFM broken-vtable workaround for tag-type lookup.
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 *
 * See macsurf_dom_compat.h for the root cause (DOM HTML vtable slot 75 is a
 * broken CW8 CFM transition vector that crashes through LNGInitFonts). This
 * resolves the same dom_html_element_type via the WORKING tag-name slot:
 *   dom_element_get_tag_name() -> case-insensitive name match -> enum.
 * Only the ~24 tags the call sites actually switch on are mapped; anything
 * else (and any error) returns DOM_HTML_ELEMENT_TYPE__UNKNOWN, which every
 * call site already handles in its default path. C89 / CW8-clean.
 */

#include <string.h>
#include <stdbool.h>

#include <dom/dom.h>

#include "html/macsurf_dom_compat.h"
#include "macsurf_debug.h"

dom_exception
macsurf_html_element_get_tag_type(const void *node, dom_html_element_type *type)
{
	dom_string *tag;
	dom_exception exc;
	const char *name;
	dom_html_element_type t;
	dom_element *el;

	*type = DOM_HTML_ELEMENT_TYPE__UNKNOWN;

	if (node == NULL) {
		return DOM_NO_ERR;
	}

	/* dom_element_get_tag_name takes a non-const dom_element *; the const
	 * is dropped by the same explicit cast the original macro used. */
	el = (dom_element *) node;

	tag = NULL;
	exc = dom_element_get_tag_name(el, &tag);
	if (exc != DOM_NO_ERR || tag == NULL) {
		/* leave *type = __UNKNOWN; mirror the get_tag_type error path */
		return exc;
	}

	name = dom_string_data(tag);
	t = DOM_HTML_ELEMENT_TYPE__UNKNOWN;

	if (name != NULL) {
		if (strcasecmp(name, "a") == 0)
			t = DOM_HTML_ELEMENT_TYPE_A;
		else if (strcasecmp(name, "body") == 0)
			t = DOM_HTML_ELEMENT_TYPE_BODY;
		else if (strcasecmp(name, "html") == 0)
			t = DOM_HTML_ELEMENT_TYPE_HTML;
		else if (strcasecmp(name, "br") == 0)
			t = DOM_HTML_ELEMENT_TYPE_BR;
		else if (strcasecmp(name, "button") == 0)
			t = DOM_HTML_ELEMENT_TYPE_BUTTON;
		else if (strcasecmp(name, "canvas") == 0)
			t = DOM_HTML_ELEMENT_TYPE_CANVAS;
		else if (strcasecmp(name, "embed") == 0)
			t = DOM_HTML_ELEMENT_TYPE_EMBED;
		else if (strcasecmp(name, "frameset") == 0)
			t = DOM_HTML_ELEMENT_TYPE_FRAMESET;
		else if (strcasecmp(name, "iframe") == 0)
			t = DOM_HTML_ELEMENT_TYPE_IFRAME;
		else if (strcasecmp(name, "img") == 0)
			t = DOM_HTML_ELEMENT_TYPE_IMG;
		else if (strcasecmp(name, "input") == 0)
			t = DOM_HTML_ELEMENT_TYPE_INPUT;
		else if (strcasecmp(name, "li") == 0)
			t = DOM_HTML_ELEMENT_TYPE_LI;
		else if (strcasecmp(name, "link") == 0)
			t = DOM_HTML_ELEMENT_TYPE_LINK;
		else if (strcasecmp(name, "meta") == 0)
			t = DOM_HTML_ELEMENT_TYPE_META;
		else if (strcasecmp(name, "noscript") == 0)
			t = DOM_HTML_ELEMENT_TYPE_NOSCRIPT;
		else if (strcasecmp(name, "object") == 0)
			t = DOM_HTML_ELEMENT_TYPE_OBJECT;
		else if (strcasecmp(name, "ol") == 0)
			t = DOM_HTML_ELEMENT_TYPE_OL;
		else if (strcasecmp(name, "pre") == 0)
			t = DOM_HTML_ELEMENT_TYPE_PRE;
		else if (strcasecmp(name, "script") == 0)
			t = DOM_HTML_ELEMENT_TYPE_SCRIPT;
		else if (strcasecmp(name, "select") == 0)
			t = DOM_HTML_ELEMENT_TYPE_SELECT;
		else if (strcasecmp(name, "style") == 0)
			t = DOM_HTML_ELEMENT_TYPE_STYLE;
		else if (strcasecmp(name, "textarea") == 0)
			t = DOM_HTML_ELEMENT_TYPE_TEXTAREA;
		else if (strcasecmp(name, "title") == 0)
			t = DOM_HTML_ELEMENT_TYPE_TITLE;
		else if (strcasecmp(name, "ul") == 0)
			t = DOM_HTML_ELEMENT_TYPE_UL;
	}

	/* Probe: log the first N resolutions so a hardware run confirms the
	 * working slot is being exercised and the slot-75 path is gone. */
	{
		static long dbg_count = 0;
		if (dbg_count < 40) {
			dbg_count++;
			macsurf_debug_log_writef("tagtype: %s -> %d",
				(name != NULL) ? name : "(null)", (int) t);
		}
	}

	dom_string_unref(tag);

	*type = t;
	return DOM_NO_ERR;
}
