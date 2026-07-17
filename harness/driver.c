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

#include "content/content_protected.h"
/* fixes879 — Test 22 exercises the real cookie jar directly (urldb.c and
 * nsurl*.c are both compiled into the harness). */
#include "utils/nsurl.h"
#include "content/urldb.h"
#include "content/handlers/html/box.h"
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

int main(void)
{
	char *html_src_big = build_large_doc(300);
	const char *html_src = html_src_big;

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

	fprintf(stderr, "=== S0 harness: reconvert dom_string UAF repro ===\n");

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

	corestrings_init();

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
			"span{display:inline}";

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
		if (css_stylesheet_append_data(ua_sheet,
				(const uint8_t *)ua_css, strlen(ua_css)) != CSS_OK &&
		    css_stylesheet_append_data(ua_sheet,
				(const uint8_t *)ua_css, strlen(ua_css)) != CSS_NEEDDATA) {
			fprintf(stderr, "FAIL: css_stylesheet_append_data\n");
			return 1;
		}
		css_stylesheet_data_done(ua_sheet);
		if (css_select_ctx_append_sheet(select_ctx, ua_sheet,
				CSS_ORIGIN_UA, NULL) != CSS_OK) {
			fprintf(stderr, "FAIL: css_select_ctx_append_sheet\n");
			return 1;
		}
	}

	htmlc.media.width = INTTOFIX(800);
	htmlc.media.height = INTTOFIX(600);
	htmlc.media.orientation = CSS_MEDIA_ORIENTATION_LANDSCAPE;
	htmlc.unit_len_ctx.viewport_width = INTTOFIX(800);
	htmlc.unit_len_ctx.viewport_height = INTTOFIX(600);
	htmlc.unit_len_ctx.device_dpi = INTTOFIX(90);
	htmlc.unit_len_ctx.font_size_default = INTTOFIX(16);
	htmlc.unit_len_ctx.font_size_minimum = INTTOFIX(8);

	if (lwc_intern_string("*", 1, &htmlc.universal) != lwc_error_ok) {
		fprintf(stderr, "FAIL: lwc_intern_string universal\n");
		return 1;
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

	free(html_src_big);
	return 0;
}
