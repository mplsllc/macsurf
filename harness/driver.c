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
#include "content/handlers/html/private.h"		/* html_content, for layout_internal.h */
#include "content/handlers/html/layout_internal.h"	/* fixes929 Test 32 */
extern void macsurf_imgdims_remember(struct nsurl *url, int w, int h);
extern int macsurf_imgdims_lookup(struct nsurl *url, int *w, int *h);	/* fixes921 Test 31: struct content_html_object */
#include "content/handlers/html/private.h"
#include "content/handlers/html/box_construct.h"
#include "content/handlers/javascript/js.h"

#include "libcss/libcss.h"
#include "libcss/fpmath.h"

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
	fprintf(stderr, "%*s%-34s type=%d disp=%d fs=%d x=%d y=%d w=%d h=%d\n",
			depth * 2, "", nm, (int)b->type,
			(b->style != NULL) ?
				(int)css_computed_display_static(b->style) : -1,
			fszq, x, y,
			(b->width  >= 1000000 || b->width  < 0) ? -1 : b->width,
			(b->height >= 1000000 || b->height < 0) ? -1 : b->height);
	for (b = b->children; b != NULL; b = b->next)
		harness_dump_boxes(b, depth + 1, maxdepth);
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
				fprintf(stderr, "WARNING: real default.css not "
						"found -- layout numbers are "
						"NOT trustworthy\n");
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
					(const uint8_t *)layout_css,
					strlen(layout_css));
			css_error de = css_stylesheet_data_done(sheet);
			fprintf(stderr, "author sheet: append=%d done=%d\n",
					(int)ae, (int)de);
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
	 * This does NOT yet require full jQuery init: the element wrapper's
	 * traversal API is still hardcoded (cloneNode returns the element
	 * itself, childNodes=[], firstChild/lastChild=null), so jQuery
	 * legitimately throws later at
	 * le.checkClone=xe.cloneNode(!0).cloneNode(!0).lastChild.checked.  What
	 * must never come back is the document-identity failure.  The test
	 * tightens itself automatically once real traversal lands. --- */
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
			fprintf(stderr, "SKIP: jquery-3.7.1.min.js not present "
					"(fetch it to run the real-jQuery leg)\n");
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
				"if(/createElement/.test(globalThis.__jqErr))"
					"throw new Error('fixes855 REGRESSED: '+globalThis.__jqErr);";
			const char *report_js =
				"globalThis.__jqOK?('OK jQuery '+jQuery.fn.jquery)"
					":('GAP '+(globalThis.__jqErr||'(no error recorded)'))";

			fseek(jf, 0, SEEK_END); jlen = ftell(jf); fseek(jf, 0, SEEK_SET);
			jsrc = (char *)malloc((size_t)jlen + 1);
			rd = fread(jsrc, 1, (size_t)jlen, jf);
			jsrc[rd] = '\0';
			fclose(jf);

			wn = strlen(pre) + rd + strlen(post) + 1;
			wrapped = (char *)malloc(wn);
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

			/* Hard-fails ONLY if the fixes855 document-identity error is back. */
			ok = js_exec(thread, (const unsigned char *)verdict_js,
					strlen(verdict_js), "driver-jq-verdict.js");
			if (!ok) {
				fprintf(stderr, "FAIL: fixes855 REGRESSED -- jQuery is back "
						"to failing on the document handle\n");
				return 1;
			}
			(void)js_exec(thread, (const unsigned char *)report_js,
					strlen(report_js), "driver-jq-report.js");
		}
	}
	fprintf(stderr, "=== Test 7 PASS: jQuery clears the document-identity probe "
			"(fixes855); any remaining throw is the known traversal gap "
			"(cloneNode/firstChild/lastChild/childNodes) ===\n");

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
				"if(n.tag!=='div')"
					"throw new Error('ASSERT FAIL: tagName is '+n.tag+', expected div');"
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
	 * appendChild grows a NEW subtree that itself carries markers (<li>), floats
	 * and <img> objects; innerHTML= parses a fragment into it; removeChild drops
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
			"var p0=document.getElementById('p0');if(p0){feed.removeChild(p0);}"
			"var p1=document.getElementById('p1');if(p1){feed.removeChild(p1);}"
			"var p2=document.getElementById('p2');"
			"if(p2){p2.setAttribute('data-xf-init','1');"
			"p2.textContent='churned';}";
		unsigned char ok29;
		int rc29;
		dom_nodelist *all29 = NULL;
		dom_string *star29 = NULL;
		uint32_t len29 = 0, k29;
		unsigned long touched29 = 0, live29 = 0;
		dom_node *root29 = NULL;

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
			"(markers/floats/img/innerHTML/removeChild) is ASan-clean ===\n");

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
				fprintf(stderr,
					"=== Test 38 FAILED AS EXPECTED (Phase 0 control) ===\n"
					"    The positive control passed (part1_ok=%d), so the "
					"harness CAN dispatch through the real libdom path.\n"
					"    What is red is the diagnosis under test: document "
					"delegation and event.target.\n"
					"    This turns green when Phase 1b + 1c land. Until "
					"then a red suite here is the CORRECT result.\n",
					part1_ok);
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

	return 0;
}
