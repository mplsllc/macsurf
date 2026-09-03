#!/usr/bin/env python3
from pathlib import Path
p = Path('browser/netsurf/content/handlers/html/box_construct.c')
s = p.read_text()

old = '''\tconst css_computed_style *root_style = NULL;\n\tbool is_svg = false;\n'''
new = '''\tconst css_computed_style *root_style = NULL;\n\tbool is_svg = false;\n\tbool is_table_cell_tag = false;\n'''
assert old in s
s = s.replace(old, new, 1)

old = '''\t\t\tif (c0 >= 'A' && c0 <= 'Z')\n\t\t\t\tc0 = (unsigned char)(c0 + ('a' - 'A'));\n\n\t\t\tif (tlen == 3 && c0 == 's') {\n'''
new = '''\t\t\tif (c0 >= 'A' && c0 <= 'Z')\n\t\t\t\tc0 = (unsigned char)(c0 + ('a' - 'A'));\n\n\t\t\tif (tlen == 2 && c0 == 't') {\n\t\t\t\tunsigned char c1 = (unsigned char)tag[1];\n\t\t\t\tif (c1 >= 'A' && c1 <= 'Z')\n\t\t\t\t\tc1 = (unsigned char)(c1 + ('a' - 'A'));\n\t\t\t\tis_table_cell_tag = (c1 == 'd' || c1 == 'h');\n\t\t\t} else if (tlen == 3 && c0 == 's') {\n'''
assert old in s
s = s.replace(old, new, 1)

start = s.index('\t/* Deal with colspan/rowspan */')
end = s.index('\n\n\tcss_display = ns_computed_display_static(box->style);', start)
old_block = s[start:end]
new_block = r'''	/* colspan/rowspan are table-cell attributes.  The old path queried both
	 * attributes on every element in the document, even though they have no
	 * HTML semantics outside <td>/<th>.  Tag classification above is already
	 * paid for, so ordinary elements skip two DOM attribute-map lookups. */
	if (is_table_cell_tag) {
		err = dom_element_get_attribute(ctx->n, corestring_dom_colspan, &s);
		if (err != DOM_NO_ERR)
			return false;

		if (s != NULL) {
			const char *val = dom_string_data(s);

			/* Convert to a number, clamping to [1,1000] according to 4.9.11 */
			if ('0' <= val[0] && val[0] <= '9')
				box->columns = clamp(strtol(val, NULL, 10), 1, 1000);

			dom_string_unref(s);
		}

		err = dom_element_get_attribute(ctx->n, corestring_dom_rowspan, &s);
		if (err != DOM_NO_ERR)
			return false;

		if (s != NULL) {
			const char *val = dom_string_data(s);

			/* Convert to a number, clamping to [0,65534] according to 4.9.11 */
			if ('0' <= val[0] && val[0] <= '9')
				box->rows = clamp(strtol(val, NULL, 10), 0, 65534);

			dom_string_unref(s);
		}
	}'''
s = s[:start] + new_block + s[end:]
p.write_text(s)
