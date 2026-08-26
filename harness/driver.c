/* S0 harness driver — reconvert dom_string UAF repro.
 *
 * Sequence: parse a small HTML doc -> build the box tree (like the initial
 * page load) -> wire a QuickJS thread to the SAME dom_document (like
 * js_newthread(doc_priv=htmlc)) -> run a JS mutation script through the
 * REAL macsurf_qjs.c bindings (.textContent=, .setAttribute -- the same
 * C functions FB's React reconciler would call) -> call html_reconvert()
 * (the REAL fixes421 double-buffer / fixes489-gated path) -> pump its
 * resumable box walk to completion -> walk the finished tree reading every
 * text node's data exactly like box_construct_text does. ASan traps any
 * heap-use-after-free anywhere in that sequence.
 *
 * There is no real event loop here -- box construction is resumable via
 * guit->misc->schedule, so this file provides a minimal real scheduler
 * (a FIFO you drain by hand) instead of a no-op, so the SAME yield/resume
 * pattern the Mac's WaitNextEvent loop drives is actually exercised.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "quickjs.h"	/* Test 93: raw job-queue API, independent of MacSurf's
			 * js_newthread/js_exec glue (see harness/Makefile's
			 * -I$(QJS) -> quickjs-macos9/quickjs.h). */

#include <dom/dom.h>
#include "dom/bindings/hubbub/parser.h"

#include "utils/ns_errors.h"
#include "utils/nsoption.h"
#include "desktop/gui_table.h"
#include "netsurf/misc.h"
#include "netsurf/content_type.h"
#include "netsurf/plot_style.h"   /* fixes1025: plot_font_style, PLOT_STYLE_SCALE */
#include "netsurf/layout.h"       /* fixes1025: gui_layout_table */

#include "content/content_protected.h"
/* fixes879 — Test 22 exercises the real cookie jar directly (urldb.c and
 * nsurl*.c are both compiled into the harness). */
#include "utils/nsurl.h"
/* Test 38 (Phase 0 control) — corestring_dom_click, for dispatching through
 * fire_generic_dom_event() the way html_mouse_action() does. corestrings.c is
 * already in the harness link set (EXTRA_SRC) and corestrings_init() already
 * runs at startup. */
#include "utils/corestrings.h"
#include "content/urldb.h"
#include "content/handlers/html/box.h"
#include "content/handlers/html/html.h"
#include "content/handlers/css/internal.h"	/* fixes1161c: grid-template rewrite for --layout */
#include "content/handlers/html/private.h"		/* html_content, for layout_internal.h */
#include "content/handlers/html/layout_internal.h"	/* fixes929 Test 32 */
extern void macsurf_imgdims_remember(struct nsurl *url, int w, int h);
extern int macsurf_imgdims_lookup(struct nsurl *url, int *w, int *h);	/* fixes921 Test 31: struct content_html_object */
#include "content/handlers/html/private.h"
#include "content/handlers/html/box_construct.h"
#include "content/handlers/javascript/js.h"

#include "libcss/libcss.h"
#include "libcss/fpmath.h"

/* fixes1268a (#167) - Test 75 probes libcss's PRIVATE stylesheet layout.
 * Those internal headers cannot be included here: libcss's propstrings.h
 * and opcodes.h define enumerators (TOP/LEFT/RIGHT/BOTTOM, CONTENT_NONE,
 * COLUMN_WIDTH_AUTO) that collide with netsurf's own enums already in
 * this file. The probe therefore lives in its own translation unit,
 * cssprobe.c, behind the plain C API below. */
#include "cssprobe.h"

#include "macos9_content_registry.h"

extern int html_reconvert_content(struct content *c);
/* fixes866 (#292): the real parser hooks html.c uses, so the harness exercises
 * the same DOMNodeInserted -> dom_SCRIPT_showed_up path the browser does. */
#include "content/handlers/html/dom_event.h"
extern dom_hubbub_error html_process_script(void *ctx, dom_node *node);
extern void macsurf_js_set_reconvert_enabled(int enabled);
extern nserror corestrings_init(void);

/* ------------------------------------------------------------------ */
/* Minimal real scheduler: a FIFO of pending (callback, param) pairs,   */
/* draining exactly the way convert_xml_to_box_inner's self-reschedule  */
/* and the macos9 WaitNextEvent loop would.                             */
/* ------------------------------------------------------------------ */

struct sched_item {
	void (*cb)(void *p);
	void *p;
	struct sched_item *next;
};

static struct sched_item *g_sched_head = NULL;
static struct sched_item *g_sched_tail = NULL;
static int g_sched_pending = 0;

static nserror harness_schedule(int t, void (*callback)(void *p), void *p)
{
	struct sched_item *it;

	if (t < 0) {
		/* cancel: remove matching (callback, p) entries */
		struct sched_item *cur = g_sched_head, *prev = NULL;
		while (cur != NULL) {
			if (cur->cb == callback && cur->p == p) {
				struct sched_item *dead = cur;
				if (prev != NULL) prev->next = cur->next;
				else g_sched_head = cur->next;
				if (g_sched_tail == dead) g_sched_tail = prev;
				cur = cur->next;
				free(dead);
				g_sched_pending--;
			} else {
				prev = cur;
				cur = cur->next;
			}
		}
		return NSERROR_OK;
	}

	it = (struct sched_item *)malloc(sizeof(*it));
	if (it == NULL) return NSERROR_NOMEM;
	it->cb = callback;
	it->p = p;
	it->next = NULL;

	if (g_sched_tail != NULL) g_sched_tail->next = it;
	else g_sched_head = it;
	g_sched_tail = it;
	g_sched_pending++;

	return NSERROR_OK;
}

/* Drain everything currently queued (and whatever it re-queues) until
 * empty. Mirrors "keep pumping WaitNextEvent until this batch settles". */
static int harness_pump_all(int max_steps)
{
	int steps = 0;
	while (g_sched_head != NULL && steps < max_steps) {
		struct sched_item *it = g_sched_head;
		g_sched_head = it->next;
		if (g_sched_head == NULL) g_sched_tail = NULL;
		g_sched_pending--;
		it->cb(it->p);
		free(it);
		steps++;
	}
	return steps;
}

/* Pump exactly one queued step. Returns 1 if something ran, 0 if the queue
 * was empty. Used to interleave a JS mutation BETWEEN two batches of a
 * resumable box walk -- convert_xml_to_box_inner processes up to 100 nodes
 * then re-queues itself via guit->misc->schedule, so calling this once and
 * then running JS before pumping again reproduces the real WaitNextEvent
 * interleaving a long walk gets on the Mac. */
static int harness_pump_one(void)
{
	struct sched_item *it;
	if (g_sched_head == NULL) return 0;
	it = g_sched_head;
	g_sched_head = it->next;
	if (g_sched_head == NULL) g_sched_tail = NULL;
	g_sched_pending--;
	it->cb(it->p);
	free(it);
	return 1;
}

/* Parse a small independent document for multi-realm tests.  No layout or
 * content registration is needed: these tests exercise JS/DOM ownership, not
 * the box tree. */
static int harness_parse_document(const char *html, dom_document **out)
{
	dom_hubbub_parser_params params;
	dom_hubbub_parser *parser = NULL;
	dom_document *document = NULL;
	dom_hubbub_error err;

	if (out == NULL) return 0;
	*out = NULL;
	memset(&params, 0, sizeof(params));
	params.fix_enc = true;
	params.enable_script = false;
	err = dom_hubbub_parser_create(&params, &parser, &document);
	if (err != DOM_HUBBUB_OK || parser == NULL || document == NULL)
		return 0;
	err = dom_hubbub_parser_parse_chunk(parser,
			(const uint8_t *)html, strlen(html));
	if (err == DOM_HUBBUB_OK)
		err = dom_hubbub_parser_completed(parser);
	dom_hubbub_parser_destroy(parser);
	if (err != DOM_HUBBUB_OK) {
		dom_node_unref((dom_node *)document);
		return 0;
	}
	*out = document;
	return 1;
}

/* ------------------------------------------------------------------ */
/* guit + nsoptions[] -- real, correctly-typed globals (the earlier    */
/* auto-gen stubs for these were type-mismatched landmines; see        */
/* harness_gen_stubs.c).                                               */
/* ------------------------------------------------------------------ */

static struct gui_misc_table g_misc_table;
struct netsurf_table *guit = NULL;
static struct nsoption_s g_nsoptions_storage[NSOPTION_LISTEND];
struct nsoption_s *nsoptions = g_nsoptions_storage;

/* ------------------------------------------------------------------ */
/* Box tree completion callback for the INITIAL build.                  */
/* ------------------------------------------------------------------ */

/* CONTENT_IS_DEAD(c) is "(c)->handler == NULL" -- any non-NULL handler
 * pointer satisfies it. content__reformat/friends NULL-check every member
 * function before calling it, so an all-NULL vtable body is safe as long
 * as the driver never exercises a path that dereferences a specific member
 * (box construction / reconvert do not call through c->handler at all). */
static content_handler g_dummy_handler;

static int g_initial_build_done = 0;
static bool g_initial_build_ok = 0;

static void initial_build_cb(html_content *c, bool success)
{
	(void)c;
	g_initial_build_done = 1;
	g_initial_build_ok = success;
}

/* ------------------------------------------------------------------ */
/* Post-mutation tree walk: read every text node's data exactly like    */
/* box_construct_text does (dom_characterdata_get_data + dom_string_data */
/* + dom_string_unref). If any dom_node/dom_string in the CURRENT tree   */
/* is a dangling pointer, ASan traps right here.                        */
/* ------------------------------------------------------------------ */

static int g_text_nodes_read = 0;

static void walk_read_text(dom_node *node)
{
	dom_node_type type = 0;
	dom_node *child = NULL, *next = NULL;

	if (node == NULL) return;

	dom_node_get_node_type(node, &type);

	if (type == DOM_TEXT_NODE || type == DOM_CDATA_SECTION_NODE) {
		dom_string *content = NULL;
		dom_exception err = dom_characterdata_get_data(
				(dom_characterdata *)node, &content);
		if (err == DOM_NO_ERR && content != NULL) {
			const char *s = dom_string_data(content);
			size_t len = dom_string_byte_length(content);
			/* Touch every byte -- an ASan redzone/poison read
			 * anywhere in [s, s+len) trips here, not just s[0]. */
			size_t i;
			volatile unsigned char sink = 0;
			for (i = 0; i < len; i++) sink ^= (unsigned char)s[i];
			(void)sink;
			g_text_nodes_read++;
			dom_string_unref(content);
		}
	}

	dom_node_get_first_child(node, &child);
	while (child != NULL) {
		walk_read_text(child);
		dom_node_get_next_sibling(child, &next);
		dom_node_unref(child);
		child = next;
	}
}

/* Trivial URL resolver for css_stylesheet_create (mandatory field). Not
 * exercised for a UA sheet with no @import/url() references. */
static css_error harness_css_resolve_url(void *pw, const char *base,
		lwc_string *rel, lwc_string **abs)
{
	(void)pw; (void)base;
	lwc_string_ref(rel);
	*abs = rel;
	return CSS_OK;
}

/* Build a doc with N <p id="pNNN">text NNN</p> siblings inside #feed, big
 * enough that dom_to_box's 100-node yield threshold fires mid-walk (each
 * <p> + its text child = 2 box_construct nodes, so N=300 gives ~4 batches).
 * Caller frees the returned buffer. */
static char *build_large_doc(int n)
{
	size_t cap = 4096 + (size_t)n * 64;   /* fixes889: + list/float section */
	char *buf = (char *)malloc(cap);
	size_t off = 0;
	int i;
	if (buf == NULL) return NULL;
	off += (size_t)snprintf(buf + off, cap - off,
			"<html><body><div id=\"feed\">");
	for (i = 0; i < n; i++) {
		off += (size_t)snprintf(buf + off, cap - off,
				"<p id=\"p%d\">text node %d original</p>", i, i);
	}
	/* fixes889 — LIST ITEMS AND FLOATS, and they are the whole point.
	 *
	 * This fixture was <p> elements and nothing else, so Tests 1/2 have never
	 * once constructed a list_marker or a float_children box -- which are
	 * exactly the boxes html_reconvert_clear_node_boxes cannot reach (it
	 * descends children/next only; a marker hangs off box->list_marker and a
	 * float off float_children/next_float). That is why the reconvert UAF has
	 * never reproduced here while it keeps killing the G4: the harness was
	 * blind to the box shapes that carry the bug, not proving their absence.
	 *
	 * <li> gives every item a list_marker box; float:left gives the container
	 * a float_children chain. */
	off += (size_t)snprintf(buf + off, cap - off, "<ul id=\"lst\">");
	for (i = 0; i < 24; i++) {
		off += (size_t)snprintf(buf + off, cap - off,
				"<li id=\"li%d\">item %d</li>", i, i);
	}
	off += (size_t)snprintf(buf + off, cap - off, "</ul>");
	for (i = 0; i < 12; i++) {
		off += (size_t)snprintf(buf + off, cap - off,
				"<div id=\"fl%d\" style=\"float:left\">f%d</div>", i, i);
	}
	/* fixes890 — MALFORMED TABLES, to reach box_normalise's box_free() path.
	 * box_normalise discards mis-nested table boxes via box_free ->
	 * box_free_box, which destroys styles/scrollbars and then talloc_free()s
	 * the box -- running box_talloc_destructor, which destroys the SAME things
	 * again because box_free_box never nulled them. Nothing in this fixture
	 * had a table, so that path had never once run here. Stray <tr>/<td>
	 * outside a <table>, and a row group with no rows, are what force it. */
	off += (size_t)snprintf(buf + off, cap - off,
			"<table id=\"t1\"><tr><td>a</td><td>b</td></tr>"
			"<tbody></tbody>"
			"<tr><td>c</td></tr></table>");
	off += (size_t)snprintf(buf + off, cap - off,
			"<table id=\"t2\"><thead></thead><tfoot></tfoot>"
			"<tr><td>d</td></tr></table>");
	off += (size_t)snprintf(buf + off, cap - off,
			"<tr><td>orphan row</td></tr><td>orphan cell</td>");
	off += (size_t)snprintf(buf + off, cap - off, "</div></body></html>");
	return buf;
}

/* ------------------------------------------------------------------ */

/* fixes1025 — LAYOUT MODE. Run MacSurf's OWN layout over an arbitrary
 * html+css pair on Linux and dump the resulting box geometry:
 *
 *     ./reconvert_harness --layout page.html page.css 993
 *
 * Why this exists: hackaday's article river renders at 562px in MacSurf
 * where Chrome and STOCK NetSurf 3.11 both give ~1900px for the identical
 * markup and stylesheet. That difference is in our fork's layout, and every
 * previous attempt to find it cost a hardware round-trip per hypothesis.
 * The harness already compiles layout.c / layout_flex.c / layout_grid.c --
 * it simply never called layout_document, so every box kept its birth
 * width and the whole layer was untested here. Now it is bisectable in
 * seconds, locally, against a known-good reference. */
static const char *g_layout_html_path = NULL;
static const char *g_layout_css_path = NULL;
static int g_layout_width = 993;


/* fixes1025 — a synthetic font table for LAYOUT MODE.
 *
 * layout_minmax_line dereferences content->font_func, which the harness never
 * set (it never called layout). Metrics are proportional-ish rather than real
 * QuickDraw: width scales with the style's font size, which is all the
 * STRUCTURAL question needs -- whether our layout collapses a block is not a
 * question about glyph widths. Line counts will differ slightly from Chrome;
 * block heights and containment will not. */
static int harness_char_w(const struct plot_font_style *fstyle)
{
	int px = (fstyle != NULL) ? (int)(fstyle->size / PLOT_STYLE_SCALE) : 16;
	int w;
	if (px <= 0) px = 16;
	w = (px * 55) / 100;          /* ~0.55em average advance */
	return (w > 0) ? w : 1;
}

static nserror harness_font_width(const struct plot_font_style *fstyle,
		const char *string, size_t length, int *width)
{
	(void)string;
	*width = (int)length * harness_char_w(fstyle);
	return NSERROR_OK;
}

static nserror harness_font_position(const struct plot_font_style *fstyle,
		const char *string, size_t length, int x,
		size_t *char_offset, int *actual_x)
{
	int cw = harness_char_w(fstyle);
	size_t n = (x < 0) ? 0 : (size_t)(x / cw);
	(void)string;
	if (n > length) n = length;
	*char_offset = n;
	*actual_x = (int)n * cw;
	return NSERROR_OK;
}

static nserror harness_font_split(const struct plot_font_style *fstyle,
		const char *string, size_t length, int x,
		size_t *char_offset, int *actual_x)
{
	int cw = harness_char_w(fstyle);
	size_t fit = (x < 0) ? 0 : (size_t)(x / cw);
	size_t i, last_space = 0;
	if (fit >= length) {
		*char_offset = length;
		*actual_x = (int)length * cw;
		return NSERROR_OK;
	}
	for (i = 0; i < fit && i < length; i++)
		if (string[i] == ' ') last_space = i;
	/* Core's contract: offset lands ON the space, and 0 means "cannot
	 * split here" (fixes788). */
	*char_offset = last_space;
	*actual_x = (int)last_space * cw;
	return NSERROR_OK;
}

static struct gui_layout_table harness_layout_table = {
	harness_font_width,
	harness_font_position,
	harness_font_split
};

static nsurl *g_base_url = NULL;

static char *harness_slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;
	char *buf;
	if (f == NULL) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	buf = (char *)malloc((size_t)n + 1);
	if (buf == NULL) { fclose(f); return NULL; }
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
	buf[n] = '\0';
	fclose(f);
	return buf;
}

/* Identity of a box, for the dump. */
static void harness_box_brief(struct box *b, char *out, size_t cap)
{
	const char *tag = "?";
	out[0] = '\0';
    if (b->node != NULL) {
		dom_string *nm = NULL;
		if (dom_node_get_node_name(b->node, &nm) == DOM_NO_ERR && nm != NULL) {
			snprintf(out, cap, "%s", dom_string_data(nm));
			dom_string_unref(nm);
			tag = NULL;
		}
	}
	if (tag != NULL) {
		static const char *tn[] = {"BLOCK","INLINE_CONTAINER","INLINE",
			"TABLE","TABLE_ROW","TABLE_CELL","TABLE_ROW_GROUP",
			"FLOAT_LEFT","FLOAT_RIGHT","INLINE_BLOCK","BR","TEXT",
			"INLINE_END","INLINE_FLEX","FLEX","GRID","INLINE_GRID"};
		int t = (int)b->type;
		snprintf(out, cap, "<%s>", (t >= 0 && t < 17) ? tn[t] : "box");
	}
	if (b->node != NULL) {
		dom_string *cls = NULL;
		dom_string *key = NULL;
		if (dom_string_create((const uint8_t *)"class", 5, &key) == DOM_NO_ERR) {
			if (dom_element_get_attribute((dom_element *)b->node, key,
					&cls) == DOM_NO_ERR && cls != NULL) {
				size_t l = strlen(out);
				snprintf(out + l, cap - l, ".%.28s", dom_string_data(cls));
				dom_string_unref(cls);
			}
			dom_string_unref(key);
		}
	}
}

static void harness_dump_boxes(struct box *b, int depth, int maxdepth)
{
	char nm[80];
	int x = 0, y = 0;
	int fszq = -1;
	css_color colorq = 0;
	char colorbuf[16];
	if (b == NULL || depth > maxdepth) return;
	harness_box_brief(b, nm, sizeof nm);
	box_coords(b, &x, &y);
	{
		css_fixed fsq = 0; css_unit fuq = CSS_UNIT_PX;
		if (b->style != NULL &&
				css_computed_font_size(b->style, &fsq, &fuq) ==
				CSS_FONT_SIZE_DIMENSION)
			fszq = (int)FIXTOINT(fsq);
	}
	/* fixes1261 (#167) - var()-scope harness verification. Prints the
	 * computed `color` (0xRRGGBBAA) alongside the existing box dump
	 * fields so a --layout run can directly show what a var(--x) custom
	 * property actually resolved to for a given element, without adding
	 * a whole separate query mechanism. */
	colorbuf[0] = '-'; colorbuf[1] = '\0';
	if (b->style != NULL) {
		css_computed_color(b->style, &colorq);
		/* fixes1268b: css_color is 0xAARRGGBB - `>> 8` rotated the
		 * alpha into red and misreported every colour in this dump
		 * (green printed as FF0080). Mask instead. */
		sprintf(colorbuf, "#%06lX",
				(unsigned long)(colorq & 0x00FFFFFFUL));
	}
	fprintf(stderr, "%*s%-34s type=%d disp=%d fs=%d x=%d y=%d w=%d h=%d "
			"color=%s\n",
			depth * 2, "", nm, (int)b->type,
			(b->style != NULL) ?
				(int)css_computed_display_static(b->style) : -1,
			fszq, x, y,
			(b->width  >= 1000000 || b->width  < 0) ? -1 : b->width,
			(b->height >= 1000000 || b->height < 0) ? -1 : b->height,
			colorbuf);
	for (b = b->children; b != NULL; b = b->next)
		harness_dump_boxes(b, depth + 1, maxdepth);
}

/* fixes1169 (#226) — Test 66 helpers. Class-token membership read from the
 * box's OWN node via dom_element_get_attribute, token-exact so "node-extra"
 * does not match "node-extra-icon" and "avatar" does not match
 * "avatar-u2-s". */
static bool t66_cls_has(struct box *b, const char *cls)
{
	dom_string *key = NULL;
	dom_string *val = NULL;
	const char *p;
	size_t len = strlen(cls);
	bool found = false;

	if (b == NULL || b->node == NULL)
		return false;
	if (dom_string_create((const uint8_t *)"class", 5, &key) != DOM_NO_ERR)
		return false;
	if (dom_element_get_attribute((dom_element *)b->node, key, &val) ==
			DOM_NO_ERR && val != NULL) {
		p = dom_string_data(val);
		while (*p != '\0' && !found) {
			const char *sp = strchr(p, ' ');
			size_t tok = (sp != NULL) ? (size_t)(sp - p) : strlen(p);

			if (tok == len && strncmp(p, cls, len) == 0)
				found = true;
			if (sp == NULL)
				break;
			p = sp + 1;
		}
		dom_string_unref(val);
	}
	dom_string_unref(key);
	return found;
}

/* fixes1268b (#167) - Test 76 helper. Find the first box carrying `cls`
 * and report its computed color as 0xRRGGBB, or -1 if not found. */
static long t76_color_of(struct box *b, const char *cls)
{
	long r;

	if (b == NULL)
		return -1;
	if (t66_cls_has(b, cls) && b->style != NULL) {
		css_color c = 0;
		css_computed_color(b->style, &c);
		/* css_color is 0xAARRGGBB. Shifting right by 8 rotates the
		 * alpha into the red slot (green 0xFF008000 reads back as
		 * 0xFF0080) - the same mistake fixes1263 made in the FBCSS
		 * report. Mask, don't shift. */
		return (long)(c & 0x00FFFFFFUL);
	}
	for (b = b->children; b != NULL; b = b->next) {
		r = t76_color_of(b, cls);
		if (r >= 0)
			return r;
	}
	return -1;
}

/* fixes1299 (#167) - Test 95 helper. Find the first box carrying `cls`
 * and report its min-height calc slot via cssprobe, or false if not
 * found / not calc-valued. */
static bool t95_calc_of(struct box *b, const char *cls, uint8_t *slot_out)
{
	if (b == NULL)
		return false;
	if (t66_cls_has(b, cls) && b->style != NULL) {
		return cssprobe_min_height_calc_slot(b->style, slot_out);
	}
	for (b = b->children; b != NULL; b = b->next) {
		if (t95_calc_of(b, cls, slot_out))
			return true;
	}
	return false;
}

/* fixes1300 (#167) - Test 96 helper. Find and return the first box
 * carrying `cls`, or NULL. Unlike t95_calc_of, hands back the box itself
 * so the caller can read whichever computed properties it needs directly
 * (min-height here), not just the one cssprobe accessor. */
static struct box *t96_box_of_class(struct box *b, const char *cls)
{
	struct box *found;

	if (b == NULL)
		return NULL;
	if (t66_cls_has(b, cls) && b->style != NULL)
		return b;
	for (b = b->children; b != NULL; b = b->next) {
		found = t96_box_of_class(b, cls);
		if (found != NULL)
			return found;
	}
	return NULL;
}

/* Record the FIRST box whose class list holds each token. */
static void t66_walk(struct box *b,
		int *row_h, int *icon_h, int *avatar_h, int *img_h,
		int *blend_mode)
{
	if (b == NULL)
		return;
	if (t66_cls_has(b, "node-extra") && *row_h < 0)
		*row_h = b->height;
	else if (t66_cls_has(b, "node-extra-icon") && *icon_h < 0)
		*icon_h = b->height;
	else if (t66_cls_has(b, "avatar") && *avatar_h < 0) {
		*avatar_h = b->height;
		if (b->style != NULL)
			*blend_mode = css_computed_background_blend_mode(b->style);
	}
	else if (t66_cls_has(b, "avatar-u2-s") && *img_h < 0)
		*img_h = b->height;
	for (b = b->children; b != NULL; b = b->next)
		t66_walk(b, row_h, icon_h, avatar_h, img_h, blend_mode);
}

/* Test 93: raw QuickJS job-queue callbacks. File-scope because JS_EnqueueJob
 * needs a real C function pointer (JSJobFunc), not a closure. */
static int t93_a_fired = 0;
static int t93_b_fired = 0;

static JSValue t93_job_mark_a(JSContext *ctx, int argc, JSValueConst *argv)
{
	(void)ctx; (void)argc; (void)argv;
	t93_a_fired++;
	return JS_UNDEFINED;
}

static JSValue t93_job_mark_b(JSContext *ctx, int argc, JSValueConst *argv)
{
	(void)ctx; (void)argc; (void)argv;
	t93_b_fired++;
	return JS_UNDEFINED;
}

int main(int argc, char **argv)
{
	char *html_src_big = build_large_doc(300);
	const char *html_src = html_src_big;
	char *layout_html = NULL;
	char *layout_css = NULL;

	dom_hubbub_parser_params params;
	dom_hubbub_parser *parser = NULL;
	dom_document *document = NULL;
	dom_hubbub_error herr;

	html_content htmlc;
	dom_node *html_root = NULL;
	void *box_ctx = NULL;
	nserror nerr;

	css_select_ctx *select_ctx = NULL;

	struct jsheap *heap = NULL;
	struct jsthread *thread = NULL;
	/* Test 1 (sequential, matches the plan's literal S0 target): mutate a
	 * handful of EARLY elements (already box-constructed by the initial
	 * build) before reconvert starts at all -- no interleaving. */
	const char *mutate_js =
		"(function(){"
		"var i;"
		"for(i=0;i<20;i++){"
		"var b=document.getElementById('p0');"
		"if(b){b.textContent='mutated pass '+i+' hello world this is a feed item churning like React';}"
		"var a=document.getElementById('p1');"
		"if(a){a.setAttribute('data-xf-init','v'+i);}"
		"}"
		"})();";
	/* Test 2 (interleaved): mutate a LATE element -- one the reconvert walk
	 * has NOT reached yet after one 100-node batch -- injected BETWEEN two
	 * pump steps of the reconvert's own resumable walk. This is the actual
	 * cross-yield scenario the bug report describes (a JS mutation firing
	 * while dom_to_box is paused mid-walk, not before it starts). */
	const char *mutate_js_late =
		"(function(){"
		"var i;"
		"for(i=250;i<260;i++){"
		"var el=document.getElementById('p'+i);"
		"if(el){el.textContent='LATE MUTATION during reconvert yield '+i;}"
		"}"
		"})();";
	/* Defined only by the harness build. It injects a post-detach box-build
	 * failure, so we can prove that the old rendered tree is restored. */
	extern void macsurf_reconvert_test_fail_once(void);

	if (argc >= 4 && strcmp(argv[1], "--layout") == 0) {
		g_layout_html_path = argv[2];
		g_layout_css_path  = argv[3];
		if (argc >= 5) g_layout_width = atoi(argv[4]);
		layout_html = harness_slurp(g_layout_html_path);
		layout_css  = harness_slurp(g_layout_css_path);
		if (layout_html == NULL || layout_css == NULL) {
			fprintf(stderr, "FAIL: cannot read %s / %s\n",
					g_layout_html_path, g_layout_css_path);
			return 1;
		}
		html_src = layout_html;
		fprintf(stderr, "=== LAYOUT MODE: %s + %s at width %d ===\n",
				g_layout_html_path, g_layout_css_path,
				g_layout_width);
	} else {
		fprintf(stderr, "=== S0 harness: reconvert dom_string UAF repro ===\n");
	}

	/* --- wire the scheduler + option table before anything touches them --- */
	memset(&g_misc_table, 0, sizeof(g_misc_table));
	g_misc_table.schedule = harness_schedule;
	{
		static struct netsurf_table nt;
		memset(&nt, 0, sizeof(nt));
		nt.misc = &g_misc_table;
		guit = &nt;
	}
	memset(g_nsoptions_storage, 0, sizeof(g_nsoptions_storage));
	/* fixes1026 — the option table was left ALL ZERO, so every boolean
	 * option read false regardless of its documented default. That silently
	 * disabled author_level_css (inline style="" attributes are skipped
	 * entirely in box_get_style when it is false) and background_images.
	 * The fixture's own `style="float:left"` divs were therefore never
	 * floats, so fixes889's float coverage has never actually existed. */
	if (argc >= 4 && strcmp(argv[1], "--layout") == 0) {
		nsoptions[NSOPTION_author_level_css].value.b = true;
		nsoptions[NSOPTION_background_images].value.b = true;
		/* fixes1066 — foreground_images, the SAME trap fixes1026 fixed
		 * for the other two. box_image() bails at box_special.c:1792
		 * BEFORE it sets IS_REPLACED/REPLACE_DIM or resolves any
		 * dimensions:
		 *
		 *   if (nsoption_bool(foreground_images) == false) return true;
		 *
		 * so with the option false every <img> laid out as an empty
		 * inline box, w=0, regardless of its width/height attributes or
		 * CSS. Chasing #226's collapsed avatar box that looked exactly
		 * like the reported product bug -- it was the harness. With it
		 * on, a synthetic .structItem row lays out correctly: the avatar
		 * box is 48x48 with IS_REPLACED|REPLACE_DIM and the title cell
		 * starts beside it, not beneath. */
		nsoptions[NSOPTION_foreground_images].value.b = true;
	}

	corestrings_init();

	/* fixes1062 — the presentational-hint table.
	 *
	 * css_hint_init() allocates hint_ctx.hints and is called from the text/css
	 * content-handler registration (cssh_css.c:6731), which this harness never
	 * runs. Without it hint_ctx.hints is NULL and css_hint_width/height write
	 * straight through it:
	 *
	 *   struct css_hint *hint = &hint_ctx.hints[hint_ctx.len];
	 *   parse_dimension(..., &hint->data.length.value, ...)
	 *
	 * so ANY element carrying a presentational attribute -- width, height,
	 * bgcolor, border, align -- segfaulted in the cascade. That is
	 * <img width=..>, <table border=1>, <td bgcolor=..>: ordinary markup this
	 * harness could not lay out AT ALL. No test happened to use one until an
	 * <a download> repro did, and ASan named it immediately: WRITE to the zero
	 * page in parse_dimension (hints.c:239). */
	if (css_hint_init() != NSERROR_OK) {
		fprintf(stderr, "FAIL: css_hint_init\n");
		return 1;
	}

	/* fixes1026 — EVERY run needs a base URL. box_get_style passes
	 * nsurl_access(c->base_url) to nscss_create_inline_style for any
	 * element carrying style="", and box_construct joins relative
	 * hrefs against it. It was never set because author_level_css read
	 * false from the zeroed option table, so that path was dead. */
	{
		if (nsurl_create("http://local/page.html", &g_base_url) !=
				NSERROR_OK) {
			fprintf(stderr, "FAIL: base nsurl_create\n");
			return 1;
		}
		htmlc.base_url = g_base_url;
	}


	/* --- parse a real document through the real hubbub/dom parser --- */
	memset(&params, 0, sizeof(params));
	params.enc = NULL;
	params.fix_enc = true;
	/* fixes866 (#292) — mirror html.c:896-899 exactly. params.daf was NULL and
	 * enable_script false, so dom_default_action_DOMNodeInserted_cb was never
	 * registered here and the ENTIRE dynamic-<script> path (DOMNodeInserted ->
	 * dom_SCRIPT_showed_up -> html_process_script) was untestable in the
	 * harness -- which is why it took hardware round-trips to learn anything
	 * about it. With these wired, Test 11 can exercise it locally. */
	params.enable_script = true;
	params.script = html_process_script;
	params.msg = NULL;
	params.ctx = &htmlc;
	params.daf = html_dom_event_fetcher;

	herr = dom_hubbub_parser_create(&params, &parser, &document);
	if (herr != DOM_HUBBUB_OK || parser == NULL || document == NULL) {
		fprintf(stderr, "FAIL: dom_hubbub_parser_create herr=%d\n", (int)herr);
		return 1;
	}
	herr = dom_hubbub_parser_parse_chunk(parser,
			(const uint8_t *)html_src, strlen(html_src));
	if (herr != DOM_HUBBUB_OK) {
		fprintf(stderr, "FAIL: parse_chunk herr=%d\n", (int)herr);
		return 1;
	}
	herr = dom_hubbub_parser_completed(parser);
	if (herr != DOM_HUBBUB_OK) {
		fprintf(stderr, "FAIL: parser_completed herr=%d\n", (int)herr);
		return 1;
	}
	dom_hubbub_parser_destroy(parser);
	fprintf(stderr, "parsed OK, document=%p\n", (void *)document);

	/* --- build a minimal but real html_content --- */
	memset(&htmlc, 0, sizeof(htmlc));
	/* fixes1026 — restore after the memset: it is needed BEFORE the parse
	 * (script/link tags join against it) and again for box construction. */
	htmlc.base_url = g_base_url;
	htmlc.document = document;
	htmlc.quirks = DOM_DOCUMENT_QUIRKS_MODE_NONE;
	htmlc.enable_scripting = true;

	if (css_select_ctx_create(&select_ctx) != CSS_OK) {
		fprintf(stderr, "FAIL: css_select_ctx_create\n");
		return 1;
	}
	htmlc.select_ctx = select_ctx;

	/* A real UA stylesheet, even a tiny one -- css_select_style's success
	 * path with ZERO appended sheets appears to behave unexpectedly (under
	 * active investigation); real pages always have at least the UA sheet,
	 * so match that instead of chasing the zero-sheet edge case. */
	{
		css_stylesheet_params params;
		css_stylesheet *ua_sheet = NULL;
		const char *ua_css =
			"html,body,div,span,p{display:block}"
			"span{display:inline}"
			/* #310 (Test 48) — two CLASS rules so a class change is
			 * observable through the cascade. Class beats the bare element
			 * selector on specificity. Both keep a box: display:none would
			 * remove it and geometry would answer undefined (fixes1014). */
			".t48a{display:inline-block}"
			".t48b{display:flex}";
		char *ua_real = NULL;

		/* fixes1025 — LAYOUT MODE needs the REAL UA sheet. The tiny
		 * inline one above declares display:block for five elements
		 * only, so main/aside/ul/li/h2 come out INLINE and the whole
		 * document collapses into one inline run -- a harness artifact
		 * that would read exactly like the bug being hunted. */
		if (layout_html != NULL) {
			ua_real = harness_slurp(
				"../browser/netsurf/resources/default.css");
			if (ua_real == NULL)
				ua_real = harness_slurp(
					"browser/netsurf/resources/default.css");
			if (ua_real != NULL) {
				ua_css = ua_real;
				fprintf(stderr, "UA sheet: real default.css "
						"(%ld bytes)\n",
						(long)strlen(ua_real));
			} else {
				fprintf(stderr, "FAIL: real default.css not "
						"found -- layout numbers would be "
						"NOT trustworthy\n");
				return 1;
			}
		}

		memset(&params, 0, sizeof(params));
		params.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		params.level = CSS_LEVEL_3;
		params.charset = "UTF-8";
		params.url = "resource:default.css";
		params.title = "default";
		params.allow_quirks = false;
		params.inline_style = false;
		params.resolve = harness_css_resolve_url;
		params.resolve_pw = NULL;

		if (css_stylesheet_create(&params, &ua_sheet) != CSS_OK) {
			fprintf(stderr, "FAIL: css_stylesheet_create\n");
			return 1;
		}
		{
			/* fixes1026 — this used to call append_data TWICE via
			 * the && short-circuit (the second call was the test
			 * for its own success), feeding the parser the sheet
			 * body a second time mid-parse. Harmless for a 60-byte
			 * literal, not for a real 5 KB default.css. */
			css_error ae = css_stylesheet_append_data(ua_sheet,
					(const uint8_t *)ua_css, strlen(ua_css));
			css_error de;
			if (ae != CSS_OK && ae != CSS_NEEDDATA) {
				fprintf(stderr, "FAIL: ua append=%d\n", (int)ae);
				return 1;
			}
			de = css_stylesheet_data_done(ua_sheet);
			if (layout_html != NULL)
				fprintf(stderr, "ua sheet: append=%d done=%d\n",
						(int)ae, (int)de);
		}
		/* fixes1026 — MEDIA "screen", NOT NULL. html_css.c:783 passes
		 * "screen" for every sheet it appends; NULL here meant the
		 * sheet matched no medium at all, so the whole cascade
		 * silently produced initial values -- every element came out
		 * display:inline and the entire document collapsed into one
		 * inline run. The harness has therefore NEVER exercised
		 * CSS-driven box construction, which is exactly why Test 43
		 * had to inject geometry by hand. */
		if (css_select_ctx_append_sheet(select_ctx, ua_sheet,
				CSS_ORIGIN_UA, "screen") != CSS_OK) {
			fprintf(stderr, "FAIL: css_select_ctx_append_sheet\n");
			return 1;
		}
	}

	/* fixes1025 — LAYOUT MODE: the page's own stylesheet, as AUTHOR origin. */
	if (layout_css != NULL) {
		css_stylesheet_params ap;
		css_stylesheet *sheet = NULL;
		/* fixes1161c: run the real grid-template-columns/-rows
		 * preprocessing before the author sheet is parsed. Without
		 * this, --layout mode never sets -macsurf-grid, has_tracks
		 * stays false in layout_grid.c, and grid tests silently
		 * fall through to the implicit single-column path — which
		 * looked like a row-gap bug on first read of a hardware
		 * screenshot before this was added. */
		const char *css_to_parse = layout_css;
		size_t css_to_parse_len = strlen(layout_css);
		char *gt_cols = macsurf__rewrite_grid_template_columns(
				layout_css, css_to_parse_len);
		if (gt_cols != NULL) {
			css_to_parse = gt_cols;
		}
		{
			char *gt_rows = macsurf__rewrite_grid_template_rows(
					css_to_parse, css_to_parse_len);
			if (gt_rows != NULL) {
				if (gt_cols != NULL) free(gt_cols);
				gt_cols = gt_rows;
				css_to_parse = gt_rows;
			}
		}
		memset(&ap, 0, sizeof(ap));
		ap.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		ap.level = CSS_LEVEL_3;
		ap.charset = "UTF-8";
		ap.url = "http://local/page.css";
		ap.title = "author";
		ap.allow_quirks = false;
		ap.inline_style = false;
		ap.resolve = harness_css_resolve_url;
		ap.resolve_pw = NULL;
		if (css_stylesheet_create(&ap, &sheet) != CSS_OK) {
			fprintf(stderr, "FAIL: author sheet create\n"); return 1;
		}
		{
			css_error ae = css_stylesheet_append_data(sheet,
					(const uint8_t *)css_to_parse,
					css_to_parse_len);
			css_error de = css_stylesheet_data_done(sheet);
			fprintf(stderr, "author sheet: append=%d done=%d\n",
					(int)ae, (int)de);
			if (gt_cols != NULL) free(gt_cols);
		}
		if (css_select_ctx_append_sheet(select_ctx, sheet,
				CSS_ORIGIN_AUTHOR, "screen") != CSS_OK) {
			fprintf(stderr, "FAIL: author sheet append\n"); return 1;
		}
		fprintf(stderr, "author stylesheet appended (%ld bytes)\n",
				(long)strlen(layout_css));
	}

	/* fixes1026 — MEDIA TYPE. html.c:1129 sets this on every real content;
	 * the harness left it 0, so sheets appended for "screen" matched
	 * nothing and the ENTIRE cascade produced initial values. Every
	 * element came out display:inline, which is why the harness has never
	 * once exercised CSS-driven box construction. */
	/* fixes1041 — UNCONDITIONAL. fixes1026 set this only in LAYOUT MODE, so
	 * every one of the 47 JS/DOM tests still ran with media.type 0 and
	 * therefore with a DEAD CASCADE: the UA sheet is appended for "screen" a
	 * few lines above, matched nothing, and every element came out at its
	 * initial value. getComputedStyle(div#feed).display answered "inline"
	 * (the CSS initial) while the UA sheet says div{display:block}.
	 *
	 * Test 43 could not catch it: its only assertion on the computed value is
	 * `typeof r.disp !== 'string' || !r.disp`, which passes on any non-empty
	 * string INCLUDING the initial. Same shape as the fixes1005 double-fire
	 * ("did it fire" vs "how many times") and #264 ("did it bubble" vs "in
	 * what order"). Assert the VALUE, not the type. */
	htmlc.media.type = CSS_MEDIA_SCREEN;
	htmlc.media.width = INTTOFIX(g_layout_width);
	htmlc.media.height = INTTOFIX(600);
	htmlc.media.orientation = CSS_MEDIA_ORIENTATION_LANDSCAPE;
	htmlc.unit_len_ctx.viewport_width = INTTOFIX(g_layout_width);
	htmlc.unit_len_ctx.viewport_height = INTTOFIX(600);
	htmlc.unit_len_ctx.device_dpi = INTTOFIX(90);
	htmlc.unit_len_ctx.font_size_default = INTTOFIX(16);
	htmlc.unit_len_ctx.font_size_minimum = INTTOFIX(8);

	if (lwc_intern_string("*", 1, &htmlc.universal) != lwc_error_ok) {
		fprintf(stderr, "FAIL: lwc_intern_string universal\n");
		return 1;
	}

	if (layout_html != NULL) {
		uint32_t nsheets = 0;
		css_select_ctx_count_sheets(select_ctx, &nsheets);
		fprintf(stderr, "select_ctx holds %u sheet(s)\n",
				(unsigned)nsheets);
	}

	htmlc.base.status = CONTENT_STATUS_LOADING;
	htmlc.base.active = 0;
	htmlc.base.handler = &g_dummy_handler;

	macos9_content_register((struct content *)&htmlc);

	/* --- initial box construction (mirrors first page load) --- */
	nerr = dom_document_get_document_element(document, (void *)&html_root);
	if (nerr != DOM_NO_ERR || html_root == NULL) {
		fprintf(stderr, "FAIL: get_document_element\n");
		return 1;
	}

	nerr = dom_to_box(html_root, &htmlc, initial_build_cb, &box_ctx);
	dom_node_unref(html_root);
	if (nerr != NSERROR_OK) {
		fprintf(stderr, "FAIL: dom_to_box nerr=%d\n", (int)nerr);
		return 1;
	}

	harness_pump_all(100000);

	if (!g_initial_build_done || !g_initial_build_ok) {
		fprintf(stderr, "FAIL: initial build done=%d ok=%d\n",
				g_initial_build_done, (int)g_initial_build_ok);
		return 1;
	}
	fprintf(stderr, "initial box tree built OK, layout=%p\n",
			(void *)htmlc.layout);

	/* fixes1025 — LAYOUT MODE: run MacSurf's real layout and dump geometry. */
	if (layout_html != NULL) {
		extern bool layout_document(struct html_content *content,
				int width, int height);
		bool lok;
		htmlc.font_func = &harness_layout_table;
		htmlc.base.status = CONTENT_STATUS_DONE;
		htmlc.base.available_width = g_layout_width;
		htmlc.base.available_height = 600;
		lok = layout_document(&htmlc, g_layout_width, 600);
		fprintf(stderr, "\n=== layout_document -> %s ===\n",
				lok ? "OK" : "FAILED");
		if (htmlc.layout != NULL) {
			fprintf(stderr, "root: w=%d h=%d desc_y1=%d\n",
					htmlc.layout->width, htmlc.layout->height,
					htmlc.layout->descendant_y1);
		}
		fprintf(stderr, "\n=== BOX TREE (MacSurf layout) ===\n");
		harness_dump_boxes(htmlc.layout, 0, 12);
		return 0;
	}

	htmlc.base.status = CONTENT_STATUS_DONE;

	/* --- wire a JS thread to the SAME live document, like a real page --- */
	nerr = js_newheap(20000, &heap);
	if (nerr != NSERROR_OK || heap == NULL) {
		fprintf(stderr, "FAIL: js_newheap nerr=%d\n", (int)nerr);
		return 1;
	}
	nerr = js_newthread(heap, NULL, (void *)&htmlc, &thread);
	if (nerr != NSERROR_OK || thread == NULL) {
		fprintf(stderr, "FAIL: js_newthread nerr=%d\n", (int)nerr);
		return 1;
	}
	fprintf(stderr, "js thread wired to live document\n");

	macsurf_js_set_reconvert_enabled(1);

	/* fixes1274 (#167) - `--js FILE...` : execute real-world scripts through
	 * the REAL MacSurf engine (this same qjs glue, this same DOM, the same
	 * shims and the same default switch config the Mac ships), then run a
	 * final inline probe.
	 *
	 * Built because the timer audit closed off the last environment-shaped
	 * theory: on hardware the pipeline is healthy (pumps=29343, frozen=0,
	 * owner_skip=0, evicted=0, and the 2 timers that were created both
	 * fired) while Facebook's requireLazy waiters still never release. Stock
	 * qjs replays Facebook's real bundles correctly, so the divergence is
	 * something MacSurf's environment does that bare qjs does not - and
	 * bisecting that over hardware round trips is far slower than bisecting
	 * it here.
	 *
	 * Usage:
	 *   ./reconvert_harness --js a.js b.js ... [--probe 'JS']
	 * Every file runs in order in one realm; --probe runs last. Exit status
	 * is 0 only if every script executed without throwing. */
	if (argc >= 3 && strcmp(argv[1], "--js") == 0) {
		int ji;
		int jfail = 0;
		const char *probe = NULL;

		for (ji = 2; ji < argc; ji++) {
			if (strcmp(argv[ji], "--probe") == 0 && ji + 1 < argc) {
				probe = argv[ji + 1];
				break;
			}
		}
		for (ji = 2; ji < argc; ji++) {
			char *src;
			unsigned char ok;
			long t0, t1;
			extern long macsurf_monotonic_ms(void);

			if (strcmp(argv[ji], "--probe") == 0) break;
			src = harness_slurp(argv[ji]);
			if (src == NULL) {
				fprintf(stderr, "FAIL: --js cannot read %s\n",
						argv[ji]);
				return 1;
			}
			t0 = macsurf_monotonic_ms();
			ok = js_exec(thread, (const unsigned char *)src,
					strlen(src), argv[ji]);
			t1 = macsurf_monotonic_ms();
			fprintf(stderr, "--js %-40s bytes=%-9lu ok=%d %ldms\n",
					argv[ji], (unsigned long)strlen(src),
					(int)ok, t1 - t0);
			if (!ok) jfail = 1;
			free(src);
			/* let anything the script scheduled actually run, the
			 * way the event loop would between scripts */
			{
				int p;
				for (p = 0; p < 50; p++) {
					macsurf_qjs_pump_all();
					harness_pump_all(1000);
				}
			}
		}
		if (probe != NULL) {
			unsigned char ok;
			int p;
			for (p = 0; p < 200; p++) {
				macsurf_qjs_pump_all();
				harness_pump_all(1000);
			}
			ok = js_exec(thread, (const unsigned char *)probe,
					strlen(probe), "--probe");
			fprintf(stderr, "--probe ok=%d\n", (int)ok);
			if (!ok) jfail = 1;
		}
		fprintf(stderr, "=== --js MODE DONE (%s) ===\n",
				jfail ? "a script threw" : "all clean");
		return jfail;
	}

	/* --- run the real mutation path: .textContent= / .setAttribute via
	 * the REAL macsurf_qjs.c C bindings, same functions React would call --- */
	{
		unsigned char ok = js_exec(thread,
				(const unsigned char *)mutate_js,
				strlen(mutate_js), "driver-mutate.js");
		fprintf(stderr, "js_exec(mutate) ok=%d\n", (int)ok);
	}

	/* Drain anything the mutation scheduled (timers, dirty-mark reschedules
	 * from macos9_js_mark_dom_dirty -- currently a no-op in the harness, so
	 * this should be empty, but drain defensively). */
	harness_pump_all(100000);

	/* --- now the real bug-report scenario: RECONVERT the box tree from the
	 * JS-mutated DOM. This is the exact html_reconvert() path (fixes421
	 * double-buffer, fixes489 gate) -- html_reconvert_content is the thin
	 * struct-content wrapper macos9_reconvert.c calls. --- */
	htmlc.base.status = CONTENT_STATUS_DONE;
	htmlc.reflowing = false;
	htmlc.box_conversion_context = NULL;
	htmlc.aborted = false;
	htmlc.base.active = 0;

	{
		int rc = html_reconvert_content((struct content *)&htmlc);
		fprintf(stderr, "html_reconvert_content rc=%d (0=queued)\n", rc);
		if (rc != 0) {
			fprintf(stderr, "FAIL: reconvert did not queue\n");
			return 1;
		}
	}

	/* Pump the resumable reconvert box walk to completion. If the walk is
	 * long enough to yield (>100 nodes), interleave a SECOND JS mutation
	 * mid-walk here to probe the cross-yield race directly; for this small
	 * doc it will finish in one pump, which is fine -- the sequential
	 * mutate-then-reconvert case is the literal S0 target from the plan. */
	harness_pump_all(100000);

	fprintf(stderr, "reconvert pump drained, layout=%p\n",
			(void *)htmlc.layout);

	/* --- belt-and-suspenders: walk the FINISHED tree and read every text
	 * node's data exactly like box_construct_text does. Any dangling
	 * dom_node/dom_string anywhere in the current tree traps here. --- */
	{
		dom_node *root = NULL;
		dom_document_get_document_element(document, (void *)&root);
		g_text_nodes_read = 0;
		walk_read_text(root);
		if (root != NULL) dom_node_unref(root);
		fprintf(stderr, "post-reconvert tree walk read %d text node(s)\n",
				g_text_nodes_read);
	}

	fprintf(stderr, "=== Test 1 PASS: no ASan trap through initial-build + "
			"JS-mutate + reconvert + tree-walk (sequential) ===\n");

	/* --- Test 1b: a failed reconvert must be transactional. Preserve tree
	 * ownership and DOM->box identity, then fail at the point the replacement
	 * box build would begin. */
	{
		struct box *old_layout = htmlc.layout;
		void *old_bctx = htmlc.bctx;
		dom_node *root = NULL;
		struct box *root_box = NULL;
		int rc;

		fprintf(stderr, "\n=== Test 1b: failed reconvert restores old tree ===\n");
		macsurf_reconvert_test_fail_once();
		rc = html_reconvert_content((struct content *)&htmlc);
		if (rc == 0) {
			fprintf(stderr, "FAIL: forced reconvert failure reported success\n");
			return 1;
		}
		if (htmlc.layout != old_layout || htmlc.bctx != old_bctx ||
			htmlc.layout == NULL) {
			fprintf(stderr, "FAIL: rollback lost the previous render tree\n");
			return 1;
		}
		if (htmlc.unit_len_ctx.root_style != htmlc.layout->style) {
			fprintf(stderr, "FAIL: rollback left root_style detached\n");
			return 1;
		}
		if (dom_document_get_document_element(document, (void *)&root) !=
				DOM_NO_ERR || root == NULL ||
			dom_node_get_user_data(root,
					corestring_dom___ns_key_box_node_data,
					&root_box) != DOM_NO_ERR || root_box != old_layout) {
			fprintf(stderr, "FAIL: rollback did not restore DOM box identity\n");
			if (root != NULL) dom_node_unref(root);
			return 1;
		}
		dom_node_unref(root);
		fprintf(stderr, "=== Test 1b PASS: failed reconvert kept the old tree live ===\n");
	}

	/* --- Test 2: the interleaved-yield scenario. Queue ANOTHER reconvert,
	 * pump exactly ONE batch (up to 100 nodes) of its resumable walk, THEN
	 * run a JS mutation targeting elements the walk has NOT reached yet
	 * (p250-p259, near the end of document order), THEN drain the rest.
	 * This is the actual cross-yield race the bug report describes: a JS
	 * mutation firing while dom_to_box is paused mid-walk via
	 * guit->misc->schedule, not before/after it runs. --- */
	fprintf(stderr, "\n=== Test 2: interleaved mutation during reconvert yield ===\n");

	htmlc.base.status = CONTENT_STATUS_DONE;
	htmlc.reflowing = false;
	htmlc.box_conversion_context = NULL;
	htmlc.aborted = false;
	htmlc.base.active = 0;

	{
		int rc = html_reconvert_content((struct content *)&htmlc);
		fprintf(stderr, "html_reconvert_content (test2) rc=%d (0=queued)\n", rc);
		if (rc != 0) {
			fprintf(stderr, "FAIL: reconvert (test2) did not queue\n");
			return 1;
		}
	}

	{
		int batch = 0;
		int did_interleave = 0;
		while (harness_pump_one()) {
			batch++;
			if (!did_interleave) {
				/* One batch of the resumable walk just ran (up to 100
				 * nodes). Inject the late mutation now, while the walk
				 * is paused mid-tree waiting for its next scheduled
				 * continuation -- exactly the yield window. */
				unsigned char ok = js_exec(thread,
						(const unsigned char *)mutate_js_late,
						strlen(mutate_js_late), "driver-mutate-late.js");
				fprintf(stderr,
						"batch %d done; injected late mutation ok=%d\n",
						batch, (int)ok);
				did_interleave = 1;
			}
		}
		fprintf(stderr, "reconvert (test2) pump drained after %d batch(es), "
				"layout=%p\n", batch, (void *)htmlc.layout);
	}

	{
		dom_node *root = NULL;
		dom_document_get_document_element(document, (void *)&root);
		g_text_nodes_read = 0;
		walk_read_text(root);
		if (root != NULL) dom_node_unref(root);
		fprintf(stderr, "post-test2 tree walk read %d text node(s)\n",
				g_text_nodes_read);
	}

	fprintf(stderr, "=== Test 2 PASS: no ASan trap through the "
			"interleaved-yield reconvert scenario ===\n");

	/* --- Test 3 (fixes845): smoke-test the new XMLHttpRequest shim + the
	 * existing fetch() shim through the REAL js_exec path, confirming
	 * neither throws and both produce the expected WORK census lines,
	 * before trusting this new JS surface on real hardware. --- */
	fprintf(stderr, "\n=== Test 3: XMLHttpRequest + fetch shim smoke test ===\n");
	{
		const char *xhr_fetch_js =
			"(function(){"
			"var x=new XMLHttpRequest();"
			"x.open('GET','https://example.invalid/xhr-direct');"
			"x.send(null);"
			"fetch('https://example.invalid/fetch-path').then(function(r){"
			"return r.json();"
			"});"
			"})();";
		unsigned char ok = js_exec(thread,
				(const unsigned char *)xhr_fetch_js,
				strlen(xhr_fetch_js), "driver-xhr-smoke.js");
		fprintf(stderr, "js_exec(xhr+fetch smoke) ok=%d\n", (int)ok);
		if (!ok) {
			fprintf(stderr, "FAIL: xhr/fetch smoke test threw\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 3 PASS: XMLHttpRequest + fetch shims run "
			"cleanly through js_exec ===\n");

	/* --- Test 4 (fixes846, #167 S3): createTextNode / createDocumentFragment
	 * / innerHTML= are all brand-new real-DOM code paths (previously fake/
	 * no-op objects per the S1 census). Each assert throws on failure, so
	 * a regression shows up as js_exec returning !ok, not a silent pass. --- */
	fprintf(stderr, "\n=== Test 4: createTextNode/createDocumentFragment/"
			"innerHTML DOM mutation smoke test ===\n");
	{
		const char *dom_mutate_js =
			"(function(){"
			"function assert(c,m){if(!c)throw new Error('ASSERT FAIL: '+m);}"
			"var body=document.body;"
			"assert(body,'document.body exists');"

			"var el=document.createElement('div');"
			"var tn=document.createTextNode('hello-s0-text');"
			"assert(tn.nodeType===3,'text node nodeType===3, got '+tn.nodeType);"
			"el.appendChild(tn);"
			"assert(el.textContent==='hello-s0-text',"
			"'textContent after appendChild(textNode): '+el.textContent);"
			"assert(tn.nodeValue==='hello-s0-text','nodeValue readback');"
			"tn.data='changed';"
			"assert(el.textContent==='changed',"
			"'textContent reflects data= mutation: '+el.textContent);"

			"var frag=document.createDocumentFragment();"
			"assert(frag.nodeType===11,'fragment nodeType===11, got '+frag.nodeType);"
			"var a=document.createElement('span');a.textContent='A';"
			"var b=document.createElement('span');b.textContent='B';"
			"frag.appendChild(a);frag.appendChild(b);"
			"var host=document.createElement('div');"
			"host.appendChild(frag);"
			"assert(host.textContent==='AB',"
			"'fragment unwraps into host children: '+host.textContent);"

			"var h2=document.createElement('div');"
			"h2.innerHTML='<b>bold</b> and <i>italic</i>';"
			"assert(h2.textContent.indexOf('bold')>=0&&"
			"h2.textContent.indexOf('italic')>=0,"
			"'innerHTML fragment-parsed text present: '+h2.textContent);"
			"assert(h2.children&&h2.children.length>=2,"
			"'innerHTML built real child elements, length='+"
			"(h2.children?h2.children.length:'undef'));"

			"/* fixes1168 (#262/#299) — innerHTML READ-BACK via the real "
			"serializer (__getInnerHTML) + UPPERCASE tagName. The getter "
			"must return markup, not textContent; text and attribute "
			"values must be escaped; void elements must not get end "
			"tags; comments must round-trip; matches must still answer "
			"against the uppercase tag. */"
			"var rb=document.createElement('div');"
			"rb.setAttribute('id','probe-1');"
			"rb.setAttribute('data-x','v&<\\\"x');"
			"rb.innerHTML='<b>bold &amp; <i>ital</i></b><br><img src=x>"
			"<p class=\\\"c1\\\">t&amp;t</p><!-- note -->';"
			"assert(typeof rb.__getInnerHTML==='function',"
			"'__getInnerHTML native helper installed');"
			"var ih=rb.innerHTML;"
			"assert(ih.indexOf('<b>')>=0,"
			"'innerHTML getter returns markup, got: '+ih);"
			"assert(ih.indexOf('bold &amp;')>=0,"
			"'text escaped in getter output: '+ih);"
			"assert(ih.indexOf('<br>')>=0&&ih.indexOf('</br>')<0,"
			"'void element br: single tag, no end tag: '+ih);"
			"assert(ih.indexOf('<img src=\\\"x\\\">')>=0,"
			"'img with attribute serialized: '+ih);"
			"assert(ih.indexOf('class=\\\"c1\\\"')>=0,"
			"'child element attrs serialized: '+ih);"
			"assert(ih.indexOf('<!-- note -->')>=0,"
			"'comment round-trips: '+ih);"
			"assert(ih.indexOf('</p>')>=0&&ih.indexOf('</i>')>=0,"
			"'closing tags present: '+ih);"
			"/* innerHTML is descendants-only: rb's OWN attributes (id/"
			"data-x) must NOT appear in it but MUST in outerHTML */"
			"assert(ih.indexOf('probe-1')<0&&ih.indexOf('data-x')<0,"
			"'element own attrs excluded from innerHTML: '+ih);"
			"var oh=rb.outerHTML;"
			"assert(oh.indexOf('<div id=\\\"probe-1\\\" "
			"data-x=\\\"v&amp;&lt;&quot;x\\\">')===0,"
			"'outerHTML carries own attrs first: '+oh);"
			"assert(oh.indexOf('data-x=\\\"v&amp;&lt;&quot;x\\\"')>=0,"
			"'attribute value escaped (& < \\\"): '+oh);"
			"assert(rb.tagName==='DIV','tagName uppercase, got: '+rb.tagName);"
			"assert(rb.firstChild&&rb.firstChild.tagName==='B',"
			"'child tagName uppercase: '+(rb.firstChild?rb.firstChild.tagName:''));"
			"assert(rb.matches('div')===true,"
			"'matches() still answers against uppercase tagName');"
			"/* round-trip: markup read back must re-parse identically */"
			"rb.innerHTML=ih;"
			"assert(rb.children&&rb.children.length===4,"
			"'round-trip keeps structure, children='+"
			"(rb.children?rb.children.length:'undef')+' ih='+ih);"
			"assert(rb.textContent.indexOf('bold')>=0&&"
			"rb.textContent.indexOf('t&t')>=0,"
			"'round-trip keeps text: '+rb.textContent);"

			"body.appendChild(el);"
			"body.appendChild(host);"
			"body.appendChild(h2);"
			"})();";
		unsigned char ok = js_exec(thread,
				(const unsigned char *)dom_mutate_js,
				strlen(dom_mutate_js), "driver-dom-mutate.js");
		fprintf(stderr, "js_exec(dom mutate smoke) ok=%d\n", (int)ok);
		if (!ok) {
			fprintf(stderr, "FAIL: DOM mutation smoke test threw "
					"(see the assert message above)\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 4 PASS: createTextNode/createDocumentFragment/"
			"innerHTML all build real, connected DOM ===\n");

	/* --- Test 5 (fixes853): IntersectionObserver must actually FIRE. Modern
	 * feeds (Facebook) gate content load on it -- a no-op observer means the
	 * feed JS runs but never requests data. This observes an element, pumps
	 * the real JS timer arena (macsurf_qjs_pump_all, exactly what the WNE
	 * loop calls), and asserts the callback fired asynchronously with
	 * isIntersecting=true. --- */
	fprintf(stderr, "\n=== Test 5: IntersectionObserver fires the callback ===\n");
	{
		extern void macsurf_qjs_pump_all(void);
		const char *io_setup_js =
			"globalThis.__ioFired=0;globalThis.__ioIntersecting=0;"
			"(function(){"
			"var el=document.createElement('div');"
			"var io=new IntersectionObserver(function(entries){"
				"globalThis.__ioFired=1;"
				"if(entries&&entries[0]&&entries[0].isIntersecting)"
					"globalThis.__ioIntersecting=1;"
			"});"
			"io.observe(el);"
			"})();";
		const char *io_check_js =
			"if(!globalThis.__ioFired)"
				"throw new Error('ASSERT FAIL: IO callback never fired');"
			"if(!globalThis.__ioIntersecting)"
				"throw new Error('ASSERT FAIL: IO entry not isIntersecting');";
		unsigned char ok1, ok2;
		int pump;

		ok1 = js_exec(thread, (const unsigned char *)io_setup_js,
				strlen(io_setup_js), "driver-io-setup.js");
		if (!ok1) {
			fprintf(stderr, "FAIL: IO setup threw\n");
			return 1;
		}
		/* observe() scheduled the entry via setTimeout(0); the async
		 * delivery only happens when the timer arena is pumped. */
		for (pump = 0; pump < 8; pump++) {
			macsurf_qjs_pump_all();
			harness_pump_all(1000);
		}
		ok2 = js_exec(thread, (const unsigned char *)io_check_js,
				strlen(io_check_js), "driver-io-check.js");
		fprintf(stderr, "js_exec(io check) ok=%d\n", (int)ok2);
		if (!ok2) {
			fprintf(stderr, "FAIL: IntersectionObserver did not deliver "
					"an isIntersecting entry after pumping timers\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 5 PASS: IntersectionObserver delivers an "
			"isIntersecting entry asynchronously ===\n");

	/* --- Test 5c: navigator.sendBeacon (real fire-and-forget POST over
	 * the XHR slot arena) + localStorage/sessionStorage. On Linux the
	 * storage natives are compiled out (__storageLoad returns null,
	 * __storageSave is a no-op), so this exercises the shim surface +
	 * in-memory behaviour; the persistence write itself is __MACOS9__-
	 * only. sendBeacon returns false here whenever no fetcher can carry
	 * the request (no network in the harness) — the assertion is the
	 * boolean contract + no-throw, and that every invalid URL answers
	 * false. --- */
	fprintf(stderr, "\n=== Test 5c: sendBeacon + storage surface ===\n");
	{
		const char *storage_js =
			"(function(){"
			"function assert(c,m){if(!c)throw new Error('ASSERT FAIL: '+m);}"
			"assert(typeof navigator.sendBeacon==='function',"
			"'sendBeacon is a function: '+typeof navigator.sendBeacon);"
			"var r1=navigator.sendBeacon('https://example.invalid/beacon',"
			"'a=1&b=2');"
			"assert(typeof r1==='boolean','sendBeacon returns boolean, got '"
			"+typeof r1);"
			"var r2=navigator.sendBeacon('http://example.invalid/x','d');"
			"assert(typeof r2==='boolean','sendBeacon http returns boolean');"
			"var r3=navigator.sendBeacon('', 'x');"
			"assert(r3===false,'sendBeacon empty url returns false');"
			"var r4=navigator.sendBeacon('ftp://example.invalid/x');"
			"assert(r4===false,'sendBeacon non-http scheme returns false');"
			"var r5=navigator.sendBeacon();"
			"assert(r5===false,'sendBeacon no args returns false');"
			"assert(typeof localStorage==='object'&&localStorage!==null,"
			"'localStorage exists');"
			"assert(typeof localStorage.length==='number',"
			"'localStorage.length is a number');"
			"localStorage.clear();"
			"assert(localStorage.length===0,'clear empties');"
			"assert(localStorage.getItem('k')===null,"
			"'getItem missing returns null');"
			"localStorage.setItem('k','v1');"
			"assert(localStorage.getItem('k')==='v1',"
			"'setItem/getItem round-trip: '+localStorage.getItem('k'));"
			"assert(localStorage.length===1,'length after set');"
			"localStorage.setItem('k',42);"
			"assert(localStorage.getItem('k')==='42',"
			"'setItem coerces to string: '+localStorage.getItem('k'));"
			"localStorage.setItem('k2','v2');"
			"assert(localStorage.key(0)!==null&&localStorage.key(1)!==null,"
			"'key(i) enumerates');"
			"localStorage.removeItem('k');"
			"assert(localStorage.getItem('k')===null&&"
			"localStorage.length===1,'removeItem deletes');"
			"localStorage.clear();"
			"assert(localStorage.length===0,'clear after remove');"
			"assert(localStorage._persist===true,"
			"'localStorage persistence flag set');"
			"assert(typeof __storageLoad==='function'&&"
			"typeof __storageSave==='function',"
			"'storage natives installed');"
			"var ld=__storageLoad();"
			"assert(ld===null||typeof ld==='string',"
			"'__storageLoad returns null or string on Linux, got '"
			"+typeof ld);"
			"var saved='';"
			"var realSave=__storageSave;"
			"__storageSave=function(j){saved=j;return realSave(j);};"
			"localStorage.setItem('spy','yes');"
			"assert(saved.indexOf('\"spy\":\"yes\"')>=0,"
			"'setItem triggers __storageSave with the JSON map: '+saved);"
			"localStorage.removeItem('spy');"
			"assert(saved.indexOf('\"spy\"')<0,"
			"'removeItem triggers __storageSave: '+saved);"
			"localStorage.clear();"
			"assert(saved==='{}','clear saves empty map: '+saved);"
			"__storageSave=realSave;"
			"assert(typeof sessionStorage==='object',"
			"'sessionStorage exists');"
			"assert(sessionStorage._persist!==true,"
			"'sessionStorage does NOT persist');"
			"sessionStorage.setItem('s','t');"
			"assert(sessionStorage.getItem('s')==='t',"
			"'sessionStorage round-trip');"
			"assert(localStorage.getItem('s')===null,"
			"'local/session storage are independent');"
			"sessionStorage.clear();"
			"assert(sessionStorage.length===0,'sessionStorage clear');"
			"})();";
		unsigned char ok = js_exec(thread,
				(const unsigned char *)storage_js,
				strlen(storage_js), "driver-storage.js");
		fprintf(stderr, "js_exec(storage+beacon) ok=%d\n", (int)ok);
		if (!ok) {
			fprintf(stderr, "FAIL: sendBeacon/storage test threw "
					"(see the assert message above)\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 5c PASS: sendBeacon + storage surface "
			"=== \n");

	/* --- Test 6 (fixes854, #283): THE hackaday.com crash — a timer JSValue
	 * must never be freed against a foreign JSRuntime.
	 *
	 * js_newheap() runs per browser_window AND per (i)frame
	 * (browser_window.c:3373 "new javascript context for each
	 * window/(i)frame"), and each heap gets its OWN JSRuntime — so ANY page
	 * carrying an iframe (hackaday's Jetpack comment form; Facebook's fbsbx
	 * pixel) has two runtimes alive at once.  s_timer_arena, though, is ONE
	 * global array.  qjs_flush_timers() used to free every live slot against
	 * the passed ctx regardless of which heap owned it, so navigating the
	 * main page freed the IFRAME's callback against the MAIN runtime:
	 *   JS_FreeValue(ctx_main, fn_iframe) -> refcount 0
	 *     -> free_object(rt_main, obj_iframe)
	 *       -> js_free_shape(rt_main, shape_iframe)
	 *         -> js_shape_hash_unlink(rt_main, shape_iframe)
	 * and that last one walks rt_main's bucket chain hunting a shape that
	 * lives in rt_iframe's table.  It never matches, the loop has no NULL
	 * check, and it runs off the end of the chain — on hardware a PowerPC
	 * unmapped memory exception at js_shape_hash_unlink+0004C; here a SEGV
	 * near-null as the walk dereferences (NULL)->shape_hash_next.
	 *
	 * This models exactly that: two heaps, one live anonymous-closure timer
	 * in each (refcount 1, so the flush really does drive it to zero), then
	 * a navigation flush on heap 1.  With the fix, heap 1 touches only its
	 * own slot and heap 2's timer survives untouched. --- */
	fprintf(stderr, "\n=== Test 6: cross-runtime timer free "
			"(main page + iframe) ===\n");
	{
		struct jsheap *heap2 = NULL;
		struct jsthread *thread2 = NULL;
		const char *arm_js =
			"globalThis.__fired=0;"
			"setTimeout(function(){globalThis.__fired=1;}, 99000);";
		const char *live_js =
			"if(typeof setTimeout!=='function')"
				"throw new Error('ASSERT FAIL: realm damaged');";
		unsigned char ok;

		/* heap2 == the iframe: a second, independent JSRuntime. */
		nerr = js_newheap(20000, &heap2);
		if (nerr != NSERROR_OK) {
			fprintf(stderr, "FAIL: js_newheap(2) nerr=%d\n", (int)nerr);
			return 1;
		}
		nerr = js_newthread(heap2, NULL, (void *)&htmlc, &thread2);
		if (nerr != NSERROR_OK) {
			fprintf(stderr, "FAIL: js_newthread(2) nerr=%d\n", (int)nerr);
			return 1;
		}

		/* Arm a long timer in EACH runtime. Anonymous closures, so the arena
		 * slot holds the only reference and the flush drives refcount to 0. */
		ok = js_exec(thread, (const unsigned char *)arm_js,
				strlen(arm_js), "driver-timer-main.js");
		if (!ok) { fprintf(stderr, "FAIL: arm main timer\n"); return 1; }
		ok = js_exec(thread2, (const unsigned char *)arm_js,
				strlen(arm_js), "driver-timer-iframe.js");
		if (!ok) { fprintf(stderr, "FAIL: arm iframe timer\n"); return 1; }
		fprintf(stderr, "armed one live timer in each of 2 runtimes\n");

		/* Navigate heap2 (the iframe). js_newthread() calls
		 * qjs_flush_timers(heap2->ctx); pre-fix that freed the MAIN heap's
		 * still-live callback against heap2's runtime and blew up inside
		 * js_shape_hash_unlink.
		 *
		 * Deliberately navigating heap2 and NOT the main heap: js_newthread
		 * does `JS_FreeContext(heap->ctx); heap->ctx = fresh;` and only
		 * points the NEW jsthread at the fresh ctx -- so navigating the main
		 * heap would leave this file's long-lived `thread` holding a freed
		 * JSContext, and every later test (7, 8) would silently be running in
		 * a dead realm. Same cross-runtime exercise either way. */
		{
			struct jsthread *thread_nav = NULL;
			nerr = js_newthread(heap2, NULL, (void *)&htmlc, &thread_nav);
			if (nerr != NSERROR_OK) {
				fprintf(stderr, "FAIL: js_newthread(nav) nerr=%d\n",
						(int)nerr);
				return 1;
			}
			thread2 = thread_nav;   /* old thread2 ctx is now freed */
			fprintf(stderr, "iframe navigation flush survived\n");
		}

		/* heap2's realm must still be intact and usable. */
		ok = js_exec(thread2, (const unsigned char *)live_js,
				strlen(live_js), "driver-iframe-live.js");
		if (!ok) {
			fprintf(stderr, "FAIL: iframe realm damaged by heap1's flush\n");
			return 1;
		}

		/* Tearing down heap2 must also be clean: js_destroyheap now flushes
		 * this heap's own slots before JS_FreeContext, so no live slot is
		 * left pointing into a freed runtime. */
		js_destroyheap(heap2);
		fprintf(stderr, "iframe heap destroyed cleanly\n");

		/* And the main heap must still run JS afterwards. */
		ok = js_exec(thread, (const unsigned char *)live_js,
				strlen(live_js), "driver-main-live.js");
		if (!ok) {
			fprintf(stderr, "FAIL: main realm damaged by iframe teardown\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 6 PASS: timers are per-runtime; no cross-runtime "
			"free through navigation-flush or heap teardown ===\n");

	/* --- Test 7 (fixes855, #284): the REAL jQuery must clear its own first
	 * support probe.  hackaday's first script is the _static ??-bundle of
	 * jquery.min.js + jquery-migrate.min.js; on hardware it died with
	 * "TypeError: cannot read property 'createElement' of undefined" and
	 * every dependent bundle then reported "jQuery is not defined".  jQuery
	 * 3.7.1's setDocument only captures the document when
	 * `9===n.nodeType && n.documentElement`, and document.nodeType was never
	 * set -- so its internal handle T stayed undefined and the first
	 * T.createElement() probe threw.  Runs the byte-exact file hackaday
	 * serves, wrapped in JS try/catch so we can assert on the error TEXT.
	 *
	 * The fixture is mandatory and full jQuery initialization is required.
	 * A missing fixture or any caught exception is a failed test; reporting a
	 * generic "GAP" followed by PASS made unrelated regressions invisible. */
	fprintf(stderr, "\n=== Test 7: real jQuery 3.7.1 clears the document probe ===\n");
	{
		FILE *jf = fopen("jquery-3.7.1.min.js", "rb");
		const char *probe_js =
			"if(document.nodeType!==9)"
				"throw new Error('ASSERT FAIL: document.nodeType='+document.nodeType);"
			"if(!document.documentElement)"
				"throw new Error('ASSERT FAIL: no documentElement');";
		unsigned char ok;

		ok = js_exec(thread, (const unsigned char *)probe_js,
				strlen(probe_js), "driver-doc-probe.js");
		if (!ok) {
			fprintf(stderr, "FAIL: document node identity wrong "
					"(nodeType must be 9, DOCUMENT_NODE)\n");
			return 1;
		}
		fprintf(stderr, "document.nodeType==9 and documentElement present\n");

		if (jf == NULL) {
			fprintf(stderr, "FAIL: jquery-3.7.1.min.js fixture is missing\n");
			return 1;
		} else {
			char *jsrc, *wrapped;
			long jlen;
			size_t rd, wn;
			const char *pre =
				"globalThis.__jqErr='';try{";
			const char *post =
				"}catch(e){globalThis.__jqErr=String((e&&e.message)||e);}";
			const char *verdict_js =
				"globalThis.__jqOK=(typeof jQuery!=='undefined'&&"
					"!!(jQuery.fn&&jQuery.fn.jquery));"
				"if(!globalThis.__jqOK)"
					"throw new Error('jQuery did not initialize: '+"
					"(globalThis.__jqErr||'(no error recorded)'));";

			fseek(jf, 0, SEEK_END); jlen = ftell(jf); fseek(jf, 0, SEEK_SET);
			jsrc = (char *)malloc((size_t)jlen + 1);
			if (jsrc == NULL) {
				fclose(jf);
				fprintf(stderr, "FAIL: oom reading jQuery fixture\n");
				return 1;
			}
			rd = fread(jsrc, 1, (size_t)jlen, jf);
			jsrc[rd] = '\0';
			fclose(jf);

			wn = strlen(pre) + rd + strlen(post) + 1;
			wrapped = (char *)malloc(wn);
			if (wrapped == NULL) {
				free(jsrc);
				fprintf(stderr, "FAIL: oom wrapping jQuery fixture\n");
				return 1;
			}
			strcpy(wrapped, pre);
			memcpy(wrapped + strlen(pre), jsrc, rd);
			strcpy(wrapped + strlen(pre) + rd, post);

			ok = js_exec(thread, (const unsigned char *)wrapped,
					strlen(wrapped), "jquery-3.7.1.min.js");
			fprintf(stderr, "js_exec(real jQuery, %ld bytes) ok=%d\n",
					jlen, (int)ok);
			free(jsrc);
			free(wrapped);
			if (!ok) {
				fprintf(stderr, "FAIL: the jQuery wrapper itself threw\n");
				return 1;
			}

			/* A caught jQuery exception is a test failure, not a PASS-with-GAP. */
			ok = js_exec(thread, (const unsigned char *)verdict_js,
					strlen(verdict_js), "driver-jq-verdict.js");
			if (!ok) {
				fprintf(stderr, "FAIL: real jQuery did not initialize\n");
				return 1;
			}
			fprintf(stderr, "=== Test 7 PASS: real jQuery initializes "
					"after the document-identity probe ===\n");
		}
	}

	/* --- Test 7b (DIAGNOSTIC): the REAL XenForo bundles, unstubbed
	 *
	 * js_exec SUBSTITUTES ES5 stubs for preamble.min.js, core-compiled.js and
	 * editor-compiled.js (fixes648), and an EMPTY string for lightbox / xfmg /
	 * attachment_manager / token_input / prefix_menu (fixes670). So the real
	 * bundles have never run on this engine: the "white box with no editor"
	 * reported on 68kmla IS s_xf_editor_stub, which only sets display:block +
	 * minHeight on the textarea. Froala is absent because it was never
	 * executed.
	 *
	 * The justification was a documented cascade -- preamble's
	 * `div.parentNode` hiddenscroll probe reading null and throwing. That
	 * premise PREDATES fixes846 (real DOM mutation), fixes878 (real libdom
	 * traversal) and fixes989-997 (the event model), all of which rebuilt the
	 * exact surface it blames; CLAUDE.md flags the entry as needing a re-test
	 * before being used as a starting point.
	 *
	 * So run the REAL bundles against the CURRENT engine and report what
	 * happens, rather than asking hardware for another log or deleting the
	 * stubs on a hope. DIAGNOSTIC: prints outcomes, never fails the run --
	 * the question is "what breaks now", not "is it already perfect".
	 *
	 * Bundles live in harness/ (fetched from 68kmla.org). Absent -> skip. --- */
	fprintf(stderr, "\n=== Test 7b (DIAGNOSTIC): the REAL XenForo bundles ===\n");
	{
		static const char *const xf_files[2] = {
			"preamble.min.js", "core-compiled.js"
		};
		const char *xf_probe =
			"globalThis.__xf='XF='+(typeof XF)"
			"+' XF.Element='+((typeof XF!=='undefined'&&XF)?typeof XF.Element:'n/a')"
			"+' XF.ready='+((typeof XF!=='undefined'&&XF)?typeof XF.ready:'n/a')"
			"+' jQuery='+(typeof jQuery)"
			"+' htmlClass='+((document.documentElement&&"
				"document.documentElement.className)||'(none)');";
		const char *xf_report =
			"console.log('  XF-PROBE: '+globalThis.__xf);";
		int fi;

		for (fi = 0; fi < 2; fi++) {
			FILE *bf = fopen(xf_files[fi], "rb");
			char *raw, *wrapped;
			long blen;
			size_t rd, wn;
			const char *pre  = "globalThis.__xfErr='';try{";
			const char *post = "}catch(e){globalThis.__xfErr=String((e&&e.message)||e);}";
			unsigned char xok;

			if (bf == NULL) {
				fprintf(stderr, "  SKIP %s (not present)\n", xf_files[fi]);
				continue;
			}
			fseek(bf, 0, SEEK_END); blen = ftell(bf); fseek(bf, 0, SEEK_SET);
			raw = (char *)malloc((size_t)blen + 1);
			if (raw == NULL) { fclose(bf); continue; }
			rd = fread(raw, 1, (size_t)blen, bf);
			raw[rd] = '\0';
			fclose(bf);

			wn = strlen(pre) + rd + strlen(post) + 1;
			wrapped = (char *)malloc(wn);
			if (wrapped == NULL) { free(raw); continue; }
			strcpy(wrapped, pre);
			memcpy(wrapped + strlen(pre), raw, rd);
			strcpy(wrapped + strlen(pre) + rd, post);

			fprintf(stderr, "  --- running REAL %s (%ld bytes) ---\n",
					xf_files[fi], blen);
			xok = js_exec(thread, (const unsigned char *)wrapped,
					strlen(wrapped), xf_files[fi]);
			fprintf(stderr, "  js_exec ok=%d\n", (int)xok);
			harness_pump_all(200000);
			/* The REAL browser pumps the QuickJS job queue (promise
			 * microtasks) between scripts from the WNE poll loop;
			 * harness_pump_all drains only the scheduler, so the XF
			 * probe's setter-queued Promise retries never run here.
			 * Mirror the Mac: pump the engine after every exec. */
			{
				extern void macsurf_qjs_pump_all(void);
				int pump;
				for (pump = 0; pump < 8; pump++)
					macsurf_qjs_pump_all();
			}
			{
				const char *ejs =
					"console.log('  THREW: '+"
					"(globalThis.__xfErr||'(no throw)'));";
				(void)js_exec(thread, (const unsigned char *)ejs,
						strlen(ejs), "xf-err.js");
			}
			free(raw);
			free(wrapped);
		}

		(void)js_exec(thread, (const unsigned char *)xf_probe,
				strlen(xf_probe), "xf-probe.js");
		(void)js_exec(thread, (const unsigned char *)xf_report,
				strlen(xf_report), "xf-report.js");
		harness_pump_all(100000);

		/* DIAGNOSTIC probe: call XF.activate(document), which is what
		 * XF's own init does, and which drives loadLazyHandlers. */
		{
			const char *act_js =
				"globalThis.__xfErr3='';"
				"try{"
				"XF.config.url=XF.config.url||{};"
				"XF.config.url.js='https://x.test/js/__SENTINEL__?_v=1';"
				"XF.config.jsMt={};"
				"var __d=document.createElement('div');"
				"__d.setAttribute('data-xf-init','emoji-completer');"
				"document.body.appendChild(__d);"
				"XF.LazyHandlerLoader.loadLazyHandlers(__d);}"
				"catch(e){globalThis.__xfErr3="
				"'NEWEL: name='+(e&&e.name)+' msg='+(e&&e.message)+"
				"' stack='+(e&&e.stack);}"
				"console.log('ACTIVATE-ERR2: '+"
				"(globalThis.__xfErr3||'(no throw newel)'));";
			(void)js_exec(thread, (const unsigned char *)act_js,
					strlen(act_js), "xf-activate.js");
			harness_pump_all(100000);
		}

		/* RAW arm — the MAC path, no try{} wrap: the real bundles at true
		 * global scope, exactly as js_exec runs them on hardware. The
		 * wrapped arm above puts `const XF={}` inside a try BLOCK (block
		 * scope), so window.XF=XF goes through the probe's setter trap.
		 * Raw, `const XF` is a GLOBAL lexical binding — if that changes
		 * what window.XF=XF hits on this engine, the XF LAZY probe lines
		 * will not appear for the raw arm even though they do for the
		 * wrapped one. DIAGNOSTIC: prints, never fails. */
		{
			static const char *const raw_files[2] = {
				"preamble.min.js", "core-compiled.js"
			};
			int fi;
			for (fi = 0; fi < 2; fi++) {
				FILE *bf = fopen(raw_files[fi], "rb");
				char *raw;
				long blen;
				size_t rd;
				if (bf == NULL) {
					fprintf(stderr, "  SKIP raw %s (not present)\n",
							raw_files[fi]);
					continue;
				}
				fseek(bf, 0, SEEK_END); blen = ftell(bf);
				fseek(bf, 0, SEEK_SET);
				raw = (char *)malloc((size_t)blen + 1);
				if (raw == NULL) { fclose(bf); continue; }
				rd = fread(raw, 1, (size_t)blen, bf);
				raw[rd] = '\0';
				fclose(bf);
				fprintf(stderr, "  --- RAW %s (%ld bytes, MAC path) ---\n",
						raw_files[fi], blen);
				{
					unsigned char xok = js_exec(thread,
							(const unsigned char *)raw,
							strlen(raw), raw_files[fi]);
					fprintf(stderr, "  RAW js_exec ok=%d\n", (int)xok);
				}
				{
					extern void macsurf_qjs_pump_all(void);
					int pump;
					for (pump = 0; pump < 8; pump++)
						macsurf_qjs_pump_all();
				}
				free(raw);
			}
		}
	}
	fprintf(stderr, "=== Test 7b done (diagnostic) ===\n");

	/* --- Test 8 (fixes861, #289): EVERY heap's timers must fire, not just the
	 * newest.  js_newheap() runs per browser_window AND per (i)frame, and the
	 * browser's one pump (main.c -> macsurf_qjs_pump_all) used to pump only
	 * g_heap = the most-recently-created heap.  That was survivable while
	 * run_timers fired every arena slot regardless of owner; fixes854 correctly
	 * gated slots on `t->ctx == qctx` (they were being freed/called against a
	 * foreign runtime), which left exactly ONE heap able to tick.
	 *
	 * On hardware that froze the Jetpack comment iframe: its
	 * IntersectionObserver.observe() delivery is a setTimeout(...,0), so the
	 * reply box never loaded.  This models it directly -- two heaps, a timer
	 * armed in each, one pump -- and asserts BOTH fire.  Pre-fix the older
	 * heap's timer never runs. --- */
	fprintf(stderr, "\n=== Test 8: pump_all fires timers in EVERY heap ===\n");
	{
		extern void macsurf_qjs_pump_all(void);
		struct jsheap *heapA = NULL, *heapB = NULL;
		struct jsthread *thA = NULL, *thB = NULL;
		const char *arm =
			"globalThis.__ticked=0;setTimeout(function(){globalThis.__ticked=1;},0);";
		const char *check =
			"if(!globalThis.__ticked)"
				"throw new Error('ASSERT FAIL: this heap never ticked');";
		unsigned char ok;
		int pump;

		/* heapA first, heapB second -> g_heap == heapB, so pre-fix heapA
		 * is the frozen one (exactly the iframe-vs-page situation). */
		if (js_newheap(20000, &heapA) != NSERROR_OK ||
		    js_newthread(heapA, NULL, (void *)&htmlc, &thA) != NSERROR_OK) {
			fprintf(stderr, "FAIL: heapA setup\n"); return 1;
		}
		if (js_newheap(20000, &heapB) != NSERROR_OK ||
		    js_newthread(heapB, NULL, (void *)&htmlc, &thB) != NSERROR_OK) {
			fprintf(stderr, "FAIL: heapB setup\n"); return 1;
		}
		ok = js_exec(thA, (const unsigned char *)arm, strlen(arm), "arm-A.js");
		if (!ok) { fprintf(stderr, "FAIL: arm heapA\n"); return 1; }
		ok = js_exec(thB, (const unsigned char *)arm, strlen(arm), "arm-B.js");
		if (!ok) { fprintf(stderr, "FAIL: arm heapB\n"); return 1; }
		fprintf(stderr, "armed a 0ms timer in each of 2 heaps (g_heap == heapB)\n");

		for (pump = 0; pump < 8; pump++) macsurf_qjs_pump_all();

		ok = js_exec(thB, (const unsigned char *)check, strlen(check), "check-B.js");
		if (!ok) {
			fprintf(stderr, "FAIL: newest heap (g_heap) never ticked\n");
			return 1;
		}
		fprintf(stderr, "heapB (newest) ticked\n");
		ok = js_exec(thA, (const unsigned char *)check, strlen(check), "check-A.js");
		if (!ok) {
			fprintf(stderr, "FAIL: the OLDER heap never ticked -- pump_all is "
					"still single-heap, so an iframe's realm is frozen "
					"(this is the hackaday reply-box bug)\n");
			return 1;
		}
		fprintf(stderr, "heapA (older) ticked\n");
		js_destroyheap(heapB);
		js_destroyheap(heapA);
		/* The main heap must still tick after those teardowns -- proves
		 * js_destroyheap unlinks cleanly and pump_all is not walking freed
		 * memory. */
		ok = js_exec(thread, (const unsigned char *)arm, strlen(arm), "arm-main.js");
		if (!ok) { fprintf(stderr, "FAIL: arm main heap\n"); return 1; }
		for (pump = 0; pump < 8; pump++) macsurf_qjs_pump_all();
		ok = js_exec(thread, (const unsigned char *)check, strlen(check), "check-main.js");
		if (!ok) {
			fprintf(stderr, "FAIL: main heap stopped ticking after teardown "
					"(bad unlink in js_destroyheap)\n");
			return 1;
		}
		fprintf(stderr, "main heap still ticks after both teardowns\n");
	}
	fprintf(stderr, "=== Test 8 PASS: every live heap's timers fire; teardown "
			"unlinks cleanly ===\n");

	/* --- Test 9 (fixes863): reproduce hackaday's loader shape exactly --
	 * register a DOMContentLoaded handler on WINDOW (not document), fire
	 * js_fire_dom_ready, and assert the handler actually ran.  This is the
	 * precise mechanism the Jetpack comment iframe uses. --- */
	fprintf(stderr, "\n=== Test 9: window DOMContentLoaded listener fires ===\n");
	{
		const char *reg =
			"globalThis.__ran=0;globalThis.__found=-1;"
			"window.addEventListener('DOMContentLoaded',function(){"
				"globalThis.__ran=1;"
				"var e=document.querySelector('#commentform');"
				"globalThis.__found=e?1:0;"
			"});";
		const char *chk =
			"if(!globalThis.__ran)"
				"throw new Error('ASSERT FAIL: window DOMContentLoaded listener never ran');";
		unsigned char ok;
		ok = js_exec(thread, (const unsigned char *)reg, strlen(reg), "reg-win.js");
		if (!ok) { fprintf(stderr, "FAIL: register\n"); return 1; }
		/* the real path the browser uses */
		js_fire_dom_ready(thread, (struct dom_document *)htmlc.document);
		ok = js_exec(thread, (const unsigned char *)chk, strlen(chk), "chk-win.js");
		if (!ok) {
			fprintf(stderr, "FAIL: a window DOMContentLoaded listener does NOT "
					"fire -- this is hackaday's loader mechanism\n");
			return 1;
		}
		fprintf(stderr, "window DOMContentLoaded listener ran\n");
	}
	fprintf(stderr, "=== Test 9 PASS: window DOMContentLoaded listeners fire ===\n");

	/* --- Test 10 (fixes864): document.querySelector('#id').  hackaday's
	 * comment-iframe loader is literally
	 *     var e = document.querySelector("#commentform"); if (e) { ...everything... }
	 * so if #id lookup returns null the ENTIRE form-loading chain is skipped
	 * silently -- no throw, no fetch, no injected script, no error line.  The
	 * harness doc has a real <div id="feed">, so this is a direct test. --- */
	fprintf(stderr, "\n=== Test 10: querySelector('#id') ===\n");
	{
		const char *q =
			"globalThis.__byId   = document.getElementById('feed') ? 1 : 0;"
			"globalThis.__byQS   = document.querySelector('#feed') ? 1 : 0;"
			"globalThis.__byQSA  = (document.querySelectorAll('#feed')||[]).length;";
		const char *chk =
			"if(!globalThis.__byId)"
				"throw new Error('getElementById(feed) failed - harness doc wrong');"
			"if(!globalThis.__byQS)"
				"throw new Error('ASSERT FAIL: querySelector(\\'#feed\\') returned null "
					"while getElementById found it - #id selectors unsupported');";
		unsigned char ok = js_exec(thread, (const unsigned char *)q, strlen(q), "qs-id.js");
		if (!ok) { fprintf(stderr, "FAIL: probe threw\n"); return 1; }
		ok = js_exec(thread, (const unsigned char *)chk, strlen(chk), "qs-id-chk.js");
		if (!ok) {
			fprintf(stderr, "FAIL: querySelector('#id') is BROKEN -- this is why "
					"hackaday's reply box never loads\n");
			return 1;
		}
		fprintf(stderr, "querySelector('#feed') resolves\n");
	}
	fprintf(stderr, "=== Test 10 PASS: #id selectors work ===\n");

	/* --- Test 11 (fixes866, #292): DYNAMIC <script> INJECTION -- the exact
	 * sequence hackaday's verbum loader uses and the last thing standing
	 * between us and a real comment box:
	 *     const s = document.createElement('script');
	 *     s.onload = () => resolve();
	 *     s.src = url;
	 *     document.body.appendChild(s);
	 * On HW the fetch now succeeds (fixes865, ok=1 status=200) but
	 * verbum-comments.js still never executes and ZERO flags=4 scripts reach
	 * dom_SCRIPT_showed_up -- so the injection itself is not landing. The path
	 * (DOMNodeInserted -> dom_SCRIPT_showed_up -> html_process_script) is
	 * fully wired in core; the question is whether our createElement/
	 * appendChild produce a REAL libdom node that dispatches the event.
	 * Checks each link separately so the failure names itself. --- */
	fprintf(stderr, "\n=== Test 11: dynamic <script> injection ===\n");
	{
		const char *probe =
			"globalThis.__r={};"
			"var s=document.createElement('script');"
			"globalThis.__r.made      = !!s;"
			"globalThis.__r.isNative  = !!(s&&s.__ptr);"          /* real libdom node? */
			"globalThis.__r.tag       = (s&&s.tagName)||'?';"
			"try{s.src='https://example.invalid/injected.js';"
			"globalThis.__r.srcSet=(s.getAttribute?(s.getAttribute('src')||''):'');}"
			"catch(e){globalThis.__r.srcSet='THREW:'+e;}"
			"globalThis.__r.bodyIs    = document.body?(document.body.__ptr?'native':'FALLBACK'):'null';"
			"try{document.body.appendChild(s);globalThis.__r.appended=1;}"
			"catch(e){globalThis.__r.appended='THREW:'+e;}";
		const char *report =
			"'made='+globalThis.__r.made+' isNative='+globalThis.__r.isNative+"
			"' tag='+globalThis.__r.tag+' srcSet='+globalThis.__r.srcSet+"
			"' body='+globalThis.__r.bodyIs+' appended='+globalThis.__r.appended";
		unsigned char ok = js_exec(thread, (const unsigned char *)probe,
				strlen(probe), "inject-probe.js");
		if (!ok) { fprintf(stderr, "FAIL: injection probe threw\n"); return 1; }
		{
			/* surface the values through the same console->MS_LOG path */
			const char *emit = "console.error('WORK inject '+(";
			char *buf = (char *)malloc(strlen(emit)+strlen(report)+8);
			strcpy(buf, emit); strcat(buf, report); strcat(buf, "))");
			(void)js_exec(thread, (const unsigned char *)buf, strlen(buf),
					"inject-report.js");
			free(buf);
		}
		{
			const char *chk =
				"if(!globalThis.__r.made)"
					"throw new Error('ASSERT FAIL: createElement(script) returned nothing');"
				"if(!globalThis.__r.isNative)"
					"throw new Error('ASSERT FAIL: createElement(script) is a JS FALLBACK object, "
						"not a libdom node -- appendChild can never dispatch DOMNodeInserted');"
				"if(globalThis.__r.bodyIs!=='native')"
					"throw new Error('ASSERT FAIL: document.body is '+globalThis.__r.bodyIs+"
						"' -- appendChild goes nowhere');"
				"if(globalThis.__r.appended!==1)"
					"throw new Error('ASSERT FAIL: appendChild -> '+globalThis.__r.appended);"
				"if(!globalThis.__r.srcSet)"
					"throw new Error('ASSERT FAIL: script.src did not stick (getAttribute empty) "
						"-- html_process_script would treat it as an INLINE script');";
			ok = js_exec(thread, (const unsigned char *)chk, strlen(chk),
					"inject-chk.js");
			if (!ok) {
				fprintf(stderr, "FAIL: dynamic <script> injection is broken -- "
						"see the ASSERT above; this is why the reply box "
						"never loads\n");
				return 1;
			}
		}
		fprintf(stderr, "createElement(script) is native, src sticks, body is "
				"native, appendChild ok\n");

		/* fixes867 (#293) — CONTROL: prove the instrument can actually FIRE.
		 * Everything above only shows appendChild not throwing; that was true
		 * even when it silently swallowed every rejection (the old `appended=1`
		 * assert could not fail). So force a rejection libdom is guaranteed to
		 * produce -- inserting an ancestor into its own descendant, which
		 * node.c:752-756 rejects with HIERARCHY_REQUEST_ERR -- and require that
		 * it THROWS. If this control ever goes quiet, the probe is blind again
		 * and every "append ok" above is worthless. */
		{
			const char *ctrl =
				"globalThis.__threw=0;"
				"var p=document.body;"
				"var c=document.createElement('div');"
				"p.appendChild(c);"
				"try{c.appendChild(p);}catch(e){globalThis.__threw=1;}";
			const char *ctrl_chk =
				"if(!globalThis.__threw)"
					"throw new Error('ASSERT FAIL: appendChild(ancestor) did NOT throw "
						"- the exception is still being swallowed, so no append "
						"result in this file can be trusted');";
			unsigned char cok = js_exec(thread, (const unsigned char *)ctrl,
					strlen(ctrl), "inject-control.js");
			if (!cok) { fprintf(stderr, "FAIL: control setup threw\n"); return 1; }
			cok = js_exec(thread, (const unsigned char *)ctrl_chk,
					strlen(ctrl_chk), "inject-control-chk.js");
			if (!cok) {
				fprintf(stderr, "FAIL: the DOM-failure probe is BLIND - a "
						"guaranteed-invalid append reported success\n");
				return 1;
			}
			fprintf(stderr, "control: invalid append throws (probe is live)\n");
		}
	}
	fprintf(stderr, "=== Test 11 PASS: dynamic <script> injection reaches the DOM; "
			"DOM-failure probe verified live ===\n");

	/* --- Test 12 (fixes868, #294): PROMISE REACTIONS MUST RUN.
	 * QuickJS queues every .then() as a pending JOB; they only execute when the
	 * host drains the queue via JS_ExecutePendingJob(). Nothing in MacSurf ever
	 * called it -- so every Promise chain in the browser is dead past the first
	 * resolve(). That is why hackaday's reply box never loads: the fetch DOES
	 * resolve (HW log: `WORK fetch ... ok=1 status=200`, emitted from
	 * xhr.onreadystatechange, a DIRECT JS_Call), but the loader's
	 *     loadExtra(...).then(...).then(() => loadExternalScript(handle))
	 * chain never advances, so createElement/appendChild are never reached --
	 * exactly matching zero appends AND zero append failures on hardware. --- */
	fprintf(stderr, "\n=== Test 12: Promise .then() reactions run ===\n");
	{
		extern void macsurf_qjs_pump_all(void);
		const char *arm =
			"globalThis.__p1=0;globalThis.__p2=0;globalThis.__chain=0;"
			"Promise.resolve(1).then(function(){globalThis.__p1=1;});"
			"Promise.resolve(2).then(function(){return 3;})"
				".then(function(){globalThis.__p2=1;})"
				".then(function(){globalThis.__chain=1;});";
		const char *chk =
			"if(!globalThis.__p1)"
				"throw new Error('ASSERT FAIL: a single .then() never ran - the "
					"QuickJS job queue is not being drained (JS_ExecutePendingJob)');"
			"if(!globalThis.__p2||!globalThis.__chain)"
				"throw new Error('ASSERT FAIL: chained .then() did not advance');";
		unsigned char ok;
		int pump;

		ok = js_exec(thread, (const unsigned char *)arm, strlen(arm), "promise-arm.js");
		if (!ok) { fprintf(stderr, "FAIL: promise arm threw\n"); return 1; }
		/* Same pump the WNE loop calls. */
		for (pump = 0; pump < 8; pump++) macsurf_qjs_pump_all();
		ok = js_exec(thread, (const unsigned char *)chk, strlen(chk), "promise-chk.js");
		if (!ok) {
			fprintf(stderr, "FAIL: Promise reactions never run -- EVERY modern JS "
					"chain in the browser is dead past the first resolve()\n");
			return 1;
		}
		fprintf(stderr, "single + chained .then() both ran\n");
	}
	fprintf(stderr, "=== Test 12 PASS: Promise reactions are drained ===\n");

	/* --- Test 13 (fixes869, #295): GATE 2 -- script.onload MUST FIRE.
	 * With fixes868 the loader's Promise chain finally advances and wp-polyfill
	 * EXECUTES on hardware (the first JS-injected script ever to run in
	 * MacSurf). It then stops dead, because the universal loader idiom
	 *     const s = document.createElement('script');
	 *     s.onload = () => resolve();     // <- settles the caller's promise
	 *     s.src = url; document.body.appendChild(s);
	 * resolves ONLY on the load event, and nothing in MacSurf ever fired one.
	 * So promises['wp-polyfill'] never settles and verbum-comments.js is never
	 * even requested (HW: `wp-polyfill EXECUTED=1`, `verbum EXECUTED=0`).
	 *
	 * Both delivery routes must work -- pages use either:
	 *   s.onload = fn                 (the property)
	 *   s.addEventListener('load',fn) (the listener map)
	 * and the event object must carry a usable .type/.target.
	 *
	 * NEGATIVE CONTROL: this test fails red if js_fire_script_load is a no-op
	 * (every flag stays 0), which is exactly the pre-fix state -- so it cannot
	 * pass vacuously the way Test 11's old `appended=1` assert could. --- */
	fprintf(stderr, "\n=== Test 13: script.onload / onerror fire ===\n");
	{
		dom_string *id_str = NULL;
		dom_element *el_ok = NULL, *el_err = NULL;
		unsigned char ok;

		/* Arm two scripts: one we will report success for, one failure. */
		const char *arm =
			"globalThis.__g2={onload:0,onerror:0,al:0,type:'',tgt:0,order:[]};"
			"function mk(id){"
				"var s=document.createElement('script');"
				"s.id=id;"
				"s.onload=function(e){globalThis.__g2.onload++;"
					"globalThis.__g2.type=e?e.type:'NOEVENT';"
					"globalThis.__g2.tgt=(e&&e.target===s)?1:0;"
					"globalThis.__g2.order.push('prop');};"
				"s.onerror=function(e){globalThis.__g2.onerror++;"
					"globalThis.__g2.type=e?e.type:'NOEVENT';};"
				"s.addEventListener('load',function(){"
					"globalThis.__g2.al++;globalThis.__g2.order.push('listener');});"
				"s.src='https://example.invalid/'+id+'.js';"
				"document.body.appendChild(s);"
				"return s;}"
			"mk('g2ok');mk('g2err');";
		ok = js_exec(thread, (const unsigned char *)arm, strlen(arm), "gate2-arm.js");
		if (!ok) { fprintf(stderr, "FAIL: gate2 arm threw\n"); return 1; }

		/* Reach the elements the same way core does -- through libdom, not
		 * through any QuickJS internals. If getElementById can't find them the
		 * append in the arm never really landed and the rest is meaningless. */
		if (dom_string_create((const uint8_t *)"g2ok", 4, &id_str) != DOM_NO_ERR) {
			fprintf(stderr, "FAIL: dom_string_create\n"); return 1;
		}
		dom_document_get_element_by_id(document, id_str, &el_ok);
		dom_string_unref(id_str);
		id_str = NULL;
		if (dom_string_create((const uint8_t *)"g2err", 5, &id_str) != DOM_NO_ERR) {
			fprintf(stderr, "FAIL: dom_string_create\n"); return 1;
		}
		dom_document_get_element_by_id(document, id_str, &el_err);
		dom_string_unref(id_str);

		if (el_ok == NULL || el_err == NULL) {
			fprintf(stderr, "FAIL: the injected <script> elements are not in the "
					"document (getElementById found %s/%s) -- appendChild "
					"did not really land\n",
					el_ok ? "ok" : "NULL", el_err ? "ok" : "NULL");
			return 1;
		}

		/* This is what script.c now does when the fetch+exec completes. */
		js_fire_script_load(thread, (dom_node *)el_ok, 1);
		js_fire_script_load(thread, (dom_node *)el_err, 0);

		{
			const char *chk =
				"var g=globalThis.__g2;"
				"if(g.onload!==1)"
					"throw new Error('ASSERT FAIL: script.onload fired '+g.onload+' "
						"times, expected 1 -- the dynamic-loader idiom "
						"(s.onload=()=>resolve()) can NEVER settle, so every "
						"chained script load hangs forever');"
				"if(g.al!==1)"
					"throw new Error('ASSERT FAIL: addEventListener(\"load\") fired "
						"'+g.al+' times, expected 1 -- the event reaches the "
						"onload PROPERTY but not the listener map (or vice "
						"versa); pages use both');"
				"if(g.type!=='error')"
					"throw new Error('ASSERT FAIL: last event type was \"'+g.type+"
						"'\", expected \"error\" from the second fire');"
				"if(g.onerror!==1)"
					"throw new Error('ASSERT FAIL: script.onerror fired '+g.onerror+"
						"' times, expected 1 -- a failed script must REJECT, not "
						"hang; a promise that never settles is worse than a "
						"rejected one');";
			ok = js_exec(thread, (const unsigned char *)chk, strlen(chk), "gate2-chk.js");
			if (!ok) {
				fprintf(stderr, "FAIL: Gate 2 is shut -- script load/error events "
						"do not reach the element, so verbum is never "
						"requested\n");
				return 1;
			}
		}

		/* The success fire must not ALSO have run the error handler, and the
		 * failure fire must not have run onload a second time. Both are checked
		 * above by the exact counts (1 and 1), which a fire-everything
		 * implementation would break. */
		{
			const char *ordchk =
				"var o=globalThis.__g2.order.join(',');"
				"if(o!=='prop,listener'&&o!=='listener,prop')"
					"throw new Error('ASSERT FAIL: unexpected delivery set: '+o);"
				"if(!globalThis.__g2.tgt)"
					"throw new Error('ASSERT FAIL: event.target is not the script "
						"element -- loaders that read e.target.src to key their "
						"promise map will misfire');";
			ok = js_exec(thread, (const unsigned char *)ordchk, strlen(ordchk),
					"gate2-ord.js");
			if (!ok) { fprintf(stderr, "FAIL: Gate 2 event shape is wrong\n"); return 1; }
		}

		dom_node_unref(el_ok);
		dom_node_unref(el_err);
		fprintf(stderr, "onload fires once via BOTH the property and the listener "
				"map; onerror fires on failure; event.target is the script\n");
	}
	fprintf(stderr, "=== Test 13 PASS: script.onload/onerror fire ===\n");

	/* --- Test 14 (fixes870, #297): GATE 3 -- document.createElementNS.
	 * Preact's ONLY element factory; its reconciler never calls createElement:
	 *     if("svg"==k)a="http://www.w3.org/2000/svg";
	 *     else if("math"==k)a="http://www.w3.org/1998/Math/MathML";
	 *     else if(!a)a="http://www.w3.org/1999/xhtml";
	 *     e=document.createElementNS(a,k,w.is&&w)
	 * (verbatim from verbum-comments.js). With createElementNS missing, a Preact
	 * app renders NOTHING -- no amount of fixing the loader gets a comment box.
	 *
	 * The THIRD ARG is the trap this test exists for: `w.is && w` is undefined
	 * when props.is is unset but the ENTIRE props object when it is set. A
	 * control that only passes undefined would not exercise what Preact actually
	 * sends, so both forms are checked here.
	 *
	 * Elements must come out REAL (native, appendable, findable), not fallback
	 * objects -- a fallback would satisfy a naive "did it return something?"
	 * assert while being invisible to layout, which is the Test-11 false-green
	 * class. --- */
	fprintf(stderr, "\n=== Test 14: createElementNS (Preact's element factory) ===\n");
	{
		const char *probe =
			"globalThis.__ns={};"
			"var XHTML='http://www.w3.org/1999/xhtml';"
			"var SVG='http://www.w3.org/2000/svg';"
			"var n=globalThis.__ns;"
			"n.exists=(typeof document.createElementNS==='function');"
			/* the ordinary Preact case: 3rd arg undefined.  Guarded like every
			 * other probe line below: a probe must COLLECT FACTS and never
			 * throw, so that the descriptive ASSERT in the check phase is what
			 * names the failure. An unguarded call here dies with a bare
			 * "TypeError: not a function" and the reader learns nothing. */
			"var d=null;"
			"try{d=document.createElementNS(XHTML,'div',undefined);}"
			"catch(e){n.madeErr=''+e;}"
			"n.made=!!d; n.native=!!(d&&d.__ptr); n.tag=(d&&d.tagName)||'?';"
			/* THE TRAP: props.is set => 3rd arg is the whole props object */
			"try{var props={is:'my-el',className:'x',children:[]};"
			"var c=document.createElementNS(XHTML,'div',props.is&&props);"
			"n.optObj=!!(c&&c.__ptr);}catch(e){n.optObj='THREW:'+e;}"
			/* null namespace is legal per spec */
			"try{var z=document.createElementNS(null,'span');"
			"n.nullNs=!!(z&&z.__ptr);}catch(e){n.nullNs='THREW:'+e;}"
			/* svg branch must not blow up */
			"try{var s=document.createElementNS(SVG,'svg');"
			"n.svg=!!(s&&s.__ptr);}catch(e){n.svg='THREW:'+e;}"
			/* and the element must be REAL: appendable + findable */
			"try{d.id='ns-probe';document.body.appendChild(d);"
			"n.found=(document.getElementById('ns-probe')===d)?1:0;}"
			"catch(e){n.found='THREW:'+e;}";
		unsigned char ok = js_exec(thread, (const unsigned char *)probe,
				strlen(probe), "ns-probe.js");
		if (!ok) { fprintf(stderr, "FAIL: createElementNS probe threw\n"); return 1; }
		{
			const char *chk =
				"var n=globalThis.__ns;"
				"if(!n.exists)"
					"throw new Error('ASSERT FAIL: document.createElementNS does not "
						"exist -- Preact never calls createElement, so its "
						"reconciler cannot create a single element');"
				"if(!n.made)"
					"throw new Error('ASSERT FAIL: createElementNS returned nothing'+"
						"(n.madeErr?' ('+n.madeErr+')':''));"
				"if(!n.native)"
					"throw new Error('ASSERT FAIL: createElementNS returned a JS "
						"FALLBACK object, not a libdom node -- invisible to "
						"layout, so the form would never paint');"
				"if(n.tag!=='DIV')"
					"throw new Error('ASSERT FAIL: tagName is '+n.tag+', expected DIV');"
				"if(n.optObj!==true)"
					"throw new Error('ASSERT FAIL: createElementNS(ns,tag,PROPS_OBJECT) "
						"-> '+n.optObj+'. Preact passes `props.is && props`, i.e. "
						"the ENTIRE props object, whenever props.is is set; it "
						"must be ignored, not choked on');"
				"if(n.nullNs!==true)"
					"throw new Error('ASSERT FAIL: createElementNS(null,tag) -> '+n.nullNs+"
						"' -- a null namespace is legal per spec');"
				"if(n.svg!==true)"
					"throw new Error('ASSERT FAIL: createElementNS(SVG,...) -> '+n.svg);"
				"if(n.found!==1)"
					"throw new Error('ASSERT FAIL: the created element is not in the "
						"document after appendChild (getElementById -> '+n.found+') "
						"-- it is not a real, connected node');";
			ok = js_exec(thread, (const unsigned char *)chk, strlen(chk), "ns-chk.js");
			if (!ok) {
				fprintf(stderr, "FAIL: Gate 3 is shut -- Preact cannot create "
						"elements, so the comment form renders nothing\n");
				return 1;
			}
		}
		fprintf(stderr, "createElementNS: xhtml/svg/null-ns all native; the "
				"props-object 3rd arg is tolerated; element is connected\n");
	}
	fprintf(stderr, "=== Test 14 PASS: createElementNS ===\n");

	/* --- Test 15 (fixes871, #298): GATE 4 -- class + descendant selectors.
	 * Preact's Verbum mount is, literally:
	 *     document.querySelectorAll(".comment-form__verbum").forEach(...)
	 * A class-only selector returned EMPTY ("class-only sel unsupported"), so
	 * that forEach ran ZERO times and the comment form never rendered -- even
	 * with a working loader (#294/#295) and a working element factory (#297).
	 *
	 * The whole bundle's selector surface is four literals: `#comment_parent`,
	 * `img`, `.comment-form__verbum`, `.wp-die-message p`. All four are checked
	 * here, plus the traps:
	 *   - `div.foo` must NOT match every div (the old code ignored the class);
	 *   - `.foo` must NOT match class="foobar" (substring vs token);
	 *   - `#a .b` must return the .b INSIDE #a, not #a itself (the old strchr
	 *     fast path returned the wrong element, confidently);
	 *   - the return value must have .forEach -- Preact calls it directly. --- */
	fprintf(stderr, "\n=== Test 15: class + descendant selectors ===\n");
	{
		const char *arm =
			"var host=document.createElement('div');"
			"host.id='sel-host';"
			"host.innerHTML="
				"'<div class=\"comment-form__verbum dark\">MOUNT</div>'+"
				"'<div class=\"foobar\">NOT-A-MATCH</div>'+"
				"'<div class=\"wp-die-message\"><p>ERRTEXT</p></div>'+"
				"'<span class=\"foo\">span-foo</span>'+"
				"'<div class=\"foo\">div-foo</div>';"
			"document.body.appendChild(host);";
		const char *probe =
			"globalThis.__s={};var s=globalThis.__s;"
			"var q=function(x){try{return document.querySelectorAll(x);}"
				"catch(e){return 'THREW:'+e;}};"
			/* THE MOUNT -- the one that matters */
			"var m=q('.comment-form__verbum');"
			"s.mountLen=m.length; s.mountTxt=(m[0]&&m[0].textContent)||'';"
			"s.hasForEach=(typeof m.forEach==='function');"
			/* forEach must actually iterate (Preact calls it directly) */
			"s.iter=0; if(s.hasForEach) m.forEach(function(){s.iter++;});"
			/* token vs substring: .foo must not match class='foobar' */
			"var f=q('.foo'); s.fooLen=f.length;"
			/* tag.class must not degrade to 'every div' */
			"var dv=q('div.foo'); s.divFooLen=dv.length;"
			"s.divFooTag=((dv[0]&&dv[0].tagName)||'?').toLowerCase();"
			/* descendant: .wp-die-message p */
			"var d=q('.wp-die-message p');"
			"s.descLen=d.length; s.descTxt=(d[0]&&d[0].textContent)||'';"
			/* descendant must not match a p OUTSIDE the ancestor */
			"var bogus=q('.comment-form__verbum p'); s.bogusLen=bogus.length;"
			/* '#a .b' must return the INNER element, not #a.
			 * NOTE tagName is compared case-insensitively: MacSurf reports it
			 * LOWERCASE, where a real browser uppercases it for HTML elements.
			 * That is a genuine fidelity gap (tracked separately) but not this
			 * gate's business -- and it does not bite Verbum, which defensively
			 * calls tagName.toUpperCase(). Asserting 'SPAN' here would make this
			 * test fail for a reason that has nothing to do with selectors. */
			"var inner=document.querySelector('#sel-host .foo');"
			"s.innerTag=((inner&&inner.tagName)||'null').toUpperCase();"
			"s.innerTxt=(inner&&inner.textContent)||'';"
			/* plain id still works (fixes864 must not regress) */
			"s.idOk=(document.querySelector('#sel-host')===host)?1:0;"
			/* bare tag still works */
			"s.pLen=q('p').length;";
		unsigned char ok;

		ok = js_exec(thread, (const unsigned char *)arm, strlen(arm), "sel-arm.js");
		if (!ok) { fprintf(stderr, "FAIL: selector arm threw\n"); return 1; }
		ok = js_exec(thread, (const unsigned char *)probe, strlen(probe), "sel-probe.js");
		if (!ok) { fprintf(stderr, "FAIL: selector probe threw\n"); return 1; }
		{
			const char *chk =
				"var s=globalThis.__s;"
				"if(s.mountLen!==1)"
					"throw new Error('ASSERT FAIL: querySelectorAll(\".comment-form"
						"__verbum\") returned '+s.mountLen+' elements, expected 1 "
						"-- this is Preact\\'s Verbum MOUNT; 0 means the comment "
						"form can never render');"
				"if(s.mountTxt!=='MOUNT')"
					"throw new Error('ASSERT FAIL: matched the wrong element: '+s.mountTxt);"
				"if(!s.hasForEach)"
					"throw new Error('ASSERT FAIL: querySelectorAll result has no "
						"forEach -- the mount code calls it directly');"
				"if(s.iter!==1)"
					"throw new Error('ASSERT FAIL: forEach iterated '+s.iter+' times');"
				"if(s.fooLen!==2)"
					"throw new Error('ASSERT FAIL: .foo matched '+s.fooLen+', expected 2 "
						"-- class matching must be WHITESPACE-TOKEN based; "
						"class=\"foobar\" must not match .foo (a substring test "
						"would give 3)');"
				"if(s.divFooLen!==1||s.divFooTag!=='div')"
					"throw new Error('ASSERT FAIL: div.foo matched '+s.divFooLen+' ('+"
						"s.divFooTag+'), expected 1 div -- the old code ignored the "
						"class and matched EVERY div');"
				"if(s.descLen!==1||s.descTxt!=='ERRTEXT')"
					"throw new Error('ASSERT FAIL: .wp-die-message p -> len='+s.descLen+"
						"' txt='+s.descTxt+' (expected 1/ERRTEXT)');"
				"if(s.bogusLen!==0)"
					"throw new Error('ASSERT FAIL: .comment-form__verbum p matched '+"
						"s.bogusLen+' -- the descendant combinator is not "
						"constraining by ancestor at all');"
				"if(s.innerTag!=='SPAN'||s.innerTxt!=='span-foo')"
					"throw new Error('ASSERT FAIL: querySelector(\"#sel-host .foo\") "
						"returned <'+s.innerTag+'> \"'+s.innerTxt+'\", expected the "
						"SPAN inside -- the old strchr(sel,\"#\") fast path returned "
						"the #id element itself for any \"#a .b\" selector');"
				"if(s.idOk!==1)"
					"throw new Error('ASSERT FAIL: plain #id selector regressed');"
				/* The harness document is build_large_doc(300), so a bare 'p'
				 * legitimately matches ~301. The assert is only that bare-tag
				 * matching still works at all (fixes864 must not regress); an
				 * exact count here would be asserting the fixture, not the code. */
				"if(s.pLen<1)"
					"throw new Error('ASSERT FAIL: bare tag selector regressed: p -> '+s.pLen);";
			ok = js_exec(thread, (const unsigned char *)chk, strlen(chk), "sel-chk.js");
			if (!ok) {
				fprintf(stderr, "FAIL: Gate 4 is shut -- Preact's mount query finds "
						"nothing, so the comment form never renders\n");
				return 1;
			}
		}
		/* Clean the fixture out of the document. It uses the same
		 * `.comment-form__verbum` class the CAPSTONE below queries for, and a
		 * leftover copy makes that test count 2 mounts and fail for a reason
		 * that has nothing to do with the code under test. Tests share one
		 * document here, so each one puts its fixture back. */
		{
			const char *cleanup =
				"if(typeof host!=='undefined'&&host&&host.parentNode)"
					"host.parentNode.removeChild(host);"
				"globalThis.__selLeft="
					"document.querySelectorAll('.comment-form__verbum').length;";
			const char *cleanup_chk =
				"if(globalThis.__selLeft!==0)"
					"throw new Error('ASSERT FAIL: Test 15 leaked '+globalThis.__selLeft+"
						"' .comment-form__verbum node(s) into the shared document');";
			ok = js_exec(thread, (const unsigned char *)cleanup, strlen(cleanup),
					"sel-cleanup.js");
			if (!ok) { fprintf(stderr, "FAIL: selector cleanup threw\n"); return 1; }
			ok = js_exec(thread, (const unsigned char *)cleanup_chk,
					strlen(cleanup_chk), "sel-cleanup-chk.js");
			if (!ok) { fprintf(stderr, "FAIL: Test 15 fixture leaked\n"); return 1; }
		}
		fprintf(stderr, "class-only mount query works; token (not substring) class "
				"matching; tag.class narrows; descendant constrains by ancestor; "
				"'#a .b' returns the inner element; fixture removed\n");
	}
	fprintf(stderr, "=== Test 15 PASS: class + descendant selectors ===\n");

	/* --- Test 16 (fixes872, #300): GATE 5 -- `"onclick" in el` must be TRUE.
	 * Preact picks an event's NAME from whether the property exists (verbatim):
	 *     a = t.toLowerCase(),
	 *     t = a in e || "onFocusOut"==t || "onFocusIn"==t ? a.slice(2) : t.slice(2),
	 *     ... e.addEventListener(t, i?p:m, i)
	 * For onClick: if `"onclick" in e` is TRUE  -> addEventListener("click").
	 *              if FALSE -> addEventListener("Click") -- capital C, which
	 * nothing ever dispatches. The form then renders PERFECTLY and ignores every
	 * click. That is the worst failure shape there is: it looks finished, so the
	 * only way to catch it is to assert the mechanism, which is what this does --
	 * including a literal replay of Preact's own name-picking expression.
	 *
	 * `el.onclick =` appears ZERO times in the bundle, so replace semantics are
	 * not required by Verbum -- but they are implemented and asserted here for
	 * the many sites that DO assign on*, where an accumulating handler would be
	 * a nasty, silent bug. --- */
	fprintf(stderr, "\n=== Test 16: on* handler properties ===\n");
	{
		const char *probe =
			"globalThis.__h={};var h=globalThis.__h;"
			"var e=document.createElement('button');"
			"document.body.appendChild(e);"
			/* THE requirement */
			"h.inClick=('onclick' in e);"
			"h.inChange=('onchange' in e);"
			"h.inInput=('oninput' in e);"
			"h.inKeyDown=('onkeydown' in e);"
			"h.inBlur=('onblur' in e);"
			/* Preact's ACTUAL name-picking expression, replayed verbatim */
			"var t='onClick';var a=t.toLowerCase();"
			"h.preactName=(a in e||'onFocusOut'==t||'onFocusIn'==t)?a.slice(2):t.slice(2);"
			/* default is null, not undefined */
			"h.initial=(e.onclick===null);"
			/* replace semantics: second assignment must REPLACE the first */
			"h.a=0;h.b=0;"
			"e.onclick=function(){h.a++;};"
			"e.onclick=function(){h.b++;};"
			"h.getterIsFn=(typeof e.onclick==='function');"
			/* addEventListener must ACCUMULATE alongside, not replace */
			"h.l=0;e.addEventListener('click',function(){h.l++;});"
			"e.dispatchEvent({type:'click'});"
			/* null clears it */
			"e.onclick=null;h.clearedNull=(e.onclick===null);"
			"e.dispatchEvent({type:'click'});";
		unsigned char ok = js_exec(thread, (const unsigned char *)probe,
				strlen(probe), "on-probe.js");
		if (!ok) { fprintf(stderr, "FAIL: on* probe threw\n"); return 1; }
		{
			const char *chk =
				"var h=globalThis.__h;"
				"if(!h.inClick)"
					"throw new Error('ASSERT FAIL: \"onclick\" in element is FALSE. "
						"Preact then does addEventListener(\"Click\") -- capital C, "
						"which nothing dispatches. The comment form renders "
						"perfectly and ignores every click.');"
				"if(!h.inChange||!h.inInput||!h.inKeyDown||!h.inBlur)"
					"throw new Error('ASSERT FAIL: missing on* props: change='+h.inChange+"
						"' input='+h.inInput+' keydown='+h.inKeyDown+' blur='+h.inBlur+"
						"' -- all are used by verbum-comments.js');"
				"if(h.preactName!=='click')"
					"throw new Error('ASSERT FAIL: Preact would register the event as \"'+"
						"h.preactName+'\" instead of \"click\" -- this is its real "
						"name-picking expression, so the handler would never fire');"
				"if(!h.initial)"
					"throw new Error('ASSERT FAIL: el.onclick should default to null, got '+"
						"(typeof h.initial));"
				"if(!h.getterIsFn)"
					"throw new Error('ASSERT FAIL: the on* getter does not return the "
						"assigned function');"
				"if(h.a!==0)"
					"throw new Error('ASSERT FAIL: the REPLACED onclick handler still fired "
						"('+h.a+' times) -- on* assignment must replace, not "
						"accumulate');"
				"if(h.b!==1)"
					"throw new Error('ASSERT FAIL: the current onclick handler fired '+h.b+"
						"' times, expected exactly 1');"
				/* TWO dispatches happen in the probe (one before onclick=null,
				 * one after) and the addEventListener listener is never removed,
				 * so 2 is correct. It also proves the two routes are INDEPENDENT:
				 * clearing on* must not silently unregister the listener. */
				"if(h.l!==2)"
					"throw new Error('ASSERT FAIL: addEventListener handler fired '+h.l+"
						"' times across 2 dispatches, expected 2 -- on* and "
						"addEventListener are independent routes; clearing on* "
						"must not disturb the listener list');"
				"if(!h.clearedNull)"
					"throw new Error('ASSERT FAIL: el.onclick=null did not clear it');"
				"if(h.b!==1)"
					"throw new Error('ASSERT FAIL: handler still fired after being set to "
						"null (b='+h.b+')');";
			ok = js_exec(thread, (const unsigned char *)chk, strlen(chk), "on-chk.js");
			if (!ok) {
				fprintf(stderr, "FAIL: Gate 5 is shut -- the form would render and "
						"then ignore every click\n");
				return 1;
			}
		}
		fprintf(stderr, "\"onclick\" in el is true (Preact resolves 'click', not "
				"'Click'); assignment replaces; addEventListener accumulates "
				"alongside; null clears\n");
	}
	fprintf(stderr, "=== Test 16 PASS: on* handler properties ===\n");

	/* --- Test 17 (CAPSTONE): the whole chain, end to end.
	 *
	 * The five gates are SERIAL, so five green unit tests can still add up to a
	 * red chain -- that is exactly the false-green class Test 11's old
	 * `appended=1` assert belonged to. This runs the real sequence in one go and
	 * asserts the thing a user would actually check: a button exists and
	 * clicking it runs code.
	 *
	 * Mirrors hackaday's actual loader shape:
	 *   dynamic-loader.js:  querySelector(mount) -> loadScript('wp-polyfill')
	 *                       -> .then(() => loadScript('verbum'))
	 *   each loadScript:    createElement('script'); s.onload = resolve;
	 *                       s.src = url; body.appendChild(s)
	 *   verbum body:        querySelectorAll('.comment-form__verbum').forEach(
	 *                         createElementNS -> on* bind -> appendChild)
	 *
	 * Every gate is load-bearing here and a failure in ANY of them turns this
	 * red:
	 *   #294 job queue      -- .then() never advances, chain stops at 0
	 *   #295 script.onload  -- promise never settles, verbum never requested
	 *   #297 createElementNS-- Preact's factory throws, nothing is built
	 *   #298 .class         -- the mount query is empty, forEach runs 0 times
	 *   #300 on*            -- registers "Click", the button ignores the click
	 *
	 * The C side stands in for the network + core: it finds each injected
	 * <script>, runs its "body", and fires load -- which is what
	 * html_script_exec does on real hardware. --- */
	fprintf(stderr, "\n=== Test 17 (CAPSTONE): loader -> onload -> Preact -> "
			"a button you can click ===\n");
	{
		extern void macsurf_qjs_pump_all(void);
		unsigned char ok;
		int pump;
		int step;
		/* The two scripts the chain loads, in order. */
		const char *want[2];
		want[0] = "polyfill";
		want[1] = "verbum";

		/* The parent page's empty Preact mount, as served in the child frame. */
		{
			const char *arm =
				"var host=document.createElement('div');"
				"host.id='cap-host';"
				"host.innerHTML='<div class=\"comment-form__verbum dark\"></div>';"
				"document.body.appendChild(host);"
				"globalThis.__cap={log:[],chain:0,clicked:0};";
			ok = js_exec(thread, (const unsigned char *)arm, strlen(arm), "cap-arm.js");
			if (!ok) { fprintf(stderr, "FAIL: capstone arm threw\n"); return 1; }
		}

		/* The loader. Note it resolves ONLY on script.onload, like the real one. */
		{
			const char *loader =
				"var cap=globalThis.__cap;"
				"function loadScript(id){"
				"return new Promise(function(resolve,reject){"
					"var s=document.createElement('script');"
					"s.id=id;"
					"s.onload=function(){cap.log.push('load:'+id);resolve(id);};"
					"s.onerror=function(){cap.log.push('err:'+id);reject(id);};"
					"s.src='https://example.invalid/'+id+'.js';"
					"document.body.appendChild(s);"
				"});}"
				/* the mount gate the real loader checks first */
				"cap.mountSeen=document.querySelector('#cap-host')?1:0;"
				"loadScript('polyfill').then(function(){"
					"cap.chain=1;"
					"return loadScript('verbum');"
				"}).then(function(){cap.chain=2;})"
				".catch(function(e){cap.err=''+e;});";
			ok = js_exec(thread, (const unsigned char *)loader, strlen(loader),
					"cap-loader.js");
			if (!ok) { fprintf(stderr, "FAIL: capstone loader threw\n"); return 1; }
		}

		/* Drive the chain: for each expected script, find it, run its body, fire
		 * load -- then pump so the .then() reactions can queue the next one. */
		for (step = 0; step < 2; step++) {
			dom_string *id_str = NULL;
			dom_element *el = NULL;

			for (pump = 0; pump < 8; pump++) macsurf_qjs_pump_all();

			if (dom_string_create((const uint8_t *)want[step],
					(unsigned)strlen(want[step]), &id_str) != DOM_NO_ERR) {
				fprintf(stderr, "FAIL: dom_string_create\n"); return 1;
			}
			dom_document_get_element_by_id(document, id_str, &el);
			dom_string_unref(id_str);

			if (el == NULL) {
				fprintf(stderr, "FAIL: CAPSTONE broke at step %d: the loader never "
						"injected <script id=%s>.%s\n", step, want[step],
						step == 1 ? " The first script's onload never resolved "
							"its promise, so the chain stopped -- verbum is "
							"never even requested (#294/#295)."
						 : " The very first injection never landed (#292).");
				return 1;
			}

			/* Step 1 is verbum: run its body BEFORE firing load, exactly as
			 * html_script_exec does (a loader resolving on load expects the
			 * script's globals to already exist). */
			if (step == 1) {
				const char *verbum_body =
					"(function(){var cap=globalThis.__cap;"
					"var mounts=document.querySelectorAll('.comment-form__verbum');"
					"cap.mounts=mounts.length;"
					"mounts.forEach(function(root){"
						"var XHTML='http://www.w3.org/1999/xhtml';"
						"var props={className:'verbum-submit'};"
						/* Preact's factory, with its real 3rd-arg expression */
						"var btn=document.createElementNS(XHTML,'button',"
							"props.is&&props);"
						"btn.className=props.className;"
						/* Preact's real on*-name resolution */
						"var t='onClick';var a=t.toLowerCase();"
						"var nm=(a in btn)?a.slice(2):t.slice(2);"
						"cap.evName=nm;"
						"btn.addEventListener(nm,function(){cap.clicked++;});"
						"root.appendChild(btn);"
					"});})();";
				ok = js_exec(thread, (const unsigned char *)verbum_body,
						strlen(verbum_body), "cap-verbum.js");
				if (!ok) {
					fprintf(stderr, "FAIL: CAPSTONE broke: the verbum body threw "
							"while rendering\n");
					dom_node_unref((dom_node *)el);
					return 1;
				}
			}

			js_fire_script_load(thread, (dom_node *)el, 1);
			dom_node_unref((dom_node *)el);
		}
		for (pump = 0; pump < 8; pump++) macsurf_qjs_pump_all();

		/* Now the user's test: is there a button, and does clicking it work? */
		{
			const char *click =
				"var cap=globalThis.__cap;"
				"var b=document.querySelector('.comment-form__verbum .verbum-submit');"
				"cap.btn=b?1:0;"
				"cap.btnTag=(b&&b.tagName||'?').toLowerCase();"
				"if(b)b.dispatchEvent({type:'click'});";
			ok = js_exec(thread, (const unsigned char *)click, strlen(click),
					"cap-click.js");
			if (!ok) { fprintf(stderr, "FAIL: capstone click threw\n"); return 1; }
		}
		{
			const char *chk =
				"var cap=globalThis.__cap;"
				"if(cap.err)"
					"throw new Error('ASSERT FAIL: the loader chain REJECTED: '+cap.err);"
				"if(!cap.mountSeen)"
					"throw new Error('ASSERT FAIL: the loader could not even find its "
						"host element (#290)');"
				"if(cap.chain!==2)"
					"throw new Error('ASSERT FAIL: the promise chain reached step '+"
						"cap.chain+' of 2. log=['+cap.log.join(',')+'] -- on real "
						"hardware this is the exact shape of \"wp-polyfill "
						"executed, verbum never requested\" (#294/#295)');"
				"if(cap.mounts!==1)"
					"throw new Error('ASSERT FAIL: the mount query matched '+cap.mounts+"
						"' elements, expected 1 (#298)');"
				"if(cap.evName!=='click')"
					"throw new Error('ASSERT FAIL: Preact resolved the event name to \"'+"
						"cap.evName+'\", expected \"click\" (#300)');"
				"if(!cap.btn)"
					"throw new Error('ASSERT FAIL: no button in the DOM -- Preact built "
						"nothing, or it never got connected (#297)');"
				"if(cap.btnTag!=='button')"
					"throw new Error('ASSERT FAIL: created <'+cap.btnTag+'>, expected "
						"<button>');"
				"if(cap.clicked!==1)"
					"throw new Error('ASSERT FAIL: THE BUTTON RENDERED BUT THE CLICK DID "
						"NOTHING (handler ran '+cap.clicked+' times). This is the "
						"failure that looks like success (#300).');";
			ok = js_exec(thread, (const unsigned char *)chk, strlen(chk), "cap-chk.js");
			if (!ok) {
				fprintf(stderr, "FAIL: CAPSTONE -- the five gates pass individually "
						"but the CHAIN does not complete\n");
				return 1;
			}
		}
		fprintf(stderr, "loader -> script.onload -> promise chain -> second script -> "
				"createElementNS -> .class mount -> on* bind -> CLICK HANDLER RAN\n");

		/* Remove the fixture, same reason Test 15 does: it holds a
		 * .comment-form__verbum mount, and Test 18 below counts those. Test 18
		 * reported mount=2 until this landed. */
		{
			const char *cleanup =
				"var h=document.querySelector('#cap-host');"
				"if(h&&h.parentNode)h.parentNode.removeChild(h);"
				"globalThis.__capLeft="
					"document.querySelectorAll('.comment-form__verbum').length;";
			const char *cleanup_chk =
				"if(globalThis.__capLeft!==0)"
					"throw new Error('ASSERT FAIL: capstone leaked '+globalThis.__capLeft+"
						"' mount node(s) into the shared document');";
			ok = js_exec(thread, (const unsigned char *)cleanup, strlen(cleanup),
					"cap-cleanup.js");
			if (!ok) { fprintf(stderr, "FAIL: capstone cleanup threw\n"); return 1; }
			ok = js_exec(thread, (const unsigned char *)cleanup_chk,
					strlen(cleanup_chk), "cap-cleanup-chk.js");
			if (!ok) { fprintf(stderr, "FAIL: capstone fixture leaked\n"); return 1; }
		}
	}
	fprintf(stderr, "=== Test 17 PASS (CAPSTONE): the full reply-box chain "
			"completes end to end ===\n");

	/* --- Test 18: run the REAL verbum-comments.js (86,970 B, Preact 10.26/27).
	 * Hardware (2026-07-17) got the whole chain running -- dynamic-loader ->
	 * wp-polyfill -> verbum-comments.js all EXECUTED, `script fired load`x2,
	 * zero errors -- and then the bundle THREW, leaving the iframe at boxes=6
	 * (empty shell). The log line that should have named the exception was
	 * truncated to "Erro" by the 255-byte cap (the URL ate the budget; fixes873
	 * reorders it), so hardware could not say what broke.
	 *
	 * It does not have to. The real bundle is a file, and this harness runs the
	 * same QuickJS + same DOM glue on Linux, where the full message and stack are
	 * free. Synthetic gate tests answer "is the mechanism present"; only the real
	 * bundle answers "is it ENOUGH".
	 *
	 * DIAGNOSTIC, NOT A GATE: this reports whatever the bundle does and does not
	 * fail the suite. Verbum legitimately needs browser surface we have not built
	 * yet, so a throw here is information (the next target), not a regression.
	 * It fails ONLY if the bundle is missing -- silence must never look like
	 * success. --- */
	fprintf(stderr, "\n=== Test 18 (DIAGNOSTIC): the real verbum-comments.js ===\n");
	{
		FILE *vf = fopen("verbum-comments.js", "rb");
		if (vf == NULL) {
			fprintf(stderr, "SKIP: verbum-comments.js not present next to the "
					"harness (copy it from the HAR to run this leg)\n");
		} else {
			char *vsrc;
			long vlen;
			unsigned char ok;

			fseek(vf, 0, SEEK_END); vlen = ftell(vf); fseek(vf, 0, SEEK_SET);
			vsrc = (char *)malloc((size_t)vlen + 1);
			if (vsrc == NULL) { fclose(vf); fprintf(stderr, "FAIL: oom\n"); return 1; }
			if (fread(vsrc, 1, (size_t)vlen, vf) != (size_t)vlen) {
				fclose(vf); free(vsrc);
				fprintf(stderr, "FAIL: short read of verbum-comments.js\n");
				return 1;
			}
			fclose(vf);
			vsrc[vlen] = '\0';

			/* The child frame's own HTML supplies BOTH of these before the
			 * bundle runs, so they are FIXTURE, not browser surface:
			 *  - the mount + the real (second) #commentform;
			 *  - the `VerbumComments` config global. Its property set is read
			 *    straight out of the bundle (every VerbumComments.<prop> plus
			 *    the destructured ones), so this is the real shape rather than a
			 *    guess. Values match the guest-comment state established from
			 *    the HAR: comment_registration=0 (guests allowed),
			 *    require_name_email=1.
			 * Not providing them would make the bundle throw for a reason that
			 * says nothing about MacSurf. */
			{
				const char *mount =
					"var vhost=document.createElement('div');"
					"vhost.id='verbum-host';"
					"vhost.innerHTML='<form id=\"commentform\" method=\"post\">'+"
						"'<div class=\"comment-form__verbum dark\"></div>'+"
						"'</form>';"
					"document.body.appendChild(vhost);"
					"globalThis.VerbumComments={"
						"siteId:'123456',postId:'99',"
						"mustLogIn:false,commentRegistration:false,"
						"requireNameEmail:true,"
						"connectURL:'https://jetpack.wordpress.com/connect',"
						"logoutURL:'https://jetpack.wordpress.com/logout',"
						"subscribeToComment:false,subscribeToBlog:false,"
						"enableSubscriptionModal:false,enableBlocks:false,"
						"currentLocale:'en',isRTL:false,colorScheme:'dark',"
						"embedNonce:'nonce',vbeCacheBuster:'1',"
						"jetpackAvatar:'',jetpackSignature:'',"
						"jetpackUserId:0,jetpackUsername:'',"
						"isJetpackComments:true,isJetpackCommentsLoggedIn:false,"
						"fullyLoadedTime:0};";
				ok = js_exec(thread, (const unsigned char *)mount, strlen(mount),
						"verbum-mount.js");
				if (!ok) { free(vsrc); fprintf(stderr, "FAIL: mount threw\n"); return 1; }
			}

			fprintf(stderr, "--- running the real bundle (%ld bytes) ---\n", vlen);
			ok = js_exec(thread, (const unsigned char *)vsrc, (size_t)vlen,
					"verbum-comments.js");
			free(vsrc);

			{
				const char *report =
					"globalThis.__v={};var v=globalThis.__v;"
					"v.mount=document.querySelectorAll('.comment-form__verbum').length;"
					"var m=document.querySelector('.comment-form__verbum');"
					"v.kids=m?m.childNodes.length:-1;"
					"v.html=m?(m.innerHTML||'').length:-1;"
					"v.inputs=document.querySelectorAll('textarea').length;";
				(void)js_exec(thread, (const unsigned char *)report,
						strlen(report), "verbum-report.js");
			}
			{
				const char *emit =
					"console.error('WORK verbum mount='+globalThis.__v.mount+"
					"' kidsInMount='+globalThis.__v.kids+"
					"' mountHtmlLen='+globalThis.__v.html+"
					"' textareas='+globalThis.__v.inputs);"
					"var mm=document.querySelector('.comment-form__verbum');"
					"console.error('WORK verbum HTML: '+(mm?(mm.innerHTML||'(empty)'):'(no mount)'));"
					"var ta=document.querySelector('textarea');"
					"console.error('WORK verbum textarea: '+(ta?('name='+ta.getAttribute('name')+"
						"' placeholder='+ta.getAttribute('placeholder')):'(none)'))";
				(void)js_exec(thread, (const unsigned char *)emit, strlen(emit),
						"verbum-emit.js");
			}

			if (!ok) {
				fprintf(stderr, "=== Test 18: the real bundle THREW -- the message "
						"and stack above are the next target ===\n");
				return 1;
			}
			/* It renders now, so this is a GATE, not a diagnostic: the real
			 * bundle must build a real comment box. Asserting the textarea (not
			 * just "didn't throw") is the point -- Preact can swallow an error
			 * and render nothing while the bundle still "succeeds". */
			{
				const char *chk =
					"var v=globalThis.__v;"
					"if(v.mount!==1)"
						"throw new Error('ASSERT FAIL: expected exactly 1 verbum mount, "
							"got '+v.mount+' -- a leftover fixture from an earlier "
							"test, or the mount query broke');"
					"if(v.inputs<1)"
						"throw new Error('ASSERT FAIL: the real verbum bundle ran but "
							"built NO textarea -- there is no comment box');"
					"var ta=document.querySelector('textarea');"
					"if(!ta||ta.getAttribute('name')!=='comment')"
						"throw new Error('ASSERT FAIL: the textarea is not the comment "
							"field (name='+(ta&&ta.getAttribute('name'))+')');"
					"if(v.html<1)"
						"throw new Error('ASSERT FAIL: the mount is empty (innerHTML len "
							"'+v.html+') -- Preact rendered nothing into it');";
				ok = js_exec(thread, (const unsigned char *)chk, strlen(chk),
						"verbum-chk.js");
				if (!ok) {
					fprintf(stderr, "FAIL: the real bundle ran but produced no "
							"comment box\n");
					return 1;
				}
			}
			fprintf(stderr, "=== Test 18 PASS: the REAL verbum-comments.js (86,970 B, "
					"Preact) runs clean and builds a real comment box ===\n");
		}
	}

	/* --- Test 19 (fixes876): setTimeout MUST forward its extra arguments,
	 * bind `this`, and give requestAnimationFrame a real DOMHighResTimeStamp.
	 *
	 * qjs_settimeout_impl stored only argv[0] and macsurf_qjs_run_timers called
	 * it with JS_Call(qctx, fn, JS_UNDEFINED, 0, NULL) -- zero arguments. So:
	 *   - setTimeout(fn, 0, a, b) silently dropped a and b;
	 *   - rAF (a one-liner over setTimeout(fn,16)) handed its callback NOTHING,
	 *     so the ubiquitous `function(t){ dt = t - last; }` idiom saw undefined,
	 *     made dt NaN, and every delta-driven animation loop silently stalled.
	 *
	 * The rAF timestamp is checked for MONOTONIC ADVANCE across two frames, not
	 * merely for being a number: passing the time as a setTimeout extra arg
	 * would compile and return a plausible number frozen at REGISTRATION time.
	 * Only a fire-time read advances, so this assert is what distinguishes the
	 * real fix from the plausible-but-wrong one.
	 *
	 * NEGATIVE CONTROL: verified red against the pre-fix tree -- the arg assert
	 * throws ('got undefined,undefined') and the rAF assert throws on a
	 * non-number timestamp. */
	fprintf(stderr, "\n=== Test 19: setTimeout extra args + this-binding + rAF "
			"timestamp ===\n");
	{
		extern void macsurf_qjs_pump_all(void);
		const char *arm =
			"globalThis.__args=null;globalThis.__this_ok=0;"
			"globalThis.__raf=[];"
			"setTimeout(function(a,b){"
				"globalThis.__args=[a,b];"
				"globalThis.__this_ok=(this===globalThis)?1:0;"
			"},0,'A','B');"
			/* two chained frames: the second is registered from inside the
			 * first, so its timestamp must be strictly later. */
			"requestAnimationFrame(function(t1){"
				"globalThis.__raf.push(t1);"
				"requestAnimationFrame(function(t2){globalThis.__raf.push(t2);});"
			"});";
		const char *chk =
			"if(globalThis.__args===null)"
				"throw new Error('ASSERT FAIL: the 0ms timer never fired at all');"
			"if(globalThis.__args[0]!=='A'||globalThis.__args[1]!=='B')"
				"throw new Error('ASSERT FAIL: setTimeout dropped its extra args -- "
					"got '+globalThis.__args[0]+','+globalThis.__args[1]);"
			"if(!globalThis.__this_ok)"
				"throw new Error('ASSERT FAIL: setTimeout callback `this` is not the "
					"global (HTML spec says it is the window)');"
			"if(globalThis.__raf.length<2)"
				"throw new Error('ASSERT FAIL: rAF did not deliver 2 frames, got '"
					"+globalThis.__raf.length);"
			"if(typeof globalThis.__raf[0]!=='number')"
				"throw new Error('ASSERT FAIL: rAF callback got no DOMHighResTimeStamp "
					"(typeof '+(typeof globalThis.__raf[0])+') -- every `t - last` "
					"animation loop is NaN');"
			"if(!(globalThis.__raf[1]>globalThis.__raf[0]))"
				"throw new Error('ASSERT FAIL: rAF timestamp did not ADVANCE between "
					"frames ('+globalThis.__raf[0]+' -> '+globalThis.__raf[1]+') -- it "
					"is frozen at registration time, not read at fire time');";
		unsigned char ok;
		int pump;

		ok = js_exec(thread, (const unsigned char *)arm, strlen(arm), "timer-arm.js");
		if (!ok) { fprintf(stderr, "FAIL: timer arm threw\n"); return 1; }

		/* rAF is setTimeout(...,16) and the timebase is a real clock, so the
		 * frames need real elapsed time. Pump around each sleep. */
		for (pump = 0; pump < 6; pump++) {
			macsurf_qjs_pump_all();
			usleep(12000);
			macsurf_qjs_pump_all();
		}

		ok = js_exec(thread, (const unsigned char *)chk, strlen(chk), "timer-chk.js");
		if (!ok) {
			fprintf(stderr, "FAIL: setTimeout/rAF argument contract broken\n");
			return 1;
		}
		fprintf(stderr, "extra args forwarded; `this`===globalThis; rAF timestamps "
				"advance across frames\n");
	}
	fprintf(stderr, "=== Test 19 PASS: setTimeout forwards extra args, binds `this`, "
			"and rAF delivers an advancing timestamp ===\n");

	/* --- Test 20 (fixes877): arena overflow MUST NOT evict the timer that is
	 * about to fire.
	 *
	 * timer_alloc's eviction loop used `<` on expiry_ms, selecting the MINIMUM
	 * deadline -- the soonest-expiring timer, i.e. the one closest to running
	 * and most likely to be needed imminently. So a page that briefly
	 * over-filled the arena silently lost the callback that was due NOW while
	 * keeping ones due minutes later.
	 *
	 * The shape of this test matters: the victim must be armed FIRST and then
	 * buried under far-future registrations. Arming the 0ms timer LAST would
	 * pass either way, because the eviction victim would be some OTHER slot and
	 * the new timer would get the freed one regardless of the comparison
	 * direction -- a test that cannot distinguish the bug from the fix.
	 *
	 * NEGATIVE CONTROL: verified red against the pre-fix tree ("the 0ms timer
	 * was evicted"). */
	fprintf(stderr, "\n=== Test 20: arena overflow evicts the furthest-out timer, "
			"not the imminent one ===\n");
	{
		extern void macsurf_qjs_pump_all(void);
		/* 400 > QJS_MAX_TIMERS (256), so eviction is guaranteed. Each filler is
		 * ~16 minutes out, so every one of them is further out than the 0ms
		 * timer armed first. */
		const char *arm =
			"globalThis.__imminent=0;"
			"setTimeout(function(){globalThis.__imminent=1;},0);"
			"var i;for(i=0;i<400;i++){setTimeout(function(){},1000000);}";
		const char *chk =
			"if(!globalThis.__imminent)"
				"throw new Error('ASSERT FAIL: the 0ms timer was EVICTED by later "
					"far-future registrations -- arena overflow is dropping the "
					"callback that was about to fire');";
		unsigned char ok;
		int pump;

		ok = js_exec(thread, (const unsigned char *)arm, strlen(arm), "evict-arm.js");
		if (!ok) { fprintf(stderr, "FAIL: eviction arm threw\n"); return 1; }
		for (pump = 0; pump < 8; pump++) macsurf_qjs_pump_all();

		ok = js_exec(thread, (const unsigned char *)chk, strlen(chk), "evict-chk.js");
		if (!ok) {
			fprintf(stderr, "FAIL: arena overflow evicted the imminent timer\n");
			return 1;
		}
		fprintf(stderr, "imminent 0ms timer survived 400 far-future registrations\n");
	}
	fprintf(stderr, "=== Test 20 PASS: overflow evicts furthest-out; the imminent "
			"timer still fires ===\n");

	/* --- Test 21 (fixes878): node-oriented traversal is REAL, and cloneNode
	 * COPIES instead of returning the element itself.
	 *
	 * qjs_el_install_js_helpers used to hardcode the whole node surface as
	 * constants -- cloneNode returned `el`, contains returned false, childNodes
	 * was a fresh [] forever, and firstChild/lastChild/nextSibling/
	 * previousSibling were `null` data properties frozen at wrap time. None of
	 * them shadowed a native; they WERE the implementation. So each was a wrong
	 * answer rather than a missing one, which is why nothing ever threw.
	 *
	 * The cloneNode assert is the sharp one: returning `el` means
	 * parent.appendChild(node.cloneNode(true)) MOVES the original, so a page
	 * that meant N copies silently rendered one relocated node.
	 *
	 * This test removes its own fixture: the harness shares one document. */
	fprintf(stderr, "\n=== Test 21: real node traversal + cloneNode copies ===\n");
	{
		const char *arm =
			"var box=document.createElement('div');"
			"box.setAttribute('id','trav-fixture');"
			"document.body.appendChild(box);"
			/* text + element + comment: firstChild must see the TEXT, and
			 * childNodes must count all three, where `children` sees 1. */
			"box.appendChild(document.createTextNode('lead'));"
			"var inner=document.createElement('span');"
			"inner.setAttribute('class','kid');"
			"box.appendChild(inner);"
			"globalThis.__t={};"
			"var t=globalThis.__t;"
			"t.first_type=box.firstChild?box.firstChild.nodeType:-1;"
			"t.last_is_span=(box.lastChild===inner);"
			"t.childNodes_len=box.childNodes.length;"
			"t.children_len=box.children.length;"
			"t.next_of_first=box.firstChild?(box.firstChild.nextSibling===inner):false;"
			"t.prev_of_span=(inner.previousSibling===box.firstChild);"
			"t.contains_kid=box.contains(inner);"
			"t.contains_self=box.contains(box);"
			"t.contains_foreign=box.contains(document.body);"
			/* THE clone bug */
			"var clone=inner.cloneNode(true);"
			"t.clone_is_self=(clone===inner);"
			"t.clone_parent_null=(clone.parentNode===null);"
			"box.appendChild(clone);"
			"t.after_clone_children=box.children.length;"
			"t.original_still_in=(inner.parentNode===box);"
			/* the canonical clear-children idiom */
			"var guard=0;"
			"while(box.firstChild&&guard<100){box.removeChild(box.firstChild);guard++;}"
			"t.cleared=box.childNodes.length;"
			"t.guard=guard;"
			"document.body.removeChild(box);";
		const char *chk =
			"var t=globalThis.__t;"
			"if(t.first_type!==3)"
				"throw new Error('ASSERT FAIL: firstChild did not return the leading "
					"TEXT node (nodeType '+t.first_type+') -- node traversal is still "
					"hardcoded');"
			"if(!t.last_is_span)"
				"throw new Error('ASSERT FAIL: lastChild is wrong');"
			"if(t.childNodes_len!==2)"
				"throw new Error('ASSERT FAIL: childNodes saw '+t.childNodes_len+', "
					"expected 2 (text + span)');"
			"if(t.children_len!==1)"
				"throw new Error('ASSERT FAIL: children (elements only) saw '"
					"+t.children_len+', expected 1');"
			"if(!t.next_of_first||!t.prev_of_span)"
				"throw new Error('ASSERT FAIL: nextSibling/previousSibling do not link');"
			"if(!t.contains_kid)"
				"throw new Error('ASSERT FAIL: contains(descendant) is false');"
			"if(!t.contains_self)"
				"throw new Error('ASSERT FAIL: contains(self) must be true per spec');"
			"if(t.contains_foreign)"
				"throw new Error('ASSERT FAIL: contains(ancestor) must be false');"
			"if(t.clone_is_self)"
				"throw new Error('ASSERT FAIL: cloneNode returned the element ITSELF -- "
					"clone-and-append MOVES the original instead of copying it');"
			"if(!t.clone_parent_null)"
				"throw new Error('ASSERT FAIL: a fresh clone must be parentless');"
			"if(t.after_clone_children!==2)"
				"throw new Error('ASSERT FAIL: after appending the clone the box has '"
					"+t.after_clone_children+' element children, expected 2 (original + "
					"copy) -- the original was MOVED, not copied');"
			"if(!t.original_still_in)"
				"throw new Error('ASSERT FAIL: the original left its parent -- it was "
					"moved by the clone-append');"
			"if(t.cleared!==0)"
				"throw new Error('ASSERT FAIL: the while(firstChild) removeChild idiom "
					"left '+t.cleared+' children');"
			"if(t.guard===0)"
				"throw new Error('ASSERT FAIL: the clear loop never ran even once -- "
					"firstChild was falsy from the start');";
		unsigned char ok;

		ok = js_exec(thread, (const unsigned char *)arm, strlen(arm), "trav-arm.js");
		if (!ok) { fprintf(stderr, "FAIL: traversal arm threw\n"); return 1; }
		ok = js_exec(thread, (const unsigned char *)chk, strlen(chk), "trav-chk.js");
		if (!ok) {
			fprintf(stderr, "FAIL: node traversal / cloneNode contract broken\n");
			return 1;
		}
		fprintf(stderr, "firstChild/lastChild/siblings/childNodes/contains real; "
				"cloneNode copies; clear-children idiom empties the node\n");
	}
	fprintf(stderr, "=== Test 21 PASS: node traversal is real libdom and cloneNode "
			"copies ===\n");

	/* --- Test 22 (fixes879): the jar must not hand HttpOnly cookies to script,
	 * and document.cookie must be a real accessor.
	 *
	 * SCOPE, stated honestly: this harness's html_content is memset to 0, so
	 * c->llcache is NULL and qjs_cookie_doc_url() correctly returns NULL --
	 * the JS getter therefore returns '' here no matter what, and the full
	 * JS->jar round trip is HARDWARE-GATED. struct llcache_handle is private to
	 * llcache.c, so fabricating one would mean guessing a struct layout: a
	 * worse test than an honest gap.
	 *
	 * What IS verified, because it is the part with a security consequence and
	 * it is fully testable (urldb.c and nsurl are compiled in):
	 *   (a) that include_http_only=false -- the argument the getter passes --
	 *       really withholds HttpOnly cookies. THIS TEST CAUGHT A REAL BUG:
	 *       urldb_get_cookie gathers matches in four loops and only the
	 *       exact-path one had the HttpOnly gate, so the ordinary Path=/
	 *       session cookie was handed out anyway. Latent upstream because
	 *       nothing had ever passed false; document.cookie is the first caller
	 *       that does, which would have turned it into "any script reads the
	 *       session token".
	 *   (b) the JS shape: `cookie` is an ACCESSOR rather than the old data
	 *       property, and navigator.cookieEnabled is true. */
	fprintf(stderr, "\n=== Test 22: cookie jar HttpOnly gate + document.cookie "
			"accessor shape ===\n");
	{
		nsurl *curl = NULL;
		char *got = NULL;
		unsigned char ok;

		if (nsurl_create("http://cookies.example/path", &curl) != NSERROR_OK ||
		    curl == NULL) {
			fprintf(stderr, "FAIL: nsurl_create for the cookie test\n");
			return 1;
		}
		/* Path=/ so this matches by PREFIX, not exact path -- which is the
		 * loop that was missing the gate, and the shape of essentially every
		 * real session cookie. An exact-path cookie would have passed against
		 * the buggy code and proved nothing. */
		urldb_set_cookie("plain=visible; path=/", curl, NULL);
		urldb_set_cookie("sess=secret; path=/; HttpOnly", curl, NULL);

		got = urldb_get_cookie(curl, false);	/* what document.cookie does */
		if (got == NULL) {
			fprintf(stderr, "FAIL: jar returned nothing for a cookie it was "
					"just given\n");
			nsurl_unref(curl);
			return 1;
		}
		if (strstr(got, "plain=visible") == NULL) {
			fprintf(stderr, "FAIL: script-visible cookie missing from the jar "
					"read: '%s'\n", got);
			free(got); nsurl_unref(curl);
			return 1;
		}
		if (strstr(got, "sess=secret") != NULL) {
			fprintf(stderr, "FAIL: SECURITY -- urldb_get_cookie(url,false) "
					"returned an HttpOnly cookie ('%s'); document.cookie would "
					"hand the session token to any script on the page\n", got);
			free(got); nsurl_unref(curl);
			return 1;
		}
		free(got);

		/* The wire-facing read (what the fetchers pass) must STILL see it --
		 * otherwise the assert above passes for the wrong reason, i.e. the
		 * cookie was never stored at all. */
		got = urldb_get_cookie(curl, true);
		if (got == NULL || strstr(got, "sess=secret") == NULL) {
			fprintf(stderr, "FAIL: the HttpOnly cookie is missing even with "
					"include_http_only=true ('%s') -- the exclusion above "
					"proved nothing\n", got ? got : "(null)");
			if (got) free(got);
			nsurl_unref(curl);
			return 1;
		}
		free(got);
		nsurl_unref(curl);
		fprintf(stderr, "jar: script read hides HttpOnly, wire read keeps it\n");

		{
			const char *chk =
				"var d=Object.getOwnPropertyDescriptor(document,'cookie');"
				"if(!d)throw new Error('ASSERT FAIL: document.cookie is not "
					"defined at all');"
				"if(typeof d.get!=='function')"
					"throw new Error('ASSERT FAIL: document.cookie is still a plain "
						"data property -- writes go to a string and never reach the "
						"jar');"
				"if(typeof document.cookie!=='string')"
					"throw new Error('ASSERT FAIL: document.cookie did not read back "
						"a string');"
				"document.cookie='x=1';"
				"if(navigator.cookieEnabled!==true)"
					"throw new Error('ASSERT FAIL: navigator.cookieEnabled is '"
						"+String(navigator.cookieEnabled)+' -- sites gate their login "
						"flow on it and will show \"please enable cookies\" instead "
						"of the page');"
				/* the navigator shim block as a whole was dead; spot-check a
				 * second property so a future re-ordering fails loudly here */
				"if(navigator.onLine!==true)"
					"throw new Error('ASSERT FAIL: navigator.onLine is '"
						"+String(navigator.onLine)+' -- the navigator extended-shim "
						"block is not running again');";
			ok = js_exec(thread, (const unsigned char *)chk, strlen(chk),
					"cookie-chk.js");
			if (!ok) {
				fprintf(stderr, "FAIL: document.cookie binding shape wrong\n");
				return 1;
			}
		}
		fprintf(stderr, "document.cookie is an accessor; a write does not throw; "
				"cookieEnabled+onLine live\n");
	}
	fprintf(stderr, "=== Test 22 PASS: jar withholds HttpOnly from script; "
			"document.cookie is a real accessor (JS->jar round trip is "
			"HW-gated: no llcache here) ===\n");

	/* --- Test 23 (fixes880): element-scoped querySelector uses the REAL
	 * matcher.
	 *
	 * qjs_el_qsa_data truncated the selector at the first '[', '.', ':' or ' '
	 * and collected by the remaining tag prefix. That produced silent wrong
	 * answers in BOTH directions, which is why both are asserted here:
	 *   - '.foo' truncated to '' -> returned null. Every class-scoped lookup.
	 *   - 'div.bar' truncated to 'div' -> returned EVERY descendant div,
	 *     ignoring the class.
	 * fixes871 fixed the document level and left the element level behind.
	 *
	 * Also asserts the scope rule: per spec the element itself never matches
	 * its own query, though a descendant combinator may still resolve through
	 * ancestors above it.
	 *
	 * NEGATIVE CONTROL: verified red pre-fix on the '.foo' assert. */
	fprintf(stderr, "\n=== Test 23: element-scoped querySelector uses the real "
			"matcher ===\n");
	{
		const char *arm =
			"var box=document.createElement('div');"
			"box.setAttribute('id','qsa-fixture');"
			"box.setAttribute('class','bar');"   /* scope el itself matches .bar */
			"document.body.appendChild(box);"
			"var a=document.createElement('div');a.setAttribute('class','bar');"
			"var b=document.createElement('div');b.setAttribute('class','baz');"
			"var c=document.createElement('span');c.setAttribute('class','bar');"
			"box.appendChild(a);box.appendChild(b);box.appendChild(c);"
			"globalThis.__q={};var q=globalThis.__q;"
			"q.dot_bar=box.querySelectorAll('.bar').length;"
			"q.first_dot_bar_is_a=(box.querySelector('.bar')===a);"
			"q.div_bar=box.querySelectorAll('div.bar').length;"
			"q.all_div=box.querySelectorAll('div').length;"
			"q.miss=box.querySelector('.nope');"
			"q.by_id=(document.querySelector('#qsa-fixture')===box);"
			"document.body.removeChild(box);";
		const char *chk =
			"var q=globalThis.__q;"
			"if(q.dot_bar!==2)"
				"throw new Error('ASSERT FAIL: el.querySelectorAll(\".bar\") found '"
					"+q.dot_bar+', expected 2 (the div and the span inside; NOT the "
					"scope element itself, which also has class=bar)');"
			"if(!q.first_dot_bar_is_a)"
				"throw new Error('ASSERT FAIL: el.querySelector(\".bar\") did not "
					"return the first matching DESCENDANT -- class-scoped lookup is "
					"still truncating the selector to nothing and returning null');"
			"if(q.div_bar!==1)"
				"throw new Error('ASSERT FAIL: el.querySelectorAll(\"div.bar\") found '"
					"+q.div_bar+', expected 1 -- the class qualifier is being ignored "
					"and every descendant div matched');"
			"if(q.all_div!==2)"
				"throw new Error('ASSERT FAIL: bare tag selector regressed: found '"
					"+q.all_div+' divs, expected 2');"
			"if(q.miss!==null)"
				"throw new Error('ASSERT FAIL: a non-matching selector must return "
					"null, got '+String(q.miss));"
			"if(!q.by_id)"
				"throw new Error('ASSERT FAIL: document-level #id regressed');";
		unsigned char ok;

		ok = js_exec(thread, (const unsigned char *)arm, strlen(arm), "qsa-arm.js");
		if (!ok) { fprintf(stderr, "FAIL: qsa arm threw\n"); return 1; }
		ok = js_exec(thread, (const unsigned char *)chk, strlen(chk), "qsa-chk.js");
		if (!ok) {
			fprintf(stderr, "FAIL: element-scoped querySelector contract broken\n");
			return 1;
		}
		fprintf(stderr, "class-scoped lookup works; tag.class no longer over-matches; "
				"scope element excluded\n");
	}
	fprintf(stderr, "=== Test 23 PASS: element-scoped querySelector matches like the "
			"document level ===\n");

	/* --- Test 24 (fixes881): the document lifecycle -- readyState really moves
	 * loading -> interactive -> complete, and DOMContentLoaded precedes load.
	 *
	 * Two compounding bugs:
	 *   - readyState was initialised to 'complete' at realm setup and set to
	 *     'complete' again by js_fire_dom_ready. It was NEVER 'loading' or
	 *     'interactive' -- neither string appeared in the file. So the
	 *     near-universal guard
	 *         if (document.readyState === 'loading')
	 *             addEventListener('DOMContentLoaded', init);
	 *         else init();
	 *     always took the else branch and ran init() synchronously during
	 *     parse, BEFORE the box tree existed: scripts that carefully wait for
	 *     the DOM got exactly what they were avoiding.
	 *   - The events fired in reverse: html_finish_conversion fired `load` at
	 *     the window ~30 lines before dom_to_box, then js_fire_dom_ready fired
	 *     DOMContentLoaded and a SECOND `load` at the document. Observed:
	 *     window load -> DOMContentLoaded -> document load.
	 *
	 * Needs a FRESH realm: the main thread already fired dom_ready in Test 9,
	 * and the fire is idempotent per realm by design.
	 *
	 * NEGATIVE CONTROL: verified red pre-fix -- readyState reads 'complete' at
	 * realm setup. */
	fprintf(stderr, "\n=== Test 24: readyState lifecycle + DOMContentLoaded "
			"precedes load ===\n");
	{
		struct jsheap *heapL = NULL;
		struct jsthread *thL = NULL;
		unsigned char ok;

		if (js_newheap(20000, &heapL) != NSERROR_OK ||
		    js_newthread(heapL, NULL, (void *)&htmlc, &thL) != NSERROR_OK) {
			fprintf(stderr, "FAIL: lifecycle heap setup\n");
			return 1;
		}

		/* At realm setup the page is, by definition, still parsing. */
		{
			const char *arm =
				"globalThis.__seq=[];"
				"globalThis.__rs_at_setup=document.readyState;"
				"document.addEventListener('DOMContentLoaded',function(){"
					"globalThis.__seq.push('dcl:'+document.readyState);});"
				"document.addEventListener('load',function(){"
					"globalThis.__seq.push('load-doc:'+document.readyState);});"
				"window.addEventListener('load',function(){"
					"globalThis.__seq.push('load-win');});";
			ok = js_exec(thL, (const unsigned char *)arm, strlen(arm),
					"life-arm.js");
			if (!ok) { fprintf(stderr, "FAIL: lifecycle arm threw\n"); return 1; }
		}

		js_fire_dom_ready(thL, htmlc.document);
		{
			const char *mid =
				"globalThis.__rs_after_ready=document.readyState;";
			js_exec(thL, (const unsigned char *)mid, strlen(mid), "life-mid.js");
		}

		js_fire_window_load(thL, htmlc.document);
		/* twice on purpose: object.c can call html_proceed_to_done repeatedly
		 * as subresources land, so the per-realm guard must hold. */
		js_fire_window_load(thL, htmlc.document);

		{
			const char *chk =
				"var s=globalThis.__seq;"
				"if(globalThis.__rs_at_setup!=='loading')"
					"throw new Error('ASSERT FAIL: readyState at realm setup is '"
						"+globalThis.__rs_at_setup+', expected loading -- the standard "
						"init guard runs init() synchronously during parse, before the "
						"box tree exists');"
				"if(globalThis.__rs_after_ready!=='interactive')"
					"throw new Error('ASSERT FAIL: readyState after DOMContentLoaded "
						"is '+globalThis.__rs_after_ready+', expected interactive');"
				"if(document.readyState!=='complete')"
					"throw new Error('ASSERT FAIL: readyState after load is '"
						"+document.readyState+', expected complete');"
				"var dcl=s.indexOf('dcl:interactive');"
				"if(dcl<0)"
					"throw new Error('ASSERT FAIL: DOMContentLoaded did not fire at "
						"the document with readyState=interactive; seq='+s.join(','));"
				"var lw=s.indexOf('load-win');"
				"if(lw<0)"
					"throw new Error('ASSERT FAIL: load never fired at WINDOW -- seq='"
						"+s.join(',')+' (it used to fire only at the document)');"
				"if(!(dcl<lw))"
					"throw new Error('ASSERT FAIL: load fired BEFORE DOMContentLoaded "
						"-- seq='+s.join(','));"
				"var n=0,i;for(i=0;i<s.length;i++)if(s[i]==='load-win')n++;"
				"if(n!==1)"
					"throw new Error('ASSERT FAIL: window load fired '+n+' times, "
						"expected exactly 1 (the per-realm guard is not holding); seq='"
						"+s.join(','));";
			ok = js_exec(thL, (const unsigned char *)chk, strlen(chk),
					"life-chk.js");
			if (!ok) {
				fprintf(stderr, "FAIL: document lifecycle contract broken\n");
				return 1;
			}
		}
		js_destroythread(thL);
		js_destroyheap(heapL);
		fprintf(stderr, "loading -> interactive -> complete; DOMContentLoaded "
				"precedes load; load fires once at window\n");
	}
	fprintf(stderr, "=== Test 24 PASS: readyState lifecycle and event order match "
			"the spec ===\n");

	/* --- Test 25 (#304 repro attempt): the NAVIGATION realm reset, which is
	 * what actually crashes on hardware.
	 *
	 * HW signature: unmapped memory at js_shape_hash_unlink+0004C via
	 *   browser_window_callback -> js_newthread -> qjs_flush_timers ->
	 *   JS_FreeValue -> free_object -> js_free_shape -> js_shape_hash_unlink
	 * i.e. the flush DECIDED TO FREE (the fixes875 gate passed) and the free
	 * blew up. Reproduced by refreshing a page (68kmla) with timers pending.
	 *
	 * Test 6 already covers a CLEAN destroy of two heaps; it passes, and it is
	 * not this path. js_newthread's realm reset is different in ways that
	 * matter: it flushes timers, then builds a fresh context ON THE SAME
	 * RUNTIME, frees the old context, and only THEN bumps heap->ctx_gen -- so
	 * during qjs_build_context() the heap still advertises the OLD (ctx, gen)
	 * while a NEW context already exists on the same runtime.
	 *
	 * Two heaps on purpose: a page with an iframe is the hardware situation,
	 * and it is what makes a stale slot from realm A able to meet a recycled
	 * address in realm B. */
	fprintf(stderr, "\n=== Test 25: navigation realm reset with timers pending "
			"(#304 repro) ===\n");
	{
		extern void macsurf_qjs_pump_all(void);
		struct jsheap *hA = NULL, *hB = NULL;
		struct jsthread *tA1 = NULL, *tA2 = NULL, *tB1 = NULL, *tB2 = NULL;
		const char *arm =
			/* a spread of shapes: closures over objects, so the freed
			 * JSValues own real shapes in the runtime's shape table --
			 * which is what js_shape_hash_unlink walks. */
			"var keep=[];"
			"for(var i=0;i<24;i++){(function(n){"
				"var o={a:n,b:'x'+n,c:[n,n+1],d:{deep:{deeper:n}}};"
				"keep.push(o);"
				"setTimeout(function(){return o.d.deep.deeper+n;},100000);"
				"setInterval(function(){return o.c[0];},100000);"
			"})(i);}";
		int pump;

		if (js_newheap(20000, &hA) != NSERROR_OK ||
		    js_newheap(20000, &hB) != NSERROR_OK) {
			fprintf(stderr, "FAIL: heap setup\n"); return 1;
		}
		/* realm 1 in each */
		if (js_newthread(hA, NULL, (void *)&htmlc, &tA1) != NSERROR_OK ||
		    js_newthread(hB, NULL, (void *)&htmlc, &tB1) != NSERROR_OK) {
			fprintf(stderr, "FAIL: thread setup\n"); return 1;
		}
		js_exec(tA1, (const unsigned char *)arm, strlen(arm), "navA1.js");
		js_exec(tB1, (const unsigned char *)arm, strlen(arm), "navB1.js");
		fprintf(stderr, "armed 48 far-future timers per realm across 2 heaps\n");

		/* THE NAVIGATION: a second js_newthread on the SAME heap is exactly
		 * what a refresh does -- flush old timers, free old ctx, fresh realm. */
		if (js_newthread(hA, NULL, (void *)&htmlc, &tA2) != NSERROR_OK) {
			fprintf(stderr, "FAIL: heapA navigation\n"); return 1;
		}
		fprintf(stderr, "heapA navigated (realm reset #1)\n");
		js_exec(tA2, (const unsigned char *)arm, strlen(arm), "navA2.js");

		/* navigate B too: B's fresh ctx may land on A's freed ctx address */
		if (js_newthread(hB, NULL, (void *)&htmlc, &tB2) != NSERROR_OK) {
			fprintf(stderr, "FAIL: heapB navigation\n"); return 1;
		}
		fprintf(stderr, "heapB navigated (realm reset #2)\n");

		/* and again, to churn the allocator into reusing addresses */
		{
			struct jsthread *tA3 = NULL, *tB3 = NULL;
			js_newthread(hA, NULL, (void *)&htmlc, &tA3);
			js_exec(tA3, (const unsigned char *)arm, strlen(arm), "navA3.js");
			js_newthread(hB, NULL, (void *)&htmlc, &tB3);
			for (pump = 0; pump < 4; pump++) macsurf_qjs_pump_all();
			js_destroythread(tA3); js_destroythread(tB3);
		}

		for (pump = 0; pump < 4; pump++) macsurf_qjs_pump_all();

		js_destroythread(tA1); js_destroythread(tA2);
		js_destroythread(tB1); js_destroythread(tB2);
		js_destroyheap(hB);
		js_destroyheap(hA);
		fprintf(stderr, "both heaps torn down after repeated realm resets\n");
	}
	fprintf(stderr, "=== Test 25 PASS: navigation realm reset is clean under ASan "
			"===\n");

	/* --- Test 26 (fixes889): after a reconvert, NO node may hand box_for_node
	 * a freed box. This is the click crash, reproduced.
	 *
	 * HW trace: macos9_handle_mouse_down -> browser_window_mouse_click ->
	 *   html_mouse_action -> get_mouse_action_node -> link_box_for_ancestor ->
	 *   box_for_node -> illegal instruction (PC in zeroed System heap).
	 *
	 * box_for_node() returns a raw `struct box *` stashed on the DOM node as
	 * user data. Only html_reconvert_clear_node_boxes clears it, and it walks
	 * children/next ONLY -- so a box reachable solely via list_marker or
	 * float_children/next_float keeps its node pointing at a box the reconvert
	 * then frees. The next click resolves that node and dereferences freed
	 * memory.
	 *
	 * This test does exactly what the click path does: resolve every element's
	 * box and READ it. Under ASan a dangling backlink is a heap-use-after-free
	 * at the read, with a clean free stack pointing at the reconvert.
	 *
	 * It needs the fixture to actually CONTAIN markers and floats -- see
	 * build_large_doc; it did not until fixes889, which is why this never
	 * reproduced here. */
	fprintf(stderr, "\n=== Test 26: box_for_node after reconvert (the click "
			"crash) ===\n");
	{
		extern struct box *box_for_node(dom_node *n);
		dom_nodelist *all = NULL;
		dom_string *star = NULL;
		uint32_t len = 0, i2;
		unsigned long touched = 0, live = 0;

		if (dom_string_create((const uint8_t *)"*", 1, &star) != DOM_NO_ERR) {
			fprintf(stderr, "FAIL: dom_string_create('*')\n"); return 1;
		}
		if (dom_document_get_elements_by_tag_name(document, star, &all)
				!= DOM_NO_ERR || all == NULL) {
			fprintf(stderr, "FAIL: get_elements_by_tag_name('*')\n");
			dom_string_unref(star); return 1;
		}
		dom_nodelist_get_length(all, &len);
		fprintf(stderr, "sweeping %lu elements through box_for_node()\n",
				(unsigned long) len);

		for (i2 = 0; i2 < len; i2++) {
			dom_node *nd = NULL;
			struct box *bx;
			if (dom_nodelist_item(all, i2, &nd) != DOM_NO_ERR || nd == NULL)
				continue;
			bx = box_for_node(nd);
			touched++;
			if (bx != NULL) {
				/* THE DEREF. If this box was freed by the reconvert, ASan
				 * traps here with the free stack in html_reconvert_free_old
				 * -- which is the hardware's illegal instruction, caught. */
				volatile int probe = (int) bx->type;
				volatile int probe2 = (int) bx->width;
				(void) probe; (void) probe2;
				live++;
			}
			dom_node_unref(nd);
		}
		dom_nodelist_unref(all);
		dom_string_unref(star);

		fprintf(stderr, "resolved %lu nodes, %lu returned a box, all readable\n",
				touched, live);
		if (touched == 0) {
			fprintf(stderr, "FAIL: swept nothing -- the test proves nothing\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 26 PASS: every post-reconvert box_for_node result "
			"is live memory ===\n");

	/* --- Test 27 (fixes890): scrollbar_get_offset must survive a WILD pointer.
	 *
	 * HW: macos9_poll_mouse_hover -> browser_window_mouse_track ->
	 *     html_mouse_action -> get_mouse_action_node -> box_at_point ->
	 *     scrollbar_get_offset -> `return s->offset;`  s = 0xEC7D8381
	 * Not NULL (so the old guard passed it through), not a poison fill (so the
	 * block had been reused) -- a genuinely wild pointer, dereferenced.
	 *
	 * A magic word inside the struct cannot defend this: reading s->magic
	 * faults on 0xEC7D8381 exactly like s->offset does. Only a membership test
	 * against a set we own can decide validity WITHOUT dereferencing, which is
	 * why fixes890 uses a live registry.
	 *
	 * This feeds the accessors the real crash value and requires them to
	 * survive. Verified RED against the pre-fix tree: ASan reports SEGV on an
	 * unknown address inside scrollbar_get_offset. */
	fprintf(stderr, "\n=== Test 27: scrollbar accessors survive a wild pointer "
			"(the hover crash) ===\n");
	{
		extern int scrollbar_get_offset(struct scrollbar *s);
		extern void *scrollbar_get_data(struct scrollbar *s);
		extern void scrollbar_destroy(struct scrollbar *s);
		/* the exact value off the G4 debugger */
		struct scrollbar *wild = (struct scrollbar *) (size_t) 0xEC7D8381UL;
		struct scrollbar *real = NULL;
		int off;
		void *data;

		if (scrollbar_get_offset(NULL) != 0) {
			fprintf(stderr, "FAIL: NULL scrollbar must read 0\n"); return 1;
		}

		off = scrollbar_get_offset(wild);
		if (off != 0) {
			fprintf(stderr, "FAIL: wild scrollbar returned %d, expected 0\n", off);
			return 1;
		}
		data = scrollbar_get_data(wild);
		if (data != NULL) {
			fprintf(stderr, "FAIL: wild scrollbar returned client_data %p -- "
					"the caller would free() that\n", data);
			return 1;
		}
		scrollbar_destroy(wild);   /* must not free a pointer we never made */

		/* CONTROL: a real scrollbar must still work, or the guard is just
		 * breaking scrollbars rather than protecting them. */
		if (scrollbar_create(true, 100, 400, 100, NULL, NULL, &real)
				!= NSERROR_OK || real == NULL) {
			fprintf(stderr, "FAIL: could not create a real scrollbar\n");
			return 1;
		}
		if (scrollbar_get_offset(real) != 0) {
			fprintf(stderr, "FAIL: a REAL scrollbar was rejected by the "
					"registry -- the guard is over-rejecting\n");
			return 1;
		}
		scrollbar_destroy(real);
		/* after a legitimate destroy it must be treated as stale, not live */
		if (scrollbar_get_offset(real) != 0) {
			fprintf(stderr, "FAIL: a destroyed scrollbar is still considered "
					"live\n");
			return 1;
		}
		fprintf(stderr, "wild pointer -> 0 / NULL / no-op; real scrollbar works; "
				"destroyed scrollbar reads stale\n");
	}
	fprintf(stderr, "=== Test 27 PASS: a stale scrollbar can no longer take the "
			"machine down ===\n");

	/* --- Test 28 (fixes891): a hover during reconvert must not walk a freed
	 * box tree. THE hover crash, reproduced.
	 *
	 * HW: macos9_poll_mouse_hover -> ... -> get_mouse_action_node ->
	 *     box_at_point + link_box_for_ancestor -> box_for_node ->
	 *     dispatch through a freed dom_node's vtable -> PC in zeroed heap.
	 * Reconvert frees the old box tree on one poll pass; hover runs on another;
	 * there was no guard, so a hover mid-reconvert walked freed boxes. The same
	 * dead box gave up a stale scroll_x (fixes890) and a stale node (this).
	 *
	 * Reproduced by putting the content in the exact unsafe state -- a
	 * conversion in flight -- and hovering. get_mouse_action_node must refuse
	 * to touch the tree (NEED_DATA), not walk it.
	 *
	 * NEGATIVE CONTROL: with the guard reverted this SEGVs inside
	 * get_mouse_action_node dereferencing html->layout, exactly as hardware. */
	fprintf(stderr, "\n=== Test 28: hover during reconvert does not walk a torn "
			"tree (the box_for_node crash) ===\n");
	{
		extern nserror html_mouse_action(struct content *c,
				struct browser_window *bw,
				browser_mouse_state mouse, int x, int y);
		nserror r;

		/* State 1: conversion in flight (dom_to_box building). c->layout may
		 * be a partial subtree; the hardware walked it. */
		htmlc.box_conversion_context = (void *) 0x1;   /* non-NULL sentinel */
		r = html_mouse_action((struct content *) &htmlc, NULL,
				BROWSER_MOUSE_HOVER, 10, 10);
		if (r != NSERROR_NEED_DATA) {
			fprintf(stderr, "FAIL: hover during in-flight reconvert returned "
					"%d, expected NEED_DATA (it walked the tree)\n", (int) r);
			return 1;
		}
		htmlc.box_conversion_context = NULL;

		/* State 2: layout torn down (html_reconvert set it NULL). Without the
		 * guard this is `html->layout->node` on NULL. */
		{
			struct box *saved = htmlc.layout;
			htmlc.layout = NULL;
			r = html_mouse_action((struct content *) &htmlc, NULL,
					BROWSER_MOUSE_HOVER, 10, 10);
			if (r != NSERROR_NEED_DATA) {
				fprintf(stderr, "FAIL: hover with NULL layout returned %d, "
						"expected NEED_DATA\n", (int) r);
				return 1;
			}
			htmlc.layout = saved;
		}

		/* State 3: mid-reformat. */
		htmlc.reflowing = true;
		r = html_mouse_action((struct content *) &htmlc, NULL,
				BROWSER_MOUSE_HOVER, 10, 10);
		if (r != NSERROR_NEED_DATA) {
			fprintf(stderr, "FAIL: hover while reflowing returned %d, "
					"expected NEED_DATA\n", (int) r);
			return 1;
		}
		htmlc.reflowing = false;

		fprintf(stderr, "hover refused to walk the tree in all three unsafe "
				"states (in-flight / NULL layout / reflowing)\n");
	}
	fprintf(stderr, "=== Test 28 PASS: the mouse path will not walk a tree a "
			"reconvert is rebuilding ===\n");

	/* --- Test 29 (fixes895): reconvert over a JS-CREATED subtree, the shape the
	 * static fixture never had.
	 *
	 * build_large_doc's lists/floats/tables (fixes889/890) all exist at PARSE
	 * time. Preact/jQuery build the mount at RUN time: createElement +
	 * appendChild grows a NEW subtree that itself carries markers (<li>), floats,
	 * <img> objects, and a form control; innerHTML= parses a fragment into it;
	 * removeChild drops
	 * existing text nodes toward refcount 0 -- all BEFORE the reconvert. That is
	 * the hackaday shape the reproduced HW crash fires on (log dies inside
	 * dom_to_box, between rc=0 and DONE), and it is exactly what Tests 1/2/26
	 * never constructed. This runs that shape through the FULL reconvert cycle
	 * (clear_node_boxes -> pin -> double-buffer -> dom_to_box -> done ->
	 * reformat) and then sweeps box_for_node + every text node, so any dangling
	 * box/dom_node/dom_string traps under ASan with the reconvert on the stack.
	 *
	 * NEGATIVE CONTROL: the detection power of the box_for_node sweep below is
	 * the SAME instrument Test 26 uses, which its author verified RED against the
	 * pre-fixes889 tree; a Test-29-specific pin/double-buffer revert needs a
	 * build toggle in the shipped html.c, deferred to keep this round's shipped
	 * diff instrumentation-only. If the HW log localizes the crash to a phase,
	 * the next round adds the matching RED control here. */
	fprintf(stderr, "\n=== Test 29: reconvert over a JS-created subtree "
			"(the hackaday shape) ===\n");
	{
		extern struct box *box_for_node(dom_node *n);
		const char *mutate29 =
			"var feed=document.getElementById('feed');"
			"var i;"
			"var ul=document.createElement('ul');"
			"for(i=0;i<10;i++){var li=document.createElement('li');"
			"li.textContent='new item '+i;ul.appendChild(li);}"
			"feed.appendChild(ul);"
			"var fl=document.createElement('div');"
			"fl.setAttribute('style','float:left');"
			"fl.textContent='new float';feed.appendChild(fl);"
			"var im=document.createElement('img');"
			"im.setAttribute('src','x.png');feed.appendChild(im);"
			"var m=document.createElement('div');"
			"m.setAttribute('class','comment-form__verbum');"
			"feed.appendChild(m);"
			"m.innerHTML='<span>reply</span><ul><li>a</li><li>b</li></ul>';"
			"var tf=document.createElement('form');tf.id='t29-form';"
			"var ta=document.createElement('textarea');ta.id='t29-ta';"
			"ta.textContent='painted after JS mutation';tf.appendChild(ta);"
			"feed.appendChild(tf);"
			"var p0=document.getElementById('p0');if(p0){feed.removeChild(p0);}"
			"var p1=document.getElementById('p1');if(p1){feed.removeChild(p1);}"
			"var p2=document.getElementById('p2');"
			"if(p2){p2.setAttribute('data-xf-init','1');"
			"p2.textContent='churned';}";
		unsigned char ok29;
		int rc29;
		dom_nodelist *all29 = NULL;
		dom_string *star29 = NULL;
		dom_string *tid29 = NULL;
		uint32_t len29 = 0, k29;
		unsigned long touched29 = 0, live29 = 0;
		dom_node *root29 = NULL;
		dom_element *ta29 = NULL;
		struct box *tab29 = NULL;

		ok29 = js_exec(thread, (const unsigned char *)mutate29,
				strlen(mutate29), "driver-mutate29.js");
		fprintf(stderr, "js_exec(mutate29) ok=%d\n", (int)ok29);
		harness_pump_all(100000);

		htmlc.base.status = CONTENT_STATUS_DONE;
		htmlc.reflowing = false;
		htmlc.box_conversion_context = NULL;
		htmlc.aborted = false;
		htmlc.base.active = 0;

		rc29 = html_reconvert_content((struct content *)&htmlc);
		fprintf(stderr, "html_reconvert_content(t29) rc=%d (0=queued)\n", rc29);
		if (rc29 != 0) {
			fprintf(stderr, "FAIL: t29 reconvert did not queue\n");
			return 1;
		}
		harness_pump_all(100000);
		fprintf(stderr, "t29 reconvert drained, layout=%p\n",
				(void *)htmlc.layout);

		/* The dynamic textarea is the form-registry regression: a control
		 * added by JS must acquire a real box/gadget in the replacement tree,
		 * not merely leave the previous tree visible on rollback. */
		if (dom_string_create((const uint8_t *)"t29-ta", 6, &tid29) !=
				DOM_NO_ERR || tid29 == NULL ||
			dom_document_get_element_by_id(document, tid29, &ta29) !=
				DOM_NO_ERR || ta29 == NULL) {
			fprintf(stderr, "FAIL: t29 dynamic textarea missing\n");
			if (tid29 != NULL) dom_string_unref(tid29);
			return 1;
		}
		tab29 = box_for_node((dom_node *)ta29);
		if (tab29 == NULL || tab29->gadget == NULL) {
			fprintf(stderr, "FAIL: t29 dynamic textarea did not paint\n");
			dom_node_unref((dom_node *)ta29);
			dom_string_unref(tid29);
			return 1;
		}
		dom_node_unref((dom_node *)ta29);
		dom_string_unref(tid29);

		/* sweep box_for_node over the rebuilt tree (the click-crash instrument) */
		if (dom_string_create((const uint8_t *)"*", 1, &star29) != DOM_NO_ERR) {
			fprintf(stderr, "FAIL: t29 dom_string_create('*')\n"); return 1;
		}
		if (dom_document_get_elements_by_tag_name(document, star29, &all29)
				!= DOM_NO_ERR || all29 == NULL) {
			fprintf(stderr, "FAIL: t29 get_elements_by_tag_name\n");
			dom_string_unref(star29); return 1;
		}
		dom_nodelist_get_length(all29, &len29);
		for (k29 = 0; k29 < len29; k29++) {
			dom_node *nd = NULL;
			struct box *bx;
			if (dom_nodelist_item(all29, k29, &nd) != DOM_NO_ERR || nd == NULL)
				continue;
			bx = box_for_node(nd);
			touched29++;
			if (bx != NULL) {
				volatile int probe = (int) bx->type;
				(void) probe;
				live29++;
			}
			dom_node_unref(nd);
		}
		dom_nodelist_unref(all29);
		dom_string_unref(star29);

		/* read every text node exactly like box_construct_text does */
		dom_document_get_document_element(document, (void *)&root29);
		g_text_nodes_read = 0;
		walk_read_text(root29);
		if (root29 != NULL) dom_node_unref(root29);

		fprintf(stderr, "t29 swept %lu elements (%lu boxed), read %d text nodes\n",
				touched29, live29, g_text_nodes_read);
		if (touched29 == 0) {
			fprintf(stderr, "FAIL: t29 swept nothing -- proves nothing\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 29 PASS: reconvert over a JS-created subtree "
		"(markers/floats/img/textarea/innerHTML/removeChild) is ASan-clean ===\n");

	/* --- Test 30 (fixes900): closing an IFRAME heap must NOT free the PARENT
	 * runtime's DOM wrappers. THE crash the whole reconvert hunt actually kept
	 * hitting (crash B): the durable ReconvPos marker on hardware said the
	 * bomb was in qjs_flush_timers/realm teardown, not the box build. Root
	 * cause: qjs_wrap_drain walked the GLOBAL, file-static wrap map (shared by
	 * every runtime) and, on ANY js_destroyheap, unref'd + freed EVERY entry --
	 * so tearing down an iframe freed the parent document's node + keepalive
	 * refs out from under its still-live runtime -> next parent DOM/JS op calls
	 * through freed memory -> PC=0 / js_shape_hash_unlink. On hardware the
	 * trigger is the reconvert rebuilding hackaday's iframe (~box node 60-80).
	 *
	 * Repro: parent runtime holds an ORPHAN element whose ONLY ref is its
	 * wrapper; a second (iframe) heap wraps its own nodes; destroy the iframe;
	 * then the parent natively derefs its orphan. Pre-fix the orphan's node was
	 * freed by the iframe drain -> ASan heap-use-after-free at appendChild.
	 * VERIFIED RED against the unconditional-drain code, GREEN with the
	 * per-runtime drain (fixes900). */
	fprintf(stderr, "\n=== Test 30: iframe heap-destroy must not free the "
			"parent runtime's wrappers (crash B) ===\n");
	{
		struct jsheap *ih = NULL;
		struct jsthread *it = NULL;
		const char *parent_wrap =
			"globalThis.__orphan = document.createElement('div');"
			"var b = document.body; var d = document.documentElement;";
		const char *iframe_wrap = "var x = document.body;";
		/* NATIVE deref of the parent orphan; pre-fix its node is freed. */
		const char *parent_use =
			"globalThis.__orphan.appendChild("
				"document.createElement('span'));"
			"if(globalThis.__orphan.nodeType!==1)"
				"throw new Error('ASSERT FAIL: parent wrapper lost');";
		unsigned char ok;

		ok = js_exec(thread, (const unsigned char *)parent_wrap,
				strlen(parent_wrap), "t30-parent-wrap.js");
		if (!ok) { fprintf(stderr, "FAIL: t30 parent wrap\n"); return 1; }

		nerr = js_newheap(20000, &ih);
		if (nerr != NSERROR_OK) {
			fprintf(stderr, "FAIL: t30 iframe heap nerr=%d\n", (int)nerr);
			return 1;
		}
		nerr = js_newthread(ih, NULL, (void *)&htmlc, &it);
		if (nerr != NSERROR_OK) {
			fprintf(stderr, "FAIL: t30 iframe thread nerr=%d\n", (int)nerr);
			return 1;
		}
		ok = js_exec(it, (const unsigned char *)iframe_wrap,
				strlen(iframe_wrap), "t30-iframe-wrap.js");
		(void) ok;

		/* Close the iframe: drains ONLY its own runtime's wrappers now. */
		js_destroyheap(ih);

		/* Parent must still natively deref its orphan wrapper. */
		ok = js_exec(thread, (const unsigned char *)parent_use,
				strlen(parent_use), "t30-parent-use.js");
		if (!ok) {
			fprintf(stderr, "FAIL: t30 parent wrapper freed by iframe "
					"teardown (crash B)\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 30 PASS: parent wrappers survive an iframe "
			"heap-destroy (crash B closed) ===\n");

	free(html_src_big);

	/* --- Test 31 (fixes921): the reconvert object RE-LINK ---------------------
	 *
	 * Drives a REAL reconvert with fabricated entries on object_list, so the
	 * re-link runs in situ (it is static in html.c -- deliberately, after two
	 * link failures on new cross-TU symbols).
	 *
	 * Covers what fixes921 newly wrote, none of which needs a completed fetch:
	 * the partition walk, box re-resolution via box_for_node, image-only
	 * scoping, retirement through html_object_free_objects, and num_objects
	 * (never decremented anywhere before this).
	 *
	 * The MIXED entry set is what makes it discriminating, so no kill-switch
	 * toggle is needed: a pass that did nothing would leave the non-image and
	 * node-gone entries alive; a pass that retired everything would lose the
	 * image. Only correct behaviour satisfies both halves.
	 *
	 * NOT covered: the armed-callback disarm inside the teardown. Measured, not
	 * assumed -- a spike showed html_fetch_object SEGVs in llcache_handle_get_url
	 * because the harness never initialises the fetch stack, so an armed
	 * callback is unreachable here. That code is unchanged from what already
	 * ships; hardware covers it. */
	fprintf(stderr, "\n=== Test 31: reconvert object re-link (fixes921) ===\n");
	{
		extern struct box *box_for_node(dom_node *n);
		dom_nodelist *ps = NULL;
		dom_string *ptag = NULL;
		uint32_t plen = 0;
		dom_node *keep_node = NULL, *gone_node = NULL;
		struct box *old_box = NULL;
		struct content_html_object *img, *gone, *notimg;
		static struct box gone_box;   /* static: must outlive the reconvert */
		int rc;

		if (dom_string_create((const uint8_t *)"p", 1, &ptag) != DOM_NO_ERR ||
		    dom_document_get_elements_by_tag_name(document, ptag, &ps)
				!= DOM_NO_ERR || ps == NULL) {
			fprintf(stderr, "FAIL: Test 31 could not list <p>\n"); return 1;
		}
		dom_nodelist_get_length(ps, &plen);
		if (plen < 1) { fprintf(stderr, "FAIL: Test 31 needs a <p>\n"); return 1; }
		dom_nodelist_item(ps, 0, &keep_node);
		old_box = keep_node ? box_for_node(keep_node) : NULL;
		if (old_box == NULL) {
			fprintf(stderr, "FAIL: Test 31 <p> has no box\n"); return 1;
		}
		/* detached element: never in the tree, so box_for_node -> NULL */
		if (dom_document_create_element(document, ptag,
				(dom_element **)&gone_node) != DOM_NO_ERR ||
				gone_node == NULL) {
			fprintf(stderr, "FAIL: Test 31 detached node\n"); return 1;
		}
		memset(&gone_box, 0, sizeof(gone_box));
		gone_box.node = gone_node;

		img    = calloc(1, sizeof(*img));
		gone   = calloc(1, sizeof(*gone));
		notimg = calloc(1, sizeof(*notimg));
		if (img == NULL || gone == NULL || notimg == NULL) {
			fprintf(stderr, "FAIL: Test 31 calloc\n"); return 1;
		}
		img->parent    = (struct content *)&htmlc;
		img->box = old_box;    img->permitted_types = CONTENT_IMAGE;
		gone->parent   = (struct content *)&htmlc;
		gone->box = &gone_box; gone->permitted_types = CONTENT_IMAGE;
		notimg->parent = (struct content *)&htmlc;
		notimg->box = old_box; notimg->permitted_types = CONTENT_ANY;

		/* order so a retire lands at head, middle and tail of the walk */
		gone->next = img; img->next = notimg; notimg->next = NULL;
		htmlc.object_list = gone;
		htmlc.num_objects = 3;

		htmlc.box_conversion_context = NULL;
		htmlc.aborted = false;
		htmlc.base.active = 0;
		rc = html_reconvert_content((struct content *)&htmlc);
		if (rc != 0) {
			fprintf(stderr, "FAIL: Test 31 reconvert did not queue (rc=%d)\n", rc);
			return 1;
		}
		harness_pump_all(100000);

		if (htmlc.object_list == NULL || htmlc.object_list->next != NULL) {
			fprintf(stderr, "FAIL: Test 31 expected exactly ONE surviving "
				"entry, got %s\n",
				htmlc.object_list ? "more than one" : "none");
			return 1;
		}
		if (htmlc.object_list->permitted_types != CONTENT_IMAGE) {
			fprintf(stderr, "FAIL: Test 31 survivor is not the image entry\n");
			return 1;
		}
		if (htmlc.object_list->box == NULL) {
			fprintf(stderr, "FAIL: Test 31 survivor lost its box\n");
			return 1;
		}
		if (htmlc.object_list->box != box_for_node(keep_node)) {
			fprintf(stderr, "FAIL: Test 31 survivor box not re-resolved to "
				"the rebuilt tree\n");
			return 1;
		}
		if (htmlc.num_objects != 1) {
			fprintf(stderr, "FAIL: Test 31 num_objects=%u expected 1\n",
					htmlc.num_objects);
			return 1;
		}
		fprintf(stderr, "  image kept + re-resolved; node-gone and non-image "
			"retired; num_objects 3->%u\n", htmlc.num_objects);

		while (htmlc.object_list != NULL) {
			struct content_html_object *nx = htmlc.object_list->next;
			free(htmlc.object_list);
			htmlc.object_list = nx;
		}
		htmlc.num_objects = 0;
		dom_node_unref(gone_node);
		dom_nodelist_unref(ps);
		dom_string_unref(ptag);
	}
	fprintf(stderr, "=== Test 31 PASS: reconvert keeps and re-resolves image "
		"objects, retires node-gone and non-image entries, and fixes the "
		"num_objects leak ===\n");

	/* --- Test 32 (fixes929): an image knows its size WITHOUT its object ----
	 *
	 * The squish: content_get_width(box->object) was the engine's only source
	 * of an image's intrinsic size, so the moment box->object was NULL the
	 * <img> stopped being a replaced element (lh__box_is_object does not test
	 * IS_REPLACED) and layout sized it as inline text -- line-height tall,
	 * alt-string wide. box->object is NULL far more often than it looks:
	 * every lazy image before its first paint, every object a reconvert
	 * retires, and every image on a revisit after hlcache_clean reaped it.
	 *
	 * Test the two halves that make the size survive: the URL->size memo, and
	 * the box-level predicate that decides "replaced element or inline text".
	 * The three PASSES here are the exact states that produced the squish.
	 */
	fprintf(stderr, "\n=== Test 32: image size survives a NULL object (fixes929) ===\n");
	{
		nsurl *ua = NULL, *ub = NULL;
		int w = 0, h = 0;
		struct box tb;

		if (nsurl_create("http://example.org/photo.png", &ua) != NSERROR_OK ||
		    nsurl_create("http://example.org/other.png", &ub) != NSERROR_OK) {
			fprintf(stderr, "FAIL: Test 32 nsurl_create\n"); return 1;
		}

		/* An unknown URL must MISS -- otherwise a hit proves nothing. */
		if (macsurf_imgdims_lookup(ua, &w, &h) != 0) {
			fprintf(stderr, "FAIL: Test 32 unknown URL reported a size\n");
			return 1;
		}

		macsurf_imgdims_remember(ua, 640, 480);

		if (macsurf_imgdims_lookup(ua, &w, &h) == 0 || w != 640 || h != 480) {
			fprintf(stderr, "FAIL: Test 32 memo lost the size (got %dx%d)\n",
					w, h);
			return 1;
		}
		/* A DIFFERENT url must not inherit it -- a memo that answers every
		 * query would "pass" the line above while sizing every image wrong. */
		w = h = 0;
		if (macsurf_imgdims_lookup(ub, &w, &h) != 0) {
			fprintf(stderr, "FAIL: Test 32 memo answered for a foreign URL "
					"(%dx%d)\n", w, h);
			return 1;
		}
		fprintf(stderr, "  memo: miss -> remember -> hit 640x480, no cross-URL bleed\n");

		/* The predicate layout branches on. With no object and no
		 * REPLACE_DIM this returned false, which is what sent the <img>
		 * down the inline-text path and produced the squish. */
		memset(&tb, 0, sizeof(tb));
		tb.type = BOX_INLINE;
		if (lh__box_is_object(&tb) != false) {
			fprintf(stderr, "FAIL: Test 32 sizeless box claims to be an object\n");
			return 1;
		}
		tb.obj_w = 640;
		tb.obj_h = 480;
		if (lh__box_is_object(&tb) == false) {
			fprintf(stderr, "FAIL: Test 32 box with a known intrinsic size is "
					"NOT treated as replaced -- this is the squish\n");
			return 1;
		}
		if (lh__box_intrinsic_w(&tb) != 640 ||
		    lh__box_intrinsic_h(&tb) != 480) {
			fprintf(stderr, "FAIL: Test 32 intrinsic accessors gave %dx%d\n",
					lh__box_intrinsic_w(&tb),
					lh__box_intrinsic_h(&tb));
			return 1;
		}
		fprintf(stderr, "  box: object=NULL + obj_w/h -> replaced, 640x480\n");

		nsurl_unref(ua);
		nsurl_unref(ub);
	}
	fprintf(stderr, "=== Test 32 PASS: a known image size survives a NULL "
		"object, so lazy/retired/revisited images lay out correctly ===\n");

	/* --- Test 33 (lifecycle Stage 1): a BACKGROUND object survives reconvert
	 *
	 * The disappear bug's largest class: html_reconvert_relink_objects retired
	 * every o->background unconditionally (background=70 of 101 on hackaday),
	 * so a CSS background image was destroyed on every DOM mutation. The fix
	 * relinks it to the rebuilt box instead of retiring it.
	 *
	 * Modelled on Test 31 but with a single background entry whose box is a
	 * live node in the tree. RED before the fix (the entry is retired ->
	 * object_list empty); GREEN after (it survives and re-resolves its box).
	 * Content-pointer wiring needs a real fetch and is hardware-covered, same
	 * scope caveat Test 31 records; this asserts survival + box re-resolution,
	 * which is what the relink itself is responsible for. */
	fprintf(stderr, "\n=== Test 33: background object survives reconvert "
		"(lifecycle Stage 1) ===\n");
	{
		extern struct box *box_for_node(dom_node *n);
		dom_nodelist *ps = NULL;
		dom_string *ptag = NULL;
		uint32_t plen = 0;
		dom_node *keep_node = NULL;
		struct box *old_box = NULL;
		struct content_html_object *bg;
		int rc;

		if (dom_string_create((const uint8_t *)"p", 1, &ptag) != DOM_NO_ERR ||
		    dom_document_get_elements_by_tag_name(document, ptag, &ps)
				!= DOM_NO_ERR || ps == NULL) {
			fprintf(stderr, "FAIL: Test 33 could not list <p>\n"); return 1;
		}
		dom_nodelist_get_length(ps, &plen);
		if (plen < 1) { fprintf(stderr, "FAIL: Test 33 needs a <p>\n"); return 1; }
		dom_nodelist_item(ps, 0, &keep_node);
		old_box = keep_node ? box_for_node(keep_node) : NULL;
		if (old_box == NULL) {
			fprintf(stderr, "FAIL: Test 33 <p> has no box\n"); return 1;
		}

		bg = calloc(1, sizeof(*bg));
		if (bg == NULL) { fprintf(stderr, "FAIL: Test 33 calloc\n"); return 1; }
		bg->parent = (struct content *)&htmlc;
		bg->box = old_box;
		bg->permitted_types = CONTENT_IMAGE;
		bg->background = true;   /* the class that was unconditionally retired */
		bg->next = NULL;
		htmlc.object_list = bg;
		htmlc.num_objects = 1;

		htmlc.box_conversion_context = NULL;
		htmlc.aborted = false;
		htmlc.base.active = 0;
		rc = html_reconvert_content((struct content *)&htmlc);
		if (rc != 0) {
			fprintf(stderr, "FAIL: Test 33 reconvert did not queue (rc=%d)\n", rc);
			return 1;
		}
		harness_pump_all(100000);

		if (htmlc.object_list == NULL) {
			fprintf(stderr, "FAIL: Test 33 background object was RETIRED "
				"(the disappear bug) -- expected it to survive\n");
			return 1;
		}
		if (htmlc.object_list->next != NULL) {
			fprintf(stderr, "FAIL: Test 33 expected exactly one survivor\n");
			return 1;
		}
		if (htmlc.object_list->background != true) {
			fprintf(stderr, "FAIL: Test 33 survivor lost its background flag\n");
			return 1;
		}
		if (htmlc.object_list->box == NULL) {
			fprintf(stderr, "FAIL: Test 33 survivor lost its box\n");
			return 1;
		}
		if (htmlc.object_list->box != box_for_node(keep_node)) {
			fprintf(stderr, "FAIL: Test 33 survivor box not re-resolved to "
				"the rebuilt tree\n");
			return 1;
		}
		if (htmlc.num_objects != 1) {
			fprintf(stderr, "FAIL: Test 33 num_objects=%u expected 1\n",
					htmlc.num_objects);
			return 1;
		}
		fprintf(stderr, "  background kept + re-resolved; num_objects stayed 1\n");

		while (htmlc.object_list != NULL) {
			struct content_html_object *nx = htmlc.object_list->next;
			free(htmlc.object_list);
			htmlc.object_list = nx;
		}
		htmlc.num_objects = 0;
		dom_nodelist_unref(ps);
		dom_string_unref(ptag);
	}
	fprintf(stderr, "=== Test 33 PASS: a CSS background image survives a "
		"reconvert instead of being retired ===\n");

	/* --- Test 34 (lifecycle Stage 1): reconvert DEDUPES duplicate objects
	 *
	 * Keeping objects instead of retiring them would leak without this: a
	 * reconvert re-runs box construction, which prepends a fresh object for
	 * every image already on the list. Two objects for the same (box,
	 * background) display slot must collapse to one, or the list grows every
	 * reconvert. Simulates that by putting TWO foreground image objects on the
	 * same box (the "new" one at the head, the "old" at the tail) plus one
	 * background pair, and asserts each slot survives exactly once. */
	fprintf(stderr, "\n=== Test 34: reconvert dedupes duplicate objects "
		"(lifecycle Stage 1) ===\n");
	{
		extern struct box *box_for_node(dom_node *n);
		dom_nodelist *ps = NULL;
		dom_string *ptag = NULL;
		uint32_t plen = 0;
		dom_node *keep_node = NULL;
		struct box *old_box = NULL;
		struct content_html_object *fg_new, *fg_old, *bg_new, *bg_old;
		int fg = 0, bg = 0;
		struct content_html_object *w;

		if (dom_string_create((const uint8_t *)"p", 1, &ptag) != DOM_NO_ERR ||
		    dom_document_get_elements_by_tag_name(document, ptag, &ps)
				!= DOM_NO_ERR || ps == NULL) {
			fprintf(stderr, "FAIL: Test 34 could not list <p>\n"); return 1;
		}
		dom_nodelist_get_length(ps, &plen);
		if (plen < 1) { fprintf(stderr, "FAIL: Test 34 needs a <p>\n"); return 1; }
		dom_nodelist_item(ps, 0, &keep_node);
		old_box = keep_node ? box_for_node(keep_node) : NULL;
		if (old_box == NULL) {
			fprintf(stderr, "FAIL: Test 34 <p> has no box\n"); return 1;
		}

		fg_new = calloc(1, sizeof(*fg_new));
		fg_old = calloc(1, sizeof(*fg_old));
		bg_new = calloc(1, sizeof(*bg_new));
		bg_old = calloc(1, sizeof(*bg_old));
		if (!fg_new || !fg_old || !bg_new || !bg_old) {
			fprintf(stderr, "FAIL: Test 34 calloc\n"); return 1;
		}
		fg_new->parent = fg_old->parent = (struct content *)&htmlc;
		bg_new->parent = bg_old->parent = (struct content *)&htmlc;
		fg_new->box = fg_old->box = old_box;
		bg_new->box = bg_old->box = old_box;
		fg_new->permitted_types = fg_old->permitted_types = CONTENT_IMAGE;
		bg_new->permitted_types = bg_old->permitted_types = CONTENT_IMAGE;
		bg_new->background = bg_old->background = true;
		/* head..tail: new fg, new bg, old fg, old bg (new prepended) */
		fg_new->next = bg_new; bg_new->next = fg_old;
		fg_old->next = bg_old; bg_old->next = NULL;
		htmlc.object_list = fg_new;
		htmlc.num_objects = 4;

		htmlc.box_conversion_context = NULL;
		htmlc.aborted = false;
		htmlc.base.active = 0;
		if (html_reconvert_content((struct content *)&htmlc) != 0) {
			fprintf(stderr, "FAIL: Test 34 reconvert did not queue\n"); return 1;
		}
		harness_pump_all(100000);

		for (w = htmlc.object_list; w != NULL; w = w->next) {
			if (w->background) bg++; else fg++;
		}
		if (fg != 1 || bg != 1) {
			fprintf(stderr, "FAIL: Test 34 expected 1 fg + 1 bg survivor, "
				"got fg=%d bg=%d (dedupe failed -> leak)\n", fg, bg);
			return 1;
		}
		if (htmlc.num_objects != 2) {
			fprintf(stderr, "FAIL: Test 34 num_objects=%u expected 2\n",
					htmlc.num_objects);
			return 1;
		}
		fprintf(stderr, "  4 objects (2 duplicated slots) -> 2 survivors\n");

		while (htmlc.object_list != NULL) {
			struct content_html_object *nx = htmlc.object_list->next;
			free(htmlc.object_list);
			htmlc.object_list = nx;
		}
		htmlc.num_objects = 0;
		dom_nodelist_unref(ps);
		dom_string_unref(ptag);
	}
	fprintf(stderr, "=== Test 34 PASS: duplicate objects for one display slot "
		"collapse to one, so keeping objects cannot leak ===\n");

	/* --- Test 35 (lifecycle Stage 1): creation-time URL ADOPTION
	 *
	 * html_fetch_object had no URL dedupe of its own, so the speculative
	 * fetch fired by html_process_inserted_img (box == NULL) and the fetch
	 * fired later by box construction for the SAME <img> were two
	 * independent fetches for one image. The Stage 0b hardware probe
	 * measured it: 22 of 32 speculative URLs were fetched twice, and one
	 * thumbnail five times. fixes975 matches on the URL at creation:
	 *
	 *   another speculative fetch for a held URL  -> do nothing
	 *   a BOX fetch for a URL a box-less entry holds -> adopt that entry
	 *
	 * Both cases return before hlcache_handle_retrieve, which is also what
	 * makes them testable here: the harness never initialises the fetch
	 * stack (see Test 31's note), so a call that does NOT dedupe reaches
	 * llcache and dies. That is the negative control -- a build without the
	 * adoption block does not fail this test politely, it SEGVs in
	 * llcache_handle_get_url. Verified by removing the block and re-running.
	 *
	 * The entries are hand-built, as in Tests 31/33/34, with a zeroed stand-in
	 * hlcache_handle: entry == NULL reads back as "no content yet", i.e.
	 * exactly the in-flight speculative case, so adoption takes its
	 * not-yet-DONE branch and must balance c->base.active for the DONE the
	 * box callback will later consume.
	 *
	 * NOT covered (same scope caveat as Test 31): a second box wanting the
	 * same URL must NOT steal the first box's object. That path deliberately
	 * falls through to a real fetch, which the harness cannot reach. It rests
	 * on the `box != NULL && cand->box != NULL` guard in the match loop. */
	fprintf(stderr, "\n=== Test 35: html_fetch_object adopts instead of "
		"refetching (lifecycle Stage 1) ===\n");
	{
		extern struct box *box_for_node(dom_node *n);
		extern bool html_fetch_object(html_content *c, nsurl *url,
				struct box *box, content_type permitted_types,
				bool background);
		dom_nodelist *ps = NULL;
		dom_string *ptag = NULL;
		uint32_t plen = 0;
		dom_node *keep_node = NULL;
		struct box *bx = NULL;
		struct content_html_object *spec;
		nsurl *u = NULL;
		int active_before;
		int n = 0;
		struct content_html_object *w;

		if (nsurl_create("http://example.org/img/spec.png", &u)
				!= NSERROR_OK) {
			fprintf(stderr, "FAIL: Test 35 nsurl_create\n"); return 1;
		}
		if (dom_string_create((const uint8_t *)"p", 1, &ptag) != DOM_NO_ERR ||
		    dom_document_get_elements_by_tag_name(document, ptag, &ps)
				!= DOM_NO_ERR || ps == NULL) {
			fprintf(stderr, "FAIL: Test 35 could not list <p>\n"); return 1;
		}
		dom_nodelist_get_length(ps, &plen);
		if (plen < 1) { fprintf(stderr, "FAIL: Test 35 needs a <p>\n"); return 1; }
		dom_nodelist_item(ps, 0, &keep_node);
		bx = keep_node ? box_for_node(keep_node) : NULL;
		if (bx == NULL) {
			fprintf(stderr, "FAIL: Test 35 <p> has no box\n"); return 1;
		}

		/* the speculative entry html_process_inserted_img would have made */
		spec = calloc(1, sizeof(*spec));
		if (spec == NULL) { fprintf(stderr, "FAIL: Test 35 calloc\n"); return 1; }
		spec->parent = (struct content *)&htmlc;
		spec->box = NULL;                       /* no box yet -- the point */
		spec->permitted_types = CONTENT_IMAGE;
		spec->background = false;
		spec->url = u;
		nsurl_ref(u);
		/* stand-in for an unresolved retrieval: non-NULL handle, no content */
		spec->content = (struct hlcache_handle *)calloc(1, 256);
		if (spec->content == NULL) {
			fprintf(stderr, "FAIL: Test 35 calloc handle\n"); return 1;
		}
		spec->next = NULL;
		htmlc.object_list = spec;
		htmlc.num_objects = 1;
		htmlc.aborted = false;
		htmlc.base.active = 0;

		/* (a) a second SPECULATIVE fetch for the same URL must do nothing */
		if (html_fetch_object(&htmlc, u, NULL, CONTENT_IMAGE, false)
				== false) {
			fprintf(stderr, "FAIL: Test 35 speculative re-fetch reported "
				"failure\n");
			return 1;
		}
		for (n = 0, w = htmlc.object_list; w != NULL; w = w->next) n++;
		if (n != 1) {
			fprintf(stderr, "FAIL: Test 35 speculative re-fetch created a "
				"duplicate entry (%d entries)\n", n);
			return 1;
		}
		if (htmlc.object_list->box != NULL) {
			fprintf(stderr, "FAIL: Test 35 speculative re-fetch must not "
				"assign a box\n");
			return 1;
		}
		if (htmlc.base.active != 0) {
			fprintf(stderr, "FAIL: Test 35 speculative re-fetch moved "
				"base.active to %d\n", (int)htmlc.base.active);
			return 1;
		}
		fprintf(stderr, "  second speculative fetch: no new entry, no fetch\n");

		/* (b) box construction claims it instead of fetching again */
		active_before = (int)htmlc.base.active;
		if (html_fetch_object(&htmlc, u, bx, CONTENT_IMAGE, false) == false) {
			fprintf(stderr, "FAIL: Test 35 box fetch reported failure\n");
			return 1;
		}
		for (n = 0, w = htmlc.object_list; w != NULL; w = w->next) n++;
		if (n != 1) {
			fprintf(stderr, "FAIL: Test 35 box fetch created a SECOND entry "
				"for a URL already held (%d entries) -- this is the "
				"double fetch\n", n);
			return 1;
		}
		if (htmlc.object_list->box != bx) {
			fprintf(stderr, "FAIL: Test 35 box fetch did not adopt the "
				"speculative entry (box=%p want %p)\n",
				(void *)htmlc.object_list->box, (void *)bx);
			return 1;
		}
		if ((int)htmlc.base.active != active_before + 1) {
			fprintf(stderr, "FAIL: Test 35 adoption left base.active at %d, "
				"expected %d -- the DONE decrement would unbalance it\n",
				(int)htmlc.base.active, active_before + 1);
			return 1;
		}
		fprintf(stderr, "  box fetch: adopted the in-flight entry, "
			"base.active balanced\n");

		free(htmlc.object_list->content);
		nsurl_unref(htmlc.object_list->url);
		free(htmlc.object_list);
		htmlc.object_list = NULL;
		htmlc.num_objects = 0;
		htmlc.base.active = 0;
		nsurl_unref(u);
		dom_nodelist_unref(ps);
		dom_string_unref(ptag);
	}
	fprintf(stderr, "=== Test 35 PASS: a URL the document already holds is "
		"adopted, not fetched a second time ===\n");

	/* --- Test 36 (lifecycle Stage 1): a box-less entry SURVIVES a reconvert
	 *
	 * The fixes975 hardware log showed 87 images constructed, 30 eager and
	 * 57 handed to the lazy viewport queue -- and every speculative URL that
	 * was never adopted was one of the deferred ones. The reconvert retired
	 * those box-less entries (nobox=9), throwing away a fetched image that
	 * the lazy drain was about to ask for again, so scrolling paid for it
	 * twice.
	 *
	 * fixes976 keeps them. That is only safe because fixes975 bounds them:
	 * creation-time dedupe refuses a second speculative fetch for a held
	 * URL, so there is at most one box-less entry per distinct URL and they
	 * cannot accumulate per reconvert.
	 *
	 * Asserts the keep, and the one exclusion: a box-less entry whose handle
	 * is gone (its fetch errored) is dead weight and must still be retired.
	 * The mixed entry set is what makes it discriminating -- a pass that
	 * kept everything would keep the errored one too, and a pass that
	 * retired everything would lose the live one.
	 *
	 * Negative control, measured not assumed: with the keep branch removed
	 * the run dies at hlcache.c:1029 in hlcache_handle_release, because
	 * retirement releases the handle and this fixture's is a stand-in. So
	 * the control fails deterministically, at the exact line reached only by
	 * retiring the entry -- but it fails by crashing, not by assertion, the
	 * same fetch-stack limitation Tests 31 and 35 record. */
	fprintf(stderr, "\n=== Test 36: box-less speculative entry survives "
		"reconvert (lifecycle Stage 1) ===\n");
	{
		struct content_html_object *live, *dead;
		int n_live = 0, n_dead = 0, n = 0;
		struct content_html_object *w;

		live = calloc(1, sizeof(*live));
		dead = calloc(1, sizeof(*dead));
		if (live == NULL || dead == NULL) {
			fprintf(stderr, "FAIL: Test 36 calloc\n"); return 1;
		}
		live->parent = dead->parent = (struct content *)&htmlc;
		live->permitted_types = dead->permitted_types = CONTENT_IMAGE;
		live->box = dead->box = NULL;          /* both still speculative */
		/* live still holds a handle; dead's fetch errored and released it */
		live->content = (struct hlcache_handle *)calloc(1, 256);
		if (live->content == NULL) {
			fprintf(stderr, "FAIL: Test 36 calloc handle\n"); return 1;
		}
		dead->content = NULL;
		live->next = dead; dead->next = NULL;
		htmlc.object_list = live;
		htmlc.num_objects = 2;

		htmlc.box_conversion_context = NULL;
		htmlc.aborted = false;
		htmlc.base.active = 0;
		if (html_reconvert_content((struct content *)&htmlc) != 0) {
			fprintf(stderr, "FAIL: Test 36 reconvert did not queue\n");
			return 1;
		}
		harness_pump_all(100000);

		for (w = htmlc.object_list; w != NULL; w = w->next) {
			n++;
			if (w->content != NULL) n_live++; else n_dead++;
		}
		if (n_live != 1) {
			fprintf(stderr, "FAIL: Test 36 the box-less entry with a live "
				"handle was RETIRED -- the lazy drain will refetch it "
				"(survivors: %d live, %d dead)\n", n_live, n_dead);
			return 1;
		}
		if (n_dead != 0) {
			fprintf(stderr, "FAIL: Test 36 kept a box-less entry whose "
				"fetch had already errored -- it can never be adopted\n");
			return 1;
		}
		if (htmlc.num_objects != 1) {
			fprintf(stderr, "FAIL: Test 36 num_objects=%u expected 1\n",
					htmlc.num_objects);
			return 1;
		}
		fprintf(stderr, "  box-less + live handle kept; box-less + dead "
			"handle retired\n");

		while (htmlc.object_list != NULL) {
			struct content_html_object *nx = htmlc.object_list->next;
			free(htmlc.object_list->content);
			free(htmlc.object_list);
			htmlc.object_list = nx;
		}
		htmlc.num_objects = 0;
	}
	fprintf(stderr, "=== Test 36 PASS: a speculative entry survives the "
		"reconvert, so the lazy drain adopts it instead of refetching ===\n");

	/* --- Test 37 (lifecycle Stage 1): node-keyed adoption across a rebuild
	 *
	 * fixes975 only adopted BOX-LESS entries, so the reconvert case stayed:
	 * construction mints a new object for every background on every
	 * reconvert (122 background fetches, dedup=35 per cycle on hackaday) and
	 * the relink then collapses the duplicate it just made. fixes977 adopts
	 * an entry that already has a box when it is THIS SAME ELEMENT's entry
	 * being rebuilt -- same node, same slot, same URL -- so the second fetch
	 * never starts.
	 *
	 * The two halves that must both hold, and the reason node identity was
	 * chosen over any "does its box look stale" test:
	 *   (a) same node  -> adopt, re-pointing the entry at the rebuilt box;
	 *   (b) DIFFERENT node, same URL -> do NOT adopt. hackaday's sprite is
	 *       the background of 54 boxes; one element must never take
	 *       another's object.
	 *
	 * (b) is asserted the only way the harness can: the non-adopting path
	 * falls through to a fetch stack that does not exist here, so it dies in
	 * llcache. That is the same limitation Tests 31/35/36 record -- and it
	 * is now the third test to pay it, which is the argument for giving the
	 * harness a real fetch stack. Here (b) is checked structurally instead,
	 * by pointing the candidate at a different node and confirming the entry
	 * is NOT re-pointed before the call would reach llcache: not possible,
	 * so (b) rests on the `cand->box->node != want_node` guard being the
	 * only way past the loop. Only (a) is executed. */
	fprintf(stderr, "\n=== Test 37: node-keyed adoption re-points an entry "
		"at its rebuilt box (lifecycle Stage 1) ===\n");
	{
		extern struct box *box_for_node(dom_node *n);
		extern bool html_fetch_object(html_content *c, nsurl *url,
				struct box *box, content_type permitted_types,
				bool background);
		dom_nodelist *ps = NULL;
		dom_string *ptag = NULL;
		uint32_t plen = 0;
		dom_node *keep_node = NULL;
		struct box *old_box = NULL;
		static struct box rebuilt;   /* stands in for the new tree's box */
		struct content_html_object *bg;
		nsurl *u = NULL;
		int n = 0;
		int active_before;
		struct content_html_object *w;

		if (nsurl_create("http://example.org/img/sprite.png", &u)
				!= NSERROR_OK) {
			fprintf(stderr, "FAIL: Test 37 nsurl_create\n"); return 1;
		}
		if (dom_string_create((const uint8_t *)"p", 1, &ptag) != DOM_NO_ERR ||
		    dom_document_get_elements_by_tag_name(document, ptag, &ps)
				!= DOM_NO_ERR || ps == NULL) {
			fprintf(stderr, "FAIL: Test 37 could not list <p>\n"); return 1;
		}
		dom_nodelist_get_length(ps, &plen);
		if (plen < 1) { fprintf(stderr, "FAIL: Test 37 needs a <p>\n"); return 1; }
		dom_nodelist_item(ps, 0, &keep_node);
		old_box = keep_node ? box_for_node(keep_node) : NULL;
		if (old_box == NULL || old_box->node == NULL) {
			fprintf(stderr, "FAIL: Test 37 <p> has no box/node\n"); return 1;
		}

		/* the entry as it stands after the previous convert: linked to a
		 * box in the tree that is about to be replaced */
		bg = calloc(1, sizeof(*bg));
		if (bg == NULL) { fprintf(stderr, "FAIL: Test 37 calloc\n"); return 1; }
		bg->parent = (struct content *)&htmlc;
		bg->box = old_box;
		bg->permitted_types = CONTENT_IMAGE;
		bg->background = true;
		bg->url = u;
		nsurl_ref(u);
		bg->content = (struct hlcache_handle *)calloc(1, 256);
		if (bg->content == NULL) {
			fprintf(stderr, "FAIL: Test 37 calloc handle\n"); return 1;
		}
		bg->next = NULL;
		bg->active_counted = true;
		htmlc.object_list = bg;
		htmlc.num_objects = 1;
		htmlc.aborted = false;
		htmlc.base.active = 1;   /* a boxed entry is already counted */

		/* box construction on the rebuilt tree: same element, same URL */
		memset(&rebuilt, 0, sizeof(rebuilt));
		rebuilt.node = old_box->node;
		active_before = (int)htmlc.base.active;
		if (html_fetch_object(&htmlc, u, &rebuilt, CONTENT_IMAGE, true)
				== false) {
			fprintf(stderr, "FAIL: Test 37 rebuild fetch reported "
				"failure\n");
			return 1;
		}
		for (n = 0, w = htmlc.object_list; w != NULL; w = w->next) n++;
		if (n != 1) {
			fprintf(stderr, "FAIL: Test 37 the rebuild started a SECOND "
				"fetch for the same element (%d entries) -- this is "
				"dedup=35\n", n);
			return 1;
		}
		if (htmlc.object_list->box != &rebuilt) {
			fprintf(stderr, "FAIL: Test 37 entry was not re-pointed at "
				"the rebuilt box\n");
			return 1;
		}
		if ((int)htmlc.base.active != active_before) {
			fprintf(stderr, "FAIL: Test 37 re-pointing moved base.active "
				"%d -> %d; an already-counted entry must not be "
				"counted twice\n",
				active_before, (int)htmlc.base.active);
			return 1;
		}
		fprintf(stderr, "  same node + same URL: re-pointed, no second "
			"fetch, base.active untouched\n");

		free(htmlc.object_list->content);
		nsurl_unref(htmlc.object_list->url);
		free(htmlc.object_list);
		htmlc.object_list = NULL;
		htmlc.num_objects = 0;
		htmlc.base.active = 0;
		nsurl_unref(u);
		dom_nodelist_unref(ps);
		dom_string_unref(ptag);
	}
	fprintf(stderr, "=== Test 37 PASS: a rebuilt box adopts its own "
		"element's object instead of fetching it again ===\n");

	/* --- Test 38 (PHASE 0 CONTROL): the REAL libdom dispatch path ----------
	 *
	 * THIS TEST IS EXPECTED TO FAIL until Phase 1b/1c land. That is its job.
	 *
	 * Every existing event test in this file dispatches through
	 * el.dispatchEvent -- the path that already worked -- which is exactly why
	 * the suite reports 37/37 healthy while hardware ignores every click. The
	 * recorded false-green traps (a bare `make` compiling nothing and printing
	 * all-PASS from a stale binary) mean a green suite proves nothing until
	 * something has been shown to go red. So this dispatches the way real mouse
	 * input does -- fire_generic_dom_event(), the same call
	 * html_mouse_action() makes at interaction.c:1785 -- and asserts the two
	 * things the audit says are broken:
	 *
	 *   A. docFired == 0
	 *      document.addEventListener never reaches libdom. The handler list
	 *      lives in the JS-only document._listeners registry
	 *      (macsurf_qjs.c:5180) that libdom knows nothing about, so a real
	 *      click never reaches $(document).on('click', ...) -- the dominant
	 *      pattern in jQuery, XenForo (XF.activate) and WordPress.
	 *
	 *   B. targetType != 'object'
	 *      qjs_dom_listener_cb (macsurf_qjs.c:2969) looks the target up in the
	 *      wrap table and, on a miss, simply does not set the property.
	 *      Delegation handlers universally begin e.target.matches(...) /
	 *      e.target.closest(...), which throws on undefined and takes the
	 *      handler down with it.
	 *
	 * PART 1 IS A POSITIVE CONTROL and it must PASS today. Without it, a red
	 * Part 2 is ambiguous between "document registration is broken" (the
	 * claim) and "libdom dispatch does not reach JS in this harness at all"
	 * (a harness defect). Part 1 registers an element-level listener the
	 * normal way and dispatches at that same element, so it exercises the
	 * identical C entry point over a path that is known to work.
	 *
	 * The two parts deliberately use DIFFERENT nodes. Part 1 must look its
	 * node up from script, which creates a wrap-table entry -- and Part 2's
	 * whole point is a node that has NO entry, so it uses a node no script in
	 * this file ever touches. #li22 and #li23 exist only in build_large_doc's
	 * <ul> and are referenced nowhere else.
	 */
	fprintf(stderr, "\n=== Test 38 (PHASE 0 CONTROL): real libdom dispatch "
			"-> document delegation + event.target ===\n");
	{
		dom_string *id22 = NULL, *id23 = NULL;
		dom_element *el22 = NULL, *el23 = NULL;
		unsigned char ok;
		int part1_ok = 0;
		int red_count = 0;

		if (dom_string_create((const uint8_t *)"li22", 4, &id22) != DOM_NO_ERR ||
		    dom_string_create((const uint8_t *)"li23", 4, &id23) != DOM_NO_ERR) {
			fprintf(stderr, "FAIL: t38 dom_string_create\n");
			return 1;
		}
		dom_document_get_element_by_id(document, id22, &el22);
		dom_document_get_element_by_id(document, id23, &el23);
		dom_string_unref(id22);
		dom_string_unref(id23);
		if (el22 == NULL || el23 == NULL) {
			fprintf(stderr, "FAIL: t38 fixture missing #li22/#li23 "
					"(el22=%p el23=%p) -- build_large_doc changed?\n",
					(void *)el22, (void *)el23);
			return 1;
		}

		/* ---- Part 1: POSITIVE CONTROL. Element-level listener, script
		 * looks the node up itself (which is what creates its wrapper). */
		{
			const char *setup1 =
				"globalThis.__t38={elFired:0,docFired:0,"
					"targetType:'never-ran'};"
				"var t=globalThis.__t38;"
				"var e=document.getElementById('li22');"
				"if(!e)throw new Error('li22 not reachable from script');"
				"e.addEventListener('click',function(){t.elFired++;});";
			ok = js_exec(thread, (const unsigned char *)setup1,
					strlen(setup1), "t38-setup1.js");
			if (!ok) {
				fprintf(stderr, "FAIL: t38 part 1 setup threw\n");
				return 1;
			}
		}
		(void)fire_generic_dom_event(corestring_dom_click,
				(dom_node *)el22, true, true);
		{
			/* The control question is only "does the real libdom dispatch
			 * reach JS at all", because that is what makes a red Part 2
			 * interpretable. Assert >= 1, not == 1 -- the exact count is a
			 * SEPARATE finding, checked below, and folding it in here would
			 * block Part 2 from ever running. */
			const char *chk1 =
				"if(globalThis.__t38.elFired<1)"
					"throw new Error('CONTROL BROKEN: an element-level "
						"addEventListener handler did NOT fire at all from "
						"fire_generic_dom_event. Part 2 below cannot be "
						"interpreted until this passes -- a red Part 2 would "
						"mean the harness cannot dispatch at all, not that "
						"document delegation is broken.');";
			ok = js_exec(thread, (const unsigned char *)chk1,
					strlen(chk1), "t38-chk1.js");
			if (!ok) {
				fprintf(stderr, "FAIL: Test 38 positive control is broken -- "
						"fix the harness before reading Part 2\n");
				return 1;
			}
			part1_ok = 1;
			fprintf(stderr, "  part 1 OK (control): element-level listener "
					"fires from the real libdom dispatch\n");
		}

		/* ---- Part 1b: THE DOUBLE-FIRE. Found by this control on its first
		 * run; it was not in the audit.
		 *
		 * ONE addEventListener('click', fn) on the target fires TWICE per
		 * dispatch. libdom's event-path walk
		 * (browser/libdom/src/core/node.c:2549) starts at the target itself,
		 * so targets[0] IS the target; the target is then dispatched once in
		 * the AT_TARGET phase (matching clause 3 of the filter in
		 * events/event_target.c:235, `evt->target == evt->current &&
		 * phase == DOM_AT_TARGET`) and AGAIN in the bubble pass, where
		 * targets[0] matches clause 2 (`le->capture == false &&
		 * phase == DOM_BUBBLING_PHASE`). Per spec the target must be visited
		 * exactly once.
		 *
		 * This is live on hardware: interaction.c:1785 calls
		 * fire_generic_dom_event(corestring_dom_click, mas.node, true, true),
		 * so since fixes989 wired addEventListener to libdom, every
		 * element-level click handler on the clicked element has run twice per
		 * click -- double form submits, double toggles, double counters.
		 *
		 * NOT a MacSurf double-registration: qjs_el_add_event_listener_data
		 * only calls qjs_dom_register_listener when `fresh` (the first
		 * listener of that type on that node), and this test registers one. */
		{
			const char *chk1b =
				"if(globalThis.__t38.elFired!==1)"
					"throw new Error('ASSERT FAIL (C): ONE "
						"addEventListener(\"click\") handler fired '+"
						"globalThis.__t38.elFired+' times for ONE dispatch. "
						"libdom visits the target twice -- once at AT_TARGET "
						"and again as targets[0] in the bubble pass "
						"(core/node.c:2549 builds the path starting AT the "
						"target; events/event_target.c:235 then matches both "
						"the AT_TARGET clause and the non-capture BUBBLING "
						"clause). Live on hardware via interaction.c:1785: "
						"every click handler on the clicked element runs "
						"twice.');";
			ok = js_exec(thread, (const unsigned char *)chk1b,
					strlen(chk1b), "t38-chk1b.js");
			if (!ok) {
				red_count++;
				fprintf(stderr, "  part 1b RED: target double-fire (see "
						"assertion above) -- continuing so the round reports "
						"every finding in one run\n");
			} else {
				fprintf(stderr, "  part 1b OK: target visited exactly once\n");
			}
		}

		/* ---- Part 1c: THE CAPTURE MIRROR.
		 *
		 * The capture loop at core/node.c:2601 runs targetnr = ntargets down
		 * to 1, i.e. it accesses targets[ntargets-1] .. targets[0] -- and
		 * targets[0] IS the target. So a capture:true listener on the target
		 * fires in the CAPTURE pass (clause 1) and AGAIN at AT_TARGET (clause
		 * 3, which ignores the capture flag entirely).
		 *
		 * That makes the bug symmetric, and it makes the fix two clauses
		 * rather than one: BOTH the capture and the bubble clause must
		 * additionally require evt->target != evt->current. Per DOM spec the
		 * capture and bubble passes cover ANCESTORS ONLY; the target is
		 * visited exactly once, in AT_TARGET, where capture and non-capture
		 * listeners both run in registration order.
		 *
		 * Uses #li21 so it cannot interfere with part 1b's node. */
		{
			dom_string *id21 = NULL;
			dom_element *el21 = NULL;
			if (dom_string_create((const uint8_t *)"li21", 4, &id21) != DOM_NO_ERR) {
				fprintf(stderr, "FAIL: t38 dom_string_create li21\n");
				return 1;
			}
			dom_document_get_element_by_id(document, id21, &el21);
			dom_string_unref(id21);
			if (el21 == NULL) {
				fprintf(stderr, "FAIL: t38 fixture missing #li21\n");
				return 1;
			}
			{
				const char *setup1c =
					"globalThis.__t38.capFired=0;"
					"var t=globalThis.__t38;"
					"var e=document.getElementById('li21');"
					"e.addEventListener('click',function(){t.capFired++;},true);";
				ok = js_exec(thread, (const unsigned char *)setup1c,
						strlen(setup1c), "t38-setup1c.js");
				if (!ok) {
					fprintf(stderr, "FAIL: t38 part 1c setup threw\n");
					return 1;
				}
			}
			(void)fire_generic_dom_event(corestring_dom_click,
					(dom_node *)el21, true, true);
			{
				const char *chk1c =
					"if(globalThis.__t38.capFired!==1)"
						"throw new Error('ASSERT FAIL (D): a capture:true "
							"listener ON THE TARGET fired '+"
							"globalThis.__t38.capFired+' times for ONE "
							"dispatch. The capture loop (core/node.c:2601) "
							"walks targets[ntargets-1]..targets[0] and "
							"targets[0] IS the target, so it fires in the "
							"capture pass AND again at AT_TARGET. The bug is "
							"SYMMETRIC: the fix must exclude the target from "
							"BOTH the capture and the bubble pass, not just "
							"the bubble pass.');";
				ok = js_exec(thread, (const unsigned char *)chk1c,
						strlen(chk1c), "t38-chk1c.js");
				if (!ok) {
					red_count++;
					fprintf(stderr, "  part 1c RED: capture mirror -- the fix "
							"is two clauses, not one\n");
				} else {
					fprintf(stderr, "  part 1c OK: capture listener on the "
							"target fires exactly once\n");
				}
			}
		}

		/* ---- Part 1d: IT IS NOT CLICK-SPECIFIC.
		 *
		 * The double-visit is in the PATH WALK, not in the event type, so
		 * every type dispatched through this entry point is affected. The
		 * claim in the round writeup is "every element-level handler since
		 * fixes989", and that deserves an assertion rather than an inference
		 * -- double form submit is the most user-visible instance of this bug.
		 *
		 * interaction.c dispatches exactly three types today: click (:1785),
		 * submit (:1857) and keydown (:2024). All three are checked here, on
		 * separate nodes so they cannot interfere. */
		{
			static const char *const ids[3] = { "li20", "li19", "li18" };
			dom_string *types_ds[3];
			dom_element *els[3];
			int i;

			types_ds[0] = corestring_dom_click;
			types_ds[1] = corestring_dom_submit;
			types_ds[2] = corestring_dom_keydown;

			for (i = 0; i < 3; i++) {
				dom_string *idn = NULL;
				els[i] = NULL;
				if (dom_string_create((const uint8_t *)ids[i], 4, &idn)
						!= DOM_NO_ERR) {
					fprintf(stderr, "FAIL: t38 dom_string_create %s\n", ids[i]);
					return 1;
				}
				dom_document_get_element_by_id(document, idn, &els[i]);
				dom_string_unref(idn);
				if (els[i] == NULL) {
					fprintf(stderr, "FAIL: t38 fixture missing #%s\n", ids[i]);
					return 1;
				}
			}
			{
				const char *setup1d =
					"globalThis.__t38.n={click:0,submit:0,keydown:0};"
					"var t=globalThis.__t38.n;"
					"document.getElementById('li20')"
						".addEventListener('click',function(){t.click++;});"
					"document.getElementById('li19')"
						".addEventListener('submit',function(){t.submit++;});"
					"document.getElementById('li18')"
						".addEventListener('keydown',function(){t.keydown++;});";
				ok = js_exec(thread, (const unsigned char *)setup1d,
						strlen(setup1d), "t38-setup1d.js");
				if (!ok) {
					fprintf(stderr, "FAIL: t38 part 1d setup threw\n");
					return 1;
				}
			}
			for (i = 0; i < 3; i++) {
				(void)fire_generic_dom_event(types_ds[i],
						(dom_node *)els[i], true, true);
			}
			{
				const char *chk1d =
					"var n=globalThis.__t38.n;"
					"if(n.click!==1||n.submit!==1||n.keydown!==1)"
						"throw new Error('ASSERT FAIL (E): the double-visit is "
							"not click-specific -- click='+n.click+' submit='+"
							"n.submit+' keydown='+n.keydown+', each from ONE "
							"dispatch. All three are the types interaction.c "
							"dispatches today (:1785 click, :1857 submit, "
							":2024 keydown), so on hardware a form with a "
							"submit handler submits TWICE.');";
				ok = js_exec(thread, (const unsigned char *)chk1d,
						strlen(chk1d), "t38-chk1d.js");
				if (!ok) {
					red_count++;
					fprintf(stderr, "  part 1d RED: click/submit/keydown all "
							"double-fire -- double form submit confirmed\n");
				} else {
					fprintf(stderr, "  part 1d OK: click/submit/keydown each "
							"fire exactly once\n");
				}
			}
		}

		/* ---- Part 2: THE FAILING CONTROL. Document-level delegation, and
		 * event.target for a node script has never looked up. */
		{
			const char *setup2 =
				"var t=globalThis.__t38;"
				"document.addEventListener('click',function(ev){"
					"t.docFired++;"
					"t.targetType=(ev===undefined||ev===null)?'no-event':"
						"(ev.target===undefined?'undefined':"
						"(ev.target===null?'null':'object'));"
				"});";
			ok = js_exec(thread, (const unsigned char *)setup2,
					strlen(setup2), "t38-setup2.js");
			if (!ok) {
				fprintf(stderr, "FAIL: t38 part 2 setup threw\n");
				return 1;
			}
		}
		(void)fire_generic_dom_event(corestring_dom_click,
				(dom_node *)el23, true, true);
		{
			const char *chk2 =
				"var t=globalThis.__t38;"
				"if(t.docFired===0)"
					"throw new Error('ASSERT FAIL (A): a document-level "
						"click listener did NOT fire from the real libdom "
						"dispatch. document.addEventListener stores into the "
						"JS-only document._listeners registry (macsurf_qjs.c "
						":5180) and never calls qjs_dom_register_listener, so "
						"libdom has no listener on the document node and a "
						"real mouse click can never reach "
						"$(document).on(\"click\", ...) -- the dominant "
						"pattern in jQuery, XenForo and WordPress.');"
				"if(t.targetType!=='object')"
					"throw new Error('ASSERT FAIL (B): ev.target was \"'+"
						"t.targetType+'\", not an object, for a node script "
						"never looked up. qjs_dom_listener_cb "
						"(macsurf_qjs.c:2969) looks the target up in the wrap "
						"table and skips the property on a miss, so every "
						"delegation handler that begins e.target.matches(...) "
						"or e.target.closest(...) throws and dies.');";
			ok = js_exec(thread, (const unsigned char *)chk2,
					strlen(chk2), "t38-chk2.js");
			if (!ok) {
				red_count++;
				fprintf(stderr,
					"=== Test 38 FAILED AS EXPECTED (Phase 0 control) ===\n"
					"    The positive control passed (part1_ok=%d), so the "
					"harness CAN dispatch through the real libdom path.\n"
					"    What is red is the diagnosis under test: document "
					"delegation and event.target.\n"
					"    This turns green when Phase 1b + 1c land. Until "
					"then a red suite here is the CORRECT result.\n",
					part1_ok);
			}
			if (red_count != 0) {
				return 1;
			}
		}
		/* ---- Part 3: THE REAL DELEGATION PATTERN, end to end.
		 *
		 * Parts 1-2 prove the mechanism. This proves the thing sites
		 * actually write -- jQuery's $(document).on('click', sel, fn),
		 * XenForo's XF.activate, every WordPress admin script:
		 *
		 *   document.addEventListener('click', function (e) {
		 *       var t = e.target.closest('.js-thing');
		 *       if (!t) return;
		 *       ...
		 *   });
		 *
		 * Both halves had to be true at once for this to work, which is why
		 * it is asserted as one test rather than two: the listener has to
		 * REACH the document (1b) AND e.target has to be a real wrapper with
		 * real methods on a node script never looked up (1c). Either one
		 * missing and this is dead -- undefined.closest throws, or the
		 * handler never runs at all.
		 *
		 * Also asserts window listeners fire (they registered against the
		 * document node and fan out separately) and that e.target is the
		 * node that was actually clicked, not an ancestor. */
		{
			dom_string *id17 = NULL;
			dom_element *el17 = NULL;
			if (dom_string_create((const uint8_t *)"li17", 4, &id17) != DOM_NO_ERR) {
				fprintf(stderr, "FAIL: t38 dom_string_create li17\n");
				return 1;
			}
			dom_document_get_element_by_id(document, id17, &el17);
			dom_string_unref(id17);
			if (el17 == NULL) {
				fprintf(stderr, "FAIL: t38 fixture missing #li17\n");
				return 1;
			}
			{
				const char *setup3 =
					"var t=globalThis.__t38;"
					"t.dele=0;t.deleId='';t.win=0;t.threw='';"
					"document.addEventListener('click',function(e){"
						"try{"
							"var n=e.target.closest('li');"
							"if(!n)return;"
							"t.dele++;t.deleId=n.getAttribute('id')||'';"
						"}catch(err){t.threw=String(err&&err.message||err);}"
					"});"
					"window.addEventListener('click',function(){t.win++;});";
				ok = js_exec(thread, (const unsigned char *)setup3,
						strlen(setup3), "t38-setup3.js");
				if (!ok) {
					fprintf(stderr, "FAIL: t38 part 3 setup threw\n");
					return 1;
				}
			}
			(void)fire_generic_dom_event(corestring_dom_click,
					(dom_node *)el17, true, true);
			{
				const char *chk3 =
					"var t=globalThis.__t38;"
					"if(t.threw)"
						"throw new Error('ASSERT FAIL (F): the delegation "
							"handler THREW: '+t.threw+' -- this is the "
							"e.target.closest(...) pattern every framework "
							"opens with.');"
					"if(t.dele!==1)"
						"throw new Error('ASSERT FAIL (F): the delegation "
							"handler ran '+t.dele+' times, expected 1. "
							"$(document).on(\"click\", sel, fn) is the "
							"dominant pattern on the web; if this is 0 the "
							"listener never reached the document, and the "
							"page renders perfectly and ignores every "
							"click.');"
					"if(t.deleId!=='li17')"
						"throw new Error('ASSERT FAIL (F): closest() resolved "
							"to \"'+t.deleId+'\", expected li17 -- e.target "
							"is not the node that was clicked.');"
					"if(t.win!==1)"
						"throw new Error('ASSERT FAIL (G): a window click "
							"listener fired '+t.win+' times, expected 1. "
							"window has no DOM node, so its listeners "
							"register against the document node and fan out "
							"in qjs_dom_listener_cb.');";
				ok = js_exec(thread, (const unsigned char *)chk3,
						strlen(chk3), "t38-chk3.js");
				if (!ok) {
					fprintf(stderr, "FAIL: Test 38 part 3 -- real delegation "
							"is still broken\n");
					return 1;
				}
			}
			fprintf(stderr, "  part 3 OK: $(document).on-style delegation "
					"works -- e.target.closest('li') resolved li17, and "
					"window listeners fire\n");
		}

		fprintf(stderr, "=== Test 38 PASS: document delegation and "
				"event.target work from the real libdom dispatch ===\n");
	}

	/* --- Test 39: document.write + head-element fragments (fixes1007) ------
	 *
	 * Hardware picked this one, not the audit. With fixes1006 in, the TOP
	 * remaining JS exception on hackaday.com is
	 *   TypeError: not a function   at <eval> (?inline script?:2:1480)
	 * and that offset is `document.write(OA_spc)` in its ad script.
	 * document.write did not exist at all -- zero occurrences in
	 * macsurf_qjs.c.
	 *
	 * Writing it surfaced a SECOND, more general bug: the fragment parser has
	 * no context-element support, so it wraps markup in an implied
	 * <html><head>/<body>, and the innerHTML descent took <body>'s children
	 * ONLY. Markup starting with a head-only element -- <script>, <style>,
	 * <link>, <meta> -- was silently dropped. Measured before the fix:
	 * `div.innerHTML = '<script src=...></script>'` gave ZERO children.
	 * That is an innerHTML bug in its own right; document.write just made it
	 * visible, because a written <script> is exactly what the ad code emits.
	 */
	fprintf(stderr, "\n=== Test 39: document.write + head-element fragments ===\n");
	{
		const char *probe =
			"globalThis.__t39={};var r=globalThis.__t39;"
			/* (a) the innerHTML half, independent of document.write */
			"var h=document.createElement('div');"
			"h.innerHTML='<script id=\"zz\" src=\"http://e.invalid/a.js\">"
				"<\\/script>';"
			"r.ihKids=0;var n=h.firstChild;"
			"while(n){r.ihKids++;n=n.nextSibling;}"
			/* (b) document.write itself */
			"r.isFn=(typeof document.write==='function');"
			"r.isLnFn=(typeof document.writeln==='function');"
			"if(r.isFn){"
				"document.currentScript=null;"   /* force the <body> fallback */
				"document.write('<p id=\"dw1\">written</p>"
					"<span id=\"dw2\">two</span>');"
				"var e=document.getElementById('dw1');"
				"r.p=!!e;r.s=!!document.getElementById('dw2');"
				"r.text=e?(e.textContent||''):'';"
				"r.tag=e?String(e.tagName).toLowerCase():'';"
				"document.write('<script id=\"dw3\" "
					"src=\"http://example.invalid/x.js\"><\\/script>');"
				"var s3=document.getElementById('dw3');"
				"r.script=!!s3;"
				"r.scriptSrc=s3?(s3.getAttribute('src')||''):'';"
			"}";
		unsigned char ok = js_exec(thread, (const unsigned char *)probe,
				strlen(probe), "t39-probe.js");
		if (!ok) { fprintf(stderr, "FAIL: t39 probe threw\n"); return 1; }
		{
			const char *chk =
				"var r=globalThis.__t39;"
				"if(r.ihKids<1)"
					"throw new Error('ASSERT FAIL: innerHTML with a leading "
						"<script> produced '+r.ihKids+' children. The "
						"fragment parser puts head-only elements (script, "
						"style, link, meta) in the implied <head>, and the "
						"descent took <body> only -- so they were dropped "
						"silently.');"
				"if(!r.isFn)"
					"throw new Error('ASSERT FAIL: document.write is not a "
						"function -- the TOP JS exception on real "
						"hackaday.com.');"
				"if(!r.isLnFn)"
					"throw new Error('ASSERT FAIL: document.writeln missing');"
				"if(!r.p||!r.s)"
					"throw new Error('ASSERT FAIL: written markup did not "
						"reach the tree (p='+r.p+' span='+r.s+') -- it must "
						"be PARSED into elements, not inserted as text.');"
				"if(r.tag!=='p')"
					"throw new Error('ASSERT FAIL: written node is a \"'+"
						"r.tag+'\", expected a real <p>');"
				"if(r.text!=='written')"
					"throw new Error('ASSERT FAIL: written text is \"'+"
						"r.text+'\"');"
				"if(!r.script)"
					"throw new Error('ASSERT FAIL: a written <script> did not "
						"become a script element, so dom_SCRIPT_showed_up can "
						"never fetch it -- which is exactly what the hackaday "
						"ad script writes.');"
				"if(r.scriptSrc.indexOf('example.invalid')<0)"
					"throw new Error('ASSERT FAIL: written script lost its "
						"src (got \"'+r.scriptSrc+'\")');";
			ok = js_exec(thread, (const unsigned char *)chk,
					strlen(chk), "t39-chk.js");
			if (!ok) {
				fprintf(stderr, "FAIL: document.write / head-element "
						"fragments are not usable\n");
				return 1;
			}
		}
		fprintf(stderr, "  head-element fragments survive; markup parses into "
				"real elements; a written <script> keeps its src\n");
	}
	fprintf(stderr, "=== Test 39 PASS: document.write + head-element "
			"fragments ===\n");

	/* --- Test 40: the Phase 1 event model + DOM surface (fixes1008) --------
	 *
	 * Covers, in one place, what a page actually needs from the event model.
	 * Every assertion is a COUNT or a VALUE, never a boolean -- the libdom
	 * target double-fire hid for ~15 rounds behind `if (fired)`. */
	fprintf(stderr, "\n=== Test 40: Event objects, bubbling dispatch, "
			"stopImmediatePropagation, DOM surface ===\n");
	{
		dom_string *idl = NULL;
		dom_element *ell = NULL;
		unsigned char ok;

		if (dom_string_create((const uint8_t *)"li16", 4, &idl) != DOM_NO_ERR) {
			fprintf(stderr, "FAIL: t40 dom_string_create\n"); return 1;
		}
		dom_document_get_element_by_id(document, idl, &ell);
		dom_string_unref(idl);
		if (ell == NULL) { fprintf(stderr, "FAIL: t40 missing #li16\n"); return 1; }

		{
			const char *setup =
				"globalThis.__t40={};var r=globalThis.__t40;"
				"r.n=0;r.sib=0;r.phase=-1;r.trusted=null;r.isEv=null;"
				"r.bub=null;r.cancel=null;r.ts=0;"
				"var e=document.getElementById('li16');"
				/* two listeners: the first stops the second immediately */
				"e.addEventListener('click',function(ev){"
					"r.n++;"
					"r.isEv=(ev instanceof Event);"
					"r.phase=ev.eventPhase;r.trusted=ev.isTrusted;"
					"r.bub=ev.bubbles;r.cancel=ev.cancelable;"
					"r.ts=ev.timeStamp;"
					"if(ev.stopImmediatePropagation)ev.stopImmediatePropagation();"
				"});"
				"e.addEventListener('click',function(){r.sib++;});"
				/* synthetic dispatch must BUBBLE to the document */
				"r.doc=0;"
				"document.addEventListener('synth',function(){r.doc++;});"
				"r.ret=e.dispatchEvent({type:'synth',bubbles:true,"
					"cancelable:true});"
				/* DOM surface */
				"var host=document.createElement('div');"
				"var a=document.createElement('span');"
				"host.appendChild(a);"
				"r.hasRemove=(typeof a.remove==='function');"
				"a.remove();r.afterRemove=host.children.length;"
				"host.insertAdjacentHTML('beforeend','<b id=\"iah\">x</b>');"
				"r.iah=(host.children.length===1);"
				"r.iahTag=host.children.length?"
					"String(host.children[0].tagName).toLowerCase():'';"
				"var d2=document.createElement('i');"
				"host.append(d2);r.appended=(host.children.length===2);"
				"r.conn=document.getElementById('li16').isConnected;"
				"r.detached=host.isConnected;"
				"r.owner=(document.getElementById('li16').ownerDocument===document);"
				"var inp=document.createElement('input');"
				"inp.checked=true;r.checked=inp.checked;"
				"inp.checked=false;r.unchecked=inp.checked;";
			ok = js_exec(thread, (const unsigned char *)setup,
					strlen(setup), "t40-setup.js");
			if (!ok) { fprintf(stderr, "FAIL: t40 setup threw\n"); return 1; }
		}

		(void)fire_generic_dom_event(corestring_dom_click,
				(dom_node *)ell, true, true);

		{
			const char *chk =
				"var r=globalThis.__t40;"
				"if(r.n!==1)"
					"throw new Error('ASSERT FAIL: listener ran '+r.n+' times, "
						"expected 1');"
				"if(r.sib!==0)"
					"throw new Error('ASSERT FAIL: stopImmediatePropagation "
						"did not stop the sibling listener (it ran '+r.sib+' "
						"times). It was aliased to stopPropagation, which "
						"stops the next NODE but lets the rest of THIS "
						"node run -- the exact distinction the method "
						"exists for.');"
				"if(r.isEv!==true)"
					"throw new Error('ASSERT FAIL: the event is not an "
						"instance of Event -- it was an ad-hoc object, so "
						"instanceof checks in library code fail.');"
				"if(r.phase!==2)"
					"throw new Error('ASSERT FAIL: eventPhase is '+r.phase+', "
						"expected 2 (AT_TARGET). Before fixes1005 the "
						"spurious second visit reported BUBBLING, and a "
						"count-only test would not have caught it.');"
				"if(r.trusted!==true)"
					"throw new Error('ASSERT FAIL: isTrusted is '+r.trusted+' "
						"for a NATIVE dispatch; must be true here and false "
						"for dispatchEvent.');"
				"if(r.bub!==true||r.cancel!==true)"
					"throw new Error('ASSERT FAIL: bubbles/cancelable missing "
						"(bubbles='+r.bub+' cancelable='+r.cancel+')');"
				"if(!(r.ts>0))"
					"throw new Error('ASSERT FAIL: timeStamp is '+r.ts);"
				"if(r.doc!==1)"
					"throw new Error('ASSERT FAIL: el.dispatchEvent did not "
						"BUBBLE to the document (fired '+r.doc+'). It used "
						"to fire only the node own _L/_H, so a framework "
						"triggering a control and relying on delegation saw "
						"nothing.');"
				"if(r.ret!==true)"
					"throw new Error('ASSERT FAIL: dispatchEvent returned '+"
						"r.ret+' for an uncancelled event, expected true');"
				"if(!r.hasRemove)throw new Error('ASSERT FAIL: el.remove missing');"
				"if(r.afterRemove!==0)"
					"throw new Error('ASSERT FAIL: el.remove() left '+"
						"r.afterRemove+' children');"
				"if(!r.iah||r.iahTag!=='b')"
					"throw new Error('ASSERT FAIL: insertAdjacentHTML gave "
						"tag \"'+r.iahTag+'\"');"
				"if(!r.appended)throw new Error('ASSERT FAIL: append() failed');"
				"if(r.conn!==true)"
					"throw new Error('ASSERT FAIL: isConnected false for an "
						"in-document node');"
				"if(r.detached!==false)"
					"throw new Error('ASSERT FAIL: isConnected true for a "
						"detached node');"
				"if(!r.owner)throw new Error('ASSERT FAIL: ownerDocument wrong');"
				"if(r.checked!==true||r.unchecked!==false)"
					"throw new Error('ASSERT FAIL: input.checked does not "
						"round-trip (set true -> '+r.checked+', set false -> "
						"'+r.unchecked+')');";
			ok = js_exec(thread, (const unsigned char *)chk,
					strlen(chk), "t40-chk.js");
			if (!ok) {
				fprintf(stderr, "FAIL: Test 40 -- event model / DOM surface\n");
				return 1;
			}
		}
		fprintf(stderr, "  real Event instances (phase/isTrusted/bubbles/"
				"timeStamp), stopImmediatePropagation stops siblings, "
				"synthetic dispatch bubbles to document, DOM surface live\n");
	}
	fprintf(stderr, "=== Test 40 PASS: event model + DOM surface ===\n");

	/* --- Test 41: element-scoped getElementsBy* (fixes1009) ----------------
	 *
	 * Hardware named this one. With fixes1008 in, the ONLY two JS exceptions
	 * left on hackaday.com were both
	 *   TypeError: not a function   at <bundle>:17:41
	 * and line 17 col 41 of that bundle is
	 *   container.getElementsByTagName('ul')[0]
	 * -- the WordPress navigation script. getElementsBy* existed on document
	 * but never on elements.
	 *
	 * Asserts SCOPING, not just presence: a document-wide fallback would
	 * satisfy "is a function" while returning the wrong nodes, which is the
	 * more dangerous failure because it looks like it works. */
	fprintf(stderr, "\n=== Test 41: element-scoped getElementsBy* ===\n");
	{
		const char *probe =
			"globalThis.__t41={};var r=globalThis.__t41;"
			"var host=document.createElement('div');"
			"var inner=document.createElement('ul');"
			"inner.className='wanted';"
			"host.appendChild(inner);"
			"var other=document.createElement('ul');"
			"other.className='wanted';"
			"var far=document.getElementById('feed');"
			"if(far)far.appendChild(other);"   /* OUTSIDE host, in the doc */
			"r.isFn=(typeof host.getElementsByTagName==='function');"
			"r.isCls=(typeof host.getElementsByClassName==='function');"
			"r.byTag=r.isFn?host.getElementsByTagName('ul').length:-1;"
			"r.byCls=r.isCls?host.getElementsByClassName('wanted').length:-1;"
			"r.hasKids=(typeof host.hasChildNodes==='function')?"
				"host.hasChildNodes():null;"
			"r.toggle=(typeof host.toggleAttribute==='function')?"
				"(host.toggleAttribute('hidden'),host.hasAttribute('hidden')):null;";
		unsigned char ok = js_exec(thread, (const unsigned char *)probe,
				strlen(probe), "t41-probe.js");
		if (!ok) { fprintf(stderr, "FAIL: t41 probe threw\n"); return 1; }
		{
			const char *chk =
				"var r=globalThis.__t41;"
				"if(!r.isFn)"
					"throw new Error('ASSERT FAIL: element.getElementsByTagName "
						"is not a function -- the exact throw hackaday hit at "
						"bundle:17:41, container.getElementsByTagName(\"ul\").');"
				"if(!r.isCls)"
					"throw new Error('ASSERT FAIL: "
						"element.getElementsByClassName missing');"
				"if(r.byTag!==1)"
					"throw new Error('ASSERT FAIL: getElementsByTagName "
						"returned '+r.byTag+' , expected exactly 1 -- it must "
						"be SCOPED to the element. A document-wide fallback "
						"would also be \"a function\" while returning the "
						"wrong nodes, which is worse than throwing.');"
				"if(r.byCls!==1)"
					"throw new Error('ASSERT FAIL: getElementsByClassName "
						"returned '+r.byCls+', expected 1 (scoped)');"
				"if(r.hasKids!==true)"
					"throw new Error('ASSERT FAIL: hasChildNodes');"
				"if(r.toggle!==true)"
					"throw new Error('ASSERT FAIL: toggleAttribute');";
			ok = js_exec(thread, (const unsigned char *)chk,
					strlen(chk), "t41-chk.js");
			if (!ok) {
				fprintf(stderr, "FAIL: Test 41 -- element-scoped "
						"getElementsBy*\n");
				return 1;
			}
		}
		fprintf(stderr, "  scoped to the element, not the document\n");
	}
	fprintf(stderr, "=== Test 41 PASS: element-scoped getElementsBy* ===\n");

	/* --- Test 42: node universals on EVERY shape (fixes1010) ---------------
	 *
	 * fixes1009 added getRootNode to elements only, and that ALONE broke
	 * jQuery. jQuery feature-detects `getRootNode` on a probe element and, if
	 * it is present, swaps isAttached for a version that calls
	 * e.getRootNode() on arbitrary nodes -- including text nodes, which did
	 * not have it. Hardware caught it as the last remaining exception on
	 * hackaday, at jQuery's isAttached.
	 *
	 * The general rule, and the reason this test exists: a feature-detect
	 * makes a PARTIAL implementation worse than none at all. Anything a
	 * library probes for must exist on every shape it can then be called on.
	 * So this asserts the universals on element, TEXT and document alike. */
	fprintf(stderr, "\n=== Test 42: node universals on every wrapper shape ===\n");
	{
		const char *probe =
			"globalThis.__t42={};var r=globalThis.__t42;"
			"var host=document.getElementById('feed');"
			"var t=document.createTextNode('hi');"
			"host.appendChild(t);"
			"r.elRoot=(typeof host.getRootNode==='function');"
			"r.txRoot=(typeof t.getRootNode==='function');"
			"r.docRoot=(typeof document.getRootNode==='function');"
			"r.txOwner=(t.ownerDocument===document);"
			"r.elOwner=(host.ownerDocument===document);"
			"r.txConn=t.isConnected;"
			"r.elConn=host.isConnected;"
			"r.docContains=(typeof document.contains==='function');"
			"r.containsEl=document.contains(host);"
			/* jQuery's actual isAttached, replayed verbatim */
			"r.jq=null;"
			"try{r.jq=(document.contains(t.ownerDocument===document?host:t)||"
				"t.getRootNode()===t.ownerDocument);}"
			"catch(e){r.jq='THREW: '+((e&&e.message)||e);}";
		unsigned char ok = js_exec(thread, (const unsigned char *)probe,
				strlen(probe), "t42-probe.js");
		if (!ok) { fprintf(stderr, "FAIL: t42 probe threw\n"); return 1; }
		{
			const char *chk =
				"var r=globalThis.__t42;"
				"if(!r.elRoot||!r.txRoot||!r.docRoot)"
					"throw new Error('ASSERT FAIL: getRootNode missing "
						"(element='+r.elRoot+' text='+r.txRoot+' document='+"
						"r.docRoot+'). A feature-detect makes a PARTIAL "
						"implementation worse than none: jQuery probes for "
						"it on an element and then calls it on text nodes.');"
				"if(!r.txOwner||!r.elOwner)"
					"throw new Error('ASSERT FAIL: ownerDocument wrong "
						"(text='+r.txOwner+' element='+r.elOwner+')');"
				"if(r.txConn!==true||r.elConn!==true)"
					"throw new Error('ASSERT FAIL: isConnected wrong for "
						"in-document nodes (text='+r.txConn+' element='+"
						"r.elConn+')');"
				"if(!r.docContains)"
					"throw new Error('ASSERT FAIL: document.contains missing. "
						"jQuery GUARDS this call, so it does not throw -- it "
						"silently answers \"detached\" for every element, "
						"which is worse than throwing.');"
				"if(r.containsEl!==true)"
					"throw new Error('ASSERT FAIL: document.contains(el) is "
						"'+r.containsEl+' for an in-document element');"
				"if(r.jq!==true)"
					"throw new Error('ASSERT FAIL: jQuery isAttached gives '+"
						"r.jq);";
			ok = js_exec(thread, (const unsigned char *)chk,
					strlen(chk), "t42-chk.js");
			if (!ok) {
				fprintf(stderr, "FAIL: Test 42 -- node universals\n");
				return 1;
			}
		}
		fprintf(stderr, "  getRootNode/ownerDocument/isConnected on element, "
				"text and document; jQuery isAttached resolves\n");
	}
	fprintf(stderr, "=== Test 42 PASS: node universals ===\n");

	/* --- Test 43: layout is visible to JS (fixes1011, Phase 3) -------------
	 *
	 * getComputedStyle returned inline styles only, getBoundingClientRect
	 * returned all zeros, and offsetWidth/clientHeight existed only on the
	 * MOCK fallback elements. This is the failure mode that does NOT throw:
	 * zeros propagate into arithmetic and the page merely looks wrong, which
	 * is why it outlived every throwing bug.
	 *
	 * Asserted against the REAL box tree, cross-checked in C via box_coords
	 * and box->width -- so a stub that returned plausible constants would
	 * still fail. That cross-check is the point: "non-zero" alone would pass
	 * for a hardcoded 100. */
	fprintf(stderr, "\n=== Test 43: layout visible to JS ===\n");
	{
		dom_string *idf = NULL;
		dom_element *elf = NULL;
		struct box *bx;
		int cx = 0, cy = 0, cw = 0, ch = 0;
		unsigned char ok;
		char expect[160];

		if (dom_string_create((const uint8_t *)"feed", 4, &idf) != DOM_NO_ERR) {
			fprintf(stderr, "FAIL: t43 dom_string_create\n"); return 1;
		}
		dom_document_get_element_by_id(document, idf, &elf);
		dom_string_unref(idf);
		if (elf == NULL) { fprintf(stderr, "FAIL: t43 missing #feed\n"); return 1; }

		/* fixes1073 (#265) — SETTLE FIRST, then inject.
		 *
		 * This test injects known geometry straight into a real box and
		 * asserts JS reads it back, which is the only way to test the
		 * read path in a harness that has no layout pass. That works
		 * only if the box survives until the probe.
		 *
		 * Geometry now forces a synchronous reconvert when a mutation is
		 * outstanding, and earlier tests leave mutations pending (the
		 * harness stubs macos9_schedule to a no-op, so the debounced
		 * reconvert never fires here and the pending set only grows).
		 * So the probe below would rebuild the tree, free the box we
		 * just wrote into, and read a fresh one -- which is the feature
		 * behaving correctly and the test measuring the wrong thing.
		 *
		 * Draining here makes the probe's own flush a no-op, so the
		 * injected values survive and the assertions still describe the
		 * read path. Re-resolve the box AFTER, because the drain may
		 * have replaced the tree. */
		htmlc.base.status = CONTENT_STATUS_DONE;
		htmlc.reflowing = false;
		htmlc.box_conversion_context = NULL;
		htmlc.aborted = false;
		htmlc.base.active = 0;
		{
			extern int macos9_reconvert_flush_now(void *cv);
			(void) macos9_reconvert_flush_now((void *)&htmlc);
		}

		bx = box_for_node((dom_node *)elf);
		if (bx == NULL) {
			fprintf(stderr, "FAIL: t43 #feed has no box -- the fixture did "
					"not lay out, so this test cannot mean anything\n");
			return 1;
		}
		/* THE HARNESS HAS NO LAYOUT PASS. It runs dom_to_box (box
		 * construction) but never layout_document, so every box keeps its
		 * birth width UNKNOWN_WIDTH == INT_MAX and nothing has real geometry.
		 * A comparison of 0 against 0 would pass even with every accessor
		 * stubbed out, so this INJECTS known geometry into a real box and
		 * asserts JS reads exactly that back. That tests the read path --
		 * box_for_node -> box_coords -> the metric arithmetic -- which is the
		 * part that is ours. Whether layout produces the right numbers is
		 * layout's own business and is not what this test is for. */
		bx->width = 300;
		bx->height = 40;
		bx->padding[LEFT] = 5; bx->padding[RIGHT] = 7;
		bx->padding[TOP] = 2;  bx->padding[BOTTOM] = 3;
		bx->border[LEFT].width = 1; bx->border[RIGHT].width = 1;
		bx->border[TOP].width = 1;  bx->border[BOTTOM].width = 1;
box_coords(bx, &cx, &cy);
		/* Mirror the engine's sanitiser: a box that never resolved keeps
		 * its birth width UNKNOWN_WIDTH == INT_MAX (the fixes625 sentinel),
		 * and #feed in this fixture is exactly that -- the first run of this
		 * test reported a border-box of 2147483647x0, which is what put the
		 * sanitiser into fixes1011. Anything out of range means 0. */
		#define T43_SANE(v) (((v) < 0 || (v) >= 1000000) ? 0 : (v))
		cw = T43_SANE(bx->width) + bx->padding[LEFT] + bx->padding[RIGHT] +
			bx->border[LEFT].width + bx->border[RIGHT].width;
		ch = T43_SANE(bx->height) + bx->padding[TOP] + bx->padding[BOTTOM] +
			bx->border[TOP].width + bx->border[BOTTOM].width;
		fprintf(stderr, "  C side: box_coords=(%d,%d) border-box=%dx%d\n",
				cx, cy, cw, ch);

		sprintf(expect,
			"globalThis.__t43e={x:%d,y:%d,w:%d,h:%d,cw:%d};",
			cx, cy, cw, ch,
			T43_SANE(bx->width) + bx->padding[LEFT] + bx->padding[RIGHT]);
		(void)js_exec(thread, (const unsigned char *)expect,
				strlen(expect), "t43-expect.js");

		{
			const char *probe =
				"globalThis.__t43={};var r=globalThis.__t43;"
				"var e=document.getElementById('feed');"
				"var rc=e.getBoundingClientRect();"
				"r.rx=rc.left;r.ry=rc.top;r.rw=rc.width;r.rh=rc.height;"
				"r.right=rc.right;r.bottom=rc.bottom;"
				"r.ow=e.offsetWidth;r.oh=e.offsetHeight;"
				"r.cw=e.clientWidth;"
				"r.rects=(e.getClientRects()||[]).length;"
				"var cs=getComputedStyle(e);"
				"r.disp=cs.display;"
				"r.width=cs.width;"
				"r.gpv=cs.getPropertyValue('display');"
				/* display:none must be reported from the CASCADE */
				"var h=document.createElement('div');"
				"document.getElementById('feed').appendChild(h);"
				"r.vw=window.innerWidth;r.vh=window.innerHeight;"
				"r.sx=window.scrollX;r.sy=window.scrollY;";
			ok = js_exec(thread, (const unsigned char *)probe,
					strlen(probe), "t43-probe.js");
			if (!ok) { fprintf(stderr, "FAIL: t43 probe threw\n"); return 1; }
		}
		{
			const char *chk =
				"var r=globalThis.__t43,x=globalThis.__t43e;"
				"if(r.rw!==x.w||r.rh!==x.h)"
					"throw new Error('ASSERT FAIL: getBoundingClientRect size "
						"is '+r.rw+'x'+r.rh+', the box tree says '+x.w+'x'+"
						"x.h+'. It returned all zeros before -- measuring "
						"code then computes garbage SILENTLY, with no error "
						"anywhere.');"
				"if(r.rx!==x.x||r.ry!==x.y)"
					"throw new Error('ASSERT FAIL: rect origin is ('+r.rx+','+"
						"r.ry+'), box_coords says ('+x.x+','+x.y+')');"
				"if(r.right!==r.rx+r.rw||r.bottom!==r.ry+r.rh)"
					"throw new Error('ASSERT FAIL: right/bottom inconsistent "
						"with left/top+size');"
				"if(r.ow!==x.w||r.oh!==x.h)"
					"throw new Error('ASSERT FAIL: offsetWidth/Height '+r.ow+"
						"'x'+r.oh+', expected '+x.w+'x'+x.h);"
				"if(r.cw!==x.cw)"
					"throw new Error('ASSERT FAIL: clientWidth is '+r.cw+', "
						"expected '+x.cw+' (padding in, border OUT -- that "
						"difference is the whole point of the property)');"
				"if(r.rects!==1)"
					"throw new Error('ASSERT FAIL: getClientRects gave '+"
						"r.rects);"
				"if(typeof r.disp!=='string'||!r.disp)"
					"throw new Error('ASSERT FAIL: computed display is '+"
						"r.disp+' -- getComputedStyle read only inline "
						"styles before, so display was undefined for "
						"everything styled by a sheet.');"
				"if(r.gpv!==r.disp)"
					"throw new Error('ASSERT FAIL: getPropertyValue(\"display\") "
						"gave \"'+r.gpv+'\" but .display gave \"'+r.disp+'\"');"
				"if(!/px$/.test(r.width))"
					"throw new Error('ASSERT FAIL: computed width is \"'+"
						"r.width+'\" -- must be a STRING WITH UNITS. A bare "
						"number makes parseInt succeed and string compares "
						"fail, producing NaN three lines later.');"
				"if(!(r.vw>0)||!(r.vh>0))"
					"throw new Error('ASSERT FAIL: viewport '+r.vw+'x'+r.vh);"
				"if(typeof r.sx!=='number'||typeof r.sy!=='number')"
					"throw new Error('ASSERT FAIL: scrollX/scrollY not "
						"numeric');"
				/* The sentinel must never reach script. */
				"if(r.rw>=1000000||r.rh>=1000000||r.ow>=1000000||"
				   "r.oh>=1000000||r.cw>=1000000)"
					"throw new Error('ASSERT FAIL: a layout SENTINEL leaked "
						"into JS (rect '+r.rw+'x'+r.rh+', offset '+r.ow+'x'+"
						"r.oh+'). Every box is born width=UNKNOWN_WIDTH="
						"INT_MAX and this fork zeroes a failed box height but "
						"never its width (fixes625) -- so an unresolved "
						"element would hand a page 2147483647 and measuring "
						"code computes nonsense from a plausible-looking "
						"number.');";
			ok = js_exec(thread, (const unsigned char *)chk,
					strlen(chk), "t43-chk.js");
			if (!ok) {
				fprintf(stderr, "FAIL: Test 43 -- layout still invisible\n");
				return 1;
			}
		}
		fprintf(stderr, "  rect/offset/client match the box tree; computed "
				"style reads the cascade with units\n");

		/* fixes1014 — THE UNSETTLED WINDOW. DOMContentLoaded fires from
		 * html_box_convert_done BEFORE content_set_ready, so every
		 * ready-time init on every page measures while the box tree cannot
		 * be trusted. The fixes1012 gate answered 0 there -- a fabricated
		 * real-looking number that scripts wrote back as inline width:0,
		 * which erased whole page sections (the fixes1011 hardware
		 * regression on hackaday/68kmla). The contract now: unsettled
		 * metrics are UNDEFINED (NaN-propagating, so a style write is a
		 * no-op -- the pre-1011 shape pages tolerated for years) and the
		 * rect is the LITERAL zero rect. This control goes red if the gate
		 * ever answers 0 again. */
		/* fixes1094 (#265 Round B) — RE-DRAIN, for the reason this test
		 * already documents above.
		 *
		 * The drain further up ran while status was DONE. Round B lets the
		 * synchronous flush run at READY too, so the probe below can now
		 * trigger its own reconvert -- which frees the box this test
		 * injected geometry into and hands back a fresh one whose birth
		 * width is UNKNOWN_WIDTH (qjs_sane -> 0). That is the feature
		 * working; without this drain the test measures the rebuild rather
		 * than the read path, and reports `READY offsetWidth is 0`.
		 *
		 * Drain with the mutations retired FIRST, then re-resolve and
		 * re-inject, so the probe's own flush is a no-op and every
		 * assertion below still means what it meant. */
		{
			extern int macos9_reconvert_flush_now(void *cv);
			htmlc.base.status = CONTENT_STATUS_DONE;
			(void) macos9_reconvert_flush_now((void *)&htmlc);
			(void) harness_pump_all(20000);
			bx = box_for_node((dom_node *)elf);
			if (bx == NULL) {
				fprintf(stderr, "FAIL: t43 #feed lost its box across the "
						"Round B re-drain\n");
				return 1;
			}
			bx->x = 11; bx->y = 22;
			bx->width = 640; bx->height = 480;
			bx->padding[LEFT] = bx->padding[RIGHT] = 0;
			bx->padding[TOP] = bx->padding[BOTTOM] = 0;
			bx->border[LEFT].width = bx->border[RIGHT].width = 0;
			bx->border[TOP].width = bx->border[BOTTOM].width = 0;
		}
		htmlc.base.status = CONTENT_STATUS_READY;
		{
			/* fixes1087 — CONTRACT NARROWED, not dropped.
			 *
			 * This asserted that CONTENT_STATUS_READY answers undefined
			 * for EVERYTHING. That was over-broad, and hackaday's
			 * featured slider is the bill: PAGEMAP shows it
			 * slick-initialized with 5 slides and the track collapsed to
			 * h=15, because slick sets .slick-list height from a
			 * measurement and every measurement it took during load was
			 * refused (declined=660, notdone 100%).
			 *
			 * What fixes1011 actually got wrong was fabricating 0 for an
			 * element with NO BOX -- scripts wrote that back as inline
			 * width:0 and erased page sections. That is still forbidden
			 * and still asserted below. Refusing to answer for an element
			 * that HAS a laid-out box was never what bought the safety;
			 * it was collateral, and it is what starves every
			 * measure-then-layout widget on the web. */
			const char *pre =
				"var e=document.getElementById('feed');"
				"if(typeof e.offsetWidth!=='number')"
					"throw new Error('ASSERT FAIL: READY offsetWidth is '+"
						"e.offsetWidth+' for an element WITH a box -- it "
						"must be measurable during load or every "
						"measure-then-layout widget gets nothing and "
						"collapses (slick, dotdotdot).');"
				"if(!(e.offsetWidth>0))"
					"throw new Error('ASSERT FAIL: READY offsetWidth is '+"
						"e.offsetWidth+', want the real box width');"
				"var rc=e.getBoundingClientRect();"
				"if(!(rc.width>0))"
					"throw new Error('ASSERT FAIL: READY rect is '+rc.width+"
						"'x'+rc.height+' for a boxed element');"
				/* The fixes1011 guard, unchanged in substance: an element
				 * with no box must NOT be handed a fabricated number. */
				"var d=document.createElement('div');"
				"if(d.offsetWidth!==undefined)"
					"throw new Error('ASSERT FAIL: an element with NO box "
						"answered '+d.offsetWidth+' before DONE -- must be "
						"undefined. A fabricated 0 gets written back as "
						"inline width:0 and ERASES page sections; undefined "
						"propagates as NaN and the write is a no-op.');"
				"if(d.clientHeight!==undefined||d.scrollWidth!==undefined)"
					"throw new Error('ASSERT FAIL: unboxed clientHeight/"
						"scrollWidth not undefined before DONE');";
			ok = js_exec(thread, (const unsigned char *)pre,
					strlen(pre), "t43-unsettled.js");
			htmlc.base.status = CONTENT_STATUS_DONE;
			if (!ok) {
				fprintf(stderr, "FAIL: Test 43 -- the unsettled window "
						"answered a fabricated value\n");
				return 1;
			}
		}
		fprintf(stderr, "  unsettled window answers undefined / zero-rect, "
				"never a fabricated 0\n");
	}
	fprintf(stderr, "=== Test 43 PASS: layout visible to JS ===\n");

	/* --- Test 44: the dotdotdot pattern (deep clone + re-append) --------
	 *
	 * hackaday's article entries reach the box tree EMPTY (kids=0). The
	 * removal audit cleared removeChild; the culprit is the jQuery
	 * dotdotdot plugin, whose core move is:
	 *
	 *     $inr = $dot.wrapInner('<div class="dotdotdot" />').children();
	 *     $inr.contents().detach().end().append( orgContent.clone(true) );
	 *
	 * i.e. EMPTY the element, then restore its content from a DEEP CLONE
	 * of what was there. If cloneNode(true) does not carry the subtree, or
	 * the clone cannot be appended, the content is destroyed -- which is
	 * exactly kids=0. Seven runs were logged on hardware, one per entry.
	 *
	 * Asserts the whole round trip, counting nodes at each step rather than
	 * testing a boolean, so a clone that is merely SHALLOW fails here. */
	fprintf(stderr, "\n=== Test 44: deep clone + re-append (the dotdotdot pattern) ===\n");
	{
		const char *src =
			"globalThis.__t44={};var r=globalThis.__t44;"
			"var host=document.getElementById('feed');"
			"var box=document.createElement('div');"
			"box.innerHTML='<span class=\"a\">one</span>"
				"<em>two</em><p>three</p>';"
			"host.appendChild(box);"
			"r.before=box.children.length;"
			/* snapshot the children the way jQuery does */
			"var org=[];for(var i=0;i<box.childNodes.length;i++)"
				"org.push(box.childNodes[i]);"
			"r.org=org.length;"
			/* deep-clone each, as orgContent.clone(true) does */
			"var clones=[];for(var j=0;j<org.length;j++)"
				"clones.push(org[j].cloneNode(true));"
			"r.clones=clones.length;"
			"r.cloneKids=0;"
			"for(var k=0;k<clones.length;k++){"
				"if(clones[k].childNodes)r.cloneKids+=clones[k].childNodes.length;}"
			/* empty it, exactly as jQuery .empty() does */
			"box.textContent='';"
			"r.afterEmpty=box.children.length;"
			/* restore from the clones */
			"for(var m=0;m<clones.length;m++)box.appendChild(clones[m]);"
			"r.restored=box.children.length;"
			"r.text=box.textContent||'';";
		unsigned char ok44 = js_exec(thread, (const unsigned char *)src,
				strlen(src), "t44.js");
		if (!ok44) { fprintf(stderr, "FAIL: t44 setup threw\n"); return 1; }
	}
	{
		const char *chk =
			"var r=globalThis.__t44;"
			"if(r.before!==3)throw new Error('ASSERT FAIL: setup built '+"
				"r.before+' children, expected 3');"
			"if(r.clones!==r.org)throw new Error('ASSERT FAIL: cloned '+"
				"r.clones+' of '+r.org+' nodes');"
			"if(r.cloneKids<3)throw new Error('ASSERT FAIL: the deep clones "
				"carry only '+r.cloneKids+' descendant nodes -- a SHALLOW "
				"clone. dotdotdot restores content from clone(true), so a "
				"shallow clone DELETES the page content it was restoring.');"
			"if(r.afterEmpty!==0)throw new Error('ASSERT FAIL: emptying "
				"left '+r.afterEmpty+' children');"
			"if(r.restored!==3)throw new Error('ASSERT FAIL: restored '+"
				"r.restored+' children from clones, expected 3 -- this is "
				"hackaday entry-intro kids=0 reproduced.');"
			"if(r.text.indexOf('one')<0||r.text.indexOf('three')<0)"
				"throw new Error('ASSERT FAIL: restored text is ['+r.text+"
					"'] -- the subtree did not survive the round trip');";
		unsigned char ok44b = js_exec(thread, (const unsigned char *)chk,
				strlen(chk), "t44-chk.js");
		if (!ok44b) {
			fprintf(stderr, "FAIL: Test 44 -- the dotdotdot round trip "
					"loses content\n");
			return 1;
		}
	}
	/* Part 2: the SAME round trip on PARSER-BUILT content. dotdotdot acts
	 * on markup that came from hubbub, not from script, and a clone path
	 * can easily work for one and not the other. */
	{
		const char *src2 =
			"globalThis.__t44b={};var r=globalThis.__t44b;"
			"var box=document.getElementById('lst');"   /* parser-built <ul> */
			"r.before=box.children.length;"
			"var org=[];for(var i=0;i<box.childNodes.length;i++)"
				"org.push(box.childNodes[i]);"
			"var clones=[];for(var j=0;j<org.length;j++)"
				"clones.push(org[j].cloneNode(true));"
			"r.cloneKids=0;"
			"for(var k=0;k<clones.length;k++){"
				"if(clones[k].childNodes)r.cloneKids+=clones[k].childNodes.length;}"
			"box.textContent='';"
			"r.afterEmpty=box.children.length;"
			"for(var m=0;m<clones.length;m++)box.appendChild(clones[m]);"
			"r.restored=box.children.length;"
			"r.text=box.textContent||'';";
		unsigned char o2 = js_exec(thread, (const unsigned char *)src2,
				strlen(src2), "t44b.js");
		if (!o2) { fprintf(stderr, "FAIL: t44b setup threw\n"); return 1; }
	}
	{
		const char *chk2 =
			"var r=globalThis.__t44b;"
			"if(r.before<2)throw new Error('ASSERT FAIL: parser subtree had '+"
				"r.before+' children');"
			"if(r.cloneKids<r.before)throw new Error('ASSERT FAIL: deep clones "
				"of PARSER-BUILT nodes carry only '+r.cloneKids+' descendants "
				"for '+r.before+' items -- shallow. This is the dotdotdot "
				"content loss.');"
			"if(r.restored!==r.before)throw new Error('ASSERT FAIL: restored '+"
				"r.restored+' of '+r.before+' parser-built children');"
			"if(r.text.indexOf('item')<0)throw new Error('ASSERT FAIL: restored "
				"text lost its content: ['+r.text+']');";
		unsigned char o2b = js_exec(thread, (const unsigned char *)chk2,
				strlen(chk2), "t44b-chk.js");
		if (!o2b) {
			fprintf(stderr, "FAIL: Test 44b -- parser-built content does not "
					"survive the dotdotdot round trip\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 44 PASS: deep clone + re-append preserves the subtree "
			"(script-built AND parser-built) ===\n");

	/* --- Test 45: the REAL dotdotdot, on a real article entry ----------
	 *
	 * Hardware shows every hackaday DIV.entry-intro reaching the box tree
	 * with kids=0, while the SAME page parsed and laid out here keeps them
	 * at h=181..262. The only ingredient the device adds is the page's own
	 * JavaScript, and the log names dotdotdot running exactly seven times
	 * -- once per entry -- immediately before. Rather than ask for another
	 * hardware round, run the actual plugin (extracted from hackaday's own
	 * bundle) against an entry of the same shape and see whether the
	 * content survives. This is the method that ended fixes998. */
	fprintf(stderr, "\n=== Test 45: the REAL dotdotdot plugin on an article entry ===\n");
	{
		FILE *bf = fopen("hackaday-bundle.js", "rb");
		if (bf == NULL) {
			fprintf(stderr, "SKIP: hackaday-bundle.js not present\n");
		} else {
			char *bsrc, *wrapped;
			long blen; size_t rd, wn;
			const char *pre = "globalThis.__ddErr='';try{";
			const char *post = "}catch(e){globalThis.__ddErr="
					"String((e&&e.message)||e);}";
			unsigned char ok45;

			fseek(bf, 0, SEEK_END); blen = ftell(bf); fseek(bf, 0, SEEK_SET);
			bsrc = (char *)malloc((size_t)blen + 1);
			rd = fread(bsrc, 1, (size_t)blen, bf);
			bsrc[rd] = '\0';
			fclose(bf);
			wn = strlen(pre) + rd + strlen(post) + 1;
			wrapped = (char *)malloc(wn);
			strcpy(wrapped, pre);
			memcpy(wrapped + strlen(pre), bsrc, rd);
			strcpy(wrapped + strlen(pre) + rd, post);
			ok45 = js_exec(thread, (const unsigned char *)wrapped,
					strlen(wrapped), "hackaday-bundle.js");
			fprintf(stderr, "js_exec(hackaday bundle, %ld bytes) ok=%d\n",
					blen, (int)ok45);
			free(bsrc); free(wrapped);

			{
				const char *setup =
					"globalThis.__t45={};var r=globalThis.__t45;"
					"r.err=globalThis.__ddErr||'';"
					"r.hasPlugin=(typeof jQuery!=='undefined'&&"
						"!!(jQuery.fn&&jQuery.fn.dotdotdot));"
					"if(r.hasPlugin){"
					  "var host=document.getElementById('feed');"
					  "var e=document.createElement('div');"
					  "e.className='entry-intro';"
					  "e.innerHTML='<a class=\"entries-image-holder\">"
						"<div class=\"entry-image\"></div></a>"
						"<h2><a>E-ink Writing Deck Rocks A Typewriter "
						"Aesthetic</a></h2>"
						"<div class=\"recent-post-meta\"><p>By Zoe</p></div>"
						"<p>Some excerpt text that is long enough to be "
						"truncated by a plugin that truncates things.</p>';"
					  "host.appendChild(e);"
					  "r.before=e.children.length;"
					  "r.textBefore=(e.textContent||'').length;"
					  "try{jQuery(e).dotdotdot();}catch(x){"
						"r.threw=String((x&&x.message)||x);}"
					  "r.after=e.children.length;"
					  "r.textAfter=(e.textContent||'').length;"
					  /* which jQuery primitive lost it? */
					  "var e2=document.createElement('div');"
					  "e2.innerHTML='<b>x</b><i>y</i>';"
					  "host.appendChild(e2);"
					  "r.contentsLen=jQuery(e2).contents().length;"
					  "r.childNodesLen=e2.childNodes.length;"
					  "r.cloneOK=(jQuery(e2).contents().clone(true)||[]).length;"
					  "jQuery(e2).wrapInner('<div class=\"w\" />');"
					  "r.afterWrap=e2.children.length;"
					  "r.wrapKids=e2.children[0]?e2.children[0].children.length:-1;"
					  /* the primitives jQuery.wrapAll depends on */
					  "var e3=document.createElement('div');"
					  "e3.innerHTML='<b>x</b><i>y</i>';host.appendChild(e3);"
					  "r.ownerDoc=(typeof e3.ownerDocument);"
					  "r.firstEC=(typeof e3.firstElementChild);"
					  "var w=document.createElement('div');"
					  "var fc=e3.childNodes[0];"
					  "r.fcParent=(fc&&fc.parentNode)?'yes':'no';"
					  "try{e3.insertBefore(w,fc);r.ibThrew='';}"
						"catch(x){r.ibThrew=String((x&&x.message)||x);}"
					  "r.ibKids=e3.children.length;"
					  "r.ibFirst=(e3.children[0]===w)?'wrapper':'other';"
					  /* does jQuery build the wrapper at all? */
					  "var t=document.createElement('div');"
					  "t.innerHTML='<div class=\"w\" />';"
					  "r.rawIH=t.childNodes.length;"
					  "r.rawIHkids=t.children.length;"
					  "r.jqMake=jQuery('<div class=\"w\" />').length;"
					  "r.jqMakeClone=jQuery('<div class=\"w\" />').eq(0)"
						".clone(true).length;"
					  "var mk=jQuery('<div class=\"w\" />');var nn=[];"
					  "for(var q=0;q<mk.length;q++){var n0=mk[q];"
						"nn.push((n0&&n0.nodeName)+':'+(n0&&n0.nodeType)+':k'+"
						"((n0&&n0.childNodes)?n0.childNodes.length:-1));}"
					  "r.mkNames=nn.join(',');"
					  /* wrapAll directly */
					  "var e4=document.createElement('div');"
					  "e4.innerHTML='<b>x</b><i>y</i>';host.appendChild(e4);"
					  "try{jQuery(e4).contents().wrapAll('<div class=\"w\" />');"
						"r.waThrew='';}catch(x){r.waThrew=String((x&&x.message)||x);}"
					  "r.waKids=e4.children.length;"
					  "r.waText=(e4.textContent||'').length;"
					"}";
				(void) js_exec(thread, (const unsigned char *)setup,
						strlen(setup), "t45-setup.js");
			}
			{
				const char *chk =
					"var r=globalThis.__t45;"
					"var D=' [DIAG contents='+r.contentsLen+' childNodes='+"
						"r.childNodesLen+' clone='+r.cloneOK+' afterWrap='+"
						"r.afterWrap+' wrapKids='+r.wrapKids+"
						"' ownerDoc='+r.ownerDoc+' firstElementChild='+r.firstEC+"
						"' fcParent='+r.fcParent+' insertBeforeThrew='+"
						"(r.ibThrew||'no')+' ibKids='+r.ibKids+"
						"' ibFirst='+r.ibFirst+' rawInnerHTMLkids='+r.rawIH+"
						"'/'+r.rawIHkids+' jQueryMake='+r.jqMake+"
						"' jQueryMakeClone='+r.jqMakeClone+' made=['+r.mkNames+']"
						" wrapAllKids='+r.waKids+' wrapAllText='+r.waText+"
						"' wrapAllThrew='+(r.waThrew||'no')+']';"
					"if(!r.hasPlugin)throw new Error('SKIP-AS-FAIL: the "
						"bundle did not register jQuery.fn.dotdotdot; "
						"bundle error was ['+r.err+']');"
					"if(r.threw)throw new Error('ASSERT FAIL: dotdotdot threw: '"
						"+r.threw);"
					"if(r.after===0)throw new Error('ASSERT FAIL: dotdotdot "
						"EMPTIED the entry (children '+r.before+' -> 0). This "
						"is hackaday entry-intro kids=0, reproduced locally.'+D);"
					"if(r.textAfter===0)throw new Error('ASSERT FAIL: dotdotdot "
						"removed all text ('+r.textBefore+' chars -> 0)');";
				unsigned char ok45b = js_exec(thread,
						(const unsigned char *)chk, strlen(chk),
						"t45-chk.js");
				if (!ok45b) {
					fprintf(stderr, "FAIL: Test 45 -- the real dotdotdot "
							"destroys the entry\n");
					return 1;
				}
			}
		}
	}
	fprintf(stderr, "=== Test 45 PASS: the real dotdotdot preserves the entry ===\n");

	/* --- Test 54: attribute selectors, and the REAL slick.js that needs
	 * them -----------------------------------------------------------
	 *
	 * Root-caused the "hackaday featured slider doesn't load at all"
	 * report: qjs_sel_parse (the hand-rolled matcher behind
	 * querySelectorAll/jQuery's qsa fast path) swallowed `[...]`
	 * attribute selectors whole and fell back to whatever tag/class/id
	 * preceded them. `img[data-lazy]` therefore degraded to bare `img`
	 * and matched EVERY image, lazy or not. hackaday's own theme script
	 * (in the harness's real hackaday-bundle.js) hits this directly:
	 * slick's init -> setSlideClasses -> lazyLoad -> loadImages selects
	 * `img[data-lazy]` and calls jQuery's deprecated `.load(fn)` event
	 * shorthand on each match -- removed in jQuery 3.x, where `.load()`
	 * is ONLY the AJAX method, so a bare function argument throws
	 * (`e.indexOf is not a function`) inside jQuery itself. Confirmed
	 * with rawH=0/no-op images BEFORE the fix: the throw aborted the
	 * whole `.slick()` call synchronously, so the theme's IIFE never
	 * even reached the slider's own setup, let alone anything after it
	 * in the same script. fixes1090c parses [attr]/[attr=val]/
	 * [attr~=val]/[attr^=val]/[attr$=val]/[attr*=val] for real instead
	 * of discarding them. */
	fprintf(stderr, "\n=== Test 54: attribute selectors + real slick.js ===\n");
	{
		const char *setup =
			"globalThis.__t54={};var r=globalThis.__t54;"
			"var host=document.getElementById('feed');"
			"var m1=document.createElement('div');"
			"m1.innerHTML='<img id=\"withattr\" data-lazy=\"x.png\">"
				"<img id=\"noattr\">"
				"<a id=\"pfx\" href=\"https://example.com/x\"></a>"
				"<a id=\"nopfx\" href=\"http://example.com/x\"></a>';"
			"host.appendChild(m1);"
			"r.qsaPresence=m1.querySelectorAll('img[data-lazy]').length;"
			"r.qsaEquals=m1.querySelectorAll('img[id=\"withattr\"]').length;"
			"r.qsaPrefix=m1.querySelectorAll("
				"'a[href^=\"https://\"]').length;"
			"r.jqPresence=jQuery('img[data-lazy]',m1).length;"
			"var fs=document.createElement('div');"
			"fs.className='featured-slides';"
			"for(var i=0;i<3;i++){"
			"  var s=document.createElement('div');"
			"  s.innerHTML='<a><img></a><h2>Story '+i+'</h2>';"
			"  fs.appendChild(s);"
			"}"
			"host.appendChild(fs);"
			"r.lazyImgLenBefore=jQuery('img[data-lazy]',fs).length;"
			"try{"
			"  jQuery('.featured-slides').slick({autoplay:true,speed:300,"
			"    arrows:false,dots:true,pauseOnHover:true});"
			"  r.slickThrew='';"
			"}catch(e){r.slickThrew=String((e&&e.message)||e);}"
			"r.slickInitialized=jQuery('.featured-slides')"
				".hasClass('slick-initialized');";
		unsigned char ok54 = js_exec(thread, (const unsigned char *)setup,
				strlen(setup), "t54-setup.js");
		if (!ok54) {
			fprintf(stderr, "FAIL: Test 54 -- setup script itself threw\n");
			return 1;
		}
		{
			const char *chk =
				"var r=globalThis.__t54;"
				"if(r.qsaPresence!==1)throw new Error('ASSERT FAIL: "
					"img[data-lazy] matched '+r.qsaPresence+"
					"' images, want 1 (the one WITH the attribute) -- "
					"attribute-presence selectors are not filtering');"
				"if(r.qsaEquals!==1)throw new Error('ASSERT FAIL: "
					"img[id=\"withattr\"] matched '+r.qsaEquals+"
					"', want 1');"
				"if(r.qsaPrefix!==1)throw new Error('ASSERT FAIL: "
					"a[href^=\"https://\"] matched '+r.qsaPrefix+"
					"', want 1 (only the https link)');"
				"if(r.jqPresence!==1)throw new Error('ASSERT FAIL: "
					"jQuery(\"img[data-lazy]\") matched '+r.jqPresence+"
					"', want 1 -- jQuery relies on the same native "
					"matcher');"
				"if(r.lazyImgLenBefore!==0)throw new Error('ASSERT "
					"FAIL: img[data-lazy] inside .featured-slides "
					"matched '+r.lazyImgLenBefore+' plain <img> "
					"elements with no data-lazy attribute at all');"
				"if(r.slickThrew)throw new Error('ASSERT FAIL: the "
					"REAL slick.js threw during init: '+r.slickThrew+"
					"' -- this is what aborted hackaday\\'s theme "
					"script before the slider (or anything after it "
					"in the same IIFE) ever ran');"
				"if(!r.slickInitialized)throw new Error('ASSERT FAIL: "
					"slick did not reach slick-initialized');";
			unsigned char ok54b = js_exec(thread, (const unsigned char *)chk,
					strlen(chk), "t54-chk.js");
			if (!ok54b) {
				fprintf(stderr, "FAIL: Test 54 -- attribute selectors "
						"still broken, or the real slider still "
						"throws\n");
				return 1;
			}
		}
	}
	fprintf(stderr, "=== Test 54 PASS: attribute selectors match for real; "
			"hackaday's own slick.js init no longer throws ===\n");

	/* --- Test 55: the fixes1093 SLIDER PROBE actually walks ---------------
	 *
	 * A diagnostic that silently prints nothing is worse than none: it
	 * reads as "the widget is fine" when it means "the probe is broken",
	 * and this project has now lost rounds to exactly that (fixes1090's
	 * compiled-out resize, fixes1022's defines-below-uses). Test 54 leaves
	 * a real .featured-slides subtree in the document, so drive the probe
	 * over it and assert it emits subtree lines rather than its
	 * NOT-IN-DOM branch. "ready" resets the probe's per-nav budget, which
	 * Test 54's own reconverts have otherwise exhausted by now. */
	fprintf(stderr, "\n=== Test 55: the slider probe walks a real subtree ===\n");
	{
		extern void html_slider_probe(html_content *c, const char *when);
		extern long macsurf_probe_slider_lines; /* fixes1093 counter */
		long before = macsurf_probe_slider_lines;
		html_slider_probe(&htmlc, "ready");
		if (macsurf_probe_slider_lines <= before) {
			fprintf(stderr, "FAIL: Test 55 -- the slider probe emitted "
					"NO subtree lines over a .featured-slides that "
					"Test 54 demonstrably built. The probe would have "
					"gone to hardware printing nothing and been read "
					"as 'the slider is fine'.\n");
			return 1;
		}
		fprintf(stderr, "  probe emitted %ld subtree lines\n",
				macsurf_probe_slider_lines - before);
	}
	fprintf(stderr, "=== Test 55 PASS: the slider probe reaches the subtree ===\n");

	/* --- Test 56: WHAT THE fixes421 active-GUARD ACTUALLY PROTECTS (#265 A) --
	 *
	 * html_reconvert refuses outright while `c->base.active > 0`, citing a
	 * use-after-free: html_object_callback holds a pw into object_list
	 * entries that html_object_free_objects is about to free. That guard is
	 * blocker #2 for #265 (synchronous layout on measure), and it is the one
	 * with a real crash behind it, so it cannot be deleted on a reading.
	 *
	 * Test 31 already characterises the partition for entries with NO fetch
	 * outstanding (content == NULL): image+resolvable-box kept, node-gone and
	 * non-image retired. It cannot answer the #265 question, because the
	 * entry that matters is the one with a LIVE fetch -- box == NULL and
	 * content != NULL -- which is the fixes976 "carry, do not retire" branch
	 * and the exact shape base.active counts.
	 *
	 * If an in-flight IMAGE is CARRIED, a reconvert cannot free it, so the
	 * guard does not need to block on it -- and images are essentially all of
	 * base.active on a real page, which is the difference between #265 being
	 * reachable and not.
	 *
	 * HARNESS LIMIT, stated because it bounds the claim: hlcache is never
	 * initialised here, so actually RELEASING a handle dereferences the NULL
	 * hlcache global. The in-flight entry therefore carries a non-NULL dummy
	 * handle purely to select the fixes976 branch; if the partition instead
	 * DROPPED it, release would run and this test dies loudly rather than
	 * silently passing. The non-image control keeps content == NULL so its
	 * (expected) retire stays on the safe path. This proves carry-vs-drop,
	 * NOT that release itself is UAF-free -- that needs a live fetcher and is
	 * Round B. */
	fprintf(stderr, "\n=== Test 56: in-flight objects across a reconvert (#265 Round A) ===\n");
	{
		extern struct box *box_for_node(dom_node *n);
		struct content_html_object *img_flight, *nonimg, *w;
		struct content_html_object *saved_list = htmlc.object_list;
		unsigned int saved_n = htmlc.num_objects;
		void *dummy_handle = calloc(1, 256);
		int saw_flight = 0, saw_nonimg = 0, n = 0;

		img_flight = calloc(1, sizeof(*img_flight));
		nonimg     = calloc(1, sizeof(*nonimg));
		if (img_flight == NULL || nonimg == NULL || dummy_handle == NULL) {
			fprintf(stderr, "FAIL: Test 56 calloc\n"); return 1;
		}

		/* (1) IMAGE still fetching: no box yet, handle outstanding. This is
		 *     what base.active is counting on a loading page. */
		img_flight->parent = (struct content *)&htmlc;
		img_flight->box = NULL;
		img_flight->permitted_types = CONTENT_IMAGE;
		img_flight->content = (struct hlcache_handle *)dummy_handle;

		/* (2) non-image control: must still be retired (CONTENT_ANY). */
		nonimg->parent = (struct content *)&htmlc;
		nonimg->box = NULL;
		nonimg->permitted_types = CONTENT_ANY;
		nonimg->content = NULL;

		img_flight->next = nonimg;
		nonimg->next = NULL;
		htmlc.object_list = img_flight;
		htmlc.num_objects = 2;

		macsurf_js_set_reconvert_enabled(1);
		(void) html_reconvert_content((struct content *)&htmlc);
		(void) harness_pump_all(20000);

		for (w = htmlc.object_list; w != NULL; w = w->next) {
			if (w == img_flight) saw_flight = 1;
			if (w == nonimg)     saw_nonimg = 1;
			if (++n > 4096) break;
		}
		fprintf(stderr, "  in-flight IMAGE  survived: %s\n",
				saw_flight ? "YES (carried)" : "NO (dropped+freed)");
		fprintf(stderr, "  non-image        survived: %s\n",
				saw_nonimg ? "yes" : "no (retired, as expected)");

		if (!saw_flight) {
			fprintf(stderr, "FAIL: Test 56 -- an in-flight IMAGE was dropped "
				"by the reconvert. The fixes421 active-guard is then "
				"load-bearing for images, which dominate base.active, and "
				"#265 cannot narrow it. Round A answer: guard must stay.\n");
			return 1;
		}
		if (saw_nonimg) {
			fprintf(stderr, "FAIL: Test 56 -- the non-image control was NOT "
				"retired, so this run is not exercising the partition at "
				"all and the in-flight result above is meaningless.\n");
			return 1;
		}
		fprintf(stderr, "  => in-flight IMAGE entries are CARRIED, not freed: "
				"the active-guard need not block on image fetches\n");

		htmlc.object_list = saved_list;
		htmlc.num_objects = saved_n;
		/* We own img_flight again now that it is off the list -- free it
		 * and its dummy handle directly rather than through
		 * html_object_free_objects, which would try to release the handle
		 * against the uninitialised harness hlcache. Keeps the tracked
		 * 42-allocation leak baseline honest. */
		free(dummy_handle);
		free(img_flight);
	}
	fprintf(stderr, "=== Test 56 PASS: in-flight images survive a reconvert ===\n");

	/* --- Test 57: the sync flush must actually FIRE at READY (#265 Round B) --
	 *
	 * Round B's whole claim is that geometry can be answered before the load
	 * finishes. Hardware measured the opposite on hackaday --
	 * `JSSYNC flush=0 declined=630`, `JSSYNCWHY notdone=630` -- i.e. the
	 * forced-layout path built in fixes1073 had never once run, because both
	 * gates demanded CONTENT_STATUS_DONE.
	 *
	 * A flush counter that stays 0 is indistinguishable from a feature that
	 * is compiled out, which is precisely how the fixes1019 resize hid for
	 * ten rounds. So assert the COUNT, not a boolean: with a mutation
	 * outstanding and status READY, a flush must happen.
	 *
	 * Also asserts the narrowed active-guard from Round A in the same run: an
	 * in-flight IMAGE must NOT block the flush (it is carried, never freed),
	 * while an in-flight NON-image must (it is dropped and freed). */
	fprintf(stderr, "\n=== Test 57: sync flush fires at READY (#265 Round B) ===\n");
	{
		extern void macos9_reconvert_sync_stats(long *f, long *d, long *us);
		extern void macos9_reconvert_sync_reset(void);
		extern int macos9_reconvert_flush_now(void *cv);
		extern void macos9_js_mark_dom_dirty(struct content *c);
		struct content_html_object *blocker;
		struct content_html_object *saved_list = htmlc.object_list;
		unsigned int saved_n = htmlc.num_objects;
		void *dummy_handle;
		long f0 = 0, d0 = 0, u0 = 0, f1 = 0, d1 = 0, u1 = 0;

		htmlc.reflowing = false;
		htmlc.box_conversion_context = NULL;
		htmlc.aborted = false;

		/* (a) READY + a pending mutation + NO objects -> must FLUSH. */
		htmlc.base.status = CONTENT_STATUS_READY;
		htmlc.base.active = 1;          /* the html fetch itself, as on a real load */
		htmlc.object_list = NULL;
		htmlc.num_objects = 0;
		macos9_js_mark_dom_dirty((struct content *)&htmlc);
		macos9_reconvert_sync_reset();
		macos9_reconvert_sync_stats(&f0, &d0, &u0);
		(void) macos9_reconvert_flush_now((void *)&htmlc);
		macos9_reconvert_sync_stats(&f1, &d1, &u1);
		fprintf(stderr, "  READY + active=1, no objects: flush %ld -> %ld, "
				"declined %ld -> %ld\n", f0, f1, d0, d1);
		if (f1 <= f0) {
			fprintf(stderr, "FAIL: Test 57 -- no flush at READY. Round B "
				"changed nothing: geometry is still dead for the whole "
				"load window, which is when every widget measures.\n");
			return 1;
		}

		/* (b) an in-flight NON-image must still block (Round A: it is the
		 *     one entry class a reconvert really does free). */
		dummy_handle = calloc(1, 256);
		blocker = calloc(1, sizeof(*blocker));
		if (blocker == NULL || dummy_handle == NULL) {
			fprintf(stderr, "FAIL: Test 57 calloc\n"); return 1;
		}
		blocker->parent = (struct content *)&htmlc;
		blocker->permitted_types = CONTENT_ANY;
		blocker->content = (struct hlcache_handle *)dummy_handle;
		blocker->next = NULL;
		htmlc.object_list = blocker;
		htmlc.num_objects = 1;
		macos9_js_mark_dom_dirty((struct content *)&htmlc);
		macos9_reconvert_sync_reset();
		macos9_reconvert_sync_stats(&f0, &d0, &u0);
		(void) macos9_reconvert_flush_now((void *)&htmlc);
		macos9_reconvert_sync_stats(&f1, &d1, &u1);
		fprintf(stderr, "  READY + in-flight NON-image: flush %ld -> %ld, "
				"declined %ld -> %ld\n", f0, f1, d0, d1);
		if (f1 > f0) {
			fprintf(stderr, "FAIL: Test 57 -- flushed with an in-flight "
				"non-image object present. That entry is DROPPED and "
				"FREED by the partition while html_object_callback may "
				"still hold it: the fixes421 use-after-free, re-opened.\n");
			return 1;
		}
		if (d1 <= d0) {
			fprintf(stderr, "FAIL: Test 57 -- neither flushed nor declined; "
				"the guard is not being reached at all, so this case "
				"proves nothing.\n");
			return 1;
		}
		fprintf(stderr, "  => flushes at READY, still defers for the entry "
				"class that is genuinely freed\n");

		htmlc.object_list = saved_list;
		htmlc.num_objects = saved_n;
		htmlc.base.status = CONTENT_STATUS_DONE;
		htmlc.base.active = 0;
		free(blocker);
		free(dummy_handle);
	}
	fprintf(stderr, "=== Test 57 PASS: flush fires at READY, guard still covers the hazard ===\n");

	/* --- Test 58: the phase timer must ATTRIBUTE, not just emit (#265 C1) ---
	 *
	 * Round C1 exists to answer one question: of the ~1.7s a synchronous
	 * reconvert costs, which phase owns it? That answer decides whether Round
	 * C is "make a flush cheaper" or "make flushes rarer" -- opposite work --
	 * so a timer that reports plausible-but-wrong numbers is worse than none.
	 *
	 * Two properties, both asserted as COUNTS:
	 *   1. the phases do not EXCEED the wall-clock of the reconvert that
	 *      produced them. `build` brackets the synchronous
	 *      html_reconvert_done, so relink/freeold/reformat run INSIDE it and
	 *      are subtracted back out; drop that subtraction and their time is
	 *      counted twice, making the sum overshoot real elapsed time. Note
	 *      the report line's own `total=` is computed AS the sum of parts, so
	 *      "parts sum to total" is true by construction and proves nothing --
	 *      an external clock is the only thing that can catch this;
	 *   2. `build` is non-zero after a real reconvert -- if the instrument
	 *      were not wired into the dom_to_box path at all, every bucket but
	 *      that one would still look sane and the line would still print. */
	fprintf(stderr, "\n=== Test 58: reconvert phase attribution (#265 Round C1) ===\n");
	{
		extern void html_reconvert_phase_stats(int ph, long *us, long *n);
		extern void html_reconvert_phase_reset(void);
		static const char *nm[8] = { "h1","h3","css","pin","build",
					     "relink","freeold","reformat" };
		long before_n = 0, after_n = 0;
		long v[8], v0[8];
		long sum = 0, sum0 = 0, delta = 0, wall = 0;
		struct timespec ts0, ts1;
		int i;

		for (i = 0; i < 8; i++) {
			html_reconvert_phase_stats(i, &v0[i], NULL);
			sum0 += v0[i];
		}
		html_reconvert_phase_stats(0, NULL, &before_n);
		htmlc.base.status = CONTENT_STATUS_DONE;
		htmlc.base.active = 0;
		htmlc.reflowing = false;
		htmlc.box_conversion_context = NULL;
		htmlc.aborted = false;
		clock_gettime(CLOCK_MONOTONIC, &ts0);
		(void) html_reconvert_content((struct content *)&htmlc);
		(void) harness_pump_all(20000);
		clock_gettime(CLOCK_MONOTONIC, &ts1);
		wall = (long)((ts1.tv_sec - ts0.tv_sec) * 1000000L +
				(ts1.tv_nsec - ts0.tv_nsec) / 1000L);
		html_reconvert_phase_stats(0, NULL, &after_n);

		if (after_n <= before_n) {
			fprintf(stderr, "FAIL: Test 58 -- the reconvert counter did not "
				"advance, so no reconvert ran and the phase numbers below "
				"describe nothing.\n");
			return 1;
		}
		for (i = 0; i < 8; i++) {
			html_reconvert_phase_stats(i, &v[i], NULL);
			if (v[i] < 0) {
				fprintf(stderr, "FAIL: Test 58 -- phase %d out of range\n", i);
				return 1;
			}
			sum += v[i];
		}
		for (i = 0; i < 8; i++)
			fprintf(stderr, "  %-8s %ldus\n", nm[i], v[i]);
		delta = sum - sum0;
		fprintf(stderr, "  n=%ld sum=%ldus  (this reconvert: phases=%ldus "
				"wall=%ldus)\n", after_n, sum, delta, wall);

		/* Double-counting shows up as phases exceeding real elapsed time.
		 * Slack is generous because wall includes harness_pump_all and ASan
		 * overhead OUTSIDE the instrumented phases, which can only make wall
		 * larger -- so an overshoot is a genuine signal, not noise. */
		if (wall > 0 && delta > wall) {
			fprintf(stderr, "FAIL: Test 58 -- phases total %ldus for a "
				"reconvert that took %ldus of wall clock. Time is being "
				"counted twice: relink/freeold/reformat run inside the "
				"build bracket and must be subtracted back out of it.\n",
				delta, wall);
			return 1;
		}

		if (v[4] <= 0) {
			fprintf(stderr, "FAIL: Test 58 -- build=0 after a real reconvert. "
				"dom_to_box is the single largest cost on hardware and the "
				"instrument is not measuring it, so Round C would be "
				"optimising whichever bucket happened to be non-zero.\n");
			return 1;
		}
		if (sum <= 0) {
			fprintf(stderr, "FAIL: Test 58 -- every phase is zero; the clock "
				"is not being read (cf. fixes1070, where an int-returning "
				"macos9_micros made every harness timing garbage).\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 58 PASS: phases attribute and sum ===\n");

	/* --- Test 59: measuring during LOADING, the case the header needs (C3) --
	 *
	 * This is the ONLY case that fixes hackaday's featured slider, and neither
	 * Round B nor C1 touches it. The theme runs at end-of-body -- hardware has
	 * the bundle at tick 48579 and domready at 49060 -- so it measures while
	 * status is LOADING and c->layout is still NULL. Geometry answers
	 * undefined there, jQuery's `parseFloat(x)||0` turns that into 0, and the
	 * theme bakes `height:0px` onto every .slick-slide in a one-shot onInit
	 * that never runs again. Every log since fixes1093 shows exactly that.
	 *
	 * A real browser answers truly because offsetHeight forces layout of what
	 * has been parsed so far. We can do the same: html_reconvert already
	 * builds a first tree from scratch (this harness does it 59 times a run
	 * with layout=(nil)), and html_reconvert_done touches no content status,
	 * so it is not structurally unsafe mid-parse.
	 *
	 * Written RED first, deliberately: with the gates as Round B left them
	 * this must fail, because LOADING is refused. If it passes before the gate
	 * changes then it is not testing what it claims. */
	fprintf(stderr, "\n=== Test 59: geometry during LOADING (#265 Round C3) ===\n");
	{
		extern int macos9_reconvert_flush_now(void *cv);
		extern void macos9_reconvert_sync_stats(long *f, long *d, long *us);
		extern void macos9_reconvert_sync_reset(void);
		extern void macos9_js_mark_dom_dirty(struct content *c);
		long f0 = 0, d0 = 0, u0 = 0, f1 = 0, d1 = 0, u1 = 0;

		htmlc.reflowing = false;
		htmlc.box_conversion_context = NULL;
		htmlc.aborted = false;
		htmlc.object_list = NULL;
		htmlc.num_objects = 0;

		/* Exactly the shape hardware reports: mid-parse, no tree yet,
		 * the html fetch itself still counted active. */
		htmlc.base.status = CONTENT_STATUS_LOADING;
		htmlc.base.active = 1;
		macos9_js_mark_dom_dirty((struct content *)&htmlc);

		macos9_reconvert_sync_reset();
		macos9_reconvert_sync_stats(&f0, &d0, &u0);
		(void) macos9_reconvert_flush_now((void *)&htmlc);
		macos9_reconvert_sync_stats(&f1, &d1, &u1);

		fprintf(stderr, "  LOADING + layout=NULL: flush %ld -> %ld, "
				"declined %ld -> %ld\n", f0, f1, d0, d1);

		htmlc.base.status = CONTENT_STATUS_DONE;
		htmlc.base.active = 0;

		if (f1 <= f0) {
			fprintf(stderr, "FAIL: Test 59 -- no flush during LOADING. This "
				"is the window every end-of-body script measures in, so "
				"geometry still answers undefined there, jQuery still "
				"coerces it to 0, and hackaday's slider still gets "
				"height:0px. Round B/C1 do not reach this case.\n");
			return 1;
		}
		fprintf(stderr, "  => a script measuring mid-parse can be answered "
				"from a freshly built tree\n");
	}
	fprintf(stderr, "=== Test 59 PASS: LOADING-window geometry is reachable ===\n");

	/* --- Test 60: measuring during LOADING must ANSWER, not just flush -----
	 *
	 * Test 59 asserts a flush HAPPENS during LOADING. That was necessary and
	 * not sufficient, and shipping on it cost a hardware round: fixes1096
	 * opened the two gates that BUILD a tree and hardware came back
	 * notdone=0, flush=155 -- and JSGEOMANS real=2, unchanged. 155 trees were
	 * built and every reading was discarded, because a THIRD gate
	 * (macsurf_html_tree_stable, which decides whether an answer may be
	 * given) still demanded READY. The count that used to be `notdone`
	 * simply became `unstable`.
	 *
	 * So assert the thing the page actually depends on: a script measuring a
	 * boxed element mid-parse gets a REAL NUMBER. A flush counter cannot see
	 * this failure -- it was 155 and everything was still broken. */
	fprintf(stderr, "\n=== Test 60: LOADING answers a real number (#265 C3b) ===\n");
	{
		extern void macos9_js_mark_dom_dirty(struct content *c);
		unsigned char ok;

		htmlc.reflowing = false;
		htmlc.box_conversion_context = NULL;
		htmlc.aborted = false;
		htmlc.object_list = NULL;
		htmlc.num_objects = 0;
		htmlc.base.status = CONTENT_STATUS_DONE;
		htmlc.base.active = 0;
		(void) html_reconvert_content((struct content *)&htmlc);
		(void) harness_pump_all(20000);

		/* Now put the content back into the shape a mid-parse script sees:
		 * LOADING, html fetch active, a tree present from the build above. */
		htmlc.base.status = CONTENT_STATUS_LOADING;
		htmlc.base.active = 1;
		{
			struct box *fb;
			dom_string *idf = NULL;
			dom_element *elf = NULL;
			if (dom_string_create((const uint8_t *)"feed", 4, &idf)
					!= DOM_NO_ERR) {
				fprintf(stderr, "FAIL: t60 dom_string_create\n"); return 1;
			}
			dom_document_get_element_by_id(document, idf, &elf);
			dom_string_unref(idf);
			if (elf == NULL) {
				fprintf(stderr, "FAIL: t60 missing #feed\n"); return 1;
			}
			fb = box_for_node((dom_node *)elf);
			if (fb == NULL) {
				fprintf(stderr, "FAIL: t60 #feed has no box after the "
						"build, so this test cannot mean anything\n");
				return 1;
			}
			fb->x = 5; fb->y = 7; fb->width = 321; fb->height = 123;
			fb->padding[LEFT] = fb->padding[RIGHT] = 0;
			fb->padding[TOP] = fb->padding[BOTTOM] = 0;
			fb->border[LEFT].width = fb->border[RIGHT].width = 0;
			fb->border[TOP].width = fb->border[BOTTOM].width = 0;
		}

		{
			const char *chk =
				"var e=document.getElementById('feed');"
				"var w=e.offsetWidth;"
				"if(typeof w!=='number')"
					"throw new Error('ASSERT FAIL: mid-parse offsetWidth is '"
						"+w+' (typeof '+(typeof w)+') for an element WITH a "
						"box. jQuery turns that into a hard 0 via "
						"parseFloat(x)||0, which is exactly how the hackaday "
						"theme bakes height:0px onto every slide.');"
				"if(w!==321)"
					"throw new Error('ASSERT FAIL: mid-parse offsetWidth is '"
						"+w+', want the real box width 321');";
			ok = js_exec(thread, (const unsigned char *)chk,
					strlen(chk), "t60.js");
		}
		htmlc.base.status = CONTENT_STATUS_DONE;
		htmlc.base.active = 0;
		if (!ok) {
			fprintf(stderr, "FAIL: Test 60 -- a script measuring during "
				"LOADING still gets no real answer. Building the tree is "
				"not enough; the gate that decides whether an answer may "
				"be GIVEN must accept a LOADING document that has one.\n");
			return 1;
		}
		fprintf(stderr, "  => mid-parse measurement returns the real box "
				"width, not undefined\n");
	}
	fprintf(stderr, "=== Test 60 PASS: LOADING geometry answers truly ===\n");

	/* --- Test 61: XHR RESPONSE HEADERS SURVIVE THE ACCUMULATOR ----------
	 *
	 * Both fetchers emit ONE bare header line per FETCH_HEADER: find_line()
	 * / mfs_find_line() NUL each line's own '\r' and send len=strlen(p), so
	 * no terminator ever reaches the callback. macos9_js_fetch.c's
	 * xhr_accum() is a pure byte-append, so before fixes1098 every header
	 * fused into ONE line starting with the "HTTP/1.1 200 OK" status line.
	 * The prelude's getResponseHeader() splits on /\r\n|\n/, found a single
	 * line, and compared the name against "HTTP/1.1 200 OK" -- so EVERY
	 * lookup returned null and getAllResponseHeaders() returned one mash.
	 *
	 * null reads as "that header isn't present", not as an error, so a
	 * caller silently takes its no-such-header branch. That is the LYING
	 * ANSWER shape from the fixes1005->1031 batch, not a missing API.
	 *
	 * Assert COUNTS and exact values, never truthiness: a boolean cannot
	 * tell a correctly-parsed header from a mashed one that happens to be
	 * non-empty. The control is the pre-fix byte-stream (no terminators),
	 * which MUST still parse to zero recoverable headers. */
	fprintf(stderr, "\n=== Test 61: XHR response headers survive accum ===\n");
	{
		unsigned char ok;
		char accum_js[2048];
		const char *hdr_lines[4];
		const char *raw;
		const char *p;
		char esc[1024];
		size_t ei = 0;

		extern const char *macos9_js_fetch_test_accum_headers(
				const char *const *lines, int nlines);

		/* EXACTLY what both fetchers emit: one BARE line per FETCH_HEADER,
		 * '\r' already NUL'd and len=strlen(p). No terminators. */
		hdr_lines[0] = "HTTP/1.1 200 OK";
		hdr_lines[1] = "Content-Type: application/json; charset=utf-8";
		hdr_lines[2] = "X-Frame-Options: SAMEORIGIN";
		hdr_lines[3] = "Set-Cookie: sid=abc; HttpOnly";

		/* Drive the REAL shipping accumulator, not a reimplementation.
		 * This is the whole point of the test: a version that builds the
		 * header string in JS passes with OR without the fix. */
		raw = macos9_js_fetch_test_accum_headers(hdr_lines, 4);

		for (p = raw; *p != '\0' && ei + 4 < sizeof(esc); p++) {
			if (*p == '\r')      { esc[ei++] = '\\'; esc[ei++] = 'r'; }
			else if (*p == '\n') { esc[ei++] = '\\'; esc[ei++] = 'n'; }
			else if (*p == '\'') { esc[ei++] = '\\'; esc[ei++] = '\''; }
			else if (*p == '\\') { esc[ei++] = '\\'; esc[ei++] = '\\'; }
			else                 { esc[ei++] = *p; }
		}
		esc[ei] = '\0';

		fprintf(stderr, "  accumulator produced %lu bytes\n",
				(unsigned long) strlen(raw));

		snprintf(accum_js, sizeof(accum_js),
			"var fixed='%s';"
			/* The control: the same headers with terminators stripped,
			 * i.e. the exact pre-fixes1098 accumulator output. */
			"var broken='HTTP/1.1 200 OK'+"
				"'Content-Type: application/json; charset=utf-8'+"
				"'X-Frame-Options: SAMEORIGIN'+"
				"'Set-Cookie: sid=abc; HttpOnly';"
			"function mk(raw){var x=new XMLHttpRequest();"
				"x.__responseHeadersRaw=raw;return x;}"
			"var f=mk(fixed), b=mk(broken);", esc);
		{
		char full_js[4096];
		const char *chk =

			/* 1. Case-insensitive lookup returns the EXACT value. */
			"var ct=f.getResponseHeader('content-type');"
			"if(ct!=='application/json; charset=utf-8')"
				"throw new Error('ASSERT FAIL: content-type is '+ct+', want "
					"the exact value. A null here is the lying answer: the "
					"caller reads it as header-absent and takes its fallback "
					"branch without ever erroring.');"

			/* 2. Every distinct header is individually recoverable --
			 *    a count, so a fused string cannot pass by being truthy. */
			"var names=['content-type','x-frame-options','set-cookie'],"
				"got=0,i;"
			"for(i=0;i<names.length;i++)"
				"if(f.getResponseHeader(names[i])!==null)got++;"
			"if(got!==3)"
				"throw new Error('ASSERT FAIL: recovered '+got+'/3 headers. "
					"Fewer than 3 means lines fused and only the first "
					"name is addressable.');"

			/* 3. The status line is NOT addressable as a header name. */
			"if(f.getResponseHeader('HTTP/1.1 200 OK')!==null)"
				"throw new Error('ASSERT FAIL: the status line parsed as a "
					"header -- the split is not landing on line boundaries.');"

			/* 4. A genuinely absent header still returns null. */
			"if(f.getResponseHeader('x-not-sent')!==null)"
				"throw new Error('ASSERT FAIL: absent header did not "
					"return null.');"

			/* 5. getAllResponseHeaders() yields one line per header. */
			"var lines=f.getAllResponseHeaders().split(/\\r\\n|\\n/)"
				".filter(function(s){return s.length>0;});"
			"if(lines.length!==4)"
				"throw new Error('ASSERT FAIL: getAllResponseHeaders gave '"
					"+lines.length+' lines, want 4 (status + 3 headers).');"

			/* 6. THE CONTROL: the pre-fix stream must still fail, or this
			 *    test proves nothing about the fix. */
			"if(b.getResponseHeader('content-type')!==null)"
				"throw new Error('ASSERT FAIL: the unterminated control "
					"PARSED. This test can no longer detect the bug it "
					"exists to catch.');"
			"var bl=b.getAllResponseHeaders().split(/\\r\\n|\\n/)"
				".filter(function(s){return s.length>0;});"
			"if(bl.length!==1)"
				"throw new Error('ASSERT FAIL: control split into '+bl.length"
					"+' lines, want exactly 1 mashed line.');";

		snprintf(full_js, sizeof(full_js), "%s%s", accum_js, chk);
		ok = js_exec(thread, (const unsigned char *)full_js,
				strlen(full_js), "t61.js");
		if (!ok) {
			fprintf(stderr, "FAIL: Test 61 -- XHR response headers do not "
				"survive the accumulator. getResponseHeader() returning "
				"null for a header that WAS sent is a lying answer: callers "
				"read it as absent and take the wrong branch silently.\n");
			return 1;
		}
		fprintf(stderr, "  => all 3 headers individually addressable; "
				"unterminated control still fails as designed\n");
		}
	}
	fprintf(stderr, "=== Test 61 PASS: XHR headers parse per-line ===\n");

	/* --- Test 62: NO RECONVERT WITHOUT A SELECT CONTEXT (#265) ----------
	 *
	 * select_ctx is created ONCE, in html_finish_conversion(). Script that
	 * mutates the DOM during parse schedules a reconvert before that, so it
	 * runs with select_ctx == NULL; libcss then rejects the first
	 * css_select_style() with CSS_BADPARM (guard: `ctx == NULL || ...`),
	 * that call being for the ROOT <html>, so box_construct_element bails
	 * on box_get_style()==NULL and the WHOLE rebuild is discarded.
	 *
	 * Hardware before fixes1105: 709 reconverts across seven logs, ZERO
	 * successes. After: 4/4 (done-ok).
	 *
	 * Both halves are asserted. Part 2 is what stops the guard from being
	 * "satisfied" by refusing every reconvert -- a fix that never rebuilds
	 * would pass part 1 alone. */
	fprintf(stderr, "\n=== Test 62: reconvert needs a select context ===\n");
	{
		css_select_ctx *saved = htmlc.select_ctx;
		nserror rc_null, rc_live;

		htmlc.reflowing = false;
		htmlc.box_conversion_context = NULL;
		htmlc.aborted = false;
		htmlc.base.status = CONTENT_STATUS_DONE;
		htmlc.base.active = 0;

		htmlc.select_ctx = NULL;
		rc_null = html_reconvert_content((struct content *)&htmlc);
		(void) harness_pump_all(5000);

		if (rc_null == NSERROR_OK) {
			fprintf(stderr, "FAIL: Test 62 -- a reconvert was ACCEPTED "
				"with select_ctx==NULL. Every element's cascade returns "
				"CSS_BADPARM, the root <html> first, and the whole "
				"rebuild is discarded.\n");
			htmlc.select_ctx = saved;
			return 1;
		}

		htmlc.select_ctx = saved;
		rc_live = html_reconvert_content((struct content *)&htmlc);
		(void) harness_pump_all(20000);

		if (rc_live != NSERROR_OK) {
			fprintf(stderr, "FAIL: Test 62 -- reconvert refused (err=%d) "
				"even WITH a live select_ctx. The guard is too broad: it "
				"disables reconverts rather than deferring them.\n",
				(int) rc_live);
			return 1;
		}
		fprintf(stderr, "  => NULL ctx deferred; live ctx converts\n");
	}
	fprintf(stderr, "=== Test 62 PASS: reconvert gated on select context ===\n");


	/* --- Test 46: DOM SPEC CONFORMANCE SWEEP -----------------------------
	 *
	 * fixes1031 was a one-line deviation from the DOM spec (textContent=""
	 * must add no Text node) that silently DESTROYED page content on any
	 * jQuery site, and nothing caught it for as long as it existed because
	 * every symptom looked like a layout or font bug. This sweep hunts its
	 * siblings: the primitives real libraries lean on, each asserted
	 * against what the spec actually says rather than against "did it
	 * throw". Counts, not booleans -- a duplicated or dropped node is
	 * invisible to a truthiness check. */
	fprintf(stderr, "\n=== Test 46: DOM spec conformance sweep ===\n");
	{
		const char *sweep =
			"globalThis.__t46=[];var F=globalThis.__t46;"
			"function chk(name,got,want){if(got!==want)"
				"F.push(name+': got '+got+' want '+want);}"
			"var host=document.getElementById('feed');"
			"function mk(html){var d=document.createElement('div');"
				"if(html)d.innerHTML=html;host.appendChild(d);return d;}"

			/* textContent: empty adds NO node; non-empty adds exactly one */
			"var a=mk('<b>x</b><i>y</i>');a.textContent='';"
			"chk('textContent-empty-childNodes',a.childNodes.length,0);"
			"var a2=mk('<b>x</b>');a2.textContent='hi';"
			"chk('textContent-set-childNodes',a2.childNodes.length,1);"
			"chk('textContent-set-value',a2.textContent,'hi');"

			/* innerHTML='' empties completely */
			"var b=mk('<b>x</b><i>y</i>');b.innerHTML='';"
			"chk('innerHTML-empty',b.childNodes.length,0);"

			/* innerHTML REPLACES, never appends */
			"var c=mk('<b>x</b>');c.innerHTML='<i>y</i>';"
			"chk('innerHTML-replaces',c.children.length,1);"

			/* insertBefore(node,null) == appendChild (Preact's only insert) */
			"var d=mk('<b>x</b>');var nn=document.createElement('i');"
			"d.insertBefore(nn,null);"
			"chk('insertBefore-null-appends',d.children.length,2);"
			"chk('insertBefore-null-is-last',d.children[1]===nn,true);"

			/* appendChild/removeChild return the node */
			"var e=mk('');var f1=document.createElement('b');"
			"chk('appendChild-returns',e.appendChild(f1)===f1,true);"
			"chk('removeChild-returns',e.removeChild(f1)===f1,true);"

			/* appendChild MOVES an already-parented node, never copies */
			"var g1=mk('<b>x</b>');var g2=mk('');"
			"g2.appendChild(g1.children[0]);"
			"chk('appendChild-moves-src',g1.children.length,0);"
			"chk('appendChild-moves-dst',g2.children.length,1);"

			/* a DocumentFragment inserts its CHILDREN and is left empty */
			"var h=mk('');var fr=document.createDocumentFragment();"
			"fr.appendChild(document.createElement('b'));"
			"fr.appendChild(document.createElement('i'));"
			"h.appendChild(fr);"
			"chk('fragment-children-moved',h.children.length,2);"
			"chk('fragment-emptied',fr.childNodes.length,0);"

			/* cloneNode: shallow carries nothing, deep carries all */
			"var k=mk('<b>x</b><i>y</i>');"
			"chk('cloneNode-shallow',k.cloneNode(false).childNodes.length,0);"
			"chk('cloneNode-deep',k.cloneNode(true).children.length,2);"
			"chk('cloneNode-not-self',k.cloneNode(true)===k,false);"

			/* element traversal skips text nodes; childNodes does not */
			"var m=mk('');m.innerHTML='text<b>x</b>more';"
			"chk('childNodes-counts-text',m.childNodes.length,3);"
			"chk('children-skips-text',m.children.length,1);"
			"chk('firstElementChild-skips-text',"
				"m.firstElementChild===m.children[0],true);"
			"chk('lastElementChild',m.lastElementChild===m.children[0],true);"
			"chk('childElementCount',m.childElementCount,1);"

			/* removing a child updates the parent link both ways */
			"var n=mk('<b>x</b>');var nc=n.children[0];n.removeChild(nc);"
			"chk('removeChild-clears-parent',nc.parentNode,null);"
			"chk('removeChild-updates-count',n.children.length,0);";
		unsigned char ok46 = js_exec(thread, (const unsigned char *)sweep,
				strlen(sweep), "t46.js");
		if (!ok46) { fprintf(stderr, "FAIL: t46 sweep threw\n"); return 1; }
	}
	{
		const char *rep =
			"var F=globalThis.__t46;"
			"if(F.length)throw new Error('DOM SPEC DEVIATIONS ('+F.length+"
				"'): '+F.join(' | '));";
		unsigned char ok46b = js_exec(thread, (const unsigned char *)rep,
				strlen(rep), "t46-chk.js");
		if (!ok46b) {
			fprintf(stderr, "FAIL: Test 46 -- DOM spec deviations found "
					"(each one is a candidate for silently breaking a "
					"real site, the way textContent did)\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 46 PASS: DOM spec conformance sweep clean ===\n");

	/* --- Test 47: event PHASE ORDERING (#264) ------------------------------
	 * THE CAPTURE BLIND SPOT. Before this test there were 37
	 * addEventListener calls in this file and not one passed capture=true,
	 * so no test could observe that the three phases come out in the wrong
	 * ORDER. Test 40 asserted that events fire and that they bubble; both
	 * are true even when the ordering is wrong. Hardware (t.html,
	 * 2026-07-25) reported [cap,bubble,target] and #264 had already been
	 * closed on Test 40 + Test 38 passing.
	 *
	 * Root cause this pins: qjs_el_add_event_listener_data registers with
	 * libdom ONCE per (node, type) carrying the FIRST listener's capture
	 * flag, and __msFireLocal then fires that node's whole _L[type] array
	 * regardless of which phase libdom is currently in. A node holding both
	 * a capture and a non-capture listener therefore fires BOTH during the
	 * capture pass, before the target is ever reached.
	 *
	 * Assert the ORDER, not the occurrence -- same lesson as the fixes1005
	 * double-fire, one level up. */
	fprintf(stderr, "\n=== Test 47: capture/target/bubble ordering ===\n");
	{
		const char *ord =
			"var host=document.body||document.documentElement;"
			"var outer=document.createElement('div');"
			"var inner=document.createElement('span');"
			"outer.appendChild(inner);host.appendChild(outer);"
			"globalThis.__t47=[];"
			/* registration order is deliberately cap, target, bubble so a
			 * naive "fire _L in insertion order" cannot pass by accident */
			"outer.addEventListener('click',function(){__t47.push('cap');},true);"
			"inner.addEventListener('click',function(){__t47.push('target');},false);"
			"outer.addEventListener('click',function(){__t47.push('bubble');},false);"
			"inner.dispatchEvent(new Event('click',{bubbles:true}));"
			"host.removeChild(outer);";
		unsigned char ok47 = js_exec(thread, (const unsigned char *)ord,
				strlen(ord), "t47.js");
		if (!ok47) { fprintf(stderr, "FAIL: t47 dispatch threw\n"); return 1; }
	}
	{
		const char *rep =
			"var o=globalThis.__t47.join(',');"
			"if(o!=='cap,target,bubble')"
			"throw new Error('phase order was ['+o+'], want [cap,target,bubble]');";
		unsigned char ok47b = js_exec(thread, (const unsigned char *)rep,
				strlen(rep), "t47-chk.js");
		if (!ok47b) {
			fprintf(stderr, "FAIL: Test 47 -- event phase ordering (#264)\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 47 PASS: capture/target/bubble ordering ===\n");

	/* --- Test 48: a className change must re-cascade EVERY time (#310) -----
	 * Hardware (t.html, 2026-07-25) showed rows whose className changed after
	 * first paint keep their OLD styling, while textContent on the same
	 * element updated and a sibling's .style.background repainted.
	 *
	 * The signature that cracked it: the FIRST class change applied, the
	 * SECOND did not. libdom caches the parsed class list in ele->classes and
	 * the selector engine matches against that cache, but upstream only
	 * refreshed it when the class attribute node was first CREATED -- never
	 * when an existing one's value was replaced (fixes1042, element.c).
	 *
	 * So this test MUST make two consecutive changes. One change passes even
	 * with the bug present.
	 *
	 * The baseline assertion is also the live-cascade canary: before fixes1041
	 * it read "inline" (the CSS initial) for a div the UA sheet calls block,
	 * because media.type was set only in layout mode. */
	fprintf(stderr, "\n=== Test 48: className re-cascades on EVERY change ===\n");
#define T48_RECONVERT() do { \
		htmlc.base.status = CONTENT_STATUS_DONE; \
		htmlc.reflowing = false; \
		htmlc.box_conversion_context = NULL; \
		htmlc.aborted = false; \
		htmlc.base.active = 0; \
		(void) html_reconvert_content((struct content *)&htmlc); \
		harness_pump_all(100000); \
	} while (0)
	{
		const char *q = "var e0=document.getElementById('feed');"
			"if(!e0)throw new Error('t48 fixture #feed is gone');"
			"globalThis.__t48z=getComputedStyle(e0).display;";
		if (!js_exec(thread,(const unsigned char*)q,strlen(q),"t48-0.js")) {
			fprintf(stderr,"FAIL: t48 baseline threw\n"); return 1; }
	}
	{
		const char *q = "document.getElementById('feed').className='t48a';";
		if (!js_exec(thread,(const unsigned char*)q,strlen(q),"t48-1.js")) {
			fprintf(stderr,"FAIL: t48 flip-1 threw\n"); return 1; }
	}
	T48_RECONVERT();
	{
		const char *q = "globalThis.__t48a=getComputedStyle("
			"document.getElementById('feed')).display;";
		if (!js_exec(thread,(const unsigned char*)q,strlen(q),"t48-2.js")) {
			fprintf(stderr,"FAIL: t48 probe-1 threw\n"); return 1; }
	}
	{
		const char *q = "document.getElementById('feed').className='t48b';";
		if (!js_exec(thread,(const unsigned char*)q,strlen(q),"t48-3.js")) {
			fprintf(stderr,"FAIL: t48 flip-2 threw\n"); return 1; }
	}
	T48_RECONVERT();
	{
		const char *q = "globalThis.__t48b=getComputedStyle("
			"document.getElementById('feed')).display;"
			"globalThis.__t48cls=document.getElementById('feed')"
			".getAttribute('class');"
			"document.getElementById('feed').className='';";
		if (!js_exec(thread,(const unsigned char*)q,strlen(q),"t48-4.js")) {
			fprintf(stderr,"FAIL: t48 probe-2 threw\n"); return 1; }
	}
	{
		const char *q =
			"var z=globalThis.__t48z,a=globalThis.__t48a,"
			"b=globalThis.__t48b,c=globalThis.__t48cls;"
			"if(z!=='block')"
			"throw new Error('CASCADE IS DEAD: bare #feed display is \"'+z+"
				"'\", want \"block\" from div{display:block}. Check "
				"media.type is CSS_MEDIA_SCREEN unconditionally "
				"(fixes1041) -- nothing below means anything.');"
			"if(a!=='inline-block')"
			"throw new Error('FIRST class change lost: display is \"'+a+"
				"'\", want \"inline-block\"');"
			"if(b!=='flex')"
			"throw new Error('SECOND class change lost: display is \"'+b+"
				"'\", want \"flex\" (class attr correctly reads \"'+c+"
				"'\") -- libdom ele->classes cache went stale, see "
				"fixes1042 in libdom element.c (#310)');";
		if (!js_exec(thread,(const unsigned char*)q,strlen(q),"t48-chk.js")) {
			fprintf(stderr,"FAIL: Test 48 -- className re-cascade (#310)\n");
			return 1; }
	}
	fprintf(stderr, "=== Test 48 PASS: className re-cascades on every change ===\n");

	/* --- Test 49: the perf instrument must MEASURE, not merely emit -------
	 * fixes1070 added a compile-vs-run split to js_exec, a per-script cost
	 * table and a GC hook, because `js=25.4s` as a single number cannot say
	 * whether the fix is bytecode caching (compile-bound) or something else
	 * entirely (run-bound). Those are opposite pieces of work.
	 *
	 * A profiler is unusually easy to ship broken: swap the two brackets and
	 * it still produces two plausible non-zero numbers, and every decision
	 * taken from them is backwards. So this test does not ask "are the
	 * numbers non-zero" -- it runs one script engineered to be COMPILE-heavy
	 * and RUN-trivial, and one engineered to be the exact opposite, then
	 * checks the split lands on the correct side of the seam for each. That
	 * assertion fails if the brackets are swapped, if either bracket is
	 * dropped, or if the per-script table misattributes a row.
	 *
	 * It also covers the eviction rule, the eval count, and the GC hook in
	 * quickjs.c's JS_RunGC -- which is a patch to a vendored file and
	 * therefore the single most likely thing to be lost in a future
	 * QuickJS update. If that patch goes missing, gcruns stays 0 and this
	 * test says so instead of the GC column silently reading zero forever. */
	fprintf(stderr, "\n=== Test 49: JS perf split measures the right halves ===\n");
	{
		extern void macsurf_qjs_emit_js_profile(void);
		extern int  macsurf_qjs_perf_slot(int i, char *name, int cap,
				long *bytes, long *compile_us, long *run_us,
				long *evals);
		extern void macsurf_qjs_perf_totals(long *evals,
				long *compile_us, long *run_us, long *gc_us,
				long *gc_runs, int *gc_armed);
		extern void macsurf_qjs_run_gc(struct jsheap *heap);
		char *big;
		long evals = 0, tc = 0, tr = 0, gcus = 0, gcruns = 0;
		int gcarmed = -1;
		long c_compile = -1, c_run = -1, r_compile = -1, r_run = -1;
		int i;
		int rows = 0;

		/* Earlier tests have already run dozens of scripts through
		 * js_exec. Emit (which resets) so this test measures only its
		 * own three, and so the emit path itself is exercised. */
		macsurf_qjs_emit_js_profile();
		macsurf_qjs_perf_totals(&evals, &tc, &tr, &gcus, &gcruns,
				&gcarmed);
		if (evals != 0) {
			fprintf(stderr, "FAIL: Test 49 -- emit did not reset "
				"the per-navigation counters (evals=%ld, want 0). "
				"Carrying one page's scripts into the next page's "
				"table misattributes cost to whichever site loaded "
				"second.\n", evals);
			return 1;
		}

		/* COMPILE-heavy, RUN-trivial: ~4000 function declarations that
		 * are never called. QuickJS must parse and codegen every one;
		 * executing the program only creates the bindings. */
		big = (char *)malloc(4000 * 64 + 64);
		if (big == NULL) {
			fprintf(stderr, "FAIL: Test 49 -- OOM building fixture\n");
			return 1;
		}
		big[0] = '\0';
		{
			char *w = big;
			for (i = 0; i < 4000; i++) {
				w += sprintf(w, "function t49f%d(a,b){return "
					"a*%d+b-%d;}\n", i, i + 1, i);
			}
		}
		if (!js_exec(thread, (const unsigned char *)big, strlen(big),
				"t49-compile.js")) {
			fprintf(stderr, "FAIL: Test 49 -- compile-heavy fixture "
				"threw\n");
			free(big); return 1;
		}
		free(big);

		/* RUN-heavy, COMPILE-trivial: a few dozen bytes of source, a
		 * couple of million bytecode iterations. */
		{
			const char *q = "var s=0;for(var i=0;i<2000000;i++)"
				"s+=i;globalThis.__t49r=s;";
			if (!js_exec(thread, (const unsigned char *)q,
					strlen(q), "t49-run.js")) {
				fprintf(stderr, "FAIL: Test 49 -- run-heavy "
					"fixture threw\n");
				return 1;
			}
		}

		/* Allocation-heavy. NOT to trip automatic GC -- automatic GC is
		 * DISARMED (js_newheap pushes JS_SetGCThreshold to 1GB, past
		 * the 128MB memory cap, a fixes593 workaround for a suspected
		 * cycle-collector double-free that was never root-caused). The
		 * first version of this test asserted 200k allocations produce
		 * a collection, and it failed -- correctly. gcruns=0 on a real
		 * page load is the truth, not a broken hook, which is exactly
		 * why the JSPHASE line carries gcarmed. */
		{
			const char *q = "var a=[];for(var i=0;i<200000;i++)"
				"a.push({x:i,y:'v'+i});a=null;";
			if (!js_exec(thread, (const unsigned char *)q,
					strlen(q), "t49-gc.js")) {
				fprintf(stderr, "FAIL: Test 49 -- gc fixture "
					"threw\n");
				return 1;
			}
		}

		macsurf_qjs_perf_totals(&evals, &tc, &tr, &gcus, &gcruns,
				&gcarmed);
		if (evals != 3) {
			fprintf(stderr, "FAIL: Test 49 -- eval COUNT is %ld, "
				"want exactly 3. A count, not a boolean: a "
				"double-counted eval reads as plausible work and "
				"inflates every per-script row.\n", evals);
			return 1;
		}

		for (i = 0; i < 16; i++) {
			char nm[40];
			long b = 0, c = 0, r = 0, n = 0;
			if (!macsurf_qjs_perf_slot(i, nm, (int)sizeof(nm), &b,
					&c, &r, &n))
				continue;
			rows++;
			fprintf(stderr, "  t49 slot %s b=%ld c=%ldus r=%ldus "
				"n=%ld\n", nm, b, c, r, n);
			if (strcmp(nm, "t49-compile.js") == 0) {
				c_compile = c; c_run = r;
			} else if (strcmp(nm, "t49-run.js") == 0) {
				r_compile = c; r_run = r;
			}
		}
		if (rows != 3) {
			fprintf(stderr, "FAIL: Test 49 -- per-script table has "
				"%d rows, want 3 (one per script this test "
				"ran).\n", rows);
			return 1;
		}
		if (c_compile < 0 || r_compile < 0) {
			fprintf(stderr, "FAIL: Test 49 -- per-script table lost "
				"a row (compile-heavy found=%d run-heavy "
				"found=%d)\n", c_compile >= 0, r_compile >= 0);
			return 1;
		}

		/* THE assertion. Not "both are non-zero" -- which a swapped
		 * pair of brackets passes -- but "each script's time landed on
		 * the side it was built to be heavy on". */
		if (!(c_compile > c_run)) {
			fprintf(stderr, "FAIL: Test 49 -- 4000 uncalled function "
				"declarations measured compile=%ldus run=%ldus. "
				"Compile must dominate; it does not. The compile "
				"and run brackets in js_exec are swapped or one "
				"is missing.\n", c_compile, c_run);
			return 1;
		}
		if (!(r_run > r_compile)) {
			fprintf(stderr, "FAIL: Test 49 -- a 2M-iteration loop in "
				"~40 bytes of source measured compile=%ldus "
				"run=%ldus. Run must dominate; it does not. The "
				"compile and run brackets in js_exec are swapped "
				"or one is missing.\n", r_compile, r_run);
			return 1;
		}
		if (tc <= 0 || tr <= 0) {
			fprintf(stderr, "FAIL: Test 49 -- JSPHASE totals are "
				"compile=%ldus run=%ldus; both must be positive "
				"after the fixtures above.\n", tc, tr);
			return 1;
		}
		/* Automatic GC is off, so nothing above can have collected. If
		 * this is ever non-zero, the fixes593 threshold has been
		 * changed or lost and the perf picture changes with it. */
		if (gcruns != 0 || gcarmed != 0) {
			fprintf(stderr, "FAIL: Test 49 -- expected automatic GC "
				"to be DISARMED (gcruns=0 gcarmed=0), got "
				"gcruns=%ld gcarmed=%d. Either js_newheap's "
				"JS_SetGCThreshold(1GB) workaround from fixes593 "
				"was changed, or g_perf_gc_armed drifted from it. "
				"Re-arming GC is a real decision with real perf "
				"consequences -- make it deliberately.\n",
				gcruns, gcarmed);
			return 1;
		}

		/* Now prove the timing hook itself works, independently of
		 * whether anything calls it during a load. Without this the
		 * hook in quickjs.c is unreachable code that no test can
		 * distinguish from a missing patch -- and it IS a patch to a
		 * vendored file, so a future QuickJS update is exactly how it
		 * would get silently dropped. */
		macsurf_qjs_run_gc(heap);
		macsurf_qjs_perf_totals(NULL, NULL, NULL, &gcus, &gcruns, NULL);
		if (gcruns != 1) {
			fprintf(stderr, "FAIL: Test 49 -- one explicit "
				"macsurf_qjs_run_gc produced gcruns=%ld, want "
				"exactly 1. The timing hook in JS_RunGC "
				"(quickjs.c, a VENDORED file patched in BOTH "
				"browser/libquickjs and quickjs-macos9) is "
				"missing or fires more than once per "
				"collection.\n", gcruns);
			return 1;
		}
		fprintf(stderr, "  t49 totals evals=%ld compile=%ldus run=%ldus "
			"gc=%ldus gcruns=%ld gcarmed=%d\n",
			evals, tc, tr, gcus, gcruns, gcarmed);
	}
	fprintf(stderr, "=== Test 49 PASS: compile/run split and GC hook measure "
		"the right things ===\n");

	/* --- Test 50: wrapper helpers compile ONCE, not once per element -----
	 * fixes1070's hardware log found ONE 72KB script (hackaday's
	 * navigation.js concat bundle) running 24.7 SECONDS -- half the entire
	 * page load. Root cause: every element wrapper JS_Eval'd four fixed C
	 * string literals totalling ~13.9 KB of JavaScript, so QuickJS parsed
	 * and code-generated the same source again for every element the page
	 * touched. At the 2.17us/byte compile rate that log priced, that is
	 * ~30ms per wrapper, and a script walking ~800 elements IS the 24.7s.
	 *
	 * It hid because the recompilation happens inside a C binding during
	 * script execution, so it was charged to run_us and read as "the script
	 * is slow" rather than "we recompile our own helpers 800 times".
	 *
	 * The invariant is simple and is the only honest way to state the fix:
	 * helper compiles must NOT scale with wrapper count. Asserting "the
	 * page got faster" would be a timing test on an ASan -O0 build; this
	 * asserts the mechanism, in counts. */
	fprintf(stderr, "\n=== Test 50: wrapper helpers compile once, not per "
		"element ===\n");
	{
		extern void macsurf_qjs_wrap_stats(long *wraps, long *hcompiles,
				long *hbytes);
		long w0 = 0, c0 = 0, b0 = 0, w1 = 0, c1 = 0, b1 = 0;
		const char *walk =
			"var host=document.getElementById('feed');"
			"if(!host)throw new Error('t50 fixture #feed is gone');"
			"var i,d;"
			"for(i=0;i<120;i++){d=document.createElement('div');"
			"d.className='t50row';d.textContent='r'+i;"
			"host.appendChild(d);}"
			"var all=host.querySelectorAll('.t50row');"
			"if(all.length<120)throw new Error('t50 built '+all.length"
			"+' rows, want >=120');"
			/* Touch each one so a wrapper is genuinely built per
			 * element -- the whole point is per-element wrapping. */
			"var n=0;for(i=0;i<all.length;i++){"
			"if(all[i].className==='t50row')n++;}"
			"globalThis.__t50n=n;";

		macsurf_qjs_wrap_stats(&w0, &c0, &b0);
		if (!js_exec(thread, (const unsigned char *)walk, strlen(walk),
				"t50-walk.js")) {
			fprintf(stderr, "FAIL: Test 50 -- fixture threw\n");
			return 1;
		}
		macsurf_qjs_wrap_stats(&w1, &c1, &b1);
		fprintf(stderr, "  t50 wraps +%ld  helper-compiles +%ld  "
			"helper-bytes +%ld\n", w1 - w0, c1 - c0, b1 - b0);

		if (w1 - w0 < 100) {
			fprintf(stderr, "FAIL: Test 50 -- only %ld element "
				"wrappers were built; the fixture is not "
				"exercising per-element wrapping and the test "
				"below would pass vacuously.\n", w1 - w0);
			return 1;
		}
		/* THE assertion: compiles must not scale with wrappers. Four
		 * helper blocks exist, so four compiles per fresh context is
		 * the ceiling -- and this context is already warm from Tests
		 * 1-49, so the honest expectation here is zero. Allow the
		 * four-block ceiling so the test is not brittle about which
		 * test warmed the cache first. */
		if (c1 - c0 > 4) {
			fprintf(stderr, "FAIL: Test 50 -- %ld element wrappers "
				"caused %ld helper compiles (%ld bytes). Helper "
				"compiles must NOT scale with wrapper count: the "
				"sources are compile-time constants and are "
				"cached per context by qjs_helper_fn. This is "
				"the hackaday navigation.js regression -- 24.7s "
				"on one script, half the page load.\n",
				w1 - w0, c1 - c0, b1 - b0);
			return 1;
		}
	}
	fprintf(stderr, "=== Test 50 PASS: helper compiles do not scale with "
		"wrapper count ===\n");

	/* --- Test 51: geometry answers POST-mutation truth (#265) ------------
	 * The measure/mutate contract: change the DOM, then immediately ask how
	 * big something is. Every modern component is built on it, and a real
	 * browser answers by reflowing right there.
	 *
	 * This engine could not, so fixes1016 made it decline -- geometry
	 * returned `undefined` whenever a mutation was awaiting its debounced
	 * reconvert. That was correct as far as it went (a fabricated 0 gets
	 * written back as an inline size and destroys the page; `undefined`
	 * NaN-propagates into a harmless no-op), but declining is still a wrong
	 * answer, and it is why widgets that measure before laying themselves
	 * out get nothing and lay themselves out wrong.
	 *
	 * fixes1073 forces a synchronous reconvert+layout at the geometry entry
	 * points. This test asserts the observable consequence: measuring
	 * straight after a mutation, with NO manual reconvert in between, must
	 * report the NEW height.
	 *
	 * It is a real control. With MACSURF_JS_GEOMETRY=0, or with the flush
	 * removed, h1 comes back undefined and the assertion below fires. */
	fprintf(stderr, "\n=== Test 51: geometry reflows before answering "
		"(#265) ===\n");
	{
		extern void macos9_reconvert_sync_stats(long *f, long *d,
				long *us);
		long f0 = 0, d0 = 0, u0 = 0;

		/* The flush needs the same preconditions html_reconvert
		 * enforces for itself: a finished document, no layout or
		 * convert in flight, no active fetches. */
		htmlc.base.status = CONTENT_STATUS_DONE;
		htmlc.reflowing = false;
		htmlc.box_conversion_context = NULL;
		htmlc.aborted = false;
		htmlc.base.active = 0;

		{
			const char *q =
				"var h=document.getElementById('feed');"
				"if(!h)throw new Error('t51 fixture #feed is gone');"
				"h.innerHTML='';"
				"globalThis.__t51a=h.offsetHeight;";
			if (!js_exec(thread, (const unsigned char *)q,
					strlen(q), "t51-base.js")) {
				fprintf(stderr, "FAIL: Test 51 -- baseline threw\n");
				return 1;
			}
		}
		harness_pump_all(100000);
		htmlc.base.status = CONTENT_STATUS_DONE;
		htmlc.reflowing = false;
		htmlc.box_conversion_context = NULL;
		htmlc.base.active = 0;

		macos9_reconvert_sync_stats(&f0, &d0, &u0);

		/* Mutate and measure in ONE script, with no reconvert driven
		 * between them. This is the whole point -- a pumped reconvert
		 * would make the test pass without the feature. */
		{
			const char *q =
				"var h=document.getElementById('feed');"
				"var i,d;"
				"for(i=0;i<40;i++){d=document.createElement('div');"
				"d.style.height='20px';d.id='t51row'+i;"
				"d.textContent='row'+i;h.appendChild(d);}"
				"globalThis.__t51b=h.offsetHeight;";
			if (!js_exec(thread, (const unsigned char *)q,
					strlen(q), "t51-measure.js")) {
				fprintf(stderr, "FAIL: Test 51 -- measure "
					"fixture threw\n");
				return 1;
			}
		}
		{
			long f1 = 0, d1 = 0, u1 = 0;
			dom_string *rid = NULL;
			dom_element *rel = NULL;

			/* (1) The behavioural change: geometry must ANSWER a
			 * measurement taken straight after a mutation, not
			 * decline it. Before fixes1073 this was `undefined`. */
			{
				const char *q =
					"var b=globalThis.__t51b;"
					"if(b===undefined)"
					"throw new Error('geometry DECLINED after a "
						"mutation (offsetHeight undefined) -- the "
						"forced synchronous layout did not run. "
						"Check MACSURF_JS_GEOMETRY is 1 and that "
						"qjs_geometry_flush reaches "
						"macos9_reconvert_flush_now (#265).');"
					"if(typeof b!=='number')"
					"throw new Error('offsetHeight is '+(typeof b)+"
						"', want number');";
				if (!js_exec(thread, (const unsigned char *)q,
						strlen(q), "t51-chk.js")) {
					fprintf(stderr, "FAIL: Test 51 -- measure/"
						"mutate contract (#265)\n");
					return 1;
				}
			}

			/* (2) A flush was actually recorded. Without this the
			 * assertion above could pass on a tree that happened to
			 * be clean rather than on the feature. */
			macos9_reconvert_sync_stats(&f1, &d1, &u1);
			fprintf(stderr, "  t51 forced flushes +%ld declined +%ld "
				"cost +%ldus\n", f1 - f0, d1 - d0, u1 - u0);
			if (f1 - f0 < 1) {
				fprintf(stderr, "FAIL: Test 51 -- the answer was "
					"right but NO forced flush was recorded "
					"(+%ld), so the test is passing on a "
					"stale-but-lucky box tree rather than on "
					"the feature.\n", f1 - f0);
				return 1;
			}

			/* (3) The flush picked up THIS mutation: a div created
			 * by script moments ago must now own a box. Box
			 * construction happens only in dom_to_box, so the only
			 * way this element has one is the forced reconvert.
			 *
			 * This is what the height assertion would be on
			 * hardware. The harness never runs layout_document --
			 * content__reformat dispatches through
			 * c->handler->reformat and this fixture has no handler
			 * -- so every box keeps its birth width and heights read
			 * 0 here regardless. Asserting the tree was rebuilt is
			 * the honest harness-side proxy; the pixel assertion
			 * belongs to a hardware log. */
			if (dom_string_create((const uint8_t *)"t51row7", 7,
					&rid) != DOM_NO_ERR) {
				fprintf(stderr, "FAIL: t51 dom_string_create\n");
				return 1;
			}
			dom_document_get_element_by_id(document, rid, &rel);
			dom_string_unref(rid);
			if (rel == NULL) {
				fprintf(stderr, "FAIL: Test 51 -- #t51row7 is not "
					"in the DOM; the fixture did not append\n");
				return 1;
			}
			if (box_for_node((dom_node *)rel) == NULL) {
				fprintf(stderr, "FAIL: Test 51 -- a script-created "
					"element has NO box after a forced flush. "
					"The reconvert ran but did not rebuild from "
					"the mutated DOM, so geometry answered "
					"against a tree that does not contain the "
					"element being measured (#265).\n");
				return 1;
			}
			dom_node_unref((dom_node *)rel);
			fprintf(stderr, "  t51 script-created #t51row7 owns a box "
				"after the forced flush\n");
		}
	}
	fprintf(stderr, "=== Test 51 PASS: geometry reflows before answering "
		"===\n");

	/* Test 52 (prototype-reflected attributes) REMOVED with fixes1085: the
	 * fixes1079/1081 prototype migration it asserted has been reverted. Test
	 * 53 below stays -- text nodes must not expose element-only properties
	 * whether those properties live on a prototype or on the instance, so
	 * that invariant is independent of how they are installed. */
	/* --- Test 53: text nodes must NOT inherit element properties ---------
	 * The control for fixes1081, and the test whose absence let fixes1080
	 * ship a 13-second regression.
	 *
	 * qjs_wrap_text_node and qjs_wrap_fragment build their objects with the
	 * SAME class id as elements, so the class prototype is shared by all
	 * three. fixes1079/1080 moved element-only accessors onto it, which gave
	 * every text node `children`, `firstElementChild`, `src`, `checked` and
	 * the rest. Hardware went 20s -> 33.5s: not because the work got slower,
	 * but because libraries feature-detect these and a text node that claims
	 * to have `children` sends them down element code paths.
	 *
	 * This is the fixes1010 lesson from the opposite direction. There a
	 * method was present on elements and MISSING on text nodes; here a
	 * property that must not exist on text nodes was PRESENT. Both break the
	 * same way, and neither throws -- which is why only a test that asks
	 * about the SHAPE of a text node can catch it. */
	fprintf(stderr, "\n=== Test 53: text nodes do not inherit element "
		"properties ===\n");
	{
		const char *q =
			"var t=document.createTextNode('hello');"
			"var bad=[];"
			"var probe=['children','firstElementChild',"
				"'lastElementChild','childElementCount',"
				"'src','href','checked','disabled','readOnly',"
				"'offsetParent','scrollIntoView','getClientRects'];"
			"var i;for(i=0;i<probe.length;i++){"
				"if(probe[i] in t)bad.push(probe[i]);}"
			"if(bad.length)"
			"throw new Error('a TEXT NODE exposes element-only "
				"properties ['+bad.join(',')+']. It shares "
				"s_el_class_id with elements, so anything put on "
				"the CLASS prototype reaches it -- element "
				"accessors belong on the element-only prototype "
				"(fixes1081). Libraries feature-detect these and "
				"will take element paths on text nodes.');"
			/* And the surface it SHOULD have still works. */
			"if(t.nodeType!==3)"
			"throw new Error('text node nodeType is '+t.nodeType);"
			"if(typeof t.getRootNode!=='function')"
			"throw new Error('text node lost getRootNode -- node-nav "
				"must stay on every wrapper shape (fixes1010)');"
			/* An element still has them, i.e. we did not overcorrect. */
			"var e=document.createElement('div');"
			"if(!('children' in e))"
			"throw new Error('an ELEMENT lost children -- the "
				"element prototype is not being applied');"
			"if(!('src' in e))"
			"throw new Error('an ELEMENT lost src');";
		if (!js_exec(thread, (const unsigned char *)q, strlen(q),
				"t53.js")) {
			fprintf(stderr, "FAIL: Test 53 -- text/element prototype "
				"separation (fixes1081)\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 53 PASS: text nodes do not inherit element "
		"properties ===\n");

	/* --- Test 63: instanceof answers truthfully for the DOM constructor
	 * family (fixes1127) ------------------------------------------------
	 * XenForo core-compiled.js measureScrollBar calls
	 * XF.createElement("div", {className:"scrollMeasure"}, m.body), which
	 * gates its append behind `b instanceof HTMLElement`. The DOM
	 * constructors are JS stubs; before fixes1127 no wrapper answered
	 * instanceof HTMLElement, so the probe div was never appended and
	 * `b.parentNode.removeChild(b)` threw "cannot read property 'removeChild'
	 * of null" -- blocking XF.Element registration and the whole editor.
	 *
	 * Assert the family answers truthfully for every wrapper shape:
	 * elements (including per-tag identity), text, and fragments -- and that
	 * the instanceof-gated append-remove idiom completes without throwing.
	 * Counts asserts by construction: each assert that fails throws. */
	fprintf(stderr, "\n=== Test 63: instanceof answers truthfully for the "
		"DOM constructor family ===\n");
	{
		const char *q =
			"function assert(c,m){if(!c)throw new Error('ASSERT FAIL: '+m);}"
			"var div=document.createElement('div');"
			"assert(div instanceof HTMLElement,"
				"'div instanceof HTMLElement');"
			"assert(div instanceof HTMLDivElement,"
				"'div instanceof HTMLDivElement');"
			"assert(div instanceof Element,'div instanceof Element');"
			"assert(div instanceof Node,'div instanceof Node');"
			"assert(!(div instanceof HTMLBodyElement),"
				"'div must not be instanceof HTMLBodyElement');"
			"var body=document.body;"
			"assert(body&&body instanceof HTMLElement,"
				"'document.body instanceof HTMLElement');"
			"assert(body instanceof HTMLBodyElement,"
				"'document.body instanceof HTMLBodyElement');"
			"var sp=document.createElement('span');"
			"assert(sp instanceof HTMLSpanElement,"
				"'span instanceof HTMLSpanElement');"
			"assert(!(sp instanceof HTMLDivElement),"
				"'span must not be instanceof HTMLDivElement');"
			"var abbr=document.createElement('abbr');"
			"assert(abbr instanceof HTMLElement,"
				"'unknown-tag element still instanceof HTMLElement');"
			"var t=document.createTextNode('x');"
			"assert(t instanceof Text,'text instanceof Text');"
			"assert(t instanceof CharacterData,"
				"'text instanceof CharacterData');"
			"assert(t instanceof Node,'text instanceof Node');"
			"assert(!(t instanceof HTMLElement),"
				"'text must NOT be instanceof HTMLElement');"
			"assert(!(t instanceof Element),"
				"'text must NOT be instanceof Element');"
			"var frag=document.createDocumentFragment();"
			"assert(frag instanceof DocumentFragment,"
				"'fragment instanceof DocumentFragment');"
			"assert(frag instanceof Node,'fragment instanceof Node');"
			"assert(!(frag instanceof HTMLElement),"
				"'fragment must NOT be instanceof HTMLElement');"
			/* THE BUG SHAPE: XF.createElement's instanceof-gated append. */
			"var b=document.createElement('div');"
			"if(body instanceof HTMLElement)body.appendChild(b);"
			"assert(b.parentNode,"
				"'parentNode set after instanceof-gated append');"
			"b.parentNode.removeChild(b);"
			"assert(!b.parentNode,"
				"'parentNode cleared after removeChild');"
			/* on* handlers must survive the prototype rerouting (fixes872). */
			"var h=document.createElement('div');"
			"h.onclick=function(){};"
			"assert(h._H&&h._H.click,"
				"'on* handler still works on a rerouted wrapper');";
		if (!js_exec(thread, (const unsigned char *)q, strlen(q),
				"t63.js")) {
			fprintf(stderr, "FAIL: Test 63 -- instanceof DOM constructor "
				"family (fixes1127)\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 63 PASS: instanceof answers truthfully "
		"for the DOM constructor family ===\n");

	/* --- Test 64: geometry settles ONCE per JS execution (#265) ----------
	 *
	 * Hardware: hackaday's slick init did 1280 geometry reads interleaved
	 * with DOM writes; every read found a pending mutation and attempted a
	 * synchronous flush (~1.2 s each), 25 succeeded, the 30 s budget
	 * expired, and the remaining 1255 reads were DECLINED and answered
	 * undefined/0 -- so the slider baked width:0px/height:0px into every
	 * slide (`LIFE JSSYNC flush=25 declined=1255 us=30680059`).
	 *
	 * The fix: settle-once-per-JS-execution. One flush per burst is all a
	 * script needs -- the box tree cannot change while JS is not running
	 * (cooperative model) -- so after the first read settles, subsequent
	 * reads of the SAME burst answer from the settled tree without paying
	 * for another reconvert. The flag is cleared at every execution
	 * boundary (pump_all top, js_exec top, per timer, per event dispatch),
	 * NOT on DOM mutation: clearing on mutation re-arms the flag on the
	 * first write after a settle, which keeps ~1280 flush attempts and the
	 * budget still breaks.
	 *
	 * Asserted as COUNTS, per the house rule:
	 *   (1) one burst doing 3 appends interleaved with 3 reads must record
	 *       EXACTLY ONE flush (old behaviour: 3), and every read must
	 *       answer a number -- the skip must answer from the settled tree,
	 *       not refuse;
	 *   (2) after a pump (JS yielded), the next burst's first read flushes
	 *       again -- the flag must not leak across executions;
	 *   (3) two back-to-back js_exec calls with NO pump between (the
	 *       harness's own execution shape) still each flush -- the
	 *       js_exec-top clear is the boundary that makes this work.
	 *
	 * Control: with the settle flag removed, (1) records +3 flushes and
	 * this test fails on the exact-count assertion. */
	fprintf(stderr, "\n=== Test 64: geometry settles once per JS execution "
		"(#265) ===\n");
	{
		extern void macos9_reconvert_sync_stats(long *f, long *d,
				long *us);
		extern void macos9_reconvert_sync_reset(void);
		extern int macos9_reconvert_flush_now(void *cv);
		long f0 = 0, d0 = 0, u0 = 0, f1 = 0, d1 = 0, u1 = 0;
		unsigned char ok;

		/* Same preconditions Test 51 needs: a finished document, no
		 * layout/convert in flight, no active fetches. */
		htmlc.base.status = CONTENT_STATUS_DONE;
		htmlc.reflowing = false;
		htmlc.box_conversion_context = NULL;
		htmlc.aborted = false;
		htmlc.base.active = 0;
		/* Drain any leftover pending marks from earlier tests, then pump
		 * so the settle flag (if a later test ever set it) starts 0. */
		(void) macos9_reconvert_flush_now((void *)&htmlc);
		(void) harness_pump_all(100000);

		/* (1) 3 appends interleaved with 3 reads in ONE burst. */
		{
			const char *q =
				"var f=document.getElementById('feed');"
				"if(!f)throw new Error('t64 fixture #feed is gone');"
				"var i,d,out=[];"
				"for(i=0;i<3;i++){"
					"d=document.createElement('div');"
					"d.id='t64row'+i;d.textContent='r'+i;"
					"f.appendChild(d);"
					"out.push(f.offsetHeight);"
				"}"
				"globalThis.__t64out=out;";
			macos9_reconvert_sync_reset();
			macos9_reconvert_sync_stats(&f0, &d0, &u0);
			if (!js_exec(thread, (const unsigned char *)q, strlen(q),
					"t64-burst.js")) {
				fprintf(stderr, "FAIL: Test 64 -- burst fixture "
					"threw\n");
				return 1;
			}
			macos9_reconvert_sync_stats(&f1, &d1, &u1);
			fprintf(stderr, "  burst of 3 reads: flushes +%ld, "
				"declined +%ld, cost +%ldus\n",
				f1 - f0, d1 - d0, u1 - u0);
			if (f1 - f0 != 1) {
				fprintf(stderr, "FAIL: Test 64 -- 3 reads in one "
					"burst recorded +%ld flushes, want exactly 1. "
					"Old behaviour was +3 (each read a 1.2s "
					"reconvert); the settle-once flag is not "
					"skipping the 2nd and 3rd reads.\n",
					f1 - f0);
				return 1;
			}
		}

		/* Every read answered a number: the skip must serve the settled
		 * tree, not decline (a decline would make the answer undefined). */
		{
			const char *q =
				"var out=globalThis.__t64out;"
				"if(!out||out.length!==3)"
				"throw new Error('t64 answers missing');"
				"var i;for(i=0;i<3;i++){"
					"if(typeof out[i]!=='number')"
					"throw new Error('t64 read '+(i+1)+' answered '"
						"+out[i]+' (typeof '+(typeof out[i])+') -- "
						"the settled-tree skip must ANSWER, not "
						"decline.');"
				"}";
			ok = js_exec(thread, (const unsigned char *)q, strlen(q),
					"t64-ans.js");
		}
		if (!ok) {
			fprintf(stderr, "FAIL: Test 64 -- a settled-burst read "
				"was not answered from the settled tree\n");
			return 1;
		}

		/* (2) A pump = JS yielded = the next burst starts fresh: its
		 * first read must flush again. */
		(void) harness_pump_all(100000);
		{
			const char *q =
				"var f=document.getElementById('feed');"
				"var d=document.createElement('div');"
				"d.id='t64after';f.appendChild(d);"
				"globalThis.__t64h=f.offsetHeight;"
				"if(typeof globalThis.__t64h!=='number')"
				"throw new Error('t64 post-pump read declined');";
			macos9_reconvert_sync_reset();
			macos9_reconvert_sync_stats(&f0, &d0, &u0);
			if (!js_exec(thread, (const unsigned char *)q, strlen(q),
					"t64-after.js")) {
				fprintf(stderr, "FAIL: Test 64 -- post-pump burst "
					"threw\n");
				return 1;
			}
			macos9_reconvert_sync_stats(&f1, &d1, &u1);
			fprintf(stderr, "  post-pump burst: flushes +%ld\n",
				f1 - f0);
			if (f1 - f0 != 1) {
				fprintf(stderr, "FAIL: Test 64 -- a burst AFTER a "
					"pump recorded +%ld flushes, want 1. The "
					"settle flag leaked across the yield "
					"(pump_all-top clear missing).\n",
					f1 - f0);
				return 1;
			}
		}

		/* (3) Back-to-back js_exec with NO pump between (the harness's
		 * own shape -- and the shape script loads take on hardware): each
		 * execution is its own burst, so each first read flushes. */
		{
			const char *q =
				"var f=document.getElementById('feed');"
				"var d=document.createElement('div');"
				"d.id='t64b2';f.appendChild(d);"
				"globalThis.__t64h2=f.offsetHeight;";
			macos9_reconvert_sync_reset();
			macos9_reconvert_sync_stats(&f0, &d0, &u0);
			if (!js_exec(thread, (const unsigned char *)q, strlen(q),
					"t64-b2.js")) {
				fprintf(stderr, "FAIL: Test 64 -- exec B threw\n");
				return 1;
			}
			macos9_reconvert_sync_stats(&f1, &d1, &u1);
			fprintf(stderr, "  exec B (no pump): flushes +%ld\n",
				f1 - f0);
			if (f1 - f0 != 1) {
				fprintf(stderr, "FAIL: Test 64 -- exec B recorded "
					"+%ld flushes, want 1: the js_exec-top "
					"settle clear is missing, so a prior burst's "
					"settle silenced this execution's flush.\n",
					f1 - f0);
				return 1;
			}
		}

		/* And a THIRD exec, still no pump: the js_exec-top clear must
		 * keep working burst after burst, not just once. */
		{
			const char *q =
				"var f=document.getElementById('feed');"
				"var d=document.createElement('div');"
				"d.id='t64b3';f.appendChild(d);"
				"globalThis.__t64h3=f.offsetHeight;";
			macos9_reconvert_sync_reset();
			macos9_reconvert_sync_stats(&f0, &d0, &u0);
			if (!js_exec(thread, (const unsigned char *)q, strlen(q),
					"t64-b3.js")) {
				fprintf(stderr, "FAIL: Test 64 -- exec C threw\n");
				return 1;
			}
			macos9_reconvert_sync_stats(&f1, &d1, &u1);
			fprintf(stderr, "  exec C (no pump): flushes +%ld\n",
				f1 - f0);
			if (f1 - f0 != 1) {
				fprintf(stderr, "FAIL: Test 64 -- exec C recorded "
					"+%ld flushes, want 1.\n", f1 - f0);
				return 1;
			}
		}

		htmlc.base.status = CONTENT_STATUS_DONE;
		htmlc.base.active = 0;
	}
	fprintf(stderr, "=== Test 64 PASS: geometry settles once per JS "
		"execution ===\n");

	/* --- Test 66 (fixes1169, #226): an inline-flex avatar must height the
	 * flex row it sits in ------------------------------------------------
	 *
	 * XenForo 2.2 thread lists (68kmla.org) build each row as
	 * .node-extra{display:flex} > .node-extra-icon > a.avatar{
	 * display:inline-flex;width:48px;height:48px} > img{width:100%;
	 * height:100%}. layout_line's y-placement loop counted the margin
	 * boxes of BOX_INLINE (replaced) and BOX_INLINE_BLOCK toward the line
	 * height but forgot BOX_INLINE_FLEX (and BOX_INLINE_GRID), so the
	 * avatar contributed ZERO height to its line box: the icon cell
	 * measured the text line (19px at 15px font), the flex row cross-sized
	 * to that, and the 48px avatar hung out the bottom, overlapping the
	 * next row's title. Fix (layout.c): the same used_height/y accounting
	 * for BOX_INLINE_FLEX/BOX_INLINE_GRID as inline-block.
	 *
	 * This test runs the REAL layout over the REAL markup/CSS and asserts
	 * the resulting heights. Pre-fix it fails: row/icon measured 19. */
	fprintf(stderr,
		"\n=== Test 66: inline-flex avatar heights the flex row (#226) ===\n");
	{
		static const char *t66_html =
			"<!DOCTYPE html><html><head></head><body>"
			"<div class=\"node-extra\">"
			"<div class=\"node-extra-icon\">"
			"<a class=\"avatar avatar--s\" href=\"#\">"
			"<img class=\"avatar-u2-s\" src=\"x.jpg\" "
			"width=\"48\" height=\"48\" loading=\"lazy\" alt=\"u2\">"
			"</a></div>"
			"<div class=\"node-extra-row\">"
			"<a class=\"node-extra-title\" href=\"#\">"
			"Thread title text here</a></div>"
			"</div></body></html>";
		static const char *t66_css =
			".node-extra { display: flex; }"
			".node-extra-icon { flex: 0 0 auto; padding-right: 8px; }"
			".avatar { display: inline-flex; justify-content: center;"
			" align-items: center; border-radius: 50%;"
			" vertical-align: top; overflow: hidden;"
			" background-blend-mode: multiply; }"
			".avatar--s { width: 48px; height: 48px; }"
			".avatar img { text-indent: 100%; overflow: hidden;"
			" white-space: nowrap; word-wrap: normal; display: block;"
			" border-radius: inherit; width: 100%; height: 100%; }"
			"img { max-width: 100%; height: auto; }"
			"body { font-size: 15px; }";
		struct html_content t66c;
		dom_hubbub_parser *t66p = NULL;
		dom_document *t66doc = NULL;
		dom_node *t66root = NULL;
		css_select_ctx *t66ctx = NULL;
		css_stylesheet *t66ua = NULL;
		css_stylesheet *t66auth = NULL;
		dom_hubbub_parser_params t66params;
		css_stylesheet_params t66sp;
		void *t66_box_ctx = NULL;
		int t66_row_h = -1, t66_icon_h = -1;
		int t66_avatar_h = -1, t66_img_h = -1;
		int t67_blend_mode = -1;
		nserror t66err;
		dom_exception t66derr;
		css_error t66cerr;

		memset(&t66params, 0, sizeof(t66params));
		t66params.enc = NULL;
		t66params.fix_enc = true;
		t66params.enable_script = false;
		t66params.daf = NULL;
		t66derr = dom_hubbub_parser_create(&t66params, &t66p,
				&t66doc);
		if (t66derr != DOM_HUBBUB_OK || t66p == NULL ||
				t66doc == NULL) {
			fprintf(stderr, "FAIL: Test 66 parser create %d\n",
					(int)t66derr);
			return 1;
		}
		t66derr = dom_hubbub_parser_parse_chunk(t66p,
				(const uint8_t *)t66_html, strlen(t66_html));
		if (t66derr != DOM_HUBBUB_OK) {
			fprintf(stderr, "FAIL: Test 66 parse chunk %d\n",
					(int)t66derr);
			return 1;
		}
		t66derr = dom_hubbub_parser_completed(t66p);
		if (t66derr != DOM_HUBBUB_OK) {
			fprintf(stderr, "FAIL: Test 66 parse done %d\n",
					(int)t66derr);
			return 1;
		}
		dom_hubbub_parser_destroy(t66p);

		memset(&t66c, 0, sizeof(t66c));
		t66c.base_url = g_base_url;
		t66c.document = t66doc;
		t66c.quirks = DOM_DOCUMENT_QUIRKS_MODE_NONE;
		t66c.enable_scripting = false;
		if (css_select_ctx_create(&t66ctx) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 66 select_ctx\n");
			return 1;
		}
		t66c.select_ctx = t66ctx;

		/* UA sheet: the same tiny default the main fixture uses. */
		memset(&t66sp, 0, sizeof(t66sp));
		t66sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t66sp.level = CSS_LEVEL_3;
		t66sp.charset = "UTF-8";
		t66sp.url = "resource:default.css";
		t66sp.title = "default";
		t66sp.allow_quirks = false;
		t66sp.inline_style = false;
		t66sp.resolve = harness_css_resolve_url;
		t66sp.resolve_pw = NULL;
		t66cerr = css_stylesheet_create(&t66sp, &t66ua);
		if (t66cerr != CSS_OK) {
			fprintf(stderr, "FAIL: Test 66 UA sheet\n");
			return 1;
		}
		{
			const char *ua_css =
				"html,body,div,span,p{display:block}"
				"span{display:inline}";
			css_error ae = css_stylesheet_append_data(t66ua,
					(const uint8_t *)ua_css, strlen(ua_css));

			if (ae != CSS_OK && ae != CSS_NEEDDATA) {
				fprintf(stderr, "FAIL: Test 66 UA append=%d\n",
						(int)ae);
				return 1;
			}
			(void)css_stylesheet_data_done(t66ua);
		}
		if (css_select_ctx_append_sheet(t66ctx, t66ua,
				CSS_ORIGIN_UA, "screen") != CSS_OK) {
			fprintf(stderr, "FAIL: Test 66 UA append sheet\n");
			return 1;
		}

		/* Author sheet: the real XenForo avatar rules. */
		memset(&t66sp, 0, sizeof(t66sp));
		t66sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t66sp.level = CSS_LEVEL_3;
		t66sp.charset = "UTF-8";
		t66sp.url = "http://local/page.css";
		t66sp.title = "author";
		t66sp.allow_quirks = false;
		t66sp.inline_style = false;
		t66sp.resolve = harness_css_resolve_url;
		t66sp.resolve_pw = NULL;
		t66cerr = css_stylesheet_create(&t66sp, &t66auth);
		if (t66cerr != CSS_OK) {
			fprintf(stderr, "FAIL: Test 66 author sheet\n");
			return 1;
		}
		{
			css_error ae = css_stylesheet_append_data(t66auth,
					(const uint8_t *)t66_css,
					strlen(t66_css));

			if (ae != CSS_OK && ae != CSS_NEEDDATA) {
				fprintf(stderr, "FAIL: Test 66 author append=%d\n",
						(int)ae);
				return 1;
			}
			(void)css_stylesheet_data_done(t66auth);
		}
		if (css_select_ctx_append_sheet(t66ctx, t66auth,
				CSS_ORIGIN_AUTHOR, "screen") != CSS_OK) {
			fprintf(stderr, "FAIL: Test 66 author append sheet\n");
			return 1;
		}

		/* media + unit_len_ctx, mirroring the main fixture so the
		 * cascade matches a real page. */
		t66c.media.type = CSS_MEDIA_SCREEN;
		t66c.media.width = INTTOFIX(993);
		t66c.media.height = INTTOFIX(600);
		t66c.media.orientation = CSS_MEDIA_ORIENTATION_LANDSCAPE;
		t66c.unit_len_ctx.viewport_width = INTTOFIX(993);
		t66c.unit_len_ctx.viewport_height = INTTOFIX(600);
		t66c.unit_len_ctx.device_dpi = INTTOFIX(90);
		t66c.unit_len_ctx.font_size_default = INTTOFIX(16);
		t66c.unit_len_ctx.font_size_minimum = INTTOFIX(8);
		if (lwc_intern_string("*", 1, &t66c.universal) !=
				lwc_error_ok) {
			fprintf(stderr, "FAIL: Test 66 universal\n");
			return 1;
		}

		t66c.base.status = CONTENT_STATUS_LOADING;
		t66c.base.active = 0;
		t66c.base.handler = &g_dummy_handler;

		t66derr = dom_document_get_document_element(t66doc,
				(void *)&t66root);
		if (t66derr != DOM_NO_ERR || t66root == NULL) {
			fprintf(stderr, "FAIL: Test 66 doc element\n");
			return 1;
		}
		g_initial_build_done = 0;
		g_initial_build_ok = false;
		t66err = dom_to_box(t66root, &t66c, initial_build_cb,
				&t66_box_ctx);
		dom_node_unref(t66root);
		if (t66err != NSERROR_OK) {
			fprintf(stderr, "FAIL: Test 66 dom_to_box nerr=%d\n",
					(int)t66err);
			return 1;
		}
		harness_pump_all(100000);
		if (!g_initial_build_done || !g_initial_build_ok) {
			fprintf(stderr, "FAIL: Test 66 initial build done=%d "
					"ok=%d\n", g_initial_build_done,
					(int)g_initial_build_ok);
			return 1;
		}

		/* run the REAL layout at the same width as the --layout
		 * reproduction. */
		{
			extern bool layout_document(struct html_content *content,
					int width, int height);
			bool t66lok;

			t66c.font_func = &harness_layout_table;
			t66c.base.status = CONTENT_STATUS_DONE;
			t66c.base.available_width = 993;
			t66c.base.available_height = 600;
			t66lok = layout_document(&t66c, 993, 600);
			if (!t66lok) {
				fprintf(stderr, "FAIL: Test 66 layout_document\n");
				return 1;
			}
		}
		t66_walk(t66c.layout, &t66_row_h, &t66_icon_h,
				&t66_avatar_h, &t66_img_h, &t67_blend_mode);
		fprintf(stderr, "  row h=%d icon h=%d avatar h=%d img h=%d\n",
				t66_row_h, t66_icon_h, t66_avatar_h, t66_img_h);
		if (t66_row_h != 48 || t66_icon_h != 48 ||
				t66_avatar_h != 48 || t66_img_h != 48) {
			fprintf(stderr, "FAIL: Test 66 -- inline-flex avatar did "
					"not height the flex row: row=%d icon=%d "
					"avatar=%d img=%d (all should be 48; "
					"pre-fixes1169 the row was 19 and the avatar "
					"hung below it, overlapping the next row's "
					"title)\n",
					t66_row_h, t66_icon_h, t66_avatar_h,
					t66_img_h);
			return 1;
		}
		if (t67_blend_mode != CSS_BACKGROUND_BLEND_MODE_MULTIPLY) {
			fprintf(stderr, "FAIL: Test 67 background-blend-mode cascade=%d "
					"expected=%d\n", t67_blend_mode,
					CSS_BACKGROUND_BLEND_MODE_MULTIPLY);
			return 1;
		}
	}
	fprintf(stderr, "=== Test 66 PASS: inline-flex avatar heights the "
			"flex row (row/icon/avatar/img all 48) ===\n");
	fprintf(stderr, "=== Test 67 PASS: background-blend-mode parsed and "
			"cascaded to multiply ===\n");

	/* --- Test 68 (fixes1231): ResizeObserver on document.documentElement/
	 * body must actually FIRE with real size data. Hardware evidence
	 * (2026-08-20, Facebook Bloks checkpoint pages): the old shared
	 * no-op _Observer delivered an EMPTY entries array, so a callback
	 * reading entries[0].contentRect got undefined and the app's
	 * viewport-size-dependent mount never ran. Observes
	 * document.documentElement (the pattern every hardware capture
	 * showed -- target id/tagName logged as "HTML"), pumps the real JS
	 * timer arena, and asserts the callback fired with a non-empty
	 * entries array whose contentRect matches the real viewport size. */
	fprintf(stderr, "\n=== Test 68: ResizeObserver fires with real size "
			"data ===\n");
	{
		extern void macsurf_qjs_pump_all(void);
		const char *ro_setup_js =
			"globalThis.__roFired=0;globalThis.__roW=-1;"
			"globalThis.__roH=-1;globalThis.__roLen=-1;"
			"(function(){"
			"var ro=new ResizeObserver(function(entries){"
				"globalThis.__roFired=1;"
				"globalThis.__roLen=entries.length;"
				"if(entries&&entries[0]&&entries[0].contentRect){"
					"globalThis.__roW=entries[0].contentRect.width;"
					"globalThis.__roH=entries[0].contentRect.height;"
				"}"
			"});"
			"ro.observe(document.documentElement);"
			"})();";
		const char *ro_check_js =
			"if(!globalThis.__roFired)"
				"throw new Error('ASSERT FAIL: RO callback never fired');"
			"if(globalThis.__roLen!==1)"
				"throw new Error('ASSERT FAIL: RO entries.length='"
					"+globalThis.__roLen+' expected 1');"
			"if(globalThis.__roW!==innerWidth||globalThis.__roH!==innerHeight)"
				"throw new Error('ASSERT FAIL: RO contentRect '"
					"+globalThis.__roW+'x'+globalThis.__roH+' != viewport '"
					"+innerWidth+'x'+innerHeight);";
		unsigned char ok1, ok2;
		int pump;

		ok1 = js_exec(thread, (const unsigned char *)ro_setup_js,
				strlen(ro_setup_js), "driver-ro-setup.js");
		if (!ok1) {
			fprintf(stderr, "FAIL: RO setup threw\n");
			return 1;
		}
		for (pump = 0; pump < 8; pump++) {
			macsurf_qjs_pump_all();
			harness_pump_all(1000);
		}
		ok2 = js_exec(thread, (const unsigned char *)ro_check_js,
				strlen(ro_check_js), "driver-ro-check.js");
		fprintf(stderr, "js_exec(ro check) ok=%d\n", (int)ok2);
		if (!ok2) {
			fprintf(stderr, "FAIL: ResizeObserver did not deliver a real "
					"sized entry after pumping timers\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 68 PASS: ResizeObserver delivers a real "
			"contentRect for document.documentElement ===\n");

	/* --- Test 69 (fixes1235): MutationObserver fires after a real
	 * reconvert, through the REAL html_reconvert_done path -- not a bare
	 * timer pump like Tests 5/68. Own ISOLATED fixture (Test 66's pattern),
	 * not the giant shared htmlc/document Tests 1-65 accumulate state on:
	 * a first attempt reusing that shared content hit reconvert
	 * DONE-ENTRY success=0 on its 35th cycle for reasons unrelated to this
	 * feature (34 prior tests' worth of accumulated box-tree/registry
	 * state), which a clean fixture sidesteps entirely, same reasoning
	 * Test 66 already established. Registers an observer on
	 * document.documentElement, appends a real child through the same C binding
	 * React would call, drives an ACTUAL reconvert through html_reconvert_content,
	 * and asserts the observer sees that real target and child. This guards the
	 * bootloader-critical addedNodes path, not merely a synthetic callback. */
	fprintf(stderr, "\n=== Test 69: MutationObserver fires after a real "
			"reconvert ===\n");
	{
		static const char *t69_html =
			"<!DOCTYPE html><html><head></head><body>"
			"<div id=\"root\"><p id=\"p0\">hello</p></div>"
			"</body></html>";
		struct html_content t69c;
		dom_hubbub_parser *t69p = NULL;
		dom_document *t69doc = NULL;
		dom_node *t69root = NULL;
		css_select_ctx *t69ctx = NULL;
		css_stylesheet *t69ua = NULL;
		dom_hubbub_parser_params t69params;
		css_stylesheet_params t69sp;
		void *t69_box_ctx = NULL;
		struct jsheap *t69heap = NULL;
		struct jsthread *t69thread = NULL;
		nserror t69nerr;
		dom_exception t69derr;
		css_error t69cerr;
		int rc;

		memset(&t69params, 0, sizeof(t69params));
		t69params.enc = NULL;
		t69params.fix_enc = true;
		t69params.enable_script = false;
		t69params.daf = NULL;
		t69derr = dom_hubbub_parser_create(&t69params, &t69p, &t69doc);
		if (t69derr != DOM_HUBBUB_OK || t69p == NULL || t69doc == NULL) {
			fprintf(stderr, "FAIL: Test 69 parser create %d\n",
					(int)t69derr);
			return 1;
		}
		t69derr = dom_hubbub_parser_parse_chunk(t69p,
				(const uint8_t *)t69_html, strlen(t69_html));
		if (t69derr != DOM_HUBBUB_OK) {
			fprintf(stderr, "FAIL: Test 69 parse chunk %d\n", (int)t69derr);
			return 1;
		}
		t69derr = dom_hubbub_parser_completed(t69p);
		if (t69derr != DOM_HUBBUB_OK) {
			fprintf(stderr, "FAIL: Test 69 parse done %d\n", (int)t69derr);
			return 1;
		}
		dom_hubbub_parser_destroy(t69p);

		memset(&t69c, 0, sizeof(t69c));
		t69c.base_url = g_base_url;
		t69c.document = t69doc;
		t69c.quirks = DOM_DOCUMENT_QUIRKS_MODE_NONE;
		t69c.enable_scripting = true;
		if (css_select_ctx_create(&t69ctx) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 69 select_ctx\n");
			return 1;
		}
		t69c.select_ctx = t69ctx;

		memset(&t69sp, 0, sizeof(t69sp));
		t69sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t69sp.level = CSS_LEVEL_3;
		t69sp.charset = "UTF-8";
		t69sp.url = "resource:default.css";
		t69sp.title = "default";
		t69sp.allow_quirks = false;
		t69sp.inline_style = false;
		t69sp.resolve = harness_css_resolve_url;
		t69sp.resolve_pw = NULL;
		t69cerr = css_stylesheet_create(&t69sp, &t69ua);
		if (t69cerr != CSS_OK) {
			fprintf(stderr, "FAIL: Test 69 UA sheet\n");
			return 1;
		}
		{
			const char *ua_css =
				"html,body,div,p{display:block}";
			css_error ae = css_stylesheet_append_data(t69ua,
					(const uint8_t *)ua_css, strlen(ua_css));
			if (ae != CSS_OK && ae != CSS_NEEDDATA) {
				fprintf(stderr, "FAIL: Test 69 UA append=%d\n", (int)ae);
				return 1;
			}
			(void)css_stylesheet_data_done(t69ua);
		}
		if (css_select_ctx_append_sheet(t69ctx, t69ua,
				CSS_ORIGIN_UA, "screen") != CSS_OK) {
			fprintf(stderr, "FAIL: Test 69 UA append sheet\n");
			return 1;
		}

		t69c.media.type = CSS_MEDIA_SCREEN;
		t69c.media.width = INTTOFIX(993);
		t69c.media.height = INTTOFIX(600);
		t69c.media.orientation = CSS_MEDIA_ORIENTATION_LANDSCAPE;
		t69c.unit_len_ctx.viewport_width = INTTOFIX(993);
		t69c.unit_len_ctx.viewport_height = INTTOFIX(600);
		t69c.unit_len_ctx.device_dpi = INTTOFIX(90);
		t69c.unit_len_ctx.font_size_default = INTTOFIX(16);
		t69c.unit_len_ctx.font_size_minimum = INTTOFIX(8);
		if (lwc_intern_string("*", 1, &t69c.universal) != lwc_error_ok) {
			fprintf(stderr, "FAIL: Test 69 universal\n");
			return 1;
		}

		t69c.base.status = CONTENT_STATUS_LOADING;
		t69c.base.active = 0;
		t69c.base.handler = &g_dummy_handler;
		macos9_content_register((struct content *)&t69c);

		t69derr = dom_document_get_document_element(t69doc,
				(void *)&t69root);
		if (t69derr != DOM_NO_ERR || t69root == NULL) {
			fprintf(stderr, "FAIL: Test 69 doc element\n");
			return 1;
		}
		g_initial_build_done = 0;
		g_initial_build_ok = false;
		t69nerr = dom_to_box(t69root, &t69c, initial_build_cb,
				&t69_box_ctx);
		dom_node_unref(t69root);
		if (t69nerr != NSERROR_OK) {
			fprintf(stderr, "FAIL: Test 69 dom_to_box nerr=%d\n",
					(int)t69nerr);
			return 1;
		}
		harness_pump_all(100000);
		if (!g_initial_build_done || !g_initial_build_ok) {
			fprintf(stderr, "FAIL: Test 69 initial build done=%d ok=%d\n",
					g_initial_build_done, (int)g_initial_build_ok);
			return 1;
		}
		t69c.base.status = CONTENT_STATUS_DONE;

		t69nerr = js_newheap(20000, &t69heap);
		if (t69nerr != NSERROR_OK || t69heap == NULL) {
			fprintf(stderr, "FAIL: Test 69 js_newheap nerr=%d\n",
					(int)t69nerr);
			return 1;
		}
		t69nerr = js_newthread(t69heap, NULL, (void *)&t69c, &t69thread);
		if (t69nerr != NSERROR_OK || t69thread == NULL) {
			fprintf(stderr, "FAIL: Test 69 js_newthread nerr=%d\n",
					(int)t69nerr);
			return 1;
		}
		t69c.js_thread = t69thread;
		macsurf_js_set_reconvert_enabled(1);

		{
			const char *mo_setup_js =
				"globalThis.__moFired=0;globalThis.__moLen=-1;"
				"globalThis.__moTargetOk=0;globalThis.__moAddedOk=0;"
				"globalThis.__moInnerTargetOk=0;"
				"globalThis.__moInnerAddedOk=0;"
				"globalThis.__moInnerRemovedOk=0;"
				"(function(){"
				"var mo=new MutationObserver(function(records){"
					"globalThis.__moFired=1;"
					"globalThis.__moLen=records.length;"
					"for(var i=0;i<records.length;i++){var r=records[i];"
						"if(r.target!==document.getElementById('p0'))continue;"
						"for(var a=0;a<r.addedNodes.length;a++){"
							"if(r.addedNodes[a].id==='t69-added')"
								"globalThis.__moAddedOk=1;"
							"if(r.addedNodes[a].id==='t69-inner'){"
								"globalThis.__moInnerTargetOk=1;"
								"globalThis.__moInnerAddedOk=1;}}"
						"for(var d=0;d<r.removedNodes.length;d++)"
							"if(r.removedNodes[d].id==='t69-added')"
								"globalThis.__moInnerRemovedOk=1;}"
					"if(globalThis.__moAddedOk)"
						"globalThis.__moTargetOk=1;"
				"});"
				"mo.observe(document.documentElement,"
					"{childList:true,subtree:true,attributes:true});"
				"})();";
			const char *mo_mutate_js =
				"(function(){var n=document.createElement('i');"
				"n.id='t69-added';document.getElementById('p0').appendChild(n);})();";
			const char *mo_check_js =
				"if(!globalThis.__moFired)"
					"throw new Error('ASSERT FAIL: MO callback never "
						"fired');"
				"if(globalThis.__moLen<1)"
					"throw new Error('ASSERT FAIL: MO records.length='"
						"+globalThis.__moLen+' expected >=1');"
				"if(!globalThis.__moTargetOk)"
					"throw new Error('ASSERT FAIL: MO record target was "
						"not the appended-to element');"
				"if(!globalThis.__moAddedOk)"
					"throw new Error('ASSERT FAIL: MO addedNodes lost the "
						"real appended child');";
			const char *mo_inner_mutate_js =
				"document.getElementById('p0').innerHTML="
				"'<b id=\"t69-inner\">innerHTML child</b>';";
			const char *mo_inner_check_js =
				"if(!globalThis.__moInnerTargetOk)"
					"throw new Error('ASSERT FAIL: innerHTML MO record target "
						"was not the replaced element');"
				"if(!globalThis.__moInnerAddedOk)"
					"throw new Error('ASSERT FAIL: innerHTML addedNodes lost "
						"the parsed child');"
				"if(!globalThis.__moInnerRemovedOk)"
					"throw new Error('ASSERT FAIL: innerHTML removedNodes lost "
						"the detached child');";
			unsigned char ok1, ok2, ok3;

			ok1 = js_exec(t69thread, (const unsigned char *)mo_setup_js,
					strlen(mo_setup_js), "driver-mo-setup.js");
			if (!ok1) {
				fprintf(stderr, "FAIL: MO setup threw\n");
				return 1;
			}

			ok2 = js_exec(t69thread, (const unsigned char *)mo_mutate_js,
					strlen(mo_mutate_js), "driver-mo-mutate.js");
			if (!ok2) {
				fprintf(stderr, "FAIL: MO mutate threw\n");
				return 1;
			}
			harness_pump_all(100000);

			t69c.reflowing = false;
			t69c.box_conversion_context = NULL;
			t69c.aborted = false;
			t69c.base.active = 0;
			rc = html_reconvert_content((struct content *)&t69c);
			fprintf(stderr,
					"Test 69 html_reconvert_content rc=%d (0=queued)\n",
					rc);
			if (rc != 0) {
				fprintf(stderr, "FAIL: Test 69 reconvert did not queue\n");
				return 1;
			}
			harness_pump_all(100000);

			ok3 = js_exec(t69thread, (const unsigned char *)mo_check_js,
					strlen(mo_check_js), "driver-mo-check.js");
			fprintf(stderr, "js_exec(mo check) ok=%d\n", (int)ok3);
			if (!ok3) {
				fprintf(stderr, "FAIL: MutationObserver did not deliver "
						"a real record after reconvert completed\n");
				return 1;
			}

			ok2 = js_exec(t69thread,
					(const unsigned char *)mo_inner_mutate_js,
					strlen(mo_inner_mutate_js), "driver-mo-innerhtml.js");
			if (!ok2) {
				fprintf(stderr, "FAIL: MO innerHTML mutation threw\n");
				return 1;
			}
			harness_pump_all(100000);

			t69c.reflowing = false;
			t69c.box_conversion_context = NULL;
			t69c.aborted = false;
			t69c.base.active = 0;
			rc = html_reconvert_content((struct content *)&t69c);
			if (rc != 0) {
				fprintf(stderr, "FAIL: Test 69 innerHTML reconvert did not "
						"queue\n");
				return 1;
			}
			harness_pump_all(100000);

			ok3 = js_exec(t69thread,
					(const unsigned char *)mo_inner_check_js,
					strlen(mo_inner_check_js),
					"driver-mo-innerhtml-check.js");
			if (!ok3) {
				fprintf(stderr, "FAIL: MutationObserver did not report "
						"innerHTML's real replacement\n");
				return 1;
			}
		}
		/* fixes1243 - MUST destroy before t69c (this block's stack-local
		 * html_content) goes out of scope. Without this, t69heap stays
		 * linked in g_heap_list forever and any LATER test that calls the
		 * bare macsurf_qjs_pump_all() (which walks every live heap, not
		 * just its own) dereferences t69c/t69ctx through a dangling stack
		 * pointer -- confirmed in the harness: an ASan
		 * stack-use-after-scope inside macos9_reconvert_flush_now, three
		 * tests later, that had nothing to do with whatever test actually
		 * triggered the pump.
		 *
		 * Realm owner records also retain raw content pointers until the normal
		 * death-row notification clears them. This stack-local fixture does not
		 * go through that lifecycle, so notify before the frame ends. */
		{
			extern void macsurf_js_notify_content_freed(
					struct content *c);
			macsurf_js_notify_content_freed((struct content *)&t69c);
		}
		js_destroyheap(t69heap);
	}
	fprintf(stderr, "=== Test 69 PASS: MutationObserver delivers a real "
			"record after html_reconvert_done ===\n");

	fprintf(stderr, "\n=== Test 70: querySelectorAll + textContent + :not() "
			"+ comma-lists read a real "
			"<script type=\"application/json\"> data island "
			"(#167 Facebook SSR) ===\n");
	{
		/* fixes1240 - the 2026-08-20 Facebook investigation found the
		 * page's splash-screen reveal depends on already-running JS
		 * reading 170 <script type="application/json"> "data island"
		 * elements via document.querySelectorAll + .textContent (the
		 * elements are never EXECUTED -- application/json is not a JS
		 * mimetype in any browser, ours included, and script.c already
		 * correctly skips them: LIFE script_exec: SKIP inline
		 * unsupported mimetype=application/json). Both
		 * qjs_sel_parse/qjs_collect_by_sel's attribute-selector support
		 * (fixes1090c) and qjs_el_get_text_content_data looked complete
		 * by inspection; this proves it end-to-end against a real
		 * parsed DOM rather than trusting the read. */
		static const char *t70_html =
			"<!DOCTYPE html><html><head></head><body>"
			"<script type=\"application/json\" data-sjs=\"1\">"
			"{\"hello\":\"world\",\"n\":42}</script>"
			"<script type=\"application/json\" data-sjs=\"1\" "
			"data-processed=\"1\">{\"already\":\"done\"}</script>"
			"<div id=\"root\"><p id=\"p0\">hello</p>"
			"<p class=\"skip\">skip me</p>"
			"<p class=\"keep\">keep me</p></div>"
			"</body></html>";
		struct html_content t70c;
		dom_hubbub_parser *t70p = NULL;
		dom_document *t70doc = NULL;
		dom_node *t70root = NULL;
		css_select_ctx *t70ctx = NULL;
		css_stylesheet *t70ua = NULL;
		dom_hubbub_parser_params t70params;
		css_stylesheet_params t70sp;
		void *t70_box_ctx = NULL;
		struct jsheap *t70heap = NULL;
		struct jsthread *t70thread = NULL;
		nserror t70nerr;
		dom_exception t70derr;
		css_error t70cerr;

		memset(&t70params, 0, sizeof(t70params));
		t70params.enc = NULL;
		t70params.fix_enc = true;
		t70params.enable_script = false;
		t70params.daf = NULL;
		t70derr = dom_hubbub_parser_create(&t70params, &t70p, &t70doc);
		if (t70derr != DOM_HUBBUB_OK || t70p == NULL || t70doc == NULL) {
			fprintf(stderr, "FAIL: Test 70 parser create %d\n",
					(int)t70derr);
			return 1;
		}
		t70derr = dom_hubbub_parser_parse_chunk(t70p,
				(const uint8_t *)t70_html, strlen(t70_html));
		if (t70derr != DOM_HUBBUB_OK) {
			fprintf(stderr, "FAIL: Test 70 parse chunk %d\n", (int)t70derr);
			return 1;
		}
		t70derr = dom_hubbub_parser_completed(t70p);
		if (t70derr != DOM_HUBBUB_OK) {
			fprintf(stderr, "FAIL: Test 70 parse done %d\n", (int)t70derr);
			return 1;
		}
		dom_hubbub_parser_destroy(t70p);

		memset(&t70c, 0, sizeof(t70c));
		t70c.base_url = g_base_url;
		t70c.document = t70doc;
		t70c.quirks = DOM_DOCUMENT_QUIRKS_MODE_NONE;
		t70c.enable_scripting = true;
		if (css_select_ctx_create(&t70ctx) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 70 select_ctx\n");
			return 1;
		}
		t70c.select_ctx = t70ctx;

		memset(&t70sp, 0, sizeof(t70sp));
		t70sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t70sp.level = CSS_LEVEL_3;
		t70sp.charset = "UTF-8";
		t70sp.url = "resource:default.css";
		t70sp.title = "default";
		t70sp.allow_quirks = false;
		t70sp.inline_style = false;
		t70sp.resolve = harness_css_resolve_url;
		t70sp.resolve_pw = NULL;
		t70cerr = css_stylesheet_create(&t70sp, &t70ua);
		if (t70cerr != CSS_OK) {
			fprintf(stderr, "FAIL: Test 70 UA sheet\n");
			return 1;
		}
		{
			const char *ua_css =
				"html,body,div,p,script{display:block}";
			css_error ae = css_stylesheet_append_data(t70ua,
					(const uint8_t *)ua_css, strlen(ua_css));
			if (ae != CSS_OK && ae != CSS_NEEDDATA) {
				fprintf(stderr, "FAIL: Test 70 UA append=%d\n", (int)ae);
				return 1;
			}
			(void)css_stylesheet_data_done(t70ua);
		}
		if (css_select_ctx_append_sheet(t70ctx, t70ua,
				CSS_ORIGIN_UA, "screen") != CSS_OK) {
			fprintf(stderr, "FAIL: Test 70 UA append sheet\n");
			return 1;
		}

		t70c.media.type = CSS_MEDIA_SCREEN;
		t70c.media.width = INTTOFIX(993);
		t70c.media.height = INTTOFIX(600);
		t70c.media.orientation = CSS_MEDIA_ORIENTATION_LANDSCAPE;
		t70c.unit_len_ctx.viewport_width = INTTOFIX(993);
		t70c.unit_len_ctx.viewport_height = INTTOFIX(600);
		t70c.unit_len_ctx.device_dpi = INTTOFIX(90);
		t70c.unit_len_ctx.font_size_default = INTTOFIX(16);
		t70c.unit_len_ctx.font_size_minimum = INTTOFIX(8);
		if (lwc_intern_string("*", 1, &t70c.universal) != lwc_error_ok) {
			fprintf(stderr, "FAIL: Test 70 universal\n");
			return 1;
		}

		t70c.base.status = CONTENT_STATUS_LOADING;
		t70c.base.active = 0;
		t70c.base.handler = &g_dummy_handler;
		macos9_content_register((struct content *)&t70c);

		t70derr = dom_document_get_document_element(t70doc,
				(void *)&t70root);
		if (t70derr != DOM_NO_ERR || t70root == NULL) {
			fprintf(stderr, "FAIL: Test 70 doc element\n");
			return 1;
		}
		g_initial_build_done = 0;
		g_initial_build_ok = false;
		t70nerr = dom_to_box(t70root, &t70c, initial_build_cb,
				&t70_box_ctx);
		dom_node_unref(t70root);
		if (t70nerr != NSERROR_OK) {
			fprintf(stderr, "FAIL: Test 70 dom_to_box nerr=%d\n",
					(int)t70nerr);
			return 1;
		}
		harness_pump_all(100000);
		if (!g_initial_build_done || !g_initial_build_ok) {
			fprintf(stderr, "FAIL: Test 70 initial build done=%d ok=%d\n",
					g_initial_build_done, (int)g_initial_build_ok);
			return 1;
		}
		t70c.base.status = CONTENT_STATUS_DONE;

		t70nerr = js_newheap(20000, &t70heap);
		if (t70nerr != NSERROR_OK || t70heap == NULL) {
			fprintf(stderr, "FAIL: Test 70 js_newheap nerr=%d\n",
					(int)t70nerr);
			return 1;
		}
		t70nerr = js_newthread(t70heap, NULL, (void *)&t70c, &t70thread);
		if (t70nerr != NSERROR_OK || t70thread == NULL) {
			fprintf(stderr, "FAIL: Test 70 js_newthread nerr=%d\n",
					(int)t70nerr);
			return 1;
		}
		t70c.js_thread = t70thread;

		{
			const char *check_js =
				"globalThis.__t70ok=0;"
				"(function(){"
				"var els=document.querySelectorAll("
					"'script[type=\"application/json\"]');"
				"if(!els||els.length!==2)"
				"throw new Error('ASSERT FAIL: qsa[type=json] found '"
					"+(els?els.length:'null')+' expected 2');"
				"var byAttr=document.querySelectorAll('[data-sjs]');"
				"if(!byAttr||byAttr.length!==2)"
				"throw new Error('ASSERT FAIL: qsa[data-sjs] found '"
					"+(byAttr?byAttr.length:'null')+' expected 2');"
				"var txt=els[0].textContent;"
				"if(typeof txt!=='string'||txt.indexOf('hello')===-1)"
				"throw new Error('ASSERT FAIL: textContent='"
					"+JSON.stringify(txt));"
				"var parsed=JSON.parse(txt);"
				"if(parsed.hello!=='world'||parsed.n!==42)"
				"throw new Error('ASSERT FAIL: parsed wrong: '"
					"+JSON.stringify(parsed));"
				/* fixes1240 (#167) - the ACTUAL Facebook selector
				 * (ServerJSPayloadListener_NEW): compound attribute
				 * selector + :not([attr]). Must exclude the
				 * data-processed one and keep only the real payload. */
				"var un=document.querySelectorAll("
					"'script[data-sjs]:not([data-processed])');"
				"if(!un||un.length!==1)"
				"throw new Error('ASSERT FAIL: qsa :not([data-processed]) "
					"found '+(un?un.length:'null')+' expected 1');"
				"if(JSON.parse(un[0].textContent).hello!=='world')"
				"throw new Error('ASSERT FAIL: :not() selected the "
					"WRONG script element');"
				/* :not(.class) on a plain element, no attribute
				 * involved -- a different code path (qjs_class_has vs
				 * the attr matcher) through the same qjs_simple_match. */
				"var kept=document.querySelectorAll('p:not(.skip)');"
				"if(!kept||kept.length!==2)"
				"throw new Error('ASSERT FAIL: p:not(.skip) found '"
					"+(kept?kept.length:'null')+' expected 2 (p0, "
					"p.keep)');"
				"for(var i=0;i<kept.length;i++)"
				"if(kept[i].className==='skip')"
				"throw new Error('ASSERT FAIL: p:not(.skip) "
					"included the excluded element');"
				/* fixes1242 (#167) - comma-separated selector LISTS,
				 * e.g. Facebook's own
				 * `querySelectorAll('button, [role="button"], "
				 * "[tabindex="0"]')`. Two distinct alternatives that
				 * each match a DIFFERENT element. */
				"var two=document.querySelectorAll('#p0, .keep');"
				"if(!two||two.length!==2)"
				"throw new Error('ASSERT FAIL: qsa(#p0, .keep) found '"
					"+(two?two.length:'null')+' expected 2');"
				/* dedup: an element matching BOTH alternatives must
				 * appear once, not twice -- .keep is also a <p>. */
				"var ded=document.querySelectorAll('p, .keep');"
				"if(!ded||ded.length!==3)"
				"throw new Error('ASSERT FAIL: qsa(p, .keep) found '"
					"+(ded?ded.length:'null')+' expected 3 (no dupes)');"
				/* querySelector (singular): the #id fast path must NOT
				 * fire for a multi-alternative list -- '#nope' alone
				 * matches nothing, so if the fast path wrongly fired on
				 * just the first alternative this would wrongly return
				 * null instead of falling through to find #p0 via the
				 * second alternative. */
				"var one=document.querySelector('#nope, #p0');"
				"if(!one||one.id!=='p0')"
				"throw new Error('ASSERT FAIL: qs(#nope, #p0) = '"
					"+(one?one.id:'null')+' expected p0');"
				"globalThis.__t70ok=1;"
				"})();"
				"if(!globalThis.__t70ok)"
				"throw new Error('ASSERT FAIL: t70ok flag not set');";
			unsigned char ok;

			ok = js_exec(t70thread, (const unsigned char *)check_js,
					strlen(check_js), "driver-t70-check.js");
			fprintf(stderr, "js_exec(t70 check) ok=%d\n", (int)ok);
			if (!ok) {
				fprintf(stderr, "FAIL: Test 70 querySelectorAll/"
						"textContent/:not()/comma-list did not "
						"read the JSON island correctly\n");
				return 1;
			}
		}
		/* Same realm-owner lifetime rule as Test 69: clear this stack-local
		 * fixture before leaving its scope. */
		{
			extern void macsurf_js_notify_content_freed(
					struct content *c);
			macsurf_js_notify_content_freed((struct content *)&t70c);
		}
		js_destroyheap(t70heap);
	}
	fprintf(stderr, "=== Test 70 PASS: querySelectorAll + textContent + "
			":not() + comma-lists read a real JSON data island ===\n");

	fprintf(stderr, "\n=== Test 71: real AbortController/AbortSignal, wired "
			"into fetch() (#167) ===\n");
	{
		/* fixes1243 - the network-dependent half (aborting a REAL
		 * in-flight XHR mid-flight) delegates to XMLHttpRequest.abort(),
		 * already real and already exercised (Test 3, Test 61) --
		 * calling __xhrNativeAbort natively. What's new and needs its own
		 * proof here is the JS-level AbortController/AbortSignal object
		 * itself (construction, event delivery, reason propagation) and
		 * fetch()'s handling of an ALREADY-aborted signal, which is
		 * network-free by design (fetch must never even open the XHR) and
		 * so is safe to assert against in a harness with no real network
		 * mock. */
		const char *ac_js =
			"(function(){"
			"var c=new AbortController();"
			"if(!c.signal)throw new Error('ASSERT FAIL: no .signal');"
			"if(c.signal.aborted)"
			"throw new Error('ASSERT FAIL: aborted=true before abort()');"
			"var fired=0,seen=null;"
			"c.signal.addEventListener('abort',function(ev){"
				"fired++;seen=ev;});"
			"var oaFired=0;"
			"c.signal.onabort=function(){oaFired++;};"
			"c.abort('custom reason');"
			"if(!c.signal.aborted)"
			"throw new Error('ASSERT FAIL: aborted still false after "
				"abort()');"
			"if(c.signal.reason!=='custom reason')"
			"throw new Error('ASSERT FAIL: reason='+c.signal.reason);"
			"if(fired!==1)"
			"throw new Error('ASSERT FAIL: abort listener fired '"
				"+fired+' times, expected 1');"
			"if(!seen||seen.type!=='abort'||seen.target!==c.signal)"
			"throw new Error('ASSERT FAIL: abort event shape wrong');"
			"if(oaFired!==1)"
			"throw new Error('ASSERT FAIL: onabort fired '+oaFired"
				"+' times, expected 1');"
			"c.abort('second call');"
			"if(fired!==1)"
			"throw new Error('ASSERT FAIL: second abort() re-fired "
				"the listener (fired='+fired+')');"
			"if(c.signal.reason!=='custom reason')"
			"throw new Error('ASSERT FAIL: second abort() overwrote "
				"the reason: '+c.signal.reason);"
			/* default reason shape when none given -- real code checks
			 * err.name==='AbortError'. */
			"var c2=new AbortController();"
			"c2.abort();"
			"if(!c2.signal.reason||c2.signal.reason.name!=='AbortError')"
			"throw new Error('ASSERT FAIL: default reason.name='"
				"+(c2.signal.reason&&c2.signal.reason.name));"
			/* AbortSignal.abort() static. */
			"var s=AbortSignal.abort('pre-aborted');"
			"if(!s.aborted||s.reason!=='pre-aborted')"
			"throw new Error('ASSERT FAIL: AbortSignal.abort() static "
				"wrong: aborted='+s.aborted+' reason='+s.reason);"
			"globalThis.__t71sync=1;"
			"})();"
			"if(!globalThis.__t71sync)"
			"throw new Error('ASSERT FAIL: t71sync flag not set');"
			/* fetch() with an ALREADY-aborted signal: must reject with
			 * the signal's reason, and (by construction -- the check
			 * runs before the XHR is ever created) never touch the
			 * network. globalThis flags because js_exec is synchronous
			 * but the promise settles on a later microtask pump. */
			"globalThis.__t71fetchDone=0;globalThis.__t71fetchErr=null;"
			"fetch('https://example.invalid/should-never-be-requested',"
				"{signal:AbortSignal.abort('nope')})"
			".then(function(){globalThis.__t71fetchDone=2;},"
				"function(e){globalThis.__t71fetchDone=1;"
					"globalThis.__t71fetchErr=e;});";
		unsigned char ok;

		ok = js_exec(thread, (const unsigned char *)ac_js,
				strlen(ac_js), "driver-t71-ac.js");
		fprintf(stderr, "js_exec(t71 abortcontroller) ok=%d\n", (int)ok);
		if (!ok) {
			fprintf(stderr, "FAIL: Test 71 AbortController/AbortSignal "
					"assertions threw\n");
			return 1;
		}
		/* fixes1243 - harness_pump_all drains the HARNESS's own mock
		 * scheduler queue (macos9_schedule-style callbacks), not
		 * QuickJS's promise job queue -- the .then() reaction is a
		 * pending JOB, drained only by the real engine pump (Test 12's
		 * pattern), same as every other Promise-based harness test. */
		{
			extern void macsurf_qjs_pump_all(void);
			int pump;
			for (pump = 0; pump < 8; pump++) macsurf_qjs_pump_all();
		}
		{
			const char *check2_js =
				"if(globalThis.__t71fetchDone!==1)"
				"throw new Error('ASSERT FAIL: fetch(aborted signal) "
					"settled as '+globalThis.__t71fetchDone"
					"+' (0=never settled, 2=wrongly resolved), "
					"expected 1 (rejected)');"
				"if(globalThis.__t71fetchErr!=='nope')"
				"throw new Error('ASSERT FAIL: fetch rejection reason='"
					"+globalThis.__t71fetchErr+' expected \"nope\"');";
			unsigned char ok2 = js_exec(thread,
					(const unsigned char *)check2_js,
					strlen(check2_js), "driver-t71-fetch-check.js");
			fprintf(stderr, "js_exec(t71 fetch check) ok=%d\n", (int)ok2);
			if (!ok2) {
				fprintf(stderr, "FAIL: Test 71 fetch() did not reject "
						"an already-aborted signal correctly\n");
				return 1;
			}
		}
	}
	fprintf(stderr, "=== Test 71 PASS: AbortController/AbortSignal real, "
			"fetch() honours an already-aborted signal ===\n");

	fprintf(stderr, "\n=== Test 72: canvas 2D getContext -- real measureText, "
			"honest no-op drawing (#167) ===\n");
	{
		/* fixes1245 - the real point of this test is measureText: proof
		 * that it is a genuine, hardware-backed measurement (scales with
		 * string length and font size) rather than a fabricated
		 * constant, which would be exactly the "confidently wrong
		 * answer" class of bug this project has hit before. Everything
		 * else just needs to not throw. */
		const char *canvas_js =
			"(function(){"
			"var c=document.createElement('canvas');"
			"c.width=200;c.height=100;"
			"var ctx=c.getContext('2d');"
			"if(!ctx)throw new Error('ASSERT FAIL: getContext(2d) "
				"returned falsy');"
			"if(ctx.canvas!==c)"
			"throw new Error('ASSERT FAIL: ctx.canvas !== the canvas "
				"element');"
			"if(ctx!==c.getContext('2d'))"
			"throw new Error('ASSERT FAIL: getContext(2d) did not "
				"return the SAME context on a second call');"
			"if(c.getContext('webgl')!==null)"
			"throw new Error('ASSERT FAIL: getContext(webgl) should "
				"be null (honest -- genuinely unsupported), got '"
					"+c.getContext('webgl'));"
			/* real measurement: longer string -> bigger width, at
			 * least roughly proportional (a fabricated constant width
			 * would fail this outright). */
			"var w1=ctx.measureText('a').width;"
			"var w10=ctx.measureText('aaaaaaaaaa').width;"
			"if(!(w1>0))"
			"throw new Error('ASSERT FAIL: measureText(\"a\").width='"
				"+w1+' expected >0');"
			"if(!(w10>w1*3))"
			"throw new Error('ASSERT FAIL: measureText(\"aaaaaaaaaa\")."
				"width='+w10+' vs \"a\"='+w1+' -- expected roughly "
				"10x, not a flat per-call constant');"
			/* real measurement: bigger font -> bigger width for the
			 * SAME text (proves the font string is actually parsed and
			 * fed into the query, not ignored). */
			"ctx.font='10px sans-serif';"
			"var wSmall=ctx.measureText('hello world').width;"
			"ctx.font='40px sans-serif';"
			"var wBig=ctx.measureText('hello world').width;"
			"if(!(wBig>wSmall*2))"
			"throw new Error('ASSERT FAIL: 40px width='+wBig+' vs "
				"10px width='+wSmall+' -- font size not honoured');"
			/* honest no-op drawing surface: every method callable,
			 * none throw, state properties settable. */
			"ctx.fillStyle='#ff0000';ctx.strokeStyle='blue';"
			"ctx.lineWidth=3;"
			"ctx.beginPath();ctx.moveTo(0,0);ctx.lineTo(10,10);"
			"ctx.arc(5,5,5,0,Math.PI*2);ctx.closePath();"
			"ctx.fill();ctx.stroke();"
			"ctx.fillRect(0,0,10,10);ctx.strokeRect(0,0,10,10);"
			"ctx.clearRect(0,0,10,10);"
			"ctx.save();ctx.translate(1,1);ctx.scale(2,2);ctx.restore();"
			"ctx.fillText('hi',0,0);ctx.strokeText('hi',0,0);"
			"ctx.drawImage(c,0,0);"
			/* real-shaped pixel buffers. */
			"var id=ctx.createImageData(4,3);"
			"if(id.width!==4||id.height!==3)"
			"throw new Error('ASSERT FAIL: createImageData(4,3) size='"
				"+id.width+'x'+id.height);"
			"if(!(id.data instanceof Uint8ClampedArray))"
			"throw new Error('ASSERT FAIL: ImageData.data is not a "
				"Uint8ClampedArray');"
			"if(id.data.length!==4*3*4)"
			"throw new Error('ASSERT FAIL: ImageData.data.length='"
				"+id.data.length+' expected '+(4*3*4));"
			"ctx.putImageData(id,0,0);"
			"var id2=ctx.getImageData(0,0,5,5);"
			"if(id2.width!==5||id2.height!==5)"
			"throw new Error('ASSERT FAIL: getImageData(0,0,5,5) size='"
				"+id2.width+'x'+id2.height);"
			/* toDataURL must never throw/return non-string. */
			"var durl=c.toDataURL();"
			"if(typeof durl!=='string'||durl.indexOf('data:image/png')"
				"!==0)"
			"throw new Error('ASSERT FAIL: toDataURL()='+durl);"
			"globalThis.__t72sync=1;"
			/* No Blob byte backend exists: callback reports null, not a
			 * synthetic zero-byte Blob that claims it encoded this canvas. */
			"globalThis.__t72blobResult='not-called';"
			"c.toBlob(function(b){"
				"globalThis.__t72blobResult=(b===null)?'null':'non-null';"
			"},'image/png');"
			"})();"
			"if(!globalThis.__t72sync)"
			"throw new Error('ASSERT FAIL: t72sync flag not set');";
		unsigned char ok;
		int pump;

		ok = js_exec(thread, (const unsigned char *)canvas_js,
				strlen(canvas_js), "driver-t72-canvas.js");
		fprintf(stderr, "js_exec(t72 canvas) ok=%d\n", (int)ok);
		if (!ok) {
			fprintf(stderr, "FAIL: Test 72 canvas 2D assertions threw\n");
			return 1;
		}
		{
			extern void macsurf_qjs_pump_all(void);
			for (pump = 0; pump < 8; pump++) macsurf_qjs_pump_all();
		}
		{
			const char *check_js =
				"if(globalThis.__t72blobResult!=='null')"
				"throw new Error('ASSERT FAIL: toBlob result='"
					"+globalThis.__t72blobResult"
					"+' (must report unavailable byte encoding as null)');";
			unsigned char ok2 = js_exec(thread,
					(const unsigned char *)check_js,
					strlen(check_js), "driver-t72-blob-check.js");
			fprintf(stderr, "js_exec(t72 blob check) ok=%d\n", (int)ok2);
			if (!ok2) {
				fprintf(stderr, "FAIL: Test 72 toBlob callback did not "
						"fire correctly\n");
				return 1;
			}
		}
	}
	fprintf(stderr, "=== Test 72 PASS: canvas 2D real measureText, honest "
			"no-op drawing surface ===\n");

	fprintf(stderr, "\n=== Test 73: console.error/warn are LIFE-visible and "
			"budget-capped (#167) ===\n");
	{
		/* fixes1246 - the harness's own macsurf_debug_log_write
		 * (harness_stubs.c) prints unconditionally to stderr; it does not
		 * simulate the release-build crash-only gate, so this test can't
		 * observe THAT filtering directly. What it CAN and does verify:
		 * the calls don't throw, and the new budget counter
		 * (g_console_err_audit) decrements exactly once per LIFE-tagged
		 * call and floors at 0 rather than going negative or uncapped --
		 * the actual new logic this round added, as opposed to the
		 * string-prefix constant, which a compile already proves correct. */
		extern long g_console_err_audit;
		long before = g_console_err_audit;
		const char *js1 =
			"console.error('Warning: something recoverable failed');"
			"console.warn('a deprecation notice');"
			"console.log('ordinary log, not budgeted');"
			"console.info('ordinary info, not budgeted');";
		unsigned char ok;
		long after;
		long i;

		ok = js_exec(thread, (const unsigned char *)js1, strlen(js1),
				"driver-t73-console.js");
		fprintf(stderr, "js_exec(t73 console) ok=%d\n", (int)ok);
		if (!ok) {
			fprintf(stderr, "FAIL: Test 73 console.error/warn threw\n");
			return 1;
		}
		after = g_console_err_audit;
		if (before - after != 2) {
			fprintf(stderr, "FAIL: Test 73 budget moved by %ld, "
					"expected exactly 2 (one error + one warn; "
					"log/info must NOT consume it)\n",
					before - after);
			return 1;
		}
		/* Drain the rest of the budget, then confirm it floors at 0 (and
		 * that going past it still doesn't throw). */
		for (i = 0; i < before + 10; i++) {
			const char *js2 = "console.error('spam');";
			ok = js_exec(thread, (const unsigned char *)js2,
					strlen(js2), "driver-t73-spam.js");
			if (!ok) {
				fprintf(stderr, "FAIL: Test 73 console.error threw "
						"on iteration %ld\n", i);
				return 1;
			}
		}
		if (g_console_err_audit != 0) {
			fprintf(stderr, "FAIL: Test 73 budget=%ld after "
					"over-spending, expected floored at 0\n",
					g_console_err_audit);
			return 1;
		}
	}
	fprintf(stderr, "=== Test 73 PASS: console.error/warn LIFE-visible, "
			"budget floors at 0, log/info unaffected ===\n");

	/* Tests 74--83 cover the opt-in Facebook loader diagnostic traps.  Those
	 * traps are intentionally not part of a production browser realm: the page
	 * must own its loader globals. Build the harness with
	 * -DMACSURF_JS_FB_LOADER_TRAP=1 when specifically auditing that diagnostic
	 * instrumentation. */
#if MACSURF_JS_FB_LOADER_TRAP
	fprintf(stderr, "\n=== Test 74: __onBeforeModuleFactory require-trace "
			"survives the page's own reset and never throws (#167) ===\n");
	{
		/* fixes1247 - simulates the exact real-world shape confirmed in
		 * Facebook's own bundle: the page's bootstrap does
		 * `t.__onBeforeModuleFactory=null;` as part of its OWN init
		 * (unconditional, no guard), then its require() dispatch calls
		 * `t.__onBeforeModuleFactory==null||t.__onBeforeModuleFactory(l)`
		 * for every module -- WITH NO TRY/CATCH AROUND THAT CALL. Both
		 * properties matter: our defineProperty trap must survive the
		 * plain-assignment reset, AND the hook itself must never throw
		 * no matter what `l` looks like (malformed module records
		 * included), since a throw here would abort the PAGE's own
		 * require() call, not just this diagnostic. */
		const char *js =
			"(function(){"
			/* the page's own reset attempt -- must NOT actually null
			 * out our hook. */
			"window.__onBeforeModuleFactory=null;"
			"if(typeof window.__onBeforeModuleFactory!=='function')"
			"throw new Error('ASSERT FAIL: page\\'s own \"=null\" "
				"reset defeated the require-trace hook');"
			/* normal calls: a watched id, an unwatched id, the same "
			 * watched id again (dedup path). None may throw. */
			"window.__onBeforeModuleFactory({id:'ServerJSPayloadListener_NEW'});"
			"window.__onBeforeModuleFactory({id:'SomeUnrelatedModule'});"
			"window.__onBeforeModuleFactory({id:'ServerJSPayloadListener_NEW'});"
			/* malformed records: null, undefined, no .id, a non-string "
			 * .id, and calling with no argument at all. Every one of
			 * these must be swallowed silently, not thrown. */
			"window.__onBeforeModuleFactory(null);"
			"window.__onBeforeModuleFactory(undefined);"
			"window.__onBeforeModuleFactory({});"
			"window.__onBeforeModuleFactory({id:42});"
			"window.__onBeforeModuleFactory({id:null});"
			"window.__onBeforeModuleFactory();"
			"globalThis.__t74sync=1;"
			"})();"
			"if(!globalThis.__t74sync)"
			"throw new Error('ASSERT FAIL: t74sync flag not set');";
		unsigned char ok;

		ok = js_exec(thread, (const unsigned char *)js, strlen(js),
				"driver-t74-requiretrace.js");
		fprintf(stderr, "js_exec(t74 require-trace) ok=%d\n", (int)ok);
		if (!ok) {
			fprintf(stderr, "FAIL: Test 74 require-trace hook threw "
					"(would have broken the page's own "
					"require() in real use)\n");
			return 1;
		}
		{
			const char *js2 =
				"var t=globalThis.__msRequireTraceTotal();"
				"if(t!==9)"
				"throw new Error('ASSERT FAIL: total='+t+' expected 9 "
					"(every call counts, watched or not, "
					"well-formed or not)');";
			unsigned char ok2 = js_exec(thread,
					(const unsigned char *)js2, strlen(js2),
					"driver-t74-total-check.js");
			fprintf(stderr, "js_exec(t74 total check) ok=%d\n",
					(int)ok2);
			if (!ok2) {
				fprintf(stderr, "FAIL: Test 74 __msRequireTraceTotal() "
						"assertion failed\n");
				return 1;
			}
		}
	}
	fprintf(stderr, "=== Test 74 PASS: require-trace hook survives the "
			"page's own reset, never throws on any input ===\n");

	/* ---------------------------------------------------------------
	 * fixes1268a (#167) - custom-property definitions are retained
	 * PER RULE, not collapsed into one per-sheet bucket.
	 *
	 * This is the storage-layer control for the 1268 series. Before
	 * 1268a the only store was css_stylesheet::custom_properties, a
	 * single last-write-wins list per sheet: three rules defining
	 * "--x" left exactly ONE surviving value, so the selector and
	 * @media scope each definition was written under were gone before
	 * selection ever ran. That is what hands facebook.com the dark
	 * --web-wash on a light-mode page.
	 *
	 * The assertion is a COUNT (three distinct retained values), not a
	 * boolean: a boolean "did we keep any" passes on the broken build.
	 * Pre-1268a this test finds 0 rule-scoped entries and fails.
	 * ------------------------------------------------------------- */
	fprintf(stderr, "\n=== Test 75: custom-property definitions are "
			"rule-scoped, not sheet-global (fixes1268a) ===\n");
	{
		const char *t75_css =
			".light { --x: green; color: black }"
			".dark  { --x: white }"
			"@media print { .mq { --x: black } }";
		css_stylesheet *t75sheet = NULL;
		css_stylesheet_params t75sp;
		css_error t75err;
		char t75vals[8][64];
		int t75n = 0;
		int t75distinct = 0;
		int i, j;
		int t75sheet_global = 0;

		memset(&t75sp, 0, sizeof(t75sp));
		t75sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t75sp.level = CSS_LEVEL_3;
		t75sp.charset = "UTF-8";
		t75sp.url = "http://local/t75.css";
		t75sp.title = "t75";
		t75sp.allow_quirks = false;
		t75sp.inline_style = false;
		t75sp.resolve = harness_css_resolve_url;
		t75sp.resolve_pw = NULL;

		if (css_stylesheet_create(&t75sp, &t75sheet) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 75 sheet create\n");
			return 1;
		}
		t75err = css_stylesheet_append_data(t75sheet,
				(const uint8_t *)t75_css, strlen(t75_css));
		if (t75err != CSS_OK && t75err != CSS_NEEDDATA) {
			fprintf(stderr, "FAIL: Test 75 append=%d\n",
					(int)t75err);
			return 1;
		}
		if (css_stylesheet_data_done(t75sheet) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 75 data_done\n");
			return 1;
		}

		/* Walk every rule in the sheet, descending into @media, and
		 * collect the rule-scoped "--x" values. */
		t75n = cssprobe_rule_custom_props(t75sheet, "x", t75vals,
				(int)(sizeof(t75vals) / sizeof(t75vals[0])));

		for (i = 0; i < t75n; i++) {
			int seen = 0;
			for (j = 0; j < i; j++)
				if (strcmp(t75vals[i], t75vals[j]) == 0)
					seen = 1;
			if (!seen)
				t75distinct++;
			fprintf(stderr, "  rule-scoped --x[%d] = '%s'\n",
					i, t75vals[i]);
		}

		t75sheet_global = cssprobe_sheet_custom_props(t75sheet, "x");
		fprintf(stderr, "  rule-scoped entries=%d distinct=%d, "
				"sheet-global entries=%d\n",
				t75n, t75distinct, t75sheet_global);

		if (t75n != 3 || t75distinct != 3) {
			fprintf(stderr, "FAIL: Test 75 -- expected 3 rule-scoped "
					"--x definitions with 3 distinct values, "
					"got n=%d distinct=%d. Pre-fixes1268a "
					"this is 0/0: definitions went only to "
					"the per-sheet last-write-wins list, "
					"discarding the selector and @media "
					"scope each was written under.\n",
					t75n, t75distinct);
			return 1;
		}
		if (t75sheet_global != 1) {
			fprintf(stderr, "FAIL: Test 75 -- the legacy sheet-global "
					"list should still hold exactly 1 "
					"surviving --x (last-write-wins) while "
					"1268a dual-writes; got %d. If this "
					"changed, resolution semantics moved "
					"before 1268b/1268e intended them to.\n",
					t75sheet_global);
			return 1;
		}

		css_stylesheet_destroy(t75sheet);
	}
	fprintf(stderr, "=== Test 75 PASS: 3 rules retain 3 distinct --x "
			"values, incl. the one inside @media print ===\n");

	/* ---------------------------------------------------------------
	 * fixes1268b (#167) - custom properties resolve against the
	 * ELEMENT's environment, so selector scope and @media scope are
	 * both honoured.
	 *
	 * Both cases here have their definition and their consumer on the
	 * SAME element, which is exactly what 1268b alone can fix:
	 * inheritance from an ancestor is 1268c, and a definition from a
	 * rule that cascades later is 1268d.
	 *
	 * Each case is chosen so the OLD per-sheet last-write-wins store
	 * gives a different, wrong answer:
	 *   .light expects green, but the sheet-global store's last --x is
	 *          .dark's blue, so a broken build paints it blue;
	 *   .mq    expects white, but the sheet-global store's last --w is
	 *          the one inside @media print (black), which must never
	 *          apply on screen.
	 * ------------------------------------------------------------- */
	fprintf(stderr, "\n=== Test 76: custom properties honour selector "
			"scope and @media scope (fixes1268b) ===\n");
	{
		const char *t76_html =
			"<html><body>"
			"<div class=\"light\">L</div>"
			"<div class=\"dark\">D</div>"
			"<p class=\"mq\">M</p>"
			"</body></html>";
		const char *t76_css =
			".light { --x: rgb(0,128,0); color: var(--x) }"
			".dark  { --x: rgb(0,0,255); color: var(--x) }"
			".mq    { --w: rgb(255,255,255); color: var(--w) }"
			"@media print { .mq { --w: rgb(0,0,0) } }";
		struct html_content t76c;
		dom_hubbub_parser *t76p = NULL;
		dom_document *t76doc = NULL;
		dom_node *t76root = NULL;
		css_select_ctx *t76ctx = NULL;
		css_stylesheet *t76ua = NULL;
		css_stylesheet *t76auth = NULL;
		dom_hubbub_parser_params t76params;
		css_stylesheet_params t76sp;
		void *t76_box_ctx = NULL;
		long t76_light, t76_dark, t76_mq;
		int t76_bad = 0;
		nserror t76err;
		dom_exception t76derr;

		memset(&t76params, 0, sizeof(t76params));
		t76params.fix_enc = true;
		t76derr = dom_hubbub_parser_create(&t76params, &t76p, &t76doc);
		if (t76derr != DOM_HUBBUB_OK || t76p == NULL) {
			fprintf(stderr, "FAIL: Test 76 parser create %d\n",
					(int)t76derr);
			return 1;
		}
		if (dom_hubbub_parser_parse_chunk(t76p,
				(const uint8_t *)t76_html,
				strlen(t76_html)) != DOM_HUBBUB_OK ||
				dom_hubbub_parser_completed(t76p) !=
					DOM_HUBBUB_OK) {
			fprintf(stderr, "FAIL: Test 76 parse\n");
			return 1;
		}
		dom_hubbub_parser_destroy(t76p);

		memset(&t76c, 0, sizeof(t76c));
		t76c.base_url = g_base_url;
		t76c.document = t76doc;
		t76c.quirks = DOM_DOCUMENT_QUIRKS_MODE_NONE;
		t76c.enable_scripting = false;
		if (css_select_ctx_create(&t76ctx) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 76 select_ctx\n");
			return 1;
		}
		t76c.select_ctx = t76ctx;

		memset(&t76sp, 0, sizeof(t76sp));
		t76sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t76sp.level = CSS_LEVEL_3;
		t76sp.charset = "UTF-8";
		t76sp.url = "resource:default.css";
		t76sp.title = "default";
		t76sp.resolve = harness_css_resolve_url;
		if (css_stylesheet_create(&t76sp, &t76ua) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 76 UA sheet\n");
			return 1;
		}
		{
			const char *ua = "html,body,div,p{display:block}";
			(void)css_stylesheet_append_data(t76ua,
					(const uint8_t *)ua, strlen(ua));
			(void)css_stylesheet_data_done(t76ua);
		}
		if (css_select_ctx_append_sheet(t76ctx, t76ua,
				CSS_ORIGIN_UA, "screen") != CSS_OK) {
			fprintf(stderr, "FAIL: Test 76 UA append\n");
			return 1;
		}

		memset(&t76sp, 0, sizeof(t76sp));
		t76sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t76sp.level = CSS_LEVEL_3;
		t76sp.charset = "UTF-8";
		t76sp.url = "http://local/t76.css";
		t76sp.title = "author";
		t76sp.resolve = harness_css_resolve_url;
		if (css_stylesheet_create(&t76sp, &t76auth) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 76 author sheet\n");
			return 1;
		}
		{
			css_error ae = css_stylesheet_append_data(t76auth,
					(const uint8_t *)t76_css,
					strlen(t76_css));
			if (ae != CSS_OK && ae != CSS_NEEDDATA) {
				fprintf(stderr, "FAIL: Test 76 author append=%d\n",
						(int)ae);
				return 1;
			}
			(void)css_stylesheet_data_done(t76auth);
		}
		if (css_select_ctx_append_sheet(t76ctx, t76auth,
				CSS_ORIGIN_AUTHOR, "screen") != CSS_OK) {
			fprintf(stderr, "FAIL: Test 76 author append sheet\n");
			return 1;
		}

		/* CSS_MEDIA_SCREEN + sheets appended as "screen": without
		 * both, the cascade silently yields initial values. */
		t76c.media.type = CSS_MEDIA_SCREEN;
		t76c.media.width = INTTOFIX(800);
		t76c.media.height = INTTOFIX(600);
		t76c.media.orientation = CSS_MEDIA_ORIENTATION_LANDSCAPE;
		t76c.unit_len_ctx.viewport_width = INTTOFIX(800);
		t76c.unit_len_ctx.viewport_height = INTTOFIX(600);
		t76c.unit_len_ctx.device_dpi = INTTOFIX(90);
		t76c.unit_len_ctx.font_size_default = INTTOFIX(16);
		t76c.unit_len_ctx.font_size_minimum = INTTOFIX(8);
		if (lwc_intern_string("*", 1, &t76c.universal) !=
				lwc_error_ok) {
			fprintf(stderr, "FAIL: Test 76 universal\n");
			return 1;
		}
		t76c.base.status = CONTENT_STATUS_LOADING;
		t76c.base.active = 0;
		t76c.base.handler = &g_dummy_handler;

		if (dom_document_get_document_element(t76doc,
				(void *)&t76root) != DOM_NO_ERR ||
				t76root == NULL) {
			fprintf(stderr, "FAIL: Test 76 doc element\n");
			return 1;
		}
		g_initial_build_done = 0;
		g_initial_build_ok = false;
		t76err = dom_to_box(t76root, &t76c, initial_build_cb,
				&t76_box_ctx);
		dom_node_unref(t76root);
		if (t76err != NSERROR_OK) {
			fprintf(stderr, "FAIL: Test 76 dom_to_box=%d\n",
					(int)t76err);
			return 1;
		}
		harness_pump_all(100000);
		if (!g_initial_build_done || !g_initial_build_ok) {
			fprintf(stderr, "FAIL: Test 76 build done=%d ok=%d\n",
					g_initial_build_done,
					(int)g_initial_build_ok);
			return 1;
		}

		t76_light = t76_color_of(t76c.layout, "light");
		t76_dark  = t76_color_of(t76c.layout, "dark");
		t76_mq    = t76_color_of(t76c.layout, "mq");
		fprintf(stderr, "  .light=#%06lX (want #008000)  "
				".dark=#%06lX (want #0000FF)  "
				".mq=#%06lX (want #FFFFFF)\n",
				t76_light, t76_dark, t76_mq);

		if (t76_light != 0x008000L) {
			fprintf(stderr, "FAIL: Test 76 selector scoping -- "
					".light resolved var(--x) to #%06lX, "
					"expected #008000. #0000FF means the "
					"per-sheet last-write-wins store "
					"answered with .dark's definition, "
					"which is the facebook.com "
					"--web-wash defect exactly.\n",
					t76_light);
			t76_bad = 1;
		}
		if (t76_dark != 0x0000FFL) {
			fprintf(stderr, "FAIL: Test 76 selector scoping -- "
					".dark resolved var(--x) to #%06lX, "
					"expected #0000FF\n", t76_dark);
			t76_bad = 1;
		}
		if (t76_mq != 0xFFFFFFL) {
			fprintf(stderr, "FAIL: Test 76 media scoping -- "
					".mq resolved var(--w) to #%06lX, "
					"expected #FFFFFF. #000000 means a "
					"definition inside @media print "
					"reached a screen cascade.\n",
					t76_mq);
			t76_bad = 1;
		}
		if (t76_bad)
			return 1;
	}
	fprintf(stderr, "=== Test 76 PASS: selector scope and @media scope "
			"both honoured by var() resolution ===\n");

	/* ---------------------------------------------------------------
	 * fixes1268c (#167) - custom properties INHERIT, with the
	 * computed-value semantics CSS Variables 1 requires.
	 *
	 * Cases, in fixture order:
	 *   .child      basic inheritance from an ancestor
	 *   .ovr        a local definition overrides the inherited one
	 *   .isoA/.isoB sibling isolation - two subtrees, same name
	 *   .leaf       inheritance through an intermediate element
	 *   .other      NEGATIVE: a non-descendant must NOT see the value
	 *               and must fall back to the var() fallback. This is
	 *               the test that catches an accidental document-global
	 *               carry-over, which is exactly what the old per-sheet
	 *               store was.
	 *   .vc         computed-value timing: the parent's "--b: var(--a)"
	 *               is substituted AT THE PARENT, so the child gets the
	 *               PARENT's --a even though it redefines --a itself.
	 *               Verified against Chrome, which returns red here.
	 * ------------------------------------------------------------- */
	fprintf(stderr, "\n=== Test 77: custom-property inheritance and "
			"computed-value timing (fixes1268c) ===\n");
	{
		const char *t77_html =
			"<html><body>"
			"<div class=\"parent\"><p class=\"child\">C</p>"
			"<p class=\"ovr\">O</p></div>"
			"<div class=\"isoA\"><p class=\"ia\">A</p></div>"
			"<div class=\"isoB\"><p class=\"ib\">B</p></div>"
			"<div class=\"root\"><div class=\"mid\">"
			"<p class=\"leaf\">L</p></div></div>"
			"<p class=\"other\">N</p>"
			"<div class=\"vp\"><p class=\"vc\">V</p></div>"
			"<p class=\"use scope\">U</p>"
			"</body></html>";
		const char *t77_css =
			".parent { --x: rgb(255,0,0) }"
			".child  { color: var(--x) }"
			".ovr    { --x: rgb(0,0,255); color: var(--x) }"
			".isoA   { --y: rgb(0,128,0) }"
			".isoB   { --y: rgb(0,0,255) }"
			".ia     { color: var(--y) }"
			".ib     { color: var(--y) }"
			".root   { --z: rgb(128,0,128) }"
			".leaf   { color: var(--z) }"
			".other  { color: var(--x, rgb(0,128,0)) }"
			".vp     { --a: rgb(255,0,0); --b: var(--a) }"
			".vc     { --a: rgb(0,0,255); color: var(--b) }"
			/* fixes1268d - the consumer's rule cascades BEFORE
			 * the rule that defines the property. Resolving
			 * var() inline during cascade_style could never see
			 * .scope; the second pass can. */
			".use    { color: var(--w, rgb(255,0,0)) }"
			".scope  { --w: rgb(0,0,255) }";
		struct html_content t77c;
		dom_hubbub_parser *t77p = NULL;
		dom_document *t77doc = NULL;
		dom_node *t77root = NULL;
		css_select_ctx *t77ctx = NULL;
		css_stylesheet *t77ua = NULL;
		css_stylesheet *t77auth = NULL;
		dom_hubbub_parser_params t77params;
		css_stylesheet_params t77sp;
		void *t77_box_ctx = NULL;
		int t77_bad = 0;
		int k;
		nserror t77err;
		static const struct {
			const char *cls;
			long want;
			const char *what;
		} t77_want[] = {
			{ "child", 0xFF0000L, "basic inheritance" },
			{ "ovr",   0x0000FFL, "local override" },
			{ "ia",    0x008000L, "sibling isolation A" },
			{ "ib",    0x0000FFL, "sibling isolation B" },
			{ "leaf",  0x800080L, "inheritance through mid" },
			{ "other", 0x008000L, "NEGATIVE: non-descendant "
					"falls back" },
			{ "vc",    0xFF0000L, "computed-value timing "
					"(parent's --a, not child's)" },
			{ "use",   0x0000FFL, "later rule's definition "
					"reaches an earlier consumer" }
		};

		memset(&t77params, 0, sizeof(t77params));
		t77params.fix_enc = true;
		if (dom_hubbub_parser_create(&t77params, &t77p, &t77doc) !=
				DOM_HUBBUB_OK) {
			fprintf(stderr, "FAIL: Test 77 parser create\n");
			return 1;
		}
		if (dom_hubbub_parser_parse_chunk(t77p,
				(const uint8_t *)t77_html,
				strlen(t77_html)) != DOM_HUBBUB_OK ||
				dom_hubbub_parser_completed(t77p) !=
					DOM_HUBBUB_OK) {
			fprintf(stderr, "FAIL: Test 77 parse\n");
			return 1;
		}
		dom_hubbub_parser_destroy(t77p);

		memset(&t77c, 0, sizeof(t77c));
		t77c.base_url = g_base_url;
		t77c.document = t77doc;
		t77c.quirks = DOM_DOCUMENT_QUIRKS_MODE_NONE;
		t77c.enable_scripting = false;
		if (css_select_ctx_create(&t77ctx) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 77 select_ctx\n");
			return 1;
		}
		t77c.select_ctx = t77ctx;

		memset(&t77sp, 0, sizeof(t77sp));
		t77sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t77sp.level = CSS_LEVEL_3;
		t77sp.charset = "UTF-8";
		t77sp.url = "resource:default.css";
		t77sp.title = "default";
		t77sp.resolve = harness_css_resolve_url;
		if (css_stylesheet_create(&t77sp, &t77ua) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 77 UA sheet\n");
			return 1;
		}
		{
			const char *ua = "html,body,div,p{display:block}";
			(void)css_stylesheet_append_data(t77ua,
					(const uint8_t *)ua, strlen(ua));
			(void)css_stylesheet_data_done(t77ua);
		}
		(void)css_select_ctx_append_sheet(t77ctx, t77ua,
				CSS_ORIGIN_UA, "screen");

		memset(&t77sp, 0, sizeof(t77sp));
		t77sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t77sp.level = CSS_LEVEL_3;
		t77sp.charset = "UTF-8";
		t77sp.url = "http://local/t77.css";
		t77sp.title = "author";
		t77sp.resolve = harness_css_resolve_url;
		if (css_stylesheet_create(&t77sp, &t77auth) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 77 author sheet\n");
			return 1;
		}
		{
			css_error ae = css_stylesheet_append_data(t77auth,
					(const uint8_t *)t77_css,
					strlen(t77_css));
			if (ae != CSS_OK && ae != CSS_NEEDDATA) {
				fprintf(stderr, "FAIL: Test 77 append=%d\n",
						(int)ae);
				return 1;
			}
			(void)css_stylesheet_data_done(t77auth);
		}
		(void)css_select_ctx_append_sheet(t77ctx, t77auth,
				CSS_ORIGIN_AUTHOR, "screen");

		t77c.media.type = CSS_MEDIA_SCREEN;
		t77c.media.width = INTTOFIX(800);
		t77c.media.height = INTTOFIX(600);
		t77c.media.orientation = CSS_MEDIA_ORIENTATION_LANDSCAPE;
		t77c.unit_len_ctx.viewport_width = INTTOFIX(800);
		t77c.unit_len_ctx.viewport_height = INTTOFIX(600);
		t77c.unit_len_ctx.device_dpi = INTTOFIX(90);
		t77c.unit_len_ctx.font_size_default = INTTOFIX(16);
		t77c.unit_len_ctx.font_size_minimum = INTTOFIX(8);
		if (lwc_intern_string("*", 1, &t77c.universal) !=
				lwc_error_ok) {
			fprintf(stderr, "FAIL: Test 77 universal\n");
			return 1;
		}
		t77c.base.status = CONTENT_STATUS_LOADING;
		t77c.base.active = 0;
		t77c.base.handler = &g_dummy_handler;

		if (dom_document_get_document_element(t77doc,
				(void *)&t77root) != DOM_NO_ERR ||
				t77root == NULL) {
			fprintf(stderr, "FAIL: Test 77 doc element\n");
			return 1;
		}
		g_initial_build_done = 0;
		g_initial_build_ok = false;
		t77err = dom_to_box(t77root, &t77c, initial_build_cb,
				&t77_box_ctx);
		dom_node_unref(t77root);
		if (t77err != NSERROR_OK) {
			fprintf(stderr, "FAIL: Test 77 dom_to_box=%d\n",
					(int)t77err);
			return 1;
		}
		harness_pump_all(100000);
		if (!g_initial_build_done || !g_initial_build_ok) {
			fprintf(stderr, "FAIL: Test 77 build\n");
			return 1;
		}

		for (k = 0; k < (int)(sizeof(t77_want) /
				sizeof(t77_want[0])); k++) {
			long got = t76_color_of(t77c.layout,
					t77_want[k].cls);
			fprintf(stderr, "  .%-6s = #%06lX  want #%06lX  %s\n",
					t77_want[k].cls, got,
					t77_want[k].want, t77_want[k].what);
			if (got != t77_want[k].want) {
				fprintf(stderr, "FAIL: Test 77 .%s -- %s: "
						"got #%06lX expected #%06lX\n",
						t77_want[k].cls,
						t77_want[k].what, got,
						t77_want[k].want);
				t77_bad = 1;
			}
		}
		if (t77_bad)
			return 1;
	}
	fprintf(stderr, "=== Test 77 PASS: inheritance, override, sibling "
			"isolation, non-descendant isolation, and "
			"computed-value timing ===\n");

	/* ---------------------------------------------------------------
	 * fixes1269 (#167) - style sharing must not leak custom properties,
	 * plus the remaining var() semantics.
	 *
	 * The sharing concern is real and specific: sharing skips
	 * cascade_style entirely, so a sharer contributes none of its own
	 * definitions. Two siblings can have IDENTICAL ordinary computed
	 * styles (both merely define a custom property) while their
	 * descendants require different environments. Comparing only the
	 * inherited environment cannot establish equivalence.
	 *
	 * What actually protects it is that sharing already demands the
	 * same element name, the same class list in order, no id on either
	 * node, and no attribute / sibling / pseudo-class taint - so two
	 * shareable nodes matched the same rules and therefore carry the
	 * same definitions. This test proves that rather than assuming it,
	 * from both directions:
	 *
	 *   .a / .b   different classes, same ordinary style, must NOT
	 *             cross-contaminate (the reported case)
	 *   .t / .t   identical classes, genuinely shareable, WITH custom
	 *             properties - exercises the adopt path on purpose
	 *
	 * The adoption COUNT is asserted too. Without it this test would
	 * pass just as happily on a build where sharing never triggered,
	 * which is the classic false green.
	 * ------------------------------------------------------------- */
	fprintf(stderr, "\n=== Test 78: style sharing keeps custom-property "
			"environments separate; var() edge cases "
			"(fixes1269) ===\n");
	{
		const char *t78_html =
			"<html><body>"
			"<div class=\"a\"><span class=\"as\">A</span></div>"
			"<div class=\"b\"><span class=\"bs\">B</span></div>"
			"<div class=\"t\"><span class=\"t1\">1</span></div>"
			"<div class=\"t\"><span class=\"t2\">2</span></div>"
			"<div class=\"mp\"><span class=\"mk\">M</span></div>"
			"<span class=\"fb\">F</span>"
			"<span class=\"nfb\">N</span>"
			"<span class=\"cy\">C</span>"
			"</body></html>";
		const char *t78_css =
			".a { --x: rgb(255,0,0) }"
			".b { --x: rgb(0,0,255) }"
			".a .as { color: var(--x) }"
			".b .bs { color: var(--x) }"
			".t { --q: rgb(0,128,0) }"
			".t .t1 { color: var(--q) }"
			".t .t2 { color: var(--q) }"
			/* multi-hop computed substitution */
			".mp { --m1: rgb(255,0,0); --m2: var(--m1);"
			"      --m3: var(--m2) }"
			".mk { --m1: rgb(0,0,255); color: var(--m3) }"
			/* fallback, nested fallback */
			".fb  { color: var(--nope, rgb(0,128,0)) }"
			".nfb { color: var(--nope, var(--nope2, rgb(0,0,255))) }"
			/* var() cycle: --z is invalid at computed-value time,
			 * so the consumer takes its fallback (Chrome: green) */
			".cy { --z: var(--w); --w: var(--z);"
			"      color: var(--z, rgb(0,128,0)) }";
		struct html_content t78c;
		dom_hubbub_parser *t78p = NULL;
		dom_document *t78doc = NULL;
		dom_node *t78root = NULL;
		css_select_ctx *t78ctx = NULL;
		css_stylesheet *t78ua = NULL;
		css_stylesheet *t78auth = NULL;
		dom_hubbub_parser_params t78params;
		css_stylesheet_params t78sp;
		void *t78_box_ctx = NULL;
		uint32_t share_before, share_after;
		int t78_bad = 0;
		int k;
		nserror t78err;
		static const struct {
			const char *cls;
			long want;
			const char *what;
		} t78_want[] = {
			{ "as",  0xFF0000L, "sharing: .a subtree keeps red" },
			{ "bs",  0x0000FFL, "sharing: .b subtree keeps blue" },
			{ "t1",  0x008000L, "shareable .t sibling 1" },
			{ "t2",  0x008000L, "shareable .t sibling 2" },
			{ "mk",  0xFF0000L, "multi-hop: parent's --m1" },
			{ "fb",  0x008000L, "var() fallback" },
			{ "nfb", 0x0000FFL, "nested var() fallback" },
			{ "cy",  0x008000L, "var() cycle is invalid -> "
					"fallback, not black" }
		};

		memset(&t78params, 0, sizeof(t78params));
		t78params.fix_enc = true;
		if (dom_hubbub_parser_create(&t78params, &t78p, &t78doc) !=
				DOM_HUBBUB_OK) {
			fprintf(stderr, "FAIL: Test 78 parser create\n");
			return 1;
		}
		if (dom_hubbub_parser_parse_chunk(t78p,
				(const uint8_t *)t78_html,
				strlen(t78_html)) != DOM_HUBBUB_OK ||
				dom_hubbub_parser_completed(t78p) !=
					DOM_HUBBUB_OK) {
			fprintf(stderr, "FAIL: Test 78 parse\n");
			return 1;
		}
		dom_hubbub_parser_destroy(t78p);

		memset(&t78c, 0, sizeof(t78c));
		t78c.base_url = g_base_url;
		t78c.document = t78doc;
		t78c.quirks = DOM_DOCUMENT_QUIRKS_MODE_NONE;
		t78c.enable_scripting = false;
		if (css_select_ctx_create(&t78ctx) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 78 select_ctx\n");
			return 1;
		}
		t78c.select_ctx = t78ctx;

		memset(&t78sp, 0, sizeof(t78sp));
		t78sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t78sp.level = CSS_LEVEL_3;
		t78sp.charset = "UTF-8";
		t78sp.url = "resource:default.css";
		t78sp.title = "default";
		t78sp.resolve = harness_css_resolve_url;
		if (css_stylesheet_create(&t78sp, &t78ua) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 78 UA sheet\n");
			return 1;
		}
		{
			const char *ua = "html,body,div,p{display:block}";
			(void)css_stylesheet_append_data(t78ua,
					(const uint8_t *)ua, strlen(ua));
			(void)css_stylesheet_data_done(t78ua);
		}
		(void)css_select_ctx_append_sheet(t78ctx, t78ua,
				CSS_ORIGIN_UA, "screen");

		memset(&t78sp, 0, sizeof(t78sp));
		t78sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t78sp.level = CSS_LEVEL_3;
		t78sp.charset = "UTF-8";
		t78sp.url = "http://local/t78.css";
		t78sp.title = "author";
		t78sp.resolve = harness_css_resolve_url;
		if (css_stylesheet_create(&t78sp, &t78auth) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 78 author sheet\n");
			return 1;
		}
		{
			css_error ae = css_stylesheet_append_data(t78auth,
					(const uint8_t *)t78_css,
					strlen(t78_css));
			if (ae != CSS_OK && ae != CSS_NEEDDATA) {
				fprintf(stderr, "FAIL: Test 78 append=%d\n",
						(int)ae);
				return 1;
			}
			(void)css_stylesheet_data_done(t78auth);
		}
		(void)css_select_ctx_append_sheet(t78ctx, t78auth,
				CSS_ORIGIN_AUTHOR, "screen");

		t78c.media.type = CSS_MEDIA_SCREEN;
		t78c.media.width = INTTOFIX(800);
		t78c.media.height = INTTOFIX(600);
		t78c.media.orientation = CSS_MEDIA_ORIENTATION_LANDSCAPE;
		t78c.unit_len_ctx.viewport_width = INTTOFIX(800);
		t78c.unit_len_ctx.viewport_height = INTTOFIX(600);
		t78c.unit_len_ctx.device_dpi = INTTOFIX(90);
		t78c.unit_len_ctx.font_size_default = INTTOFIX(16);
		t78c.unit_len_ctx.font_size_minimum = INTTOFIX(8);
		if (lwc_intern_string("*", 1, &t78c.universal) !=
				lwc_error_ok) {
			fprintf(stderr, "FAIL: Test 78 universal\n");
			return 1;
		}
		t78c.base.status = CONTENT_STATUS_LOADING;
		t78c.base.active = 0;
		t78c.base.handler = &g_dummy_handler;

		if (dom_document_get_document_element(t78doc,
				(void *)&t78root) != DOM_NO_ERR ||
				t78root == NULL) {
			fprintf(stderr, "FAIL: Test 78 doc element\n");
			return 1;
		}
		share_before = css_select_share_adoptions();
		g_initial_build_done = 0;
		g_initial_build_ok = false;
		t78err = dom_to_box(t78root, &t78c, initial_build_cb,
				&t78_box_ctx);
		dom_node_unref(t78root);
		if (t78err != NSERROR_OK) {
			fprintf(stderr, "FAIL: Test 78 dom_to_box=%d\n",
					(int)t78err);
			return 1;
		}
		harness_pump_all(100000);
		if (!g_initial_build_done || !g_initial_build_ok) {
			fprintf(stderr, "FAIL: Test 78 build\n");
			return 1;
		}
		share_after = css_select_share_adoptions();

		for (k = 0; k < (int)(sizeof(t78_want) /
				sizeof(t78_want[0])); k++) {
			long got = t76_color_of(t78c.layout,
					t78_want[k].cls);
			fprintf(stderr, "  .%-4s = #%06lX  want #%06lX  %s\n",
					t78_want[k].cls, got,
					t78_want[k].want, t78_want[k].what);
			if (got != t78_want[k].want) {
				fprintf(stderr, "FAIL: Test 78 .%s -- %s: "
						"got #%06lX expected #%06lX\n",
						t78_want[k].cls,
						t78_want[k].what, got,
						t78_want[k].want);
				t78_bad = 1;
			}
		}

		fprintf(stderr, "  style-sharing adoptions during this "
				"fixture: %lu\n",
				(unsigned long)(share_after - share_before));
		if (share_after == share_before) {
			fprintf(stderr, "FAIL: Test 78 -- style sharing never "
					"triggered, so the colours above prove "
					"nothing about the sharing path. The "
					"two identical .t divs exist to force "
					"it; if eligibility rules changed, "
					"this fixture must change with them "
					"rather than the assertion being "
					"dropped.\n");
			t78_bad = 1;
		}
		if (t78_bad)
			return 1;
	}
	fprintf(stderr, "=== Test 78 PASS: sharing preserves custom-property "
			"scope (and really did share); multi-hop, fallback, "
			"nested fallback and cycle all correct ===\n");

	/* ---------------------------------------------------------------
	 * fixes1273 (#167) - the browser event-loop contract itself.
	 *
	 * A real facebook.com load logged 83 SECONDS of JS execution with
	 * timers=0 and raf=0. requestAnimationFrame is implemented as
	 * setTimeout(fn,16), so those are ONE fact: deferred work is not
	 * being delivered. That is not a Facebook bug - it is every site
	 * whose content arrives via a timer, an animation frame, or a
	 * promise continuation scheduled from one.
	 *
	 * The loader investigation that preceded this is now closed:
	 * Facebook's real loader and the real 15MB bundle set were replayed
	 * under stock qjs and BOTH the fast and deferred requireLazy paths
	 * fired, resolving ServerJSPayloadListener with .process. The engine
	 * and the module graph are fine. What is missing is the task layer.
	 *
	 * These assertions are the contract that layer must keep. They are
	 * ordering- and chaining-sensitive on purpose: a pump that drains
	 * only the timers present when it was entered looks healthy on the
	 * first case and starves every later one, which is exactly the shape
	 * that would leave a React scheduler permanently stalled.
	 * ------------------------------------------------------------- */
	fprintf(stderr, "\n=== Test 79: event loop delivers deferred work "
			"(timer, rAF, nested, promise interaction) ===\n");
	{
		extern void macsurf_qjs_pump_all(void);
		const char *t79_setup =
			"globalThis.__seen=[];"
			"setTimeout(function(){__seen.push('timeout');},0);"
			"requestAnimationFrame(function(t){"
				"__seen.push('raf:'+(typeof t));"
			"});"
			/* work scheduled FROM scheduled work - the case a
			 * drain-what-was-there-at-entry pump starves */
			"setTimeout(function(){"
				"setTimeout(function(){__seen.push('nested');},0);"
			"},0);"
			/* microtask -> task */
			"Promise.resolve().then(function(){"
				"setTimeout(function(){__seen.push('micro2task');},0);"
			"});"
			/* task -> microtask */
			"setTimeout(function(){"
				"Promise.resolve().then(function(){"
					"__seen.push('task2micro');"
				"});"
			"},0);"
			"__seen.push('sync');";
		const char *t79_check =
			"(function(){"
			"var need=['sync','timeout','nested','micro2task',"
				"'task2micro'];"
			"var i,missing=[];"
			"for(i=0;i<need.length;i++)"
				"if(__seen.indexOf(need[i])<0)missing.push(need[i]);"
			"var raf=0;"
			"for(i=0;i<__seen.length;i++)"
				"if(String(__seen[i]).indexOf('raf:')===0)raf=1;"
			"if(!raf)missing.push('raf');"
			"if(__seen.indexOf('raf:number')<0&&raf)"
				"missing.push('raf-timestamp-not-number');"
			"if(__seen[0]!=='sync')"
				"missing.push('sync-must-be-first(got:'+__seen[0]+')');"
			"if(missing.length)"
				"throw new Error('ASSERT FAIL missing/wrong: '+"
					"missing.join(',')+' seen=['+__seen.join(',')+']');"
			"globalThis.__t79seen=__seen.join(',');"
			"})();";
		unsigned char ok1, ok2;
		int pump;

		ok1 = js_exec(thread, (const unsigned char *)t79_setup,
				strlen(t79_setup), "driver-t79-setup.js");
		if (!ok1) {
			fprintf(stderr, "FAIL: Test 79 setup threw\n");
			return 1;
		}
		/* Pump the way the real WaitNextEvent loop does: repeatedly,
		 * over REAL elapsed time. A fixed spin count is not enough -
		 * requestAnimationFrame is setTimeout(fn,16), and 40 tight
		 * iterations complete in well under 16ms of wall clock, so the
		 * frame callback would never come due and the test would report
		 * a broken rAF that is actually just an impatient harness.
		 * Bounded by both time and iterations so it can never hang. */
		{
			extern long macsurf_monotonic_ms(void);
			long t_start = macsurf_monotonic_ms();
			pump = 0;
			while (pump < 20000 &&
				(macsurf_monotonic_ms() - t_start) < 500L) {
				macsurf_qjs_pump_all();
				harness_pump_all(1000);
				pump++;
			}
		}
		ok2 = js_exec(thread, (const unsigned char *)t79_check,
				strlen(t79_check), "driver-t79-check.js");
		if (!ok2) {
			fprintf(stderr, "FAIL: Test 79 -- the event loop did not "
					"deliver deferred work. This is the "
					"hardware shape: 83s of JS with timers=0 "
					"and raf=0 on facebook.com. Every site "
					"that loads content via a timer, an "
					"animation frame, or a promise "
					"continuation scheduled from one depends "
					"on this.\n");
			return 1;
		}
		fprintf(stderr, "  order seen: ");
		{
			const char *dump =
				"globalThis.__t79seen";
			(void)dump;
		}
		fprintf(stderr, "(all five delivered, sync first, rAF "
				"timestamp is a number)\n");
	}
	fprintf(stderr, "=== Test 79 PASS: timer, rAF, nested scheduling and "
			"both promise/task directions all deliver ===\n");

	/* ---------------------------------------------------------------
	 * fixes1275 (#167) - diagnostics must not FABRICATE EXISTENCE.
	 *
	 * The __d / requireLazy property traps (fixes1247/1259, added to
	 * investigate why Facebook never booted) returned their wrapper
	 * unconditionally, so both names appeared to exist before any page
	 * script had run. facebook.com feature-detects exactly those names.
	 * Verbatim from the real page, recovered from the hardware log and
	 * confirmed to execute at t=961, BEFORE the bootstrap that defines
	 * requireLazy at t=991:
	 *
	 *   window.requireLazy ? window.requireLazy(["Env"],copyVariables)
	 *                      : (window.Env=window.Env||{},
	 *                         copyVariables(window.Env))
	 *
	 * A real browser has requireLazy undefined there, takes the ELSE
	 * branch, and installs window.Env - the page's whole configuration.
	 * MacSurf said "it exists", so the page handed Env to a requireLazy
	 * that did not exist yet and the wrapper dropped it. window.Env was
	 * NEVER SET. The instrumentation added to find the bug was causing
	 * it.
	 *
	 * This asserts the contract that prevents a recurrence: an
	 * observer-only trap reports undefined until the page assigns, and
	 * the wrapper afterwards. It runs the REAL branch, not a mock of it.
	 * ------------------------------------------------------------- */
	fprintf(stderr, "\n=== Test 80: instrumentation reports absence "
			"truthfully (fixes1275) ===\n");
	{
		const char *t80 =
			"(function(){"
			/* the exact shape the real page uses */
			"var probe=(typeof globalThis.__msT80Name==='undefined')?"
				"'__msT80Name':'__msT80Name';"
			"if(typeof globalThis.requireLazy!=='undefined'&&"
					"globalThis.__msT80Assigned!==1){"
				"throw new Error('ASSERT FAIL: requireLazy reports "
					"as existing before any page script assigned "
					"it -- a feature-detect like facebook.com\\'s "
					"envjson script will take the wrong branch "
					"and lose window.Env');"
			"}"
			/* real envjson branch, verbatim logic */
			"var variables={MARK:'ENV-OK'};"
			"var copyVariables=function(e){"
				"for(var n in variables)e[n]=variables[n];"
			"};"
			"globalThis.requireLazy?"
				"globalThis.requireLazy(['Env'],copyVariables):"
				"(globalThis.Env=globalThis.Env||{},"
					"copyVariables(globalThis.Env));"
			"if(!globalThis.Env||globalThis.Env.MARK!=='ENV-OK')"
				"throw new Error('ASSERT FAIL: window.Env was not "
					"installed -- the page config is lost, which "
					"is exactly the facebook.com failure');"
			/* after a real assignment the trap must engage */
			"globalThis.__msT80Assigned=1;"
			"globalThis.requireLazy=function(){"
				"globalThis.__msT80Called=1;"
			"};"
			"if(typeof globalThis.requireLazy!=='function')"
				"throw new Error('ASSERT FAIL: after assignment "
					"requireLazy must be callable');"
			"globalThis.requireLazy(['x'],function(){});"
			"if(globalThis.__msT80Called!==1)"
				"throw new Error('ASSERT FAIL: assigned "
					"implementation was not reached -- the "
					"observer must delegate, not swallow');"
			"})();";
		unsigned char ok = js_exec(thread, (const unsigned char *)t80,
				strlen(t80), "driver-t80.js");
		if (!ok) {
			fprintf(stderr, "FAIL: Test 80 -- see assertion above\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 80 PASS: absence reported truthfully, "
			"window.Env installs, assignment still delegates ===\n");

	/* --- Test 81: pre-loader requireLazy call survives the stub->real
	 * transition (#167, fixes1276) -------------------------------------
	 *
	 * Test 80 proved the trap stops LYING about existence. It did not
	 * prove the trap still WORKS end to end across the one transition
	 * that matters on the real page: Facebook installs a temporary
	 * stub-queueing __d/requireLazy BEFORE its 323KB real loader has
	 * downloaded, an inline SSR island calls requireLazy() against that
	 * stub, the stub queues the call into window.__rl_stub, and only
	 * later does the real loader arrive, install itself over __d/
	 * requireLazy, and drain the queue. If MacSurf's wrapper ever
	 * captured a STALE reference to the stub instead of re-reading its
	 * realD/realRL closure vars on each call, a call queued during the
	 * stub phase would be silently lost -- which is exactly the shape
	 * of the still-open rl_target_fires=0 hardware symptom.
	 *
	 * Uses harness/fbcdn.net-loader-b25.js, the REAL 323664-byte loader bundle
	 * recovered from a live facebook.com session's hardware log (same
	 * file fixes1274/1275 used to prove the window.Env root cause).
	 * The stub-queueing bootstrap itself was not separately recovered
	 * from a saved capture, so it is not literally replayed verbatim --
	 * but its shape is not invented: b25.js's own drain code demands
	 * `t.__d.apply(null, t.__d_stub[Fe])` and
	 * `_e.apply(null, t.__rl_stub[Oe])`, i.e. each queued entry must be
	 * exactly an arguments object, which is what forces the classic
	 * Haste/BigPipe `(queue=queue||[]).push(arguments)` idiom used
	 * below. */
	fprintf(stderr, "\n=== Test 81: pre-loader requireLazy call survives "
			"the stub->real transition (fixes1276) ===\n");
	{
		char *b25src = harness_slurp("fbcdn.net-loader-b25.js");
		if (b25src == NULL) {
			fprintf(stderr, "SKIP: fbcdn.net-loader-b25.js not present\n");
		} else {
			/* Plain assignment, not the `x = x || fn` idiom -- this
			 * test's realm has run 80 prior tests, so __d/requireLazy
			 * are not necessarily untouched here the way they are on
			 * a fresh navigation. Reinstalling defensively over an
			 * existing wrapper is exactly what Test 82 exists to
			 * cover; this test's job is the stub-queue -> real-loader
			 * -> drain -> fire pipeline, so it forces a known-clean
			 * stub directly rather than depending on ambient state. */
			const char *setup =
				"(function(){\"use strict\";"
				"globalThis.__t81={fired:0};"
				"globalThis.__d=function(){"
					"(globalThis.__d_stub=globalThis.__d_stub||[])"
						".push(arguments);"
				"};"
				"globalThis.requireLazy="
					"function(){"
					"(globalThis.__rl_stub=globalThis.__rl_stub||[])"
						".push(arguments);"
				"};"
				"globalThis.requireLazy(['ServerJSPayloadListener'],"
					"function(m){globalThis.__t81.fired=1;});"
				"if(globalThis.__t81.fired!==0)"
					"throw new Error('ASSERT FAIL: callback fired "
						"before the target was ever defined and "
						"before the real loader even loaded');"
				"if(!(globalThis.__rl_stub&&"
						"globalThis.__rl_stub.length===1))"
					"throw new Error('ASSERT FAIL: a requireLazy "
						"call made during the pre-loader stub "
						"phase did not reach the stub queue -- "
						"got __rl_stub.length='+"
						"(globalThis.__rl_stub?"
							"globalThis.__rl_stub.length:"
							"'undefined')+"
						"' -- MacSurf is not delegating through "
						"to the assigned stub');"
				"})();";
			const char *post =
				"(function(){\"use strict\";"
				"if(typeof globalThis.__d!=='function')"
					"throw new Error('ASSERT FAIL: real loader did "
						"not install __d');"
				"if(globalThis.__d_stub!==undefined)"
					"throw new Error('ASSERT FAIL: real loader did "
						"not drain/delete __d_stub');"
				"if(globalThis.__rl_stub!==undefined)"
					"throw new Error('ASSERT FAIL: real loader did "
						"not drain/delete __rl_stub -- the "
						"pre-loader-queued call was lost');"
				"globalThis.__d('ServerJSPayloadListener',[],"
					"function(global,require,requireDynamic,"
						"requireLazy,module,exports){"
						"exports.process=function(){};"
					"});"
				"if(globalThis.__t81.fired!==1)"
					"throw new Error('ASSERT FAIL: a requireLazy "
						"callback queued during the PRE-LOADER "
						"stub phase never fired after its target "
						"was defined post-loader -- this "
						"reproduces the rl_target_fires=0 "
						"hardware symptom locally');"
				"})();";
			char *b25len_src; size_t wn; unsigned char ok81a, ok81b;

			ok81a = js_exec(thread, (const unsigned char *)setup,
					strlen(setup), "driver-t81-setup.js");
			if (!ok81a) {
				fprintf(stderr, "FAIL: Test 81 setup -- see assertion "
						"above\n");
				free(b25src);
				return 1;
			}

			ok81b = js_exec(thread, (const unsigned char *)b25src,
					strlen(b25src), "fbcdn.net-loader-b25.js");
			fprintf(stderr, "js_exec(fbcdn.net-loader-b25.js, %lu bytes) "
					"ok=%d\n", (unsigned long)strlen(b25src),
					(int)ok81b);
			free(b25src);
			if (!ok81b) {
				fprintf(stderr, "FAIL: Test 81 -- the real loader "
						"bundle threw; see the exception reported "
						"above\n");
				return 1;
			}

			wn = strlen(post);
			b25len_src = (char *)malloc(wn + 1);
			memcpy(b25len_src, post, wn + 1);
			ok81b = js_exec(thread, (const unsigned char *)b25len_src,
					wn, "driver-t81-post.js");
			free(b25len_src);
			if (!ok81b) {
				fprintf(stderr, "FAIL: Test 81 -- see assertion "
						"above\n");
				return 1;
			}
		}
	}
	fprintf(stderr, "=== Test 81 PASS: a requireLazy call queued during "
			"Facebook's pre-loader stub phase survives the stub->real "
			"transition and fires once its target is defined, through "
			"the current fixes1275 wrapper unmodified ===\n");

	/* --- Test 82: reinstalling __d/requireLazy defensively must not
	 * self-reference (#167, fixes1276) ----------------------------------
	 *
	 * Found while writing Test 81: `window.requireLazy = window.requireLazy
	 * || function(){...}` is the completely ordinary "install a stub only
	 * if nothing is there yet" idiom, and it plausibly runs once per
	 * SSR island/prelude chunk on a real page -- many times per
	 * navigation, not once. Before this fix, the SECOND such reinstall
	 * (against a trap that has already captured a real value) read back
	 * `wrappedRL` itself from the getter and wrote it straight through
	 * the setter, making the wrapper its own delegate. Every call after
	 * that recursed into itself and blew the stack -- reproduced with
	 * nothing more than two consecutive `x = x || fn` installs against a
	 * fresh trap, no real Facebook code involved. A RangeError inside
	 * this codebase's near-universal try/catch reads from the outside as
	 * "nothing happened", the same silent-failure shape fixes1275 fixed
	 * one layer up. This asserts the fix: the FIRST real assignment must
	 * keep winning no matter how many times the idempotent idiom repeats
	 * afterward, for both __d and requireLazy. */
	fprintf(stderr, "\n=== Test 82: defensive reinstall does not "
			"self-reference (fixes1276) ===\n");
	{
		const char *t82 =
			"(function(){\"use strict\";"
			"globalThis.__t82={dCalls:0,rlCalls:0,wrongStub:0};"
			/* first real assignment -- plain, not `x = x || fn`. By
			 * this point in the harness the trap has already been
			 * exercised by 81 prior tests, so it is not necessarily
			 * virgin the way it is on a fresh navigation; this line
			 * establishes a KNOWN baseline the same way Test 81's
			 * setup does. It is the reinstalls below, against that
			 * now-known-real baseline, that this test exists to
			 * check. */
			"globalThis.__d=function(){"
				"globalThis.__t82.dCalls++;"
			"};"
			"globalThis.requireLazy=function(){"
				"globalThis.__t82.rlCalls++;"
			"};"
			/* a second island's defensive reinstall, shaped exactly
			 * like the first -- must be a no-op against the trap */
			"globalThis.__d=globalThis.__d||function(){"
				"globalThis.__t82.wrongStub++;"
			"};"
			"globalThis.requireLazy=globalThis.requireLazy||function(){"
				"globalThis.__t82.wrongStub++;"
			"};"
			/* and a third, because real pages are not limited to two */
			"globalThis.__d=globalThis.__d||function(){"
				"globalThis.__t82.wrongStub++;"
			"};"
			"globalThis.requireLazy=globalThis.requireLazy||function(){"
				"globalThis.__t82.wrongStub++;"
			"};"
			"globalThis.__d('probe');"
			"globalThis.requireLazy(['probe'],function(){});"
			"if(globalThis.__t82.dCalls!==1||globalThis.__t82.rlCalls!==1)"
				"throw new Error('ASSERT FAIL: the FIRST real __d/"
					"requireLazy implementation did not receive the "
					"call -- dCalls='+globalThis.__t82.dCalls+"
					"' rlCalls='+globalThis.__t82.rlCalls);"
			"if(globalThis.__t82.wrongStub!==0)"
				"throw new Error('ASSERT FAIL: a LATER defensive "
					"reinstall\\'s stub ran instead of the first -- "
					"the self-reference guard let a later `x = x || "
					"fn` overwrite the real implementation');"
			"})();";
		unsigned char ok = js_exec(thread, (const unsigned char *)t82,
				strlen(t82), "driver-t82.js");
		if (!ok) {
			fprintf(stderr, "FAIL: Test 82 -- see assertion above (a "
					"stack-overflow RangeError here means the "
					"self-reference guard is missing/broken)\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 82 PASS: repeated defensive reinstall stays "
			"a no-op once the trap holds a real implementation, for "
			"both __d and requireLazy ===\n");

	/* --- Test 83: Hyperion's browser-prototype validation (#167,
	 * fixes1277) ------------------------------------------------------
	 *
	 * Facebook's real loader bundle installs Hyperion before the app boots.
	 * Hyperion creates a shadow prototype for Node, Window, XMLHttpRequest,
	 * and History, then asserts that every child target actually inherits its
	 * parent's target prototype. MacSurf had the objects but not the standard
	 * EventTarget ancestry, so Hyperion threw its own "Invalid prototype
	 * chain" assertion (three times in the hardware log) before the page got
	 * to the later app code. This is the exact ancestry predicate from the
	 * recovered b25 loader, kept small enough to make a failed edge explicit.
	 *
	 * The test deliberately checks the live global too: the WANT census puts
	 * an exotic prototype immediately above it, so merely checking
	 * Window.prototype is a false green unless window still reaches it. */
	fprintf(stderr, "\n=== Test 83: Facebook Hyperion sees a real browser "
			"prototype graph (fixes1277) ===\n");
	{
		const char *t83 =
			"(function(){"
			"function bad(s){throw new Error('ASSERT FAIL: '+s);}"
			"function shadow(ctor,parent,opts){"
			"var target=(opts&&opts.targetPrototype)||"
				"(ctor&&ctor.prototype);"
			"if(!target&&opts&&opts.sampleObject)"
				"target=Object.getPrototypeOf(opts.sampleObject);"
			"if(!target||typeof target!=='object')"
				"bad('no target prototype for '+(ctor&&ctor.name));"
			"if(parent){var p=target,want=parent.targetPrototype,found=false;"
				"while(p&&!found){found=p===want;p=Object.getPrototypeOf(p);}"
				"if(!found)bad('Invalid prototype chain for '+"
					"(ctor&&ctor.name));}"
			"return{targetPrototype:target};"
			"}"
			"if(typeof EventTarget!=='function'||typeof Window!=='function'||"
				"typeof History!=='function')bad('missing browser constructor');"
			"if(!(window instanceof Window))bad('window is not a Window');"
			"if(!(history instanceof History))bad('history is not a History');"
			"if(!(document.head instanceof Node))bad('head is not a Node');"
			"if(!(new XMLHttpRequest() instanceof EventTarget))"
				"bad('XMLHttpRequest is not an EventTarget');"
			"var et=shadow(EventTarget,null,{sampleObject:document.head});"
			"var no=shadow(Node,et,{sampleObject:document.head});"
			"shadow(Attr,no,{nodeType:document.ATTRIBUTE_NODE});"
			"var el=shadow(Element,no,{sampleObject:document.head});"
			"var he=shadow(HTMLElement,el,{sampleObject:document.head});"
			"shadow(Window,et,{targetPrototype:window});"
			"shadow(XMLHttpRequest,et,{sampleObject:new XMLHttpRequest()});"
			"shadow(History,null,{sampleObject:history});"
			/* Test 81 has already loaded the recovered real Facebook loader.
			 * Make its Hyperion feature flag true and invoke the actual module,
			 * not only our compact copy of its predicate. */
			"globalThis.Env=globalThis.Env||{};globalThis.Env.loadHyperion=true;"
			"if(typeof require!=='function')bad('real loader has no require');"
			"require('Hyperion');"
			"globalThis.__t83ok=he.targetPrototype===HTMLElement.prototype;"
			"if(!globalThis.__t83ok)bad('HTMLElement target changed');"
			"})();";
		unsigned char ok = js_exec(thread, (const unsigned char *)t83,
				strlen(t83), "driver-t83-hyperion-protos.js");
		if (!ok) {
			fprintf(stderr, "FAIL: Test 83 -- Facebook Hyperion's exact "
					"prototype-chain check rejected the browser graph\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 83 PASS: Node, Window, XMLHttpRequest, and "
			"History carry the prototype graph Hyperion requires ===\n");
#endif /* MACSURF_JS_FB_LOADER_TRAP */

	/* --- Test 84: document.location is the live window Location (#167,
	 * fixes1279) --------------------------------------------------------
	 *
	 * Facebook's Messenger initialization reaches getDocumentDomain(), whose
	 * first operation is `document.location.href`.  The location object was
	 * present at window.location but the document alias was absent, so the
	 * otherwise healthy MWV2Chat path threw "cannot read property 'href' of
	 * undefined".  Check the actual expression and the platform identity
	 * guarantee; this deliberately does not assign, because that would turn a
	 * regression test into a navigation. */
	fprintf(stderr, "\n=== Test 84: document.location is window.location "
			"(fixes1279) ===\n");
	{
		const char *t84 =
			"(function(){"
			"function bad(s){throw new Error('ASSERT FAIL: '+s);}"
			"if(document.location!==window.location)"
				"bad('document.location is not window.location');"
			"if(typeof document.location.href!=='string')"
				"bad('document.location.href is not a string');"
			"var d=Object.getOwnPropertyDescriptor(document,'location');"
			"if(!d||typeof d.get!=='function'||typeof d.set!=='function')"
				"bad('document.location is not a forwarding accessor');"
			"})();";
		unsigned char ok = js_exec(thread, (const unsigned char *)t84,
				strlen(t84), "driver-t84-document-location.js");
		if (!ok) {
			fprintf(stderr, "FAIL: Test 84 -- document.location does not "
					"expose the live Location object\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 84 PASS: document.location exposes the live "
			"Location object ===\n");

	/* --- Test 85: the application task/message surface (#167, fixes1280)
	 *
	 * Facebook's bundles complete, then React asks for setImmediate,
	 * MessageChannel, postMessage, and reportError.  These drive real deferred
	 * scheduler work; merely defining names would strand the same render tasks
	 * one layer later.  Exercise all four through the actual timer pump. */
	fprintf(stderr, "\n=== Test 85: task/message APIs deliver real work "
			"(fixes1280) ===\n");
	{
		extern void macsurf_qjs_pump_all(void);
		const char *t85_setup =
			"globalThis.__t85=[];"
			"setImmediate(function(){__t85.push('immediate');});"
			"var c=new MessageChannel();"
			"c.port1.onmessage=function(e){__t85.push('port:'+e.data);};"
			"c.port1.addEventListener('message',function(e){__t85.push('listener:'+e.data);});"
			"c.port2.postMessage('task');"
			"addEventListener('message',function(e){if(e.data==='window')__t85.push('window:'+e.origin);});"
			"postMessage('window','*');"
			"addEventListener('error',function(e){if(e.message==='reported')__t85.push('error');});"
			"reportError(new Error('reported'));";
		const char *t85_check =
			"(function(){var n=['immediate','port:task','listener:task','error'],i;"
			"for(i=0;i<n.length;i++)if(__t85.indexOf(n[i])<0)"
			"throw new Error('ASSERT FAIL missing '+n[i]+' ['+__t85.join(',')+']');"
			"if(!__t85.some(function(x){return x.indexOf('window:')===0;}))"
			"throw new Error('ASSERT FAIL missing window message ['+__t85.join(',')+']');"
			"})();";
		unsigned char ok1, ok2;
		int pump;

		ok1 = js_exec(thread, (const unsigned char *)t85_setup,
				strlen(t85_setup), "driver-t85-task-message-setup.js");
		if (!ok1) {
			fprintf(stderr, "FAIL: Test 85 setup threw\n");
			return 1;
		}
		for (pump = 0; pump < 100; pump++) {
			macsurf_qjs_pump_all();
			harness_pump_all(1000);
		}
		ok2 = js_exec(thread, (const unsigned char *)t85_check,
				strlen(t85_check), "driver-t85-task-message-check.js");
		if (!ok2) {
			fprintf(stderr, "FAIL: Test 85 -- a scheduler task/message API "
					"failed to deliver work\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 85 PASS: immediate, channel, window message, "
			"and reportError all deliver ===\n");

	/* --- Test 86: Comment is a real CharacterData node (#167, fixes1283)
	 *
	 * React identifies hydration and Suspense boundaries by COMMENT_NODE.  The
	 * old document.createComment implementation returned a Text node, which
	 * makes the marker indistinguishable from user content and violates every
	 * DOM traversal React performs around it. */
	fprintf(stderr, "\n=== Test 86: document.createComment creates a native "
			"Comment node (fixes1283) ===\n");
	{
		const char *t86 =
			"(function(){"
			"function bad(s){throw new Error('ASSERT FAIL: '+s);}"
			"if(typeof document.__createCommentNative!=='function')"
				"bad('native createComment binding is missing');"
			"var c=document.createComment('react-marker'),host=document.createElement('div');"
			"if(!c)bad('createComment returned null');"
			"if(c.nodeType!==Node.COMMENT_NODE)bad('nodeType='+c.nodeType);"
			"if(c.nodeName!=='#comment')bad('nodeName='+c.nodeName);"
			"if(c.data!=='react-marker'||c.nodeValue!=='react-marker')"
				"bad('CharacterData content is wrong');"
			"if(!(c instanceof Comment)||!(c instanceof CharacterData)||!(c instanceof Node))"
				"bad('Comment prototype family is wrong');"
			"host.appendChild(c);if(host.firstChild!==c||c.parentNode!==host)"
				"bad('native comment did not attach');"
			"c.data='hydration-boundary';"
			"if(c.textContent!=='hydration-boundary')bad('data setter missed native node');"
			"})();";
		unsigned char ok = js_exec(thread, (const unsigned char *)t86,
				strlen(t86), "driver-t86-comment-node.js");
		if (!ok) {
			fprintf(stderr, "FAIL: Test 86 -- document.createComment is not a "
					"real Comment CharacterData node\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 86 PASS: native Comment node preserves React "
			"hydration-marker semantics ===\n");

	/* --- Test 87: node traversal keeps fragment and Document identity (#167,
	 * fixes1284) ---------------------------------------------------------
	 *
	 * React's host tree crosses Comment -> DocumentFragment during insertion,
	 * then follows parentNode through documentElement to Document.  Both edges
	 * must return the existing native-backed JS identity; filtering parents to
	 * Element nodes makes the tree look detached even when libdom attached it
	 * correctly. */
	fprintf(stderr, "\n=== Test 87: fragment and Document traversal preserve "
			"native identity (fixes1284) ===\n");
	{
		const char *t87 =
			"(function(){"
			"function bad(s){throw new Error('ASSERT FAIL: '+s);}"
			"var f=document.createDocumentFragment(),"
			"c=document.createComment('marker'),"
			"host=document.createElement('div'),de=document.documentElement;"
			"f.appendChild(c);"
			"if(f.firstChild!==c||f.lastChild!==c||f.childNodes.length!==1)"
				"bad('fragment child identity');"
			"if(c.parentNode!==f||c.getRootNode()!==f)"
				"bad('comment fragment parent/root');"
			"if(c.nextSibling!==null||c.previousSibling!==null)"
				"bad('comment detached sibling edge');"
			"host.appendChild(f);"
			"if(host.firstChild!==c||host.lastChild!==c||c.parentNode!==host)"
				"bad('fragment insertion did not preserve comment');"
			"if(c.getRootNode()!==host||f.firstChild!==null||f.lastChild!==null)"
				"bad('fragment transfer/root');"
			"if(de.parentNode!==document||de.getRootNode()!==document)"
				"bad('document traversal identity');"
			"if(de.isConnected!==true)bad('documentElement disconnected');"
			"})();";
		unsigned char ok = js_exec(thread, (const unsigned char *)t87,
				strlen(t87), "driver-t87-node-traversal.js");
		if (!ok) {
			fprintf(stderr, "FAIL: Test 87 -- fragment/Document traversal lost "
					"native identity\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 87 PASS: comment, fragment, element, and "
			"Document traversal retain identity ===\n");

	/* --- Test 88: browser capability truthfulness (#167, fixes1285) --- */
	fprintf(stderr, "\n=== Test 88: Blob has bytes and unsupported APIs stay "
			"absent (fixes1285) ===\n");
	{
		const char *t88_arm =
			"(function(){"
			"function bad(s){throw new Error('ASSERT FAIL: '+s);}"
			"var names=['indexedDB','IDBKeyRange','caches','WebSocket',"
			"'File','FileReader','Notification'],i,n;"
			"for(i=0;i<names.length;i++){n=names[i];"
			"if(typeof globalThis[n]!=='undefined'||n in globalThis)"
				"bad(n+' is advertised without a real backend');}"
			"if(typeof URL.createObjectURL!=='undefined'||"
				"'createObjectURL' in URL||'revokeObjectURL' in URL)"
				"bad('URL object URLs are advertised without retained bytes');"
			"var src=new Uint8Array([33]);"
			"var blob=new Blob(['hi',src],{type:'TEXT/PLAIN'});src[0]=63;"
			"if(!(blob instanceof Blob)||blob.size!==3||blob.type!=='text/plain')"
				"bad('Blob does not preserve its bytes and MIME type');"
			"var cut=blob.slice(1,3,'TEXT/X');"
			"if(cut.size!==2||cut.type!=='text/x')bad('Blob.slice metadata');"
			"globalThis.__t88BlobText='';globalThis.__t88BlobBytes='';"
			"blob.text().then(function(s){globalThis.__t88BlobText=s;});"
			"blob.arrayBuffer().then(function(b){var v=new Uint8Array(b);"
				"globalThis.__t88BlobBytes=v[0]+','+v[1]+','+v[2];});"
			"if(navigator.sendBeacon('https://example.invalid/beacon',blob)!==false)"
				"bad('sendBeacon must reject Blob until byte transport exists');"
			"})();";
		const char *t88_check =
			"if(globalThis.__t88BlobText!=='hi!')"
				"throw new Error('ASSERT FAIL: Blob.text lost bytes: '+globalThis.__t88BlobText);"
			"if(globalThis.__t88BlobBytes!=='104,105,33')"
				"throw new Error('ASSERT FAIL: Blob.arrayBuffer lost bytes: '+globalThis.__t88BlobBytes);";
		unsigned char ok = js_exec(thread, (const unsigned char *)t88_arm,
				strlen(t88_arm), "driver-t88-capabilities-arm.js");
		if (!ok) {
			fprintf(stderr, "FAIL: Test 88 -- capability surface or Blob construction "
					"is untruthful\n");
			return 1;
		}
		{
			extern void macsurf_qjs_pump_all(void);
			int pump;
			for (pump = 0; pump < 8; pump++) macsurf_qjs_pump_all();
		}
		ok = js_exec(thread, (const unsigned char *)t88_check,
				strlen(t88_check), "driver-t88-capabilities-check.js");
		if (!ok) {
			fprintf(stderr, "FAIL: Test 88 -- Blob Promise readers lost bytes\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 88 PASS: Blob owns bytes; feature detection sees "
			"only real browser backends ===\n");

	/* --- Test 89: host DOM hooks resolve their node's realm (#167,
	 * fixes1284) ---------------------------------------------------------
	 *
	 * The parser's inline-handler hook receives a native node, not a context.
	 * Build A then B so B is the most recent heap, bind A's node after that,
	 * and execute in A.  A last-heap lookup compiles the handler in B and the
	 * A-side call sees no handler; resolving node -> document -> realm is the
	 * only correct ownership path. */
	fprintf(stderr, "\n=== Test 89: parser hooks bind in the owning realm "
			"(fixes1284) ===\n");
	{
		struct html_content ca, cb;
		dom_document *da = NULL, *db = NULL;
		dom_element *ba = NULL;
		dom_string *ida = NULL;
		struct jsheap *ha = NULL, *hb = NULL;
		struct jsthread *ta = NULL, *tb = NULL;
		nserror e;
		unsigned char ok;
		extern void macsurf_qjs_bind_inline_handlers(struct dom_node *node);
		extern void macsurf_js_notify_content_freed(struct content *c);

		if (!harness_parse_document(
				"<html><body><button id='a' onclick=\"globalThis.__t89='A'\">A</button></body></html>",
				&da) ||
			!harness_parse_document(
				"<html><body><button id='b' onclick=\"globalThis.__t89='B'\">B</button></body></html>",
				&db)) {
			fprintf(stderr, "FAIL: Test 89 -- independent document parse\n");
			return 1;
		}
		memset(&ca, 0, sizeof(ca));
		memset(&cb, 0, sizeof(cb));
		ca.document = da;
		cb.document = db;
		ca.enable_scripting = true;
		cb.enable_scripting = true;
		e = js_newheap(20000, &ha);
		if (e == NSERROR_OK) e = js_newthread(ha, NULL, &ca, &ta);
		if (e == NSERROR_OK) e = js_newheap(20000, &hb);
		if (e == NSERROR_OK) e = js_newthread(hb, NULL, &cb, &tb);
		if (e != NSERROR_OK || ta == NULL || tb == NULL) {
			fprintf(stderr, "FAIL: Test 89 -- realm setup err=%d\n", (int)e);
			return 1;
		}
		if (dom_string_create((const uint8_t *)"a", 1, &ida) != DOM_NO_ERR ||
				ida == NULL || dom_document_get_element_by_id(da, ida, &ba)
				!= DOM_NO_ERR || ba == NULL) {
			fprintf(stderr, "FAIL: Test 89 -- A button lookup\n");
			return 1;
		}
		macsurf_qjs_bind_inline_handlers((dom_node *)ba);
		dom_string_unref(ida);
		dom_node_unref((dom_node *)ba);
		ok = js_exec(ta, (const unsigned char *)
			"(function(){var b=document.getElementById('a');"
			"if(!b||typeof b.onclick!=='function')"
			"throw new Error('ASSERT FAIL: A handler was bound in another realm');"
			"b.onclick({type:'click'});"
			"if(globalThis.__t89!=='A')"
			"throw new Error('ASSERT FAIL: handler executed outside A');})();",
			strlen("(function(){var b=document.getElementById('a');"
			"if(!b||typeof b.onclick!=='function')"
			"throw new Error('ASSERT FAIL: A handler was bound in another realm');"
			"b.onclick({type:'click'});"
			"if(globalThis.__t89!=='A')"
			"throw new Error('ASSERT FAIL: handler executed outside A');})();"),
			"driver-t89-realm-owner.js");
		if (!ok) {
			fprintf(stderr, "FAIL: Test 89 -- parser hook used the last realm\n");
			return 1;
		}
		macsurf_js_notify_content_freed((struct content *)&ca);
		macsurf_js_notify_content_freed((struct content *)&cb);
		js_destroyheap(hb);
		js_destroyheap(ha);
		dom_node_unref((dom_node *)db);
		dom_node_unref((dom_node *)da);
	}
	fprintf(stderr, "=== Test 89 PASS: native node hooks cannot target the "
			"last-created realm ===\n");

	/* --- Test 90: production realm leaves Facebook loader globals to the
	 * page (#167, fixes1285) --------------------------------------------- */
#if !MACSURF_JS_FB_LOADER_TRAP
	fprintf(stderr, "\n=== Test 90: a fresh browser realm has no Facebook "
			"loader globals (fixes1285) ===\n");
	{
		struct html_content clean_content;
		dom_document *clean_document = NULL;
		struct jsheap *clean_heap = NULL;
		struct jsthread *clean_thread = NULL;
		nserror clean_err;
		unsigned char clean_ok;
		extern void macsurf_js_notify_content_freed(struct content *c);

		if (!harness_parse_document("<html><body></body></html>",
				&clean_document)) {
			fprintf(stderr, "FAIL: Test 90 -- fresh document parse\n");
			return 1;
		}
		memset(&clean_content, 0, sizeof(clean_content));
		clean_content.document = clean_document;
		clean_content.enable_scripting = true;
		clean_err = js_newheap(20000, &clean_heap);
		if (clean_err == NSERROR_OK)
			clean_err = js_newthread(clean_heap, NULL, &clean_content,
					&clean_thread);
		if (clean_err != NSERROR_OK || clean_thread == NULL) {
			fprintf(stderr, "FAIL: Test 90 -- fresh realm setup err=%d\n",
					(int)clean_err);
			return 1;
		}
		clean_ok = js_exec(clean_thread, (const unsigned char *)
			"(function(){var n=['__d','requireLazy','__onBeforeModuleFactory'],i;"
			"for(i=0;i<n.length;i++){if(typeof globalThis[n[i]]!=='undefined'||"
			"n[i] in globalThis)throw new Error('ASSERT FAIL: preinstalled '+n[i]);}"
			"if(typeof Blob!=='function'||new Blob(['x']).size!==1)"
			"throw new Error('ASSERT FAIL: byte-backed Blob missing in fresh realm');}());",
			strlen("(function(){var n=['__d','requireLazy','__onBeforeModuleFactory'],i;"
			"for(i=0;i<n.length;i++){if(typeof globalThis[n[i]]!=='undefined'||"
			"n[i] in globalThis)throw new Error('ASSERT FAIL: preinstalled '+n[i]);}"
			"if(typeof Blob!=='function'||new Blob(['x']).size!==1)"
			"throw new Error('ASSERT FAIL: byte-backed Blob missing in fresh realm');}());"),
			"driver-t90-clean-realm.js");
		macsurf_js_notify_content_freed((struct content *)&clean_content);
		js_destroyheap(clean_heap);
		dom_node_unref((dom_node *)clean_document);
		if (!clean_ok) {
			fprintf(stderr, "FAIL: Test 90 -- production pre-installs a page "
					"loader global or lacks Blob\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 90 PASS: Facebook's loader names are page-owned "
			"in a fresh browser realm ===\n");
#endif /* !MACSURF_JS_FB_LOADER_TRAP */

	/* --- Test 91: retiring an in-flight object repays base.active (#167) -- */
	fprintf(stderr, "\n=== Test 91: in-flight object retirement balances "
			"the page load counter (fixes1288) ===\n");
	{
		struct content_html_object *retired;
		struct content_html_object *saved_list = htmlc.object_list;
		unsigned int saved_n = htmlc.num_objects;
		unsigned int saved_active = htmlc.base.active;

		retired = calloc(1, sizeof(*retired));
		if (retired == NULL) {
			fprintf(stderr, "FAIL: Test 91 calloc\n");
			return 1;
		}
		retired->parent = (struct content *)&htmlc;
		retired->active_counted = true;
		htmlc.object_list = retired;
		htmlc.num_objects = 1;
		htmlc.base.active = 1;

		(void) html_object_free_objects(&htmlc);
		if (htmlc.base.active != 0) {
			fprintf(stderr, "FAIL: Test 91 retired callback-less object left "
					"active=%u; READY can never become DONE\n",
					htmlc.base.active);
			return 1;
		}
		if (htmlc.object_list != NULL) {
			fprintf(stderr, "FAIL: Test 91 object was not retired\n");
			return 1;
		}

		htmlc.object_list = saved_list;
		htmlc.num_objects = saved_n;
		htmlc.base.active = saved_active;
	}
	fprintf(stderr, "=== Test 91 PASS: releasing an in-flight object cannot "
			"strand the document in READY ===\n");

	/* --- Test 92: structuredClone preserves browser structured data (#167,
	 * fixes1290) ----------------------------------------------------------
	 *
	 * Facebook's ConstUriUtils stores query parameters in a Map, clones it,
	 * then calls delete()/set() while constructing the logged-in root router.
	 * The former JSON-based shim returned {}, producing the hardware's exact
	 * `TypeError: not a function` and aborting before initFizz.  Cover that
	 * literal contract as well as the graph properties the browser API claims:
	 * cycles, mutation isolation, Map/Set, shared typed-array backing storage,
	 * Date/RegExp, transfers, and DataCloneError for an unsupported value. */
	fprintf(stderr, "\n=== Test 92: structuredClone preserves Maps and object "
			"graphs (fixes1290) ===\n");
	{
		const char *t92 =
			"(function(){"
			"function bad(m){throw new Error('ASSERT FAIL: '+m);}"
			"var key={id:7},value={name:'profile'},"
				"original=new Map([[key,value],['drop','tracking']]);"
			"var copied=structuredClone(original),copiedKey=null;"
			"if(!(copied instanceof Map))bad('Map became '+Object.prototype.toString.call(copied));"
			"copied.forEach(function(v,k){if(k&&k.id===7)copiedKey=k;});"
			"if(!copiedKey||copiedKey===key)bad('Map object key was not cloned');"
			"if(copied.get(copiedKey)===value||copied.get(copiedKey).name!=='profile')"
				"bad('Map value was not independently cloned');"
			"copied.delete('drop');copied.set('worker_type','CLASSIC');"
			"if(!original.has('drop')||original.has('worker_type'))bad('Map mutation leaked to source');"
			/* This is Facebook ConstUriUtils.removeQueryParams/addQueryParam. */
			"var query=new Map([['__cft__','x'],['keep','y']]);"
			"var clean=structuredClone(query);clean.delete('__cft__');clean.set('new','z');"
			"if(clean.get('keep')!=='y'||clean.get('new')!=='z'||clean.has('__cft__'))"
				"bad('Facebook query-Map contract');"
			"var cyclic={label:'root'};cyclic.self=cyclic;cyclic.map=original;"
			"var cycleCopy=structuredClone(cyclic);"
			"if(cycleCopy===cyclic||cycleCopy.self!==cycleCopy||!(cycleCopy.map instanceof Map))"
				"bad('cyclic graph identity');"
			"var backing=new ArrayBuffer(4),bytes=new Uint8Array(backing);"
			"bytes[0]=17;bytes[1]=34;"
			"var views={a:new Uint8Array(backing,0,2),b:new Uint16Array(backing,0,2)};"
			"var viewCopy=structuredClone(views);"
			"if(viewCopy.a.buffer===backing||viewCopy.a.buffer!==viewCopy.b.buffer||"
				"viewCopy.a[0]!==17||viewCopy.a[1]!==34)bad('typed-array backing graph');"
			"var extras=structuredClone({d:new Date(123),r:/ab/gi,s:new Set([1,2])});"
			"if(!(extras.d instanceof Date)||extras.d.getTime()!==123||"
				"!(extras.r instanceof RegExp)||extras.r.source!=='ab'||"
				"!(extras.s instanceof Set)||!extras.s.has(2))bad('structured built-ins');"
			"var transfer=new Uint8Array([4,5]).buffer;"
			"var moved=structuredClone({buffer:transfer},{transfer:[transfer]});"
			"if(transfer.byteLength!==0||new Uint8Array(moved.buffer)[1]!==5)"
				"bad('ArrayBuffer transfer');"
			"var threw=false;try{structuredClone(function(){});}catch(e){"
				"threw=e&&e.name==='DataCloneError';}"
			"if(!threw)bad('unsupported value did not throw DataCloneError');"
			"})();";
		unsigned char ok = js_exec(thread, (const unsigned char *)t92,
				strlen(t92), "driver-t92-structured-clone.js");
		if (!ok) {
			fprintf(stderr, "FAIL: Test 92 -- structuredClone lost a browser "
					"data contract\n");
			return 1;
		}
	}
	fprintf(stderr, "=== Test 92 PASS: Facebook query Maps and general "
			"structured data clone independently ===\n");

	/* --- Test 93 (#167, fixes1292): JS_DiscardPendingJobsForContext -- exact
	 * job-queue API contract, independent of MacSurf's navigation glue.
	 *
	 * QuickJS's rt->job_list is per-RUNTIME, not per-context: JS_EnqueueJob
	 * stores a raw JSContext* per queued job, and only JS_FreeRuntime ever
	 * drains it -- JS_FreeContext does not touch it at all (verified
	 * directly in both quickjs-macos9/quickjs.c and browser/libquickjs/
	 * quickjs.c, identical). A same-runtime navigation that frees the old
	 * JSContext while jobs queued against it are still pending leaves those
	 * jobs holding a dangling ctx; JS_ExecutePendingJob later calls
	 * job_func(dangling_ctx, ...) -- a genuine use-after-free, the suspected
	 * cause of Facebook's repeat-navigation regression. This test exercises
	 * the raw primitive directly (not through js_newthread) so its contract
	 * is proven in isolation first: discard ONLY the named context's jobs,
	 * return the EXACT count discarded, and leave a surviving context's own
	 * job -- queued both before and after the discarded context's jobs --
	 * untouched. An off-by-one, or an implementation that just clears the
	 * whole queue, would both pass a looser test than this one. */
	fprintf(stderr, "\n=== Test 93: JS_DiscardPendingJobsForContext exact "
			"count + ordering contract (#167, fixes1292) ===\n");
	{
		JSRuntime *rt93 = JS_NewRuntime();
		JSContext *ctxA = NULL, *ctxB = NULL;
		int n;

		if (rt93 == NULL) {
			fprintf(stderr, "FAIL: JS_NewRuntime\n"); return 1;
		}
		ctxA = JS_NewContext(rt93);
		ctxB = JS_NewContext(rt93);
		if (ctxA == NULL || ctxB == NULL) {
			fprintf(stderr, "FAIL: JS_NewContext\n"); return 1;
		}
		t93_a_fired = 0;
		t93_b_fired = 0;

		/* Enqueue A, B, A -- the exact interleaving a real same-heap
		 * navigation with pending work can produce (page 1's jobs both
		 * before and after whatever page 2 has already queued by the
		 * time a discard would run). */
		JS_EnqueueJob(ctxA, t93_job_mark_a, 0, NULL);
		JS_EnqueueJob(ctxB, t93_job_mark_b, 0, NULL);
		JS_EnqueueJob(ctxA, t93_job_mark_a, 0, NULL);

		/* THE DISCARD. Must report exactly 2 -- both of A's jobs, no
		 * more, no less. */
		n = JS_DiscardPendingJobsForContext(rt93, ctxA);
		if (n != 2) {
			fprintf(stderr, "FAIL: Test 93 -- discarded %d jobs for A, "
					"expected exactly 2\n", n);
			return 1;
		}
		fprintf(stderr, "JS_DiscardPendingJobsForContext(A) returned "
				"exactly 2, as expected\n");

		/* A is now safe to free -- no job_list entry still points at it. */
		JS_FreeContext(ctxA);

		/* Drain what's left. Only B's single job may fire; if any of A's
		 * survived the discard, JS_ExecutePendingJob calls job_func
		 * against the just-freed ctxA above -- a real use-after-free
		 * ASan traps on directly. */
		{
			JSContext *jctx = NULL;
			int pumped = 0;
			while (JS_IsJobPending(rt93) && pumped < 8) {
				int r = JS_ExecutePendingJob(rt93, &jctx);
				if (r < 0) {
					fprintf(stderr,
							"FAIL: Test 93 -- pending job threw\n");
					return 1;
				}
				pumped++;
			}
		}

		if (t93_a_fired != 0) {
			fprintf(stderr, "FAIL: Test 93 -- a discarded A job fired "
					"anyway (fired=%d)\n", t93_a_fired);
			return 1;
		}
		if (t93_b_fired != 1) {
			fprintf(stderr, "FAIL: Test 93 -- surviving B job did not "
					"fire exactly once (fired=%d)\n", t93_b_fired);
			return 1;
		}

		JS_FreeContext(ctxB);
		JS_FreeRuntime(rt93);
	}
	fprintf(stderr, "=== Test 93 PASS: exact discard count, surviving "
			"context's job still fires, no UAF ===\n");

	/* --- Test 94 (#167, fixes1292): the SAME bug, proven through the real
	 * MacSurf navigation code path (js_newthread), not just the raw
	 * primitive. Test 93 proves JS_DiscardPendingJobsForContext's own
	 * contract; this proves js_newthread actually calls it at the right
	 * point in the right order relative to JS_FreeContext, using the real
	 * compiled macsurf_qjs.c -- the same "run it through the real engine,
	 * not a reimplementation" discipline as Test 25's realm-reset repro.
	 * Arms 24 unpumped Promise reactions (shape diversity, same idea as
	 * Test 25's timers, so a UAF on a queued job's argv is something ASan
	 * can actually catch), navigates the SAME heap while they're still
	 * pending, then pumps -- pre-fix this traps as heap-use-after-free
	 * inside JS_ExecutePendingJob; post-fix it must run clean AND the
	 * surviving (new) realm's own Promise job must still fire. */
	fprintf(stderr, "\n=== Test 94: same-heap navigation discards stale "
			"Promise jobs, no UAF (#167, fixes1292) ===\n");
	{
		extern void macsurf_qjs_pump_all(void);
		extern int macsurf_qjs_test_last_jobdrop(void);
		struct jsheap *h94 = NULL;
		struct jsthread *t94_1 = NULL, *t94_2 = NULL;
		int dropped;
		const char *arm =
			"for(var i=0;i<24;i++){(function(n){"
				"var o={a:n,b:'x'+n,c:[n,n+1],d:{deep:{deeper:n}}};"
				"Promise.resolve(o).then(function(v){return v.d.deep.deeper+n;});"
			"})(i);}";
		const char *survive_arm =
			"window.__t94_ran=false;"
			"Promise.resolve(1).then(function(){window.__t94_ran=true;});";
		const char *survive_check =
			"(function(){if(window.__t94_ran!==true)"
			"throw new Error('ASSERT FAIL: realm 2 promise job did not run');"
			"})();";
		unsigned char ok;
		int pump;

		if (js_newheap(20000, &h94) != NSERROR_OK) {
			fprintf(stderr, "FAIL: js_newheap\n"); return 1;
		}
		if (js_newthread(h94, NULL, (void *)&htmlc, &t94_1) != NSERROR_OK) {
			fprintf(stderr, "FAIL: js_newthread(1)\n"); return 1;
		}
		ok = js_exec(t94_1, (const unsigned char *)arm, strlen(arm),
				"driver-t94-arm.js");
		if (!ok) {
			fprintf(stderr, "FAIL: Test 94 -- arm script threw\n");
			return 1;
		}
		fprintf(stderr, "armed 24 unpumped Promise reactions on realm 1\n");

		/* THE NAVIGATION: same heap, second js_newthread -- frees realm
		 * 1's JSContext while its 24 jobs are still in rt->job_list. */
		if (js_newthread(h94, NULL, (void *)&htmlc, &t94_2) != NSERROR_OK) {
			fprintf(stderr, "FAIL: navigation js_newthread(2)\n");
			return 1;
		}
		fprintf(stderr, "navigated (realm reset) with jobs pending\n");

		/* THE DETERMINISTIC CHECK: js_newthread must have discarded at
		 * least the 24 jobs `arm` queued at the point it froze heap->ctx
		 * to the fresh context -- not "some crash didn't happen," a real,
		 * testable count from the real glue. (>=, not ==: realm 1's own
		 * setup/init path may legitimately queue a small amount of its
		 * own incidental work before `arm` runs; this test's job is to
		 * prove OUR 24 got caught, not to pin an unrelated baseline.) */
		dropped = macsurf_qjs_test_last_jobdrop();
		if (dropped < 24) {
			fprintf(stderr, "FAIL: Test 94 -- js_newthread discarded only "
					"%d stale jobs, expected at least 24\n", dropped);
			return 1;
		}
		fprintf(stderr, "js_newthread discarded %d stale jobs (>= the 24 "
				"this test armed), as expected\n", dropped);

		/* Queue a job on the SURVIVING (new) context too, so pumping
		 * proves more than "didn't crash": realm 2's own work must still
		 * run correctly after realm 1's is discarded. */
		ok = js_exec(t94_2, (const unsigned char *)survive_arm,
				strlen(survive_arm), "driver-t94-survive-arm.js");
		if (!ok) {
			fprintf(stderr, "FAIL: Test 94 -- survive-arm script threw\n");
			return 1;
		}

		/* THE PUMP. Pre-fix, this is where JS_ExecutePendingJob dequeues
		 * one of realm 1's 24 jobs and calls job_func against the freed
		 * ctx -- ASan traps here as heap-use-after-free if the bug is
		 * present. */
		for (pump = 0; pump < 8; pump++) macsurf_qjs_pump_all();

		ok = js_exec(t94_2, (const unsigned char *)survive_check,
				strlen(survive_check), "driver-t94-survive-check.js");
		if (!ok) {
			fprintf(stderr, "FAIL: Test 94 -- realm 2's own Promise job "
					"did not run after realm 1's were discarded\n");
			return 1;
		}

		js_destroythread(t94_1);
		js_destroythread(t94_2);
		js_destroyheap(h94);
	}
	fprintf(stderr, "=== Test 94 PASS: stale-realm Promise jobs discarded "
			"on navigation, surviving realm's own jobs still run, no UAF "
			"===\n");

	/* ---------------------------------------------------------------
	 * fixes1299 (#167) - a LOSING calc() declaration for min-height must
	 * not clobber the WINNING declaration's macsurf_calc_expr slot.
	 *
	 * Root-caused against real Facebook CSS this session: the atomic
	 * class .xpvvgw5{min-height:calc(100vh - var(--header-height))}
	 * rendered a ~22700px box against a ~657px real viewport. Traced to
	 * select/properties/helpers.c's five shared calc-capable cascade
	 * helpers (css__cascade_border_width/length_auto/length_normal/
	 * length_none/length): each wrote
	 * state->computed->macsurf_calc_expr[slot] UNCONDITIONALLY, before
	 * css__outranks_existing() decided whether the declaration actually
	 * wins. The scalar min-height value (length/unit passed to fun())
	 * was correctly gated on the outranks check -- only the SIDE-TABLE
	 * write was not.
	 *
	 * This reproduces the exact real-world mechanism, not just the
	 * unit-level bug: a deferred var()-containing declaration is always
	 * resolved in a FINAL pass strictly after every non-deferred
	 * declaration has already cascaded (css_select__resolve_pending_vars
	 * in css_select.c), regardless of the deferred declaration's own
	 * specificity relative to those already-cascaded ones.
	 *
	 *   #hi  { min-height: calc(900px + 1px) }          -- ID, no var(),
	 *                                                       cascades INLINE
	 *   .lo  { min-height: calc(50px + var(--z)) }       -- class, HAS
	 *                                                       var(), DEFERRED
	 *
	 * #hi (higher specificity) cascades first in real time (processed
	 * later in the ascending-specificity walk than .lo, but with no
	 * competing entry yet since .lo's declaration was only QUEUED) and
	 * correctly wins. .lo's deferred declaration is resolved in the
	 * final pass, restores ITS OWN lower specificity, and correctly
	 * loses css__outranks_existing() -- fun() is correctly never called
	 * for it. Pre-fix, its unconditional write still clobbers #hi's
	 * already-finalized slot with "calc(50px + 1px)" (post-substitution
	 * text) on its way to correctly losing. Post-fix, the slot keeps
	 * #hi's "calc(900px + 1px)".
	 *
	 * The assertion is the winning declaration's EXACT TEXT surviving in
	 * the slot, not just "min-height ends up SET" -- a boolean "is it
	 * calc-valued" can't see this bug at all, since the scalar length/
	 * unit were never wrong; only the slot's owner was. */
	fprintf(stderr, "\n=== Test 95: losing calc() declaration cannot "
			"clobber the winning one's slot (fixes1299) ===\n");
	{
		const char *t95_html =
			"<html><body><div id=\"hi\" class=\"lo\">X</div></body></html>";
		const char *t95_css =
			":root { --z: 1px }"
			"#hi { min-height: calc(900px + 1px) }"
			".lo { min-height: calc(50px + var(--z)) }";
		struct html_content t95c;
		dom_hubbub_parser *t95p = NULL;
		dom_document *t95doc = NULL;
		dom_node *t95root = NULL;
		css_select_ctx *t95ctx = NULL;
		css_stylesheet *t95ua = NULL;
		css_stylesheet *t95auth = NULL;
		dom_hubbub_parser_params t95params;
		css_stylesheet_params t95sp;
		void *t95_box_ctx = NULL;
		nserror t95err;
		dom_exception t95derr;
		bool t95_found;

		memset(&t95params, 0, sizeof(t95params));
		t95params.fix_enc = true;
		t95derr = dom_hubbub_parser_create(&t95params, &t95p, &t95doc);
		if (t95derr != DOM_HUBBUB_OK || t95p == NULL) {
			fprintf(stderr, "FAIL: Test 95 parser create %d\n",
					(int)t95derr);
			return 1;
		}
		if (dom_hubbub_parser_parse_chunk(t95p,
				(const uint8_t *)t95_html,
				strlen(t95_html)) != DOM_HUBBUB_OK ||
				dom_hubbub_parser_completed(t95p) !=
					DOM_HUBBUB_OK) {
			fprintf(stderr, "FAIL: Test 95 parse\n");
			return 1;
		}
		dom_hubbub_parser_destroy(t95p);

		memset(&t95c, 0, sizeof(t95c));
		t95c.base_url = g_base_url;
		t95c.document = t95doc;
		t95c.quirks = DOM_DOCUMENT_QUIRKS_MODE_NONE;
		t95c.enable_scripting = false;
		if (css_select_ctx_create(&t95ctx) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 95 select_ctx\n");
			return 1;
		}
		t95c.select_ctx = t95ctx;

		memset(&t95sp, 0, sizeof(t95sp));
		t95sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t95sp.level = CSS_LEVEL_3;
		t95sp.charset = "UTF-8";
		t95sp.url = "resource:default.css";
		t95sp.title = "default";
		t95sp.resolve = harness_css_resolve_url;
		if (css_stylesheet_create(&t95sp, &t95ua) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 95 UA sheet\n");
			return 1;
		}
		{
			const char *ua = "html,body,div{display:block}";
			(void)css_stylesheet_append_data(t95ua,
					(const uint8_t *)ua, strlen(ua));
			(void)css_stylesheet_data_done(t95ua);
		}
		if (css_select_ctx_append_sheet(t95ctx, t95ua,
				CSS_ORIGIN_UA, "screen") != CSS_OK) {
			fprintf(stderr, "FAIL: Test 95 UA append\n");
			return 1;
		}

		memset(&t95sp, 0, sizeof(t95sp));
		t95sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t95sp.level = CSS_LEVEL_3;
		t95sp.charset = "UTF-8";
		t95sp.url = "http://local/t95.css";
		t95sp.title = "author";
		t95sp.resolve = harness_css_resolve_url;
		if (css_stylesheet_create(&t95sp, &t95auth) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 95 author sheet\n");
			return 1;
		}
		{
			css_error ae = css_stylesheet_append_data(t95auth,
					(const uint8_t *)t95_css,
					strlen(t95_css));
			if (ae != CSS_OK && ae != CSS_NEEDDATA) {
				fprintf(stderr, "FAIL: Test 95 author append=%d\n",
						(int)ae);
				return 1;
			}
			(void)css_stylesheet_data_done(t95auth);
		}
		if (css_select_ctx_append_sheet(t95ctx, t95auth,
				CSS_ORIGIN_AUTHOR, "screen") != CSS_OK) {
			fprintf(stderr, "FAIL: Test 95 author append sheet\n");
			return 1;
		}

		t95c.media.type = CSS_MEDIA_SCREEN;
		t95c.media.width = INTTOFIX(800);
		t95c.media.height = INTTOFIX(600);
		t95c.media.orientation = CSS_MEDIA_ORIENTATION_LANDSCAPE;
		t95c.unit_len_ctx.viewport_width = INTTOFIX(800);
		t95c.unit_len_ctx.viewport_height = INTTOFIX(600);
		t95c.unit_len_ctx.device_dpi = INTTOFIX(90);
		t95c.unit_len_ctx.font_size_default = INTTOFIX(16);
		t95c.unit_len_ctx.font_size_minimum = INTTOFIX(8);
		if (lwc_intern_string("*", 1, &t95c.universal) !=
				lwc_error_ok) {
			fprintf(stderr, "FAIL: Test 95 universal\n");
			return 1;
		}
		t95c.base.status = CONTENT_STATUS_LOADING;
		t95c.base.active = 0;
		t95c.base.handler = &g_dummy_handler;

		if (dom_document_get_document_element(t95doc,
				(void *)&t95root) != DOM_NO_ERR ||
				t95root == NULL) {
			fprintf(stderr, "FAIL: Test 95 doc element\n");
			return 1;
		}
		g_initial_build_done = 0;
		g_initial_build_ok = false;
		t95err = dom_to_box(t95root, &t95c, initial_build_cb,
				&t95_box_ctx);
		dom_node_unref(t95root);
		if (t95err != NSERROR_OK) {
			fprintf(stderr, "FAIL: Test 95 dom_to_box=%d\n",
					(int)t95err);
			return 1;
		}
		harness_pump_all(100000);
		if (!g_initial_build_done || !g_initial_build_ok) {
			fprintf(stderr, "FAIL: Test 95 build done=%d ok=%d\n",
					g_initial_build_done,
					(int)g_initial_build_ok);
			return 1;
		}

		{
			uint8_t t95_slot = 0;
			uint32_t t95_writes;
			uint32_t t95_last_spec;
			/* #hi has one ID selector: libcss packs specificity as
			 * (a<<16)|(b<<8)|c|d with a=ID count, so a lone ID is
			 * 0x010000 = 65536 -- directly confirmed against this
			 * exact fixture while building this test (a raw dump of
			 * state->current_specificity at the winning write). */
			const uint32_t t95_expect_spec = 65536;

			t95_found = t95_calc_of(t95c.layout, "lo", &t95_slot);
			fprintf(stderr, "  min-height calc slot for #hi.lo = %s%d\n",
					t95_found ? "" : "(not calc-valued) ",
					t95_found ? (int)t95_slot : -1);

			if (!t95_found) {
				fprintf(stderr, "FAIL: Test 95 -- min-height was "
						"not calc-valued at all; something "
						"upstream of this bug broke (test "
						"setup issue, not the bug under "
						"test)\n");
				return 1;
			}

			t95_writes = cssprobe_calc_slot_write_count(t95_slot);
			t95_last_spec =
				cssprobe_calc_slot_write_last_spec(t95_slot);
			fprintf(stderr, "  slot=%d writes=%lu last_spec=%lu "
					"(want writes=1 spec=%lu)\n",
					(int)t95_slot, (unsigned long)t95_writes,
					(unsigned long)t95_last_spec,
					(unsigned long)t95_expect_spec);

			/* THE ASSERTION: exactly ONE write to this slot, at
			 * #hi's specificity. Pre-fix, this element's min-height
			 * calc slot sees TWO writes -- #hi's winning inline
			 * cascade, THEN .lo's losing deferred var()-resolution
			 * pass unconditionally overwriting it before its own
			 * (correctly failing) outranks check, leaving writes=2
			 * and last_spec=.lo's lower class specificity, not
			 * #hi's. A write COUNT, not a boolean "is it calc-
			 * valued", is what actually distinguishes fixed from
			 * broken here -- both states report min-height as SET/
			 * CALC either way. */
			if (t95_writes != 1) {
				fprintf(stderr, "FAIL: Test 95 -- slot %d was "
						"written %lu times, expected "
						"exactly 1. >1 means the losing "
						"deferred .lo declaration reached "
						"the unconditional write this fix "
						"removed -- the exact split-brain: "
						"min-height correctly computes as "
						"SET/CALC either way (the scalar "
						"outranks check works), but the "
						"side-table gets clobbered by "
						"whichever declaration cascades "
						"LAST, not whichever one WINS.\n",
						(int)t95_slot,
						(unsigned long)t95_writes);
				return 1;
			}
			if (t95_last_spec != t95_expect_spec) {
				fprintf(stderr, "FAIL: Test 95 -- slot %d's one "
						"write carried specificity %lu, "
						"expected #hi's %lu -- the wrong "
						"declaration won\n", (int)t95_slot,
						(unsigned long)t95_last_spec,
						(unsigned long)t95_expect_spec);
				return 1;
			}
		}
	}
	fprintf(stderr, "=== Test 95 PASS: losing deferred calc() declaration "
			"did not clobber the winning declaration's calc_expr slot "
			"===\n");

	/* ---------------------------------------------------------------
	 * fixes1300 (#167) - what does MacSurf's own consumer code actually
	 * resolve .xpvvgw5's real min-height calc() to, under the real
	 * hardware viewport (993x609, from the fixes1299 hardware log's
	 * "LIFE VPORT finish ... unit_ctx vw=993 vh=609")?
	 *
	 * fixes1299 fixed WHICH declaration's calc_expr survives a cascade
	 * conflict; it says nothing about whether that surviving expression
	 * then EVALUATES correctly. The hardware log pulled after fixes1299
	 * shipped still shows .xpvvgw5 at h=22723/23136, unchanged -- so this
	 * test exercises the evaluation path directly: real rule text
	 * (verbatim from harness/fbcdn.net-css-v5.css), real selector-scoped
	 * custom property, real viewport, real consumer call
	 * (ns_computed_min_height + css_unit_len2device_px, the exact two
	 * calls layout_internal.h:855/862 makes), no synthetic shortcuts.
	 *
	 * --header-height is defined in the real bundle only inside
	 *   .x1s26u81.x1s26u81,.x1s26u81.x1s26u81:root{...--header-height:0px...}
	 * -- StyleX's doubled-class specificity-boost idiom, gating the
	 * whole design-token block behind :root ALSO carrying class
	 * x1s26u81. This fixture gives that its best chance: <html
	 * class="x1s26u81">. If --header-height resolves (0px) and 100vh
	 * evaluates against the real 609px viewport, calc(100vh -
	 * var(--header-height)) should land near 600px, not 22723px. */
	fprintf(stderr, "\n=== Test 96: real .xpvvgw5 min-height calc() "
			"resolves to a real viewport-scale pixel value ===\n");
	{
		const char *t96_html =
			"<html class=\"x1s26u81\"><body>"
			"<div class=\"xpvvgw5\">X</div>"
			"</body></html>";
		/* Verbatim from harness/fbcdn.net-css-v5.css: the selector and
		 * the two min-height declarations on .xpvvgw5 are copied
		 * character-for-character; the custom-property block is
		 * trimmed to just --header-height (the real block carries ~200
		 * unrelated design tokens on the same selector). */
		const char *t96_css =
			".x1s26u81.x1s26u81,.x1s26u81.x1s26u81:root"
			"{--header-height:0px}"
			".xpvvgw5{min-height:calc(100vh - var(--header-height));"
			"min-height:calc(100dvh - var(--header-height))}";
		struct html_content t96c;
		dom_hubbub_parser *t96p = NULL;
		dom_document *t96doc = NULL;
		dom_node *t96root = NULL;
		css_select_ctx *t96ctx = NULL;
		css_stylesheet *t96ua = NULL;
		css_stylesheet *t96auth = NULL;
		dom_hubbub_parser_params t96params;
		css_stylesheet_params t96sp;
		void *t96_box_ctx = NULL;
		nserror t96err;
		dom_exception t96derr;
		struct box *t96box;

		memset(&t96params, 0, sizeof(t96params));
		t96params.fix_enc = true;
		t96derr = dom_hubbub_parser_create(&t96params, &t96p, &t96doc);
		if (t96derr != DOM_HUBBUB_OK || t96p == NULL) {
			fprintf(stderr, "FAIL: Test 96 parser create %d\n",
					(int)t96derr);
			return 1;
		}
		if (dom_hubbub_parser_parse_chunk(t96p,
				(const uint8_t *)t96_html,
				strlen(t96_html)) != DOM_HUBBUB_OK ||
				dom_hubbub_parser_completed(t96p) !=
					DOM_HUBBUB_OK) {
			fprintf(stderr, "FAIL: Test 96 parse\n");
			return 1;
		}
		dom_hubbub_parser_destroy(t96p);

		memset(&t96c, 0, sizeof(t96c));
		t96c.base_url = g_base_url;
		t96c.document = t96doc;
		t96c.quirks = DOM_DOCUMENT_QUIRKS_MODE_NONE;
		t96c.enable_scripting = false;
		if (css_select_ctx_create(&t96ctx) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 96 select_ctx\n");
			return 1;
		}
		t96c.select_ctx = t96ctx;

		memset(&t96sp, 0, sizeof(t96sp));
		t96sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t96sp.level = CSS_LEVEL_3;
		t96sp.charset = "UTF-8";
		t96sp.url = "resource:default.css";
		t96sp.title = "default";
		t96sp.resolve = harness_css_resolve_url;
		if (css_stylesheet_create(&t96sp, &t96ua) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 96 UA sheet\n");
			return 1;
		}
		{
			const char *ua = "html,body,div{display:block}";
			(void)css_stylesheet_append_data(t96ua,
					(const uint8_t *)ua, strlen(ua));
			(void)css_stylesheet_data_done(t96ua);
		}
		if (css_select_ctx_append_sheet(t96ctx, t96ua,
				CSS_ORIGIN_UA, "screen") != CSS_OK) {
			fprintf(stderr, "FAIL: Test 96 UA append\n");
			return 1;
		}

		memset(&t96sp, 0, sizeof(t96sp));
		t96sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t96sp.level = CSS_LEVEL_3;
		t96sp.charset = "UTF-8";
		t96sp.url = "http://local/t96.css";
		t96sp.title = "author";
		t96sp.resolve = harness_css_resolve_url;
		if (css_stylesheet_create(&t96sp, &t96auth) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 96 author sheet\n");
			return 1;
		}
		{
			css_error ae = css_stylesheet_append_data(t96auth,
					(const uint8_t *)t96_css,
					strlen(t96_css));
			if (ae != CSS_OK && ae != CSS_NEEDDATA) {
				fprintf(stderr, "FAIL: Test 96 author append=%d\n",
						(int)ae);
				return 1;
			}
			(void)css_stylesheet_data_done(t96auth);
		}
		if (css_select_ctx_append_sheet(t96ctx, t96auth,
				CSS_ORIGIN_AUTHOR, "screen") != CSS_OK) {
			fprintf(stderr, "FAIL: Test 96 author append sheet\n");
			return 1;
		}

		/* Real hardware viewport from the fixes1299 log's
		 * "LIFE VPORT finish" line, not the 800x600 harness default. */
		t96c.media.type = CSS_MEDIA_SCREEN;
		t96c.media.width = INTTOFIX(993);
		t96c.media.height = INTTOFIX(609);
		t96c.media.orientation = CSS_MEDIA_ORIENTATION_LANDSCAPE;
		t96c.unit_len_ctx.viewport_width = INTTOFIX(993);
		t96c.unit_len_ctx.viewport_height = INTTOFIX(609);
		t96c.unit_len_ctx.device_dpi = INTTOFIX(96);
		t96c.unit_len_ctx.font_size_default = INTTOFIX(16);
		t96c.unit_len_ctx.font_size_minimum = INTTOFIX(8);
		if (lwc_intern_string("*", 1, &t96c.universal) !=
				lwc_error_ok) {
			fprintf(stderr, "FAIL: Test 96 universal\n");
			return 1;
		}
		t96c.base.status = CONTENT_STATUS_LOADING;
		t96c.base.active = 0;
		t96c.base.handler = &g_dummy_handler;

		if (dom_document_get_document_element(t96doc,
				(void *)&t96root) != DOM_NO_ERR ||
				t96root == NULL) {
			fprintf(stderr, "FAIL: Test 96 doc element\n");
			return 1;
		}
		g_initial_build_done = 0;
		g_initial_build_ok = false;
		t96err = dom_to_box(t96root, &t96c, initial_build_cb,
				&t96_box_ctx);
		dom_node_unref(t96root);
		if (t96err != NSERROR_OK) {
			fprintf(stderr, "FAIL: Test 96 dom_to_box=%d\n",
					(int)t96err);
			return 1;
		}
		harness_pump_all(100000);
		if (!g_initial_build_done || !g_initial_build_ok) {
			fprintf(stderr, "FAIL: Test 96 build done=%d ok=%d\n",
					g_initial_build_done,
					(int)g_initial_build_ok);
			return 1;
		}

		t96box = t96_box_of_class(t96c.layout, "xpvvgw5");
		if (t96box == NULL || t96box->style == NULL) {
			fprintf(stderr, "FAIL: Test 96 -- .xpvvgw5 box not "
					"found\n");
			return 1;
		}

		{
			enum css_min_height_e t96type;
			css_fixed t96value = 0;
			css_unit t96unit = CSS_UNIT_PX;
			int t96px;

			t96type = ns_computed_min_height(t96box->style,
					&t96value, &t96unit);
			if (t96type != CSS_MIN_HEIGHT_SET) {
				fprintf(stderr, "FAIL: Test 96 -- min-height did "
						"not compute as SET at all "
						"(type=%d) -- the calc() "
						"declaration was lost before "
						"reaching the box, not "
						"mis-evaluated\n", (int)t96type);
				return 1;
			}
			if (t96unit != CSS_UNIT_CALC) {
				fprintf(stderr, "FAIL: Test 96 -- min-height "
						"unit=%d, expected CSS_UNIT_CALC "
						"(%d) -- not exercising the calc "
						"path this test targets\n",
						(int)t96unit,
						(int)CSS_UNIT_CALC);
				return 1;
			}

			/* The exact call layout_internal.h:862 makes. */
			t96px = (int)FIXTOINT(css_unit_len2device_px(
					t96box->style, &t96c.unit_len_ctx,
					t96value, t96unit));
			fprintf(stderr, "  .xpvvgw5 min-height calc() resolves "
					"to %dpx (real viewport 993x609, real "
					"--header-height:0px)\n", t96px);

			/* Real symptom was h=22723-23136 against a 609px real
			 * viewport -- roughly 37-38x too tall. A correct
			 * evaluation of calc(100vh - 0px) against a 609px
			 * viewport must land at or under the viewport height
			 * itself; give generous headroom (2x) over 609 so this
			 * isn't a hair-trigger pixel-rounding assertion, while
			 * still being nowhere near 22723. */
			if (t96px < 0 || t96px > 1218) {
				fprintf(stderr, "FAIL: Test 96 -- resolved %dpx, "
						"expected roughly 600px "
						"(100vh - 0px against a 609px "
						"viewport). %s\n", t96px,
						t96px > 10000 ?
						"This reproduces the real "
						"22723px symptom in isolation "
						"-- the bug is in the "
						"evaluation path itself, not "
						"page-specific DOM/class "
						"structure." :
						"Unexpected direction of "
						"failure -- inspect manually.");
				return 1;
			}
		}
	}
	fprintf(stderr, "=== Test 96 PASS: .xpvvgw5's real min-height calc() "
			"resolves to a real, viewport-scale pixel value ===\n");

	/* ---------------------------------------------------------------
	 * fixes1307 (#167, C0) - does `min-height: inherit` on a CHILD of a
	 * calc()-valued min-height parent correctly inherit the parent's
	 * calc EXPRESSION (via macsurf_calc_expr[slot]), or does it inherit
	 * the raw computed unit/length pair (unit=CALC, length=slot index)
	 * without the side-table content that slot index actually points
	 * at meaning anything on the CHILD's own style?
	 *
	 * Root-caused directly from the fixes1306 hardware trace: xpvvgw5's
	 * real child (atomic class x1t2pt76, verbatim CSS in the real
	 * bundle: `.x1t2pt76{min-height:inherit}`) shows
	 * item->base_size=22434 (matching the OUTER container's own final
	 * height almost exactly) while every layout_flex_item call traced
	 * for that exact box produced 589. base_size never came from that
	 * box's own real height at all -- min_main (this item's OWN CSS
	 * min-height, resolved via layout_find_dimensions inside
	 * layout_flex_ctx__populate_item_data, clamping item->main_size
	 * upward at layout_flex.c:606-613) is the one per-item field that
	 * bypasses layout_flex_item entirely and reads straight off
	 * ns_computed_min_height/css_unit_len2device_px for the CHILD's
	 * OWN style -- exactly the `inherit` mechanism this test isolates. */
	fprintf(stderr, "\n=== Test 98: `min-height:inherit` on a calc()-"
			"valued parent produces a real, correct pixel value "
			"on the child ===\n");
	{
		const char *t98_html =
			"<html class=\"x1s26u81\"><body>"
			"<div class=\"xpvvgw5\"><div class=\"x1t2pt76\">X</div>"
			"</div></body></html>";
		/* Verbatim from harness/fbcdn.net-css-v5.css: the
		 * --header-height scoping rule (trimmed to the one relevant
		 * declaration, see Test 96), the real .xpvvgw5 rule, and the
		 * real .x1t2pt76 rule -- `min-height:inherit`, byte for byte
		 * what ships. */
		const char *t98_css =
			".x1s26u81.x1s26u81,.x1s26u81.x1s26u81:root"
			"{--header-height:0px}"
			".xpvvgw5{min-height:calc(100vh - var(--header-height))}"
			".x1t2pt76{min-height:inherit}";
		struct html_content t98c;
		dom_hubbub_parser *t98p = NULL;
		dom_document *t98doc = NULL;
		dom_node *t98root = NULL;
		css_select_ctx *t98ctx = NULL;
		css_stylesheet *t98ua = NULL;
		css_stylesheet *t98auth = NULL;
		dom_hubbub_parser_params t98params;
		css_stylesheet_params t98sp;
		void *t98_box_ctx = NULL;
		nserror t98err;
		dom_exception t98derr;
		struct box *t98child;

		memset(&t98params, 0, sizeof(t98params));
		t98params.fix_enc = true;
		t98derr = dom_hubbub_parser_create(&t98params, &t98p, &t98doc);
		if (t98derr != DOM_HUBBUB_OK || t98p == NULL) {
			fprintf(stderr, "FAIL: Test 98 parser create %d\n",
					(int)t98derr);
			return 1;
		}
		if (dom_hubbub_parser_parse_chunk(t98p,
				(const uint8_t *)t98_html,
				strlen(t98_html)) != DOM_HUBBUB_OK ||
				dom_hubbub_parser_completed(t98p) !=
					DOM_HUBBUB_OK) {
			fprintf(stderr, "FAIL: Test 98 parse\n");
			return 1;
		}
		dom_hubbub_parser_destroy(t98p);

		memset(&t98c, 0, sizeof(t98c));
		t98c.base_url = g_base_url;
		t98c.document = t98doc;
		t98c.quirks = DOM_DOCUMENT_QUIRKS_MODE_NONE;
		t98c.enable_scripting = false;
		if (css_select_ctx_create(&t98ctx) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 98 select_ctx\n");
			return 1;
		}
		t98c.select_ctx = t98ctx;

		memset(&t98sp, 0, sizeof(t98sp));
		t98sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t98sp.level = CSS_LEVEL_3;
		t98sp.charset = "UTF-8";
		t98sp.url = "resource:default.css";
		t98sp.title = "default";
		t98sp.resolve = harness_css_resolve_url;
		if (css_stylesheet_create(&t98sp, &t98ua) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 98 UA sheet\n");
			return 1;
		}
		{
			const char *ua = "html,body,div{display:block}";
			(void)css_stylesheet_append_data(t98ua,
					(const uint8_t *)ua, strlen(ua));
			(void)css_stylesheet_data_done(t98ua);
		}
		if (css_select_ctx_append_sheet(t98ctx, t98ua,
				CSS_ORIGIN_UA, "screen") != CSS_OK) {
			fprintf(stderr, "FAIL: Test 98 UA append\n");
			return 1;
		}

		memset(&t98sp, 0, sizeof(t98sp));
		t98sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t98sp.level = CSS_LEVEL_3;
		t98sp.charset = "UTF-8";
		t98sp.url = "http://local/t98.css";
		t98sp.title = "author";
		t98sp.resolve = harness_css_resolve_url;
		if (css_stylesheet_create(&t98sp, &t98auth) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 98 author sheet\n");
			return 1;
		}
		{
			css_error ae = css_stylesheet_append_data(t98auth,
					(const uint8_t *)t98_css,
					strlen(t98_css));
			if (ae != CSS_OK && ae != CSS_NEEDDATA) {
				fprintf(stderr, "FAIL: Test 98 author append=%d\n",
						(int)ae);
				return 1;
			}
			(void)css_stylesheet_data_done(t98auth);
		}
		if (css_select_ctx_append_sheet(t98ctx, t98auth,
				CSS_ORIGIN_AUTHOR, "screen") != CSS_OK) {
			fprintf(stderr, "FAIL: Test 98 author append sheet\n");
			return 1;
		}

		t98c.media.type = CSS_MEDIA_SCREEN;
		t98c.media.width = INTTOFIX(993);
		t98c.media.height = INTTOFIX(609);
		t98c.media.orientation = CSS_MEDIA_ORIENTATION_LANDSCAPE;
		t98c.unit_len_ctx.viewport_width = INTTOFIX(993);
		t98c.unit_len_ctx.viewport_height = INTTOFIX(609);
		t98c.unit_len_ctx.device_dpi = INTTOFIX(96);
		t98c.unit_len_ctx.font_size_default = INTTOFIX(16);
		t98c.unit_len_ctx.font_size_minimum = INTTOFIX(8);
		if (lwc_intern_string("*", 1, &t98c.universal) !=
				lwc_error_ok) {
			fprintf(stderr, "FAIL: Test 98 universal\n");
			return 1;
		}
		t98c.base.status = CONTENT_STATUS_LOADING;
		t98c.base.active = 0;
		t98c.base.handler = &g_dummy_handler;

		if (dom_document_get_document_element(t98doc,
				(void *)&t98root) != DOM_NO_ERR ||
				t98root == NULL) {
			fprintf(stderr, "FAIL: Test 98 doc element\n");
			return 1;
		}
		g_initial_build_done = 0;
		g_initial_build_ok = false;
		t98err = dom_to_box(t98root, &t98c, initial_build_cb,
				&t98_box_ctx);
		dom_node_unref(t98root);
		if (t98err != NSERROR_OK) {
			fprintf(stderr, "FAIL: Test 98 dom_to_box=%d\n",
					(int)t98err);
			return 1;
		}
		harness_pump_all(100000);
		if (!g_initial_build_done || !g_initial_build_ok) {
			fprintf(stderr, "FAIL: Test 98 build done=%d ok=%d\n",
					g_initial_build_done,
					(int)g_initial_build_ok);
			return 1;
		}

		t98child = t96_box_of_class(t98c.layout, "x1t2pt76");
		if (t98child == NULL || t98child->style == NULL) {
			fprintf(stderr, "FAIL: Test 98 -- .x1t2pt76 child box "
					"not found\n");
			return 1;
		}

		{
			enum css_min_height_e t98type;
			css_fixed t98value = 0;
			css_unit t98unit = CSS_UNIT_PX;
			int t98px;

			t98type = ns_computed_min_height(t98child->style,
					&t98value, &t98unit);
			fprintf(stderr, "  .x1t2pt76 (inherit) min-height "
					"type=%d unit=%d\n", (int)t98type,
					(int)t98unit);
			if (t98type != CSS_MIN_HEIGHT_SET) {
				fprintf(stderr, "FAIL: Test 98 -- inherited "
						"min-height did not compute "
						"as SET at all (type=%d) -- "
						"`inherit` lost the property "
						"entirely, not just the calc "
						"expression\n", (int)t98type);
				return 1;
			}

			if (t98unit == CSS_UNIT_CALC) {
				t98px = (int)FIXTOINT(css_unit_len2device_px(
						t98child->style,
						&t98c.unit_len_ctx,
						t98value, t98unit));
			} else {
				t98px = (int)FIXTOINT(css_unit_len2device_px(
						t98child->style,
						&t98c.unit_len_ctx,
						t98value, t98unit));
			}
			fprintf(stderr, "  .x1t2pt76 (inherit) min-height "
					"resolves to %dpx (parent .xpvvgw5's "
					"own calc() resolves to 600px, per "
					"Test 96)\n", t98px);

			/* THE ASSERTION: inherit must produce the SAME real
			 * pixel value the parent's own calc() produces
			 * (~600px, Test 96), not something wildly different.
			 * Same generous 2x headroom as Test 96. */
			if (t98px < 0 || t98px > 1218) {
				fprintf(stderr, "FAIL: Test 98 -- inherited "
						"min-height resolved to %dpx, "
						"expected ~600px matching the "
						"parent's own calc(). %s\n",
						t98px,
						t98px > 10000 ?
						"This reproduces the real "
						"22434px-scale symptom in "
						"isolation -- `inherit` on a "
						"calc()-valued property does "
						"NOT correctly carry the "
						"calc_expr side-table slot to "
						"the child." :
						"Unexpected direction of "
						"failure -- inspect manually.");
				return 1;
			}
		}
	}
	fprintf(stderr, "=== Test 98 PASS: inherited calc() min-height "
			"resolves to the same real pixel value as the "
			"parent's own ===\n");

	/* ---------------------------------------------------------------
	 * fixes1309 (#167, C0) - the FLEXOUTER hardware trace (fixes1308)
	 * named the exact field: margin-bottom=21845px, every other outer
	 * edge zero. The real bundle rule on the exact class carrying it:
	 *
	 *   .x10cihs4{margin-bottom:calc(-100vh + var(--header-height))}
	 *
	 * A NEGATIVE calc(). Against the real 609px viewport and
	 * --header-height:0px this should resolve to ~-609px, not
	 * +21845. While reading libcss/include/libcss/fpmath.h for this,
	 * found a real, independent, sign-handling bug in css_add_fixed's
	 * branchless overflow-saturation trick: it computes
	 * `ux = (ux >> 31) + INT_MAX` using an ARITHMETIC (sign-extending)
	 * shift on a SIGNED int32 -- giving -1 for a negative operand,
	 * so `ux` becomes INT_MAX-1, not INT_MIN. The standard branchless
	 * idiom this is built on needs an UNSIGNED (logical) shift here,
	 * giving 0 or 1 so `ux` lands on INT_MAX or INT_MIN respectively.
	 * css_add_fixed is shared between the CW8 double-path and the
	 * portable int64 path (it's outside the #ifdef __MWERKS__ block),
	 * so this reproduces identically on Linux -- test the REAL rule
	 * text directly rather than a synthetic add(), since css_add_fixed
	 * is only one candidate among several fixed-point primitives this
	 * calc's evaluation touches. */
	fprintf(stderr, "\n=== Test 99: real .x10cihs4 negative calc() "
			"margin-bottom resolves to a real negative pixel "
			"value ===\n");
	{
		const char *t99_html =
			"<html class=\"x1s26u81\"><body>"
			"<div class=\"x10cihs4\">X</div>"
			"</body></html>";
		/* Verbatim from harness/fbcdn.net-css-v5.css. */
		const char *t99_css =
			".x1s26u81.x1s26u81,.x1s26u81.x1s26u81:root"
			"{--header-height:0px}"
			".x10cihs4{margin-bottom:"
			"calc(-100vh + var(--header-height))}";
		struct html_content t99c;
		dom_hubbub_parser *t99p = NULL;
		dom_document *t99doc = NULL;
		dom_node *t99root = NULL;
		css_select_ctx *t99ctx = NULL;
		css_stylesheet *t99ua = NULL;
		css_stylesheet *t99auth = NULL;
		dom_hubbub_parser_params t99params;
		css_stylesheet_params t99sp;
		void *t99_box_ctx = NULL;
		nserror t99err;
		dom_exception t99derr;
		struct box *t99box;

		memset(&t99params, 0, sizeof(t99params));
		t99params.fix_enc = true;
		t99derr = dom_hubbub_parser_create(&t99params, &t99p, &t99doc);
		if (t99derr != DOM_HUBBUB_OK || t99p == NULL) {
			fprintf(stderr, "FAIL: Test 99 parser create %d\n",
					(int)t99derr);
			return 1;
		}
		if (dom_hubbub_parser_parse_chunk(t99p,
				(const uint8_t *)t99_html,
				strlen(t99_html)) != DOM_HUBBUB_OK ||
				dom_hubbub_parser_completed(t99p) !=
					DOM_HUBBUB_OK) {
			fprintf(stderr, "FAIL: Test 99 parse\n");
			return 1;
		}
		dom_hubbub_parser_destroy(t99p);

		memset(&t99c, 0, sizeof(t99c));
		t99c.base_url = g_base_url;
		t99c.document = t99doc;
		t99c.quirks = DOM_DOCUMENT_QUIRKS_MODE_NONE;
		t99c.enable_scripting = false;
		if (css_select_ctx_create(&t99ctx) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 99 select_ctx\n");
			return 1;
		}
		t99c.select_ctx = t99ctx;

		memset(&t99sp, 0, sizeof(t99sp));
		t99sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t99sp.level = CSS_LEVEL_3;
		t99sp.charset = "UTF-8";
		t99sp.url = "resource:default.css";
		t99sp.title = "default";
		t99sp.resolve = harness_css_resolve_url;
		if (css_stylesheet_create(&t99sp, &t99ua) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 99 UA sheet\n");
			return 1;
		}
		{
			const char *ua = "html,body,div{display:block}";
			(void)css_stylesheet_append_data(t99ua,
					(const uint8_t *)ua, strlen(ua));
			(void)css_stylesheet_data_done(t99ua);
		}
		if (css_select_ctx_append_sheet(t99ctx, t99ua,
				CSS_ORIGIN_UA, "screen") != CSS_OK) {
			fprintf(stderr, "FAIL: Test 99 UA append\n");
			return 1;
		}

		memset(&t99sp, 0, sizeof(t99sp));
		t99sp.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
		t99sp.level = CSS_LEVEL_3;
		t99sp.charset = "UTF-8";
		t99sp.url = "http://local/t99.css";
		t99sp.title = "author";
		t99sp.resolve = harness_css_resolve_url;
		if (css_stylesheet_create(&t99sp, &t99auth) != CSS_OK) {
			fprintf(stderr, "FAIL: Test 99 author sheet\n");
			return 1;
		}
		{
			css_error ae = css_stylesheet_append_data(t99auth,
					(const uint8_t *)t99_css,
					strlen(t99_css));
			if (ae != CSS_OK && ae != CSS_NEEDDATA) {
				fprintf(stderr, "FAIL: Test 99 author append=%d\n",
						(int)ae);
				return 1;
			}
			(void)css_stylesheet_data_done(t99auth);
		}
		if (css_select_ctx_append_sheet(t99ctx, t99auth,
				CSS_ORIGIN_AUTHOR, "screen") != CSS_OK) {
			fprintf(stderr, "FAIL: Test 99 author append sheet\n");
			return 1;
		}

		t99c.media.type = CSS_MEDIA_SCREEN;
		t99c.media.width = INTTOFIX(993);
		t99c.media.height = INTTOFIX(609);
		t99c.media.orientation = CSS_MEDIA_ORIENTATION_LANDSCAPE;
		t99c.unit_len_ctx.viewport_width = INTTOFIX(993);
		t99c.unit_len_ctx.viewport_height = INTTOFIX(609);
		t99c.unit_len_ctx.device_dpi = INTTOFIX(96);
		t99c.unit_len_ctx.font_size_default = INTTOFIX(16);
		t99c.unit_len_ctx.font_size_minimum = INTTOFIX(8);
		if (lwc_intern_string("*", 1, &t99c.universal) !=
				lwc_error_ok) {
			fprintf(stderr, "FAIL: Test 99 universal\n");
			return 1;
		}
		t99c.base.status = CONTENT_STATUS_LOADING;
		t99c.base.active = 0;
		t99c.base.handler = &g_dummy_handler;

		if (dom_document_get_document_element(t99doc,
				(void *)&t99root) != DOM_NO_ERR ||
				t99root == NULL) {
			fprintf(stderr, "FAIL: Test 99 doc element\n");
			return 1;
		}
		g_initial_build_done = 0;
		g_initial_build_ok = false;
		t99err = dom_to_box(t99root, &t99c, initial_build_cb,
				&t99_box_ctx);
		dom_node_unref(t99root);
		if (t99err != NSERROR_OK) {
			fprintf(stderr, "FAIL: Test 99 dom_to_box=%d\n",
					(int)t99err);
			return 1;
		}
		harness_pump_all(100000);
		if (!g_initial_build_done || !g_initial_build_ok) {
			fprintf(stderr, "FAIL: Test 99 build done=%d ok=%d\n",
					g_initial_build_done,
					(int)g_initial_build_ok);
			return 1;
		}

		t99box = t96_box_of_class(t99c.layout, "x10cihs4");
		if (t99box == NULL || t99box->style == NULL) {
			fprintf(stderr, "FAIL: Test 99 -- .x10cihs4 box not "
					"found\n");
			return 1;
		}

		{
			uint8_t t99type;
			css_fixed t99value = 0;
			css_unit t99unit = CSS_UNIT_PX;
			int t99px;

			t99type = css_computed_margin_bottom(t99box->style,
					&t99value, &t99unit);
			fprintf(stderr, "  .x10cihs4 margin-bottom type=%d "
					"unit=%d\n", (int)t99type,
					(int)t99unit);
			if (t99type != CSS_MARGIN_SET) {
				fprintf(stderr, "FAIL: Test 99 -- margin-"
						"bottom did not compute as "
						"SET at all (type=%d)\n",
						(int)t99type);
				return 1;
			}

			t99px = (int)FIXTOINT(css_unit_len2device_px(
					t99box->style, &t99c.unit_len_ctx,
					t99value, t99unit));
			fprintf(stderr, "  .x10cihs4 margin-bottom resolves "
					"to %dpx (real viewport 993x609, "
					"expected roughly -609px)\n", t99px);

			/* THE ASSERTION: a negative calc() must resolve to a
			 * real NEGATIVE value close to -609px (100vh of a
			 * 609px viewport, minus 0px header-height), not a
			 * large positive garbage value. Generous band: any
			 * negative value down to -1218 (2x viewport) is
			 * accepted as "roughly right direction and scale";
			 * the real bug produces +21845, a completely
			 * different sign AND magnitude, so this is not a
			 * hair-trigger assertion. */
			if (t99px > 0 || t99px < -1218) {
				fprintf(stderr, "FAIL: Test 99 -- resolved "
						"%dpx, expected a negative "
						"value near -609px. %s\n",
						t99px,
						t99px > 10000 ?
						"This reproduces the real "
						"21845px symptom in isolation "
						"-- negative calc() values "
						"are being corrupted into "
						"large positive ones "
						"somewhere in the fixed-point "
						"math or calc bytecode "
						"evaluation." :
						"Unexpected direction of "
						"failure -- inspect manually.");
				return 1;
			}
		}
	}
	fprintf(stderr, "=== Test 99 PASS: negative calc() margin-bottom "
			"resolves to a real negative pixel value ===\n");

	return 0;
}
