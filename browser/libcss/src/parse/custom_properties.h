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
 * appeared in (a conscious simplification - treat every --name as
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

/* fixes1268a (#167) - rule-scoped custom-property definitions.
 *
 * css__sheet_add_custom_property above stores "--name" definitions in a
 * single per-STYLESHEET list, discarding the selector and @media context
 * they were written under. That is wrong: a sheet may define the same
 * name several times under mutually exclusive scopes (facebook.com's
 * theme tokens define --web-wash four times, once per theme class, the
 * last inside @media (prefers-color-scheme: dark)), and last-write-wins
 * across the whole sheet then hands every consumer the wrong value.
 *
 * Definitions are therefore ALSO attached to the owning rule's css_style,
 * exactly as var()-referencing declarations already are. A css_style only
 * cascades when its rule matches, and rules inside a non-matching @media
 * are already filtered by mq_rule_good_for_media during selection, so
 * both scoping dimensions come along for free.
 *
 * Stage 1268a stores them and nothing more - resolution still reads the
 * sheet-global list, so behaviour is unchanged. 1268b makes the cascade
 * build a per-element environment from these; 1268e removes the
 * sheet-global list.
 */

/**
 * Append a custom-property definition to a css_style's own list.
 *
 * Unlike the per-sheet variant this does NOT replace an existing entry of
 * the same name: a rule restating a name is legal, and the cascade takes
 * the last one in source order. Takes ownership of name and tokens on
 * success; destroys both on failure.
 */
css_error css__style_add_custom_property(struct css_style *style,
		lwc_string *name, css_cp_token *tokens, uint32_t n);

/**
 * Look up a custom property in one css_style's own list. Returns the LAST
 * entry with a matching name (later declaration in the same rule wins).
 *
 * \return Non-NULL entry on hit, NULL on miss.
 */
const css_cp_entry *css__style_find_custom_property(
		const struct css_style *style, lwc_string *name);


/* fixes267 - doc-global inline-extras custom-property table.
 * Public API declared in <libcss/select.h>: css_inline_extras_register_sheet()
 * and css_inline_extras_clear(). */


/* --- Per-element custom-property environment (fixes1268b, #167) --- */

/*
 * The environment is the set of custom properties in force for ONE
 * element, built during selection by cascade_style as each matching rule
 * contributes its rule-scoped definitions. Because a css_style only
 * cascades when its rule matches, and rules inside a non-matching @media
 * are already filtered out by mq_rule_good_for_media, both the selector
 * scoping and the media scoping that the per-sheet store threw away are
 * restored here for free.
 *
 * Bindings BORROW their name and tokens from the stylesheet, which
 * outlives selection. Destroying an environment frees only its array.
 */

/**
 * One custom property in force for the element being selected, with the
 * cascade metadata needed to decide whether a later definition replaces
 * it.
 */
typedef struct css_cp_binding {
	css_cp_entry entry;      /**< name/tokens; see owns_tokens */
	uint32_t specificity;    /**< Specificity of the defining rule */
	uint8_t origin;          /**< css_origin of the defining sheet */
	uint8_t important;       /**< Non-zero => !important */
	/* fixes1268c - before finalisation tokens are borrowed from the
	 * stylesheet; after it they are a freshly built substituted run
	 * this binding owns and must free (each idata ref'd). */
	uint8_t owns_tokens;
	uint8_t pad[1];
} css_cp_binding;

struct css_cp_env {
	css_cp_binding *items;
	uint32_t used;
	uint32_t allocated;

	/* fixes1268c (#167) - the parent element's FINALISED environment,
	 * or NULL at the root. Custom properties inherit, and a chain of
	 * references costs nothing per element, where copying the parent's
	 * bindings would cost O(properties x elements) - facebook.com
	 * defines several hundred tokens on one ancestor.
	 *
	 * Reference-counted rather than borrowed: correctness must not
	 * depend on any particular destruction order between a parent
	 * style and a child's. */
	struct css_cp_env *inherited;
	uint32_t refcount;

	/* Non-zero once css__cp_env_finalise has substituted every own
	 * binding's var() references, making the bindings COMPUTED values
	 * that children may inherit directly. Before that the bindings
	 * hold raw, borrowed token runs. */
	uint8_t finalised;
	uint8_t pad2[3];
};
typedef struct css_cp_env css_cp_env;

/**
 * Offer a definition to an element's environment, applying the same
 * origin / importance / specificity precedence as ordinary properties
 * (see css__outranks_existing). Creates the environment on first use.
 *
 * name and tokens are BORROWED, not owned - they must outlive the
 * environment, which they do, being owned by the stylesheet.
 *
 * \param env  Environment pointer-to-pointer; *env may be NULL.
 * \return CSS_OK or CSS_NOMEM.
 */
css_error css__cp_env_add(css_cp_env **env, lwc_string *name,
		const css_cp_token *tokens, uint32_t n,
		uint32_t specificity, uint8_t origin, uint8_t important);

/**
 * Look up the winning definition of `name` in an element's environment.
 *
 * \return Borrowed entry on hit, NULL on miss.
 */
const css_cp_entry *css__cp_env_find(const css_cp_env *env,
		lwc_string *name);

/**
 * Release a reference to an environment, freeing it (and releasing its
 * reference to the inherited one) when the last goes.
 */
void css__cp_env_unref(css_cp_env *env);

/**
 * Take an additional reference. Returns env for convenience.
 */
css_cp_env *css__cp_env_ref(css_cp_env *env);

/**
 * Point a (possibly not-yet-created) environment at the parent
 * element's finalised environment, taking a reference.
 */
css_error css__cp_env_set_inherited(css_cp_env **env, css_cp_env *parent);

/**
 * Substitute every own binding's var() references, turning the
 * environment's raw token runs into COMPUTED values that children can
 * inherit directly.
 *
 * This is required by CSS Variables 1: the computed value of a custom
 * property is its specified value with variables substituted, so a
 * child inheriting "--b: var(--a)" from its parent receives the
 * PARENT's --a, not its own. Verified against Chrome:
 *
 *   .p { --a: red; --b: var(--a) }
 *   .c { --a: blue; color: var(--b) }   =>  .c color is RED
 *
 * Substituting per element rather than per lookup also makes the result
 * independent of declaration order within a rule, which Chrome also
 * confirms (".q { --b: var(--a); --a: green }" resolves to green).
 */
css_error css__cp_env_finalise(css_cp_env *env,
		const struct css_stylesheet *origin_sheet,
		const struct css_select_ctx *ctx,
		const struct css_stylesheet *inline_sheet);

/**
 * Contribute every rule-scoped definition attached to `style` to `env`,
 * in source order, at the given cascade position. Called by cascade_style
 * for each matching rule.
 */
css_error css__cp_env_add_style(css_cp_env **env,
		const struct css_style *style,
		uint32_t specificity, uint8_t origin);

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
 * \param state      Current select state - cascade writes into it.
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
