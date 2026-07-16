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
	params.enable_script = false;
	params.script = NULL;
	params.msg = NULL;
	params.ctx = NULL;
	params.daf = NULL;

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

		/* Navigate heap 1. js_newthread() calls qjs_flush_timers(heap->ctx).
		 * Pre-fix this freed heap2's callback against heap1's runtime and
		 * blew up inside js_shape_hash_unlink. */
		{
			struct jsthread *thread_nav = NULL;
			nerr = js_newthread(heap, NULL, (void *)&htmlc, &thread_nav);
			if (nerr != NSERROR_OK) {
				fprintf(stderr, "FAIL: js_newthread(nav) nerr=%d\n",
						(int)nerr);
				return 1;
			}
			fprintf(stderr, "heap1 navigation flush survived\n");
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

	/* --- Test 7 (fixes855, #284): the REAL jQuery must initialise.
	 * hackaday's first script is the _static ??-bundle of jquery.min.js +
	 * jquery-migrate.min.js, and on hardware it dies with "TypeError: cannot
	 * read property 'createElement' of undefined", after which every
	 * dependent bundle reports "ReferenceError: jQuery is not defined".
	 * jQuery 3.7.1's setDocument only captures the document when
	 * `9===n.nodeType && n.documentElement` -- and document.nodeType was
	 * never set, so its internal handle T stayed undefined and the first
	 * T.createElement() support probe threw.  This runs the byte-exact file
	 * hackaday serves through the real js_exec path. --- */
	fprintf(stderr, "\n=== Test 7: real jQuery 3.7.1 initialises ===\n");
	{
		FILE *jf = fopen("jquery-3.7.1.min.js", "rb");
		char *jsrc; long jlen; size_t rd;
		unsigned char ok;
		const char *probe_js =
			"if(document.nodeType!==9)"
				"throw new Error('ASSERT FAIL: document.nodeType='+document.nodeType);"
			"if(!document.documentElement)"
				"throw new Error('ASSERT FAIL: no documentElement');";
		const char *check_js =
			"if(typeof jQuery==='undefined')"
				"throw new Error('ASSERT FAIL: jQuery is not defined');"
			"if(typeof jQuery.fn==='undefined'||!jQuery.fn.jquery)"
				"throw new Error('ASSERT FAIL: jQuery.fn missing');"
			"globalThis.__jqver=jQuery.fn.jquery;";
		if (jf == NULL) {
			fprintf(stderr, "SKIP: jquery-3.7.1.min.js not present\n");
		} else {
			fseek(jf, 0, SEEK_END); jlen = ftell(jf); fseek(jf, 0, SEEK_SET);
			jsrc = (char *)malloc((size_t)jlen + 1);
			rd = fread(jsrc, 1, (size_t)jlen, jf);
			jsrc[rd] = '\0';
			fclose(jf);

			ok = js_exec(thread, (const unsigned char *)probe_js,
					strlen(probe_js), "driver-doc-probe.js");
			if (!ok) {
				fprintf(stderr, "FAIL: document node identity wrong "
						"(nodeType must be 9)\n");
				return 1;
			}
			fprintf(stderr, "document.nodeType==9 and documentElement present\n");

			ok = js_exec(thread, (const unsigned char *)jsrc, rd,
					"jquery-3.7.1.min.js");
			fprintf(stderr, "js_exec(real jQuery %ld bytes) ok=%d\n",
					jlen, (int)ok);
			if (!ok) {
				fprintf(stderr, "FAIL: real jQuery threw during init\n");
				free(jsrc);
				return 1;
			}
			ok = js_exec(thread, (const unsigned char *)check_js,
					strlen(check_js), "driver-jq-check.js");
			if (!ok) {
				fprintf(stderr, "FAIL: jQuery did not export itself\n");
				free(jsrc);
				return 1;
			}
			free(jsrc);
		}
	}
	fprintf(stderr, "=== Test 7 PASS: real jQuery 3.7.1 initialises and "
			"exports jQuery.fn ===\n");

	free(html_src_big);
	return 0;
}
