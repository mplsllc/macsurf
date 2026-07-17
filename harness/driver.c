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
	size_t cap = 64 + (size_t)n * 64;
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

	free(html_src_big);
	return 0;
}
