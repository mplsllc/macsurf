#!/usr/bin/env python3
from pathlib import Path
p = Path('browser/netsurf/content/handlers/html/box_construct.c')
s = p.read_text()

old = '''\tdom_exception err;\n\tstruct box_construct_props props;\n\tconst css_computed_style *root_style = NULL;\n'''
new = '''\tdom_exception err;\n\tstruct box_construct_props props;\n\tconst css_computed_style *root_style = NULL;\n\tbool is_svg = false;\n'''
assert old in s
s = s.replace(old, new, 1)

start = s.index('\t/* Skip non-rendered metadata elements unconditionally')
end = s.index('\n\n\tbox_extract_properties(ctx->n, &props);', start)
old_block = s[start:end]
new_block = r'''	/* Classify the tag once.  The old path fetched every element's tag name
	 * here for metadata rejection, tried up to six caseless comparisons, then
	 * fetched the same immutable tag name again later solely to detect <svg>.
	 * Length + first-byte dispatch leaves ordinary div/span/etc. at zero
	 * caseless comparisons and carries the SVG result forward. */
	{
		dom_string *tag_name = NULL;
		if (dom_element_get_tag_name(ctx->n, &tag_name) == DOM_NO_ERR &&
				tag_name != NULL) {
			const char *tag = (const char *)dom_string_data(tag_name);
			size_t tlen = dom_string_length(tag_name);
			unsigned char c0 = (tlen != 0) ? (unsigned char)tag[0] : 0;
			bool skip = false;

			if (c0 >= 'A' && c0 <= 'Z')
				c0 = (unsigned char)(c0 + ('a' - 'A'));

			if (tlen == 3 && c0 == 's') {
				is_svg = dom_string_caseless_lwc_isequal(
					tag_name, corestring_lwc_svg);
			} else if (tlen == 4) {
				switch (c0) {
				case 'm':
					skip = dom_string_caseless_lwc_isequal(
						tag_name, corestring_lwc_meta);
					break;
				case 'l':
					skip = dom_string_caseless_lwc_isequal(
						tag_name, corestring_lwc_link);
					break;
				case 'b':
					skip = dom_string_caseless_lwc_isequal(
						tag_name, corestring_lwc_base);
					break;
				case 'h':
					skip = dom_string_caseless_lwc_isequal(
						tag_name, corestring_lwc_head);
					break;
				default:
					break;
				}
			} else if (tlen == 5) {
				if (c0 == 's') {
					skip = dom_string_caseless_lwc_isequal(
						tag_name, corestring_lwc_style);
				} else if (c0 == 't') {
					skip = dom_string_caseless_lwc_isequal(
						tag_name, corestring_lwc_title);
				}
			}

			dom_string_unref(tag_name);
			if (skip) {
				*convert_children = false;
				return true;
			}
		}
	}'''
s = s[:start] + new_block + s[end:]

start = s.index('\t/* fixes195 - inline <svg> root detection.')
end = s.index('\n\n\tif (*convert_children)', start)
old_svg = s[start:end]
new_svg = r'''	/* Inline SVG classification was already done with the metadata tag lookup
	 * above.  Avoid a second dom_element_get_tag_name + string comparison for
	 * every constructed element. */
	if (is_svg) {
		box->flags |= SVG_INLINE | IS_REPLACED | REPLACE_DIM;
		*convert_children = false;
	}'''
s = s[:start] + new_svg + s[end:]

p.write_text(s)
