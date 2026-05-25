/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2026 The MacSurf Project.
 *
 * CSS Custom Properties ("--name") and var() deferred resolution.
 *
 * The cascade stays bytecode-driven for every declaration whose value
 * does not reference var(). Declarations that do reference var() cannot
 * be resolved at parse time because custom-property definitions may
 * live in any stylesheet that is part of the same select context; the
 * value tokens are therefore captured verbatim and attached to the
 * rule's css_style as a css_deferred_decl list. At selection time,
 * cascade_style walks that list, substitutes var() against the
 * select_ctx's aggregate custom-property table, calls the usual
 * per-property parser on the resolved tokens to obtain bytecode, and
 * applies it via the normal prop_dispatch path.
 *
 * Custom-property definitions themselves (e.g. ":root { --platinum-bg:
 * #ddd; }") are captured during parse into each stylesheet's
 * custom_properties list regardless of which selector scope they
 * appeared in (a conscious simplification — treat every --name as
 * globally scoped for the owning stylesheet).
 */

#ifndef css_css__parse_custom_properties_h_
#define css_css__parse_custom_properties_h_

#include <libwapcaplet/libwapcaplet.h>
#include <parserutils/utils/vector.h>

#include <libcss/errors.h>
#include <libcss/functypes.h>
#include <libcss/types.h>

#include "lex/lex.h"

/* Forward declarations to avoid circular includes. */
struct css_stylesheet;
struct css_style;
struct css_language;
struct css_select_ctx;
struct css_select_state;

/**
 * Preserved token. We keep only the fields that survive beyond the
 * parser's token-buffer lifetime:
 *  - type, so replay through property handlers dispatches correctly;
 *  - idata, the interned text (ref-owned by this struct);
 *  - col / line, for diagnostics.
 *
 * The css_token.data byte-buffer pointer is recomputed from idata at
 * replay time (lwc_string_data / lwc_string_length).
 */
typedef struct css_cp_token {
	uint16_t type;           /**< css_token_type */
	uint16_t pad;            /**< Alignment */
	lwc_string *idata;       /**< Interned text, ref-owned, or NULL */
	uint32_t col;
	uint32_t line;
} css_cp_token;

/**
 * A custom-property definition: "--name" -> value tokens.
 */
struct css_cp_entry {
	lwc_string *name;        /**< "--platinum-bg" etc. (ref-owned) */
	css_cp_token *tokens;    /**< Value tokens (owned) */
	uint32_t n_tokens;
	struct css_cp_entry *next;
};
typedef struct css_cp_entry css_cp_entry;

/**
 * A declaration whose value contains var() and therefore cannot be
 * compiled to bytecode at parse time.
 */
struct css_deferred_decl {
	lwc_string *property;    /**< Property name as lexed (e.g.
				  *   "background-color", ref-owned) */
	css_cp_token *tokens;    /**< Raw value tokens inc. var() calls
				  *   (owned) */
	uint32_t n_tokens;
	uint8_t important;       /**< Non-zero => !important */
	uint8_t pad[3];
	struct css_deferred_decl *next;
};
typedef struct css_deferred_decl css_deferred_decl;


/* --- Token list helpers --- */

/**
 * Deep-copy tokens [start, end) from a parserutils_vector into a
 * freshly allocated css_cp_token[] array. Tokens below
 * CSS_TOKEN_LAST_INTERN that carry idata get their lwc_string ref'd;
 * whitespace tokens with NULL idata copy cleanly.
 *
 * \param vec   Source vector.
 * \param start Inclusive start index.
 * \param end   Exclusive end index.
 * \param out_tokens Receives malloc'd array (NULL on count == 0).
 * \param out_n Receives count.
 * \return CSS_OK, CSS_NOMEM, CSS_BADPARM.
 */
css_error css__cp_tokens_from_vector(const parserutils_vector *vec,
		int32_t start, int32_t end,
		css_cp_token **out_tokens, uint32_t *out_n);

/**
 * Free a css_cp_token[] array (unrefs each idata, frees the array).
 */
void css__cp_tokens_destroy(css_cp_token *tokens, uint32_t n);


/* --- Custom-property list (per stylesheet) --- */

/**
 * Append a custom-property entry to a stylesheet's list. If the sheet
 * already defines a property with the same name, the existing entry's
 * tokens are replaced (last-write-wins, matching CSS cascade source
 * order within a sheet). The function takes ownership of tokens on
 * success; on failure it destroys them.
 *
 * \param sheet Target stylesheet (field custom_properties is updated).
 * \param name  Custom property name, already ref'd for storage.
 * \param tokens Owned token array.
 * \param n      Token count.
 */
css_error css__sheet_add_custom_property(struct css_stylesheet *sheet,
		lwc_string *name, css_cp_token *tokens, uint32_t n);

/**
 * Look up a custom property by name within one stylesheet's list.
 *
 * \return Non-NULL pointer to the entry on hit, NULL on miss.
 */
const css_cp_entry *css__sheet_find_custom_property(
		const struct css_stylesheet *sheet, lwc_string *name);

/**
 * Free an entire cp_entry linked list.
 */
void css__cp_entry_list_destroy(css_cp_entry *head);


/* fixes267 — doc-global inline-extras custom-property table.
 * Public API declared in <libcss/select.h>: css_inline_extras_register_sheet()
 * and css_inline_extras_clear(). */


/* --- Deferred declaration list (per css_style) --- */

/**
 * Create a new deferred declaration. Takes ownership of property (must
 * already be ref'd) and tokens. On failure, ownership is released.
 */
css_error css__deferred_decl_create(lwc_string *property,
		css_cp_token *tokens, uint32_t n, bool important,
		css_deferred_decl **out);

/**
 * Append a deferred decl onto a css_style's deferred list (preserves
 * source order).
 */
void css__deferred_decl_attach(struct css_style *style,
		css_deferred_decl *dd);

/**
 * Free an entire deferred_decl list.
 */
void css__deferred_decl_list_destroy(css_deferred_decl *head);


/* --- Var() detection / resolution --- */

/**
 * Scan a token vector from *ctx forward and return true if any top-level
 * token is a FUNCTION named "var" (case-insensitive).
 *
 * The CSS custom-properties spec also flags var() occurring nested
 * inside other functions (e.g. calc(var(--x))); the scan descends into
 * any FUNCTION/paren pair to catch those.
 */
bool css__value_contains_var(const parserutils_vector *vec, int32_t ctx);

/**
 * Resolve a deferred declaration against the select_ctx's aggregate
 * custom-property table and apply the resulting bytecode through
 * prop_dispatch. On successful resolution and apply, returns CSS_OK.
 * On unresolvable var() (no definition, no fallback), the declaration
 * is silently discarded and CSS_OK is returned.
 *
 * \param dd         The deferred declaration.
 * \param origin_sheet The stylesheet that owns the declaration (used
 *                     for propstrings and as first lookup scope).
 * \param ctx        Select context (provides other sheets).
 * \param state      Current select state — cascade writes into it.
 */
css_error css__deferred_decl_resolve(const css_deferred_decl *dd,
		const struct css_stylesheet *origin_sheet,
		struct css_select_ctx *ctx,
		struct css_select_state *state);

/**
 * Helper: is a freshly lexed IDENT a CSS custom-property name?
 * True when idata's first two characters are both '-'.
 */
bool css__is_custom_property_ident(const css_token *ident);

#endif
