/*
 * Copyright 2005 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2003 Phil Mellor <monkeyson@users.sourceforge.net>
 * Copyright 2005 John M Bell <jmb202@ecs.soton.ac.uk>
 * Copyright 2006 Richard Wilson <info@tinct.net>
 * Copyright 2008 Michael Drake <tlsa@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * Implementation of conversion from DOM tree to box tree.
 */

#include <string.h>
#include <dom/dom.h>

#include "utils/ns_errors.h"
#include "utils/nsoption.h"
#include "utils/corestrings.h"
#include "utils/talloc.h"
#include "utils/string.h"
#include "utils/ascii.h"
#include "utils/nsurl.h"
#include "utils/utils.h"
#include "netsurf/misc.h"
#include "css/select.h"
#include "content/hlcache.h"		/* fixes520: hlcache_content_is_live */
#include "desktop/gui_internal.h"

/* fixes533: out-of-band live-content registry (anti-UAF).  Populated at
 * content__init (before any box conversion is scheduled) and cleared at
 * content_destroy, so unlike hlcache_content_is_live (removed from the entry
 * gate at fixes521 for false-negatives) this never false-bails a live
 * conversion.  No-op (always live) on non-Mac syntax-check builds. */
#ifdef __MACOS9__
extern int macos9_content_is_live(struct content *c);
extern unsigned long macos9_content_token(struct content *c);
extern int macos9_content_token_valid(struct content *c, unsigned long token);
extern void macos9_content_drain_deferred(void);
#else
#define macos9_content_is_live(c) (1)
#define macos9_content_token(c) (1UL)
#define macos9_content_token_valid(c, t) (1)
#define macos9_content_drain_deferred() ((void)0)
#endif

/* fixes895 (reconvert-crash hunt) - dense breadcrumbs for the async box-build
 * window, which is where the reconvert crash kills the session (between
 * "html_reconvert_content rc=0" and "reconvert #N: DONE"). All of it is gated
 * on macsurf_reconvert_in_progress (defined in html.c, non-zero ONLY during a
 * re-convert) so a cold page load -- which drives the SAME convert_xml_to_box
 * path -- is not flooded. The pos marker + freemem reading are defined in
 * macsurf_debug_log.c; declared local-extern here matching this file's existing
 * `extern void macsurf_debug_log_writef` pattern. */
extern int macsurf_reconvert_in_progress;
extern void macsurf_debug_log_writef(const char *fmt, ...);
extern void macsurf_reconv_pos_set(const char *phase, long seq, long node_ix,
		const char *tag);
extern void macsurf_reconv_pos_flush(void);
extern long macsurf_free_mem(void);
extern unsigned long macsurf_reconvert_seq;   /* html.c */
/* running node index within the CURRENT reconvert's box build (reset in
 * dom_to_box). Names the furthest node reached when a bomb hits. */
static unsigned long g_reconv_node_ix = 0;

/* fixes552 - WRITER-SIDE free guard (closes the free-during-walk window at the
 * single writer instead of at every reader).  g_walk_content marks the content
 * whose box walk is CURRENTLY on the stack - set by the convert_xml_to_box
 * wrapper for the entire batch, INCLUDING the html_fetch_object -> OT yield
 * where a bulk hlcache_clean fires content_destroy.  Both the eviction path
 * (hlcache_clean, fixes553) and content_destroy (ns_content.c) call
 * macos9_box_walk_owns_content() and SKIP / DEFER freeing a pinned content, so
 * box->style / the box arena / the parser / the DOM strings cannot be torn out
 * from under the live walk (the fixes547/550 family).  The generation token
 * defeats ABA: if g_walk_content's address was freed+reused, the token no
 * longer matches and the free proceeds normally.
 *
 * fixes553 - the pin now covers the walked content's ENTIRE tree, not just the
 * content itself; see macos9_box_walk_owns_content below the html/ includes
 * (it walks object_list, which needs the full content_html_object definition). */
static struct content *g_walk_content = NULL;
static unsigned long    g_walk_gen     = 0;

#include "html/html.h"		/* fixes553: content_html_object full def for object_list walk */
#include "html/private.h"
#include "html/object.h"
#include "html/box.h"
#include "html/box_manipulate.h"
#include "html/box_construct.h"
#include "html/box_special.h"
#include "html/box_normalise.h"
#include "html/form_internal.h"

#include "macsurf_debug.h"
#include "macos9_deathrow.h"

/* fixes553 - extend the fixes552 writer-side free guard from the single walked
 * content to its ENTIRE tree.  The box walk dereferences not just the
 * html_content itself but the sub-resource contents hanging off its object_list
 * (images, iframes, embedded objects) and the DOM document strings the
 * html_content owns.  A bulk hlcache_clean (cache-pressure eviction) firing
 * while convert_xml_to_box is mid-walk (during the html_fetch_object -> OT
 * yield) must not free ANY content the walk still holds, or
 * box_image_resolve_url / hlcache_handle_retrieve byte-scans a wild pointer
 * (the reported convert_xml_to_box UAF).
 *
 * Returns non-zero when c is pinned by the live walk:
 *   (a) c is the walked html_content itself - which also covers the DOM
 *       document it owns and the dom_strings the walk dereferences; or
 *   (b) c is a sub-resource content in the walked html_content's object_list.
 *
 * The walked content's generation is validated FIRST: if its address was freed
 * and reused, g_walk_gen no longer matches and NOTHING is pinned, so a stale
 * tree cannot ABA-false-positive.  Once validated, object_list is owned by the
 * (live) html_content and safe to walk on this cooperative single thread. */
int macos9_box_walk_owns_content(struct content *c)
{
	html_content *hc;
	struct content_html_object *obj;

	if (g_walk_content == NULL || c == NULL)
		return 0;

	/* gen-gate the walked content; bail (pin nothing) on ABA mismatch */
	if (macos9_content_token_valid(g_walk_content, g_walk_gen) == 0)
		return 0;

	/* (a) the html_content itself (and the DOM document / strings it owns) */
	if (c == g_walk_content)
		return 1;

	/* (b) any sub-resource content in its object/child list */
	hc = (html_content *)g_walk_content;
	for (obj = hc->object_list; obj != NULL; obj = obj->next) {
		if (obj->content != NULL &&
		    hlcache_handle_get_content(obj->content) == c)
			return 1;
	}

	return 0;
}


/* Diagnostic: count text boxes constructed during DOM->box conversion. */
long macos9_box_text_created = 0;

/**
 * Context for box tree construction
 */
/*
 * fixes134b: flat counter table.
 *
 * One linked list head per box-construct pass. CSS counter scoping in
 * the strict spec is hierarchical (each element with counter-reset
 * starts a new counter in its own scope, descendants increment that
 * nested counter, and the outer counter is restored on element exit).
 * We deliberately simplify to a flat table -- works for the common
 * single-level pattern (body counter-reset + h2::before
 * counter-increment) which is the only counter usage we'll see on real
 * pages. Nested scopes are deferred.
 *
 * Entries are talloc-allocated against the content->bctx so they share
 * the box tree's lifetime.
 */
struct macsurf_counter_entry {
	lwc_string *name;
	int32_t value;
	struct macsurf_counter_entry *next;
};

struct box_construct_ctx {
	html_content *content;		/**< Content we're constructing for */

	dom_node *n;			/**< Current node to process */

	struct box *root_box;		/**< Root box in the tree */

	box_construct_complete_cb cb;	/**< Callback to invoke on completion */

	int *bctx;			/**< talloc context */

	/** fixes134b: head of the flat counter table.  Linear lookup is
	 * fine -- real pages have at most a handful of named counters. */
	struct macsurf_counter_entry *counters;

	/** fixes140a: document-scope quote nesting depth. Incremented by
	 * each materialised open-quote / no-open-quote item, decremented
	 * (floored at 0) by each close-quote / no-close-quote item. Per
	 * CSS 2.1 §12.3 the depth indexes into the resolved `quotes`
	 * list to choose which pair of opening/closing strings to emit
	 * for the current nesting level. */
	int32_t quote_depth;
};

/**
 * Transient properties for construction of current node
 */
struct box_construct_props {
	/** Style from which to inherit, or NULL if none */
	const css_computed_style *parent_style;
	/* fixes1268c (#167) - inherited custom properties, threaded from
	 * the parent box exactly like parent_style above. */
	css_custom_env *parent_custom_env;
	/** Current link target, or NULL if none */
	struct nsurl *href;
	/** Current frame target, or NULL if none */
	const char *target;
	/** fixes1063 (#114): enclosing <a> carries the `download` attribute */
	bool download;
	/** Current title attribute, or NULL if none */
	const char *title;
	/** Identity of the current block-level container */
	struct box *containing_block;
	/** Current container for inlines, or NULL if none
	 * \note If non-NULL, will be the last child of containing_block */
	struct box *inline_container;
	/** Whether the current node is the root of the DOM tree */
	bool node_is_root;
};

static const content_type image_types = CONTENT_IMAGE;

/* mapping from CSS display to box type
 * this table must be in sync with libcss' css_display enum */
static const box_type box_map[] = {
	BOX_BLOCK,           /* CSS_DISPLAY_INHERIT */
	BOX_INLINE,          /* CSS_DISPLAY_INLINE */
	BOX_BLOCK,           /* CSS_DISPLAY_BLOCK */
	BOX_BLOCK,           /* CSS_DISPLAY_LIST_ITEM */
	BOX_INLINE,          /* CSS_DISPLAY_RUN_IN */
	BOX_INLINE_BLOCK,    /* CSS_DISPLAY_INLINE_BLOCK */
	BOX_TABLE,           /* CSS_DISPLAY_TABLE */
	BOX_TABLE,           /* CSS_DISPLAY_INLINE_TABLE */
	BOX_TABLE_ROW_GROUP, /* CSS_DISPLAY_TABLE_ROW_GROUP */
	BOX_TABLE_ROW_GROUP, /* CSS_DISPLAY_TABLE_HEADER_GROUP */
	BOX_TABLE_ROW_GROUP, /* CSS_DISPLAY_TABLE_FOOTER_GROUP */
	BOX_TABLE_ROW,       /* CSS_DISPLAY_TABLE_ROW */
	BOX_NONE,            /* CSS_DISPLAY_TABLE_COLUMN_GROUP */
	BOX_NONE,            /* CSS_DISPLAY_TABLE_COLUMN */
	BOX_TABLE_CELL,      /* CSS_DISPLAY_TABLE_CELL */
	BOX_INLINE,          /* CSS_DISPLAY_TABLE_CAPTION */
	BOX_NONE,            /* CSS_DISPLAY_NONE */
	BOX_FLEX,            /* CSS_DISPLAY_FLEX */
	BOX_INLINE_FLEX,     /* CSS_DISPLAY_INLINE_FLEX */
	BOX_GRID,            /* CSS_DISPLAY_GRID -- fixes75 */
	BOX_INLINE_GRID,     /* CSS_DISPLAY_INLINE_GRID -- fixes75 */
	BOX_CONTENTS,        /* CSS_DISPLAY_CONTENTS */
};


/**
 * determine if a box is the root node
 *
 * \param n node to check
 * \return true if node is root else false.
 */
static inline bool box_is_root(dom_node *n)
{
	dom_node *parent;
	dom_node_type type;
	dom_exception err;

	err = dom_node_get_parent_node(n, &parent);
	if (err != DOM_NO_ERR)
		return false;

	if (parent != NULL) {
		err = dom_node_get_node_type(parent, &type);

		dom_node_unref(parent);

		if (err != DOM_NO_ERR)
			return false;

		if (type != DOM_DOCUMENT_NODE)
			return false;
	}

	return true;
}

/**
 * Extract transient construction properties
 *
 * \param n      Current DOM node to convert
 * \param props  Property object to populate
 */
static void
box_extract_properties(dom_node *n, struct box_construct_props *props)
{
	memset(props, 0, sizeof(*props));

	props->node_is_root = box_is_root(n);

	/* Extract properties from containing DOM node */
	if (props->node_is_root == false) {
		dom_node *current_node = n;
		dom_node *parent_node = NULL;
		struct box *parent_box;
		dom_exception err;

		/* Find ancestor node containing parent box */
		while (true) {
			err = dom_node_get_parent_node(current_node,
					&parent_node);
			if (err != DOM_NO_ERR || parent_node == NULL)
				break;

			parent_box = box_for_node(parent_node);

			if (parent_box != NULL) {
				props->parent_style = parent_box->style;
				props->parent_custom_env =
						parent_box->custom_env;
				props->href = parent_box->href;
				props->target = parent_box->target;
				/* fixes1063 (#114) - travels with href. */
				props->download = (parent_box->flags &
						LINK_DOWNLOAD) != 0;
				props->title = parent_box->title;

				dom_node_unref(parent_node);
				break;
			} else {
				if (current_node != n)
					dom_node_unref(current_node);
				current_node = parent_node;
				parent_node = NULL;
			}
		}

		/* Find containing block (may be parent) */
		while (true) {
			struct box *b;

			err = dom_node_get_parent_node(current_node,
					&parent_node);
			if (err != DOM_NO_ERR || parent_node == NULL) {
				if (current_node != n)
					dom_node_unref(current_node);
				break;
			}

			if (current_node != n)
				dom_node_unref(current_node);

			b = box_for_node(parent_node);

			/* Children of nodes that created an inline box
			 * will generate boxes which are attached as
			 * _siblings_ of the box generated for their
			 * parent node. Note, however, that we'll still
			 * use the parent node's styling as the parent
			 * style, above. */
			if (b != NULL && b->type != BOX_INLINE &&
					b->type != BOX_CONTENTS &&
					b->type != BOX_BR) {
				props->containing_block = b;

				dom_node_unref(parent_node);
				break;
			} else {
				current_node = parent_node;
				parent_node = NULL;
			}
		}
	}

	/* Compute current inline container, if any */
	if (props->containing_block != NULL &&
			props->containing_block->last != NULL &&
			props->containing_block->last->type ==
				BOX_INLINE_CONTAINER)
		props->inline_container = props->containing_block->last;
}


/**
 * Get the style for an element.
 *
 * \param  c               content of type CONTENT_HTML that is being processed
 * \param  parent_style    style at this point in xml tree, or NULL for root
 * \param  root_style      root node's style, or NULL for root
 * \param  n               node in xml tree
 * \return  the new style, or NULL on memory exhaustion
 */
static css_select_results *
box_get_style(html_content *c,
	      const css_computed_style *parent_style,
	      const css_computed_style *root_style,
	      dom_node *n,
	      css_custom_env *parent_custom_env,
	      css_custom_env **out_custom_env)
{
	dom_string *s = NULL;
	css_stylesheet *inline_style = NULL;
	css_select_results *styles;
	nscss_select_ctx ctx;

	/* Firstly, construct inline stylesheet, if any */
	if (nsoption_bool(author_level_css)) {
		dom_exception err;
		err = dom_element_get_attribute(n, corestring_dom_style, &s);
		if (err != DOM_NO_ERR) {
			return NULL;
		}
	}

	if (s != NULL) {
		inline_style = nscss_create_inline_style(
				(const uint8_t *) dom_string_data(s),
				dom_string_byte_length(s),
				c->encoding,
				nsurl_access(c->base_url),
				c->quirks != DOM_DOCUMENT_QUIRKS_MODE_NONE);

		dom_string_unref(s);

		if (inline_style == NULL)
			return NULL;
	}

	/* Populate selection context */
	ctx.ctx = c->select_ctx;
	ctx.quirks = (c->quirks == DOM_DOCUMENT_QUIRKS_MODE_FULL);
	ctx.base_url = c->base_url;
	ctx.universal = c->universal;
	ctx.root_style = root_style;
	ctx.parent_style = parent_style;
	/* fixes1268c (#167) - inherited custom properties in, this
	 * element's own set out. */
	ctx.parent_custom_env = parent_custom_env;
	ctx.produced_custom_env = NULL;
	/* fixes130 - propagate dynamic pseudo-class state into the
	 * select context so :hover / :active / :focus match correctly
	 * during this cascade pass. */
	ctx.dyn_hover_node = c->dyn_hover_node;
	ctx.dyn_active_node = c->dyn_active_node;
	ctx.dyn_focus_node = c->dyn_focus_node;

	/* Select style for element */
	styles = nscss_get_style(&ctx, n, &c->media, &c->unit_len_ctx,
			inline_style);

	/* Transfer the element's environment to the caller, which stores it
	 * on the box so this element's children can inherit it. */
	if (out_custom_env != NULL) {
		*out_custom_env = ctx.produced_custom_env;
	} else if (ctx.produced_custom_env != NULL) {
		css_custom_env_unref(ctx.produced_custom_env);
	}

	/* No longer need inline style */
	if (inline_style != NULL)
		css_stylesheet_destroy(inline_style);

	return styles;
}

css_select_results *html_svg_get_style(const html_content *c, dom_node *node,
		const css_computed_style *parent_style,
		css_custom_env *parent_custom_env,
		css_custom_env **out_custom_env)
{
	const css_computed_style *root_style =
			(c->layout != NULL) ? c->layout->style : parent_style;
	return box_get_style((html_content *)c, parent_style, root_style, node,
			parent_custom_env, out_custom_env);
}


/*
 * fixes134b: counter table helpers.
 *
 * Flat lookup-or-create + apply / read. All four routines are NULL-style-
 * safe (no-op on NULL style). We store the lwc_string name as a weak
 * pointer (no ref) -- the libcss cascade owns these strings via the
 * computed style, and the counter table is talloc'd against ctx->bctx
 * which shares lifetime with the styles. lwc_string_isequal handles
 * cross-object equivalence so pointer equality isn't required.
 */

static struct macsurf_counter_entry *
counter_lookup_or_create(struct box_construct_ctx *ctx, lwc_string *name)
{
	struct macsurf_counter_entry *e;
	bool same;

	if (name == NULL) return NULL;

	for (e = ctx->counters; e != NULL; e = e->next) {
		if (lwc_string_isequal(e->name, name, &same) ==
				lwc_error_ok && same) {
			return e;
		}
	}

	e = talloc_size(ctx->bctx,
			sizeof(struct macsurf_counter_entry));
	if (e == NULL) return NULL;
	e->name = name;
	e->value = 0;
	e->next = ctx->counters;
	ctx->counters = e;
	return e;
}

static void
counter_apply_reset(struct box_construct_ctx *ctx,
		const css_computed_style *style)
{
	const css_computed_counter *arr;
	struct macsurf_counter_entry *e;
	uint8_t state;

	if (style == NULL) return;
	arr = NULL;
	state = css_computed_counter_reset(style, &arr);
	if (state != CSS_COUNTER_RESET_NAMED || arr == NULL) return;

	while (arr->name != NULL) {
		e = counter_lookup_or_create(ctx, arr->name);
		if (e != NULL) {
			e->value = FIXTOINT(arr->value);
		}
		arr++;
	}
}

static void
counter_apply_increment(struct box_construct_ctx *ctx,
		const css_computed_style *style)
{
	const css_computed_counter *arr;
	struct macsurf_counter_entry *e;
	uint8_t state;

	if (style == NULL) return;
	arr = NULL;
	state = css_computed_counter_increment(style, &arr);
	if (state != CSS_COUNTER_INCREMENT_NAMED || arr == NULL) return;

	while (arr->name != NULL) {
		e = counter_lookup_or_create(ctx, arr->name);
		if (e != NULL) {
			e->value += FIXTOINT(arr->value);
		}
		arr++;
	}
}

static int32_t
counter_get_value(struct box_construct_ctx *ctx, lwc_string *name)
{
	struct macsurf_counter_entry *e;
	bool same;

	if (name == NULL) return 0;

	for (e = ctx->counters; e != NULL; e = e->next) {
		if (lwc_string_isequal(e->name, name, &same) ==
				lwc_error_ok && same) {
			return e->value;
		}
	}
	return 0;
}

/*
 * fixes134b: int32 decimal formatter.
 *
 * MSL snprintf is flagged unreliable on CW8 (see CLAUDE.md file-backed
 * log channel notes). Hand-rolled. Returns characters written (excluding
 * NUL); buf is always NUL-terminated when bufsize > 0.
 */
static size_t
counter_fmt_decimal(int32_t v, char *buf, size_t bufsize)
{
	char tmp[16];
	size_t n;
	size_t i;
	bool neg;

	if (bufsize == 0) return 0;

	neg = false;
	if (v < 0) {
		neg = true;
		v = -v;
	}

	n = 0;
	do {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	} while (v > 0 && n < sizeof(tmp));

	if (neg && n < sizeof(tmp)) {
		tmp[n++] = '-';
	}

	if (n >= bufsize) n = bufsize - 1;
	for (i = 0; i < n; i++) {
		buf[i] = tmp[n - 1 - i];
	}
	buf[n] = '\0';
	return n;
}

/**
 * Construct the box required for a generated element.
 *
 * \param ctx      Box-construct context (carries counter table)
 * \param n        XML node of type XML_ELEMENT_NODE
 * \param content  Content of type CONTENT_HTML that is being processed
 * \param box      Box which may have generated content
 * \param style    Complete computed style for pseudo element, or NULL
 */
static void
box_construct_generate(struct box_construct_ctx *ctx,
		       dom_node *n,
		       html_content *content,
		       struct box *box,
		       const css_computed_style *style)
{
	struct box *gen = NULL;
	enum css_display_e computed_display;
	const css_computed_content_item *c_item;

	/* fixes140: previously the function bailed unless box was
	 * BOX_BLOCK, which silently killed every q::before /
	 * q::after rule because <q> is BOX_INLINE by default. Now
	 * inline-class parents (BOX_INLINE, BOX_INLINE_BLOCK,
	 * BOX_INLINE_FLEX) are allowed; the text-attach site below
	 * branches on the parent type and adds BOX_TEXT directly to
	 * the inline parent rather than wrapping it in an
	 * INLINE_CONTAINER (which only belongs inside a block). */
	if (box->type != BOX_BLOCK &&
			box->type != BOX_INLINE &&
			box->type != BOX_INLINE_BLOCK &&
			box->type != BOX_INLINE_FLEX)
		return;

	/* To determine if an element has a pseudo element, we select
	 * for it and test to see if the returned style's content
	 * property is set to normal. */
	if (style == NULL ||
			css_computed_content(style, &c_item) ==
			CSS_CONTENT_NORMAL) {
		/* No pseudo element */
		return;
	}

	/* create box for this element */
	computed_display = ns_computed_display(style, box_is_root(n));
	if (computed_display == CSS_DISPLAY_BLOCK ||
			computed_display == CSS_DISPLAY_TABLE) {
		/* currently only support block level boxes */

		/** \todo Not wise to drop const from the computed style */
		gen = box_create(NULL, (css_computed_style *) style,
				false, NULL, NULL, NULL, NULL, content->bctx);
		if (gen == NULL) {
			return;
		}

		/* set box type from computed display */
		gen->type = box_map[ns_computed_display(
				style, box_is_root(n))];

		box_add_child(box, gen);

		/* fixes347 - fetch background-image on the pseudo box. The
		 * existing element-level fetch at the bottom of
		 * box_construct_element fires for elements but NEVER for
		 * pseudos, so `gen->background` stays NULL forever and the
		 * texture (e.g. mactrove's --header-tile cloth pattern via
		 * `.page__header--has-tile::before { background-image:
		 * var(--header-tile); }`) is silently never painted. */
		{
			lwc_string *bgimage_uri = NULL;
			uint8_t bgimg_kind = css_computed_background_image(
				gen->style, &bgimage_uri);
			if (bgimg_kind == CSS_BACKGROUND_IMAGE_IMAGE &&
					bgimage_uri != NULL &&
					nsoption_bool(background_images)
					== true) {
				nsurl *url = NULL;
				nserror error = nsurl_create(
					lwc_string_data(bgimage_uri),
					&url);
				if (error == NSERROR_OK) {
					if (html_fetch_object(ctx->content,
							url, gen,
							image_types,
							true) == false) {
						nsurl_unref(url);
						return;
					}
					nsurl_unref(url);
				}
			}
		}
	}

	/*
	 * fixes134a -- generated content STRINGS.
	 * fixes134b -- adds CSS_COMPUTED_CONTENT_COUNTER (decimal).
	 *
	 * Walk the css_computed_content_item array and materialise:
	 *   - CSS_COMPUTED_CONTENT_STRING  -> literal text (134a)
	 *   - CSS_COMPUTED_CONTENT_COUNTER -> decimal of the named counter (134b)
	 * as a BOX_TEXT inside an INLINE_CONTAINER wrapper added as a
	 * child of `box`.
	 *
	 * The array walk is gated on css_computed_content() returning
	 * CSS_CONTENT_SET -- this is the guard fixes37/fixes38 missed,
	 * which caused iteration of an uninitialised c_item pointer when
	 * the computed-content state was NORMAL / NONE / INHERIT.
	 *
	 * Pseudo-element counter-reset / counter-increment applied
	 * before the content walk so e.g. `h2::before { counter-increment:
	 * section; content: counter(section) ". " }` produces the
	 * post-increment value.
	 *
	 * fixes140a -- adds OPEN_QUOTE / CLOSE_QUOTE / NO_OPEN_QUOTE /
	 * NO_CLOSE_QUOTE. Quote depth on box_construct_ctx is the
	 * document-wide running counter; depth indexes the resolved
	 * `quotes` list to choose which open/close pair to emit. The
	 * NO_* variants update depth but emit no characters. If the
	 * quotes list is shorter than the current depth, CSS 2.1 §12.3
	 * says the last pair is reused.
	 *
	 * Still skipped without crashing: URI, ATTR, COUNTERS (plural).
	 * Custom counter styles (roman, alpha, etc.) are decimal-only.
	 */
	{
		uint8_t cstate;
		size_t total_len;
		size_t pos;
		size_t i;
		char *text;
		struct box *container;
		struct box *text_box;
		lwc_string **quotes_arr = NULL;
		uint32_t n_quotes = 0;
		int32_t local_depth;

		cstate = css_computed_content(style, &c_item);
		if (cstate != CSS_CONTENT_SET || c_item == NULL) {
			return;
		}

		/* Resolve quotes list. CSS_QUOTES_STRING => a NULL-terminated
		 * array of lwc_string * pairs (open0, close0, open1, close1,
		 * ..., NULL). CSS_QUOTES_NONE / INHERIT-without-ancestor =>
		 * quotes_arr stays NULL and open-quote / close-quote items
		 * become inert (depth still tracked per spec). */
		{
			uint8_t qstate = css_computed_quotes(style, &quotes_arr);
			(void) qstate;
			if (quotes_arr != NULL) {
				while (quotes_arr[n_quotes] != NULL)
					n_quotes++;
			}
		}

		/* Parent must accept inline-level children. Inline-class
		 * parents (BOX_INLINE, BOX_INLINE_BLOCK, BOX_INLINE_FLEX)
		 * are now allowed; the function-entry guard at the top
		 * already rejected anything that wouldn't take inline
		 * content. The attach site below uses parent type to pick
		 * between an INLINE_CONTAINER wrapper (block parents) and
		 * direct attachment (inline parents). */

		/* If display is BLOCK/TABLE the existing `gen` empty-box
		 * path above already fired; don't double-materialise the
		 * content as a sibling inline run. (Phase A degrades that
		 * case to an empty block; correctness in 134b+.) */
		if (computed_display == CSS_DISPLAY_BLOCK ||
				computed_display == CSS_DISPLAY_TABLE) {
			return;
		}

		/* fixes140f: inline parent dispatched before it was added
		 * to its inline_container has box->parent == NULL. Bail
		 * now -- before counter and quote-depth mutations -- so
		 * the after-time re-dispatch can run them cleanly. Block
		 * parents and inline parents that ARE wired drop through. */
		if ((box->type == BOX_INLINE ||
				box->type == BOX_INLINE_BLOCK ||
				box->type == BOX_INLINE_FLEX) &&
				box->parent == NULL) {
			return;
		}

		/* fixes134b: apply pseudo's OWN counter-reset / counter-
		 * increment before resolving counter() items in content.
		 * This is the half of the ordering rule that fires inside
		 * the pseudo's lifetime; the element's NORMAL style
		 * mutations fired earlier in box_construct_element. */
		counter_apply_reset(ctx, style);
		counter_apply_increment(ctx, style);

		/* Pass 1: total byte length across STRING / COUNTER / quote
		 * items. Loop terminates at CSS_COMPUTED_CONTENT_NONE
		 * (type == 0) per libcss. local_depth walks the same path
		 * Pass 2 will, so both passes pick the same quote string
		 * for each open/close-quote item. */
		total_len = 0;
		local_depth = ctx->quote_depth;
		for (i = 0; c_item[i].type != CSS_COMPUTED_CONTENT_NONE; i++) {
			if (c_item[i].type == CSS_COMPUTED_CONTENT_STRING &&
					c_item[i].data.string != NULL) {
				total_len += lwc_string_length(
						c_item[i].data.string);
			} else if (c_item[i].type ==
					CSS_COMPUTED_CONTENT_COUNTER &&
					c_item[i].data.counter.name != NULL) {
				char nbuf[16];
				total_len += counter_fmt_decimal(
					counter_get_value(ctx,
						c_item[i].data.counter.name),
					nbuf, sizeof(nbuf));
			} else if (c_item[i].type ==
					CSS_COMPUTED_CONTENT_OPEN_QUOTE) {
				/* fixes140a: open-quote emits quotes[2*idx]
				 * then bumps depth. idx is clamped to the
				 * last pair when depth exceeds the list. */
				if (n_quotes >= 2) {
					uint32_t idx = (uint32_t)
						(2 * local_depth);
					if (idx + 1 >= n_quotes)
						idx = n_quotes - 2;
					total_len += lwc_string_length(
							quotes_arr[idx]);
				}
				local_depth++;
			} else if (c_item[i].type ==
					CSS_COMPUTED_CONTENT_CLOSE_QUOTE) {
				/* fixes140a: close-quote first decrements
				 * depth (floored at 0) then emits
				 * quotes[2*idx+1]. */
				if (local_depth > 0) local_depth--;
				if (n_quotes >= 2) {
					uint32_t idx = (uint32_t)
						(2 * local_depth);
					if (idx + 1 >= n_quotes)
						idx = n_quotes - 2;
					total_len += lwc_string_length(
							quotes_arr[idx + 1]);
				}
			} else if (c_item[i].type ==
					CSS_COMPUTED_CONTENT_NO_OPEN_QUOTE) {
				local_depth++;
			} else if (c_item[i].type ==
					CSS_COMPUTED_CONTENT_NO_CLOSE_QUOTE) {
				if (local_depth > 0) local_depth--;
			}
		}
		if (total_len == 0) {
			/* fixes140a: even when the content materialises no
			 * characters (e.g. all-no-*-quote items), commit the
			 * depth update so subsequent siblings see the right
			 * nesting level. */
			ctx->quote_depth = local_depth;
			return;
		}

		text = talloc_size(content->bctx, total_len + 1);
		if (text == NULL) {
			return;
		}

		/* Pass 2: copy. Re-walk depth from the same start so the
		 * picks match Pass 1 exactly. */
		pos = 0;
		local_depth = ctx->quote_depth;
		for (i = 0; c_item[i].type != CSS_COMPUTED_CONTENT_NONE; i++) {
			if (c_item[i].type == CSS_COMPUTED_CONTENT_STRING &&
					c_item[i].data.string != NULL) {
				const char *s = lwc_string_data(
						c_item[i].data.string);
				size_t slen = lwc_string_length(
						c_item[i].data.string);
				memcpy(text + pos, s, slen);
				pos += slen;
			} else if (c_item[i].type ==
					CSS_COMPUTED_CONTENT_COUNTER &&
					c_item[i].data.counter.name != NULL) {
				char nbuf[16];
				size_t nlen;
				nlen = counter_fmt_decimal(
					counter_get_value(ctx,
						c_item[i].data.counter.name),
					nbuf, sizeof(nbuf));
				memcpy(text + pos, nbuf, nlen);
				pos += nlen;
			} else if (c_item[i].type ==
					CSS_COMPUTED_CONTENT_OPEN_QUOTE) {
				if (n_quotes >= 2) {
					uint32_t idx = (uint32_t)
						(2 * local_depth);
					const char *s;
					size_t slen;
					if (idx + 1 >= n_quotes)
						idx = n_quotes - 2;
					s = lwc_string_data(quotes_arr[idx]);
					slen = lwc_string_length(
							quotes_arr[idx]);
					memcpy(text + pos, s, slen);
					pos += slen;
				}
				local_depth++;
			} else if (c_item[i].type ==
					CSS_COMPUTED_CONTENT_CLOSE_QUOTE) {
				if (local_depth > 0) local_depth--;
				if (n_quotes >= 2) {
					uint32_t idx = (uint32_t)
						(2 * local_depth);
					const char *s;
					size_t slen;
					if (idx + 1 >= n_quotes)
						idx = n_quotes - 2;
					s = lwc_string_data(
							quotes_arr[idx + 1]);
					slen = lwc_string_length(
							quotes_arr[idx + 1]);
					memcpy(text + pos, s, slen);
					pos += slen;
				}
			} else if (c_item[i].type ==
					CSS_COMPUTED_CONTENT_NO_OPEN_QUOTE) {
				local_depth++;
			} else if (c_item[i].type ==
					CSS_COMPUTED_CONTENT_NO_CLOSE_QUOTE) {
				if (local_depth > 0) local_depth--;
			}
			/* URI / ATTR / COUNTERS plural still silently skipped. */
		}
		text[pos] = '\0';
		ctx->quote_depth = local_depth;

		/* INLINE_CONTAINER (no style) holds the text box. Matches
		 * the canonical pattern at convert_xml_to_box_text.
		 *
		 * fixes134a-fix1: if box already has an INLINE_CONTAINER as
		 * its last child, reuse it instead of creating a sibling.
		 * Two adjacent INLINE_CONTAINERs are block-level adjacent
		 * and force a line break between them, which made ::after
		 * content render on a new line below the element text. The
		 * ::before path doesn't hit this because box has no
		 * children when the BEFORE call runs at line 700 -- so the
		 * synthetic container becomes box's only inline container
		 * and convert_children's later text additions land in the
		 * SAME container (props.inline_container tracking is reset
		 * across recursive descent, but the box-tree ordering puts
		 * BEFORE text and element text within siblings that
		 * box_normalise then merges visually on render).
		 *
		 * fixes140: inline parents (BOX_INLINE etc) take BOX_TEXT
		 * children directly -- BOX_INLINE_CONTAINER only belongs
		 * inside a block parent. The branch below picks the right
		 * shape based on box->type. */

		/* BOX_TEXT carries the pseudo style (font/colour cascade
		 * from ::before or ::after). style_owned=false because the
		 * pseudo style is owned by the parent's styles->styles slot. */
		text_box = box_create(NULL,
				(css_computed_style *) style, false,
				NULL, NULL, NULL, NULL, content->bctx);
		if (text_box == NULL) {
			return;
		}
		text_box->type = BOX_TEXT;
		text_box->text = text;
		text_box->length = pos;

		if ((box->type == BOX_INLINE ||
				box->type == BOX_INLINE_BLOCK ||
				box->type == BOX_INLINE_FLEX) &&
				box->parent != NULL) {
			/* fixes140f: inline parent. The BOX_INLINE itself
			 * is just a start marker in the parent's
			 * INLINE_CONTAINER; the inline's content lives
			 * flat as siblings of the marker, ending at
			 * BOX_INLINE_END. ::before content must go right
			 * after the INLINE marker; ::after content must
			 * go at the end of the inline_container (just
			 * before BOX_INLINE_END is created later). We
			 * detect ::after by comparing the style pointer
			 * to box->styles->styles[CSS_PSEUDO_ELEMENT_AFTER]. */
			bool is_after = false;
			if (box->styles != NULL &&
					box->styles->styles[
						CSS_PSEUDO_ELEMENT_AFTER]
					== style) {
				is_after = true;
			}
			if (is_after) {
				box_add_child(box->parent, text_box);
			} else {
				box_insert_sibling(box, text_box);
			}
		} else if (box->type == BOX_INLINE ||
				box->type == BOX_INLINE_BLOCK ||
				box->type == BOX_INLINE_FLEX) {
			/* Inline parent but box not yet wired into its
			 * inline_container -- this can happen if ::before
			 * is dispatched before the inline is added to the
			 * tree. The caller in box_construct_element_after
			 * re-dispatches once box->parent is set. */
			return;
		} else {
			/* Block parent: keep the INLINE_CONTAINER wrapper
			 * pattern from fixes134a/fix1. */
			if (box->last != NULL &&
					box->last->type == BOX_INLINE_CONTAINER) {
				container = box->last;
			} else {
				container = box_create(NULL, NULL, false,
						NULL, NULL, NULL, NULL,
						content->bctx);
				if (container == NULL) {
					return;
				}
				container->type = BOX_INLINE_CONTAINER;
				box_add_child(box, container);
			}
			box_add_child(container, text_box);
		}
	}
}


/**
 * Construct a list marker box
 *
 * \param box      Box to attach marker to
 * \param title    Current title attribute
 * \param ctx      Box construction context
 * \param parent   Current block-level container
 * \return true on success, false on memory exhaustion
 */
static bool
box_construct_marker(struct box *box,
		     const char *title,
		     struct box_construct_ctx *ctx,
		     struct box *parent)
{
	lwc_string *image_uri;
	struct box *marker;
	enum css_list_style_type_e list_style_type;

	marker = box_create(NULL, box->style, false, NULL, NULL, title,
			NULL, ctx->bctx);
	if (marker == false)
		return false;

	marker->type = BOX_BLOCK;

	list_style_type = css_computed_list_style_type(box->style);

	/** \todo marker content (list-style-type) */
	switch (list_style_type) {
	case CSS_LIST_STYLE_TYPE_DISC:
		/* fixes143a: was U+2022 BULLET ("\342\200\242"), which the
		 * macos9 conversion correctly maps to MacRoman 0xA5 -- but
		 * on G3 hardware the Helvetica TT glyph at byte 0xA5
		 * renders as something semicolon-looking instead of a
		 * bullet. Switch to U+00B7 MIDDLE DOT (UTF-8 "\302\267"),
		 * which the conversion table already maps to MacRoman 0xE1.
		 * A different font slot, a small centred dot, and visually
		 * indistinguishable from a bullet at body sizes. */
		marker->text = (char *) "\302\267";
		marker->length = 2;
		break;

	case CSS_LIST_STYLE_TYPE_CIRCLE:
		/* 25CB WHITE CIRCLE -- conversion maps to ASCII 'o' so
		 * font-independent. */
		marker->text = (char *) "\342\227\213";
		marker->length = 3;
		break;

	case CSS_LIST_STYLE_TYPE_SQUARE:
		/* 25AA BLACK SMALL SQUARE -- conversion maps to MacRoman
		 * 0xA5 (same bullet glyph slot as disc was). If hardware
		 * shows the same semicolon-looking glyph here as it did
		 * for disc, switch to ASCII '#' or 'o' in a follow-up. */
		marker->text = (char *) "\342\226\252";
		marker->length = 3;
		break;

	default:
		/* Numerical list counters get handled in layout. */
		/* Fall through. */
	case CSS_LIST_STYLE_TYPE_NONE:
		marker->text = NULL;
		marker->length = 0;
		break;
	}

	if (css_computed_list_style_image(box->style, &image_uri) == CSS_LIST_STYLE_IMAGE_URI &&
	    (image_uri != NULL) &&
	    (nsoption_bool(foreground_images) == true)) {
		nsurl *url;
		nserror error;

		/* TODO: we get a url out of libcss as a lwc string, but
		 *       earlier we already had it as a nsurl after we
		 *       nsurl_joined it.  Can this be improved?
		 *       For now, just making another nsurl. */
		error = nsurl_create(lwc_string_data(image_uri), &url);
		if (error != NSERROR_OK)
			return false;

		if (html_fetch_object(ctx->content,
				      url,
				      marker,
				      image_types,
				      false) ==	false) {
			nsurl_unref(url);
			return false;
		}
		nsurl_unref(url);
	}

	box->list_marker = marker;
	marker->parent = box;

	return true;
}

static inline bool box__style_is_float(const struct box *box)
{
	return css_computed_float(box->style) == CSS_FLOAT_LEFT ||
	       css_computed_float(box->style) == CSS_FLOAT_RIGHT;
}

static inline bool box__is_flex(const struct box *box)
{
	return box->type == BOX_FLEX || box->type == BOX_INLINE_FLEX;
}

static inline bool box__containing_block_is_flex(
		const struct box_construct_props *props)
{
	return props->containing_block != NULL &&
	       box__is_flex(props->containing_block);
}

/**
 * Construct the box tree for an XML element.
 *
 * \param ctx               Tree construction context
 * \param convert_children  Whether to convert children
 * \return  true on success, false on memory exhaustion
 */
static bool
box_construct_element(struct box_construct_ctx *ctx, bool *convert_children)
{
	dom_string *title0, *s;
	lwc_string *id = NULL;
	enum css_display_e css_display;
	struct box *box = NULL, *old_box;
	css_select_results *styles = NULL;
	/* fixes1268c (#167) - this element's custom-property environment,
	 * moved onto the box below. */
	css_custom_env *elem_custom_env = NULL;
	lwc_string *bgimage_uri;
	dom_exception err;
	struct box_construct_props props;
	const css_computed_style *root_style = NULL;

	assert(ctx->n != NULL);

	/* Skip non-rendered metadata elements unconditionally - these never
	 * generate boxes regardless of the cascade's display value. Catches
	 * the case where the UA stylesheet's display:none rules don't reach
	 * the cascade and <style>/<script> content leaks into body as text. */
	{
		dom_string *tag_name = NULL;
		if (dom_element_get_tag_name(ctx->n, &tag_name) == DOM_NO_ERR &&
				tag_name != NULL) {
			bool skip = false;
			if (dom_string_caseless_lwc_isequal(tag_name, corestring_lwc_style)) skip = true;
			else if (dom_string_caseless_lwc_isequal(tag_name, corestring_lwc_title)) skip = true;
			else if (dom_string_caseless_lwc_isequal(tag_name, corestring_lwc_meta)) skip = true;
			else if (dom_string_caseless_lwc_isequal(tag_name, corestring_lwc_link)) skip = true;
			else if (dom_string_caseless_lwc_isequal(tag_name, corestring_lwc_base)) skip = true;
			else if (dom_string_caseless_lwc_isequal(tag_name, corestring_lwc_head)) skip = true;
			dom_string_unref(tag_name);
			if (skip) {
				*convert_children = false;
				return true;
			}
		}
	}


	box_extract_properties(ctx->n, &props);

	if (props.containing_block != NULL) {
		/* In case the containing block is a pre block, we clear
		 * the PRE_STRIP flag since it is not used if we follow
		 * the pre with a tag */
		props.containing_block->flags &= ~PRE_STRIP;
	}

	if (props.node_is_root == false) {
		root_style = ctx->root_box->style;
	}

	styles = box_get_style(ctx->content, props.parent_style, root_style,
			ctx->n, props.parent_custom_env, &elem_custom_env);
	if (styles == NULL)
		return false;

	/* fixes24-33 diagnostic probes removed; cascade is healthy
	 * and any future investigation should re-add probes scoped
	 * to the specific question, not blanket-log everything. */

	/* Extract title attribute, if present */
	err = dom_element_get_attribute(ctx->n, corestring_dom_title, &title0);
	if (err != DOM_NO_ERR)
		return false;

	if (title0 != NULL) {
		char *t = squash_whitespace(dom_string_data(title0));

		dom_string_unref(title0);

		if (t == NULL)
			return false;

		props.title = talloc_strdup(ctx->bctx, t);

		free(t);

		if (props.title == NULL)
			return false;
	}

	/* Extract id attribute, if present */
	err = dom_element_get_attribute(ctx->n, corestring_dom_id, &s);
	if (err != DOM_NO_ERR)
		return false;

	if (s != NULL) {
		err = dom_string_intern(s, &id);
		if (err != DOM_NO_ERR)
			id = NULL;

		dom_string_unref(s);
	}

	/* fixes1268c (#167) - the element's custom-property environment
	 * lives on the box, which is what props.parent_custom_env reads for
	 * each child. box_create zeroes the struct, so this must follow it. */
	box = box_create(styles, styles->styles[CSS_PSEUDO_ELEMENT_NONE], false,
			props.href, props.target, props.title, id,
			ctx->bctx);
	/* fixes1063 (#114) - carry the enclosing <a>'s `download` down with
	 * href. box_special sets it on the anchor's own box below. */
	if (box != NULL && props.download)
		box->flags |= LINK_DOWNLOAD;
	if (box != NULL) {
		box->custom_env = elem_custom_env;   /* ownership moves */
		elem_custom_env = NULL;
	}
	if (box == NULL) {
		/* fixes895 - box_create/talloc_zero returned NULL. During a
		 * reconvert this is the H1 smoking gun: the double-buffer keeps a
		 * whole SECOND box tree alive, so a heavy JS page can exhaust the
		 * partition here. Durable + flushed so a bomb right after it is
		 * unambiguous. */
		if (macsurf_reconvert_in_progress) {
			macsurf_reconv_pos_set("box_create-NULL",
				(long) macsurf_reconvert_seq,
				(long) g_reconv_node_ix, "");
			macsurf_reconv_pos_flush();
			macsurf_debug_log_writef(
				"WORK reconvert #%ld: box_create NULL"
				" -- CANDIDATE H1-memory node_ix=%ld freemem=%ld",
				(long) macsurf_reconvert_seq,
				(long) g_reconv_node_ix, macsurf_free_mem());
		}
		return false;
	}

	/* If this is the root box, add it to the context */
	if (props.node_is_root)
		ctx->root_box = box;

	/* Deal with colspan/rowspan */
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

	css_display = ns_computed_display_static(box->style);

	/* Set box type from computed display */
	if ((css_computed_position(box->style) == CSS_POSITION_ABSOLUTE ||
	     css_computed_position(box->style) == CSS_POSITION_FIXED) &&
			(css_display == CSS_DISPLAY_INLINE ||
			 css_display == CSS_DISPLAY_INLINE_BLOCK ||
			 css_display == CSS_DISPLAY_INLINE_TABLE ||
			 css_display == CSS_DISPLAY_INLINE_FLEX)) {
		/* Special case for absolute positioning: make absolute inlines
		 * into inline block so that the boxes are constructed in an
		 * inline container as if they were not absolutely positioned.
		 * Layout expects and handles this. */
		box->type = box_map[CSS_DISPLAY_INLINE_BLOCK];
	} else if (props.node_is_root) {
		/* Special case for root element: force it to BLOCK, or the
		 * rest of the layout will break. */
		box->type = BOX_BLOCK;
	} else {
		/* Normal mapping */
		box->type = box_map[ns_computed_display(box->style,
				props.node_is_root)];

		if (props.containing_block->type == BOX_FLEX ||
		    props.containing_block->type == BOX_INLINE_FLEX) {
			/* Blockification */
			switch (box->type) {
			case BOX_INLINE_FLEX:
				box->type = BOX_FLEX;
				break;
			case BOX_INLINE_BLOCK:
				box->type = BOX_BLOCK;
				break;
			default:
				break;
			}
		}
		/* fixes75: grid items behave like flex items -- inline-level
		 * children are blockified inside a grid container. */
		if (props.containing_block->type == BOX_GRID ||
		    props.containing_block->type == BOX_INLINE_GRID) {
			switch (box->type) {
			case BOX_INLINE_GRID:
				box->type = BOX_GRID;
				break;
			case BOX_INLINE_FLEX:
				box->type = BOX_FLEX;
				break;
			case BOX_INLINE_BLOCK:
				box->type = BOX_BLOCK;
				break;
			default:
				break;
			}
		}
	}

	if (convert_special_elements(ctx->n,
				     ctx->content,
				     box,
				     convert_children) == false) {
		return false;
	}

	/* fixes547: convert_special_elements above yields (box_image ->
	 * html_fetch_object -> hlcache_handle_retrieve drives OT).  A
	 * content_destroy during that yield talloc_free's ctx->content->bctx
	 * (html.c html_destroy), freeing THIS box and box->style AFTER the
	 * loop-top registry gate already passed.  Everything below derefs the
	 * freed box / box->style / box->styles / ctx->n with no re-check:
	 * counter_apply(box->style), the ::before generate, ns_computed_display,
	 * css_select_results_destroy(styles) [double-free], dom_node_set_user_data,
	 * and the background-image lwc_string byte-scan (the observed lbzu crash,
	 * MacsBug-misattributed to box_image/anim_tick).  Registry membership
	 * never derefs the freed struct, so it is the only surviving signal.
	 * Return true so the loop reaches its post-element gate (which unwinds
	 * without calling ctx->cb -- the content it would broadcast to is gone). */
	if (macos9_content_is_live((struct content *)ctx->content) == 0) {
		macsurf_debug_log_writef(
			"box: DEAD after convert_special ctx=%p content=%p",
			(void *)ctx, (void *)ctx->content);
		return true;
	}

	/* fixes134b: element NORMAL counter-reset / counter-increment
	 * fire HERE, before the ::before pseudo runs. This is half of
	 * the ordering rule:
	 *   element normal mutations -> ::before -> children -> ::after
	 * with each pseudo's own (rare) counter mutations applied
	 * inside box_construct_generate just before its content
	 * materialises. */
	counter_apply_reset(ctx, box->style);
	counter_apply_increment(ctx, box->style);

	/* Handle the :before pseudo element */
	if (!(box->flags & IS_REPLACED) && box->type != BOX_CONTENTS) {
		box_construct_generate(ctx, ctx->n, ctx->content, box,
				box->styles->styles[CSS_PSEUDO_ELEMENT_BEFORE]);
	}

	/* fixes547: box_construct_generate (::before) can yield via its own
	 * html_fetch_object for a content: url() image; re-check before the
	 * box->style / styles / ctx->n derefs below. */
	if (macos9_content_is_live((struct content *)ctx->content) == 0) {
		macsurf_debug_log_writef(
			"box: DEAD after ::before generate ctx=%p content=%p",
			(void *)ctx, (void *)ctx->content);
		return true;
	}

	if (box->type == BOX_NONE || (ns_computed_display(box->style,
			props.node_is_root) == CSS_DISPLAY_NONE &&
			props.node_is_root == false)) {
		css_select_results_destroy(styles);
		box->styles = NULL;
		box->style = NULL;

		/* Invalidate associated gadget, if any */
		if (box->gadget != NULL) {
			box->gadget->box = NULL;
			box->gadget = NULL;
		}

		/* Can't do this, because the lifetimes of boxes and gadgets
		 * are inextricably linked. Fortunately, talloc will save us
		 * (for now) */
		/* box_free_box(box); */

		*convert_children = false;

		return true;
	}

	/* Attach DOM node to box */
	err = dom_node_set_user_data(ctx->n,
			corestring_dom___ns_key_box_node_data, box, NULL,
			(void *) &old_box);
	if (err != DOM_NO_ERR) {
		/* fixes895 - the box<->node backlink box_for_node() reads could
		 * not be installed; a later hover/click on this node would then
		 * resolve to a stale/absent box. */
		if (macsurf_reconvert_in_progress)
			macsurf_debug_log_writef(
				"WORK reconvert #%ld: set_user_data FAIL node_ix=%ld exc=%d",
				(long) macsurf_reconvert_seq,
				(long) g_reconv_node_ix, (int) err);
		return false;
	}

	/* Attach box to DOM node */
	box->node = dom_node_ref(ctx->n);

	if (props.inline_container == NULL &&
			(box->type == BOX_INLINE ||
			 box->type == BOX_BR ||
			 box->type == BOX_INLINE_BLOCK ||
			 box->type == BOX_INLINE_FLEX ||
			 (box__style_is_float(box) &&
			  !box__containing_block_is_flex(&props))) &&
			props.node_is_root == false) {
		/* Found an inline child of a block without a current container
		 * (i.e. this box is the first child of its parent, or was
		 * preceded by block-level siblings) */
		assert(props.containing_block != NULL &&
				"Box must have containing block.");

		props.inline_container = box_create(NULL, NULL, false, NULL,
				NULL, NULL, NULL, ctx->bctx);
		if (props.inline_container == NULL)
			return false;

		props.inline_container->type = BOX_INLINE_CONTAINER;

		box_add_child(props.containing_block, props.inline_container);
	}

	/* Kick off fetch for any background image */
	if (css_computed_background_image(box->style, &bgimage_uri) ==
			CSS_BACKGROUND_IMAGE_IMAGE && bgimage_uri != NULL &&
			nsoption_bool(background_images) == true) {
		nsurl *url;
		nserror error;

		/* TODO: we get a url out of libcss as a lwc string, but
		 *       earlier we already had it as a nsurl after we
		 *       nsurl_joined it.  Can this be improved?
		 *       For now, just making another nsurl. */
		error = nsurl_create(lwc_string_data(bgimage_uri), &url);
		if (error == NSERROR_OK) {
			/* Fetch image if we got a valid URL */
			if (html_fetch_object(ctx->content,
					      url,
					      box,
					      image_types,
					      true) == false) {
				nsurl_unref(url);
				return false;
			}
			nsurl_unref(url);
		}
	}

	/* fixes547: the background-image html_fetch_object above also yields;
	 * the SVG detection (dom_element_get_tag_name(ctx->n) byte-scan), the
	 * inline ::before generate, ns_computed_display, and box_construct_marker
	 * (its own lwc_string byte-scan) all follow with no re-check.  Gate before
	 * them; the loop's post-element gate handles the final unwind. */
	if (macos9_content_is_live((struct content *)ctx->content) == 0) {
		macsurf_debug_log_writef(
			"box: DEAD after background fetch ctx=%p content=%p",
			(void *)ctx, (void *)ctx->content);
		return true;
	}

	/* fixes195 - inline <svg> root detection.
	 *
	 * If this element is an SVG root, mark the box and tell the
	 * caller not to descend into the DOM children. The shape
	 * elements (path / rect / circle / line / etc.) stay attached
	 * to the SVG node and are rendered at paint time by the
	 * DOM-walker in macos9_svg_inline.c. They don't participate in
	 * HTML layout, which matches SVG semantics (the root <svg>
	 * draws its viewBox into a single box).
	 *
	 * fixes197 - diagnostic instrumentation: log every tag name
	 * we see so we can confirm <svg> tags actually reach this
	 * point (e.g. that Hubbub's foreign-content path isn't
	 * stashing them in a different namespace that
	 * dom_element_get_tag_name doesn't return as plain "svg"). */
	{
		dom_string *svg_name = NULL;
		if (box != NULL &&
				dom_element_get_tag_name(ctx->n, &svg_name) ==
					DOM_NO_ERR && svg_name != NULL) {
			const char *tag = (const char *)
					dom_string_data(svg_name);
			size_t tlen = dom_string_length(svg_name);
			int matched = dom_string_caseless_lwc_isequal(
					svg_name, corestring_lwc_svg);
			if (matched) {
				/* fixes202: mark the SVG root as a replaced
				 * element with given dimensions. Without this,
				 * lh__box_is_replace() returns false and the
				 * inline layout path treats the box as non-
				 * replaced - width collapses to 0 and height
				 * collapses to the parent line-height, so the
				 * macos9 SVG painter (fixes195) is invoked with
				 * a degenerate 0xN rect and nothing renders.
				 * fixes196's presentational-hint dispatch puts
				 * width/height in computed style; this flag
				 * combination makes layout actually consume
				 * them. */
				box->flags |= SVG_INLINE | IS_REPLACED |
						REPLACE_DIM;
				*convert_children = false;
			}
			/* Only log s-prefixed tags to keep noise down; <svg>
			 * always falls in this bucket. */
			if (tlen >= 3 && tlen <= 32 &&
					(tag[0] == 's' || tag[0] == 'S')) {
				macsurf_debug_log_writef(
					"svg_box: tag=%s len=%ld match=%d box=%p flags=%ld",
					tag, (long)tlen, matched,
					(void *)box, (long)(unsigned int)box->flags);
			}
			dom_string_unref(svg_name);
		}
	}

	if (*convert_children)
		box->flags |= CONVERT_CHILDREN;

	if (box->type != BOX_CONTENTS) {
		if (box->type == BOX_INLINE || box->type == BOX_BR ||
				box->type == BOX_INLINE_FLEX ||
				box->type == BOX_INLINE_BLOCK) {
			/* Inline container must exist, as we'll have
			 * created it above if it didn't */
			assert(props.inline_container != NULL);

			box_add_child(props.inline_container, box);

			/* fixes140g: ::before for inline parents fires AT
			 * ELEMENT-OPEN TIME, right after the inline is wired
			 * into its container. The earlier ::before call at line
			 * ~1200 bailed because box->parent was NULL. Firing here
			 * gives correct quote-depth nesting: outer ::before runs
			 * before any child opens, so the inner ::before sees
			 * depth=1 (set by outer ::before) instead of depth=0. */
			if (!(box->flags & IS_REPLACED) && box->styles != NULL) {
				box_construct_generate(ctx, ctx->n, ctx->content, box,
					box->styles->styles[CSS_PSEUDO_ELEMENT_BEFORE]);
			}
		} else {
			if (ns_computed_display(box->style, props.node_is_root) ==
					CSS_DISPLAY_LIST_ITEM) {
				/* List item: compute marker */
				if (box_construct_marker(box, props.title, ctx,
						props.containing_block) == false)
					return false;
			}

			if (props.node_is_root == false &&
					box__containing_block_is_flex(&props) == false &&
					(css_computed_float(box->style) ==
					CSS_FLOAT_LEFT ||
					css_computed_float(box->style) ==
					CSS_FLOAT_RIGHT)) {
				/* Float: insert a float between the parent and box. */
				struct box *flt = box_create(NULL, NULL, false,
						props.href, props.target, props.title,
						NULL, ctx->bctx);
				if (flt == NULL)
					return false;
				if (props.download)   /* fixes1063 (#114) */
					flt->flags |= LINK_DOWNLOAD;

				if (css_computed_float(box->style) == CSS_FLOAT_LEFT)
					flt->type = BOX_FLOAT_LEFT;
				else
					flt->type = BOX_FLOAT_RIGHT;

				box_add_child(props.inline_container, flt);
				box_add_child(flt, box);
			} else {
				/* Non-floated block-level box: add to containing block
				 * if there is one. If we're the root box, then there
				 * won't be. */
				if (props.containing_block != NULL)
					box_add_child(props.containing_block, box);
			}
		}
	}

	return true;
}


/**
 * Complete construction of the box tree for an element.
 *
 * \param n        DOM node to construct for
 * \param content  Containing document
 *
 * This will be called after all children of an element have been processed
 */
static void box_construct_element_after(struct box_construct_ctx *ctx,
		dom_node *n, html_content *content)
{
	struct box_construct_props props;
	struct box *box = box_for_node(n);

	assert(box != NULL);

	box_extract_properties(n, &props);

	if (box->type == BOX_INLINE || box->type == BOX_BR) {
		/* Insert INLINE_END into containing block */
		struct box *inline_end;
		bool has_children;
		dom_exception err;

		err = dom_node_has_child_nodes(n, &has_children);
		if (err != DOM_NO_ERR)
			return;

		if (has_children == false ||
				(box->flags & CONVERT_CHILDREN) == 0) {
			/* No children, or didn't want children converted */
			return;
		}

		/* fixes140g: inline ::after fires here, after all children
		 * have been processed and added to the inline_container,
		 * but BEFORE BOX_INLINE_END is created so the ::after
		 * content lands as the last sibling before INLINE_END.
		 * ::before fires earlier in box_construct_element (right
		 * after the inline is wired to its container) so quote
		 * depth nests correctly through the open-tag traversal. */
		if (!(box->flags & IS_REPLACED) && box->styles != NULL) {
			box_construct_generate(ctx, n, content, box,
				box->styles->styles[CSS_PSEUDO_ELEMENT_AFTER]);
		}

		if (props.inline_container == NULL) {
			/* Create inline container if we don't have one */
			props.inline_container = box_create(NULL, NULL, false,
					NULL, NULL, NULL, NULL, content->bctx);
			if (props.inline_container == NULL)
				return;

			props.inline_container->type = BOX_INLINE_CONTAINER;

			box_add_child(props.containing_block,
					props.inline_container);
		}

		inline_end = box_create(NULL, box->style, false,
				box->href, box->target, box->title,
				box->id == NULL ? NULL :
				lwc_string_ref(box->id), content->bctx);
		if (inline_end != NULL) {
			inline_end->type = BOX_INLINE_END;

			assert(props.inline_container != NULL);

			box_add_child(props.inline_container, inline_end);

			box->inline_end = inline_end;
			inline_end->inline_end = box;
		}
	} else if (!(box->flags & IS_REPLACED)) {
		/* Handle the :after pseudo element. fixes134b: ctx threaded
		 * through next_node so counter mutations from this pseudo
		 * and counter() resolution can use the live counter table. */
		box_construct_generate(ctx, n, content, box,
				box->styles->styles[CSS_PSEUDO_ELEMENT_AFTER]);
	}
}


/**
 * Find the next node in the DOM tree, completing element construction
 * where appropriate.
 *
 * \param n                 Current node
 * \param content           Containing content
 * \param convert_children  Whether to consider children of \a n
 * \return Next node to process, or NULL if complete
 *
 * \note \a n will be unreferenced
 */
static dom_node *
next_node(struct box_construct_ctx *ctx, dom_node *n,
		html_content *content, bool convert_children)
{
	dom_node *next = NULL;
	bool has_children;
	dom_exception err;

	err = dom_node_has_child_nodes(n, &has_children);
	if (err != DOM_NO_ERR) {
		dom_node_unref(n);
		return NULL;
	}

	if (convert_children && has_children) {
		err = dom_node_get_first_child(n, &next);
		if (err != DOM_NO_ERR) {
			dom_node_unref(n);
			return NULL;
		}
		dom_node_unref(n);
	} else {
		err = dom_node_get_next_sibling(n, &next);
		if (err != DOM_NO_ERR) {
			dom_node_unref(n);
			return NULL;
		}

		if (next != NULL) {
			if (box_for_node(n) != NULL)
				box_construct_element_after(ctx, n, content);
			dom_node_unref(n);
		} else {
			if (box_for_node(n) != NULL)
				box_construct_element_after(ctx, n, content);

			while (box_is_root(n) == false) {
				dom_node *parent = NULL;
				dom_node *parent_next = NULL;

				err = dom_node_get_parent_node(n, &parent);
				if (err != DOM_NO_ERR) {
					dom_node_unref(n);
					return NULL;
				}

				assert(parent != NULL);

				err = dom_node_get_next_sibling(parent,
						&parent_next);
				if (err != DOM_NO_ERR) {
					dom_node_unref(parent);
					dom_node_unref(n);
					return NULL;
				}

				if (parent_next != NULL) {
					dom_node_unref(parent_next);
					dom_node_unref(parent);
					break;
				}

				dom_node_unref(n);
				n = parent;
				parent = NULL;

				if (box_for_node(n) != NULL) {
					box_construct_element_after(
							ctx, n, content);
				}
			}

			if (box_is_root(n) == false) {
				dom_node *parent = NULL;

				err = dom_node_get_parent_node(n, &parent);
				if (err != DOM_NO_ERR) {
					dom_node_unref(n);
					return NULL;
				}

				assert(parent != NULL);

				err = dom_node_get_next_sibling(parent, &next);
				if (err != DOM_NO_ERR) {
					dom_node_unref(parent);
					dom_node_unref(n);
					return NULL;
				}

				if (box_for_node(parent) != NULL) {
					box_construct_element_after(ctx,
							parent, content);
				}

				dom_node_unref(parent);
			}

			dom_node_unref(n);
		}
	}

	return next;
}


/**
 * Apply the CSS text-transform property to given text for its ASCII chars.
 *
 * \param  s	string to transform
 * \param  len  length of s
 * \param  tt	transform type
 */
static void
box_text_transform(char *s, unsigned int len, enum css_text_transform_e tt)
{
	unsigned int i;
	if (len == 0)
		return;
	switch (tt) {
		case CSS_TEXT_TRANSFORM_UPPERCASE:
			for (i = 0; i < len; ++i)
				if ((unsigned char) s[i] < 0x80)
					s[i] = ascii_to_upper(s[i]);
			break;
		case CSS_TEXT_TRANSFORM_LOWERCASE:
			for (i = 0; i < len; ++i)
				if ((unsigned char) s[i] < 0x80)
					s[i] = ascii_to_lower(s[i]);
			break;
		case CSS_TEXT_TRANSFORM_CAPITALIZE:
			if ((unsigned char) s[0] < 0x80)
				s[0] = ascii_to_upper(s[0]);
			for (i = 1; i < len; ++i)
				if ((unsigned char) s[i] < 0x80 &&
						ascii_is_space(s[i - 1]))
					s[i] = ascii_to_upper(s[i]);
			break;
		default:
			break;
	}
}


/**
 * Construct the box tree for an XML text node.
 *
 * \param  ctx  Tree construction context
 * \return  true on success, false on memory exhaustion
 */
static bool box_construct_text(struct box_construct_ctx *ctx)
{
	struct box_construct_props props;
	struct box *box = NULL;
	dom_string *content;
	dom_exception err;

	assert(ctx->n != NULL);

	box_extract_properties(ctx->n, &props);

	assert(props.containing_block != NULL);

	err = dom_characterdata_get_data(ctx->n, &content);
	if (err != DOM_NO_ERR || content == NULL)
		return false;

	if (css_computed_white_space(props.parent_style) ==
			CSS_WHITE_SPACE_NORMAL ||
			css_computed_white_space(props.parent_style) ==
			CSS_WHITE_SPACE_NOWRAP) {
		char *text;

		text = squash_whitespace(dom_string_data(content));

		dom_string_unref(content);

		if (text == NULL)
			return false;

#ifdef __MACOS9__
		/* fixes491 diag - trace the source of "data-xf-init" text nodes.
		 * Log the offending character data plus the content-data pointer
		 * so the node can be tied back to its DOM origin. Remove once the
		 * leak is root-caused. */
		{
			extern void macsurf_debug_log_writef(const char *fmt,
					...);
			if (text[0] != '\0' && strstr(text, "xf-init") != NULL) {
				macsurf_debug_log_writef(
					"fixes491 TEXTNODE='%s' node=%p",
					text, (void *)ctx->n);
			}
		}
#endif

		/* if the text is just a space, combine it with the preceding
		 * text node, if any */
		if (text[0] == ' ' && text[1] == 0) {
			if (props.inline_container != NULL) {
				assert(props.inline_container->last != NULL);

				props.inline_container->last->space =
						UNKNOWN_WIDTH;
			}

			free(text);

			return true;
		}

		/* fixes811 (#251): a &shy; entity is tokenised as its OWN
		 * character token, so libhubbub emits "super"|shy|"cali"|... as
		 * separate text nodes and this function makes a separate BOX_TEXT
		 * per node. A bare U+00AD box then renders '-' via the converter's
		 * last-codepoint rule (per-syllable dashes) and is too short to
		 * reach macos9_font_split, so the soft-hyphen break code
		 * (allow_shy/last_shy) is starved and hyphens:none can't gate.
		 * Coalesce a shy-adjacent SAME-STYLE run back into the previous
		 * BOX_TEXT so the shy sits INLINE in the word: font_split then
		 * sees it and hyphens:manual/none both work. Gated on the JOINING
		 * CODEPOINT being U+00AD specifically (0xC2 0xAD), not on "was this
		 * box entity-produced", so &nbsp; (U+00A0 = 0xC2 0xA0) and ordinary
		 * entity fragmentation (AT&amp;T) stay untouched. LIMITATION (by
		 * design, tracked #275): a shy landing exactly on a STYLE boundary
		 * (<b>ab&shy;</b>cd) fragments across a different computed style, so
		 * the same-style gate won't merge it and it won't break there (the
		 * lone shy box still paints '-'); coalescing across differing fstyle
		 * would be wrong. Rare in real content; left as a known gap. */
		if (props.inline_container != NULL &&
				props.inline_container->last != NULL &&
				props.inline_container->last->type == BOX_TEXT &&
				props.inline_container->last->style ==
					props.parent_style &&
				props.inline_container->last->space == 0 &&
				props.inline_container->last->text != NULL) {
			struct box *prev = props.inline_container->last;
			size_t tlen = strlen(text);
			size_t plen = prev->length;
			int new_is_shy = (tlen >= 2 &&
				(unsigned char) text[0] == 0xC2 &&
				(unsigned char) text[1] == 0xAD);
			int prev_shy = (plen >= 2 &&
				(unsigned char) prev->text[plen - 2] == 0xC2 &&
				(unsigned char) prev->text[plen - 1] == 0xAD);
			if ((new_is_shy || prev_shy) && tlen > 0) {
				char *merged = talloc_realloc(ctx->bctx,
					prev->text, char, plen + tlen + 1);
				if (merged != NULL) {
					memcpy(merged + plen, text, tlen);
					merged[plen + tlen] = '\0';
					prev->text = merged;
					prev->length = plen + tlen;
					free(text);
					return true;
				}
			}
		}

		/* fixes813b/815 (#275): reaching here means the shy-coalesce above
		 * DECLINED to merge (different style / inline boundary), so a NEW
		 * text box is starting. Walk back past inline start/end markers and
		 * BOX_BR to the nearest preceding BOX_TEXT; if it ends with a
		 * dangling soft hyphen (nothing same-style follows, or a <br>/style
		 * boundary intervenes) that shy can never be the wrap point yet
		 * would paint a stray '-' via the converter's last-codepoint rule
		 * (cases 2a <b>ab&shy;</b>cd, 1g, 2b foo&shy;<br>bar). Strip that
		 * one trailing U+00AD. Runs at CONSTRUCTION time, before
		 * layout/font_split creates any wrap fragment, so it structurally
		 * cannot touch a real wrap fragment's trailing break-shy; removes
		 * ONLY the last codepoint, so internal shys (valid break points in
		 * a coalesced word) are untouched. Hardware-verified via a shystrip
		 * probe (only ever fired on short boundary boxes ab/cali/foo, never
		 * a long word). Remaining micro-edge: a shy before a TRAILING <br>
		 * with no following text isn't reached (no later box triggers this
		 * walk-back). */
		if (props.inline_container != NULL) {
			struct box *pv = props.inline_container->last;
			/* fixes813b: skip inline start/end markers -- a shy at the
			 * END of <b>ab&shy;</b> sits BEFORE the BOX_INLINE_END, so
			 * the direct 'last' is the marker, not the text (this is why
			 * the fixes813 direct-last check was a no-op on hardware). */
			/* fixes815 (#275): also skip BOX_BR -- a shy right before a
			 * <br> (foo&shy;<br>bar, case 2b) is dangling too (the <br>
			 * forces the break, so the shy can never be the wrap point),
			 * so reach past it to strip foo's trailing shy. */
			while (pv != NULL && (pv->type == BOX_INLINE_END ||
					pv->type == BOX_INLINE ||
					pv->type == BOX_BR))
				pv = pv->prev;
			if (pv != NULL && pv->type == BOX_TEXT &&
					pv->text != NULL && pv->length >= 2 &&
					(unsigned char) pv->text[pv->length - 2]
						== 0xC2 &&
					(unsigned char) pv->text[pv->length - 1]
						== 0xAD) {
				size_t pl = pv->length;
				pv->text[pl - 2] = '\0';
				pv->length = pl - 2;
			}
		}

		if (props.inline_container == NULL) {
			/* Child of a block without a current container
			 * (i.e. this box is the first child of its parent, or
			 * was preceded by block-level siblings) */
			props.inline_container = box_create(NULL, NULL, false,
					NULL, NULL, NULL, NULL, ctx->bctx);
			if (props.inline_container == NULL) {
				free(text);
				return false;
			}

			props.inline_container->type = BOX_INLINE_CONTAINER;

			box_add_child(props.containing_block,
					props.inline_container);
		}

		/** \todo Dropping const here is not clever */
		box = box_create(NULL,
				(css_computed_style *) props.parent_style,
				false, props.href, props.target, props.title,
				NULL, ctx->bctx);
		/* fixes1063 (#114) - THE case this exists for: a TEXT box
		 * carries href but has NO DOM node, and the anchor's own inline
		 * box is zero-width so a click never lands on it. Without this
		 * the flag is unreachable from a text link. */
		if (box != NULL && props.download)
			box->flags |= LINK_DOWNLOAD;
		if (box == NULL) {
			free(text);
			return false;
		}

		box->type = BOX_TEXT;
		macos9_box_text_created++;

		box->text = talloc_strdup(ctx->bctx, text);
		free(text);
		if (box->text == NULL)
			return false;

		box->length = strlen(box->text);

		/* strip ending space char off */
		if (box->length > 1 && box->text[box->length - 1] == ' ') {
			box->space = UNKNOWN_WIDTH;
			box->length--;
		}

		if (css_computed_text_transform(props.parent_style) !=
				CSS_TEXT_TRANSFORM_NONE)
			box_text_transform(box->text, box->length,
				css_computed_text_transform(
					props.parent_style));

		box_add_child(props.inline_container, box);

		if (box->text[0] == ' ') {
			box->length--;

			memmove(box->text, &box->text[1], box->length);

			if (box->prev != NULL)
				box->prev->space = UNKNOWN_WIDTH;
		}
	} else {
		/* white-space: pre */
		char *text;
		size_t text_len = dom_string_byte_length(content);
		size_t i;
		char *current;
		enum css_white_space_e white_space =
				css_computed_white_space(props.parent_style);

		/* fixes307 (#56): pre-wrap and pre-line now diverge from pre.
		 *   pre       - preserve whitespace + newlines, no wrap.
		 *   pre-wrap  - preserve whitespace + newlines, wrap (inline
		 *               wrap dispatch at layout.c:667-670 already
		 *               handles this - PRE_WRAP isn't in no_wrap).
		 *   pre-line  - collapse runs of internal whitespace to a
		 *               single space, but keep newlines, and wrap.
		 * The text walk below splits on \r\n so newlines are kept
		 * for all three; the new pre-line space-collapse pass below
		 * is what differentiates pre-line from pre-wrap. */
		assert(white_space == CSS_WHITE_SPACE_PRE ||
				white_space == CSS_WHITE_SPACE_PRE_LINE ||
				white_space == CSS_WHITE_SPACE_PRE_WRAP);

		text = malloc(text_len + 1);

		if (text == NULL) {
			dom_string_unref(content);
			return false;
		}

		memcpy(text, dom_string_data(content), text_len);
		text[text_len] = '\0';

		dom_string_unref(content);

		/* fixes804 (#251): expand tabs to the next tab-stop per CSS
		 * `tab-size` (css_computed_tab_size, default 8) instead of
		 * collapsing each tab to a single space, so <pre> code
		 * indentation lines up. Column-based: a tab advances to the
		 * next multiple of tab-size columns; the column resets at each
		 * newline. Falls back to the old flatten-to-space on OOM or an
		 * absurd expansion size. */
		{
			int32_t tab_size =
				css_computed_tab_size(props.parent_style);
			char *expanded = NULL;
			size_t cap;

			if (tab_size < 1) tab_size = 8;
			if (tab_size > 32) tab_size = 32;
			cap = text_len * (size_t)tab_size + 1;
			if (cap > text_len &&
					cap < (size_t)16 * 1024 * 1024)
				expanded = malloc(cap);

			if (expanded != NULL) {
				size_t src_i;
				size_t dst_i = 0;
				size_t col = 0;
				size_t spaces;
				size_t k;

				for (src_i = 0; src_i < text_len; src_i++) {
					char c = text[src_i];
					if (c == '\t') {
						spaces = (size_t)tab_size -
							(col % (size_t)tab_size);
						for (k = 0; k < spaces; k++)
							expanded[dst_i++] = ' ';
						col += spaces;
					} else {
						expanded[dst_i++] = c;
						if (c == '\n' || c == '\r')
							col = 0;
						else
							col++;
					}
				}
				expanded[dst_i] = '\0';
				free(text);
				text = expanded;
				text_len = dst_i;
			} else {
				for (i = 0; i < text_len; i++)
					if (text[i] == '\t')
						text[i] = ' ';
			}
		}

		/* fixes307 (#56) - pre-line collapses runs of horizontal
		 * whitespace (spaces, tabs - already converted to spaces
		 * above) to a single space. Newlines are preserved and
		 * handled by the \r\n split below. The collapse happens in
		 * place; text_len shrinks. */
		if (white_space == CSS_WHITE_SPACE_PRE_LINE) {
			size_t src_i = 0, dst_i = 0;
			int prev_was_space = 0;
			while (src_i < text_len) {
				char c = text[src_i++];
				if (c == ' ') {
					if (!prev_was_space) {
						text[dst_i++] = ' ';
						prev_was_space = 1;
					}
				} else {
					text[dst_i++] = c;
					prev_was_space = 0;
				}
			}
			text[dst_i] = '\0';
			text_len = dst_i;
		}

		if (css_computed_text_transform(props.parent_style) !=
				CSS_TEXT_TRANSFORM_NONE)
			box_text_transform(text, strlen(text),
				css_computed_text_transform(
						props.parent_style));

		current = text;

		/* swallow a single leading new line */
		if (props.containing_block->flags & PRE_STRIP) {
			switch (*current) {
			case '\n':
				current++;
				break;
			case '\r':
				current++;
				if (*current == '\n')
					current++;
				break;
			}
			props.containing_block->flags &= ~PRE_STRIP;
		}

		do {
			size_t len = strcspn(current, "\r\n");

			char old = current[len];

			current[len] = 0;

			if (props.inline_container == NULL) {
				/* Child of a block without a current container
				 * (i.e. this box is the first child of its
				 * parent, or was preceded by block-level
				 * siblings) */
				props.inline_container = box_create(NULL, NULL,
						false, NULL, NULL, NULL, NULL,
						ctx->bctx);
				if (props.inline_container == NULL) {
					free(text);
					return false;
				}

				props.inline_container->type =
						BOX_INLINE_CONTAINER;

				box_add_child(props.containing_block,
						props.inline_container);
			}

			/** \todo Dropping const isn't clever */
			box = box_create(NULL,
				(css_computed_style *) props.parent_style,
				false, props.href, props.target, props.title,
				NULL, ctx->bctx);
			if (box != NULL && props.download)   /* fixes1063 */
				box->flags |= LINK_DOWNLOAD;
			if (box == NULL) {
				free(text);
				return false;
			}

			box->type = BOX_TEXT;
		macos9_box_text_created++;

			box->text = talloc_strdup(ctx->bctx, current);
			if (box->text == NULL) {
				free(text);
				return false;
			}

			box->length = strlen(box->text);

			box_add_child(props.inline_container, box);

			current[len] = old;

			current += len;

			if (current[0] != '\0') {
				/* Linebreak: create new inline container */
				props.inline_container = box_create(NULL, NULL,
						false, NULL, NULL, NULL, NULL,
						ctx->bctx);
				if (props.inline_container == NULL) {
					free(text);
					return false;
				}

				props.inline_container->type =
						BOX_INLINE_CONTAINER;

				box_add_child(props.containing_block,
						props.inline_container);

				if (current[0] == '\r' && current[1] == '\n')
					current += 2;
				else
					current++;
			}
		} while (*current);

		free(text);
	}

	return true;
}


/**
 * Convert an ELEMENT node to a box tree fragment,
 * then schedule conversion of the next ELEMENT node
 */
static void convert_xml_to_box(struct box_construct_ctx *ctx); /* fixes552 wrapper, defined just below the inner */

/* fixes895 - copy the node's tag/name into a static buffer for the durable
 * position marker. Called once per batch boundary (not per node), so the single
 * transient dom_string it allocates+frees never grows the reconvert peak the H1
 * hypothesis is about. Returns "" on any failure. */
static const char *
reconv_node_tag(dom_node *n)
{
	static char buf[32];
	dom_string *name = NULL;

	buf[0] = '\0';
	if (n == NULL)
		return "(null)";
	if (dom_node_get_node_name(n, &name) == DOM_NO_ERR && name != NULL) {
		const char *d = dom_string_data(name);
		size_t len = dom_string_byte_length(name);
		if (len > 31)
			len = 31;
		if (d != NULL) {
			memcpy(buf, d, len);
			buf[len] = '\0';
		}
		dom_string_unref(name);
	}
	return buf;
}

static void convert_xml_to_box_inner(struct box_construct_ctx *ctx)
{
	dom_node *next;
	bool convert_children;
	uint32_t num_processed = 0;
	/* fixes668 (perf): raised 10 -> 100. The box walk re-schedules itself
	 * every max_processed_before_yield nodes; at 10 a ~2200-node forum page
	 * took ~223 scheduler round-trips (each a full event-loop pass) just to
	 * build the box tree once - the "box: convert_xml storm" in the log.
	 * Image/resource nodes still yield implicitly inside box_construct_element
	 * (html_fetch_object -> OT), so this only lengthens text-heavy runs to
	 * ~100 nodes (~a few ms) between explicit yields - imperceptible latency,
	 * ~10x fewer round-trips. Bigger batches also mean FEWER yield windows, so
	 * the per-batch liveness guards run less often, not more (no added UAF
	 * exposure). This does NOT touch the JS reconvert path (still disabled). */
	/* fixes902 - a RECONVERT runs to completion in ONE uninterrupted pass (no
	 * self-reschedule), i.e. an ATOMIC box build.
	 *
	 * The reconvert yielded every N nodes purely for UI responsiveness during
	 * the initial LOAD -- but a reconvert runs on an ALREADY-LOADED page (the
	 * quiesce guard requires base.active==0 to start), and the object fetches it
	 * starts are ASYNC (html_fetch_object registers them; they do not block the
	 * build -- their callbacks fire later in the poll). Every inter-batch yield
	 * therefore opened a window where an async fetch-object callback, a
	 * death-row/deferred free, or a scheduled continuation ran against the
	 * HALF-REBUILT tree -- the box-node-60/80 crash the hunt kept chasing head
	 * to head (fixes901 gated the drains; the callback was next). Removing the
	 * explicit yield closes the ENTIRE class at once: the build finishes, then
	 * html_reconvert_done runs, THEN the poll delivers callbacks/drains on the
	 * COMPLETE tree. Cost: the event loop blocks for the ~loaded-page box build
	 * (~hundreds of ms); acceptable and far better than the crash, and Layer 2
	 * (incremental) removes the whole-tree rebuild anyway. Cold LOADS keep the
	 * 100-node yield (they genuinely wait on network and must stay responsive).
	 * The cap is a large finite backstop, not a live yield point on real pages. */
	const uint32_t max_processed_before_yield =
			macsurf_reconvert_in_progress ? 0x7FFFFFFFu : 100;

	/* fixes519: validate the content BEFORE any access to it - the log line
	 * below dereferences ctx->content.  convert_xml_to_box is static and is
	 * ONLY ever invoked as a scheduled callback (guit->misc->schedule); it is
	 * not reachable directly from the window-update handler, so schedule(-1)
	 * cancellation does cover its sole entry path.  This gate additionally
	 * covers the window where the content was destroyed (handler cleared)
	 * before the cancel ran.  ctx itself is always valid here (a cancelled
	 * entry is removed and never fires, and cancel frees ctx), so only the
	 * content it points at may be gone.  On a dead content do NOT touch its
	 * DOM - document teardown is underway - just release ctx. */
	if (ctx == NULL)
		return;
	/* fixes533: validate the OWNING content by registry membership BEFORE
	 * touching ctx->content.  convert_xml_to_box is scheduled with ctx (a
	 * box_construct_ctx*, NOT the content), so the by-owner scheduler cancel
	 * in content_destroy cannot match it; this registry gate closes that
	 * window.  ctx->content is the html_content whose first member is
	 * `struct content base`, so the cast yields the registered pointer
	 * without dereferencing it.  The registry is filled at content__init
	 * (before any conversion is scheduled), so unlike the fixes521-removed
	 * hlcache walk this cannot false-negative a live conversion. */
	if (macos9_content_is_live((struct content *)ctx->content) == 0) {
		macsurf_debug_log_writef(
			"box: convert_xml content NOT LIVE (registry) ctx=%p content=%p",
			(void*)ctx, (void*)ctx->content);
		free(ctx);
		return;
	}
	/* fixes521: the fixes520 hlcache_content_is_live() gate was removed here
	 * - it risked false-negatives that abandon a live box conversion (blank
	 * page).  The field-based dead check below plus the by-value charset gate
	 * in html_fetch_object are the retained protection. */
	if (CONTENT_IS_DEAD(&ctx->content->base)) {
		macsurf_debug_log_writef(
			"box: convert_xml DEAD content ctx=%p content=%p",
			(void*)ctx, (void*)ctx->content);
		free(ctx);
		return;
	}

	/* fixes535: claim SOLE ownership of ctx now that content is confirmed
	 * live.  NULL the owner's box_conversion_context so a teardown
	 * (html_close / html_destroy / html_stop) that runs during a later yield
	 * in this walk (html_fetch_object -> hlcache_handle_retrieve drives OT)
	 * sees NULL and does NOT cancel_dom_to_box(ctx).  Without this, that
	 * cancel frees ctx out from under this still-running walk, and the next
	 * teardown path then cancels an already-freed ctx -> cancel_dom_to_box
	 * crashes decrementing the freed dom_node refcount (r30+0x04, the
	 * reported crash).  From here convert_xml_to_box owns ctx and frees it on
	 * every exit; teardown only sets aborted, which the loop checks. */
	if (ctx->content->box_conversion_context == ctx) {
		ctx->content->box_conversion_context = NULL;
	}

	macsurf_debug_log_writef("box: convert_xml ctx=%p htmlc=%p", (void*)ctx, (void*)ctx->content);

	/* fixes503: check aborted BEFORE entering the processing loop.
	 * The scheduler may have fired this callback after html_close /
	 * html_destroy already set aborted=true (the cancel_dom_to_box
	 * call in those functions removes the scheduled entry, but if
	 * the OS 9 cooperative scheduler already dequeued this callback
	 * before cancel runs, the callback fires anyway with a dead ctx).
	 * Calling ctx->cb(content, false) here is safe: html_box_convert_done
	 * checks aborted at line 235 and routes to the error path which
	 * does not touch the DOM tree. */
	if (ctx->content->aborted) {
		macsurf_debug_log_writef("box: convert_xml ABORTED early ctx=%p", (void*)ctx);
		ctx->cb(ctx->content, false);
		dom_node_unref(ctx->n);
		free(ctx);
		return;
	}

	/* fixes901 - durable marker: THIS batch's _inner actually started. If a
	 * reconvert bombs with ReconvPos at "wrapper-pre-drain" (previous batch)
	 * and never reaches "batch-enter", the death is in the inter-batch poll
	 * (death-row/deferred drain -- now gated -- or an image-fetch callback);
	 * if it reaches "batch-enter" the death is back inside box construction. */
	if (macsurf_reconvert_in_progress) {
		macsurf_reconv_pos_set("batch-enter",
			(long) macsurf_reconvert_seq, (long) g_reconv_node_ix, "");
		macsurf_reconv_pos_flush();
	}

	do {
		convert_children = true;

		if (macsurf_reconvert_in_progress)
			g_reconv_node_ix++;

		assert(ctx->n != NULL);

		/* fixes533: re-check liveness every iteration, not just at entry.
		 * The walk yields (box_construct_element -> html_fetch_object ->
		 * hlcache_handle_retrieve drives OT/the scheduler), and a bulk
		 * hlcache_clean teardown during that yield frees+reuses ctx->content
		 * AFTER the entry-time gate passed.  The registry membership test
		 * never dereferences the (possibly reused) struct.  On a no-longer-
		 * live content, abandon the walk WITHOUT calling ctx->cb (the content
		 * it would broadcast to is gone) and without touching the DOM
		 * (document teardown is underway) -- just release the node + ctx. */
		if (macos9_content_is_live((struct content *)ctx->content) == 0) {
			macsurf_debug_log_writef(
				"box: convert_xml DEAD mid-walk ctx=%p content=%p",
				(void*)ctx, (void*)ctx->content);
			dom_node_unref(ctx->n);
			free(ctx);
			return;
		}

		/* fixes897 - per-ELEMENT durable marker for the first 150 nodes of a
		 * reconvert (the crash reproduces there: HW fixes896 died in nodes
		 * 1-20 and 80-100 with freemem healthy, so it is NOT memory and NOT the
		 * text-string pin -- it is box construction reading a bad node/attr).
		 * Logs the node POINTER (safe to format even if the node is freed) and
		 * flushes BEFORE box_construct_element touches it, so a bomb inside the
		 * cascade/create leaves this node's index+ptr as the last durable
		 * position. No tag lookup here -- dom_node_get_node_name on a freed node
		 * would itself crash and hide which node it was. */
		if (macsurf_reconvert_in_progress && g_reconv_node_ix <= 150) {
			macsurf_debug_log_writef(
				"WORK reconvert #%ld: elem node=%ld ptr=%p",
				(long) macsurf_reconvert_seq, (long) g_reconv_node_ix,
				(void *) ctx->n);
			macsurf_reconv_pos_set("construct-elem",
				(long) macsurf_reconvert_seq, (long) g_reconv_node_ix, "");
			macsurf_reconv_pos_flush();
		}

		{
			bool bce_ok = box_construct_element(ctx, &convert_children);
			/* fixes547: box_construct_element yields internally
			 * (convert_special_elements / ::before generate / background
			 * fetch / marker -> html_fetch_object -> OT).  A content_destroy
			 * during any of those frees ctx->content->bctx (this box +
			 * box->style) AFTER the loop-top gate passed AND after
			 * box_construct_element's own interior gates returned true.
			 * Re-check BEFORE interpreting the result or touching
			 * ctx->n / ctx->content: on a dead content do NOT call ctx->cb
			 * (its broadcast target is gone) and do NOT next_node (derefs the
			 * freed node) -- unwind exactly like the loop-top gate. */
			if (macos9_content_is_live((struct content *)ctx->content) == 0) {
				macsurf_debug_log_writef(
					"box: DEAD after box_construct_element ctx=%p content=%p",
					(void *)ctx, (void *)ctx->content);
				dom_node_unref(ctx->n);
				free(ctx);
				return;
			}
			if (bce_ok == false) {
				if (macsurf_reconvert_in_progress) {
					macsurf_debug_log_writef(
						"WORK reconvert #%ld: box_construct_element failed"
						" node=%ld tag=%s",
						(long) macsurf_reconvert_seq,
						(long) g_reconv_node_ix,
						reconv_node_tag(ctx->n));
				}
				ctx->cb(ctx->content, false);
				dom_node_unref(ctx->n);
				free(ctx);
				return;
			}
		}

		/* Find next element to process, converting text nodes as we go */
		next = next_node(ctx, ctx->n, ctx->content, convert_children);
		while (next != NULL) {
			dom_node_type type;
			dom_exception err;

			err = dom_node_get_node_type(next, &type);
			if (err != DOM_NO_ERR) {
				ctx->cb(ctx->content, false);
				dom_node_unref(next);
				free(ctx);
				return;
			}

			if (type == DOM_ELEMENT_NODE)
				break;

			if (type == DOM_TEXT_NODE) {
				ctx->n = next;
				/* fixes897 - per-TEXT durable marker (first 150 nodes).
				 * Distinguishes a crash in box_construct_text (the
				 * dom_string read, fixes489 UAF) from one in
				 * box_construct_element (attr/cascade). */
				if (macsurf_reconvert_in_progress &&
						g_reconv_node_ix <= 150) {
					macsurf_debug_log_writef(
						"WORK reconvert #%ld: text node=%ld ptr=%p",
						(long) macsurf_reconvert_seq,
						(long) g_reconv_node_ix, (void *) next);
					macsurf_reconv_pos_set("construct-text",
						(long) macsurf_reconvert_seq,
						(long) g_reconv_node_ix, "");
					macsurf_reconv_pos_flush();
				}
				if (box_construct_text(ctx) == false) {
					ctx->cb(ctx->content, false);
					dom_node_unref(ctx->n);
					free(ctx);
					return;
				}
			}

			next = next_node(ctx, next, ctx->content, true);
		}

		ctx->n = next;

		if (next == NULL) {
			/* Conversion complete */
			struct box root;

			memset(&root, 0, sizeof(root));

			root.type = BOX_BLOCK;
			root.children = root.last = ctx->root_box;
			root.children->parent = &root;

			if (macsurf_reconvert_in_progress) {
				/* fixes895 - all nodes walked; the crash-relevant
				 * remaining steps are box_normalise_block (H3) then
				 * the ctx->cb = html_reconvert_done. Durable marker so
				 * a bomb here is not confused with a mid-walk one. */
				macsurf_reconv_pos_set("box_normalise_block",
					(long) macsurf_reconvert_seq,
					(long) g_reconv_node_ix, "");
				macsurf_reconv_pos_flush();
				macsurf_debug_log_writef(
					"WORK reconvert #%ld: convert COMPLETE nodes=%ld"
					" -> normalise+cb freemem=%ld",
					(long) macsurf_reconvert_seq,
					(long) g_reconv_node_ix, macsurf_free_mem());
			}

			/** \todo Remove box_normalise_block */
			if (box_normalise_block(&root, ctx->root_box,
					ctx->content) == false) {
				ctx->cb(ctx->content, false);
			} else {
				ctx->content->layout = root.children;
				ctx->content->layout->parent = NULL;

				ctx->cb(ctx->content, true);
			}

			assert(ctx->n == NULL);

			free(ctx);
			return;
		}
	} while (++num_processed < max_processed_before_yield);

	/* fixes536: re-expose ctx via box_conversion_context BEFORE queuing the
	 * continuation.  fixes535's entry-time claim NULLs box_conversion_context
	 * so a teardown during this batch cannot free ctx out from under the
	 * running walk.  But that also meant the SELF-RESCHEDULED continuation
	 * below was orphaned: html_close/destroy/stop only call cancel_dom_to_box
	 * when box_conversion_context != NULL, so they could not cancel this
	 * queued entry, and it fired on the next tick against freed content
	 * (convert_xml_to_box byte-scan crash on a wild pointer, misattributed by
	 * MacsBug to macos9_reload_anim_tick).  Re-arming the handle here, while
	 * the entry merely SITS in the queue (not running), lets teardown cancel
	 * it.  Safe to deref ctx->content: the loop above bailed already if it
	 * had gone dead, so reaching here means it was live throughout the batch.
	 * The next dispatch re-claims (NULLs) it at entry, keeping the batch
	 * protected.  Completion/abort paths free ctx and leave this NULL. */
	if (ctx->content != NULL) {
		ctx->content->box_conversion_context = ctx;
	}
	if (macsurf_reconvert_in_progress) {
		/* fixes895 - durable per-batch checkpoint. On a hard bomb the
		 * position file names the furthest node reached (20-node window)
		 * and the tag of the node the NEXT batch will process; freemem
		 * trends toward 0 near the crash node if this is the H1 double-
		 * buffer memory-exhaustion path. */
		macsurf_reconv_pos_set("batch-yield",
			(long) macsurf_reconvert_seq, (long) g_reconv_node_ix,
			reconv_node_tag(ctx->n));
		macsurf_reconv_pos_flush();
		macsurf_debug_log_writef(
			"WORK reconvert #%ld: batch yield node_ix=%ld next_tag=%s"
			" freemem=%ld",
			(long) macsurf_reconvert_seq, (long) g_reconv_node_ix,
			reconv_node_tag(ctx->n), macsurf_free_mem());
	}
	guit->misc->schedule(0, (void *)convert_xml_to_box, ctx);
}

/* fixes552 - wrapper around the per-batch box walk.  Marks this content's walk
 * as ON-STACK for the whole batch (including the html_fetch_object -> OT yield)
 * so content_destroy (ns_content.c) DEFERS freeing it; runs the batch via
 * _inner; then unmarks and drains any deferred frees now that the walk's
 * references are gone.  content + generation are captured BEFORE _inner runs
 * because _inner may free ctx, and it self-reschedules THIS wrapper. */
static void convert_xml_to_box(struct box_construct_ctx *ctx)
{
	struct content *c;

	if (ctx == NULL || ctx->content == NULL) {
		convert_xml_to_box_inner(ctx);
		return;
	}
	c = (struct content *)ctx->content;
	g_walk_content = c;
	g_walk_gen = macos9_content_token(c);

	convert_xml_to_box_inner(ctx);   /* may free ctx and/or reschedule */

	g_walk_content = NULL;
	g_walk_gen = 0;
	/* fixes901 - durable marker for the inter-batch gap. If a reconvert still
	 * bombs with ReconvPos at "wrapper-pre-drain" (not "batch-yield"), the
	 * schedule/return survived and the death is in the drain or the poll after;
	 * if it stays at "batch-yield", the death is the schedule/return itself.
	 * ctx may be freed here -- use globals only, no ctx deref. */
	if (macsurf_reconvert_in_progress) {
		macsurf_reconv_pos_set("wrapper-pre-drain",
			(long) macsurf_reconvert_seq, (long) g_reconv_node_ix, "");
		macsurf_reconv_pos_flush();
	}
	macos9_content_drain_deferred();
}


/* fixes130b/c/d: re-cascade an existing box tree in place.
 *
 * fixes130c: do NOT destroy old css_select_results. Marker / inline-
 * end boxes share parent's style pointer (style_owned=false) and
 * dangling would crash layout. Leak for now.
 *
 * fixes130d: convert recursion to iterative walk with a heap work
 * queue. mactrove has ~2000 boxes; Carbon's 64K stack is too small
 * for deep C recursion (NetSurf's own convert_xml_to_box is
 * iterative for the same reason). Add per-box logging every 100
 * boxes plus a per-call hard cap so a buggy walk can't run forever.
 */
extern void macsurf_debug_log_writef(const char *fmt, ...);
extern void macsurf_debug_log_write(const char *s);

struct recascade_frame {
	struct box *box;
	const css_computed_style *parent_style;
	/* fixes1268c (#167) - custom properties inherit, so a recascade
	 * must re-thread them down the tree exactly as box construction
	 * does; otherwise every var() below the root resolves against an
	 * empty environment after the first reconvert. */
	css_custom_env *parent_custom_env;
};

nserror
html_recascade_tree(html_content *c)
{
	struct recascade_frame *stack;
	int stack_cap;
	int stack_top;
	const css_computed_style *root_style;
	int processed;
	int recascaded;
	int hard_cap;

	if (c == NULL || c->layout == NULL) return NSERROR_OK;

	macsurf_debug_log_writef("recascade: enter layout=%p node=%p",
			(void *)c->layout,
			(void *)(c->layout->node));

	stack_cap = 64;
	stack = (struct recascade_frame *)malloc(
			sizeof(struct recascade_frame) * stack_cap);
	if (stack == NULL) {
		macsurf_debug_log_write("recascade: malloc fail");
		return NSERROR_NOMEM;
	}
	stack_top = 0;
	processed = 0;
	recascaded = 0;
	hard_cap = 4000;

	root_style = c->layout->style;
	stack[stack_top].box = c->layout;
	stack[stack_top].parent_style = NULL;
	stack[stack_top].parent_custom_env = NULL;
	stack_top++;

	while (stack_top > 0 && processed < hard_cap) {
		struct recascade_frame frame;
		struct box *box;
		const css_computed_style *parent_style;
		css_custom_env *parent_custom_env;
		const css_computed_style *old_self_style;
		const css_computed_style *style_for_children;
		struct box *child;

		stack_top--;
		frame = stack[stack_top];
		box = frame.box;
		parent_style = frame.parent_style;
		parent_custom_env = frame.parent_custom_env;

		processed++;
		if ((processed % 200) == 0) {
			macsurf_debug_log_writef(
				"recascade: processed=%d recascaded=%d top=%d",
				processed, recascaded, stack_top);
		}

		if (box == NULL) continue;
		old_self_style = box->style;

		if (box->node != NULL && box->styles != NULL) {
			const css_computed_style *use_root =
				(box == c->layout) ? NULL : root_style;
			const css_computed_style *use_parent =
				(box == c->layout) ? NULL : parent_style;
			css_custom_env *use_parent_env =
				(box == c->layout) ? NULL : parent_custom_env;
			css_custom_env *new_env = NULL;
			css_select_results *new_styles = box_get_style(c,
					use_parent, use_root, box->node,
					use_parent_env, &new_env);
			if (new_styles != NULL) {
				/* fixes1268c - replace, releasing the
				 * environment from the previous cascade. */
				if (box->custom_env != NULL)
					css_custom_env_unref(box->custom_env);
				box->custom_env = new_env;
				new_env = NULL;
				box->styles = new_styles;
				box->style = new_styles->styles[
						CSS_PSEUDO_ELEMENT_NONE];
				recascaded++;
				if (box == c->layout) {
					root_style = box->style;
				}

				for (child = box->children; child != NULL;
						child = child->next) {
					if (child->styles == NULL &&
							child->style ==
							old_self_style) {
						child->style = box->style;
					}
				}
			}
		}

		style_for_children = (box->style != NULL) ?
				box->style : parent_style;

		for (child = box->children; child != NULL;
				child = child->next) {
			if (stack_top >= stack_cap) {
				int new_cap = stack_cap * 2;
				struct recascade_frame *new_stack;
				new_stack = (struct recascade_frame *)realloc(
					stack,
					sizeof(struct recascade_frame) *
					new_cap);
				if (new_stack == NULL) {
					macsurf_debug_log_write(
						"recascade: realloc fail");
					free(stack);
					return NSERROR_NOMEM;
				}
				stack = new_stack;
				stack_cap = new_cap;
			}
			stack[stack_top].box = child;
			stack[stack_top].parent_style = style_for_children;
			/* fixes1268c - a box with no cascade of its own (an
			 * anonymous or text box) passes the inherited
			 * environment straight through, mirroring how
			 * style_for_children falls back to parent_style. */
			stack[stack_top].parent_custom_env =
					(box->custom_env != NULL) ?
						box->custom_env :
						parent_custom_env;
			stack_top++;
		}
	}

	macsurf_debug_log_writef(
			"recascade: done processed=%d recascaded=%d cap=%d",
			processed, recascaded, hard_cap);

	free(stack);
	return NSERROR_OK;
}


/* exported function documented in html/box_construct.h */
nserror
dom_to_box(dom_node *n,
	   html_content *c,
	   box_construct_complete_cb cb,
	   void **box_conversion_context)
{
	struct box_construct_ctx *ctx;

	assert(box_conversion_context != NULL);

	if (macsurf_reconvert_in_progress) {
		/* fixes895 - start of the re-convert box build. Reset the node
		 * index and mark ENTER so a batch-1 crash (before the first yield
		 * flush) is still bracketed here rather than at "pre-dom_to_box". */
		g_reconv_node_ix = 0;
		macsurf_reconv_pos_set("dom_to_box-ENTER",
			(long) macsurf_reconvert_seq, 0, "");
		macsurf_reconv_pos_flush();
		macsurf_debug_log_writef(
			"WORK reconvert #%ld: dom_to_box ENTER n=%p c->bctx=%p freemem=%ld",
			(long) macsurf_reconvert_seq, (void *) n, (void *) c->bctx,
			macsurf_free_mem());
	}

	if (c->bctx == NULL) {
		/* create a context allocation for this box tree */
		c->bctx = talloc_zero(0, int);
		if (c->bctx == NULL) {
			if (macsurf_reconvert_in_progress)
				macsurf_debug_log_writef(
					"WORK reconvert #%ld: dom_to_box NOMEM (bctx)"
					" -- CANDIDATE H1-memory freemem=%ld",
					(long) macsurf_reconvert_seq, macsurf_free_mem());
			return NSERROR_NOMEM;
		}
	}

	ctx = malloc(sizeof(*ctx));
	if (ctx == NULL) {
		if (macsurf_reconvert_in_progress)
			macsurf_debug_log_writef(
				"WORK reconvert #%ld: dom_to_box NOMEM (ctx)"
				" -- CANDIDATE H1-memory freemem=%ld",
				(long) macsurf_reconvert_seq, macsurf_free_mem());
		return NSERROR_NOMEM;
	}

	ctx->content = c;
	ctx->n = dom_node_ref(n);
	ctx->root_box = NULL;
	ctx->cb = cb;
	ctx->bctx = c->bctx;
	ctx->counters = NULL;	/* fixes134b: empty counter table */
	ctx->quote_depth = 0;	/* fixes140a: quote nesting starts at 0 */

	*box_conversion_context = ctx;

	/* fixes903 - a RECONVERT runs the box build SYNCHRONOUSLY, right here, with
	 * NO event-loop re-entry between teardown and repaint.
	 *
	 * The initial LOAD schedules the walk (returns to the event loop so the UI
	 * stays live while the page streams in). But a reconvert first tears the old
	 * render down -- H2 frees the object list + image bitmaps, and the fixes421
	 * double-buffer sets c->layout = NULL -- and only rebuilds on a LATER poll
	 * pass. That leaves a window where the event loop runs against a torn-down
	 * tree: fixes902 made the build itself atomic, but a redraw firing in the
	 * one poll pass BETWEEN this schedule and the walk still painted a freed
	 * GWorld with layout==NULL -> the NQDStretch/StretchBits crash
	 * (ReconvPos stuck at dom_to_box-ENTER). Every crash this hunt chased was an
	 * async event landing in a reconvert re-entry window; running the whole walk
	 * synchronously here (fixes902 already makes it one uninterrupted pass, so
	 * this one call builds the entire tree, fires html_reconvert_done, and
	 * SCHEDULES the repaint of the COMPLETE tree) collapses the layout==NULL
	 * window to within this single call -- no poll pass, no redraw, no callback,
	 * no drain can observe the half-built state. This is the endpoint of the
	 * "no interleaving during the rebuild" gate. convert_xml_to_box frees ctx on
	 * completion, so box_conversion_context is already NULLed by then. */
	if (macsurf_reconvert_in_progress) {
		convert_xml_to_box(ctx);
		return NSERROR_OK;
	}

	return guit->misc->schedule(0, (void *)convert_xml_to_box, ctx);
}


/* exported function documented in html/box_construct.h */
nserror cancel_dom_to_box(void *box_conversion_context)
{
	struct box_construct_ctx *ctx = box_conversion_context;
	nserror err;

	/* fixes535: NULL-guard ctx before the log line derefs ctx->content, and
	 * guard + NULL ctx->n around the unref so a second cancel on the same ctx
	 * (the two teardown paths html_close + html_destroy can both reach here)
	 * cannot double-decrement an already-freed dom_node refcount (r30+0x04
	 * crash).  Paired with the convert_xml_to_box ownership claim, which
	 * makes box_conversion_context NULL while a walk is in flight so this is
	 * never called on a ctx the running walk still owns. */
	if (ctx == NULL) {
		return NSERROR_OK;
	}

	macsurf_debug_log_writef("box: cancel_dom ctx=%p htmlc=%p", (void*)ctx, (void*)ctx->content);
	err = guit->misc->schedule(-1, (void *)convert_xml_to_box, ctx);
	if (err != NSERROR_OK) {
		return err;
	}

	if (ctx->n != NULL) {
		dom_node_unref(ctx->n);
		ctx->n = NULL;
	}
	free(ctx);

	return NSERROR_OK;
}


/* exported function documented in html/box_construct.h */
struct box *box_for_node(dom_node *n)
{
	struct box *box = NULL;
	dom_exception err;

	err = dom_node_get_user_data(n, corestring_dom___ns_key_box_node_data,
			(void *) &box);
	if (err != DOM_NO_ERR)
		return NULL;

	return box;
}

/* exported function documented in html/box_construct.h */
bool
box_extract_link(const html_content *content,
		 const dom_string *dsrel,
		 nsurl *base,
		 nsurl **result)
{
	char *s, *s1, *apos0 = 0, *apos1 = 0, *quot0 = 0, *quot1 = 0;
	unsigned int i, j, end;
	nserror error;
	const char *rel;

	rel = dom_string_data(dsrel);

	s1 = s = malloc(3 * strlen(rel) + 1);
	if (!s)
		return false;

	/* copy to s, removing white space and control characters */
	for (i = 0; rel[i] && ascii_is_space(rel[i]); i++)
		;
	for (end = strlen(rel);
	     (end != i) && ascii_is_space(rel[end - 1]);
	     end--)
		;
	for (j = 0; i != end; i++) {
		if ((unsigned char) rel[i] < 0x20) {
			; /* skip control characters */
		} else if (rel[i] == ' ') {
			s[j++] = '%';
			s[j++] = '2';
			s[j++] = '0';
		} else {
			s[j++] = rel[i];
		}
	}
	s[j] = 0;

	if (content->enable_scripting == false) {
		/* extract first quoted string out of "javascript:" link */
		if (strncmp(s, "javascript:", 11) == 0) {
			apos0 = strchr(s, '\'');
			if (apos0)
				apos1 = strchr(apos0 + 1, '\'');
			quot0 = strchr(s, '"');
			if (quot0)
				quot1 = strchr(quot0 + 1, '"');
			if (apos0 && apos1 &&
					(!quot0 || !quot1 || apos0 < quot0)) {
				*apos1 = 0;
				s1 = apos0 + 1;
			} else if (quot0 && quot1) {
				*quot1 = 0;
				s1 = quot0 + 1;
			}
		}
	}

	/* construct absolute URL */
	error = nsurl_join(base, s1, result);
	free(s);
	if (error != NSERROR_OK) {
		*result = NULL;
		return false;
	}

	return true;
}


/*
 * Stage 1 death-row pinned-check: does a pending convert_xml_to_box
 * continuation still reference `target`?  Lives here because the
 * box_construct_ctx struct and convert_xml_to_box are private to this
 * translation unit.  The death-row drain calls this to keep a content
 * alive while its box walk is still queued.
 */
bool
box_construct_sched_pins(void (*cb)(void *), void *p, struct content *target)
{
	struct box_construct_ctx *ctx;

	if (cb != (void (*)(void *)) convert_xml_to_box || p == NULL) {
		return false;
	}
	ctx = (struct box_construct_ctx *) p;
	return (struct content *) ctx->content == target;
}
