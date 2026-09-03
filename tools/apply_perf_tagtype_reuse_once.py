#!/usr/bin/env python3
from pathlib import Path

# 1) Shared compat declaration.
hp = Path('browser/netsurf/content/handlers/html/macsurf_dom_compat.h')
h = hp.read_text()
needle = '''dom_exception macsurf_html_element_get_tag_type(const void *node,\n\t\tdom_html_element_type *type);\n'''
assert needle in h
repl = '''/* Map an already-fetched tag-name string without another DOM lookup. */\ndom_exception macsurf_html_tag_name_get_type(const dom_string *tag,\n\t\tdom_html_element_type *type);\n\ndom_exception macsurf_html_element_get_tag_type(const void *node,\n\t\tdom_html_element_type *type);\n'''
h = h.replace(needle, repl, 1)
hp.write_text(h)

# 2) Replace compatibility mapper with a dispatched mapper, retaining the
# element-get wrapper for unrelated call sites.
cp = Path('browser/netsurf/content/handlers/html/macsurf_dom_compat.c')
c = cp.read_text()
start = c.index('dom_exception\nmacsurf_html_element_get_tag_type(')
new_tail = r'''static unsigned char
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
	len = dom_string_length(tag);
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
'''
c = c[:start] + new_tail
cp.write_text(c)

# 3) Special dispatcher accepts already-resolved tag type.
shp = Path('browser/netsurf/content/handlers/html/box_special.h')
sh = shp.read_text()
old = 'bool convert_special_elements(dom_node *node, html_content *content, struct box *box, bool *convert_children);\n'
new = '''bool convert_special_elements(dom_node *node, html_content *content,\n\t\tstruct box *box, bool *convert_children,\n\t\tdom_html_element_type tag_type);\n'''
assert old in sh
sh = sh.replace(old, new, 1)
shp.write_text(sh)

sp = Path('browser/netsurf/content/handlers/html/box_special.c')
special = sp.read_text()
old = '''bool\nconvert_special_elements(dom_node *node,\n\t\t\t html_content *content,\n\t\t\t struct box *box,\n\t\t\t bool *convert_children)\n{\n\tdom_exception exc;\n\tdom_html_element_type tag_type;\n\tbool res;\n\n\texc = macsurf_html_element_get_tag_type(node, &tag_type);\n\tif (exc != DOM_NO_ERR) {\n\t\ttag_type = DOM_HTML_ELEMENT_TYPE__UNKNOWN;\n\t}\n\n'''
new = '''bool\nconvert_special_elements(dom_node *node,\n\t\t\t html_content *content,\n\t\t\t struct box *box,\n\t\t\t bool *convert_children,\n\t\t\t dom_html_element_type tag_type)\n{\n\tbool res;\n\n'''
assert old in special
special = special.replace(old, new, 1)
sp.write_text(special)

# 4) Box construction resolves tag type from the tag string it already owns,
# uses it for metadata, then passes it to special conversion.
bp = Path('browser/netsurf/content/handlers/html/box_construct.c')
b = bp.read_text()
old = '''\tbool is_svg = false;\n\tbool is_table_cell_tag = false;\n'''
new = '''\tbool is_svg = false;\n\tbool is_table_cell_tag = false;\n\tdom_html_element_type tag_type = DOM_HTML_ELEMENT_TYPE__UNKNOWN;\n'''
assert old in b
b = b.replace(old, new, 1)

needle = '''\t\t\tif (c0 >= 'A' && c0 <= 'Z')\n\t\t\t\tc0 = (unsigned char)(c0 + ('a' - 'A'));\n\n'''
assert needle in b
b = b.replace(needle, needle + '''\t\t\t(void)macsurf_html_tag_name_get_type(tag_name, &tag_type);\n\n''', 1)

# Replace metadata comparisons with enum dispatch plus only the two names not
# needed by the shared compatibility mapper (base/head).
start = b.index("\t\t\tif (tlen == 2 && c0 == 't') {")
end = b.index('\n\n\t\t\tdom_string_unref(tag_name);', start)
old_class = b[start:end]
new_class = r'''			if (tlen == 2 && c0 == 't') {
				unsigned char c1 = (unsigned char)tag[1];
				if (c1 >= 'A' && c1 <= 'Z')
					c1 = (unsigned char)(c1 + ('a' - 'A'));
				is_table_cell_tag = (c1 == 'd' || c1 == 'h');
			}
			if (tlen == 3 && c0 == 's') {
				is_svg = dom_string_caseless_lwc_isequal(
					tag_name, corestring_lwc_svg);
			}

			switch (tag_type) {
			case DOM_HTML_ELEMENT_TYPE_STYLE:
			case DOM_HTML_ELEMENT_TYPE_TITLE:
			case DOM_HTML_ELEMENT_TYPE_META:
			case DOM_HTML_ELEMENT_TYPE_LINK:
				skip = true;
				break;
			default:
				break;
			}
			if (!skip && tlen == 4 && c0 == 'b') {
				skip = dom_string_caseless_lwc_isequal(
					tag_name, corestring_lwc_base);
			} else if (!skip && tlen == 4 && c0 == 'h') {
				skip = dom_string_caseless_lwc_isequal(
					tag_name, corestring_lwc_head);
			}'''
b = b[:start] + new_class + b[end:]

old = '''\tif (convert_special_elements(ctx->n,\n\t\t\t\t     ctx->content,\n\t\t\t\t     box,\n\t\t\t\t     convert_children) == false) {\n'''
new = '''\tif (convert_special_elements(ctx->n,\n\t\t\t\t     ctx->content,\n\t\t\t\t     box,\n\t\t\t\t     convert_children,\n\t\t\t\t     tag_type) == false) {\n'''
assert old in b
b = b.replace(old, new, 1)
bp.write_text(b)
