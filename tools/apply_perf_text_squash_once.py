#!/usr/bin/env python3
from pathlib import Path
p=Path('browser/netsurf/content/handlers/html/box_construct.c')
s=p.read_text()

marker='''/**\n * Construct the box tree for an XML text node.\n'''
assert marker in s
helper=r'''/* Collapse HTML normal-flow ASCII whitespace directly into the final talloc
 * buffer. squash_whitespace() used to malloc(strlen+1), then the caller did a
 * second strlen + talloc copy before freeing that temporary buffer. */
static char *
box_squash_whitespace_talloc(void *bctx, const char *src, size_t src_len,
		size_t *out_len)
{
	char *dst;
	size_t i;
	size_t j;

	if (src == NULL || out_len == NULL)
		return NULL;
	dst = talloc_size(bctx, src_len + 1);
	if (dst == NULL)
		return NULL;

	i = 0;
	j = 0;
	while (i < src_len) {
		if (src[i] == ' ' || src[i] == '\n' ||
				src[i] == '\r' || src[i] == '\t') {
			dst[j++] = ' ';
			do {
				i++;
			} while (i < src_len &&
					(src[i] == ' ' || src[i] == '\n' ||
					 src[i] == '\r' || src[i] == '\t'));
		} else {
			dst[j++] = src[i++];
		}
	}
	dst[j] = '\0';
	*out_len = j;
	return dst;
}


'''
s=s.replace(marker,helper+marker,1)

old='''\t\tchar *text;\n\t\tsize_t text_len;\n\n\t\ttext = squash_whitespace(dom_string_data(content));\n\n\t\tdom_string_unref(content);\n\n\t\tif (text == NULL)\n\t\t\treturn false;\n\n\t\ttext_len = strlen(text);\n\n'''
new='''\t\tchar *text;\n\t\tconst char *raw_text;\n\t\tsize_t raw_len;\n\t\tsize_t text_len;\n\n\t\traw_text = dom_string_data(content);\n\t\traw_len = dom_string_byte_length(content);\n\t\ttext = box_squash_whitespace_talloc(ctx->bctx, raw_text, raw_len,\n\t\t\t\t&text_len);\n\t\tdom_string_unref(content);\n\n\t\tif (text == NULL)\n\t\t\treturn false;\n\n'''
assert old in s
s=s.replace(old,new,1)

# Work only inside the normal/nowrap half so the pre branch keeps its malloc.
branch_start=s.index('\tif (css_computed_white_space(props.parent_style) ==')
branch_end=s.index('\n\t} else {\n\t\t/* white-space: pre */',branch_start)
segment=s[branch_start:branch_end]
segment=segment.replace('free(text);','talloc_free(text);')
old_final='''\t\tbox->text = talloc_memdup(ctx->bctx, text, text_len + 1);\n\t\ttalloc_free(text);\n\t\tif (box->text == NULL)\n\t\t\treturn false;\n\n\t\tbox->length = text_len;\n'''
new_final='''\t\t/* text is already the final talloc-owned buffer. */\n\t\tbox->text = text;\n\t\tbox->length = text_len;\n'''
assert old_final in segment
segment=segment.replace(old_final,new_final,1)
s=s[:branch_start]+segment+s[branch_end:]
p.write_text(s)
