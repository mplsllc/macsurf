/*
 * fixes1268a (#167) - see cssprobe.h. Linux harness only.
 *
 * This file includes libcss's private headers and MUST NOT include any
 * netsurf header, or the enumerator collisions this file exists to avoid
 * come straight back.
 */
#include <string.h>

#include <libcss/libcss.h>

#include "css_internal_stylesheet.h"
#include "parse/custom_properties.h"

#include "cssprobe.h"

/* Flatten a custom-property value's token run into printable text. */
static void cssprobe_tokens_text(const css_cp_token *toks, uint32_t n,
		char *buf, size_t buflen)
{
	size_t out = 0;
	uint32_t i;

	if (buf == NULL || buflen == 0)
		return;
	buf[0] = '\0';

	for (i = 0; i < n; i++) {
		const char *d;
		size_t len, k;

		if (toks[i].idata == NULL)
			continue;
		d = lwc_string_data(toks[i].idata);
		len = lwc_string_length(toks[i].idata);
		if (d == NULL)
			continue;
		for (k = 0; k < len; k++) {
			if (out + 1 >= buflen) {
				buf[out] = '\0';
				return;
			}
			buf[out++] = d[k];
		}
	}
	buf[out] = '\0';
}

/* Custom-property names arrive carrying one or two leading dashes
 * depending on which lexer path produced them (see the normalising
 * comment on cp_name_equal in custom_properties.c). Compare on the
 * undashed tail. */
static int cssprobe_name_is(lwc_string *have, const char *want)
{
	const char *d;
	size_t len;

	if (have == NULL || want == NULL)
		return 0;
	d = lwc_string_data(have);
	len = lwc_string_length(have);
	if (d == NULL)
		return 0;
	while (len > 0 && *d == '-') {
		d++;
		len--;
	}
	return (len == strlen(want) && strncmp(d, want, len) == 0);
}

static void cssprobe_walk(css_rule *rule, const char *name,
		char vals[][64], int *n, int max)
{
	for (; rule != NULL; rule = rule->next) {
		css_style *st;
		const css_deferred_decl *e;

		/* Descend into @media: the whole point of rule-scoping is
		 * that a definition inside a non-matching media block stays
		 * attached to a rule the cascade can decline to apply. */
		if (rule->type == CSS_RULE_MEDIA) {
			cssprobe_walk(((css_rule_media *)rule)->first_child,
					name, vals, n, max);
			continue;
		}
		if (rule->type != CSS_RULE_SELECTOR)
			continue;

		st = ((css_rule_selector *)rule)->style;
		if (st == NULL)
			continue;

		/* fixes1269 - definitions live on the DEFERRED list, told
		 * apart from ordinary var() consumers by the leading "--".
		 * They used to have a css_style field of their own; that
		 * grew sizeof(css_style) and had to be withdrawn. */
		for (e = st->deferred; e != NULL; e = e->next) {
			if (*n >= max)
				return;
			if (!css__cp_decl_is_definition(e))
				continue;
			if (!cssprobe_name_is(e->property, name))
				continue;
			cssprobe_tokens_text(e->tokens, e->n_tokens,
					vals[*n], 64);
			(*n)++;
		}
	}
}

int cssprobe_rule_custom_props(struct css_stylesheet *sheet,
		const char *name, char vals[][64], int max)
{
	int n = 0;

	if (sheet == NULL)
		return 0;
	cssprobe_walk(sheet->rule_list, name, vals, &n, max);
	return n;
}

int cssprobe_sheet_custom_props(struct css_stylesheet *sheet,
		const char *name)
{
	const css_cp_entry *e;
	int n = 0;

	if (sheet == NULL)
		return 0;
	for (e = sheet->custom_properties; e != NULL; e = e->next) {
		if (cssprobe_name_is(e->name, name))
			n++;
	}
	return n;
}
