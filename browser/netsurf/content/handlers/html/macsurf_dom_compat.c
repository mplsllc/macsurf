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

static unsigned char
macsurf_tag_lower(unsigned char c)
{
	if (c >= 'A' && c <= 'Z')
		return (unsigned char)(c + ('a' - 'A'));
	return c;
}

static int
macsurf_tag_equal(const char *name, size_t len, const char *lit)
{
	size_t i;

	if (name == NULL || lit == NULL)
		return 0;
	for (i = 0; i < len; i++) {
		if (lit[i] == '\0' ||
				macsurf_tag_lower((unsigned char)name[i]) !=
				macsurf_tag_lower((unsigned char)lit[i]))
			return 0;
	}
	return lit[len] == '\0';
}

dom_exception
macsurf_html_tag_name_get_type(const dom_string *tag,
		dom_html_element_type *type)
{
	const char *name;
	size_t len;
	unsigned char c0;
	unsigned char c1;

	if (type == NULL)
		return DOM_NO_ERR;
	*type = DOM_HTML_ELEMENT_TYPE__UNKNOWN;
	if (tag == NULL)
		return DOM_NO_ERR;

	name = dom_string_data(tag);
	if (name == NULL)
		return DOM_NO_ERR;
	len = dom_string_byte_length(tag);
	if (len == 0)
		return DOM_NO_ERR;
	c0 = macsurf_tag_lower((unsigned char)name[0]);
	c1 = (len > 1) ? macsurf_tag_lower((unsigned char)name[1]) : 0;

	switch (len) {
	case 1:
		if (c0 == 'a' && macsurf_tag_equal(name, len, "a"))
			*type = DOM_HTML_ELEMENT_TYPE_A;
		break;
	case 2:
		if (c0 == 'b' && macsurf_tag_equal(name, len, "br"))
			*type = DOM_HTML_ELEMENT_TYPE_BR;
		else if (c0 == 'l' && macsurf_tag_equal(name, len, "li"))
			*type = DOM_HTML_ELEMENT_TYPE_LI;
		else if (c0 == 'o' && macsurf_tag_equal(name, len, "ol"))
			*type = DOM_HTML_ELEMENT_TYPE_OL;
		else if (c0 == 'u' && macsurf_tag_equal(name, len, "ul"))
			*type = DOM_HTML_ELEMENT_TYPE_UL;
		break;
	case 3:
		if (c0 == 'i' && macsurf_tag_equal(name, len, "img"))
			*type = DOM_HTML_ELEMENT_TYPE_IMG;
		else if (c0 == 'p' && macsurf_tag_equal(name, len, "pre"))
			*type = DOM_HTML_ELEMENT_TYPE_PRE;
		break;
	case 4:
		if (c0 == 'b' && macsurf_tag_equal(name, len, "body"))
			*type = DOM_HTML_ELEMENT_TYPE_BODY;
		else if (c0 == 'h' && macsurf_tag_equal(name, len, "html"))
			*type = DOM_HTML_ELEMENT_TYPE_HTML;
		else if (c0 == 'l' && macsurf_tag_equal(name, len, "link"))
			*type = DOM_HTML_ELEMENT_TYPE_LINK;
		else if (c0 == 'm' && macsurf_tag_equal(name, len, "meta"))
			*type = DOM_HTML_ELEMENT_TYPE_META;
		break;
	case 5:
		if (c0 == 'e' && macsurf_tag_equal(name, len, "embed"))
			*type = DOM_HTML_ELEMENT_TYPE_EMBED;
		else if (c0 == 'i' && macsurf_tag_equal(name, len, "input"))
			*type = DOM_HTML_ELEMENT_TYPE_INPUT;
		else if (c0 == 's' && macsurf_tag_equal(name, len, "style"))
			*type = DOM_HTML_ELEMENT_TYPE_STYLE;
		else if (c0 == 't' && macsurf_tag_equal(name, len, "title"))
			*type = DOM_HTML_ELEMENT_TYPE_TITLE;
		break;
	case 6:
		if (c0 == 'b' && macsurf_tag_equal(name, len, "button"))
			*type = DOM_HTML_ELEMENT_TYPE_BUTTON;
		else if (c0 == 'c' && macsurf_tag_equal(name, len, "canvas"))
			*type = DOM_HTML_ELEMENT_TYPE_CANVAS;
		else if (c0 == 'i' && macsurf_tag_equal(name, len, "iframe"))
			*type = DOM_HTML_ELEMENT_TYPE_IFRAME;
		else if (c0 == 'o' && macsurf_tag_equal(name, len, "object"))
			*type = DOM_HTML_ELEMENT_TYPE_OBJECT;
		else if (c0 == 's' && c1 == 'c' &&
				macsurf_tag_equal(name, len, "script"))
			*type = DOM_HTML_ELEMENT_TYPE_SCRIPT;
		else if (c0 == 's' && c1 == 'e' &&
				macsurf_tag_equal(name, len, "select"))
			*type = DOM_HTML_ELEMENT_TYPE_SELECT;
		break;
	case 8:
		if (c0 == 'f' && macsurf_tag_equal(name, len, "frameset"))
			*type = DOM_HTML_ELEMENT_TYPE_FRAMESET;
		else if (c0 == 'n' && macsurf_tag_equal(name, len, "noscript"))
			*type = DOM_HTML_ELEMENT_TYPE_NOSCRIPT;
		else if (c0 == 't' && macsurf_tag_equal(name, len, "textarea"))
			*type = DOM_HTML_ELEMENT_TYPE_TEXTAREA;
		break;
	default:
		break;
	}

	return DOM_NO_ERR;
}

dom_exception
macsurf_html_element_get_tag_type(const void *node, dom_html_element_type *type)
{
	dom_string *tag;
	dom_exception exc;
	dom_element *el;

	if (type == NULL)
		return DOM_NO_ERR;
	*type = DOM_HTML_ELEMENT_TYPE__UNKNOWN;
	if (node == NULL)
		return DOM_NO_ERR;

	el = (dom_element *)node;
	tag = NULL;
	exc = dom_element_get_tag_name(el, &tag);
	if (exc != DOM_NO_ERR || tag == NULL)
		return exc;

	exc = macsurf_html_tag_name_get_type(tag, type);
	dom_string_unref(tag);
	return exc;
}
