/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 The MacSurf Project.
 *
 * CSS Custom Properties and var() deferred resolution. See the header
 * for the architectural summary.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "stylesheet.h"
#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "lex/lex.h"
#include "parse/custom_properties.h"
#include "parse/important.h"
#include "parse/language.h"
#include "parse/propstrings.h"
#include "parse/properties/properties.h"
#include "select/dispatch.h"
#include "select/select.h"

#define MAX_VAR_DEPTH 10


/* ------------------------------------------------------------------ */
/* Token list helpers                                                 */
/* ------------------------------------------------------------------ */

css_error css__cp_tokens_from_vector(const parserutils_vector *vec,
		int32_t start, int32_t end,
		css_cp_token **out_tokens, uint32_t *out_n)
{
	int32_t i;
	uint32_t count;
	css_cp_token *arr;

	if (out_tokens == NULL || out_n == NULL)
		return CSS_BADPARM;

	*out_tokens = NULL;
	*out_n = 0;

	if (end <= start)
		return CSS_OK;

	count = (uint32_t)(end - start);
	arr = (css_cp_token *)calloc(count, sizeof(css_cp_token));
	if (arr == NULL)
		return CSS_NOMEM;

	for (i = 0; i < (int32_t)count; i++) {
		const css_token *t;

		t = (const css_token *)parserutils_vector_peek(vec, start + i);
		if (t == NULL) {
			arr[i].type = CSS_TOKEN_EOF;
			arr[i].idata = NULL;
			continue;
		}

		arr[i].type = (uint16_t)t->type;
		arr[i].col = t->col;
		arr[i].line = t->line;
		if (t->idata != NULL)
			arr[i].idata = lwc_string_ref(t->idata);
		else
			arr[i].idata = NULL;
	}

	*out_tokens = arr;
	*out_n = count;
	return CSS_OK;
}

void css__cp_tokens_destroy(css_cp_token *tokens, uint32_t n)
{
	uint32_t i;

	if (tokens == NULL)
		return;

	for (i = 0; i < n; i++) {
		if (tokens[i].idata != NULL)
			lwc_string_unref(tokens[i].idata);
	}
	free(tokens);
}


/* ------------------------------------------------------------------ */
/* Custom-property list (per stylesheet)                              */
/* ------------------------------------------------------------------ */

css_error css__sheet_add_custom_property(css_stylesheet *sheet,
		lwc_string *name, css_cp_token *tokens, uint32_t n)
{
	css_cp_entry *entry;
	css_cp_entry *cur;
	bool match;
	lwc_error lerr;

	if (sheet == NULL || name == NULL) {
		if (tokens != NULL)
			css__cp_tokens_destroy(tokens, n);
		if (name != NULL)
			lwc_string_unref(name);
		return CSS_BADPARM;
	}

	/* Replace if an earlier entry has the same name. */
	for (cur = sheet->custom_properties; cur != NULL; cur = cur->next) {
		match = false;
		lerr = lwc_string_isequal(cur->name, name, &match);
		if (lerr == lwc_error_ok && match) {
			css__cp_tokens_destroy(cur->tokens, cur->n_tokens);
			cur->tokens = tokens;
			cur->n_tokens = n;
			lwc_string_unref(name);
			return CSS_OK;
		}
	}

	entry = (css_cp_entry *)calloc(1, sizeof(css_cp_entry));
	if (entry == NULL) {
		css__cp_tokens_destroy(tokens, n);
		lwc_string_unref(name);
		return CSS_NOMEM;
	}

	entry->name = name;
	entry->tokens = tokens;
	entry->n_tokens = n;
	entry->next = NULL;

	/* Append at tail so in-sheet source order is preserved. */
	if (sheet->custom_properties == NULL) {
		sheet->custom_properties = entry;
	} else {
		cur = sheet->custom_properties;
		while (cur->next != NULL)
			cur = cur->next;
		cur->next = entry;
	}

	return CSS_OK;
}

const css_cp_entry *css__sheet_find_custom_property(
		const css_stylesheet *sheet, lwc_string *name)
{
	const css_cp_entry *cur;
	bool match;
	lwc_error lerr;

	if (sheet == NULL || name == NULL)
		return NULL;

	for (cur = sheet->custom_properties; cur != NULL; cur = cur->next) {
		match = false;
		lerr = lwc_string_isequal(cur->name, name, &match);
		if (lerr == lwc_error_ok && match)
			return cur;
	}
	return NULL;
}

void css__cp_entry_list_destroy(css_cp_entry *head)
{
	css_cp_entry *cur;
	css_cp_entry *next;

	cur = head;
	while (cur != NULL) {
		next = cur->next;
		css__cp_tokens_destroy(cur->tokens, cur->n_tokens);
		if (cur->name != NULL)
			lwc_string_unref(cur->name);
		free(cur);
		cur = next;
	}
}


/* ------------------------------------------------------------------ */
/* Deferred declaration list (per css_style)                          */
/* ------------------------------------------------------------------ */

css_error css__deferred_decl_create(lwc_string *property,
		css_cp_token *tokens, uint32_t n, bool important,
		css_deferred_decl **out)
{
	css_deferred_decl *dd;

	if (out == NULL || property == NULL) {
		if (tokens != NULL)
			css__cp_tokens_destroy(tokens, n);
		if (property != NULL)
			lwc_string_unref(property);
		return CSS_BADPARM;
	}

	dd = (css_deferred_decl *)calloc(1, sizeof(css_deferred_decl));
	if (dd == NULL) {
		css__cp_tokens_destroy(tokens, n);
		lwc_string_unref(property);
		return CSS_NOMEM;
	}

	dd->property = property;
	dd->tokens = tokens;
	dd->n_tokens = n;
	dd->important = important ? 1 : 0;
	dd->next = NULL;

	*out = dd;
	return CSS_OK;
}

void css__deferred_decl_attach(css_style *style, css_deferred_decl *dd)
{
	css_deferred_decl *tail;

	if (style == NULL || dd == NULL)
		return;

	if (style->deferred == NULL) {
		style->deferred = dd;
		return;
	}
	tail = style->deferred;
	while (tail->next != NULL)
		tail = tail->next;
	tail->next = dd;
}

void css__deferred_decl_list_destroy(css_deferred_decl *head)
{
	css_deferred_decl *cur;
	css_deferred_decl *next;

	cur = head;
	while (cur != NULL) {
		next = cur->next;
		css__cp_tokens_destroy(cur->tokens, cur->n_tokens);
		if (cur->property != NULL)
			lwc_string_unref(cur->property);
		free(cur);
		cur = next;
	}
}


/* ------------------------------------------------------------------ */
/* var() detection                                                    */
/* ------------------------------------------------------------------ */

static bool token_is_var_function(const css_token *t)
{
	const char *s;
	size_t len;

	if (t == NULL || t->type != CSS_TOKEN_FUNCTION || t->idata == NULL)
		return false;

	len = lwc_string_length(t->idata);
	if (len != 3)
		return false;

	s = lwc_string_data(t->idata);
	return ((s[0] == 'v' || s[0] == 'V') &&
		(s[1] == 'a' || s[1] == 'A') &&
		(s[2] == 'r' || s[2] == 'R'));
}

static bool token_is_char(const css_token *t, char ch)
{
	if (t == NULL || t->type != CSS_TOKEN_CHAR || t->idata == NULL)
		return false;
	if (lwc_string_length(t->idata) != 1)
		return false;
	return lwc_string_data(t->idata)[0] == ch;
}

bool css__value_contains_var(const parserutils_vector *vec, int32_t start)
{
	int32_t i;
	const css_token *t;

	i = start;
	for (;;) {
		t = (const css_token *)parserutils_vector_peek(vec, i);
		if (t == NULL)
			return false;
		if (token_is_var_function(t))
			return true;
		i++;
	}
}

bool css__is_custom_property_ident(const css_token *ident)
{
	const char *s;
	size_t len;

	if (ident == NULL || ident->type != CSS_TOKEN_IDENT ||
			ident->idata == NULL)
		return false;
	len = lwc_string_length(ident->idata);
	if (len < 2)
		return false;
	s = lwc_string_data(ident->idata);
	return (s[0] == '-' && s[1] == '-');
}


/* ------------------------------------------------------------------ */
/* var() substitution                                                 */
/* ------------------------------------------------------------------ */

/* A scratch array of css_cp_token pointers used while substituting.
 * We accumulate output as an array-of-pointers to source tokens, then
 * copy to a flat css_cp_token[] for replay. This avoids having to
 * ref/unref lwc_strings during substitution. */
typedef struct cp_scratch {
	const css_cp_token **items;
	uint32_t used;
	uint32_t allocated;
} cp_scratch;

static css_error scratch_grow(cp_scratch *s, uint32_t need)
{
	uint32_t newcap;
	const css_cp_token **newitems;

	if (s->used + need <= s->allocated)
		return CSS_OK;

	newcap = (s->allocated == 0) ? 16 : s->allocated * 2;
	while (newcap < s->used + need)
		newcap *= 2;

	newitems = (const css_cp_token **)realloc(s->items,
			newcap * sizeof(const css_cp_token *));
	if (newitems == NULL)
		return CSS_NOMEM;

	s->items = newitems;
	s->allocated = newcap;
	return CSS_OK;
}

static css_error scratch_append(cp_scratch *s, const css_cp_token *t)
{
	css_error error;

	error = scratch_grow(s, 1);
	if (error != CSS_OK)
		return error;
	s->items[s->used++] = t;
	return CSS_OK;
}

/* Find the matching close-paren for an open function/paren at index i
 * within a cp_token array. Returns the index of the close CHAR, or -1
 * if unbalanced. Assumes arr[i] is FUNCTION (already opens a paren
 * implicitly) OR CHAR('('). For FUNCTION we start depth at 1 since the
 * function itself counts as one open paren. */
static int32_t find_paren_close(const css_cp_token *arr, uint32_t n,
		uint32_t i, bool is_function_open)
{
	int32_t depth;
	uint32_t j;

	depth = is_function_open ? 1 : 0;
	for (j = i + (is_function_open ? 1 : 0); j < n; j++) {
		if (arr[j].type == CSS_TOKEN_FUNCTION) {
			depth++;
		} else if (arr[j].type == CSS_TOKEN_CHAR &&
				arr[j].idata != NULL &&
				lwc_string_length(arr[j].idata) == 1) {
			char ch = lwc_string_data(arr[j].idata)[0];
			if (ch == '(')
				depth++;
			else if (ch == ')') {
				depth--;
				if (depth == 0)
					return (int32_t)j;
			}
		}
	}
	return -1;
}

/* Within a var(...) body, locate the name token and (optionally) the
 * fallback-token range [fb_start, fb_end).
 *  - body_start points at the first token after the FUNCTION token.
 *  - body_end points at the index of the closing ')' (exclusive).
 *  - On success, *name_out is the IDENT token pointer and fb_start/end
 *    are populated (fb_start == fb_end when no fallback present).
 */
static bool parse_var_body(const css_cp_token *arr,
		uint32_t body_start, uint32_t body_end,
		const css_cp_token **name_out,
		uint32_t *fb_start, uint32_t *fb_end)
{
	uint32_t j;
	const css_cp_token *name;

	*name_out = NULL;
	*fb_start = body_end;
	*fb_end = body_end;
	name = NULL;

	/* Skip leading whitespace */
	j = body_start;
	while (j < body_end && arr[j].type == CSS_TOKEN_S)
		j++;
	if (j >= body_end)
		return false;

	/* Name */
	if (arr[j].type != CSS_TOKEN_IDENT || arr[j].idata == NULL)
		return false;
	if (lwc_string_length(arr[j].idata) < 2)
		return false;
	{
		const char *s = lwc_string_data(arr[j].idata);
		if (s[0] != '-' || s[1] != '-')
			return false;
	}
	name = &arr[j];
	j++;

	/* Skip whitespace */
	while (j < body_end && arr[j].type == CSS_TOKEN_S)
		j++;

	if (j < body_end) {
		/* Expect a ',' to introduce fallback */
		if (arr[j].type != CSS_TOKEN_CHAR || arr[j].idata == NULL ||
				lwc_string_length(arr[j].idata) != 1 ||
				lwc_string_data(arr[j].idata)[0] != ',') {
			return false;
		}
		j++;
		*fb_start = j;
		*fb_end = body_end;
	}

	*name_out = name;
	return true;
}

/* Count/get-sheet hooks implemented in css_select.c so we don't have to
 * expose struct css_select_ctx's layout here. */
extern uint32_t css__select_ctx_count_sheets(const css_select_ctx *ctx);
extern const css_stylesheet *css__select_ctx_sheet_at(
		const css_select_ctx *ctx, uint32_t index);

/* Look up a name across all stylesheets in the select context. Later
 * sheets override earlier (matches source-order author cascade). When
 * ctx is NULL we fall back to origin_sheet only. */
static const css_cp_entry *lookup_var(lwc_string *name,
		const css_stylesheet *origin_sheet,
		const css_select_ctx *ctx)
{
	const css_cp_entry *found;
	const css_cp_entry *hit;

	found = NULL;

	if (ctx != NULL) {
		uint32_t i;
		uint32_t count;

		count = css__select_ctx_count_sheets(ctx);
		for (i = 0; i < count; i++) {
			const css_stylesheet *sh;
			sh = css__select_ctx_sheet_at(ctx, i);
			if (sh == NULL)
				continue;
			hit = css__sheet_find_custom_property(sh, name);
			if (hit != NULL)
				found = hit;
		}
	}

	if (found == NULL) {
		found = css__sheet_find_custom_property(origin_sheet, name);
	}

	return found;
}

static css_error substitute_tokens(const css_cp_token *arr, uint32_t n,
		const css_stylesheet *origin_sheet,
		const css_select_ctx *ctx,
		int depth, cp_scratch *out, bool *ok)
{
	uint32_t i;
	css_error error;

	if (depth > MAX_VAR_DEPTH) {
		*ok = false;
		return CSS_OK;
	}

	i = 0;
	while (i < n) {
		bool is_var;
		const char *s;

		is_var = false;
		if (arr[i].type == CSS_TOKEN_FUNCTION &&
				arr[i].idata != NULL &&
				lwc_string_length(arr[i].idata) == 3) {
			s = lwc_string_data(arr[i].idata);
			if ((s[0] == 'v' || s[0] == 'V') &&
					(s[1] == 'a' || s[1] == 'A') &&
					(s[2] == 'r' || s[2] == 'R')) {
				is_var = true;
			}
		}

		if (is_var) {
			int32_t close;
			const css_cp_token *name_tok;
			uint32_t fb_s;
			uint32_t fb_e;
			const css_cp_entry *entry;

			close = find_paren_close(arr, n, i, true);
			if (close < 0) {
				*ok = false;
				return CSS_OK;
			}

			if (!parse_var_body(arr, i + 1, (uint32_t)close,
					&name_tok, &fb_s, &fb_e)) {
				*ok = false;
				return CSS_OK;
			}

			entry = lookup_var(name_tok->idata,
					origin_sheet, ctx);
			if (entry != NULL) {
				error = substitute_tokens(
						entry->tokens,
						entry->n_tokens,
						origin_sheet, ctx,
						depth + 1, out, ok);
				if (error != CSS_OK || !*ok)
					return error;
			} else if (fb_s < fb_e) {
				error = substitute_tokens(
						arr + fb_s, fb_e - fb_s,
						origin_sheet, ctx,
						depth + 1, out, ok);
				if (error != CSS_OK || !*ok)
					return error;
			} else {
				*ok = false;
				return CSS_OK;
			}

			i = (uint32_t)close + 1;
			continue;
		}

		error = scratch_append(out, &arr[i]);
		if (error != CSS_OK)
			return error;
		i++;
	}

	return CSS_OK;
}


/* ------------------------------------------------------------------ */
/* Replay: rebuild a parserutils_vector<css_token> from cp_tokens     */
/* ------------------------------------------------------------------ */

static css_error build_replay_vector(const css_cp_token **ptrs,
		uint32_t n, parserutils_vector **out)
{
	parserutils_vector *v;
	parserutils_error perr;
	uint32_t i;

	perr = parserutils_vector_create(sizeof(css_token), 16, &v);
	if (perr != PARSERUTILS_OK)
		return CSS_NOMEM;

	for (i = 0; i < n; i++) {
		css_token t;
		const css_cp_token *cp = ptrs[i];

		memset(&t, 0, sizeof(t));
		t.type = (css_token_type)cp->type;
		t.idata = cp->idata;
		t.col = cp->col;
		t.line = cp->line;

		if (cp->idata != NULL) {
			t.data.data = (uint8_t *)lwc_string_data(cp->idata);
			t.data.len = lwc_string_length(cp->idata);
		} else {
			t.data.data = NULL;
			t.data.len = 0;
		}

		perr = parserutils_vector_append(v, &t);
		if (perr != PARSERUTILS_OK) {
			parserutils_vector_destroy(v);
			return CSS_NOMEM;
		}
	}

	*out = v;
	return CSS_OK;
}


/* ------------------------------------------------------------------ */
/* Resolve a deferred declaration                                     */
/* ------------------------------------------------------------------ */

/* Linear-search for property ID matching the ident's idata. */
static int find_property_id(css_language *c, lwc_string *name)
{
	int i;
	bool match;

	for (i = FIRST_PROP; i <= LAST_PROP; i++) {
		match = false;
		if (lwc_string_caseless_isequal(name, c->strings[i],
					&match) == lwc_error_ok && match)
			return i;
	}
	return -1;
}

css_error css__deferred_decl_resolve(const css_deferred_decl *dd,
		const css_stylesheet *origin_sheet,
		css_select_ctx *ctx,
		css_select_state *state)
{
	cp_scratch scratch;
	bool ok;
	css_error error;
	parserutils_vector *replay;
	int32_t ctx_idx;
	css_style *scratch_style;
	css_language stub_lang;
	int prop_id;
	css_prop_handler handler;
	uint8_t important_flag;
	css_style cascade_walker;

	if (dd == NULL || origin_sheet == NULL || state == NULL)
		return CSS_BADPARM;

	memset(&scratch, 0, sizeof(scratch));
	ok = true;

	error = substitute_tokens(dd->tokens, dd->n_tokens,
			origin_sheet, ctx, 0, &scratch, &ok);
	if (error != CSS_OK) {
		free(scratch.items);
		return error;
	}
	if (!ok) {
		/* Unresolvable var() — silently drop declaration. */
		free(scratch.items);
		return CSS_OK;
	}

	error = build_replay_vector(scratch.items, scratch.used, &replay);
	if (error != CSS_OK) {
		free(scratch.items);
		return error;
	}
	free(scratch.items);
	scratch.items = NULL;

	/* Minimal css_language stub: property handlers only touch sheet
	 * and strings. Namespaces / context stack are unused during
	 * property parsing. */
	memset(&stub_lang, 0, sizeof(stub_lang));
	/* cast away const: property handlers don't mutate the sheet in
	 * a way that matters here; css__stylesheet_style_create uses
	 * sheet->cached_style, which is fine at resolve time. */
	stub_lang.sheet = (css_stylesheet *)origin_sheet;
	stub_lang.strings = origin_sheet->propstrings;

	prop_id = find_property_id(&stub_lang, dd->property);
	if (prop_id < 0) {
		parserutils_vector_destroy(replay);
		return CSS_OK;
	}

	handler = property_handlers[prop_id - FIRST_PROP];
	if (handler == NULL) {
		parserutils_vector_destroy(replay);
		return CSS_OK;
	}

	error = css__stylesheet_style_create(stub_lang.sheet, &scratch_style);
	if (error != CSS_OK) {
		parserutils_vector_destroy(replay);
		return error;
	}

	ctx_idx = 0;
	error = handler(&stub_lang, replay, &ctx_idx, scratch_style);
	if (error != CSS_OK) {
		css__stylesheet_style_destroy(scratch_style);
		parserutils_vector_destroy(replay);
		/* Invalid value post-substitution — drop silently. */
		return CSS_OK;
	}

	important_flag = 0;
	if (dd->important) {
		css__make_style_important(scratch_style);
		important_flag = 1;
	} else {
		/* Also honour a trailing !important captured in tokens,
		 * in case the capture path left it in. */
		error = css__parse_important(&stub_lang, replay, &ctx_idx,
				&important_flag);
		if (error == CSS_OK && important_flag != 0)
			css__make_style_important(scratch_style);
	}

	/* Apply bytecode through the normal cascade dispatch. */
	cascade_walker = *scratch_style;
	while (cascade_walker.used > 0) {
		opcode_t op;
		css_code_t opv;

		opv = *cascade_walker.bytecode;
		cascade_walker.used -= 1;
		cascade_walker.bytecode = cascade_walker.bytecode + 1;

		op = getOpcode(opv);

		error = prop_dispatch[op].cascade(opv, &cascade_walker, state);
		if (error != CSS_OK) {
			css__stylesheet_style_destroy(scratch_style);
			parserutils_vector_destroy(replay);
			return error;
		}
	}

	css__stylesheet_style_destroy(scratch_style);
	parserutils_vector_destroy(replay);

	return CSS_OK;
}
