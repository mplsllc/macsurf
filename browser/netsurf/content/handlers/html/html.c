/*
 * Copyright 2007 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2010 Michael Drake <tlsa@netsurf-browser.org>
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
 * Implementation of HTML content handling.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <nsutils/time.h>

#include "utils/utils.h"
#include "utils/config.h"
#include "utils/corestrings.h"
#include "utils/http.h"
#include "utils/libdom.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/talloc.h"
#include "utils/utf8.h"
#include "utils/nsoption.h"
#include "utils/string.h"
#include "utils/ascii.h"
#include "netsurf/content.h"
#include "netsurf/browser_window.h"
#include "netsurf/css.h"
#include "netsurf/utf8.h"
#include "netsurf/keypress.h"
#include "netsurf/layout.h"
#include "netsurf/misc.h"
#include "content/hlcache.h"
#include "content/content_factory.h"
#include "content/textsearch.h"
#include "desktop/selection.h"
#include "desktop/scrollbar.h"
#include "desktop/textarea.h"
#include "netsurf/bitmap.h"
#include "javascript/js.h"
#include "desktop/gui_internal.h"

#include "html/html.h"
#include "macsurf_debug.h"

/* fixes518: frontend scheduler cancellation (forward-declared here, Mac-only
 * fork, same approach as macsurf_debug_log_writef in content_protected.h).
 * Used in html_close/html_destroy to cancel every scheduled callback keyed
 * on this html_content (deferred_parser_unpause, html_css_process_modified_
 * styles, ...) before any parser/content state is torn down. */
#ifdef __MACOS9__
extern void macos9_schedule_cancel_owner(void *p);
#else
#define macos9_schedule_cancel_owner(p) ((void)0)
#endif

long macos9_html_bytes_processed = 0;
/* fixes560 - per-load reformat sequence counter.  Reset to 0 at
 * parse-convert-done (start of a page's reformat cycle) and incremented at
 * each html_reformat entry, so the timestamped log shows the reflow storm
 * explicitly: "reformat #1 ... #15" with each one's begin-stamp lets the
 * per-reformat cost (ms_after-ms_before, already in the SITE line) be read
 * against the count.  See project_mactrove_reflow_storm. */
static long macos9_html_reformat_seq = 0;

/* fixes848 (#167 perf investigation) - wall-clock span of box construction
 * + per-element CSS cascade (dom_to_box is an incremental, self-rescheduling
 * walk, so its own synchronous return does NOT mean it finished -- the true
 * completion signal is the html_box_convert_done callback). Set at the
 * dom_to_box() call site, read+logged in html_box_convert_done. File-scope
 * static because the two sites are different functions; imprecise only if
 * two conversions are somehow interleaved, which is not the normal case
 * this is diagnosing. */
static double s_convert_start_us = 0.0;
char macos9_html_head[64];
unsigned int macos9_html_head_len = 0;

#include <libcss/font_face.h>
#include "html/private.h"
#include "html/dom_event.h"
#include "html/css.h"

/* Count parsed Facebook data-sjs nodes directly from libdom at the instant
 * hubbub reports completion. This is deliberately independent of QuickJS's
 * querySelectorAll audit: paired with the fetcher's raw token count it tells
 * us whether the two platforms received different HTML or lost nodes while
 * parsing the same bytes. */
static void html_log_facebook_parse_fingerprint(html_content *htmlc)
{
	const char *url;
	dom_string *script_name = NULL;
	dom_string *data_sjs_name = NULL;
	dom_nodelist *scripts = NULL;
	dom_exception exc;
	uint32_t length = 0;
	uint32_t i;
	long parsed = 0;

	url = nsurl_access(content_get_url((struct content *)htmlc));
	if (url == NULL || strstr(url, "facebook.com") == NULL) return;
	exc = dom_string_create((const uint8_t *)"script", 6, &script_name);
	if (exc != DOM_NO_ERR || script_name == NULL) goto done;
	exc = dom_string_create((const uint8_t *)"data-sjs", 8,
		&data_sjs_name);
	if (exc != DOM_NO_ERR || data_sjs_name == NULL) goto done;
	exc = dom_document_get_elements_by_tag_name(htmlc->document,
		script_name, &scripts);
	if (exc != DOM_NO_ERR || scripts == NULL) goto done;
	exc = dom_nodelist_get_length(scripts, &length);
	if (exc != DOM_NO_ERR) goto done;
	for (i = 0; i < length; i++) {
		dom_node *node = NULL;
		dom_string *value = NULL;
		exc = dom_nodelist_item(scripts, i, &node);
		if (exc != DOM_NO_ERR || node == NULL) continue;
		exc = dom_element_get_attribute((dom_element *)node,
			data_sjs_name, &value);
		if (exc == DOM_NO_ERR && value != NULL) {
			parsed++;
			dom_string_unref(value);
		}
		dom_node_unref(node);
	}
	macsurf_debug_log_writef("LIFE FBDOCPARSE parsed_sjs=%ld scripts=%ld",
		parsed, (long)length);
	macsurf_debug_log_writef("LIFE FBDOCPARSE url=%s", url);

done:
	if (scripts != NULL) dom_nodelist_unref(scripts);
	if (data_sjs_name != NULL) dom_string_unref(data_sjs_name);
	if (script_name != NULL) dom_string_unref(script_name);
}
#include "html/object.h"
#include "html/html_save.h"
#include "html/interaction.h"
#include "html/box.h"
#include "html/box_construct.h"
#include "html/box_inspect.h"
#include "html/form_internal.h"
#include "html/imagemap.h"
#include "html/layout.h"
#include "html/textselection.h"

#define CHUNK 4096

/* Change these to 1 to cause a dump to stderr of the frameset or box
 * when the trees have been built.
 */
#define ALWAYS_DUMP_FRAMESET 0
#define ALWAYS_DUMP_BOX 0

static const char *html_types[] = {
	"application/xhtml+xml",
	"text/html"
};

/* fixes1015 - defined below, next to html_proceed_to_done; called from
 * html_box_convert_done (ready), html_proceed_to_done (done) and
 * html_reconvert_done (reconvert). */
void html_pagemap_dump(html_content *c, const char *when);
/* fixes1093 - the targeted `.featured-slides` subtree probe. Same three call
 * sites as the pagemap. */
void html_slider_probe(html_content *c, const char *when);

/**
 * Fire an event at the DOM
 *
 * Helper that swallows DOM errors.
 *
 * \param[in] event   the event to fire at the DOM
 * \param[in] target  the event target
 * \return true on success
 */
static bool fire_dom_event(dom_event *event, dom_node *target)
{
	dom_exception exc;
	bool result;

	exc = dom_event_target_dispatch_event(target, event, &result);
	if (exc != DOM_NO_ERR) {
		return false;
	}

	return result;
}

/* fixes990 - the layout extent as it stood BEFORE the 1,000,000 clamp, so the
 * split-scrollbar diagnostic can see the value the clamp destroys. */
static int g_macsurf_pre_clamp_dx1 = 0;

/* Exported interface, see html_internal.h */
bool fire_generic_dom_event(dom_string *type, dom_node *target,
		bool bubbles, bool cancelable)
{
	dom_exception exc;
	dom_event *evt;
	bool result;

	exc = dom_event_create(&evt);
	if (exc != DOM_NO_ERR) return false;
	exc = dom_event_init(evt, type, bubbles, cancelable);
	if (exc != DOM_NO_ERR) {
		dom_event_unref(evt);
		return false;
	}
	NSLOG(netsurf, INFO, "Dispatching '%*s' against %p",
	      (int)dom_string_length(type), dom_string_data(type), target);
	result = fire_dom_event(evt, target);
	dom_event_unref(evt);
	return result;
}

/* Exported interface, see html_internal.h */
bool fire_dom_keyboard_event(dom_string *type, dom_node *target,
		bool bubbles, bool cancelable, uint32_t key)
{
	bool is_special = key <= 0x001F || (0x007F <= key && key <= 0x009F);
	dom_string *dom_key = NULL;
	dom_keyboard_event *evt;
	dom_exception exc;
	bool result;

	if (is_special) {
		switch (key) {
		case NS_KEY_ESCAPE:
			dom_key = dom_string_ref(corestring_dom_Escape);
			break;
		case NS_KEY_LEFT:
			dom_key = dom_string_ref(corestring_dom_ArrowLeft);
			break;
		case NS_KEY_RIGHT:
			dom_key = dom_string_ref(corestring_dom_ArrowRight);
			break;
		case NS_KEY_UP:
			dom_key = dom_string_ref(corestring_dom_ArrowUp);
			break;
		case NS_KEY_DOWN:
			dom_key = dom_string_ref(corestring_dom_ArrowDown);
			break;
		case NS_KEY_PAGE_UP:
			dom_key = dom_string_ref(corestring_dom_PageUp);
			break;
		case NS_KEY_PAGE_DOWN:
			dom_key = dom_string_ref(corestring_dom_PageDown);
			break;
		case NS_KEY_TEXT_START:
			dom_key = dom_string_ref(corestring_dom_Home);
			break;
		case NS_KEY_TEXT_END:
			dom_key = dom_string_ref(corestring_dom_End);
			break;
		default:
			dom_key = NULL;
			break;
		}
	} else {
		char utf8[6];
		size_t length = utf8_from_ucs4(key, utf8);
		utf8[length] = '\0';

		exc = dom_string_create((const uint8_t *)utf8, strlen(utf8),
				&dom_key);
		if (exc != DOM_NO_ERR) {
			return exc;
		}
	}

	exc = dom_keyboard_event_create(&evt);
	if (exc != DOM_NO_ERR) {
		dom_string_unref(dom_key);
		return false;
	}

	exc = dom_keyboard_event_init(evt, type, bubbles, cancelable, NULL,
			dom_key, NULL, DOM_KEY_LOCATION_STANDARD, false,
			false, false, false, false, false);
	dom_string_unref(dom_key);
	if (exc != DOM_NO_ERR) {
		dom_event_unref(evt);
		return false;
	}

	NSLOG(netsurf, INFO, "Dispatching '%*s' against %p",
			(int)dom_string_length(type), dom_string_data(type), target);

	result = fire_dom_event((dom_event *) evt, target);
	dom_event_unref(evt);
	return result;
}

/* fixes850 (#167 perf investigation) - cheap iterative box-tree counter
 * for the WORK log below. Iterative (explicit stack via the sibling/child
 * pointers already on struct box), not recursive: this must not add its
 * own stack-depth risk to a diagnostic for a stack-depth-adjacent
 * investigation. A NULL root counts as zero. */
static long macsurf_count_boxes(struct box *root)
{
	struct box *b = root;
	long n = 0;

	if (b == NULL) return 0;

	while (b != NULL) {
		n++;
		if (b->children != NULL) {
			b = b->children;
			continue;
		}
		while (b != root && b->next == NULL) {
			b = b->parent;
		}
		if (b == root) break;
		b = b->next;
	}
	return n;
}

/**
 * Perform post-box-creation conversion of a document
 *
 * \param c        HTML content to complete conversion of
 * \param success  Whether box tree construction was successful
 */
static void html_box_convert_done(html_content *c, bool success)
{
	nserror err;
	dom_exception exc; /* returned by libdom functions */
	dom_node *html;

	NSLOG(netsurf, INFO, "DOM to box conversion complete (content %p)", c);
	macsurf_debug_log_writef("fc: box_convert_done entered (success=%d)", (int)success);
	/* fixes848 (#167 perf investigation) - WORK-prefixed pipeline
	 * checkpoint. A hardware log against a heavy Facebook page showed
	 * NAV: content_ready fire and then nothing at all until the app
	 * appeared to hang -- every intermediate line in this pipeline
	 * (this one included) was plain macsurf_debug_log_writef, invisible
	 * on the release build. This marks "box construction + per-element
	 * CSS cascade are done" (dom_to_box is itself an incremental,
	 * self-rescheduling walk -- this callback is the TRUE completion
	 * signal, not dom_to_box()'s own synchronous return). */
	{
		extern double macos9_micros(void);
		double elapsed_us = (s_convert_start_us > 0.0) ?
				(macos9_micros() - s_convert_start_us) : -1.0;
		/* fixes850 (#167 perf investigation) - the fixes849 hardware log
		 * showed layout's call count climbing steadily into the millions
		 * with bounded recursion depth (21-30) -- real work, not a hang,
		 * but that alone doesn't say whether 1.5M+ calls is proportionate
		 * to this page's actual size or a sign something is being
		 * redone. Logging the real box count here (once, cheap, box
		 * tree is already fully built at this point) gives the
		 * calls-per-box ratio the next log needs to tell those apart. */
		long box_count = macsurf_count_boxes(c->layout);
		macsurf_debug_log_writef(
				"WORK pipeline: box+cascade DONE us=%ld boxes=%ld c=%p url=%s",
				(long) elapsed_us, box_count, (void *) c,
				nsurl_access(content_get_url((struct content *) c)));
		s_convert_start_us = 0.0;
	}
	macsurf_profile_stamp("parse-convert-done");
	macos9_html_reformat_seq = 0; /* fixes560: fresh reflow-storm count per load */

	c->box_conversion_context = NULL;

	/* Clean up and report error if unsuccessful or aborted */
	if ((success == false) || (c->aborted)) {
		html_object_free_objects(c);

		if (success == false) {
			content_broadcast_error(&c->base, NSERROR_BOX_CONVERT, NULL);
		} else {
			content_broadcast_error(&c->base, NSERROR_STOPPED, NULL);
		}

		content_set_error(&c->base);
		return;
	}


#if ALWAYS_DUMP_BOX
	box_dump(stderr, c->layout->children, 0, true);
#endif
#if ALWAYS_DUMP_FRAMESET
	if (c->frameset)
		html_dump_frameset(c->frameset, 0);
#endif

	/* fixes311 -- box tree counters at convert-success boundary.
	 * Walks the layout via children/next/parent without recursion so
	 * arbitrarily deep trees do not blow the stack on real hardware. */
	{
		long total = 0, blk = 0, ic = 0, in_ = 0, txt = 0, oth = 0;
		struct box *b = c->layout;
		struct box *root_parent = b ? b->parent : NULL;
		while (b != NULL) {
			total++;
			switch (b->type) {
			case BOX_BLOCK: blk++; break;
			case BOX_INLINE_CONTAINER: ic++; break;
			case BOX_INLINE: in_++; break;
			case BOX_TEXT: txt++; break;
			default: oth++; break;
			}
			if (b->children != NULL) {
				b = b->children;
				continue;
			}
			while (b != NULL && b->next == NULL) {
				if (b->parent == root_parent) { b = NULL; break; }
				b = b->parent;
			}
			if (b != NULL) b = b->next;
		}
		macsurf_debug_log_writef(
			"box convert: layout=%p total=%ld blk=%ld ic=%ld in=%ld txt=%ld oth=%ld",
			(void *)c->layout, total, blk, ic, in_, txt, oth);
		/* fixes160a: stash box-convert counters so the SITE summary
		 * line at reformat-end can include them. Globals (not per-
		 * content) because we only need the most-recent page; the
		 * SITE line is emitted before any concurrent page could
		 * arrive. */
		{
			extern long macsurf__site_box_total;
			extern long macsurf__site_box_blk;
			extern long macsurf__site_box_inlinec;
			extern long macsurf__site_box_inline;
			extern long macsurf__site_box_text;
			extern long macsurf__site_box_other;
			macsurf__site_box_total   = total;
			macsurf__site_box_blk     = blk;
			macsurf__site_box_inlinec = ic;
			macsurf__site_box_inline  = in_;
			macsurf__site_box_text    = txt;
			macsurf__site_box_other   = oth;
		}
	}

	exc = dom_document_get_document_element(c->document, (void *) &html);
	if ((exc != DOM_NO_ERR) || (html == NULL)) {
		/** @todo should this call html_object_free_objects(c);
		 * like the other error paths
		 */
		NSLOG(netsurf, INFO, "error retrieving html element from dom");
		content_broadcast_error(&c->base, NSERROR_DOM, NULL);
		content_set_error(&c->base);
		return;
	}

	/* extract image maps - can't do this sensibly in dom_to_box */
	err = imagemap_extract(c);
	if (err != NSERROR_OK) {
		NSLOG(netsurf, INFO, "imagemap extraction failed");
		html_object_free_objects(c);
		content_broadcast_error(&c->base, err, NULL);
		content_set_error(&c->base);
		dom_node_unref(html);
		return;
	}
	/*imagemap_dump(c);*/

	/* Destroy the parser binding */
	dom_hubbub_parser_destroy(c->parser);
	c->parser = NULL;

	/* GATE 3: the initial box tree now exists.  Fire DOMContentLoaded then
	 * load into the JS document's registered listeners so XenForo's
	 * preamble XF.ready() queue drains and XF.activate(document) runs.
	 * Guarded by js_thread (JS-less pages skip it) and fires once per
	 * navigation (JS-side __ms_ready_fired; the JS realm is rebuilt per
	 * document load so the flag resets automatically). */
	if (c->js_thread != NULL) {
		js_fire_dom_ready(c->js_thread, c->document);
	}

	content_set_ready(&c->base);
	html_pagemap_dump(c, "ready"); /* fixes1015 */
	html_slider_probe(c, "ready"); /* fixes1093 */

	html_proceed_to_done(c);

	dom_node_unref(html);
}

/* ====================================================================== */
/* fixes1015 - PAGEMAP: the DOM <-> box-tree <-> render audit.
 *
 * "Entire sections are missing" has four different causes with four different
 * fixes, and aggregate logs cannot tell them apart:
 *   (a) the section was REMOVED from the DOM by script;
 *   (b) it is in the DOM but built NO BOX (display:none, or conversion drop);
 *   (c) it has a box with HEIGHT 0 (layout failure / collapsed);
 *   (d) it renders but its own content is empty.
 * This walks <body>'s element children after each settle point (ready, done,
 * every reconvert) and prints one line per section: identity, DOM child
 * count, box presence, position/size, computed display. Reading two dumps
 * side by side answers which class a vanished section fell into.
 * All LIFE-prefixed; budgeted per session. */

/* fixes1028 - THE RIVER PROBE, on for this round only.
 *
 * Chrome, at MacSurf's own 993px viewport, gives hackaday's article river
 * ASIDE#recent-posts-2 h=1893 with each LI 206-236 tall. MacSurf's OWN
 * layout, run over the same markup and the same 79 KB stylesheet on Linux,
 * gives MAIN h=1736 -- correct within the harness's synthetic font metrics.
 * The device gives 562. So the difference is something only the device has:
 * real font metrics, the CSS preprocessor, or the media queries at that
 * viewport. This prints the computed FONT SIZE beside every box so those
 * are distinguishable: text boxes present with a tiny font is a font-size
 * bug, text boxes absent is a construction bug, and correct font with short
 * boxes is neither. */
/* fixes1032 - OFF. It did its job: it is what showed entry-intro kids=0 and
 * turned a layout hunt into a DOM-deletion hunt. Define MACSURF_PAGEMAP (or
 * MACSURF_JS_AUDIT) to bring it back for the next structural question. */
/* fixes1086 - BACK ON, for exactly that: the next structural question.
 *
 * hackaday's hero/featured story does not render. It survived reverting the
 * whole prototype migration AND turning geometry off, and the page is back to
 * 22s, so it predates every JS change made today and is not a symptom of any
 * of them. I have already guessed twice at this and been wrong twice.
 *
 * CLAUDE.md's diagnostic order exists for this and starts in one place: is the
 * content even in the DOM? PAGEMAP prints per-section
 *     tag#id.class kids box y w h fs disp
 * so the answer is one line of log rather than another theory. kids=0 on the
 * hero container means script deleted it and layout is innocent -- which is
 * precisely what this channel established last time it was on. A real box with
 * height 0 means the opposite. Those two want completely different fixes and
 * nothing else distinguishes them.
 *
 * Capped at 40 dumps, one per section, so it is a handful of lines rather than
 * a firehose. Turn it off again once the hero is understood. */
#define MACSURF_PAGEMAP 1
#define MACSURF_PAGEMAP_MAX_DUMPS 40

static long macsurf_pagemap_dumps = 0;

static void html_pagemap_append(char *out, int cap, int *pos,
		const char *s, int maxn, char sep, int dot_spaces)
{
	int i = 0;
	if (s == NULL || s[0] == '\0') return;
	if (sep != '\0' && *pos < cap - 1) out[(*pos)++] = sep;
	while (s[i] != '\0' && i < maxn && *pos < cap - 1) {
		char ch = s[i];
		if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
		if (dot_spaces && ch == ' ') ch = '.';
		out[(*pos)++] = ch;
		i++;
	}
	out[*pos] = '\0';
}

/* "TAG#id.class" (truncated). Element nodes only; caller checks the type.
 * fixes1315 (#167, 68kmla) - exported (was static) so layout_flex.c's own
 * diagnostics can report a box's real identity instead of a bare pointer,
 * the same lookup fixes1305/1307 already proved for the pagemap dump. */
void html_pagemap_brief(dom_node *n, char *out, int cap)
{
	static dom_string *s_id = NULL;
	static dom_string *s_class = NULL;
	dom_string *ds = NULL;
	dom_exception exc;
	int pos = 0;

	out[0] = '\0';
	if (s_id == NULL)
		(void) dom_string_create((const uint8_t *)"id", 2, &s_id);
	if (s_class == NULL)
		(void) dom_string_create((const uint8_t *)"class", 5, &s_class);

	exc = dom_node_get_node_name(n, &ds);
	if (exc == DOM_NO_ERR && ds != NULL) {
		html_pagemap_append(out, cap, &pos, dom_string_data(ds), 12,
				'\0', 0);
		dom_string_unref(ds);
	}
	if (s_id != NULL) {
		ds = NULL;
		exc = dom_element_get_attribute((dom_element *)n, s_id, &ds);
		if (exc == DOM_NO_ERR && ds != NULL) {
			html_pagemap_append(out, cap, &pos, dom_string_data(ds),
					24, '#', 0);
			dom_string_unref(ds);
		}
	}
	if (s_class != NULL) {
		ds = NULL;
		exc = dom_element_get_attribute((dom_element *)n, s_class, &ds);
		if (exc == DOM_NO_ERR && ds != NULL) {
			/* fixes1307 (#167, C0) - was 36: real Facebook Comet
			 * elements commonly carry 15-25+ atomic classes
			 * averaging ~7-8 chars each, so 36 only ever showed
			 * the first 4-5 -- every pagemap/FBGAP line for a
			 * real Facebook box has been hiding the rest of that
			 * box's classes, which is exactly the data needed to
			 * check whether some OTHER class (not the one named
			 * in a truncated log line) is the real min-height/
			 * layout culprit. Widened to use the caller's full
			 * remaining buffer instead of a small fixed cap. */
			html_pagemap_append(out, cap, &pos, dom_string_data(ds),
					cap, '.', 1);
			dom_string_unref(ds);
		}
	}
}

#define HTML_PAGEMAP_SANE(v) (((v) < 0 || (v) >= 1000000) ? 0 : (v))

/* fixes1017 - per-dump line budget: the walk is recursive now (three levels,
 * always descending containers), so a hard cap bounds the worst page. */
static int macsurf_pagemap_line_budget = 0;

/* One line for one element; returns its element-child count so the walk can
 * decide whether to descend. fixes1017: the fixes1016 dump only descended
 * into sections that LOOKED broken, but the hackaday miss is a subtree whose
 * top-level numbers look plausible -- the interesting depth is #content /
 * #primary / main / widget level, so descend unconditionally instead. */
static int html_pagemap_line(dom_node *n, int depth, int *susp_out)
{
	char brief[88];
	struct box *b;
	dom_node *ch = NULL;
	dom_node *nx = NULL;
	int kids = 0;
	int x = 0, y = 0, w = 0, h = 0;
	int fsz = -1;
	const char *disp = "-";
	static const char *pfx[5] = { "", "> ", ">> ", ">>> ", ">>>> " };

	html_pagemap_brief(n, brief, (int)sizeof brief);

	if (dom_node_get_first_child(n, &ch) != DOM_NO_ERR) ch = NULL;
	while (ch != NULL) {
		dom_node_type t2 = (dom_node_type)0;
		if (dom_node_get_node_type(ch, &t2) == DOM_NO_ERR &&
				t2 == DOM_ELEMENT_NODE)
			kids++;
		nx = NULL;
		if (dom_node_get_next_sibling(ch, &nx) != DOM_NO_ERR) nx = NULL;
		dom_node_unref(ch);
		ch = nx;
	}

	b = box_for_node(n);
	if (b != NULL) {
		box_coords(b, &x, &y);
		w = HTML_PAGEMAP_SANE(b->width);
		h = HTML_PAGEMAP_SANE(b->height);
		if (b->style != NULL) {
			css_fixed fs = 0;
			css_unit fu = CSS_UNIT_PX;
			disp = (css_computed_display_static(b->style) ==
					CSS_DISPLAY_NONE) ? "NONE" : "ok";
			/* fixes1028 - the computed font size, in whatever unit
			 * the cascade settled on. A correct page here reads
			 * ~17px on body text and ~30px on the river's h2. */
			if (css_computed_font_size(b->style, &fs, &fu) ==
					CSS_FONT_SIZE_DIMENSION)
				fsz = (int)FIXTOINT(fs);
		}
	}
	macsurf_pagemap_line_budget--;
	macsurf_debug_log_writef(
			"LIFE pagemap %s%s kids=%d box=%d y=%d w=%d h=%d fs=%d disp=%s",
			pfx[(depth < 0) ? 0 : ((depth > 4) ? 4 : depth)],
			brief, kids, (b != NULL) ? 1 : 0, y, w, h, fsz, disp);
	/* fixes1021 -- "this subtree looks broken": boxless, flat, or a
	 * container squashed to less than a text line. The walk uses it to
	 * keep descending PAST the normal depth cap, straight into e.g. the
	 * 22px-tall slider, where the ordinary dump kept stopping one level
	 * above the answer.
	 * fixes1305 (#167, C0) -- the SAME "broken" question can point the
	 * other way: a container implausibly TALL for its child count
	 * (matches the FLEXLINE/FLEXMINH >3000px threshold already used for
	 * the xpvvgw5 investigation) is exactly as suspicious as one
	 * squashed to nothing, and the original check had no way to flag
	 * it -- the walk would stop at the normal depth cap one level above
	 * a box like that too. */
	if (susp_out != NULL)
		*susp_out = (b == NULL || h == 0 || (h < 30 && kids > 0) ||
				(h > 3000 && kids <= 2));
	return kids;
}

/* Recursive section walk: print, then descend into element children while
 * depth and the line budget allow. SCRIPT/STYLE subtrees are skipped -- they
 * can never render and only burn budget. fixes1018: depth 4, which is what
 * reaches main#main's children on a standard WordPress tree (body > #page >
 * #content > #primary > main > widgets) -- the fixes1017 dump stopped at
 * #primary and could not name WHICH widget was short. display:none subtrees
 * print their own line but are not descended (children of a none are
 * definitionally 0x0 and only burn the budget). */
static void html_pagemap_walk(dom_node *n, int depth)
{
	dom_node *ch = NULL;
	dom_node *nx = NULL;
	int kids;
	dom_string *nm = NULL;
	struct box *b;

	if (macsurf_pagemap_line_budget <= 0) return;
	if (dom_node_get_node_name(n, &nm) == DOM_NO_ERR && nm != NULL) {
		/* fixes1305 (#167, C0) - LINK/META never produce a box and
		 * carry no rendered content, but Facebook's Comet injects
		 * dozens of resource-hint LINKs directly into <body> (not
		 * just <head>). Before this, a real hardware pagemap[done]
		 * dump for facebook.com/ptricky3 spent its entire 130-line
		 * budget on 129 back-to-back "LINK kids=0 box=0 h=0" lines
		 * and never printed a single real body section -- the dump
		 * has been silently useless for any Facebook page since it
		 * shipped. Skip both, same as script/style. */
		int skip = (strcasecmp(dom_string_data(nm), "script") == 0 ||
				strcasecmp(dom_string_data(nm), "style") == 0 ||
				strcasecmp(dom_string_data(nm), "link") == 0 ||
				strcasecmp(dom_string_data(nm), "meta") == 0);
		dom_string_unref(nm);
		if (skip) return;
	}
	{
		int susp = 0;
		kids = html_pagemap_line(n, depth, &susp);
		if (kids == 0) return;
		/* fixes1028 - depth 6 unconditionally: the river's own LI and
		 * its h2/p sit at body>#page>#content>#primary>main>aside>ul>li,
		 * and every earlier dump stopped above them. */
		if (depth >= 8) return;
		if (depth >= 6 && !susp) return;
	}
	b = box_for_node(n);
	if (b != NULL && b->style != NULL &&
			css_computed_display_static(b->style) == CSS_DISPLAY_NONE)
		return;
	if (dom_node_get_first_child(n, &ch) != DOM_NO_ERR) ch = NULL;
	while (ch != NULL && macsurf_pagemap_line_budget > 0) {
		dom_node_type t2 = (dom_node_type)0;
		if (dom_node_get_node_type(ch, &t2) == DOM_NO_ERR &&
				t2 == DOM_ELEMENT_NODE) {
			html_pagemap_walk(ch, depth + 1);
		}
		nx = NULL;
		if (dom_node_get_next_sibling(ch, &nx) != DOM_NO_ERR) nx = NULL;
		dom_node_unref(ch);
		ch = nx;
	}
}

/* ====================================================================== */
/* fixes1093 - THE SLIDER PROBE.
 *
 * Three rounds have now guessed at why hackaday's featured slider collapses,
 * and every one of them was argued from code-reading rather than a number off
 * the device. This probe exists to end that.
 *
 * What the theme actually does (harness/hackaday-bundle.js:2672-2684):
 *
 *     var sliderHeight = $($('.featured-slides div')[0]).height();
 *     $('.featured-slides').slick({ ..., onInit: function() {
 *         $('.slick-slide').css('height', sliderHeight);
 *         ...
 *     }});
 *
 * and slick's own setHeight() (line 278) is a NO-OP here, because it is
 * gated on `adaptiveHeight === true` and the default is false (line 99) and
 * the theme does not pass it. So slick NEVER sizes the list itself: the
 * ENTIRE height of this widget comes from that one `sliderHeight` number,
 * measured ONCE before .slick() runs, applied ONCE in a callback that never
 * fires again. Chrome gets 404.489px; we render h=15 then h=1.
 *
 * That means exactly two outcomes are possible, and one line of output tells
 * them apart:
 *
 *   st=...height:404px...   -> the measurement WORKED and layout is dropping
 *                              an inline height. A layout/CSS bug. Nothing to
 *                              do with geometry settling, resize, or `load`.
 *   st=(none) / height:NaN  -> jQuery's .height() answered undefined/NaN at
 *                              measure time (the fixes1014/1016 unsettled
 *                              contract), so .css() no-opped. A geometry-
 *                              timing bug, and NO later resize/load event can
 *                              repair it because onInit never re-runs.
 *
 * Everything else this round could have shipped is a guess until that field
 * is read. The probe prints the whole `.featured-slides` subtree at FULL
 * depth (the pagemap flattens everything past depth 4, which is why the
 * slick-list/track/slide nesting has been invisible in every log so far),
 * with each node's inline style attribute, its box geometry, and its
 * cascaded height. */
#define MACSURF_SLIDER_MAX_DUMPS 6

static int macsurf_slider_line_budget = 0;

/* fixes1093 - every subtree line this probe emits, counted. Harness Test 55
 * asserts on it: a probe that silently walks nothing reads as "the widget is
 * fine" when it means "the probe is broken", which is how the compiled-out
 * fixes1019 resize hid for ten rounds. */
long macsurf_probe_slider_lines = 0;

/* Read an attribute into a caller buffer, whitespace-flattened. */
static void html_slider_attr(dom_node *n, const char *name, char *out, int cap)
{
	dom_string *nm = NULL;
	dom_string *v = NULL;
	const char *p;
	int i = 0;

	out[0] = '\0';
	if (dom_string_create((const uint8_t *)name, (size_t)strlen(name), &nm)
			!= DOM_NO_ERR || nm == NULL)
		return;
	if (dom_element_get_attribute((dom_element *)n, nm, &v) == DOM_NO_ERR &&
			v != NULL) {
		p = dom_string_data(v);
		while (p != NULL && *p != '\0' && i < cap - 1) {
			char ch = *p++;
			if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
			out[i++] = ch;
		}
		dom_string_unref(v);
	}
	out[i] = '\0';
	dom_string_unref(nm);
}

/* fixes1096 - tag-name filter for html_slider_find (case-insensitive).
 * Lets the probe ask for "the section whose class carries 'featured'"
 * instead of whatever element happens to match the class first. */
static int html_slider_is_tag(dom_node *n, const char *tag)
{
	dom_string *nm = NULL;
	int m = 0;

	if (dom_node_get_node_name(n, &nm) == DOM_NO_ERR && nm != NULL) {
		m = (strcasecmp(dom_string_data(nm), tag) == 0);
		dom_string_unref(nm);
	}
	return m;
}

/* Depth-first search for the first element whose class carries `cls` (and,
 * when `tag` is non-NULL, whose tag name is `tag`). Returns a REF'd node, or
 * NULL. */
static dom_node *html_slider_find(dom_node *n, const char *cls,
		const char *tag, int depth)
{
	dom_node *ch = NULL;
	dom_node *nx = NULL;
	dom_node *found = NULL;
	dom_node_type t = (dom_node_type)0;
	char buf[160];

	if (depth > 24) return NULL;

	if (dom_node_get_node_type(n, &t) == DOM_NO_ERR &&
			t == DOM_ELEMENT_NODE) {
		html_slider_attr(n, "class", buf, (int)sizeof buf);
		if (strstr(buf, cls) != NULL &&
				(tag == NULL || html_slider_is_tag(n, tag))) {
			dom_node_ref(n);
			return n;
		}
	}

	if (dom_node_get_first_child(n, &ch) != DOM_NO_ERR) ch = NULL;
	while (ch != NULL && found == NULL) {
		dom_node_type t2 = (dom_node_type)0;
		if (dom_node_get_node_type(ch, &t2) == DOM_NO_ERR &&
				t2 == DOM_ELEMENT_NODE) {
			found = html_slider_find(ch, cls, tag, depth + 1);
		}
		nx = NULL;
		if (dom_node_get_next_sibling(ch, &nx) != DOM_NO_ERR) nx = NULL;
		dom_node_unref(ch);
		ch = nx;
	}
	if (ch != NULL) dom_node_unref(ch);
	return found;
}

static void html_slider_walk(dom_node *n, int depth)
{
	dom_node *ch = NULL;
	dom_node *nx = NULL;
	struct box *b;
	char brief[64];
	char st[96];
	int x = 0, y = 0, w = 0, h = 0;
	const char *disp = "-";
	const char *ht = "none";
	int hv_px = 0;

	if (macsurf_slider_line_budget <= 0) return;
	if (depth > 12) return;

	html_pagemap_brief(n, brief, (int)sizeof brief);
	html_slider_attr(n, "style", st, (int)sizeof st);

	b = box_for_node(n);
	if (b != NULL) {
		box_coords(b, &x, &y);
		w = HTML_PAGEMAP_SANE(b->width);
		h = HTML_PAGEMAP_SANE(b->height);
		if (b->style != NULL) {
			css_fixed hval = 0;
			css_unit hunit = CSS_UNIT_PX;
			disp = (css_computed_display_static(b->style) ==
					CSS_DISPLAY_NONE) ? "NONE" : "ok";
			if (css_computed_height(b->style, &hval, &hunit) ==
					CSS_HEIGHT_SET) {
				ht = (hunit == CSS_UNIT_PX) ? "set" : "setU";
				hv_px = (int) FIXTOINT(hval);
			} else {
				ht = "auto";
			}
		}
	}

	macsurf_slider_line_budget--;
	macsurf_probe_slider_lines++;
	macsurf_debug_log_writef(
		"LIFE SLIDER d=%d %s box=%d y=%d w=%d h=%d disp=%s cssh=%s(%d) st=[%s]",
		depth, brief, (b != NULL) ? 1 : 0, y, w, h, disp, ht, hv_px,
		(st[0] == '\0') ? "(none)" : st);

	if (dom_node_get_first_child(n, &ch) != DOM_NO_ERR) ch = NULL;
	while (ch != NULL && macsurf_slider_line_budget > 0) {
		dom_node_type t2 = (dom_node_type)0;
		if (dom_node_get_node_type(ch, &t2) == DOM_NO_ERR &&
				t2 == DOM_ELEMENT_NODE) {
			html_slider_walk(ch, depth + 1);
		}
		nx = NULL;
		if (dom_node_get_next_sibling(ch, &nx) != DOM_NO_ERR) nx = NULL;
		dom_node_unref(ch);
		ch = nx;
	}
	if (ch != NULL) dom_node_unref(ch);
}

void html_slider_probe(html_content *c, const char *when)
{
	static int nav_dumps = 0;
	dom_element *root = NULL;
	dom_node *slider = NULL;

	if (c == NULL || c->document == NULL) return;
	if (strcmp(when, "ready") == 0) nav_dumps = 0;
	if (nav_dumps >= MACSURF_SLIDER_MAX_DUMPS) return;
	nav_dumps++;

	if (dom_document_get_document_element(c->document, &root) != DOM_NO_ERR
			|| root == NULL)
		return;

	slider = html_slider_find((dom_node *)root, "featured-slides", NULL, 0);
	dom_node_unref((dom_node *)root);

	if (slider == NULL) {
		/* Not a finding to skip past: if the container is absent from
		 * the DOM entirely then no amount of measuring explains the
		 * collapse, and the hunt moves to whether it was ever parsed. */
		dom_element *root2 = NULL;
		dom_node *sec = NULL;
		dom_node *fg = NULL;
		dom_node *si = NULL;
		dom_node *ch = NULL;
		dom_node *nx = NULL;
		int kids = 0;
		int slines = 0;

		macsurf_debug_log_writef(
			"LIFE SLIDER[%s] .featured-slides NOT IN DOM", when);

		/* fixes1096 - WHY is it missing? One "NOT IN DOM" line cannot
		 * tell three worlds apart:
		 *   (a) the markup never arrived or was never parsed;
		 *   (b) the markup WAS there and a script removed it;
		 *   (c) the theme never emits it for this page shape.
		 * Presence of the surrounding section.featured, of the theme's
		 * other featured classes, and of slick's own 'slick-initialized'
		 * marker splits them: section present + slides absent means a
		 * script deleted the slides; slick-init present anywhere means
		 * slick ran on SOME element; everything absent means the HTML
		 * itself never arrived. */
		if (dom_document_get_document_element(c->document, &root2)
				!= DOM_NO_ERR || root2 == NULL)
			return;

		sec = html_slider_find((dom_node *)root2, "featured", "section", 0);
		if (sec == NULL)
			sec = html_slider_find((dom_node *)root2, "featured", NULL, 0);
		fg = html_slider_find((dom_node *)root2, "featured-grid", NULL, 0);
		si = html_slider_find((dom_node *)root2, "slick-initialized",
				NULL, 0);

		macsurf_debug_log_writef(
			"LIFE SLIDER[%s] C-side: section.featured=%s"
			" featured-grid=%s slick-initialized=%s",
			when, sec ? "PRESENT" : "MISSING",
			fg ? "PRESENT" : "MISSING", si ? "PRESENT" : "MISSING");

		/* Name what actually sits where the slider should have been:
		 * one level of element children, tagged and classed. */
		if (sec != NULL) {
			macsurf_debug_log_writef(
				"LIFE SLIDER[%s] section children:", when);
			if (dom_node_get_first_child(sec, &ch) != DOM_NO_ERR)
				ch = NULL;
			while (ch != NULL && slines < 16) {
				dom_node_type t2 = (dom_node_type)0;
				dom_node *nx2 = NULL;
				if (dom_node_get_node_type(ch, &t2) == DOM_NO_ERR &&
						t2 == DOM_ELEMENT_NODE) {
					char brief[88];
					html_pagemap_brief(ch, brief,
							(int)sizeof brief);
					macsurf_debug_log_writef(
						"LIFE SLIDER[%s]   %s", when, brief);
					slines++;
					kids++;
				}
				if (dom_node_get_next_sibling(ch, &nx2) != DOM_NO_ERR)
					nx2 = NULL;
				dom_node_unref(ch);
				ch = nx2;
			}
			if (ch != NULL) dom_node_unref(ch);
			macsurf_debug_log_writef(
				"LIFE SLIDER[%s]   (%d element children)", when, kids);
		}

		if (sec != NULL) dom_node_unref(sec);
		if (fg != NULL) dom_node_unref(fg);
		if (si != NULL) dom_node_unref(si);
		dom_node_unref((dom_node *)root2);

		/* JS half of the probe: what do the page's OWN scripts see --
		 * jQuery and jQuery.fn.slick existence, plus the same featured
		 * classes answered through the engine's querySelector (which can
		 * differ from a libdom walk if script rebuilt the tree). */
		if (c->js_thread != NULL)
			js_fire_slider_probe(c->js_thread, when);
		return;
	}

	macsurf_slider_line_budget = 50;
	macsurf_debug_log_writef(
		"LIFE SLIDER[%s] ---- subtree (d, tag, box, geom, cssh, inline style)",
		when);
	html_slider_walk(slider, 0);
	macsurf_debug_log_writef("LIFE SLIDER[%s] ---- end", when);
	dom_node_unref(slider);

	/* JS half of the probe, also when the subtree IS present: the widget
	 * can be in the DOM and still dead if jQuery.fn.slick never landed. */
	if (c->js_thread != NULL)
		js_fire_slider_probe(c->js_thread, when);
}
/* ====================================================================== */

/* fixes1262 (#167) - direct C-side replacement for fixes1261's JS-side
 * FBCSS query, which came back empty (html_bg=/body_bg=/body_color= all
 * blank) because qjs_get_computed_style's box lookup (qjs_box_for) gates
 * on qjs_geometry_settled() - a gate meant for getBoundingClientRect/
 * offsetWidth's relayout-cost problem, but qjs_get_computed_style bundles
 * color/backgroundColor into the SAME box lookup as the true geometry
 * fields, so they got swept into a gate that has nothing to do with them.
 * That was a diagnostic-timing bug, not a finding.
 *
 * This calls box_for_node() directly, the same function html_pagemap_line
 * (right above) already calls successfully at this exact point in this
 * exact function - no JS round-trip, no geometry-settle gate, reading the
 * SAME struct box/css_computed_style the real renderer used for THIS
 * document. he/body are already resolved by the caller; reused, not
 * re-walked. */
static void html_fbcss_report(dom_element *he, dom_node *body,
		const char *when)
{
	struct box *hb;
	struct box *bb;
	char htmlcolor[10] = "-";
	char htmlbg[10]    = "-";
	char bodycolor[10] = "-";
	char bodybg[10]    = "-";
	char clsbuf[256] = "";

	if (he != NULL) {
		dom_string *class_name = NULL;
		if (dom_string_create((const uint8_t *) "class", 5,
				&class_name) == DOM_NO_ERR &&
				class_name != NULL) {
			dom_string *cls = NULL;
			if (dom_element_get_attribute(he, class_name, &cls) ==
					DOM_NO_ERR && cls != NULL) {
				const char *cdata = dom_string_data(cls);
				size_t clen = dom_string_byte_length(cls);
				if (cdata != NULL) {
					if (clen >= sizeof clsbuf)
						clen = sizeof clsbuf - 1;
					memcpy(clsbuf, cdata, clen);
					clsbuf[clen] = '\0';
				}
				dom_string_unref(cls);
			}
			dom_string_unref(class_name);
		}
	}

	/* fixes1262 - css_computed_color/background_color's return code is
	 * not checked, matching qjs_get_computed_style's own already-proven
	 * usage (macsurf_qjs.c) exactly: `color` is an inherited property,
	 * fully resolved by the time a box has a computed style at all, so
	 * the out-parameter is always the real answer here. */
	hb = (he != NULL) ? box_for_node((dom_node *) he) : NULL;
	if (hb != NULL && hb->style != NULL) {
		css_color col = 0;
		css_computed_color(hb->style, &col);
		ns_color_hex(htmlcolor, (unsigned long) col);
		col = 0;
		css_computed_background_color(hb->style, &col);
		ns_color_hex(htmlbg, (unsigned long) col);
	}

	bb = (body != NULL) ? box_for_node(body) : NULL;
	if (bb != NULL && bb->style != NULL) {
		css_color col = 0;
		css_computed_color(bb->style, &col);
		ns_color_hex(bodycolor, (unsigned long) col);
		col = 0;
		css_computed_background_color(bb->style, &col);
		ns_color_hex(bodybg, (unsigned long) col);
	}

	macsurf_debug_log_writef(
		"LIFE FBCSS[%s] class=[%s] html_color=%s html_bg=%s "
		"body_color=%s body_bg=%s",
		when, clsbuf, htmlcolor, htmlbg, bodycolor, bodybg);
}

/* fixes1264 (#167) - fixes1263's clean FBCSS read proved body_color=#1C1E21
 * (Facebook's own correct light-mode --fds-primary-text) against
 * body_bg=#1F1F22 (dark, wrong for a light-mode page) - the invisible text
 * is correct foreground over incorrect background, not a bad --fds-*
 * lookup for the TEXT itself. What FBCSS could not show: html_redraw_box.c
 * has real, independent CSS 2.1 SS14.2 root/body background-propagation
 * logic (html_redraw_find_bg_box, box_html/box_body in box_special.c) -
 * if the root <html> box has no background, BODY's background propagates
 * to become the page canvas fill. So body_bg=#1F1F22 could very plausibly
 * BE why the entire canvas goes dark, but nothing yet proves which box the
 * renderer actually selected. This reads html's and body's own
 * background-color directly (same box_for_node() pattern as
 * html_fbcss_report, called from the same safe point) and applies the
 * IDENTICAL selection rule html_redraw_find_bg_box uses, so root_source/
 * canvas_bg reflect what real paint decided, not a re-guess. Full
 * AARRGGBB this time (css_color's native packing needs no shift/mask at
 * all - printing it raw sidesteps the exact mistake fixes1263 had to
 * fix). */
static void html_fbpaint_report(dom_element *he, dom_node *body,
		const char *when)
{
	struct box *hb;
	struct box *bb;
	css_color hcol = 0;
	css_color bcol = 0;
	int h_has_bg = 0;
	int b_has_bg = 0;
	const char *root_source = "none";
	char htmlbgbuf[12] = "-";
	char bodybgbuf[12] = "-";
	char canvasbg[12] = "-";

	hb = (he != NULL) ? box_for_node((dom_node *) he) : NULL;
	if (hb != NULL && hb->style != NULL) {
		css_computed_background_color(hb->style, &hcol);
		h_has_bg = !nscss_color_is_transparent(hcol);
		ns_color_hex_alpha(htmlbgbuf, (unsigned long) hcol);
	}

	bb = (body != NULL) ? box_for_node(body) : NULL;
	if (bb != NULL && bb->style != NULL) {
		css_computed_background_color(bb->style, &bcol);
		b_has_bg = !nscss_color_is_transparent(bcol);
		ns_color_hex_alpha(bodybgbuf, (unsigned long) bcol);
	}

	/* Mirrors html_redraw_find_bg_box's root-box branch exactly: root's
	 * own background wins if non-transparent; otherwise body's
	 * propagates; otherwise neither paints (canvas stays UA default). */
	if (h_has_bg) {
		root_source = "html";
		ns_color_hex_alpha(canvasbg, (unsigned long) hcol);
	} else if (b_has_bg) {
		root_source = "body";
		ns_color_hex_alpha(canvasbg, (unsigned long) bcol);
	}

	/* fixes1264 CRASH-CLASS NOTE: every value below reaches
	 * macsurf_debug_log_writef via %s from a buffer pre-formatted with
	 * REAL sprintf above, never via %X/%08lX directly in this call.
	 * macsurf_debug_log.c's hand-rolled formatter only recognizes
	 * %d/%ld/%p/%s/%% (fixes1255) - an unrecognized specifier consumes
	 * no va_arg, so a later %s in the SAME call reads the wrong slot and
	 * dereferences an integer as a pointer. Caught in review before this
	 * ever reached hardware; see the fixes1255 commit for the exact
	 * mechanism. */
	macsurf_debug_log_writef(
		"LIFE FBPAINT[%s] html_bg=%s body_bg=%s "
		"root_source=%s canvas_bg=%s",
		when, htmlbgbuf, bodybgbuf, root_source, canvasbg);
}

void html_pagemap_dump(html_content *c, const char *when)
{
	dom_element *root = NULL;
	dom_node *body = NULL;
	dom_node *ch = NULL;
	dom_node *nx = NULL;
	int shown = 0;

	/* fixes1017 - per-NAVIGATION dump cap (a fresh "ready" refills it), so
	 * the dotdotdot-style 1s reconvert ticker cannot spend the whole
	 * session budget re-dumping an unchanged page: ready + done + the
	 * first four reconverts tell the story. Session cap stays as the
	 * absolute backstop. */
	static int nav_dumps = 0;

	if (c == NULL || c->document == NULL) return;
#if !defined(MACSURF_JS_AUDIT) && !defined(MACSURF_PAGEMAP)
	/* fixes1024 - the pagemap is DIAGNOSTIC and every line is a synchronous
	 * write + volume flush. Off unless explicitly asked for.
	 * fixes1028 - MACSURF_PAGEMAP is its own gate (defined below, ON for
	 * this round) so the river probe reaches the field without turning the
	 * whole JS audit back on. */
	(void) when;
	return;
#endif
	if (strcmp(when, "ready") == 0) nav_dumps = 0;
	/* fixes1093 - stays at 3. The whole-page pagemap is context; the
	 * targeted html_slider_probe carries this round's question and has its
	 * own (larger) per-nav budget, so there is no reason to spend log
	 * volume re-dumping the entire document. */
	if (nav_dumps >= 3) return;  /* fixes1020: ready + done + 1 reconvert */
	if (macsurf_pagemap_dumps >= MACSURF_PAGEMAP_MAX_DUMPS) return;
	macsurf_pagemap_dumps++;
	nav_dumps++;
	/* fixes1305 (#167, C0) - 130 was sized for a probe on a specific
	 * shallow hero container (fixes1016/1093), never for a whole real
	 * Facebook page: even with LINK/META now skipped, Comet's real DOM
	 * is thousands of elements deep in a way this budget can't reach
	 * past the first section or two of. Raised for the current
	 * whole-page-story investigation (#167 C0); OS X 10.3 test target
	 * has real headroom for this, unlike the OS 9 hardware floor this
	 * number originally had to respect. */
	macsurf_pagemap_line_budget = 600;

	/* find <body>: documentElement's first element child named BODY */
	if (dom_document_get_document_element(c->document, &root) != DOM_NO_ERR
			|| root == NULL) {
		macsurf_debug_log_writef("LIFE pagemap[%s] NO documentElement",
				when);
		return;
	}
	if (dom_node_get_first_child((dom_node *)root, &ch) != DOM_NO_ERR)
		ch = NULL;
	while (ch != NULL) {
		dom_node_type t2 = (dom_node_type)0;
		dom_string *nm = NULL;
		if (dom_node_get_node_type(ch, &t2) == DOM_NO_ERR &&
				t2 == DOM_ELEMENT_NODE &&
				dom_node_get_node_name(ch, &nm) == DOM_NO_ERR &&
				nm != NULL) {
			int is_body = (strcasecmp(dom_string_data(nm), "body")
					== 0);
			dom_string_unref(nm);
			if (is_body) { body = ch; break; }
		} else if (nm != NULL) {
			dom_string_unref(nm);
		}
		nx = NULL;
		if (dom_node_get_next_sibling(ch, &nx) != DOM_NO_ERR) nx = NULL;
		dom_node_unref(ch);
		ch = nx;
	}
	dom_node_unref((dom_node *)root);
	if (body == NULL) {
		macsurf_debug_log_writef("LIFE pagemap[%s] NO body", when);
		return;
	}

	macsurf_debug_log_writef(
		"LIFE pagemap[%s] ---- body sections (tag#id.class kids box y w h disp)",
		when);
	/* fixes1016 - the <html> element's own line first: its CLASS LIST is
	 * where page-state machines live (Typekit's wf-loading/wf-active,
	 * XenForo's has-js, no-js themes), and whether those ever resolve is
	 * exactly what the audit needs to answer. */
	{
		dom_element *he = NULL;
		if (dom_document_get_document_element(c->document, &he)
				== DOM_NO_ERR && he != NULL) {
			(void) html_pagemap_line((dom_node *)he, 0, NULL);
			html_fbcss_report(he, body, when);
			html_fbpaint_report(he, body, when);
			dom_node_unref((dom_node *)he);
		}
	}
	if (dom_node_get_first_child(body, &ch) != DOM_NO_ERR) ch = NULL;
	while (ch != NULL && macsurf_pagemap_line_budget > 0) {
		dom_node_type t2 = (dom_node_type)0;
		if (dom_node_get_node_type(ch, &t2) == DOM_NO_ERR &&
				t2 == DOM_ELEMENT_NODE) {
			shown++;
			html_pagemap_walk(ch, 0);
		}
		nx = NULL;
		if (dom_node_get_next_sibling(ch, &nx) != DOM_NO_ERR) nx = NULL;
		dom_node_unref(ch);
		ch = nx;
	}
	/* fixes1305 (#167, C0) - "sections" only ever counted top-level body
	 * children VISITED (shown++ above), not lines actually printed --
	 * with the budget exhausted deep inside child #1, every dump ever
	 * emitted read as "328 sections" regardless of whether 3 lines or
	 * 3000 actually made it out. State the truncation explicitly so a
	 * dump can be trusted (or distrusted) at a glance instead of by
	 * counting what's missing. */
	macsurf_debug_log_writef(
		"LIFE pagemap[%s] ---- end (%d sections visited, "
		"budget %s, %d remaining)",
		when, shown,
		(macsurf_pagemap_line_budget <= 0) ? "EXHAUSTED" : "ok",
		macsurf_pagemap_line_budget);
	dom_node_unref(body);
}
/* ====================================================================== */

/* Documented in html_internal.h */
nserror
html_proceed_to_done(html_content *html)
{
	switch (content__get_status(&html->base)) {
	case CONTENT_STATUS_READY:
		if (html->base.active == 0) {
			/* fixes881 (Phase 0.7) - THE load event, and the only one.
			 * This is the READY->DONE transition: the box tree exists
			 * (html_box_convert_done ran) and base.active has fallen to
			 * 0, so every subresource has settled. That is precisely
			 * what the spec means by `load`, and it is strictly after
			 * the DOMContentLoaded fired in html_box_convert_done.
			 * Fired BEFORE content_set_done so a listener observes the
			 * page in the same state a real browser would.
			 * js_fire_window_load is idempotent per realm: object.c can
			 * call this function repeatedly as subresources land. */
			/* fixes1022 - quiesced by default: firing load woke
			 * dotdotdot/slick-class widgets that measure-then-
			 * mutate, and without Phase 3's synchronous layout they
			 * degrade the page (truncated articles, collapsed
			 * slider, a 1s re-truncation ticker). Re-enable in the
			 * round that ships forced layout. */
#ifdef MACSURF_JS_FIRE_LOAD
			if (html->js_thread != NULL) {
				js_fire_window_load(html->js_thread,
						html->document);
			}
#endif
			content_set_done(&html->base);
			html_pagemap_dump(html, "done"); /* fixes1015 */
			html_slider_probe(html, "done"); /* fixes1093 */
			return NSERROR_OK;
		}
		break;
	case CONTENT_STATUS_DONE:
		/* fallthrough */
	case CONTENT_STATUS_LOADING:
		return NSERROR_OK;
	default:
		NSLOG(netsurf, ERROR, "Content status unexpectedly not LOADING/READY/DONE");
		break;
	}
	return NSERROR_UNKNOWN;
}


/* fixes615 (webfonts, Item 1) - resolve a CSS font-family name to a
 * downloadable @font-face src URL we can rasterize (raw sfnt / OpenType /
 * WOFF), joined against the document base. Returns a NEW nsurl ref (caller
 * unrefs) or NULL if the family has no usable @font-face URI rule.
 *
 * libcss fully parses @font-face and exposes css_select_font_faces(), but
 * nothing in NetSurf ever calls it, so downloadable webfonts (how modern
 * sites ship icon fonts - FontAwesome, Material Design) never render. This
 * lives in core because it needs the html content's own select ctx / media /
 * unit ctx. The macos9 frontend calls it during paint (see macos9_webfont.c)
 * and fetches the returned URL. WOFF2 (format UNKNOWN) is converted to raw
 * sfnt by the frontend (macos9_woff2.c + Brotli) before parsing. */
struct nsurl *html_macsurf_font_face_url(struct content *c, lwc_string *family)
{
	html_content *htmlc = (html_content *) c;
	css_select_font_faces_results *res = NULL;
	struct nsurl *out = NULL;

	if (c == NULL || family == NULL || htmlc->select_ctx == NULL)
		return NULL;

	if (css_select_font_faces(htmlc->select_ctx, &htmlc->media,
			&htmlc->unit_len_ctx, family, &res) != CSS_OK)
		return NULL;
	if (res == NULL)
		return NULL;

	if (res->n_font_faces > 0) {
		const css_font_face *ff = res->font_faces[0];
		uint32_t nsrc = 0;
		uint32_t i;
		int pass;

		css_font_face_count_srcs(ff, &nsrc);
		/* Tiered preference: the macos9 rasterizer parses RAW sfnt, so
		 * prefer a "truetype"/"opentype" src (a bare .ttf/.otf) - libcss
		 * maps both to _OPENTYPE. Fall back to WOFF2 (format UNKNOWN;
		 * Brotli-inflated to raw sfnt by macos9_woff2.c), then an
		 * unhinted src (magic-validated at parse time), then WOFF (zlib;
		 * still rejected at parse - kept as a future hook).
		 * Never pick EOT or SVG. Multi-pass so the .ttf wins even when
		 * the CSS lists woff/woff2 first. */
		for (pass = 0; pass < 4 && out == NULL; pass++) {
			css_font_face_format want =
				(pass == 0) ? CSS_FONT_FACE_FORMAT_OPENTYPE :
				(pass == 1) ? CSS_FONT_FACE_FORMAT_UNKNOWN :
				(pass == 2) ? CSS_FONT_FACE_FORMAT_UNSPECIFIED :
				              CSS_FONT_FACE_FORMAT_WOFF;
			for (i = 0; i < nsrc && out == NULL; i++) {
				const css_font_face_src *src = NULL;
				lwc_string *loc = NULL;

				if (css_font_face_get_src(ff, i, &src) != CSS_OK ||
						src == NULL)
					continue;
				if (css_font_face_src_location_type(src) !=
						CSS_FONT_FACE_LOCATION_TYPE_URI)
					continue;
				if (css_font_face_src_format(src) != want)
					continue;
				if (css_font_face_src_get_location(src, &loc) !=
						CSS_OK || loc == NULL)
					continue;
				nsurl_join(htmlc->base_url,
						lwc_string_data(loc), &out);
			}
		}
	}

	css_select_font_faces_results_destroy(res);
	return out;
}


static void html_get_dimensions(html_content *htmlc)
{
	css_fixed device_dpi = nscss_screen_dpi;
	unsigned f_size;
	unsigned f_min;
	/* fixes612: initialise to 0 so a GETDIMS broadcast that reaches no
	 * handler (content unattached at select time) leaves a detectable 0
	 * rather than uninitialised garbage (was observed as media.width=0,
	 * media.height=21845). */
	unsigned w = 0;
	unsigned h = 0;
	/* MacSurf: C89 unions can't use designated initializers - populate
	 * the .getdims branch via assignment. */
	union content_msg_data msg_data;

	msg_data.getdims.viewport_width = &w;
	msg_data.getdims.viewport_height = &h;

	content_broadcast(&htmlc->base, CONTENT_MSG_GETDIMS, &msg_data);

#ifdef __MACOS9__
	/* fixes612: if GETDIMS delivered nothing (content not yet bound to a
	 * browser_window at CSS-select time), w/h are still 0 and every width
	 * media query collapses to the mobile branch - the reason
	 * tinkerdifferent's desktop two-column layout never applied. Fall back
	 * to the real front-window content size (device px) so @media resolves
	 * against the actual viewport. */
	if (w == 0 || h == 0) {
		extern void macos9_frontend_viewport(int *vw, int *vh);
		int vw = 0;
		int vh = 0;
		macos9_frontend_viewport(&vw, &vh);
		if (w == 0 && vw > 0) w = (unsigned)vw;
		if (h == 0 && vh > 0) h = (unsigned)vh;
	}
#endif

	w = css_unit_device2css_px(INTTOFIX(w), device_dpi);
	h = css_unit_device2css_px(INTTOFIX(h), device_dpi);

	/* fixes124: reverted fixes122's media-query viewport lie.
	 * Atari and RISC OS NetSurf frontends report real window
	 * dimensions to libcss for `@media` matching. Lying about
	 * the viewport caused author CSS to apply desktop container
	 * widths (max-width:1100px) to physical space that didn't
	 * exist, collapsing image heights into "smear" rendering.
	 * The real fix is for MacSurf to open windows at a desktop-
	 * class default size (done in window.c) so real-window-width
	 * media queries naturally match the desktop branch. */
	htmlc->media.width  = w;
	htmlc->media.height = h;
	/* fixes273 (#52) - derive orientation from viewport dims so
	 * @media (orientation: landscape) / (orientation: portrait)
	 * evaluates correctly. Without this, orientation defaults to
	 * PORTRAIT (0) regardless of the actual window size. */
	htmlc->media.orientation = (w > h) ?
			CSS_MEDIA_ORIENTATION_LANDSCAPE :
			CSS_MEDIA_ORIENTATION_PORTRAIT;
#ifdef __MACOS9__
	/* fixes1151: the unit_len_ctx viewport must be the real frontend
	 * viewport, not the GETDIMS-derived w/h.  Mirrors the html_reformat
	 * site (fixes1150): viewport-relative CSS units (vh, vw) and calc()
	 * vh/vw operands must resolve against the actual front window, which
	 * GETDIMS can miss when the content is unattached or bound to a
	 * non-front window at select time.  media.width/height stay on the
	 * GETDIMS w/h (fixes124: media queries must not be lied to). */
	{
		extern void macos9_frontend_viewport(int *vw, int *vh);
		int vw = 0;
		int vh = 0;

		macos9_frontend_viewport(&vw, &vh);
		if (vw > 0 && vh > 0) {
			htmlc->unit_len_ctx.viewport_width =
				css_unit_device2css_px(INTTOFIX(vw), device_dpi);
			htmlc->unit_len_ctx.viewport_height =
				css_unit_device2css_px(INTTOFIX(vh), device_dpi);
		} else {
			htmlc->unit_len_ctx.viewport_width  = w;
			htmlc->unit_len_ctx.viewport_height = h;
		}
	}
#else
	htmlc->unit_len_ctx.viewport_width  = w;
	htmlc->unit_len_ctx.viewport_height = h;
#endif
	htmlc->unit_len_ctx.device_dpi = device_dpi;
	macsurf_debug_log_writef(
		"LIFE VPORT finish: media w=%d h=%d unit_ctx vw=%d vh=%d",
		(int)FIXTOINT(w), (int)FIXTOINT(h),
		(int)FIXTOINT(htmlc->unit_len_ctx.viewport_width),
		(int)FIXTOINT(htmlc->unit_len_ctx.viewport_height));

	/** \todo Change nsoption font sizes to px. */
	f_size = FDIV(FMUL(F_96, FDIV(INTTOFIX(nsoption_int(font_size)), F_10)), F_72);
	f_min  = FDIV(FMUL(F_96, FDIV(INTTOFIX(nsoption_int(font_min_size)), F_10)), F_72);

	htmlc->unit_len_ctx.font_size_default = f_size;
	htmlc->unit_len_ctx.font_size_minimum = f_min;

#ifdef __MACOS9__
	/* fixes611 DIAG: log the media viewport width used for @media
	 * evaluation. If this is < 901 at CSS-select time, the mobile
	 * (max-width:900) branch bakes into the cascade and the desktop
	 * two-column layout never applies even in a 949px window.
	 *
	 * fixes858 (#287 probe) - re-prefixed "WORK " so it survives the
	 * failures-only log gate (macsurf_debug_log.c drops anything not
	 * whitelisted; the old prefix meant this line has been invisible on
	 * every default build), and widened to carry the whole em chain.
	 *
	 * Reading it: `em` in a media query resolves against fsdef (the CSS-px
	 * default font size), so hackaday's `@media (min-width:59.5em)` -- which
	 * gates its ENTIRE desktop layout incl. .main-navigation (display:none
	 * until it matches) -- needs mediaw >= need595. Chrome: fsdef=16 ->
	 * need595=952 -> matches in a ~958px window. MacSurf: nsoption font_size
	 * is 0.1pt units, 160 = 16pt, and html.c converts 96*16/72 -> fsdef=21.33
	 * -> need595=1269 -> never matches, so we render the mobile branch and
	 * the nav stays hidden.
	 *
	 * dpi is the other half: browser_set_dpi() is never called, so
	 * nscss_screen_dpi keeps its F_96 static init while QuickDraw is 72dpi.
	 * mediaw is device2css_px(device_w, dpi), so at dpi=96 CSS px == device
	 * px (958). At the physically-correct dpi=72 the same window would report
	 * 1277 CSS px and 1277 > 1269 would match -- with text unchanged (px->pt
	 * is a fixed 72/96, independent of dpi) but every px LAYOUT length
	 * shrinking to 0.75x. That trade-off is the maintainer's call; this probe
	 * exists to supply the real numbers first rather than guess. */
	{
		extern void macsurf_debug_log_writef(const char *fmt, ...);
		int mediaw = (int)FIXTOINT(htmlc->media.width);
		int fsdef  = (int)FIXTOINT(f_size);
		macsurf_debug_log_writef(
			"WORK mediaq mediaw=%d mediah=%d dpi=%d fsdef=%d fsmin=%d "
			"need595=%d need705=%d desktop=%d",
			mediaw,
			(int)FIXTOINT(htmlc->media.height),
			(int)FIXTOINT(device_dpi),
			fsdef,
			(int)FIXTOINT(f_min),
			(int)((fsdef * 595 + 5) / 10),
			(int)((fsdef * 705 + 5) / 10),
			(mediaw * 10 >= fsdef * 595) ? 1 : 0);
	}
#endif
}

/* exported function documented in html/html_internal.h */
void html_finish_conversion(html_content *htmlc)
{
	union content_msg_data msg_data;
	dom_exception exc; /* returned by libdom functions */
	dom_node *html;
	nserror error;

	/* fixes311 -- byte-in count crosses parser-boundary here. Lets us
	 * cross-check the http fetcher's body_bytes summary. LIFE-prefixed so
	 * a near-empty response (e.g. a JS-shell page with no static markup)
	 * is distinguishable from a genuine parse failure without guessing --
	 * this line was silently dropped on every release build before. */
	macsurf_debug_log_writef(
		"LIFE finish_conversion: parser_bytes=%ld head=%s",
		macos9_html_bytes_processed,
		macos9_html_head_len > 0 ? macos9_html_head : "(none)");

	/* Bail out if we've been aborted */
	if (htmlc->aborted) {
		content_broadcast_error(&htmlc->base, NSERROR_STOPPED, NULL);
		content_set_error(&htmlc->base);
		return;
	}

	/* If we already have a selection context, then we have already
	 * "finished" conversion.  We can get here twice if e.g. some JS
	 * adds a new stylesheet, and the stylesheet gets added after
	 * the HTML content is initially finished.
	 *
	 * If we didn't do this, the HTML content would try to rebuild the
	 * box tree for the html content when this new stylesheet is ready.
	 * NetSurf has no concept of dynamically changing documents, so this
	 * would break badly.
	 */
	if (htmlc->select_ctx != NULL) {
		NSLOG(netsurf, INFO,
				"Ignoring style change: NS layout is static.");
		return;
	}

	/* create new css selection context */
	error = html_css_new_selection_context(htmlc, &htmlc->select_ctx);
	if (error != NSERROR_OK) {
		content_broadcast_error(&htmlc->base, error, NULL);
		content_set_error(&htmlc->base);
		return;
	}


	/* fixes881 (Phase 0.7) - the `load` fire that used to sit HERE is gone.
	 *
	 * It ran ~30 lines BEFORE dom_to_box below, so `load` was delivered before
	 * the box tree existed; js_fire_dom_ready (from html_box_convert_done, i.e.
	 * AFTER conversion) then fired DOMContentLoaded and a SECOND `load` at the
	 * document. Net observed order:
	 *     window load -> DOMContentLoaded -> document load
	 * which is the reverse of spec, fires load twice, and never delivers a load
	 * to window once the page is actually there.
	 *
	 * `load` now fires exactly once, from html_proceed_to_done's READY->DONE
	 * transition -- the point where the box tree exists AND base.active has
	 * reached 0, i.e. every subresource has settled. That is what the spec
	 * means by load, and it is strictly after DOMContentLoaded. */

	/* convert dom tree to box tree */
	NSLOG(netsurf, INFO, "DOM to box (%p)", htmlc);
	content_set_status(&htmlc->base, messages_get("Processing"));
	msg_data.explicit_status_text = NULL;
	content_broadcast(&htmlc->base, CONTENT_MSG_STATUS, &msg_data);

	exc = dom_document_get_document_element(htmlc->document, (void *) &html);
	if ((exc != DOM_NO_ERR) || (html == NULL)) {
		NSLOG(netsurf, INFO, "error retrieving html element from dom");
		content_broadcast_error(&htmlc->base, NSERROR_DOM, NULL);
		content_set_error(&htmlc->base);
		return;
	}

	macsurf_debug_log_writef("fc: got html element, get_dimensions");
	html_get_dimensions(htmlc);

	macsurf_debug_log_writef("fc: dimensions done, dom_to_box");
	{
		extern double macos9_micros(void);
		s_convert_start_us = macos9_micros();
	}
	macsurf_debug_log_writef(
			"WORK pipeline: dom_to_box START c=%p url=%s",
			(void *) htmlc,
			nsurl_access(content_get_url((struct content *) htmlc)));
	error = dom_to_box(html, htmlc, html_box_convert_done, &htmlc->box_conversion_context);
	macsurf_debug_log_writef("fc: dom_to_box returned err=%d", (int)error);
	if (error != NSERROR_OK) {
		NSLOG(netsurf, INFO, "box conversion failed");
		dom_node_unref(html);
		html_object_free_objects(htmlc);
		content_broadcast_error(&htmlc->base, error, NULL);
		content_set_error(&htmlc->base);
		return;
	}

	dom_node_unref(html);
}


static void
html_document_user_data_handler(dom_node_operation operation,
				dom_string *key, void *data,
				struct dom_node *src,
				struct dom_node *dst)
{
	if (dom_string_isequal(corestring_dom___ns_key_html_content_data,
			       key) == false || data == NULL) {
		return;
	}

	switch (operation) {
	case DOM_NODE_CLONED:
		NSLOG(netsurf, INFO, "Cloned");
		break;
	case DOM_NODE_RENAMED:
		NSLOG(netsurf, INFO, "Renamed");
		break;
	case DOM_NODE_IMPORTED:
		NSLOG(netsurf, INFO, "imported");
		break;
	case DOM_NODE_ADOPTED:
		NSLOG(netsurf, INFO, "Adopted");
		break;
	case DOM_NODE_DELETED:
		/* This is the only path I expect */
		break;
	default:
		NSLOG(netsurf, INFO, "User data operation not handled.");
		assert(0);
	}
}


static nserror
html_create_html_data(html_content *c, const http_parameter *params)
{
	lwc_string *charset;
	nserror nerror;
	dom_hubbub_parser_params parse_params;
	dom_hubbub_error error;
	dom_exception err;
	void *old_node_data;
	const char *prefer_color_mode = (nsoption_bool(prefer_dark_mode)) ?
			"dark" : "light";

	c->parser = NULL;
	c->parse_completed = false;
	c->conversion_begun = false;
	c->document = NULL;
	c->quirks = DOM_DOCUMENT_QUIRKS_MODE_NONE;
	c->encoding = NULL;
	c->base_url = nsurl_ref(content_get_url(&c->base));
	c->base_target = NULL;
	c->aborted = false;
	c->refresh = false;
	c->reflowing = false;
	c->title = NULL;
	c->bctx = NULL;
	c->layout = NULL;
	c->background_colour = NS_TRANSPARENT;
	c->stylesheet_count = 0;
	c->stylesheets = NULL;
	c->select_ctx = NULL;
	c->media.type = CSS_MEDIA_SCREEN;
	c->universal = NULL;
	c->num_objects = 0;
	c->object_list = NULL;
	c->img_eager_budget = 10;	/* fixes932 - eager above-the-fold images */
	c->forms = NULL;
	c->imagemaps = NULL;
	c->bw = NULL;
	c->frameset = NULL;
	c->iframe = NULL;
	c->page = NULL;
	c->font_func = guit->layout;
	c->drag_type = HTML_DRAG_NONE;
	c->drag_owner.no_owner = true;
	c->selection_type = HTML_SELECTION_NONE;
	c->selection_owner.none = true;
	c->focus_type = HTML_FOCUS_SELF;
	c->focus_owner.self = true;
	c->scripts_count = 0;
	c->scripts = NULL;
	c->js_thread = NULL;

	c->enable_scripting = nsoption_bool(enable_javascript);
#ifdef __MACOS9__
	/* fixes852 (#167) - m.facebook.com is FB's no-JS feature-phone
	 * surface (KaiOS UA - the reliable login + new-device-checkpoint
	 * path; see the role split in macos9_fetch.c: KaiOS on m, FF134 on
	 * www). Phase 1 completed login there with FB's JS effectively inert
	 * (fetch/XHR were dead stubs). fixes846 made those bundles run for
	 * real, and on hardware the notification-2FA approval stopped
	 * completing - the log shows FB's ~550KB of JS executing on the
	 * two_factor page, ZERO xhr/fetch, ZERO reconvert, and c_user/xs
	 * never issued (login parked at the checkpoint). The m. surface is
	 * DESIGNED to work without scripting; running FB's modern JS there
	 * only invites a JS-app flow that half-works. Force scripting OFF for
	 * m.facebook.com so the plain-HTML login/checkpoint flow (and any
	 * <noscript> fallback, which hubbub only parses to real DOM when
	 * scripting is off - in_head.c:147) run as they did pre-Phase-2.
	 * www.facebook.com (the feed, which NEEDS JS) is unaffected.
	 *
	 * fixes1229 (#167) - 2026-08-20 hardware log: on m., the
	 * /two_step_verification/authentication/ checkpoint DOES render real
	 * HTML (pagemap: 18 sections, ~46 KB body per the finish_conversion
	 * byte delta) but the verification dialog itself (DIV#modalDialog,
	 * DIV#dialogSpinner) is box=0 -- a JS-toggled-visible modal that
	 * fixes852's scripting-off gate can never reveal. On www, the SAME
	 * checkpoint URL confirmed (curl probe + a 9-byte finish_conversion
	 * delta on hardware, matching the curl 404's content-length exactly)
	 * to return next to nothing on a direct navigation -- not a MacSurf
	 * gap, a route Facebook's server won't serve outside its own SPA
	 * router. m. is therefore the only reachable surface for this
	 * checkpoint, and the modal's visibility is the only thing blocking
	 * it. Narrow exception: scripting stays OFF for m.facebook.com
	 * everywhere EXCEPT this one confirmed-JS-gated path, so the
	 * login/checkpoint plain-HTML flow fixes852 protects everywhere else
	 * is untouched. */
	if (c->base_url != NULL) {
		lwc_string *hcomp = nsurl_get_component(c->base_url, NSURL_HOST);
		if (hcomp != NULL) {
			const char *hs = lwc_string_data(hcomp);
			size_t hl = lwc_string_length(hcomp);
			if (hl == 14 && strncasecmp(hs, "m.facebook.com", 14) == 0) {
				const char *full = nsurl_access(c->base_url);
				if (full == NULL || strstr(full,
						"/two_step_verification/") == NULL) {
					c->enable_scripting = false;
				}
			}
			lwc_string_unref(hcomp);
		}
	}
#endif
	c->base.active = 1; /* The html content itself is active */

	if (lwc_intern_string("*", SLEN("*"), &c->universal) != lwc_error_ok) {
		return NSERROR_NOMEM;
	}

	if (lwc_intern_string(prefer_color_mode, strlen(prefer_color_mode),
			&c->media.prefers_color_scheme) != lwc_error_ok) {
		lwc_string_unref(c->universal);
		c->universal = NULL;
		return NSERROR_NOMEM;
	}

	c->sel = selection_create((struct content *)c);

	nerror = http_parameter_list_find_item(params, corestring_lwc_charset, &charset);
	if (nerror == NSERROR_OK) {
		c->encoding = strdup(lwc_string_data(charset));

		lwc_string_unref(charset);

		if (c->encoding == NULL) {
			lwc_string_unref(c->universal);
			c->universal = NULL;
			lwc_string_unref(c->media.prefers_color_scheme);
			c->media.prefers_color_scheme = NULL;
			return NSERROR_NOMEM;

		}
		c->encoding_source = DOM_HUBBUB_ENCODING_SOURCE_HEADER;
	}

	/* Create the parser binding */
	parse_params.enc = c->encoding;
	parse_params.fix_enc = true;
	parse_params.enable_script = c->enable_scripting;
	parse_params.msg = NULL;
	parse_params.script = html_process_script;
	parse_params.ctx = c;
	parse_params.daf = html_dom_event_fetcher;

	error = dom_hubbub_parser_create(&parse_params,
					 &c->parser,
					 &c->document);
	if ((error != DOM_HUBBUB_OK) && (c->encoding != NULL)) {
		/* Ok, we don't support the declared encoding. Bailing out
		 * isn't exactly user-friendly, so fall back to autodetect */
		free(c->encoding);
		c->encoding = NULL;

		parse_params.enc = c->encoding;

		error = dom_hubbub_parser_create(&parse_params,
						 &c->parser,
						 &c->document);
	}
	if (error != DOM_HUBBUB_OK) {
		nsurl_unref(c->base_url);
		c->base_url = NULL;

		lwc_string_unref(c->universal);
		c->universal = NULL;
		lwc_string_unref(c->media.prefers_color_scheme);
		c->media.prefers_color_scheme = NULL;

		return libdom_hubbub_error_to_nserror(error);
	}

	err = dom_node_set_user_data(c->document,
				     corestring_dom___ns_key_html_content_data,
				     c, html_document_user_data_handler,
				     (void *) &old_node_data);
	if (err != DOM_NO_ERR) {
		dom_hubbub_parser_destroy(c->parser);
		c->parser = NULL;
		nsurl_unref(c->base_url);
		c->base_url = NULL;

		lwc_string_unref(c->universal);
		c->universal = NULL;
		lwc_string_unref(c->media.prefers_color_scheme);
		c->media.prefers_color_scheme = NULL;

		NSLOG(netsurf, INFO, "Unable to set user data.");
		return NSERROR_DOM;
	}

	assert(old_node_data == NULL);

	return NSERROR_OK;

}

/**
 * Create a CONTENT_HTML.
 *
 * The content_html_data structure is initialized and the HTML parser is
 * created.
 */

static nserror
html_create(const content_handler *handler,
	    lwc_string *imime_type,
	    const http_parameter *params,
	    llcache_handle *llcache,
	    const char *fallback_charset,
	    bool quirks,
	    struct content **c)
{
	html_content *html;
	nserror error;

#ifdef __MACOS9__
	{
		extern void macsurf_debug_log_writef(const char *fmt, ...);
		extern long macsurf__site_img_ok;
		extern long macsurf__site_img_fail;
		extern long macsurf__site_css_ok;
		extern long macsurf__site_css_skip;
		extern long macsurf__site_rgov_skip_doc;
		extern long macsurf__site_rgov_skip_css;
		extern long macsurf__site_rgov_skip_img;
		extern long macsurf__site_rgov_skip_script;
		extern long macsurf__site_rgov_skip_font;
		extern long macsurf__site_rgov_skip_other;
		extern long macsurf__site_fetch_active_peak;
		extern long macsurf__site_fetch_slot_fail;
		extern long macsurf__site_heavy;
		extern long macsurf__site_decoded_img_bytes_peak;
		extern long macsurf__site_decoded_img_skip_budget;
		extern long macsurf__site_box_total;
		extern long macsurf__site_box_blk;
		extern long macsurf__site_box_inlinec;
		extern long macsurf__site_box_inline;
		extern long macsurf__site_box_text;
		extern long macsurf__site_box_other;
		extern unsigned long macsurf__site_css_total_bytes;
		extern long macsurf__site_blocker;
		macsurf_debug_log_writef("html_create: entered");
		/* fixes160a: reset SITE per-page counters at the top of every
		 * new HTML content so the summary line emitted at reformat
		 * reflects this page only, not session-cumulative.
		 * fixes160d: extended with css_ok / css_skip counters.
		 * fixes161a: extended with resource-governor counters.
		 * fixes161b: peak + skip_budget reset per page; the
		 * current decoded-bytes counter is NOT reset because it
		 * tracks live process-wide bitmaps across navigations. */
		macsurf__site_img_ok = 0;
		macsurf__site_img_fail = 0;
		macsurf__site_css_ok = 0;
		macsurf__site_css_skip = 0;
		macsurf__site_rgov_skip_doc = 0;
		macsurf__site_rgov_skip_css = 0;
		macsurf__site_rgov_skip_img = 0;
		macsurf__site_rgov_skip_script = 0;
		macsurf__site_rgov_skip_font = 0;
		macsurf__site_rgov_skip_other = 0;
		macsurf__site_fetch_active_peak = 0;
		macsurf__site_fetch_slot_fail = 0;
		macsurf__site_heavy = 0;
		macsurf__site_decoded_img_bytes_peak = 0;
		macsurf__site_decoded_img_skip_budget = 0;
		macsurf__site_box_total = 0;
		macsurf__site_box_blk = 0;
		macsurf__site_box_inlinec = 0;
		macsurf__site_box_inline = 0;
		macsurf__site_box_text = 0;
		macsurf__site_box_other = 0;
		/* fixes268 (#9,#11) */
		macsurf_debug_log_writef("css: budget reset was=%ld", (long)macsurf__site_css_total_bytes);
		macsurf__site_css_total_bytes = 0;
		macsurf__site_blocker = 0;
	}
#endif
	/* fixes267 - clear the doc-global inline-extras custom-property
	 * table at the start of every new HTML document so element-scoped
	 * --custom-prop declarations from the previous page don't bleed
	 * into the new one. */
	{
		extern void css_inline_extras_clear(void);
		css_inline_extras_clear();
	}

	html = calloc(1, sizeof(html_content));
	if (html == NULL)
		return NSERROR_NOMEM;

	error = content__init(&html->base, handler, imime_type, params,
			llcache, fallback_charset, quirks);
	if (error != NSERROR_OK) {
		free(html);
		return error;
	}

	MS_LOG("html create");
	error = html_create_html_data(html, params);
#ifdef __MACOS9__
	{
		extern void macsurf_debug_log_writef(const char *fmt, ...);
		macsurf_debug_log_writef("html_create: html_data err=%d", (int)error);
	}
#endif
	if (error != NSERROR_OK) {
		content_broadcast_error(&html->base, error, NULL);
#ifdef __MACOS9__
		/* fixes534: content__init registered this content; this error
		 * path frees it directly (not via content_destroy), so drop the
		 * registry slot first to avoid leaving a stale 'live' entry. */
		{ extern void macos9_content_unregister(struct content *c);
		  macos9_content_unregister(&html->base); }
#endif
		free(html);
		return error;
	}

	error = html_css_new_stylesheets(html);
#ifdef __MACOS9__
	{
		extern void macsurf_debug_log_writef(const char *fmt, ...);
		macsurf_debug_log_writef("html_create: css_new_sheets err=%d", (int)error);
	}
#endif
	if (error != NSERROR_OK) {
		content_broadcast_error(&html->base, error, NULL);
#ifdef __MACOS9__
		/* fixes534: content__init registered this content; this error
		 * path frees it directly (not via content_destroy), so drop the
		 * registry slot first to avoid leaving a stale 'live' entry. */
		{ extern void macos9_content_unregister(struct content *c);
		  macos9_content_unregister(&html->base); }
#endif
		free(html);
		return error;
	}

	*c = (struct content *) html;

#ifdef __MACOS9__
	{
		extern void macsurf_debug_log_writef(const char *fmt, ...);
		macsurf_debug_log_writef("html_create: returning OK content=%p", (void*)html);
	}
#endif
	return NSERROR_OK;
}



static nserror
html_process_encoding_change(struct content *c,
			     const char *data,
			     unsigned int size)
{
	html_content *html = (html_content *) c;
	dom_hubbub_parser_params parse_params;
	dom_hubbub_error error;
	const char *encoding;
	const uint8_t *source_data;
	size_t source_size;

	/* Retrieve new encoding */
	encoding = dom_hubbub_parser_get_encoding(html->parser,
						  &html->encoding_source);
	if (encoding == NULL) {
		return NSERROR_NOMEM;
	}

	if (html->encoding != NULL) {
		free(html->encoding);
		html->encoding = NULL;
	}

	html->encoding = strdup(encoding);
	if (html->encoding == NULL) {
		return NSERROR_NOMEM;
	}

	/* Destroy binding */
	dom_hubbub_parser_destroy(html->parser);
	html->parser = NULL;

	if (html->document != NULL) {
		dom_node_unref(html->document);
	}

	parse_params.enc = html->encoding;
	parse_params.fix_enc = true;
	parse_params.enable_script = html->enable_scripting;
	parse_params.msg = NULL;
	parse_params.script = html_process_script;
	parse_params.ctx = html;
	parse_params.daf = html_dom_event_fetcher;

	/* Create new binding, using the new encoding */
	error = dom_hubbub_parser_create(&parse_params,
					 &html->parser,
					 &html->document);
	if (error != DOM_HUBBUB_OK) {
		/* Ok, we don't support the declared encoding. Bailing out
		 * isn't exactly user-friendly, so fall back to Windows-1252 */
		free(html->encoding);
		html->encoding = strdup("Windows-1252");
		if (html->encoding == NULL) {
			return NSERROR_NOMEM;
		}
		parse_params.enc = html->encoding;

		error = dom_hubbub_parser_create(&parse_params,
						 &html->parser,
						 &html->document);

		if (error != DOM_HUBBUB_OK) {
			return libdom_hubbub_error_to_nserror(error);
		}

	}

	source_data = content__get_source_data(c, &source_size);

	/* fixes506: don't feed a NULL/empty buffer to the parser. After
	 * a double-destroy nulls c->llcache, content__get_source_data
	 * returns NULL here; passing that to dom_hubbub_parser_parse_chunk
	 * makes the tokenizer scan from a garbage pointer (r4=1 crash). */
	if (source_data == NULL || source_size == 0) {
		macsurf_debug_log_writef(
			"html: reprocess skipped, source_data=%p size=%ld",
			(void *)source_data, (long)source_size);
		return NSERROR_OK;
	}

	/* Reprocess all the data.  This is safe because
	 * the encoding is now specified at parser start which means
	 * it cannot be changed again.
	 */
	error = dom_hubbub_parser_parse_chunk(html->parser,
					      source_data,
					      source_size);

	return libdom_hubbub_error_to_nserror(error);
}


/**
 * Process data for CONTENT_HTML.
 */

static bool
html_process_data(struct content *c, const char *data, unsigned int size)
{
	html_content *html = (html_content *) c;
	dom_hubbub_error dom_ret;
	nserror err = NSERROR_OK; /* assume its all going to be ok */

	MS_LOG("html process data");
	macos9_html_bytes_processed += size;
	if (macos9_html_head_len == 0 && data != NULL && size > 0) {
		unsigned int copy = size;
		unsigned int i;
		if (copy > 63) copy = 63;
		for (i = 0; i < copy; i++) {
			unsigned char ch = (unsigned char)data[i];
			if (ch < 0x20 || ch >= 0x7f) ch = '.';
			macos9_html_head[i] = (char)ch;
		}
		macos9_html_head[copy] = '\0';
		macos9_html_head_len = copy;
	}
	{
		/* fixes640 - accumulate HTML tokenize CPU per chunk. fixes640a:
		 * inline <script> runs SYNCHRONOUSLY inside parse_chunk (parser
		 * script callback -> js_exec), and JS is bracketed separately, so
		 * subtract the JS that ran during this chunk to keep parse = pure
		 * tokenize and avoid inflating both parse and total. */
		extern double macos9_micros(void);
		extern void macsurf_profile_accum_parse(long us);
		extern long macsurf_profile_get_js_us(void);
		double t_parse = macos9_micros();
		long js_before = macsurf_profile_get_js_us();
		long parse_us;
		dom_ret = dom_hubbub_parser_parse_chunk(html->parser,
						      (const uint8_t *) data,
						      size);
		parse_us = (long)(macos9_micros() - t_parse)
				- (macsurf_profile_get_js_us() - js_before);
		macsurf_profile_accum_parse(parse_us);
	}

	err = libdom_hubbub_error_to_nserror(dom_ret);

	/* deal with encoding change */
	if (err == NSERROR_ENCODING_CHANGE) {
		 err = html_process_encoding_change(c, data, size);
	}

	/* broadcast the error if necessary */
	if (err != NSERROR_OK) {
		content_broadcast_error(c, err, NULL);
		return false;
	}

	return true;
}


/**
 * Convert a CONTENT_HTML for display.
 *
 * The following steps are carried out in order:
 *
 *  - parsing to an XML tree is completed
 *  - stylesheets are fetched
 *  - the XML tree is converted to a box tree and object fetches are started
 *
 * On exit, the content status will be either CONTENT_STATUS_DONE if the
 * document is completely loaded or CONTENT_STATUS_READY if objects are still
 * being fetched.
 */

static bool html_convert(struct content *c)
{
	html_content *htmlc = (html_content *) c;
	dom_exception exc; /* returned by libdom functions */

#ifdef __MACOS9__
	{
		extern void macsurf_debug_log_writef(const char *fmt, ...);
		macsurf_debug_log_writef("html_convert: entered");
	}
#endif

	/* The quirk check and associated stylesheet fetch is "safe"
	 * once the root node has been inserted into the document
	 * which must have happened by this point in the parse.
	 *
	 * faliure to retrive the quirk mode or to start the
	 * stylesheet fetch is non fatal as this "only" affects the
	 * render and it would annoy the user to fail the entire
	 * render for want of a quirks stylesheet.
	 */
	MS_LOG("html convert");
	exc = dom_document_get_quirks_mode(htmlc->document, &htmlc->quirks);
	if (exc == DOM_NO_ERR) {
		html_css_quirks_stylesheets(htmlc);
		NSLOG(netsurf, INFO, "quirks set to %d", htmlc->quirks);
	}

	htmlc->base.active--; /* the html fetch is no longer active */
	NSLOG(netsurf, INFO, "%d fetches active (%p)", htmlc->base.active, c);

	/* The parse cannot be completed here because it may be paused
	 * untill all the resources being fetched have completed.
	 */

	/* if there are no active fetches in progress no scripts are
	 * being fetched or they completed already.
	 */
	if (html_can_begin_conversion(htmlc)) {
		return html_begin_conversion(htmlc);
	}
	return true;
}

/* Exported interface documented in html_internal.h */
bool html_can_begin_conversion(html_content *htmlc)
{
	unsigned int i;

	/* fixes450: short-circuit on aborted content so convert_script_async_cb
	 * can never trigger html_begin_conversion on a zombie html_content */
	if (htmlc->aborted)
		return false;

	/* Cannot begin conversion if we're still fetching stuff */
	macsurf_debug_log_int("active fetches", (long)htmlc->base.active);
	/* fixes727 - the live fetch count no longer hijacks the title bar; the
	 * Netscape-style animated puffin throbber (right of the nav bar) now
	 * signals load activity instead. Keep the file-log line above for
	 * diagnostics. */
	if (htmlc->base.active != 0)
		return false;

	for (i = 0; i != htmlc->stylesheet_count; i++) {
		/* Cannot begin conversion if the stylesheets are modified */
		if (htmlc->stylesheets[i].modified)
			return false;
	}

	/* All is good, begin */
	return true;
}

bool
html_begin_conversion(html_content *htmlc)
{
	dom_node *html;
	nserror ns_error;
	struct form *f;
	dom_exception exc; /* returned by libdom functions */
	dom_string *node_name = NULL;
	dom_hubbub_error error;

	MS_LOG("html begin conversion");
	/* fixes848 (#167 perf investigation) -- see html_box_convert_done's
	 * comment; this is the matching "about to start" bracket. */
	macsurf_debug_log_writef("WORK pipeline: begin_conversion c=%p url=%s",
			(void *) htmlc,
			nsurl_access(content_get_url((struct content *) htmlc)));
	/* The act of completing the parse can result in additional data
	 * being flushed through the parser. This may result in new style or
	 * script nodes, upon which the conversion depends. Thus, once we
	 * have completed the parse, we must check again to see if we can
	 * begin the conversion. If we can't, we must stop and wait for the
	 * new styles/scripts to be processed. Once they have been processed,
	 * we will be called again to begin the conversion for real. Thus,
	 * we must also ensure that we don't attempt to complete the parse
	 * multiple times, so store a flag to indicate that parsing is
	 * complete to avoid repeating the completion pointlessly.
	 */
	if (htmlc->parse_completed == false) {
		NSLOG(netsurf, INFO, "Completing parse (%p)", htmlc);
		/* complete parsing */
		error = dom_hubbub_parser_completed(htmlc->parser);
		if (error == DOM_HUBBUB_HUBBUB_ERR_PAUSED) {
			/* The parse paused on a synchronous <script src>. The parser
			 * is resumed by the DEFERRED unpause (macos9_schedule_unpause
			 * -> deferred_parser_unpause, fixes512), which re-drives this
			 * conversion. PAUSED is therefore NOT a failure *as long as
			 * something will resume the parser*. There are exactly two such
			 * states, and we must wait in both:
			 *
			 *   - base.active > 0 : the pausing script's fetch is still in
			 *     flight; its completion callback (convert_script_sync_cb)
			 *     will schedule the unpause. This is the normal network case.
			 *
			 *   - htmlc->unpause_pending : the fetch already completed and the
			 *     unpause is scheduled but has not run yet. This is the
			 *     CACHE-HIT BURST: the document and its sync <script src> are
			 *     delivered in one synchronous burst with no event-loop tick
			 *     between them, so by the time this completion check runs
			 *     base.active is already 0 while the parser is still paused.
			 *
			 * fixes556 removed the old `&& base.active > 0` guard (which
			 * missed the cache-hit case and fired the error branch: PAUSED
			 * maps to NSERROR_OK(0) in libdom_hubbub_error_to_nserror, so the
			 * broadcast was CONTENT_MSG_ERROR code=0 msg=NULL ->
			 * about:query/fetcherror on a page that fetched fine, log
			 * "NAV: ERROR ... code=0 msg=(null)"). But fixes556 then waited on
			 * EVERY PAUSED, which would HANG at "Loading" forever if the parser
			 * were paused with nothing scheduled to resume it (e.g. the unpause
			 * schedule failed on OOM). fixes558 narrows it: wait only when a
			 * resume is guaranteed; otherwise the parser is genuinely stuck and
			 * we report a REAL error code instead of the spurious 0. Genuine
			 * parse failures return other (non-OK, non-PAUSED) codes and still
			 * fall through to the error branch below unchanged. */
			if (htmlc->base.active > 0 || htmlc->unpause_pending) {
				NSLOG(netsurf, INFO, "Completing parse brought synchronous JS to light, cannot complete yet");
				return true;
			}
			/* PAUSED with no fetch in flight and no unpause scheduled: the
			 * parser is genuinely stuck. Surface a real, non-zero error so the
			 * page routes to the error view with a meaningful code rather than
			 * the PAUSED->NSERROR_OK(0) that produced the spurious nav error. */
			NSLOG(netsurf, INFO, "Parser stuck PAUSED with no pending unpause");
			content_broadcast_error(&htmlc->base, NSERROR_STOPPED, NULL);
			return false;
		}
		if (error != DOM_HUBBUB_OK) {
			NSLOG(netsurf, INFO, "Parsing failed");

			content_broadcast_error(&htmlc->base,
						libdom_hubbub_error_to_nserror(error),
						NULL);

			return false;
		}
		htmlc->parse_completed = true;
		html_log_facebook_parse_fingerprint(htmlc);
	}

	/* Walk DOM for <style> and <link rel=stylesheet> once parse is
	 * done. The libdom DOMSubtreeModified / DOMNodeInserted event
	 * chain that normally triggers stylesheet pickup is unreliable
	 * in this build, so we discover them explicitly here. Any
	 * stylesheets found will bump htmlc->base.active and cause
	 * html_can_begin_conversion() to bail until they finish loading. */
	if (htmlc->stylesheets_discovered == false) {
		MS_LOG("html discover stylesheets");
		html_css_discover_stylesheets(htmlc);
		macsurf_debug_log_int("stylesheet count after discovery",
			(long)htmlc->stylesheet_count);
	}

	if (html_can_begin_conversion(htmlc) == false) {
		NSLOG(netsurf, INFO, "Can't begin conversion (%p)", htmlc);
		/* We can't proceed (see commentary above) */
		return true;
	}

	/* Give up processing if we've been aborted */
	if (htmlc->aborted) {
		NSLOG(netsurf, INFO, "Conversion aborted (%p) (active: %u)",
		      htmlc, htmlc->base.active);
		content_set_error(&htmlc->base);
		content_broadcast_error(&htmlc->base, NSERROR_STOPPED, NULL);
		return false;
	}

	/* Conversion begins proper at this point */
	htmlc->conversion_begun = true;

	/* complete script execution, including deferred scripts */
	html_script_exec(htmlc, true);

	/* fire a simple event that bubbles named DOMContentLoaded at
	 * the Document.
	 */

	/* get encoding */
	if (htmlc->encoding == NULL) {
		const char *encoding;

		encoding = dom_hubbub_parser_get_encoding(htmlc->parser,
					&htmlc->encoding_source);
		if (encoding == NULL) {
			content_broadcast_error(&htmlc->base,
						NSERROR_NOMEM,
						NULL);
			return false;
		}

		htmlc->encoding = strdup(encoding);
		if (htmlc->encoding == NULL) {
			content_broadcast_error(&htmlc->base,
						NSERROR_NOMEM,
						NULL);
			return false;
		}
	}

	/* locate root element and ensure it is html */
	exc = dom_document_get_document_element(htmlc->document, (void *) &html);
	if ((exc != DOM_NO_ERR) || (html == NULL)) {
		NSLOG(netsurf, INFO, "error retrieving html element from dom");
		content_broadcast_error(&htmlc->base, NSERROR_DOM, NULL);
		return false;
	}

	exc = dom_node_get_node_name(html, &node_name);
	if ((exc != DOM_NO_ERR) ||
	    (node_name == NULL) ||
	    (!dom_string_caseless_lwc_isequal(node_name,
			corestring_lwc_html))) {
		NSLOG(netsurf, INFO, "root element not html");
		content_broadcast_error(&htmlc->base, NSERROR_DOM, NULL);
		dom_node_unref(html);
		return false;
	}
	dom_string_unref(node_name);

	/* fixes743 (#204): flip has-no-js -> has-js (and no-js -> js) on the root
	 * <html> BEFORE the cascade. Modernizr/XenForo gate their JS-enabled
	 * styling behind a .has-js/.js class that a page script normally sets; we
	 * HAVE JS (QuickJS), so setting it here means the progressive-enhancement
	 * CSS applies on FIRST paint with no re-cascade - fixing the grey XenForo
	 * account-nav + search boxes and hiding aria-hidden dropdown menus. A
	 * single "no-js"->"js" pass handles both "has-no-js"->"has-js" and a bare
	 * "no-js"->"js" token, and only shrinks the string so clen+1 always fits. */
	{
		dom_string *class_name = NULL;
		if (dom_string_create((const uint8_t *) "class", 5, &class_name)
				== DOM_NO_ERR && class_name != NULL) {
			dom_string *cls = NULL;
			if (dom_element_get_attribute(html, class_name, &cls)
					== DOM_NO_ERR && cls != NULL) {
				const char *cdata = dom_string_data(cls);
				size_t clen = dom_string_byte_length(cls);
				if (cdata != NULL && clen >= 5 &&
				    strstr(cdata, "no-js") != NULL) {
					char *buf = (char *) malloc(clen + 1);
					if (buf != NULL) {
						size_t si = 0, di = 0;
						while (si < clen) {
							if (si + 5 <= clen &&
							    strncmp(cdata + si, "no-js", 5) == 0) {
								buf[di++] = 'j';
								buf[di++] = 's';
								si += 5;
							} else {
								buf[di++] = cdata[si++];
							}
						}
						{
							dom_string *newcls = NULL;
							if (dom_string_create(
								(const uint8_t *) buf, di, &newcls)
								== DOM_NO_ERR && newcls != NULL) {
								(void) dom_element_set_attribute(
									html, class_name, newcls);
								dom_string_unref(newcls);
							}
						}
						free(buf);
					}
				}
				dom_string_unref(cls);
			}
			dom_string_unref(class_name);
		}
	}

	/* Retrieve forms from parser */
	htmlc->forms = html_forms_get_forms(htmlc->encoding,
			(dom_html_document *) htmlc->document);
	for (f = htmlc->forms; f != NULL; f = f->prev) {
		nsurl *action;

		/* Make all actions absolute */
		if (f->action == NULL || f->action[0] == '\0') {
			/* HTML5 4.10.22.3 step 9 */
			nsurl *doc_addr = content_get_url(&htmlc->base);
			ns_error = nsurl_join(htmlc->base_url,
					      nsurl_access(doc_addr),
					      &action);
		} else {
			ns_error = nsurl_join(htmlc->base_url,
					      f->action,
					      &action);
		}

		if (ns_error != NSERROR_OK) {
			content_broadcast_error(&htmlc->base, ns_error, NULL);

			dom_node_unref(html);
			return false;
		}

		free(f->action);
		f->action = strdup(nsurl_access(action));
		nsurl_unref(action);
		if (f->action == NULL) {
			content_broadcast_error(&htmlc->base,
						NSERROR_NOMEM,
						NULL);

			dom_node_unref(html);
			return false;
		}

		/* Ensure each form has a document encoding */
		if (f->document_charset == NULL) {
			f->document_charset = strdup(htmlc->encoding);
			if (f->document_charset == NULL) {
				content_broadcast_error(&htmlc->base,
							NSERROR_NOMEM,
							NULL);
				dom_node_unref(html);
				return false;
			}
		}
	}

	dom_node_unref(html);

	if (htmlc->base.active == 0) {
		html_finish_conversion(htmlc);
	}

	return true;
}


/**
 * Stop loading a CONTENT_HTML.
 *
 * called when the content is aborted. This must clean up any state
 * created during the fetch.
 */

static void html_stop(struct content *c)
{
	html_content *htmlc = (html_content *) c;

	switch (c->status) {
	case CONTENT_STATUS_LOADING:
		/* fixes452: cancel any in-progress box walk FIRST, before
		 * setting aborted, so convert_xml_to_box cannot fire after
		 * html_destroy frees the html_content.  cancel_dom_to_box
		 * removes the scheduled entry and frees the ctx; we null the
		 * pointer so html_destroy's matching cancel is a no-op. */
		if (htmlc->box_conversion_context != NULL) {
			cancel_dom_to_box(htmlc->box_conversion_context);
			htmlc->box_conversion_context = NULL;
		}

		/* Still loading; flag abort and close the JS thread */
		htmlc->aborted = true;
		if (htmlc->js_thread != NULL) {
			js_closethread(htmlc->js_thread);
		}
		/* fixes452: abort object fetches started by the partial box
		 * walk.  Mirrors what CONTENT_STATUS_READY already does below.
		 * Without this, sub-resource hlcache handles remain live until
		 * html_destroy, letting their callbacks fire against a zombie
		 * html_content in the window between stop and destroy. */
		html_object_abort_objects(htmlc);

		/* fixes450: unregister all script handles so that
		 * llcache_catch_up_all_users driving a JS content to DONE
		 * cannot call convert_script_async_cb back into this zombie
		 * html_content.  Mirrors html_script_free but nulls the
		 * handles so html_destroy / html_script_free skips them. */
		{
			unsigned int si;
			for (si = 0; si < htmlc->scripts_count; si++) {
				struct html_script *s = &htmlc->scripts[si];
				if ((s->type == HTML_SCRIPT_SYNC ||
				     s->type == HTML_SCRIPT_ASYNC ||
				     s->type == HTML_SCRIPT_DEFER) &&
				     s->data.handle != NULL) {
					/* fixes501x: NULL before release. */
					safe_hlcache_handle_release(&s->data.handle);
				}
			}
		}
		break;

	case CONTENT_STATUS_READY:
		html_object_abort_objects(htmlc);

		/* If there are no further active fetches and we're still
		 * in the READY state, transition to the DONE state. */
		if (c->status == CONTENT_STATUS_READY && c->active == 0) {
			content_set_done(c);
		}

		break;

	case CONTENT_STATUS_DONE:
		/* Nothing to do */
		break;

	default:
		NSLOG(netsurf, INFO, "Unexpected status %d (%p)", c->status,
		      c);
		assert(0);
	}
}


/**
 * Reformat a CONTENT_HTML to a new width.
 */

/* ----------------------------------------------------------------- */
/* fixes383 (M2) - JS->DOM->render: re-convert after DOM mutation.     */
/* Rebuild the disposable box tree from the (JS-mutated, persistent)   */
/* DOM and repaint. Guards prevent the three crash hazards the design  */
/* review caught (docs/research/js-dom-render-plan.md).                */
/* ----------------------------------------------------------------- */

/* fixes899 (MacSurf) - restore the two initial-build CSS preconditions the
 * reconvert was silently violating (the real reconvert-crash root cause, found
 * by a 3-agent architecture study; Layer 1 of the reconvert-incremental work):
 *
 *  (A) Every element still carries the libcss node data (cached pass-1 partial
 *      computed styles + bloom filter) from the FIRST cascade. libcss's
 *      cross-node style-sharing optimization reads a NEIGHBOUR's cached node
 *      data mid-pass and refs its pass-1 partial styles into the pass-2 result
 *      -- but those styles are owned by the OLD box tree that
 *      html_reconvert_free_old is about to talloc_free. Combined with the arena
 *      remove-by-value bug (now identity, css__arena_remove_style), that is the
 *      PC=0x2710 UAF. Releasing every element's node data here makes pass 2
 *      rebuild fresh, exactly like the initial build (empty node data on every
 *      node). Routed through nscss_reset_node_data -> CSS_NODE_DELETED handler,
 *      so refs are UNREF'd, never leaked.
 *
 *  (B) unit_len_ctx.root_style: the initial cascade runs with it NULL
 *      (html_reformat sets it only AFTER layout), but on a reconvert it still
 *      points at the OLD, deferred-then-freed layout->style -- a dangling
 *      pointer the first pass never had. Mirror the initial NULL state.
 *
 * Recursive over DOM depth (bounded; 512 stack guard), element nodes only. */
extern int nscss_reset_node_data(dom_node *node);

static long html_reconvert_reset_node_data_walk(dom_node *node, int depth)
{
	dom_node_type type = 0;
	dom_node *child = NULL;
	dom_node *next = NULL;
	long cleared = 0;

	if (node == NULL || depth > 512)
		return 0;

	dom_node_get_node_type(node, &type);
	if (type == DOM_ELEMENT_NODE)
		cleared += (long) nscss_reset_node_data(node);

	dom_node_get_first_child(node, &child);
	while (child != NULL) {
		cleared += html_reconvert_reset_node_data_walk(child, depth + 1);
		dom_node_get_next_sibling(child, &next);
		dom_node_unref(child);
		child = next;
	}
	return cleared;
}

static long html_reconvert_reset_css_state(html_content *c)
{
	dom_node *root = NULL;
	long cleared = 0;

	/* (B) - mirror the initial-build NULL; html_reformat re-sets it after
	 * layout, so this is the correct state at cascade start. */
	c->unit_len_ctx.root_style = NULL;

	/* (A) - release every element's stale pass-1 libcss node data. */
	if (c->document != NULL &&
			dom_document_get_document_element(c->document,
				(void *) &root) == DOM_NO_ERR && root != NULL) {
		cleared = html_reconvert_reset_node_data_walk(root, 0);
		dom_node_unref(root);
	}
	return cleared;
}

/* HAZARD-1: clear the box* back-reference in every box's DOM-node user_data
 * BEFORE freeing the box tree. Nodes that skip box generation on re-convert
 * (display:none, <script>/<style>/<head>) would otherwise keep a dangling
 * box* and crash box_for_node(). Non-recursive walk of the still-intact tree. */
static void html_reconvert_clear_node_boxes(html_content *c)
{
	struct box *b = c->layout;
	struct box *root_parent = (b != NULL) ? b->parent : NULL;
	void *old = NULL;
	/* fixes889 - DIAGNOSTIC. This walk descends children/next ONLY, but a box
	 * can also be reachable solely via float_children/next_float or
	 * list_marker (box_construct sets marker->parent and box->list_marker,
	 * and never puts the marker in children). Those boxes are NOT visited
	 * here, so their nodes keep pointing at a box this reconvert is about to
	 * free -- which is what makes box_for_node() hand freed memory to
	 * link_box_for_ancestor on the next click.
	 *
	 * Count both populations. `missed` is the smoking gun: if it is non-zero
	 * on a page that crashes, the dangling backlink is confirmed. The actual
	 * retraction now happens per-box in box_talloc_destructor (fixes889), so
	 * this walk stays as-is and reports rather than pretending to be
	 * complete. */
	unsigned long walked = 0, cleared = 0, missed = 0;
	while (b != NULL) {
		walked++;
		if (b->node != NULL) {
			cleared++;
			(void) dom_node_set_user_data(b->node,
					corestring_dom___ns_key_box_node_data,
					NULL, NULL, &old);
		}
		if (b->list_marker != NULL && b->list_marker->node != NULL)
			missed++;
		{
			struct box *fl = b->float_children;
			while (fl != NULL) {
				if (fl->node != NULL) missed++;
				fl = fl->next_float;
			}
		}
		if (b->children != NULL) {
			b = b->children;
			continue;
		}
		while (b != NULL && b->next == NULL) {
			if (b->parent == root_parent) { b = NULL; break; }
			b = b->parent;
		}
		if (b != NULL) b = b->next;
	}
	macsurf_debug_log_writef(
		"WORK reconvert H1: walked=%ld cleared=%ld MISSED(float/marker)=%ld"
		" -- missed>0 means those nodes kept a dangling box* before fixes889",
		(long) walked, (long) cleared, (long) missed);
}

/* HAZARD-3: null the box* backpointer on every form control so a click or
 * submit between the box-free and re-convert-done cannot deref a freed box. */
static void html_reconvert_detach_forms(html_content *c)
{
	struct form *f;
	struct form_control *fc;
	for (f = c->forms; f != NULL; f = f->prev) {
		for (fc = f->controls; fc != NULL; fc = fc->next) {
			fc->box = NULL;
		}
	}
}

/* Form controls are normally discovered just once after parsing. Script-made
 * forms and controls arrive later, so a reconvert needs a fresh registry for
 * its replacement boxes. Keep the old registry until success: its controls
 * still belong to the retained old tree if conversion has to roll back. */
static void html_reconvert_free_forms(struct form *forms)
{
	struct form *prev;

	while (forms != NULL) {
		prev = forms->prev;
		form_free(forms);
		forms = prev;
	}
}

static bool html_reconvert_rebuild_forms(html_content *c)
{
	struct form *f;
	nsurl *action = NULL;
	nserror error;

	if (c == NULL || c->document == NULL)
		return false;

	c->forms = html_forms_get_forms(c->encoding,
			(dom_html_document *) c->document);
	for (f = c->forms; f != NULL; f = f->prev) {
		/* Keep dynamic forms subject to the same absolute-action rule as
		 * parser-time forms. */
		if (f->action == NULL || f->action[0] == '\0') {
			nsurl *doc_addr = content_get_url(&c->base);
			/* A manually constructed content (the deterministic harness) has
			 * no document URL. Keep an empty action rather than asserting in
			 * nsurl_access; real fetched pages always take the join below. */
			if (c->base_url == NULL || doc_addr == NULL) {
				free(f->action);
				f->action = strdup("");
				if (f->action == NULL)
					goto failed;
				continue;
			}
			error = nsurl_join(c->base_url, nsurl_access(doc_addr),
					&action);
		} else {
			error = nsurl_join(c->base_url, f->action, &action);
		}
		if (error != NSERROR_OK || action == NULL)
			goto failed;

		free(f->action);
		f->action = strdup(nsurl_access(action));
		nsurl_unref(action);
		action = NULL;
		if (f->action == NULL)
			goto failed;

		if (f->document_charset == NULL) {
			f->document_charset = strdup(c->encoding);
			if (f->document_charset == NULL)
				goto failed;
		}
	}

	return true;

failed:
	if (action != NULL)
		nsurl_unref(action);
	html_reconvert_free_forms(c->forms);
	c->forms = NULL;
	return false;
}

/* fixes843 (#167 S2) - pin the OLD tree's text-node dom_strings across the
 * teardown+rebuild window, the same way fixes421 already defers the OLD box
 * CONTEXT to protect shared/interned CSS styles. The gap fixes421 left open:
 * a JS mutation (React/FB feed churn, or a XenForo script) can drop a live
 * text node's dom_string to refcount 0 (a normal, correct DOM operation)
 * WHILE the new dom_to_box walk is still mid-tree (it self-reschedules every
 * 100 nodes through the cooperative scheduler and can be paused for several
 * event-loop passes on a slow Mac). If that freed block gets reused before
 * the new walk reaches the node, box_construct_text's read of the recycled
 * memory paints garbage (observed: an interned attribute name leaking in as
 * page text). Taking an extra ref here on every OLD BOX_TEXT box's CURRENT
 * dom_string keeps that exact memory from being freed-and-reused during the
 * window; the extra refs are released in html_reconvert_free_old(), same
 * lifetime as the deferred bctx. Iterative walk (box trees run to
 * thousands of boxes on real pages; NetSurf's own conversion walk is
 * iterative for the same reason -- see convert_xml_to_box_inner). */
struct macsurf_pinned_string {
	dom_string *str;
	struct macsurf_pinned_string *next;
};

static struct macsurf_pinned_string *g_reconvert_pinned_strings = NULL;

/* fixes896 - pin the DOM's live text-node dom_strings across the reconvert
 * teardown+rebuild window. Returns the count pinned.
 *
 * fixes843 INTENDED exactly this but walked the BOX tree for `BOX_TEXT` boxes
 * with `b->node != NULL` -- and box_construct NEVER sets `->node` on a text box
 * (only on ELEMENT boxes, box_construct.c:1438, `box->node = dom_node_ref`).
 * So the condition was never true: the walk matched nothing and pinned ZERO
 * strings on EVERY page since the day it was written (HW fixes895 confirmed:
 * "pinned 0 old text-node strings" on a 1251-box hackaday tree). That left the
 * fixes489 text-node dom_string UAF completely unprotected -- which is the
 * reconvert crash inside dom_to_box: box_construct_text's
 * `dom_characterdata_get_data(ctx->n)` reads a text node's dom_string that a JS
 * timer firing between the reconvert's cooperative yields dropped to refcount 0
 * and the allocator reused (freemem was healthy on the crashing run, so it is
 * this UAF, not memory exhaustion).
 *
 * The fix walks the DOM itself -- the actual source box_construct_text reads --
 * and takes an extra ref on every text/CDATA node's CURRENT dom_string, so that
 * exact memory cannot be freed+reused until html_reconvert_free_old releases the
 * pins after the new tree is live. Recursive over DOM DEPTH (bounded on real
 * pages; a defensive cap stops a pathological nesting from overrunning the
 * stack), not the box tree's breadth. */
static long html_reconvert_pin_dom_node(dom_node *node, int depth)
{
	dom_node_type type = 0;
	dom_node *child = NULL;
	dom_node *next = NULL;
	long pinned = 0;

	if (node == NULL)
		return 0;
	if (depth > 512)
		return 0;   /* pathological-nesting stack guard */

	dom_node_get_node_type(node, &type);
	if (type == DOM_TEXT_NODE || type == DOM_CDATA_SECTION_NODE) {
		dom_string *ds = NULL;
		dom_exception exc = dom_characterdata_get_data(
				(dom_characterdata *) node, &ds);
		if (exc == DOM_NO_ERR && ds != NULL) {
			struct macsurf_pinned_string *p = malloc(sizeof(*p));
			if (p != NULL) {
				/* adopts the ref get_data returned */
				p->str = ds;
				p->next = g_reconvert_pinned_strings;
				g_reconvert_pinned_strings = p;
				pinned++;
			} else {
				dom_string_unref(ds);
			}
		}
	}

	dom_node_get_first_child(node, &child);
	while (child != NULL) {
		pinned += html_reconvert_pin_dom_node(child, depth + 1);
		dom_node_get_next_sibling(child, &next);
		dom_node_unref(child);
		child = next;
	}
	return pinned;
}

static long html_reconvert_pin_text_strings(dom_document *doc)
{
	dom_node *root = NULL;
	long pinned;

	if (doc == NULL)
		return 0;
	if (dom_document_get_document_element(doc, (void *) &root) != DOM_NO_ERR
			|| root == NULL)
		return 0;
	pinned = html_reconvert_pin_dom_node(root, 0);
	dom_node_unref(root);
	return pinned;
}

static void html_reconvert_release_pinned_strings(void)
{
	struct macsurf_pinned_string *p = g_reconvert_pinned_strings;
	while (p != NULL) {
		struct macsurf_pinned_string *next = p->next;
		dom_string_unref(p->str);
		free(p);
		p = next;
	}
	g_reconvert_pinned_strings = NULL;
}

/* ====================================================================== */
/* fixes1095 (#265 Round C1) - WHERE THE 1.7 SECONDS GOES.
 *
 * Round B made the synchronous flush legal before DONE and hardware answered:
 * `JSSYNC flush=2 declined=1247 us=3353022` -- it fires, it works
 * (JSGEOMANS real went 2 -> 100), and it costs ~1.7s PER FLUSH, which blew the
 * 2s budget and turned into 522 further declines. Cost, not safety, is now the
 * blocker for Round C.
 *
 * The cost is NOT cascade or layout: PERFACC for the whole navigation reads
 * cascade=1.34s layout=0.81s (2.15s total) while two flushes alone cost 3.35s.
 * It is the reconvert's own teardown and box construction -- roughly ten
 * O(document) passes. Which one dominates decides whether Round C is "make a
 * flush cheaper" or "make flushes rarer", and those want opposite work.
 *
 * A census of mutation KINDS already refuted the obvious guess (a layout-only
 * fast path for class/style-only batches): hardware shows structural=1827 vs
 * cosmetic=903, with only 1 of 6 batches purely cosmetic. So the next guess is
 * not worth shipping either. Time the phases instead -- this is the fixes986
 * lesson, where a real timer inside the thing settled in one round what three
 * rounds of log-reading could not.
 *
 * Cumulative per navigation, emitted once, so a 5-reconvert page yields one
 * comparable line rather than 5x9 noise. Reset per navigation alongside the
 * other LIFE counters. */
#define RECONV_PH_H1        0	/* clear stale node->box backlinks   */
#define RECONV_PH_H3        1	/* forms + imagemap + selection      */
#define RECONV_PH_CSS       2	/* release pass-1 libcss node data   */
#define RECONV_PH_PIN       3	/* pin live text-node dom_strings    */
#define RECONV_PH_BUILD     4	/* dom_to_box (synchronous, fixes903) */
#define RECONV_PH_RELINK    5	/* re-attach carried objects          */
#define RECONV_PH_FREEOLD   6	/* drop the previous box tree         */
#define RECONV_PH_REFORMAT  7	/* content__reformat inside _done     */
#define RECONV_PH_N         8

static long g_reconv_ph_us[RECONV_PH_N];
static long g_reconv_ph_n = 0;

static const char *g_reconv_ph_name[RECONV_PH_N] = {
	"h1", "h3", "css", "pin", "build", "relink", "freeold", "reformat"
};

/* Sampled with the SAME clock every other MacSurf timer uses. macos9_micros
 * returns double -- an integer extern here would read xmm0 as rax and produce
 * garbage, which is exactly the fixes1070 harness bug. */
static double html_reconv_now(void)
{
	extern double macos9_micros(void);
	return macos9_micros();
}

static void html_reconv_ph_add(int ph, double t0)
{
	if (ph < 0 || ph >= RECONV_PH_N)
		return;
	g_reconv_ph_us[ph] += (long)(html_reconv_now() - t0);
}

void html_reconvert_phase_reset(void);
void html_reconvert_phase_reset(void)
{
	int i;
	for (i = 0; i < RECONV_PH_N; i++)
		g_reconv_ph_us[i] = 0;
	g_reconv_ph_n = 0;
}

/* fixes1095 - read the phase accumulators. Mirrors macos9_reconvert_sync_stats,
 * which exists for the same reason: a counter only anyone can READ can be
 * asserted on, and an instrument nothing asserts on is how a false green
 * survives (N_ELEMENTS, foreground_images, the fixes1070 clock). */
void html_reconvert_phase_stats(int ph, long *us, long *n);
void html_reconvert_phase_stats(int ph, long *us, long *n)
{
	if (us != NULL)
		*us = (ph >= 0 && ph < RECONV_PH_N) ? g_reconv_ph_us[ph] : -1;
	if (n != NULL)
		*n = g_reconv_ph_n;
}

void html_reconvert_phase_report(void);
void html_reconvert_phase_report(void)
{
	char line[224];
	int i;
	int pos = 0;
	long total = 0;

	if (g_reconv_ph_n == 0)
		return;			/* no reconvert this navigation */

	for (i = 0; i < RECONV_PH_N; i++)
		total += g_reconv_ph_us[i];

	/* Hand-rolled: macsurf_debug_log_writef caps at 255 bytes and supports a
	 * fixed specifier set, so build the variable part here and emit once. */
	for (i = 0; i < RECONV_PH_N; i++) {
		const char *nm = g_reconv_ph_name[i];
		long v = g_reconv_ph_us[i];
		int k;
		char num[16];
		int nl = 0;

		if (pos > (int)sizeof(line) - 24)
			break;
		for (k = 0; nm[k] != '\0' && pos < (int)sizeof(line) - 2; k++)
			line[pos++] = nm[k];
		line[pos++] = '=';
		if (v == 0) {
			num[nl++] = '0';
		} else {
			long q = v;
			char rev[16];
			int rl = 0;
			while (q > 0 && rl < 15) { rev[rl++] = (char)('0' + (q % 10)); q /= 10; }
			while (rl > 0 && nl < 15) num[nl++] = rev[--rl];
		}
		for (k = 0; k < nl && pos < (int)sizeof(line) - 2; k++)
			line[pos++] = num[k];
		if (i + 1 < RECONV_PH_N && pos < (int)sizeof(line) - 2)
			line[pos++] = ' ';
	}
	line[pos] = '\0';

	macsurf_debug_log_writef("LIFE RECONVPHASE n=%ld total=%ldus %s",
			g_reconv_ph_n, total, line);
}
/* ====================================================================== */

/* Dedicated re-convert completion. Unlike html_box_convert_done it does NOT
 * destroy the parser (already gone) or re-fire content_set_ready /
 * proceed_to_done (the content is already DONE). It re-extracts image maps
 * and relayouts the fresh box tree (same path image-load completion uses). */
/* fixes421 DOUBLE-BUFFER: the old box tree's talloc context, kept alive
 * across dom_to_box so the re-cascade can share already-interned styles
 * (refcount++) instead of free-then-reintern (use-after-free in the arena).
 * Freed in html_reconvert_done after the new tree + reformat are live.
 * Single-window browser => one re-convert at a time; file-scope is safe. */
static void *g_reconvert_old_bctx = NULL;
/* A reconvert is transactional: until the new tree completes, this is the
 * rendered tree that must return if construction fails. */
static struct box *g_reconvert_old_layout = NULL;
static struct content_html_iframe *g_reconvert_old_iframe = NULL;
static struct form *g_reconvert_old_forms = NULL;

#ifdef MACSURF_RECONVERT_TEST_HOOK
/* Harness-only deterministic failure injection. */
static int g_reconvert_test_fail_once = 0;

void macsurf_reconvert_test_fail_once(void)
{
	g_reconvert_test_fail_once = 1;
}
#endif

/* fixes889 - reconvert sequence + the layout pointer each one installed.
 * The click crash (box_for_node -> freed box -> illegal instruction) and a
 * reconvert are invisible to each other in the log today, so there is no way
 * to tell from a crash report whether a reconvert had just swapped the tree
 * out from under the click. These two make that correlation readable:
 * interaction.c stamps them on every click. */
unsigned long macsurf_reconvert_seq = 0;
void *macsurf_reconvert_last_layout = NULL;

/* fixes895 - the reconvert-crash hunt gate. Non-zero ONLY between the START of
 * a re-convert and html_reconvert_done, so the dense per-node breadcrumbs in
 * box_construct.c's shared dom_to_box path fire during a reconvert but NOT on
 * every cold page load (which uses the same convert_xml_to_box_inner). Read via
 * extern in box_construct.c. */
int macsurf_reconvert_in_progress = 0;

/* fixes1127 - reentrancy depth for html_reconvert() itself, DISTINCT from
 * macsurf_reconvert_in_progress above. That flag is deliberately cleared
 * (line ~3228, in html_reconvert_done) BEFORE the synchronous resize/load JS
 * fire, for reasons unrelated to this guard (JS-freeze gating in
 * macsurf_qjs_pump_all, death-row drain suppression) -- so it cannot be
 * reused to detect a NESTED html_reconvert() call triggered from inside that
 * resize/load handler. This counter is incremented only once html_reconvert()
 * has passed its other busy guards and committed to a rebuild, and stays
 * non-zero for the entire synchronous span through html_reconvert_done
 * (dom_to_box's callback), so a reentrant call arriving from page JS run
 * during that span sees depth > 0 and defers instead of touching the shared
 * single-instance state (g_walk_content/g_walk_gen in box_construct.c,
 * g_reconvert_old_bctx, c->box_conversion_context) a second time. */
static int g_html_reconvert_depth = 0;

/* fixes895 - durable "furthest position" marker + phase-scoped eager flush,
 * defined in macsurf_debug_log.c (local extern, matching this file's existing
 * `extern void macsurf_debug_log_writef` pattern). */
extern void macsurf_debug_log_reconv_flush(int on);
extern void macsurf_reconv_pos_set(const char *phase, long seq, long node_ix,
		const char *tag);
extern void macsurf_reconv_pos_flush(void);
extern long macsurf_free_mem(void);

/*
 * fixes910 (Phase 0) - opaque dom_node refcount shims for the macos9 frontend's
 * reconvert pending table.
 *
 * The frontend records WHICH node a JS binding mutated so later phases can do
 * less work than a full rebuild. It holds that node across the ~400ms debounce,
 * and JS may remove and free the node inside that window - so the reference is
 * mandatory, not defensive. Storing a raw dom_node* there would be the same
 * stale-pointer shape as the box_for_node and iframe->box crashes.
 *
 * These live here rather than in the frontend because frontends/macos9 carries a
 * STUB dom/dom.h that shadows the real header under CW8's access paths; the
 * frontend therefore keeps the node as void* and never dereferences it.
 */
void *macsurf_reconvert_node_ref(void *n)
{
	if (n == NULL) {
		return NULL;
	}
	return (void *) dom_node_ref((dom_node *) n);
}

void macsurf_reconvert_node_unref(void *n)
{
	if (n != NULL) {
		dom_node_unref((dom_node *) n);
	}
}

/* fixes921 - runtime kill switch for the re-link (step 4). 0 restores the old
 * free-and-re-fetch behaviour exactly -- but still through
 * html_object_free_objects, so the fallback can never lose the fixes429/501x
 * teardown ordering. */
static int g_object_relink_enabled = 1;

/**
 * fixes921 - re-attach surviving image objects to the rebuilt box tree.
 *
 * Called from html_reconvert_done AFTER dom_to_box and BEFORE
 * html_reconvert_free_old, i.e. while the old tree is still alive, so each
 * entry's old box still carries the DOM node we key on. Replaces the blanket
 * html_object_free_objects the reconvert used to do at H2, which is what made
 * every reconvert lose its images (box->object goes NULL and layout only sizes
 * an <img> at `if (b->object && !(b->flags & REPLACE_DIM))`).
 *
 * DELIBERATELY static, and deliberately retires through the EXISTING
 * html_object_free_objects rather than a new helper: fixes918 and fixes921's
 * first cut both died on `undefined: <new symbol>` at link time. In this build
 * a new cross-TU symbol is a liability, so the partition below hands the
 * doomed entries to a function html.c already calls -- which is also exactly
 * the teardown we want (cancel the refresh schedule, NULL box->object before
 * release per fixes429, release the handle to DETACH the hlcache user and
 * disarm its callback per fixes501x, then free). No new symbol, and one
 * teardown implementation.
 *
 * Safe to leave stale object->box across the rebuild window ONLY because the
 * reconvert build is SYNCHRONOUS (fixes903): no event-loop turn happens
 * between H2 and here, so no hlcache callback can fire and dereference one.
 */
/* fixes1094 (#265 Round B) - is there an in-flight object this reconvert would
 * FREE?
 *
 * The fixes421 guard blocks a reconvert whenever `base.active > 0`, on the
 * grounds that html_object_callback holds a pw into entries
 * html_object_free_objects is about to free. That was true when a reconvert
 * freed the object list wholesale. Since fixes921/972/976 it does not:
 * html_reconvert_relink_objects PARTITIONS the list and retires only the
 * doomed entries.
 *
 * Harness Test 56 drives the real reconvert over an in-flight image and finds
 * it CARRIED, not freed -- so blocking on it protects nothing. What IS still
 * dropped, in flight or not, is a non-CONTENT_IMAGE entry (<object>/<embed>/
 * applet are CONTENT_ANY). That, and only that, is the hazard the guard has
 * left to cover.
 *
 * This predicate MUST mirror the drop condition in the partition below. It is
 * deliberately defined immediately above it so the two are read together; if
 * that condition changes, this one changes with it. It is intentionally
 * CONSERVATIVE: `content != NULL` means "a handle exists", which is a superset
 * of "a fetch is outstanding", so a completed-but-unreleased non-image entry
 * also defers. Deferring an unnecessary reconvert costs a frame; freeing a
 * live callback's entry costs a crash. */
static int html_reconvert_has_droppable_inflight(html_content *c)
{
	struct content_html_object *o;
	int n = 0;

	if (c == NULL)
		return 0;
	for (o = c->object_list; o != NULL; o = o->next) {
		/* Mirrors the drop branch of html_reconvert_relink_objects. */
		if (g_object_relink_enabled == 0 ||
		    o->permitted_types != CONTENT_IMAGE) {
			if (o->content != NULL)
				return 1;
		}
		if (++n > 4096)		/* corrupt/cyclic list: fail safe */
			return 1;
	}
	return 0;
}

static void html_reconvert_relink_objects(html_content *c)
{
	struct content_html_object *keep = NULL;
	struct content_html_object *drop = NULL;
	struct content_html_object *o;
	unsigned int n_keep = 0;
	int relinked = 0, inflight = 0, retired = 0;
	/* fixes928 - PARTITION the retirements. relinked=0/retired=12 on
	 * macintoshgarden and relinked=0/retired=80 on hackaday, while 68kmla
	 * relinked 12 in the same session, is the squish -- but "retired" alone
	 * cannot say WHICH branch dropped them, and the two candidates want
	 * opposite fixes: the scope gate rejecting real <img> (fix the gate) vs
	 * box_for_node not resolving (fix the ordering/keying). Reading settled
	 * neither: image_types IS CONTENT_IMAGE, so the gate should pass, and
	 * nothing NULLs o->box, so the key should be readable. Four counters on
	 * the line already being emitted -- no new lines, no I/O. */
	int rt_disabled = 0, rt_nonimage = 0, rt_background = 0;
	int rt_nobox = 0, rt_nonode = 0, rt_unresolved = 0;
	int rt_dedup = 0;   /* fixes973b - duplicate display slot collapsed */
	int kept_nobox = 0; /* fixes976 - box-less entries CARRIED, not retired */

	if (c == NULL) {
		return;
	}

	o = c->object_list;
	while (o != NULL) {
		/* read next BEFORE re-linking o into either list */
		struct content_html_object *next = o->next;
		struct box *newbox = NULL;
		struct dom_node *node = NULL;

		/* SCOPE: images only. <object>/<embed>/applet/generated content /
		 * list-style-image / CSS backgrounds carry object_params, which is
		 * talloc'd against the OLD bctx and read back through
		 * object->box->object_params, so they must not outlive it.
		 * permitted_types is set at fetch time and needs no content deref,
		 * so in-flight entries classify correctly too. */
		/* fixes972 (lifecycle Stage 1) - backgrounds are no longer
		 * retired here. The old gate excluded o->background on the theory
		 * (comment above) that backgrounds carry object_params talloc'd
		 * against the OLD bctx. That is false for CSS backgrounds:
		 * box->object_params is set only by box_embed / box_object, i.e.
		 * <embed>/<object>, which are CONTENT_ANY and so are already
		 * retired by the permitted_types check below. A CSS background is
		 * CONTENT_IMAGE with object_params == NULL, so it is safe to relink
		 * -- and retiring it was the single largest class of the disappear
		 * bug (background=70 of 101 on hackaday). It now falls through to
		 * box re-resolution and attaches to newbox->background. */
		if (g_object_relink_enabled == 0 ||
		    o->permitted_types != CONTENT_IMAGE) {
			if (g_object_relink_enabled == 0) rt_disabled++;
			else rt_nonimage++;
			o->next = drop; drop = o; retired++;
			o = next;
			continue;
		}

		/* The key is already on the OLD box -- no extra state needed.
		 * box->node is set for every element box, and box_for_node is an
		 * O(1) DOM user-data read that dom_to_box has just repointed at
		 * the NEW box. */
		if (o->box != NULL) {
			node = o->box->node;
		}
		newbox = (node != NULL) ? box_for_node(node) : NULL;

		if (newbox == NULL) {
			if (o->box == NULL && o->content != NULL) {
				/* fixes976 (lifecycle Stage 1) - KEEP a box-less
				 * entry instead of retiring it.
				 *
				 * These are speculative fetches
				 * (html_process_inserted_img, box == NULL) whose
				 * image box construction DEFERRED to the lazy
				 * viewport queue rather than fetching -- the
				 * hardware log for fixes975 shows exactly that
				 * shape: 87 images constructed, 30 eager, 57
				 * lazy, and every un-adopted speculative URL is
				 * one of the deferred ones. Retiring them threw
				 * away a fully fetched image that the lazy drain
				 * was about to ask for again, so scrolling paid
				 * the network cost a second time.
				 *
				 * Keeping them is BOUNDED, which is what makes
				 * this safe without the reaper: fixes975's
				 * creation-time dedupe refuses to start a second
				 * speculative fetch for a URL any entry already
				 * holds, so there can never be more than one
				 * box-less entry per distinct URL in a document
				 * (27 on hackaday's front page). They cannot
				 * accumulate per reconvert, which was the whole
				 * reason retirement existed. The lazy drain then
				 * ADOPTS one the moment its image scrolls into
				 * view (it fetches with a box, which is
				 * html_fetch_object's adoption case), so the
				 * image appears immediately instead of starting
				 * a fresh fetch.
				 *
				 * Counted separately, not as `nobox`: `retired`
				 * and its partition must keep meaning
				 * "destroyed", or the acceptance criterion stops
				 * measuring anything. A box-less entry whose
				 * handle is gone (content == NULL, i.e. its
				 * fetch errored) is NOT kept -- it can never be
				 * adopted, so it is pure dead weight, and
				 * excluding it keeps the bound tight. */
				kept_nobox++;
				o->next = keep; keep = o; n_keep++;
				o = next;
				continue;
			}
			if (node == NULL) rt_nonode++;
			else rt_unresolved++;
			/* Node gone, or normalise merged/dropped its box. Cannot be
			 * linked and must not be left alone: an in-flight entry
			 * still has an armed callback. Retiring releases the handle,
			 * which disarms it. */
			o->next = drop; drop = o; retired++;
			o = next;
			continue;
		}

		o->box = newbox;

		/* fixes972/973b (lifecycle Stage 1) - DEDUPE, preferring the
		 * DECODED copy, so keeping objects neither leaks nor twitches.
		 *
		 * A reconvert re-runs box construction, which calls
		 * html_fetch_object again for every image/background in the new
		 * tree and PREPENDS the new (in-flight, re-fetching) objects onto
		 * object_list; the old (DONE, already-decoded) objects are at the
		 * tail. A box holds at most one foreground object and one
		 * background, so (box, background-flag) is a display slot, and two
		 * objects for one slot must collapse to one -- otherwise keeping
		 * them all trades disappearance for an unbounded list (Stage 0a).
		 *
		 * fixes972 kept the FIRST seen (the new in-flight head) and
		 * retired the DONE tail. That is exactly backwards and caused the
		 * hardware "twitch": every reconvert threw away the decoded image
		 * and blanked the box until the redundant re-fetch completed.
		 * fixes973b keeps whichever copy is DONE: if the just-arrived o is
		 * DONE and the already-kept k is not, unlink and retire k (its
		 * redundant in-flight fetch is aborted when its handle releases --
		 * the same established path the nobox retirements use) and let o
		 * proceed as the linked survivor. Otherwise retire o. The decoded
		 * image never leaves the box, and the double-fetch is cancelled.
		 *
		 * O(n^2) over the kept list; n is one document's object count
		 * (~100), once per reconvert -- not a hot path. */
		{
			struct content_html_object *k = NULL, *kprev = NULL;
			struct content_html_object *kk, *kkprev = NULL;
			for (kk = keep; kk != NULL; kkprev = kk, kk = kk->next) {
				if (kk->box == newbox &&
				    (kk->background ? 1 : 0) ==
				    (o->background ? 1 : 0)) {
					k = kk; kprev = kkprev; break;
				}
			}
			if (k != NULL) {
				int o_done = (o->content != NULL &&
					content_get_status(o->content) ==
						CONTENT_STATUS_DONE);
				int k_done = (k->content != NULL &&
					content_get_status(k->content) ==
						CONTENT_STATUS_DONE);
				/* fixes977 - prefer-DONE is only right when both
				 * copies are the SAME image. If the URLs differ,
				 * the element's image CHANGED (JS rewrote src or
				 * the background), and the list is ordered
				 * new-first, so k is this rebuild's copy and o is
				 * the stale one. Preferring DONE there keeps
				 * showing the old image forever: every reconvert
				 * finds the old copy DONE and the new one still
				 * in flight, so the new one never wins. Latent
				 * since fixes973b and only reachable through this
				 * branch, which fixes977's node-keyed adoption
				 * now leaves as the changed-URL case alone --
				 * i.e. exactly the case the rule got wrong.
				 * o->url is NULL only on a hand-built entry
				 * (harness); treat that as "same" and keep the
				 * old behaviour. */
				int same_url = (o->url == NULL || k->url == NULL ||
					o->url == k->url ||
					nsurl_compare(o->url, k->url,
						NSURL_COMPLETE) != false);
				if (same_url == 0) {
					/* image changed: k (the rebuild's copy)
					 * wins regardless of who is decoded. */
					o->next = drop; drop = o;
					retired++; rt_dedup++;
					o = next;
					continue;
				}
				if (o_done && !k_done) {
					/* o (decoded) wins: unlink k from keep
					 * and retire its redundant in-flight
					 * fetch; o falls through and links below. */
					if (kprev != NULL)
						kprev->next = k->next;
					else
						keep = k->next;
					n_keep--;
					k->next = drop; drop = k;
					retired++; rt_dedup++;
				} else {
					/* k is at least as good: retire o. */
					o->next = drop; drop = o;
					retired++; rt_dedup++;
					o = next;
					continue;
				}
			}
		}

		if (o->content != NULL &&
		    content_get_status(o->content) == CONTENT_STATUS_DONE) {
			/* Completed: link it, so layout sizes the box from the
			 * object's intrinsic dimensions instead of leaving it at the
			 * line-height. Mirrors html_object_done's LINK half only --
			 * its first-completion bookkeeping (the base.active
			 * decrement) must not be repeated here. */
			struct box *b;

			if (o->background) {
				/* fixes972 - a background attaches to the box's
				 * background slot, not its object slot, and needs
				 * none of the replaced-box (table/dim/clone)
				 * handling below, which is specific to <img>-style
				 * replaced content occupying the box itself. */
				newbox->background = o->content;
			} else {
				newbox->object = o->content;
				if (newbox->type == BOX_TABLE) {
					newbox->type = BOX_BLOCK;
				}
				if (!(newbox->flags & REPLACE_DIM)) {
					for (b = newbox; b != NULL; b = b->parent) {
						b->max_width = UNKNOWN_MAX_WIDTH;
					}
					while (newbox->next != NULL &&
					       (newbox->next->flags & CLONE)) {
						newbox->next = newbox->next->next;
					}
				}
			}
			relinked++;
		} else {
			/* Still in flight: box re-resolved so the eventual
			 * html_object_done writes into a LIVE box, but completion
			 * still owns box->object. Its reflow is guaranteed by
			 * fixes916. */
			inflight++;
		}

		o->next = keep; keep = o; n_keep++;
		o = next;
	}

	/* Retire the doomed entries through the existing teardown, then restore
	 * the survivors. num_objects has never been decremented anywhere, so set
	 * it from the surviving count rather than trusting it. */
	c->object_list = drop;
	(void) html_object_free_objects(c);
	c->object_list = keep;
	c->num_objects = n_keep;

	if (relinked || inflight || retired) {
		/* fixes928 - the why= partition sums to retired= by construction.
		 * Which term is non-zero picks the fix: nonimage/background means
		 * the SCOPE gate is rejecting real images; unresolved means
		 * box_for_node cannot find the new box (ordering or keying);
		 * nobox/nonode means the entry lost its key before we got here. */
		macsurf_debug_log_writef(
			"LIFE objects relinked=%d inflight=%d retired=%d"
			" spec=%d"
			" why: disabled=%d nonimage=%d background=%d"
			" nobox=%d nonode=%d unresolved=%d dedup=%d",
			relinked, inflight, retired, kept_nobox,
			rt_disabled, rt_nonimage, rt_background,
			rt_nobox, rt_nonode, rt_unresolved, rt_dedup);
	}
}


static void html_reconvert_free_old(void)
{
	extern unsigned long macsurf_box_backlink_cleared;
	unsigned long before;

	html_reconvert_release_pinned_strings();
	if (g_reconvert_old_bctx != NULL) {
		MS_LOG("reconvert: FREE old tree");
		/* fixes889 - THE window that matters. This frees the OLD box tree
		 * AFTER dom_to_box has already built the new one, so every box
		 * destructed here belongs to a dead tree while the DOM nodes now
		 * point at LIVE boxes.
		 *
		 * box_talloc_destructor only retracts a node's backlink when it
		 * still points at the box being freed (`cur == b`), so a non-zero
		 * delta here means those nodes were STILL pointing at old, about-to-
		 * be-freed boxes at this moment -- i.e. html_reconvert_clear_node_boxes
		 * missed them, which is exactly the dangling pointer box_for_node()
		 * would have handed to the next click. Pair this number with the
		 * "MISSED(float/marker)=N" line from H1. */
		before = macsurf_box_backlink_cleared;
		/* fixes907 -- name this free so a TALLOC_ABORT during it is
		 * attributable; log the root so it correlates with the
		 * content-destroy free-layout bctx=%p on nav-away. */
		{
			extern const char *macsurf_talloc_free_ctx;
			macsurf_debug_log_writef(
				"TALLOC-INFO reconvert-free old_bctx=%p",
				(void *)g_reconvert_old_bctx);
			macsurf_talloc_free_ctx = "reconvert-old-tree";
			talloc_free(g_reconvert_old_bctx);
			macsurf_talloc_free_ctx = "(none)";
		}
		g_reconvert_old_bctx = NULL;
		macsurf_debug_log_writef(
			"WORK reconvert: old tree freed -- %ld stale backlinks retracted"
			" during the free (non-zero = H1 missed them; those nodes would"
			" have handed a FREED box to box_for_node)",
			(long) (macsurf_box_backlink_cleared - before));
	}
	if (g_reconvert_old_forms != NULL) {
		html_reconvert_free_forms(g_reconvert_old_forms);
		g_reconvert_old_forms = NULL;
	}
	g_reconvert_old_layout = NULL;
	g_reconvert_old_iframe = NULL;
}

/* Restore the persistent DOM's box links after discarding a failed
 * replacement tree. Float and marker boxes are explicit because they are not
 * reliably reachable through children/next alone. */
static void html_reconvert_restore_box_links(struct box *b, int depth,
		unsigned long *rebound)
{
	struct box *fl;
	void *old = NULL;

	if (b == NULL || depth > 512)
		return;

	while (b != NULL) {
		if (b->node != NULL) {
			(void) dom_node_set_user_data(b->node,
					corestring_dom___ns_key_box_node_data,
					b, NULL, &old);
			(*rebound)++;
		}
		if (b->gadget != NULL)
			b->gadget->box = b;

		if (b->list_marker != NULL) {
			html_reconvert_restore_box_links(b->list_marker,
					depth + 1, rebound);
		}

		/* A float is normally also in the regular tree, but revisiting it
		 * is harmless and covers a detached float subtree as well. */
		for (fl = b->float_children; fl != NULL; fl = fl->next_float) {
			html_reconvert_restore_box_links(fl, depth + 1, rebound);
		}

		if (b->children != NULL) {
			html_reconvert_restore_box_links(b->children,
					depth + 1, rebound);
		}
		b = b->next;
	}
}

/* A failed reconvert must preserve the last fully rendered tree. The old
 * failure path freed it after c->layout was cleared, blanking the document. */
static void html_reconvert_rollback(html_content *c)
{
	void *new_bctx;
	unsigned long rebound = 0;

	if (c == NULL)
		return;

	new_bctx = c->bctx;
	c->bctx = NULL;
	c->layout = NULL;
	c->iframe = NULL;

	/* The partial tree owns its boxes, styles, and partial iframe list. */
	if (new_bctx != NULL) {
		extern const char *macsurf_talloc_free_ctx;
		macsurf_talloc_free_ctx = "reconvert-failed-new-tree";
		talloc_free(new_bctx);
		macsurf_talloc_free_ctx = "(none)";
	}

	html_reconvert_release_pinned_strings();
	html_reconvert_free_forms(c->forms);
	c->forms = g_reconvert_old_forms;
	c->bctx = g_reconvert_old_bctx;
	c->layout = g_reconvert_old_layout;
	c->iframe = g_reconvert_old_iframe;
	g_reconvert_old_bctx = NULL;
	g_reconvert_old_layout = NULL;
	g_reconvert_old_iframe = NULL;
	g_reconvert_old_forms = NULL;

	if (c->layout != NULL) {
		c->unit_len_ctx.root_style = c->layout->style;
		html_reconvert_restore_box_links(c->layout, 0, &rebound);
		(void) imagemap_extract(c);
	}

	macsurf_debug_log_writef(
		"LIFE reconvert rollback layout=%p bctx=%p links=%ld",
		(void *) c->layout, (void *) c->bctx, (long) rebound);
}

static void html_reconvert_done(html_content *c, bool success)
{
	nserror err;
	content_status saved_status;

	c->box_conversion_context = NULL;
	/* fixes895 - the box build finished (or failed); this brackets the dark
	 * async span. A crash BEFORE this line was in dom_to_box; a crash AFTER it
	 * is in free_old / imagemap / reformat / first-paint. */
	macsurf_reconv_pos_set("html_reconvert_done", (long) macsurf_reconvert_seq,
			0, "");
	macsurf_reconv_pos_flush();
	/* fixes843b (#167 S1 census) - WORK-gated: only reachable via the
	 * facebook.com-gated path (macos9_js_mark_dom_dirty), so naturally
	 * rate-limited; not filtered by the failures-only release gate. */
	macsurf_reconvert_last_layout = (void *) c->layout;
	macsurf_debug_log_writef(
			"WORK reconvert #%ld: DONE-ENTRY success=%d layout=%p",
			(long) macsurf_reconvert_seq, (int)success, (void *)c->layout);
	macsurf_profile_stamp("reconvert-done");

	if ((success == false) || (c->aborted)) {
		macsurf_debug_log_writef("WORK reconvert #%ld: FAILED/aborted",
				(long) macsurf_reconvert_seq);
		html_reconvert_rollback(c);
		/* fixes895 - disarm the hunt: the async span is over. */
		macsurf_reconvert_in_progress = 0;
		macsurf_debug_log_writef("LIFE reconvert in_progress=0 seq=%ld (done-failed)",
				(long) macsurf_reconvert_seq);
		macsurf_debug_log_reconv_flush(0);
		macsurf_reconv_pos_set("reconvert-idle-FAILED",
				(long) macsurf_reconvert_seq, 0, "");
		macsurf_reconv_pos_flush();
		return;
	}

	/* New tree is live + laid out - NOW free the old one. Shared styles
	 * survive via their refcount held by the new tree. */
	{	/* fixes1095 */
		double t0 = html_reconv_now();
		html_reconvert_relink_objects(c);
		html_reconv_ph_add(RECONV_PH_RELINK, t0);
		t0 = html_reconv_now();
		html_reconvert_free_old();
		html_reconv_ph_add(RECONV_PH_FREEOLD, t0);
	}
	macsurf_reconv_pos_set("after-free_old", (long) macsurf_reconvert_seq,
			0, "");
	macsurf_reconv_pos_flush();

	err = imagemap_extract(c);
	macsurf_debug_log_writef(
			"WORK reconvert #%ld: imagemap_extract -> %d",
			(long) macsurf_reconvert_seq, (int)err);

	macsurf_debug_log_writef(
			"WORK reconvert #%ld: -> content__reformat w=%d h=%d",
			(long) macsurf_reconvert_seq,
			(int) c->base.available_width, (int) c->base.available_height);
	macsurf_reconv_pos_set("content__reformat", (long) macsurf_reconvert_seq,
			0, "");
	macsurf_reconv_pos_flush();
	{	/* fixes1095 - REFORMAT (cascade + layout + first paint) */
		double t0 = html_reconv_now();
		/* content__reformat deliberately accepts only an already-live
		 * content. A JS geometry read may force this reconvert while the
		 * parser is still LOADING, though. The tree is complete at this
		 * point; borrow READY solely for the formatter assertion and restore
		 * LOADING immediately so the fetch/load lifecycle cannot advance. */
		saved_status = c->base.status;
		if (saved_status == CONTENT_STATUS_LOADING)
			c->base.status = CONTENT_STATUS_READY;
		content__reformat(&c->base, false,
				c->base.available_width, c->base.available_height);
		if (saved_status == CONTENT_STATUS_LOADING)
			c->base.status = saved_status;
		html_reconv_ph_add(RECONV_PH_REFORMAT, t0);
	}

	/* fixes895 - the full cycle repainted without a crash. Disarm. */
	macsurf_debug_log_writef(
			"WORK reconvert #%ld: first-paint OK", (long) macsurf_reconvert_seq);
	macsurf_reconvert_in_progress = 0;
	macsurf_debug_log_writef("LIFE reconvert in_progress=0 seq=%ld (done-ok)",
			(long) macsurf_reconvert_seq);
	macsurf_debug_log_reconv_flush(0);
	/* fixes1288 (#167) - relink can retire the final in-flight duplicate.
	 * Its handle is gone, so no later object callback exists to notice that
	 * active reached zero.  Complete the same READY->DONE transition here. */
	if (c->base.active == 0 &&
			c->base.status == CONTENT_STATUS_READY) {
		(void) html_proceed_to_done(c);
	}
	html_pagemap_dump(c, "reconvert"); /* fixes1015 */
	html_slider_probe(c, "reconvert"); /* fixes1093 */

	/* fixes1019 - a reconvert that CHANGED the document height fires one
	 * `resize` at window. The featured-slider class of widget (slick,
	 * dotdotdot, masonry) reshapes the DOM and then MEASURES to size
	 * itself; in this engine those init-time reads honestly answer
	 * undefined (fixes1016 -- the boxes do not exist until this deferred
	 * reconvert), so the widget lays out nothing and NOTHING EVER TELLS IT
	 * TO TRY AGAIN. `resize` is the re-measure trigger every such library
	 * already binds; firing it here, with the new box tree live and
	 * geometry settled, is the standard hook through which they converge.
	 * Height-change-gated so a layout that stabilised goes quiet instead
	 * of ping-ponging with the reconvert debounce forever. */
/* fixes1090 - ITS OWN SWITCH, and ON.
 *
 * fixes1022 quiesced this under MACSURF_JS_FIRE_LOAD, which is not defined
 * on the Mac. So fixes1019 -- which was written for, and verified against,
 * exactly this widget -- was compiled out three commits after it shipped,
 * and hackaday's featured slider has been collapsed ever since. It was
 * bundled in with the `load` quiesce despite being a different mechanism
 * entirely: `load` is a page-lifecycle event that fires once and had never
 * fired before; this is one narrow, height-gated convergence hook.
 *
 * That the two shared a switch is what hid it. The slider was investigated
 * for days as a geometry problem, a prototype problem and a DOM-deletion
 * problem, and the actual cause was a fix we already had, switched off by
 * an unrelated flag.
 *
 * Why it is safe to enable on its own: it fires ONE resize per reconvert
 * that CHANGED the document height. A stabilised layout goes quiet instead
 * of ping-ponging with the reconvert debounce, and a page that never
 * reconverts never sees it at all. */
#ifndef MACSURF_JS_RECONVERT_RESIZE
#define MACSURF_JS_RECONVERT_RESIZE 1
#endif
#if MACSURF_JS_RECONVERT_RESIZE
	{
		/* fixes1090 - track the height PER CONTENT. last_resize_h was a
		 * bare static shared across every document in the session, so a
		 * page whose height happened to match the previous page's would
		 * silently skip its convergence resize. Keyed on the content
		 * pointer, a new document always gets its first fire. */
		static void *last_resize_c = NULL;
		static int last_resize_h = -1;
		if (c->js_thread != NULL &&
				((void *)c != last_resize_c ||
				 (int)c->base.height != last_resize_h)) {
			last_resize_c = (void *)c;
			last_resize_h = (int)c->base.height;
			macsurf_debug_log_writef(
				"LIFE reconvert height %d -> resize fired",
				(int)c->base.height);
			(void) js_fire_event(c->js_thread, "resize",
					c->document, NULL);
			/* fixes1090b - `resize` alone was still a no-op for the
			 * hackaday slider: the REAL slick.js (harness/
			 * hackaday-bundle.js:933-942) gates its resize handler on
			 * `$(window).width() !== _.windowWidth` before it will call
			 * `_.setPosition()` (the actual re-measure). Our synthetic
			 * resize never changes the reported window width, so that
			 * branch is permanently false and setPosition never runs --
			 * confirmed by reading the bundled source, not guessed.
			 * slick's ONLY unconditional re-measure hooks are its
			 * one-shot init() call (which fired too early here, while
			 * the box tree was still unsettled, and measured garbage)
			 * and `$(window).on('load', _.setPosition)`
			 * (hackaday-bundle.js:944) -- and window `load` never fires
			 * on the Mac at all (MACSURF_JS_FIRE_LOAD is off). Dispatch
			 * `load` here too, under the exact same gate as `resize`:
			 * once per content, only when a reconvert actually changed
			 * the document height. This does NOT touch readyState or
			 * the once-per-navigation `__ms_load_fired` idempotency
			 * flag in js_fire_window_load -- it is a second plain
			 * window.dispatchEvent, scoped identically to the resize
			 * fire above, so it carries the same safety argument
			 * fixes1090 already made and does not reopen the
			 * MACSURF_JS_FIRE_LOAD switch or its history. */
			(void) js_fire_event(c->js_thread, "load",
					c->document, NULL);
		}
	}
#endif
	/* fixes1235 (#167) - deliver a MutationObserver batch to any
	 * registered observer. Unlike the resize/load hooks above, this is
	 * NOT height-gated: a real DOM mutation just completed (that is why
	 * reconvert ran at all), so every registered observer should hear
	 * about it, not only ones whose consequence changed the page's
	 * height. Fires once per completed reconvert -- see js_fire_mutation_
	 * batch's own comment (macsurf_qjs.c) for why this cannot introduce a
	 * new feedback-loop frequency beyond what reconvert's debounce/floor
	 * already bounds. */
	if (c->js_thread != NULL) {
		js_fire_mutation_batch(c->js_thread);
	}
	macsurf_reconv_pos_set("reconvert-idle", (long) macsurf_reconvert_seq,
			0, "");
	macsurf_reconv_pos_flush();
}

/* Re-run box construction over the current DOM. Returns NSERROR_NEED_DATA if
 * the caller should re-arm (busy: mid-layout, convert in flight, or image
 * fetches still active). The DOM persists; JS element wrappers stay valid. */
nserror html_reconvert(html_content *c)
{
	dom_node *html = NULL;
	dom_exception exc;
	nserror error;

	if ((c == NULL) || (c->document == NULL) || (c->aborted))
		return NSERROR_OK;
	/* fixes1094 (#265 Round B) - READY is now enough; it no longer has to be
	 * DONE.
	 *
	 * DONE means "the load finished". What a reconvert actually needs is "a
	 * box tree exists and nothing is walking it", which is READY onwards --
	 * READY is set by html_box_convert_done, i.e. precisely when the first
	 * tree has been built. The reflowing / box_conversion_context guards
	 * below enforce the rest, and they are the real preconditions.
	 *
	 * This does NOT reach the case #265 ultimately needs (a script measuring
	 * during LOADING, when c->layout is still NULL and there is no tree to
	 * rebuild) -- that is Round C and needs first-tree construction. It does
	 * open the window between READY and DONE, which is where sub-resources
	 * are still landing and re-laying-out the page. */
	{
		content_status st = content__get_status(&c->base);
		/* fixes1096 (#265 Round C3) - LOADING is allowed too, and this is
		 * the case the hackaday header actually needs.
		 *
		 * End-of-body scripts measure mid-parse: hardware has the theme
		 * bundle at tick 48579 with domready at 49060, status LOADING and
		 * c->layout still NULL. Geometry answers undefined there, jQuery's
		 * parseFloat(x)||0 turns it into 0, and the theme bakes height:0px
		 * onto every slide in a one-shot onInit. A real browser answers
		 * truly because offsetHeight forces layout of what has been parsed;
		 * this lets us do the same.
		 *
		 * Safe here for reasons that are checked, not assumed:
		 *   - html_reconvert_done touches NO content status, so it cannot
		 *     mis-advance the load lifecycle from under the parser;
		 *   - the box_conversion_context guard below already refuses while
		 *     the initial dom_to_box is in flight, so this can only run in
		 *     the parse window BEFORE that starts;
		 *   - scripts execute between parser tokens, so the DOM is in a
		 *     consistent state whenever this is reachable from JS;
		 *   - building a first tree from scratch is a path already exercised
		 *     constantly (the harness does it with layout=(nil) 59 times a
		 *     run), not a new one.
		 *
		 * The cost question is real and unresolved -- C1 measured build at
		 * 3.57s of a 4.01s reconvert -- but it is bounded by the existing
		 * pending-mutation short-circuit and the sync budget, and it is the
		 * subject of C2. */
		if (st != CONTENT_STATUS_LOADING &&
		    st != CONTENT_STATUS_READY && st != CONTENT_STATUS_DONE)
			return NSERROR_NEED_DATA;
	}
	if (c->reflowing)
		return NSERROR_NEED_DATA;        /* never free boxes mid-layout */
	if (c->box_conversion_context != NULL)
		return NSERROR_NEED_DATA;        /* one re-convert in flight    */
	/* fixes1127 - reentrancy guard. box_conversion_context above only blocks
	 * a SECOND reconvert while the first is still mid-dom_to_box; it does NOT
	 * block one triggered from INSIDE html_reconvert_done's synchronous
	 * resize/load JS fire, because that fire happens after
	 * macsurf_reconvert_in_progress is already cleared and after
	 * c->box_conversion_context is about to be cleared by dom_to_box's own
	 * completion path. See the g_html_reconvert_depth comment at its
	 * declaration. NEED_DATA here is the same "try again later" signal every
	 * other guard in this function already returns, and it is picked up by
	 * the existing fixes1126 debounced retry (macos9_reconvert_sync_retry,
	 * 80ms) in macos9_reconvert.c -- the mutation is not lost, just deferred
	 * to run after the outer reconvert has fully unwound. */
	macsurf_debug_log_writef(
		"LIFE reconvert depth=%d entering", g_html_reconvert_depth);
	if (g_html_reconvert_depth > 0) {
		macsurf_debug_log_writef(
			"LIFE reconvert REENTRANT BLOCKED depth=%d",
			g_html_reconvert_depth);
		return NSERROR_NEED_DATA;
	}
	g_html_reconvert_depth++;
	/* fixes1105 (#265) - THE reconvert bug, proven on hardware.
	 *
	 * Without a select context there is no cascade, and libcss rejects the
	 * very first css_select_style() call with CSS_BADPARM (its guard is
	 * `ctx == NULL || node == NULL || ...`). That first call is for the
	 * ROOT <html> element, so box_construct_element bails at its
	 * `box_get_style() == NULL` check, dom_to_box reports failure, and the
	 * ENTIRE document rebuild is thrown away. Every element would fail
	 * identically; the root simply gets there first.
	 *
	 * select_ctx is created ONCE, in html_finish_conversion(), which runs
	 * when the document finishes parsing. Script that mutates the DOM
	 * DURING parse schedules a reconvert before that point, so the
	 * reconvert runs against select_ctx == NULL.
	 *
	 * Hardware, hackaday.com (fixes1104 log): 103 reconverts, 103 failures,
	 * `selctx=00000000` on every one, and ALL 103 land BEFORE the
	 * content_ready that follows finish_conversion -- zero failures after
	 * it. Not one reconvert has ever succeeded on that page.
	 *
	 * NEED_DATA is the correct answer, not an error: it is what every other
	 * precondition here returns, and it leaves the caller's debounce free to
	 * retry once the context exists. The mutations are not lost -- the
	 * initial conversion is still to come and builds the tree from the
	 * mutated DOM.
	 *
	 * fixes1158 - LAZY CREATION: if the base stylesheet has already landed
	 * (stylesheets[STYLESHEET_BASE].sheet != NULL), create the selection
	 * context HERE - the same html_css_new_selection_context call
	 * finish_conversion uses - so the reconvert can proceed instead of
	 * deferring for the rest of the load (hardware: every pre-finish
	 * reconvert on hackaday deferred, 103/103). finish_conversion's own
	 * select_ctx != NULL guard then skips its creation + dom_to_box exactly
	 * as it does for a stylesheet re-entry, and the tree built by this
	 * reconvert stands. Only while the base sheet is still in flight - or
	 * when the creation itself fails - does the NEED_DATA defer below
	 * remain.
	 *
	 * fixes1193 - a non-NULL sheet HANDLE only means the fetch got a
	 * handle back, not that the sheet finished loading/parsing. The
	 * normal (finish_conversion) path never reaches this call without
	 * html_can_begin_conversion() having confirmed c->base.active == 0
	 * first -- i.e. every stylesheet fetch, base and author both, has
	 * fully settled. This lazy path was missing that check: an author
	 * stylesheet (or even the base sheet itself) can still be mid-parse
	 * on another poll pass when a JS-triggered reconvert races in here,
	 * and html_css_new_selection_context reads that stylesheet's
	 * selector hash table (css_select_ctx_append_sheet) while the async
	 * parse is concurrently still growing/rehashing it -- a genuine
	 * data race, not just a staleness question. Hardware crash: EXC_BAD_
	 * ACCESS in css__selector_hash_find_by_class, called from exactly
	 * this call chain (html_reconvert -> dom_to_box -> css_select_style),
	 * on hackaday, which both drives this lazy path hardest (its whole
	 * reason for existing) and loads enough author CSS for the race
	 * window to be real. Require the same precondition the normal path
	 * requires.
	 *
	 * fixes1194 - MUST NOT fire while c->base.status is still LOADING.
	 * fixes1096 (above, #265 Round C3) deliberately allows a reconvert to
	 * run during LOADING -- for mid-parse geometry reads, which is a real
	 * and narrow need -- reasoning that html_reconvert_done "touches NO
	 * content status, so it cannot mis-advance the load lifecycle from
	 * under the parser". That was true and safe as long as a LOADING-time
	 * reconvert always deferred to NEED_DATA (no select_ctx yet, the
	 * pre-fixes1158 100%-defer rate the fixes1158 comment above cites).
	 * fixes1158 changed that: a LOADING-time reconvert can now actually
	 * SUCCEED and build the page's first-ever box tree through
	 * html_reconvert_done -- which is exactly the property fixes1096
	 * called safe, except html_reconvert_done touching NO content status
	 * cuts both ways: it also never calls content_set_ready (only
	 * html_box_convert_done does, the NORMAL initial-conversion
	 * callback). html_finish_conversion, when it eventually runs, sees
	 * select_ctx already non-NULL and treats it as a legitimate late-
	 * stylesheet re-entry (`if (htmlc->select_ctx != NULL) { ... return;
	 * }`) -- skipping content_set_ready, js_fire_dom_ready, and
	 * html_proceed_to_done PERMANENTLY, because they are only reachable
	 * from html_box_convert_done. The page has a real, live, reconvert-
	 * maintained box tree from then on, and the browser window is never
	 * told it exists: hardware confirmed this directly on hackaday
	 * (NAV: content_ready never fires; reconvert cycles keep completing
	 * "done-ok" forever after). Restricting the lazy path to
	 * READY/DONE restores the pre-fixes1158, fixes1096-safe behavior for
	 * the LOADING case (always defer; the normal path builds the first
	 * tree and fires content_set_ready correctly) while keeping
	 * fixes1158's benefit for its real target -- a reconvert AFTER the
	 * page is already showing, where select_ctx being created here
	 * instead of by finish_conversion changes nothing about readiness. */
	if (c->select_ctx == NULL &&
	    c->stylesheets != NULL &&
	    c->stylesheets[STYLESHEET_BASE].sheet != NULL &&
	    c->base.active == 0 &&
	    content__get_status(&c->base) != CONTENT_STATUS_LOADING) {
		error = html_css_new_selection_context(c, &c->select_ctx);
		if (error != NSERROR_OK) {
			/* creation failed (OOM etc.) - same transient defer as
			 * every other precondition; the debounced retry tries
			 * again once the sheet state settles. */
			macsurf_debug_log_writef(
				"LIFE reconvert: lazy select_ctx create failed err=%d",
				(int) error);
			g_html_reconvert_depth--;	/* fixes1127 */
			return NSERROR_NEED_DATA;
		}
		macsurf_debug_log_writef(
			"LIFE reconvert: select_ctx created lazily (pre-"
			"finish_conversion), proceeding");
	}
	if (c->select_ctx == NULL) {
		macsurf_debug_log_writef(
			"LIFE reconvert: defer - no select_ctx yet (pre-"
			"finish_conversion); cascade would fail CSS_BADPARM");
		g_html_reconvert_depth--;	/* fixes1127 */
		return NSERROR_NEED_DATA;
	}
	/* fixes421 - quiesce guard: if sub-resource fetches (images, CSS) are
	 * still in flight, html_object_callback holds a pw pointer into
	 * object_list entries that html_object_free_objects is about to free.
	 * In cooperative MT the callback fires on the next event-loop pass -
	 * after our free - causing a use-after-free in html_object_done.
	 *
	 * fixes1094 (#265 Round B) - NARROWED to the entries that are still
	 * actually freed. `base.active > 0` was far too coarse for two reasons,
	 * and together they made it block every reconvert on a loading page:
	 *
	 *   1. base.active counts the HTML content's OWN fetch (set to 1 in
	 *      html_create, decremented at the end of html_convert), so it is
	 *      >= 1 for the whole of LOADING regardless of any object.
	 *   2. The sub-resource fetches it otherwise counts are overwhelmingly
	 *      IMAGES, and since fixes976 an in-flight image is CARRIED across a
	 *      reconvert, never freed (harness Test 56 proves this against the
	 *      real path).
	 *
	 * So block on the hazard itself -- an in-flight entry the partition would
	 * DROP -- rather than on a proxy that is almost never true for the reason
	 * it cites. */
	if (c->base.active > 0 && html_reconvert_has_droppable_inflight(c)) {
		macsurf_debug_log_writef(
			"LIFE reconvert: defer - %ld active, droppable in flight",
			(long) c->base.active);
		g_html_reconvert_depth--;	/* fixes1127 */
		return NSERROR_NEED_DATA;
	}

	{
		extern unsigned long macsurf_box_backlink_cleared;
		macsurf_reconvert_seq++;
		macsurf_debug_log_writef(
			"WORK reconvert #%ld: START layout=%p active=%d backlinks_cleared=%ld"
			" freemem=%ld",
			(long) macsurf_reconvert_seq, (void *) c->layout,
			(int) c->base.active,
			(long) macsurf_box_backlink_cleared,
			macsurf_free_mem());
	}

	/* fixes895 - arm the hunt for the ENTIRE async span (from here through
	 * html_reconvert_done). in_progress makes box_construct.c's shared
	 * dom_to_box path emit its dense per-node breadcrumbs only during a
	 * reconvert; reconv_flush forces each breadcrumb to disk so a hard bomb
	 * loses nothing; the pos marker names the phase durably in a separate file.
	 * Every exit path below (dom_to_box-failed here, both branches of _done)
	 * disarms it. */
	macsurf_reconvert_in_progress = 1;
	macsurf_debug_log_writef("LIFE reconvert in_progress=1 seq=%ld (start)",
			(long) macsurf_reconvert_seq);
	macsurf_debug_log_reconv_flush(1);
	macsurf_reconv_pos_set("reconvert-START", (long) macsurf_reconvert_seq,
			0, "");
	macsurf_reconv_pos_flush();

	/* HAZARD guards BEFORE freeing the box tree (order matters). */
	/* fixes891 - clear the stored hover/active DOM nodes. They are cleared on
	 * reformat (html_reformat) but were NOT cleared on reconvert. They are
	 * dom_nodes, and the JS mutation that triggered THIS reconvert may have
	 * removed the very node one of them holds -- leaving a dangling dom_node
	 * that the next hover would feed to box_for_node (a vtable dispatch, so a
	 * freed node -> jump through garbage -> the 003B009C crash). Same reason
	 * fixes445 clears them in html_reformat. */
	c->dyn_hover_node = NULL;
	c->dyn_active_node = NULL;
	{	/* fixes1095 - H1 */
		double t0 = html_reconv_now();
		g_reconv_ph_n++;
		html_reconvert_clear_node_boxes(c);  /* H1: stale node boxes */
		html_reconv_ph_add(RECONV_PH_H1, t0);
	}
	/* fixes921 - H2 NO LONGER FREES THE OBJECT LIST.
	 *
	 * Releasing every image handle here is what made a reconvert lose its
	 * images: box->object goes NULL, and layout only sizes an <img> at
	 * `if (b->object && !(b->flags & REPLACE_DIM))`, so any box whose object
	 * has not re-linked keeps its line-height -- the squish. Anything still
	 * in flight was aborted outright. It is also the most expensive of the
	 * ~10 O(document) passes a reconvert runs.
	 *
	 * The entries are instead carried across the rebuild and re-attached to
	 * the new boxes by html_reconvert_relink_objects(), called from
	 * html_reconvert_done once dom_to_box has built the tree and BEFORE
	 * html_reconvert_free_old drops the old one. Everything that pass does
	 * not re-link (non-image entries, and any node that no longer resolves
	 * to a box) it retires there, so nothing leaks.
	 *
	 * Safe to leave stale object->box pointers across this window ONLY
	 * because the reconvert build is SYNCHRONOUS (fixes903): no event-loop
	 * turn happens between here and the re-link, so no hlcache callback can
	 * fire and dereference one. */
	macsurf_debug_log_writef(
		"WORK reconvert #%ld: H2 objects carried (relink deferred)",
		(long) macsurf_reconvert_seq);
	macsurf_debug_log_writef(
		"WORK reconvert #%ld: H2 objects freed active=%d",
		(long) macsurf_reconvert_seq, (int) c->base.active);
	{	/* fixes1095 - H3 */
		double t0 = html_reconv_now();
		html_reconvert_detach_forms(c);      /* H3: form box pointers */
		imagemap_destroy(c);                 /* rebuilt in done       */
		if (c->sel != NULL)
			selection_destroy(c->sel);
		c->sel = selection_create((struct content *) c);
		html_reconv_ph_add(RECONV_PH_H3, t0);
	}
	macsurf_debug_log_writef(
		"WORK reconvert #%ld: H3 forms+imagemap+selection reset",
		(long) macsurf_reconvert_seq);

	/* fixes899 - restore the initial-build CSS preconditions BEFORE re-cascade:
	 * release every element's stale pass-1 libcss node data (so the second
	 * cascade cannot share freed pass-1 styles) and NULL the stale root_style.
	 * Placed here while the OLD box tree is still alive, so releasing a
	 * node_data style ref cannot drop a shared style to refcount 0 (the old
	 * box->styles still holds it); the old tree is freed later in
	 * html_reconvert_free_old, after the new tree owns its refs. This is the
	 * real reconvert-crash fix (Layer 1); the fixes889-898 guards were
	 * scaffolding around this un-restored precondition. */
	{
		double t0 = html_reconv_now();	/* fixes1095 - CSS */
		long ncleared = html_reconvert_reset_css_state(c);
		html_reconv_ph_add(RECONV_PH_CSS, t0);
		macsurf_debug_log_writef(
			"WORK reconvert #%ld: CSS reset -- node_data cleared=%ld,"
			" root_style=NULL", (long) macsurf_reconvert_seq, ncleared);
	}

	/* fixes896 - pin the DOM's live text-node dom_strings across the rebuild
	 * window. Was fixes843, which walked c->layout (the box tree) for BOX_TEXT
	 * boxes with a node backlink that box_construct never sets -> pinned 0 ->
	 * the fixes489 UAF stayed open. Walk c->document, the real source
	 * box_construct_text reads. */
	{
		double t0 = html_reconv_now();	/* fixes1095 - PIN */
		long pinned = html_reconvert_pin_text_strings(c->document);
		html_reconv_ph_add(RECONV_PH_PIN, t0);
		macsurf_debug_log_writef(
			"WORK reconvert #%ld: pinned %ld DOM text-node strings",
			(long) macsurf_reconvert_seq, pinned);
	}

	/* fixes421 DOUBLE-BUFFER: do NOT free the old bctx yet. Freeing it now
	 * runs box destructors -> css_select_results_destroy on old styles BEFORE
	 * the re-cascade can re-intern them; that free-then-reintern trips the
	 * libcss arena's "duplicate interned style destroyed while still
	 * referenced" use-after-free (0x2710 garbage-fn-ptr crash). Keep the old
	 * tree alive THROUGH dom_to_box; freed in html_reconvert_done once the
	 * new tree + reformat are live. All back-references already cleared
	 * above (H1/H2/H3), so the old tree is orphaned-but-alive until then. */
	MS_LOG("reconvert: defer old bctx free");
	g_reconvert_old_bctx = c->bctx;
	g_reconvert_old_layout = c->layout;
	g_reconvert_old_iframe = c->iframe;
	g_reconvert_old_forms = c->forms;
	c->bctx = NULL;
	c->layout = NULL;
	c->forms = NULL;

	/* fixes915 - THE IFRAME LIST DIES WITH THE OLD bctx. Drop it here.
	 *
	 * content_html_iframe records are talloc CHILDREN of bctx
	 * (box_special.c: talloc(content->bctx, struct content_html_iframe), and
	 * iframe->name via talloc_strdup(content->bctx, ...)). So
	 * html_reconvert_free_old's talloc_free(g_reconvert_old_bctx) frees every
	 * record on this list -- but box construction only ever PREPENDS
	 * (content->iframe = iframe), so without this the list becomes
	 *     [new iframes] -> [freed old iframes]
	 * with the dead tail still linked. Nothing walks it until teardown, and
	 * then html_destroy_iframe reads iframe->name out of recycled memory and
	 * hands it to talloc_free.
	 *
	 * That is the 2026-07-19 iMac crash, caught in the CodeWarrior debugger:
	 *   macos9_poll -> macos9_deathrow_drain -> content_deathrow_teardown ->
	 *   content_destroy_now -> html_destroy -> html_destroy_iframe ->
	 *   talloc_free -> talloc_chunk_from_ptr
	 * with ptr=0x2D6E6F74 -- which is not a pointer at all but the ASCII
	 * bytes "-not", i.e. text that had been allocated into the freed record.
	 * It reproduced on macintoshgarden.org, a page that reconverts.
	 *
	 * Dropping the head leaks nothing: box_iframes_talloc_destructor
	 * (box_special.c) does the nsurl_unref(f->url) when talloc frees each
	 * record with the old context. The browser-window side re-links against
	 * the rebuilt list on the next reformat (fixes905), which is exactly why
	 * that repair kept finding stale boxes -- it was matching against a list
	 * with dead entries in it. */
	c->iframe = NULL;
	macsurf_debug_log_writef(
		"WORK reconvert #%ld: old bctx deferred layout=NULL",
		(long) macsurf_reconvert_seq);

	exc = dom_document_get_document_element(c->document, (void *) &html);
	if ((exc != DOM_NO_ERR) || (html == NULL)) {
		macsurf_debug_log_writef(
			"WORK reconvert #%ld: doc_element FAILED exc=%d",
			(long) macsurf_reconvert_seq, (int) exc);
		html_reconvert_rollback(c);
		/* fixes895 - disarm: html_reconvert_done will never run. */
		macsurf_reconvert_in_progress = 0;
		macsurf_debug_log_writef("LIFE reconvert in_progress=0 seq=%ld (doc-element-failed)",
				(long) macsurf_reconvert_seq);
		macsurf_debug_log_reconv_flush(0);
		g_html_reconvert_depth--;	/* fixes1127 */
		return NSERROR_DOM;
	}
	macsurf_debug_log_writef(
		"WORK reconvert #%ld: doc_element=%p -> dims -> dom_to_box",
		(long) macsurf_reconvert_seq, (void *) html);
	html_get_dimensions(c);
	if (html_reconvert_rebuild_forms(c) == false) {
		macsurf_debug_log_writef(
			"WORK reconvert #%ld: form registry rebuild FAILED",
			(long) macsurf_reconvert_seq);
		dom_node_unref(html);
		html_reconvert_rollback(c);
		macsurf_reconvert_in_progress = 0;
		macsurf_debug_log_writef(
			"LIFE reconvert in_progress=0 seq=%ld (forms-failed)",
			(long) macsurf_reconvert_seq);
		macsurf_debug_log_reconv_flush(0);
		g_html_reconvert_depth--;
		return NSERROR_NOMEM;
	}
	macsurf_reconv_pos_set("pre-dom_to_box", (long) macsurf_reconvert_seq,
			0, "");
	macsurf_reconv_pos_flush();
	{	/* fixes1095 - BUILD. Synchronous (fixes903), so this bracket also
		 * contains html_reconvert_done, i.e. relink/freeold/reformat. Those
		 * subtract themselves out below so `build` reads as construction
		 * alone. */
		double t0 = html_reconv_now();
		long inner0 = g_reconv_ph_us[RECONV_PH_RELINK] +
			g_reconv_ph_us[RECONV_PH_FREEOLD] +
			g_reconv_ph_us[RECONV_PH_REFORMAT];
		long spent;
#ifdef MACSURF_RECONVERT_TEST_HOOK
		if (g_reconvert_test_fail_once) {
			g_reconvert_test_fail_once = 0;
			error = NSERROR_NOMEM;
		} else
#endif
		{
			error = dom_to_box(html, c, html_reconvert_done,
					&c->box_conversion_context);
		}
		spent = (long)(html_reconv_now() - t0) -
			((g_reconv_ph_us[RECONV_PH_RELINK] +
			  g_reconv_ph_us[RECONV_PH_FREEOLD] +
			  g_reconv_ph_us[RECONV_PH_REFORMAT]) - inner0);
		if (spent < 0) spent = 0;
		g_reconv_ph_us[RECONV_PH_BUILD] += spent;
	}
	if (error != NSERROR_OK) {
		macsurf_debug_log_writef(
			"WORK reconvert #%ld: dom_to_box FAILED err=%d",
			(long) macsurf_reconvert_seq, (int) error);
		html_reconvert_rollback(c);
		/* fixes895 - disarm: html_reconvert_done will never run. */
		macsurf_reconvert_in_progress = 0;
		macsurf_debug_log_writef("LIFE reconvert in_progress=0 seq=%ld (dom_to_box-failed)",
				(long) macsurf_reconvert_seq);
		macsurf_debug_log_reconv_flush(0);
	}
	/* fixes1127 - dom_to_box is synchronous (fixes903): whether it succeeded
	 * (html_reconvert_done, including its resize/load JS fire, already ran
	 * inside the call above) or failed, the reentrant span is over now. */
	g_html_reconvert_depth--;
	dom_node_unref(html);
	return error;
}

/* Thin content* wrapper so the macos9 JS bindings / scheduler can trigger a
 * re-convert without the full html_content type in scope. Returns the nserror
 * as int: 0 = NSERROR_OK (re-convert queued), non-zero = busy/skip (the caller
 * should re-arm). */
/* fixes1087 (#265) - is the box tree safe to READ from outside layout?
 *
 * The JS geometry layer gated on CONTENT_STATUS_DONE, and hardware showed
 * that refusing 100% of measurements taken during page load: every log reads
 * declined=NNN with notdone at 100%. Script init runs before DONE, so every
 * measure-then-layout widget got nothing. hackaday's featured slider is the
 * concrete casualty -- PAGEMAP shows it slick-initialized with 5 slides and
 * the track collapsed to h=15, because slick sets .slick-list height from a
 * measurement and its slides are floated inside an overflow:hidden box with
 * no natural height.
 *
 * DONE was always a proxy for the thing that actually matters, which is "a
 * box tree exists and nothing is rebuilding it right now". That is checkable
 * directly, and it is true for most of the load:
 *   !reflowing              not inside layout_document
 *   box_conversion_context  NULL = no dom_to_box walk in flight
 *   status READY or DONE    converted at least once (LOADING has no tree)
 *
 * Deliberately NOT testing layout != NULL. html_reconvert nulls c->layout and
 * then calls dom_to_box within the same synchronous call, so the gap is not
 * observable from JS, and the whole window is already covered by the
 * box_conversion_context test below. Requiring it only rejects a content whose
 * root pointer is momentarily unset while its boxes are perfectly readable via
 * box_for_node -- which is how every caller reaches them anyway.
 *
 * This is NOT a relaxation of the fixes674c rule. That rule forbids walking a
 * tree that is being CONSTRUCTED, and the two middle checks are exactly how
 * construction is detected -- more precisely than status ever did, since a
 * DONE content is also mid-rebuild during a reconvert. Callers must still
 * re-resolve through box_for_node rather than holding a box*, which is the
 * other half of the fixes1012 checklist and unchanged.
 *
 * Takes struct content so the macos9 JS glue can call it without needing
 * html_content in scope. */
int macsurf_html_tree_stable(struct content *c);
int macsurf_html_tree_stable(struct content *c)
{
	html_content *htmlc = (html_content *) c;

	static long budget = 12;	/* fixes1087 - bounded; one per cause */
	const char *why = NULL;

	if (c == NULL)
		return 0;
	/* fixes1097 (#265 Round C3b) - THE THIRD GATE.
	 *
	 * fixes1096 opened the two gates that BUILD a tree during LOADING
	 * (html_reconvert and macos9_reconvert_flush_now) and hardware answered
	 * notdone=0, flush=155 -- the builds ran. And `real` stayed at 2, because
	 * THIS predicate, which decides whether an answer may be GIVEN, still
	 * demanded READY. So 155 trees were built and every reading was thrown
	 * away: unstable=565, the exact count that used to be notdone.
	 *
	 * The condition that matters is "is there a tree and is it still", not
	 * "has the load finished" -- reflowing and box_conversion_context below
	 * are the real tests, and a LOADING document that satisfies both has a
	 * perfectly readable tree. Requiring a status as well was the DONE-gate
	 * assumption surviving one level deeper than anyone looked.
	 *
	 * NOT gated on c->layout != NULL, though the first draft was. That is
	 * redundant and, worse, too coarse: qjs_box_for already re-resolves
	 * PER ELEMENT via box_for_node and answers undefined when that element
	 * has no box, which is the precise version of the same check. The global
	 * form also refuses legitimate reads after a FAILED build, where layout
	 * is NULL but the boxes are still alive on the deferred-free path -- the
	 * harness reproduces exactly that (reconvert DONE-ENTRY success=0,
	 * layout=(nil), boxes readable), and Test 43 went red on it. Mid-build
	 * and mid-layout remain guarded below, which is what actually matters. */
	if (c->status != CONTENT_STATUS_LOADING &&
	    c->status != CONTENT_STATUS_READY &&
	    c->status != CONTENT_STATUS_DONE)
		why = "status";
	else if (htmlc->reflowing)
		why = "reflowing";
	else if (htmlc->box_conversion_context != NULL)
		why = "converting";
	if (why != NULL) {
		/* Name the blocker. A refusal count with no cause attached is
		 * what made the DONE gate survive four hardware builds -- the
		 * log said geometry was refusing but never which condition, so
		 * it read as a to-do rather than the thing to fix. */
		if (budget > 0) {
			budget--;
			macsurf_debug_log_writef(
				"LIFE geomgate REFUSE %s status=%d layout=%p",
				why, (int) c->status, (void *) htmlc->layout);
		}
		return 0;
	}
	return 1;
}

int html_reconvert_content(struct content *c)
{
	return (int) html_reconvert((html_content *) c);
}

/* fixes1094 (#265 Round B) - content* wrapper so the macos9 sync-flush gate can
 * screen on the SAME hazard html_reconvert does. Kept as a thin wrapper next to
 * html_reconvert_content for the same reason that one exists: the frontend must
 * not know the html_content layout. If this and html_reconvert's own guard ever
 * disagree, the flush declines work the reconvert would have accepted -- which
 * is the drift that produced notdone=630/630 with flush=0 on hardware. */
int macsurf_html_has_droppable_inflight(struct content *c)
{
	return html_reconvert_has_droppable_inflight((html_content *) c);
}

static void html_reformat(struct content *c, int width, int height)
{
	html_content *htmlc = (html_content *) c;
	struct box *layout;
	uint64_t ms_before;
	uint64_t ms_after;
	uint64_t ms_interval;
	/* fixes161c - arm the one-shot cascade probe in select.c so the
	 * next nscss_get_style call (if reformat triggers a recascade or
	 * dynamic style lookup) logs a stage marker. Defined in select.c. */
	extern int macsurf__cascade_probe_armed;

	macos9_html_reformat_seq++;
	macsurf_debug_log_writef(
		"html_reformat #%ld: entry w=%d h=%d",
		macos9_html_reformat_seq, width, height);
	/* fixes560 - timestamped begin marker so each reformat in the storm is
	 * bracketed in the profile trail (paired with the layout-done stamp at
	 * exit); the gap between consecutive reformat-begin stamps is the
	 * inter-reformat network/idle time, the gap to layout-done is the
	 * layout cost. */
	macsurf_profile_stamp("reformat-begin");
	macsurf__cascade_probe_armed = 1;

	/* fixes383 (M2, JS->DOM->render) - re-convert null-guard. html_reconvert
	 * frees the old box tree and nulls layout while the async re-convert is
	 * in flight; a same-pass deferred reformat must NOT deref layout->style
	 * below until html_reconvert_done rebuilds it. Bail safely. */
	if (htmlc->layout == NULL) {
		macsurf_debug_log_writef(
			"html_reformat: layout NULL (re-convert in flight), skip");
		return;
	}

	/* fixes930 - did the fixes929 URL->size memo actually do anything?
	 * stored= grew means images are completing and recording their size;
	 * hit= grew means box_image found a size and the box was laid out as a
	 * replaced element rather than as its alt text. A revisit that still
	 * squishes with hit=0 means the memo is not filling; with hit>0 it means
	 * the size is reaching the box and the fault is downstream. One line per
	 * layout, and only when a counter moved. */
	{
		extern void macsurf_imgdims_stats(int *stored, int *hit,
				int *miss);
		static int last_s = -1, last_h = -1, last_m = -1;
		int st = 0, hi = 0, mi = 0;

		macsurf_imgdims_stats(&st, &hi, &mi);
		if (st != last_s || hi != last_h || mi != last_m) {
			macsurf_debug_log_writef(
				"LIFE imgdims stored=%d hit=%d miss=%d",
				st, hi, mi);
			last_s = st; last_h = hi; last_m = mi;
		}
	}

	/* fixes934 - the LIFE img ledger. Construct census (how images enter the
	 * fetch system) + object ledger (link set/nulled, and the memo-mystery
	 * disambiguator done_ok/done_zero). One line each per reformat, only when
	 * a counter moved. Read positionally against NAV / mutcensus / LIFE
	 * objects: a `nulled` jump right after a mutcensus line is the beacon
	 * reconvert blanking a painted image; done_ok=0 with rendered images means
	 * html_object_done is not the completion sink (the imgdims stored=0 cause). */
	{
		extern void macsurf_img_ctor_stats(long *total, long *eager,
				long *lazy, long *rdim, long *dropobj);
		extern void macsurf_img_object_stats(long *done_ok,
				long *done_zero, long *set, long *nulled);
		static long lc_t = -1, lc_e = -1, lc_l = -1, lc_r = -1, lc_d = -1;
		static long lo_ok = -1, lo_z = -1, lo_s = -1, lo_n = -1;
		long ct = 0, ce = 0, cl = 0, cr = 0, cd = 0;
		long ook = 0, oz = 0, os = 0, on = 0;

		macsurf_img_ctor_stats(&ct, &ce, &cl, &cr, &cd);
		if (ct != lc_t || ce != lc_e || cl != lc_l ||
		    cr != lc_r || cd != lc_d) {
			macsurf_debug_log_writef(
				"LIFE img ctor total=%ld eager=%ld lazy=%ld"
				" rdim=%ld lazydropobj=%ld",
				ct, ce, cl, cr, cd);
			lc_t = ct; lc_e = ce; lc_l = cl; lc_r = cr; lc_d = cd;
		}

		macsurf_img_object_stats(&ook, &oz, &os, &on);
		if (ook != lo_ok || oz != lo_z || os != lo_s || on != lo_n) {
			macsurf_debug_log_writef(
				"LIFE img obj doneok=%ld donezero=%ld"
				" set=%ld nulled=%ld",
				ook, oz, os, on);
			lo_ok = ook; lo_z = oz; lo_s = os; lo_n = on;
		}
	}

	/* fixes978 - the object-fetch ledger, replacing the per-fetch objfetch /
	 * objadopt probes of fixes966-977 (one write, and one volume flush, per
	 * fetch). adopt+renode+specdup is the count of fetches PREVENTED; `fetch`
	 * is the count that can still reach the network. Read it against the
	 * `LIFE objects` relink line: `fetch` climbing while `renode` stays flat
	 * on a page that reconverts means node-keyed adoption has stopped
	 * matching, which is what fixes975-977 are exposed to. */
	{
		extern void macsurf_obj_fetch_stats(long *fetch, long *spec,
				long *bg, long *adopt, long *renode,
				long *specdup);
		static long lf_f = -1, lf_s = -1, lf_b = -1;
		static long lf_a = -1, lf_r = -1, lf_d = -1;
		long ff = 0, fs = 0, fb = 0, fa = 0, fr = 0, fd = 0;

		macsurf_obj_fetch_stats(&ff, &fs, &fb, &fa, &fr, &fd);
		if (ff != lf_f || fs != lf_s || fb != lf_b ||
		    fa != lf_a || fr != lf_r || fd != lf_d) {
			macsurf_debug_log_writef(
				"LIFE obj fetch=%ld spec=%ld bg=%ld"
				" adopt=%ld renode=%ld specdup=%ld",
				ff, fs, fb, fa, fr, fd);
			lf_f = ff; lf_s = fs; lf_b = fb;
			lf_a = fa; lf_r = fr; lf_d = fd;
		}
	}

	/* fixes979 - the memory-cache ledger, read against the one above.
	 * `fetch` there counts retrievals ASKED FOR; `miss` here counts the ones
	 * that actually went to the network. A page revisit that shows fresh
	 * climbing and miss flat is the cache doing its job; miss climbing on a
	 * revisit is what image disk caching would be for. */
	{
		extern void macsurf_llcache_stats(long *fresh, long *reval,
				long *miss, long *notmod, long *cond);
		static long ll_f = -1, ll_r = -1, ll_m = -1, ll_n = -1, ll_c = -1;
		long lf = 0, lr = 0, lm = 0, ln = 0, lc = 0;

		macsurf_llcache_stats(&lf, &lr, &lm, &ln, &lc);
		if (lf != ll_f || lr != ll_r || lm != ll_m || ln != ll_n ||
		    lc != ll_c) {
			macsurf_debug_log_writef(
				"LIFE llc fresh=%ld reval=%ld miss=%ld notmod=%ld"
				" cond=%ld",
				lf, lr, lm, ln, lc);
			ll_f = lf; ll_r = lr; ll_m = lm; ll_n = ln; ll_c = lc;
		}
	}

	/* fixes445: clear hover/active tracking on reformat. The dyn_hover_node
	 * pointer survives layout but box_construct.c seeds its context from
	 * these fields; stale values after a CONTENT_MSG_ERROR-driven reformat
	 * (e.g. a 404 image triggering content_set_done while the old box tree
	 * is still live) can direct a subsequent hover dispatch down paths that
	 * read freed content state. NULL forces a fresh node walk on the next
	 * poll with no cost: the poll recomputes the node from the box tree. */
	htmlc->dyn_hover_node = NULL;
	htmlc->dyn_active_node = NULL;

	nsu_getmonotonic_ms(&ms_before);

	htmlc->reflowing = true;

	/* fixes1150 - viewport_height must be the WINDOW height, not the
		 * document height. The width/height params of html_reformat are the
		 * available_width/available_height from the content struct, which
		 * may be stale or set to the document dimensions. For viewport-
		 * relative CSS units (vh, vw) to work correctly, we need the real
		 * frontend viewport. */
	#ifdef __MACOS9__
		{
			extern void macos9_frontend_viewport(int *vw, int *vh);
			int vw = 0, vh = 0;
			macos9_frontend_viewport(&vw, &vh);
			if (vw > 0 && vh > 0) {
				htmlc->unit_len_ctx.viewport_width =
					css_unit_device2css_px(
					INTTOFIX(vw),
					htmlc->unit_len_ctx.device_dpi);
				htmlc->unit_len_ctx.viewport_height =
					css_unit_device2css_px(
					INTTOFIX(vh),
					htmlc->unit_len_ctx.device_dpi);
			} else {
				htmlc->unit_len_ctx.viewport_width =
					css_unit_device2css_px(
					INTTOFIX(width),
					htmlc->unit_len_ctx.device_dpi);
				htmlc->unit_len_ctx.viewport_height =
					css_unit_device2css_px(
					INTTOFIX(height),
					htmlc->unit_len_ctx.device_dpi);
			}
		}
	#else
		htmlc->unit_len_ctx.viewport_width = css_unit_device2css_px(
				INTTOFIX(width), htmlc->unit_len_ctx.device_dpi);
		htmlc->unit_len_ctx.viewport_height = css_unit_device2css_px(
				INTTOFIX(height), htmlc->unit_len_ctx.device_dpi);
	#endif
	htmlc->unit_len_ctx.root_style = htmlc->layout->style;

	/* Collapse fix (Phase 1): force ONE tree-wide minmax recompute on the
	 * first fully-settled reformat. Intrinsic minmax frozen while fetches
	 * were still outstanding (undecoded images / not-yet-active fonts each
	 * measured at 0) collapses cells to min-content (one char per line) and
	 * overflows their boxes (spurious scrollbars). This re-measures the whole
	 * tree against now-settled resources exactly once, deterministically -
	 * fonts have no completion hook of their own, so the settle pass is what
	 * covers them. */
	/* Collapse fix (Phase 1) NEUTRALISED (fixes571). The tree-wide minmax
	 * resettle diverges from the incremental (object.c spine) recompute: on a
	 * cache-hit RETURN to a page it recomputes the two-column region as
	 * STACKED (sidebar pushed below the content) - proven in the log where
	 * reformat #3's `MINMAX resettle` grows c_h 4543->4642. That is a worse
	 * regression than the cold-load char-per-line it was meant to fix, so the
	 * invalidation is disabled pending a redesign (the latch field, the
	 * invalidator, and its prototype are retained for that work). */
	(void) htmlc->minmax_measured_while_active;

	MS_LOG("html_reformat: pre-layout_document");
	{
		/* fixes366i/j - bracket one layout_document pass. Count
		 * QuickDraw font-measure calls AND capture heap free / largest
		 * contiguous block before and after, so we can tell whether the
		 * escalating per-pass cost (5s -> 239s on a stable 1993-box
		 * tree, with font-measure flat at ~968 calls) is a per-reformat
		 * leak and/or Memory Manager compaction thrash rather than text
		 * measurement. */
		extern long macos9_font_measure_calls;
		extern long macos9_font_measure_chars;
		extern long macos9_heap_free_bytes(void);
		extern long macos9_heap_max_block(void);
		extern double macos9_micros(void);
		extern void macsurf_debug_log_writef(const char *fmt, ...);
		long free_before = macos9_heap_free_bytes();
		long max_before = macos9_heap_max_block();
		double t0 = macos9_micros();
		long free_after;
		long max_after;
		long layout_us;
		macos9_font_measure_calls = 0;
		macos9_font_measure_chars = 0;
		layout_document(htmlc, width, height);
		layout_us = (long)(macos9_micros() - t0);
		free_after = macos9_heap_free_bytes();
		max_after = macos9_heap_max_block();
		/* fixes366k/l - one combined line. layout_us = the ACTUAL time
		 * inside layout_document (the layout-done stamp delta is
		 * misleading; it spans inter-reformat network, dominated by TLS
		 * handshakes on cross-host image fetches). free/maxblk =
		 * app-heap free + largest contiguous block before->after. */
		/* fixes848 (#167 perf investigation) -- see html_box_convert_done's
		 * comment. This line already computed the real elapsed time
		 * inside layout_document via macos9_micros(); it just wasn't
		 * WORK-prefixed, so it never survived the release filter. */
		macsurf_debug_log_writef(
			"WORK LAYPROF layout_us=%ld mcalls=%ld mchars=%ld free=%ld->%ld maxblk=%ld->%ld",
			layout_us,
			macos9_font_measure_calls,
			macos9_font_measure_chars,
			free_before, free_after, max_before, max_after);
		/* fixes640 - feed the honest per-load layout accumulator + reflow
		 * count. layout_us is the ACTUAL time inside layout_document (the
		 * layout-done stamp delta spans inter-reformat network idle). */
		{
			extern void macsurf_profile_accum_layout(long us);
			extern void macsurf_profile_note_reflow(void);
			macsurf_profile_accum_layout(layout_us);
			macsurf_profile_note_reflow();
		}
	}
	MS_LOG("html_reformat: post-layout_document");
	macsurf_profile_stamp("layout-done");
	layout = htmlc->layout;

	/* width and height are at least margin box of document */
	c->width = layout->x + layout->padding[LEFT] + layout->width +
		layout->padding[RIGHT] + layout->border[RIGHT].width +
		layout->margin[RIGHT];
	c->height = layout->y + layout->padding[TOP] + layout->height +
		layout->padding[BOTTOM] + layout->border[BOTTOM].width +
		layout->margin[BOTTOM];

	/* if boxes overflow right or bottom edge, expand to contain it.
	 * fixes625: backstop the descendant extent against a garbage/overflow
	 * value (1000000 px -- the same ceiling as LAYOUT_SAFE_MAX, inlined
	 * here because layout_safe.h is not on html.c's include path). The
	 * real cause is fixed in layout_get_box_bbox + the flex place-site,
	 * but this guarantees a regression can never again surface a
	 * ~2.1-billion-px content width (the "split scrollbar": a giant empty
	 * canvas beside the real page). Mirrors the documented redraw.c
	 * +-200000 defensive-clamp gotcha. */
	/* fixes990 - remember the PRE-clamp extent. The diagnostic below is
	 * gated on `descendant_x1 > 1000000`, but this clamp runs first and
	 * pins it to exactly 1000000, so that gate has never once been true:
	 * the box walk that exists to name the split-scrollbar culprit has
	 * been dead code since it was written. Worse, the clamp does not fix
	 * the symptom -- a 1,000,000px canvas is still a giant empty page with
	 * a spurious horizontal scrollbar -- it only removes the INT_MAX
	 * signature everyone greps for. */
	{
		int pre_dx1 = (layout != NULL) ? (int)layout->descendant_x1 : 0;
		g_macsurf_pre_clamp_dx1 = pre_dx1;
	}
	if (layout->descendant_x1 > 1000000)
		layout->descendant_x1 = 1000000;
	if (layout->descendant_y1 > 1000000)
		layout->descendant_y1 = 1000000;
	if (c->width < layout->x + layout->descendant_x1)
		c->width = layout->x + layout->descendant_x1;
	if (c->height < layout->y + layout->descendant_y1)
		c->height = layout->y + layout->descendant_y1;

	/* fixes311 -- post-layout dimensions probe. */
	macsurf_debug_log_writef(
		"reformat: in_w=%d in_h=%d c_w=%d c_h=%d desc_x1=%d desc_y1=%d lyt_xy=%d,%d lyt_wh=%d,%d",
		width, height, (int)c->width, (int)c->height,
		(int)layout->descendant_x1, (int)layout->descendant_y1,
		(int)layout->x, (int)layout->y,
		(int)layout->width, (int)layout->height);

	/* fixes624 DIAG - when descendant_x1 blows up to ~INT_MAX (the
	 * tinkerdifferent "split": content is 949 wide but one box overflows
	 * to 2147483647, making the canvas that wide with empty base beside
	 * the content), walk the tree and log the boxes whose OWN x/width is
	 * garbage (the leak source, not the ancestors that merely inherit the
	 * bad descendant_x1). Iterative + bounded so it can't blow the OS 9
	 * stack. One-shot per session. */
	if (layout != NULL && (g_macsurf_pre_clamp_dx1 > 1000000 ||
			g_macsurf_pre_clamp_dx1 < -1000000 ||
			(c->width > 3000 && c->width > width * 2))) {
		static int td_garbage_dumped = 0;
		/* fixes990 - announce it whether or not the walk has run before,
		 * so a RECURRENCE is visible and not just the first instance.
		 * "LIFE " because the failures-only gate drops everything else,
		 * which is why this has been invisible. Anomaly-gated, so a
		 * healthy page logs nothing.
		 *
		 * fixes990b - the width gate is deliberately GENEROUS (twice the
		 * viewport and over 3000px, rather than 20000): a page that is
		 * legitimately twice its viewport wide is already worth seeing,
		 * and the cost of a false positive is one log line, while the
		 * cost of a gate set too high is another whole round. */
		macsurf_debug_log_writef(
			"LIFE SPLIT c_w=%d in_w=%d dx1_preclamp=%d dx1=%d lyt_w=%d",
			(int)c->width, width, g_macsurf_pre_clamp_dx1,
			(int)layout->descendant_x1, (int)layout->width);
		if (td_garbage_dumped == 0) {
			struct box *stack[256];
			int sp = 0;
			int found = 0;
			td_garbage_dumped = 1;
			stack[sp++] = layout;
			while (sp > 0 && found < 14) {
				struct box *bx = stack[--sp];
				struct box *ch;
				if (bx == NULL)
					continue;
				if (bx->width > 1000000 || bx->width < -1000000 ||
						bx->x > 1000000 ||
						bx->x < -1000000) {
					macsurf_debug_log_writef(
						"LIFE GARBAGEBOX type=%d x=%d w=%d dx1=%d mL=%d mR=%d",
						(int)bx->type, (int)bx->x,
						(int)bx->width,
						(int)bx->descendant_x1,
						(int)bx->margin[LEFT],
						(int)bx->margin[RIGHT]);
					found++;
				}
				for (ch = bx->children;
						ch != NULL && sp < 256;
						ch = ch->next) {
					stack[sp++] = ch;
				}
			}
		}
	}

	/* fixes1139 DIAG - THE GIANT EMPTY SPACE.
	 *
	 * Hardware, 68kmla: the document reports h=7750 while the visible content
	 * (DIV.p-body) ends at y=4683 -- roughly 3000px of blank canvas below the
	 * page, and the scrollbar scrolls into it. c->height comes from
	 * layout->descendant_y1 just above, so ONE box is overhanging; the SPLIT
	 * walk above answers the same question for the X axis and this is its Y
	 * twin.
	 *
	 * Gate: the deepest box bottom is found first, then the walk names every
	 * box whose own bottom edge lands in the last 15% of the document -- the
	 * boxes that actually DEFINE the bottom. A page whose content genuinely
	 * reaches its full height names its real last elements (cheap, one-shot);
	 * a page with phantom space names the overhanging box directly.
	 *
	 * Iterative + bounded (no recursion, 256-deep stack, 14-line cap) so it
	 * cannot blow the OS 9 stack. One-shot per navigation. */
	if (layout != NULL && c->height > 0) {
		static void *tall_dumped_c = NULL;
		if ((void *)c != tall_dumped_c) {
			struct box *stack[256];
			int sp = 0;
			int found = 0;
			int deep_content = 0;
			int cut;
			/* Pass 1: deepest bottom of a box that CARRIES content
			 * (text, or a replaced object/image). That is where the
			 * page visibly ends. */
			stack[sp++] = layout;
			while (sp > 0) {
				struct box *bx = stack[--sp];
				struct box *ch;
				if (bx == NULL) continue;
				if ((bx->text != NULL || bx->object != NULL)) {
					int bot = (int)bx->y + (int)bx->height;
					if (bot > deep_content && bot < 1000000)
						deep_content = bot;
				}
				for (ch = bx->children; ch != NULL && sp < 256;
						ch = ch->next)
					stack[sp++] = ch;
			}
			cut = (int)c->height - ((int)c->height / 7);
			macsurf_debug_log_writef(
				"LIFE TALL c_h=%d dy1=%d content_bottom=%d gap=%d",
				(int)c->height, (int)layout->descendant_y1,
				deep_content, (int)c->height - deep_content);
			/* Pass 2: name the boxes defining the bottom. */
			sp = 0;
			stack[sp++] = layout;
			while (sp > 0 && found < 14) {
				struct box *bx = stack[--sp];
				struct box *ch;
				int bot;
				if (bx == NULL) continue;
				bot = (int)bx->y + (int)bx->height;
				if (bot >= cut && bot < 1000000) {
					char nm[256];
					nm[0] = '\0';
					if (bx->node != NULL)
						html_pagemap_brief(bx->node, nm,
								(int)sizeof nm);
					macsurf_debug_log_writef(
						"LIFE TALLBOX %s type=%d y=%d h=%d bot=%d dy1=%d txt=%d obj=%d kids=%d",
						nm[0] ? nm : "(no node)",
						(int)bx->type, (int)bx->y,
						(int)bx->height, bot,
						(int)bx->descendant_y1,
						bx->text != NULL,
						bx->object != NULL,
						bx->children != NULL);
					found++;
				}
				for (ch = bx->children; ch != NULL && sp < 256;
						ch = ch->next)
					stack[sp++] = ch;
			}
			tall_dumped_c = (void *)c;
		}
	}

	/* fixes1294 (#167, C0.2) - LOCATE THE BLANK BAND, take 2.
	 *
	 * fixes1293's first hardware run (2026-08-23, all three of ptricky3/
	 * kaija.prohofsky/minnesotastatefair) exposed two bugs in the
	 * diagnostic itself, not new facts about Facebook:
	 *
	 * 1. The occupancy map only covered the first 3000px of the document,
	 *    but these pages are 22000-25000px tall -- every gap it found was
	 *    reported as "y1=3000" because the window ran out, not because the
	 *    empty region actually ended there. Fix: size the map to the WHOLE
	 *    document (c->height), with the per-bucket pixel width scaled up
	 *    so the fixed-size bucket array still covers it.
	 * 2. Pass 2's "any box intersecting the gap, capped at 8" walk just
	 *    re-printed the top of the ancestor chain every time (HTML -> BODY
	 *    -> mount point -> generic wrapper divs), because EVERY ancestor of
	 *    literally any content trivially "intersects" a gap near the top of
	 *    a 22000px document -- it never reached anything specific. Its
	 *    `kids=` field was also a bare boolean (children != NULL), not a
	 *    count, so "kids=1" on every line looked like a suspicious
	 *    single-child chain when it was just "has any children at all."
	 *    Fix: DRILL DOWN instead of flat-searching. From the root, at each
	 *    level look at which of the box's DIRECT children overlap the gap.
	 *    Exactly one overlapping child -> that child is still just part of
	 *    the ancestor spine, descend into it. Zero or multiple overlapping
	 *    children -> stop: this is the box actually responsible for (or
	 *    branching across) the gap. Report it (owner, with a real bounded
	 *    child COUNT) plus every child that overlaps the gap (branch
	 *    candidates, same fields). Reading it: owner has zero overlapping
	 *    children but a nonzero total child count -> its real children are
	 *    positioned outside the gap (margin/flow, or off in a part of the
	 *    tree that never reaches these coordinates) -- not a mount gap.
	 *    Owner has zero children at all -> genuine empty leaf -> JS/mount
	 *    gap. Multiple overlapping children -> look at what's reported for
	 *    each: real content-bearing ones with nothing rendered points at
	 *    clip/visibility; empty ones with big height/min-height points at
	 *    CSS/layout.
	 *
	 * Same one-shot-per-navigation, bounded-depth (64), capped-output
	 * discipline as fixes1293 -- cannot blow the OS 9 stack or flood the
	 * log. Diagnostic only: still no CSS or JS theory assumed. */
	if (layout != NULL && c->height > 0) {
		static void *fbgap_dumped_c = NULL;
		if ((void *)c != fbgap_dumped_c) {
#define FBGAP_BUCKETS 300
#define FBGAP_MIN_RUN_PX 150
#define FBGAP_MAX_GAPS 3
#define FBGAP_MAX_BRANCH 12
			unsigned char occ[FBGAP_BUCKETS];
			struct box *stack[256];
			int sp;
			int i;
			int gap_y0[FBGAP_MAX_GAPS];
			int gap_y1[FBGAP_MAX_GAPS];
			int gap_count;
			int bucket_px;

			/* Cover the WHOLE document with a fixed-size bucket array
			 * by scaling the bucket width up to fit. */
			bucket_px = ((int)c->height / FBGAP_BUCKETS) + 1;
			if (bucket_px < 1) bucket_px = 1;

			for (i = 0; i < FBGAP_BUCKETS; i++) occ[i] = 0;

			/* Pass 1: mark occupancy from content-bearing boxes'
			 * GLOBAL bounds. */
			sp = 0;
			stack[sp++] = layout;
			while (sp > 0) {
				struct box *bx = stack[--sp];
				struct box *ch;
				if (bx == NULL) continue;
				if (bx->text != NULL || bx->object != NULL) {
					int gx = 0, gy = 0;
					int b0, b1, bi;
					box_coords(bx, &gx, &gy);
					b0 = gy / bucket_px;
					b1 = (gy + (int)bx->height) / bucket_px;
					if (b0 < 0) b0 = 0;
					if (b1 >= FBGAP_BUCKETS)
						b1 = FBGAP_BUCKETS - 1;
					for (bi = b0; bi <= b1; bi++) {
						if (bi >= 0 && bi < FBGAP_BUCKETS)
							occ[bi] = 1;
					}
				}
				for (ch = bx->children; ch != NULL && sp < 256;
						ch = ch->next)
					stack[sp++] = ch;
			}

			/* Find empty runs >= FBGAP_MIN_RUN_PX, in document order,
			 * capped at FBGAP_MAX_GAPS. */
			gap_count = 0;
			i = 0;
			while (i < FBGAP_BUCKETS && gap_count < FBGAP_MAX_GAPS) {
				int run_start;
				int run_len;
				if (occ[i]) { i++; continue; }
				run_start = i;
				while (i < FBGAP_BUCKETS && !occ[i]) i++;
				run_len = i - run_start;
				if (run_len * bucket_px >= FBGAP_MIN_RUN_PX) {
					gap_y0[gap_count] = run_start * bucket_px;
					gap_y1[gap_count] = i * bucket_px;
					macsurf_debug_log_writef(
						"LIFE FBGAP y0=%d y1=%d h=%d bucket_px=%d",
						gap_y0[gap_count], gap_y1[gap_count],
						gap_y1[gap_count] - gap_y0[gap_count],
						bucket_px);
					gap_count++;
				}
			}

			/* Pass 2: for each gap, DRILL DOWN from the root through
			 * whichever single child still covers the gap, until we
			 * reach a box with zero or multiple overlapping children
			 * -- the box actually responsible for the gap, not just
			 * another link in the ancestor spine. */
			for (i = 0; i < gap_count; i++) {
				struct box *cur = layout;
				struct box *branch[FBGAP_MAX_BRANCH];
				int branch_n;
				int depth;

				for (depth = 0; depth < 64; depth++) {
					struct box *ch;
					branch_n = 0;
					for (ch = cur->children; ch != NULL;
							ch = ch->next) {
						int cgx = 0, cgy = 0;
						int cb0, cb1;
						box_coords(ch, &cgx, &cgy);
						cb0 = cgy;
						cb1 = cgy + (int)ch->height;
						if (cb1 > gap_y0[i] &&
								cb0 < gap_y1[i]) {
							if (branch_n < FBGAP_MAX_BRANCH)
								branch[branch_n] = ch;
							branch_n++;
						}
					}
					if (branch_n == 1) {
						cur = branch[0];
						continue;
					}
					break;
				}

				/* Report the owner: the box the descent stopped at,
				 * with a REAL bounded total child count (not the old
				 * boolean) so "zero overlapping children" can be told
				 * apart from "zero children at all". fixes1295 (C0.3)
				 * adds actual height/min-height PIXEL values (not just
				 * set/auto -- a min-height that's SET but small isn't
				 * the same finding as one set to thousands of px) plus
				 * flex-grow/basis/align, since both owners in the
				 * fixes1294 hardware run were flex containers (type=14)
				 * with mht=set and it matters whether that min-height
				 * value is what's actually driving the box's oversized
				 * height, or whether a flex-grow/align-items mismatch
				 * is the real cause. */
				{
					int gx = 0, gy = 0;
					int total_kids = 0;
					struct box *ch;
					char nm[256];
					const char *disp = "-";
					const char *ht = "auto";
					const char *mht = "auto";
					int pos = -1;
					int htpx = -1, mhtpx = -1;
					int grow_x100 = -1;
					int basis_px = -1;
					int align_items = -1, align_self = -1;
					int flex_dir = -1;

					box_coords(cur, &gx, &gy);
					for (ch = cur->children;
							ch != NULL && total_kids < 999;
							ch = ch->next)
						total_kids++;

					nm[0] = '\0';
					if (cur->node != NULL)
						html_pagemap_brief(cur->node, nm,
								(int)sizeof nm);
					if (cur->style != NULL) {
						css_fixed hv = 0, mhv = 0, gv = 0, bv = 0;
						css_unit hu = CSS_UNIT_PX,
							 mhu = CSS_UNIT_PX,
							 bu = CSS_UNIT_PX;
						disp = (css_computed_display_static(
								cur->style) ==
								CSS_DISPLAY_NONE) ?
								"NONE" : "ok";
						if (css_computed_height(cur->style,
								&hv, &hu) == CSS_HEIGHT_SET) {
							ht = "set";
							if (hu == CSS_UNIT_PX)
								htpx = (int) FIXTOINT(hv);
						}
						if (css_computed_min_height(cur->style,
								&mhv, &mhu) ==
								CSS_MIN_HEIGHT_SET) {
							mht = "set";
							if (mhu == CSS_UNIT_PX)
								mhtpx = (int) FIXTOINT(mhv);
						}
						pos = (int) css_computed_position(
								cur->style);
						if (css_computed_flex_grow(cur->style,
								&gv) == CSS_FLEX_GROW_SET)
							grow_x100 = (int)
								(((long)gv * 100) >>
								 CSS_RADIX_POINT);
						if (css_computed_flex_basis(cur->style,
								&bv, &bu) ==
								CSS_FLEX_BASIS_SET &&
								bu == CSS_UNIT_PX)
							basis_px = (int) FIXTOINT(bv);
						align_items = (int)
							css_computed_align_items(
									cur->style);
						align_self = (int)
							css_computed_align_self(
									cur->style);
						flex_dir = (int)
							css_computed_flex_direction(
									cur->style);
					}
					macsurf_debug_log_writef(
						"LIFE FBGAPOWNER box=%p gap=%d %s type=%d x=%d y=%d w=%d h=%d totalkids=%d overlapkids=%d txt=%d obj=%d disp=%s pos=%d ht=%s(%d) mht=%s(%d) grow=%d basis=%d align=%d/%d dir=%d",
						(void *)cur, i,
						nm[0] ? nm : "(no node)",
						(int)cur->type, gx, gy,
						(int)cur->width, (int)cur->height,
						total_kids, branch_n,
						cur->text != NULL,
						cur->object != NULL,
						disp, pos, ht, htpx, mht, mhtpx,
						grow_x100, basis_px,
						align_items, align_self, flex_dir);
				}

				/* Report each overlapping child (branch candidates),
				 * capped at FBGAP_MAX_BRANCH. */
				{
					int bi;
					int cap = branch_n;
					if (cap > FBGAP_MAX_BRANCH)
						cap = FBGAP_MAX_BRANCH;
					for (bi = 0; bi < cap; bi++) {
						struct box *bx = branch[bi];
						int gx = 0, gy = 0;
						int total_kids = 0;
						struct box *ch;
						char nm[256];
						const char *disp = "-";
						const char *ht = "auto";
						const char *mht = "auto";
						int pos = -1;
						int htpx = -1, mhtpx = -1;
						int grow_x100 = -1;
						int basis_px = -1;
						int align_items = -1, align_self = -1;

						box_coords(bx, &gx, &gy);
						for (ch = bx->children;
								ch != NULL && total_kids < 999;
								ch = ch->next)
							total_kids++;

						nm[0] = '\0';
						if (bx->node != NULL)
							html_pagemap_brief(bx->node,
									nm, (int)sizeof nm);
						if (bx->style != NULL) {
							css_fixed hv = 0, mhv = 0,
								  gv = 0, bv = 0;
							css_unit hu = CSS_UNIT_PX,
								 mhu = CSS_UNIT_PX,
								 bu = CSS_UNIT_PX;
							disp = (css_computed_display_static(
									bx->style) ==
									CSS_DISPLAY_NONE) ?
									"NONE" : "ok";
							if (css_computed_height(
									bx->style, &hv, &hu)
									== CSS_HEIGHT_SET) {
								ht = "set";
								if (hu == CSS_UNIT_PX)
									htpx = (int)
										FIXTOINT(hv);
							}
							if (css_computed_min_height(
									bx->style, &mhv,
									&mhu) ==
									CSS_MIN_HEIGHT_SET) {
								mht = "set";
								if (mhu == CSS_UNIT_PX)
									mhtpx = (int)
										FIXTOINT(mhv);
							}
							pos = (int) css_computed_position(
									bx->style);
							if (css_computed_flex_grow(
									bx->style, &gv) ==
									CSS_FLEX_GROW_SET)
								grow_x100 = (int)
									(((long)gv * 100) >>
									 CSS_RADIX_POINT);
							if (css_computed_flex_basis(
									bx->style, &bv, &bu) ==
									CSS_FLEX_BASIS_SET &&
									bu == CSS_UNIT_PX)
								basis_px = (int)
									FIXTOINT(bv);
							align_items = (int)
								css_computed_align_items(
										bx->style);
							align_self = (int)
								css_computed_align_self(
										bx->style);
						}
						macsurf_debug_log_writef(
							"LIFE FBGAPBRANCH gap=%d %s type=%d x=%d y=%d w=%d h=%d totalkids=%d txt=%d obj=%d disp=%s pos=%d ht=%s(%d) mht=%s(%d) grow=%d basis=%d align=%d/%d",
							i, nm[0] ? nm : "(no node)",
							(int)bx->type, gx, gy,
							(int)bx->width, (int)bx->height,
							total_kids,
							bx->text != NULL,
							bx->object != NULL,
							disp, pos, ht, htpx, mht, mhtpx,
							grow_x100, basis_px,
							align_items, align_self);
					}
				}
			}
#undef FBGAP_BUCKETS
#undef FBGAP_MIN_RUN_PX
#undef FBGAP_MAX_GAPS
#undef FBGAP_MAX_BRANCH
			fbgap_dumped_c = (void *)c;
		}
	}

	/* fixes160a - SITE summary line. Emits one compact, grep-friendly
	 * line per page reformat with the box-tree counters stashed at
	 * box_convert time plus the just-computed content dimensions and
	 * image success/fail counts. Used by the modern-site gauntlet
	 * (tests/sites/modern_gauntlet.md). HTTP status/bytes/proxy live
	 * in the `http: done ...` line earlier in the log against the
	 * matching URL - not folded in here because there are many fetches
	 * per page (HTML, CSS, images) and we'd need URL-matching to pick
	 * the right one. */
	{
		extern long macsurf__site_box_total;
		extern long macsurf__site_box_blk;
		extern long macsurf__site_box_inlinec;
		extern long macsurf__site_box_inline;
		extern long macsurf__site_box_text;
		extern long macsurf__site_box_other;
		extern long macsurf__site_img_ok;
		extern long macsurf__site_img_fail;
		extern long macsurf__site_css_ok;
		extern long macsurf__site_css_skip;
		extern long macsurf__site_rgov_skip_doc;
		extern long macsurf__site_rgov_skip_css;
		extern long macsurf__site_rgov_skip_img;
		extern long macsurf__site_rgov_skip_script;
		extern long macsurf__site_rgov_skip_font;
		extern long macsurf__site_rgov_skip_other;
		extern long macsurf__site_fetch_active_peak;
		extern long macsurf__site_fetch_slot_fail;
		extern long macsurf__site_heavy;
		extern long macsurf__decoded_img_bytes_current;
		extern long macsurf__site_decoded_img_bytes_peak;
		extern long macsurf__site_decoded_img_skip_budget;
		extern unsigned long macsurf__site_css_total_bytes;
		extern long macsurf__site_blocker;
		nsurl *u = content_get_url(&htmlc->base);
		const char *url = (u != NULL) ? nsurl_access(u) : "(null)";
		const char *blocker_name;
		const unsigned long css_total_cap = 8192UL * 1024UL; /* fixes448 - keep in sync with MACOS9_CSS_TOTAL_BUDGET in cssh_css.c */

		/* fixes268 (#11) - pick the dominant degradation source by
		 * comparing skip counters. Highest-priority counter wins; ties
		 * resolved in declaration order. heavy=1 latches when any
		 * skip counter is non-zero (degradation actually happened). */
		if (macsurf__site_decoded_img_skip_budget > 0 ||
		    macsurf__site_rgov_skip_img > 0) {
			macsurf__site_blocker = 1;   /* img_budget */
			macsurf__site_heavy = 1;
		} else if (macsurf__site_css_skip > 0) {
			macsurf__site_blocker = 2;   /* css_budget */
			macsurf__site_heavy = 1;
		} else if (macsurf__site_fetch_slot_fail > 0 ||
			   macsurf__site_rgov_skip_doc > 0 ||
			   macsurf__site_rgov_skip_css > 0 ||
			   macsurf__site_rgov_skip_script > 0 ||
			   macsurf__site_rgov_skip_other > 0) {
			macsurf__site_blocker = 3;   /* fetch_slots */
			macsurf__site_heavy = 1;
		} else if (macsurf__site_rgov_skip_font > 0) {
			macsurf__site_blocker = 4;   /* fonts */
			macsurf__site_heavy = 1;
		}
		switch ((int)macsurf__site_blocker) {
		case 1: blocker_name = "img_budget"; break;
		case 2: blocker_name = "css_budget"; break;
		case 3: blocker_name = "fetch_slots"; break;
		case 4: blocker_name = "fonts"; break;
		default: blocker_name = "none"; break;
		}

		macsurf_debug_log_writef(
			"SITE url=\"%s\" heavy=%ld blocker=%s "
			"boxes=%ld blk=%ld inlinec=%ld inline=%ld text=%ld other=%ld "
			"in_w=%d in_h=%d c_w=%d c_h=%d "
			"img_ok=%ld img_fail=%ld css_ok=%ld css_skip=%ld "
			"css_total=%ld/%ld "
			"rgov_skip=doc/%ld,css/%ld,img/%ld,scr/%ld,fnt/%ld,oth/%ld "
			"fetch_peak=%ld fetch_slot_fail=%ld "
			"decoded_img=cur/%ld,peak/%ld,skip/%ld",
			url, macsurf__site_heavy, blocker_name,
			macsurf__site_box_total,
			macsurf__site_box_blk, macsurf__site_box_inlinec,
			macsurf__site_box_inline, macsurf__site_box_text,
			macsurf__site_box_other,
			width, height, (int)c->width, (int)c->height,
			macsurf__site_img_ok, macsurf__site_img_fail,
			macsurf__site_css_ok, macsurf__site_css_skip,
			(long)macsurf__site_css_total_bytes, (long)css_total_cap,
			macsurf__site_rgov_skip_doc, macsurf__site_rgov_skip_css,
			macsurf__site_rgov_skip_img, macsurf__site_rgov_skip_script,
			macsurf__site_rgov_skip_font, macsurf__site_rgov_skip_other,
			macsurf__site_fetch_active_peak,
			macsurf__site_fetch_slot_fail,
			macsurf__decoded_img_bytes_current,
			macsurf__site_decoded_img_bytes_peak,
			macsurf__site_decoded_img_skip_budget);
	}

	selection_reinit(htmlc->sel);

	htmlc->reflowing = false;
	htmlc->had_initial_layout = true;

	/* calculate next reflow time at three times what it took to reflow */
	nsu_getmonotonic_ms(&ms_after);

	/* fixes352 (#107) - stash the last reformat duration so about:perf
	 * can render real numbers instead of a placeholder. Single-threaded
	 * cooperative app; no overlap between pages. */
#ifdef __MACOS9__
	{
		extern long macsurf__site_reformat_ms;
		macsurf__site_reformat_ms = (long)(ms_after - ms_before);
	}
#endif

	ms_interval = (ms_after - ms_before) * 3;
	if (ms_interval < (nsoption_uint(min_reflow_period) * 10)) {
		ms_interval = nsoption_uint(min_reflow_period) * 10;
	}
	c->reformat_time = ms_after + ms_interval;

	MS_LOG("html_reformat: exit");
}


/**
 * Redraw a box.
 *
 * \param  h	content containing the box, of type CONTENT_HTML
 * \param  box  box to redraw
 */

void html_redraw_a_box(hlcache_handle *h, struct box *box)
{
	int x, y;

	box_coords(box, &x, &y);

	content_request_redraw(h, x, y,
			box->padding[LEFT] + box->width + box->padding[RIGHT],
			box->padding[TOP] + box->height + box->padding[BOTTOM]);
}


/**
 * Redraw a box.
 *
 * \param html  content containing the box, of type CONTENT_HTML
 * \param box  box to redraw.
 */

void html__redraw_a_box(struct html_content *html, struct box *box)
{
	int x, y;

	box_coords(box, &x, &y);

	content__request_redraw((struct content *)html, x, y,
			box->padding[LEFT] + box->width + box->padding[RIGHT],
			box->padding[TOP] + box->height + box->padding[BOTTOM]);
}

static void html_destroy_frameset(struct content_html_frames *frameset)
{
	int i;

	if (frameset->name) {
		talloc_free(frameset->name);
		frameset->name = NULL;
	}
	if (frameset->url) {
		talloc_free(frameset->url);
		frameset->url = NULL;
	}
	if (frameset->children) {
		for (i = 0; i < (frameset->rows * frameset->cols); i++) {
			if (frameset->children[i].name) {
				talloc_free(frameset->children[i].name);
				frameset->children[i].name = NULL;
			}
			if (frameset->children[i].url) {
				nsurl_unref(frameset->children[i].url);
				frameset->children[i].url = NULL;
			}
			if (frameset->children[i].children)
				html_destroy_frameset(&frameset->children[i]);
		}
		talloc_free(frameset->children);
		frameset->children = NULL;
	}
}

static void html_destroy_iframe(struct content_html_iframe *iframe)
{
	struct content_html_iframe *next;
	next = iframe;
	while ((iframe = next) != NULL) {
		next = iframe->next;
		if (iframe->name)
			talloc_free(iframe->name);
		if (iframe->url) {
			nsurl_unref(iframe->url);
			iframe->url = NULL;
		}
		talloc_free(iframe);
	}
}


static void html_free_layout(html_content *htmlc)
{
	extern const char *macsurf_talloc_free_ctx;

	if (htmlc->bctx != NULL) {
		/* freeing talloc context should let the entire box
		 * set be destroyed
		 */
		/* fixes907 -- label this free so a TALLOC_ABORT during it names the
		 * path. The hardware crash fires on nav-away right after NAV DONE,
		 * i.e. destroying a page whose box tree was rebuilt by reconvert; if
		 * the double-free lands here the reconverted live tree still holds an
		 * already-freed (double-linked) box. bctx=%p correlates with the
		 * reconvert "old tree" pointer. */
		macsurf_debug_log_writef(
			"TALLOC-INFO free-layout bctx=%p (during=content-destroy)",
			(void *)htmlc->bctx);
		macsurf_talloc_free_ctx = "content-destroy-bctx";
		talloc_free(htmlc->bctx);
		macsurf_talloc_free_ctx = "(none)";
	}
}

/**
 * Destroy a CONTENT_HTML and free all resources it owns.
 */

static void html_destroy(struct content *c)
{
	html_content *html = (html_content *) c;
	struct form *f, *g;

	macsurf_debug_log_writef("html_destroy: htmlc=%p content=%p", (void*)html, (void*)c);
	NSLOG(netsurf, INFO, "content %p", c);

	/* fixes502: set aborted before cancel so that if convert_xml_to_box
	 * was already dequeued by the scheduler (cancel_dom_to_box is then a
	 * no-op) the callback still checks aborted and returns immediately
	 * rather than walking a freed select_ctx. */
	html->aborted = true;

	/* fixes518: cancel EVERY scheduled callback keyed on this html_content
	 * (deferred_parser_unpause, html_css_process_modified_styles, ...) before
	 * the parser/document/objects below are freed.  Without this, a queued
	 * deferred_parser_unpause fires a tick later, reads parent->parser out of
	 * the freed+reused html_content, and resumes a garbage parser inside the
	 * hubbub tokenizer (crash sig: lbzu through r4=0/1, r3=reuse garbage). */
	macos9_schedule_cancel_owner(html);

	/* If we're still converting a layout, cancel it.
	 * fixes499g - NULL the context after cancel so a second html_destroy
	 * (the double-destroy via hlcache_clean that this whole crash family
	 * stems from) does not re-cancel an already-freed conversion context.
	 * cancel_dom_to_box removes the scheduled convert_xml_to_box entry and
	 * frees the ctx, so the pointer is dangling after; leaving it non-NULL
	 * let a second pass fire cancel on freed memory. */
	if (html->box_conversion_context != NULL) {
		if (cancel_dom_to_box(html->box_conversion_context) != NSERROR_OK) {
			NSLOG(netsurf, CRITICAL, "WARNING, Unable to cancel conversion context, browser may crash");
		}
		html->box_conversion_context = NULL;
	}

	selection_destroy(html->sel);

	/* Destroy forms */
	for (f = html->forms; f != NULL; f = g) {
		g = f->prev;

		form_free(f);
	}

	imagemap_destroy(html);

	if (c->refresh)
		nsurl_unref(c->refresh);

	if (html->base_url)
		nsurl_unref(html->base_url);

	/* At this point we can be moderately confident the JS is offline
	 * so we destroy the JS thread.
	 */
	if (html->js_thread != NULL) {
		js_destroythread(html->js_thread);
		html->js_thread = NULL;
	}

	if (html->parser != NULL) {
		dom_hubbub_parser_destroy(html->parser);
		html->parser = NULL;
	}

	if (html->document != NULL) {
		dom_node_unref(html->document);
		html->document = NULL;
	}

	if (html->title != NULL) {
		dom_node_unref(html->title);
		html->title = NULL;
	}

	/* Free encoding */
	if (html->encoding != NULL) {
		free(html->encoding);
		html->encoding = NULL;
	}

	/* Free base target */
	if (html->base_target != NULL) {
		free(html->base_target);
		html->base_target = NULL;
	}

	/* Free frameset */
	if (html->frameset != NULL) {
		html_destroy_frameset(html->frameset);
		talloc_free(html->frameset);
		html->frameset = NULL;
	}

	/* Free iframes */
	if (html->iframe != NULL) {
		html_destroy_iframe(html->iframe);
		html->iframe = NULL;
	}

	/* Destroy selection context */
	if (html->select_ctx != NULL) {
		css_select_ctx_destroy(html->select_ctx);
		html->select_ctx = NULL;
	}

	lwc_string_unref(html->universal);
	html->universal = NULL;

	lwc_string_unref(html->media.prefers_color_scheme);
	html->media.prefers_color_scheme = NULL;

	/* Free stylesheets */
	html_css_free_stylesheets(html);

	/* Free scripts */
	html_script_free(html);

	/* Free objects */
	html_object_free_objects(html);

	/* free layout */
	html_free_layout(html);
}


static nserror html_clone(const struct content *old, struct content **newc)
{
	/** \todo Clone HTML specifics */

	/* In the meantime, we should never be called, as HTML contents
	 * cannot be shared and we're not intending to fix printing's
	 * cloning of documents. */
	assert(0 && "html_clone should never be called");

	return true;
}


/**
 * Handle a window containing a CONTENT_HTML being opened.
 */

static nserror
html_open(struct content *c,
	  struct browser_window *bw,
	  struct content *page,
	  struct object_params *params)
{
	html_content *html = (html_content *) c;

	html->bw = bw;
	html->page = (html_content *) page;

	html->drag_type = HTML_DRAG_NONE;
	html->drag_owner.no_owner = true;

	/* text selection */
	selection_init(html->sel);
	html->selection_type = HTML_SELECTION_NONE;
	html->selection_owner.none = true;

	html_object_open_objects(html, bw);

	return NSERROR_OK;
}


/**
 * Handle a window containing a CONTENT_HTML being closed.
 */

static nserror html_close(struct content *c)
{
	html_content *htmlc = (html_content *) c;
	nserror ret = NSERROR_OK;

	macsurf_debug_log_writef("html_close: htmlc=%p ctx=%p bw=%p",
		(void*)htmlc,
		(void*)(htmlc != NULL ? htmlc->box_conversion_context : NULL),
		(void*)(htmlc != NULL ? htmlc->bw : NULL));

	/* fixes457/502: set aborted first so that even if the box-convert
	 * callback was already dequeued by the scheduler, it sees aborted=true
	 * and returns without touching freed content state. Then cancel to
	 * prevent it firing at all if still in the queue. */
	htmlc->aborted = true;

	/* fixes518: cancel every scheduled callback keyed on this html_content
	 * here too - html_close runs on navigate-away BEFORE content_destroy, so
	 * a deferred_parser_unpause queued during script load could otherwise
	 * fire in the window between close and destroy. */
	macos9_schedule_cancel_owner(htmlc);

	if (htmlc->box_conversion_context != NULL) {
		cancel_dom_to_box(htmlc->box_conversion_context);
		htmlc->box_conversion_context = NULL;
	}

	selection_clear(htmlc->sel, false);

	/* clear the html content reference to the browser window */
	htmlc->bw = NULL;

	/* remove all object references from the html content */
	html_object_close_objects(htmlc);

	/* fixes572: release + NULL script handles at close, mirroring the
	 * fixes450 block in html_stop's LOADING case. html_close runs for
	 * READY/DONE contents navigated away from; without this the live script
	 * handles ride into the DEFERRED html_destroy (Stage-1 death-row
	 * drain), where the scripts array entry is stale/garbage and
	 * hlcache_handle_release dereferences wild memory (observed handle
	 * 0x07513EC1, unaligned). Releasing here also aborts any pending script
	 * fetch (removes the llcache user), so no late convert_script_*_cb
	 * fires against this closing content. html_script_free's != NULL guard
	 * then skips these at drain (idempotent). */
	{
		unsigned int si;
		for (si = 0; si < htmlc->scripts_count; si++) {
			struct html_script *s = &htmlc->scripts[si];
			if ((s->type == HTML_SCRIPT_SYNC ||
			     s->type == HTML_SCRIPT_ASYNC ||
			     s->type == HTML_SCRIPT_DEFER) &&
			    s->data.handle != NULL) {
				hlcache_handle_release(s->data.handle);
				s->data.handle = NULL;
			}
		}
	}

	if (htmlc->js_thread != NULL) {
		/* Close, but do not destroy (yet) the JS thread */
		ret = js_closethread(htmlc->js_thread);
	}

	return ret;
}


/**
 * Return an HTML content's selection context
 */

static void html_clear_selection(struct content *c)
{
	html_content *html = (html_content *) c;

	switch (html->selection_type) {
	case HTML_SELECTION_NONE:
		/* Nothing to do */
		assert(html->selection_owner.none == true);
		break;
	case HTML_SELECTION_TEXTAREA:
		textarea_clear_selection(html->selection_owner.textarea->
				gadget->data.text.ta);
		break;
	case HTML_SELECTION_SELF:
		assert(html->selection_owner.none == false);
		selection_clear(html->sel, true);
		break;
	case HTML_SELECTION_CONTENT:
		content_clear_selection(html->selection_owner.content->object);
		break;
	default:
		break;
	}

	/* There is no selection now. */
	html->selection_type = HTML_SELECTION_NONE;
	html->selection_owner.none = true;
}


/**
 * Return an HTML content's selection context
 */

static char *html_get_selection(struct content *c)
{
	html_content *html = (html_content *) c;

	switch (html->selection_type) {
	case HTML_SELECTION_TEXTAREA:
		return textarea_get_selection(html->selection_owner.textarea->
				gadget->data.text.ta);
	case HTML_SELECTION_SELF:
		assert(html->selection_owner.none == false);
		return selection_get_copy(html->sel);
	case HTML_SELECTION_CONTENT:
		return content_get_selection(
				html->selection_owner.content->object);
	case HTML_SELECTION_NONE:
		/* Nothing to do */
		assert(html->selection_owner.none == true);
		break;
	default:
		break;
	}

	return NULL;
}


/**
 * Get access to any content, link URLs and objects (images) currently
 * at the given (x, y) coordinates.
 *
 * \param[in] c html content to look inside
 * \param[in] x x-coordinate of point of interest
 * \param[in] y y-coordinate of point of interest
 * \param[out] data Positional features struct to be updated with any
 *             relevent content, or set to NULL if none.
 * \return NSERROR_OK on success else appropriate error code.
 */
static nserror
html_get_contextual_content(struct content *c, int x, int y,
			    struct browser_window_features *data)
{
	html_content *html = (html_content *) c;

	struct box *box = html->layout;
	struct box *next;
	int box_x = 0, box_y = 0;

	while ((next = box_at_point(&html->unit_len_ctx, box, x, y,
			&box_x, &box_y)) != NULL) {
		box = next;

		/* hidden boxes are ignored */
		if ((box->style != NULL) &&
		    css_computed_visibility(box->style) == CSS_VISIBILITY_HIDDEN) {
			continue;
		}

		if (box->iframe) {
			float scale = browser_window_get_scale(box->iframe);
			browser_window_get_features(box->iframe,
						    (x - box_x) * scale,
						    (y - box_y) * scale,
						    data);
		}

		if (box->object)
			content_get_contextual_content(box->object,
					x - box_x, y - box_y, data);

		if (box->object)
			data->object = box->object;

		if (box->href) {
			data->link = box->href;
			data->link_title = box->text;
			data->link_title_length = box->length;
		}

		if (box->usemap) {
			const char *target = NULL;
			nsurl *url = imagemap_get(html, box->usemap, box_x,
					box_y, x, y, &target);
			/* Box might have imagemap, but no actual link area
			 * at point */
			if (url != NULL)
				data->link = url;
		}
		if (box->gadget) {
			switch (box->gadget->type) {
			case GADGET_TEXTBOX:
			case GADGET_TEXTAREA:
			case GADGET_PASSWORD:
				data->form_features = CTX_FORM_TEXT;
				break;

			case GADGET_FILE:
				data->form_features = CTX_FORM_FILE;
				break;

			default:
				data->form_features = CTX_FORM_NONE;
				break;
			}
		}
	}
	return NSERROR_OK;
}


/**
 * Scroll deepest thing within the content which can be scrolled at given point
 *
 * \param c	html content to look inside
 * \param x	x-coordinate of point of interest
 * \param y	y-coordinate of point of interest
 * \param scrx	number of px try to scroll something in x direction
 * \param scry	number of px try to scroll something in y direction
 * \return true iff scroll was consumed by something in the content
 */
static bool
html_scroll_at_point(struct content *c, int x, int y, int scrx, int scry)
{
	html_content *html = (html_content *) c;

	struct box *box = html->layout;
	struct box *next;
	int box_x = 0, box_y = 0;
	bool handled_scroll = false;

	/* TODO: invert order; visit deepest box first */

	while ((next = box_at_point(&html->unit_len_ctx, box, x, y,
			&box_x, &box_y)) != NULL) {
		box = next;

		if (box->style && css_computed_visibility(box->style) ==
				CSS_VISIBILITY_HIDDEN)
			continue;

		/* Pass into iframe */
		if (box->iframe) {
			float scale = browser_window_get_scale(box->iframe);

			if (browser_window_scroll_at_point(box->iframe,
							   (x - box_x) * scale,
							   (y - box_y) * scale,
							   scrx, scry) == true)
				return true;
		}

		/* Pass into textarea widget */
		if (box->gadget && (box->gadget->type == GADGET_TEXTAREA ||
				box->gadget->type == GADGET_PASSWORD ||
				box->gadget->type == GADGET_TEXTBOX) &&
				textarea_scroll(box->gadget->data.text.ta,
						scrx, scry) == true)
			return true;

		/* Pass into object */
		if (box->object != NULL && content_scroll_at_point(
				box->object, x - box_x, y - box_y,
				scrx, scry) == true)
			return true;

		/* Handle box scrollbars */
		if (box->scroll_y && scrollbar_scroll(box->scroll_y, scry))
			handled_scroll = true;

		if (box->scroll_x && scrollbar_scroll(box->scroll_x, scrx))
			handled_scroll = true;

		if (handled_scroll == true)
			return true;
	}

	return false;
}

/** Helper for file gadgets to store their filename unencoded on the
 * dom node associated with the gadget.
 *
 * \todo Get rid of this crap eventually
 */
static void html__dom_user_data_handler(dom_node_operation operation,
		dom_string *key, void *_data, struct dom_node *src,
		struct dom_node *dst)
{
	char *oldfile;
	char *data = (char *)_data;

	if (!dom_string_isequal(corestring_dom___ns_key_file_name_node_data,
				key) || data == NULL) {
		return;
	}

	switch (operation) {
	case DOM_NODE_CLONED:
		if (dom_node_set_user_data(dst,
					   corestring_dom___ns_key_file_name_node_data,
					   strdup(data), html__dom_user_data_handler,
					   &oldfile) == DOM_NO_ERR) {
			if (oldfile != NULL)
				free(oldfile);
		}
		break;

	case DOM_NODE_RENAMED:
	case DOM_NODE_IMPORTED:
	case DOM_NODE_ADOPTED:
		break;

	case DOM_NODE_DELETED:
		free(data);
		break;
	default:
		NSLOG(netsurf, INFO, "User data operation not handled.");
		assert(0);
	}
}

static void html__set_file_gadget_filename(struct content *c,
	struct form_control *gadget, const char *fn)
{
	nserror ret;
	char *utf8_fn, *oldfile = NULL;
	html_content *html = (html_content *)c;
	struct box *file_box = gadget->box;

	ret = guit->utf8->local_to_utf8(fn, 0, &utf8_fn);
	if (ret != NSERROR_OK) {
		assert(ret != NSERROR_BAD_ENCODING);
		NSLOG(netsurf, INFO,
		      "utf8 to local encoding conversion failed");
		/* Load was for us - just no memory */
		return;
	}

	form_gadget_update_value(gadget, utf8_fn);

	/* corestring_dom___ns_key_file_name_node_data */
	if (dom_node_set_user_data((dom_node *)file_box->gadget->node,
				   corestring_dom___ns_key_file_name_node_data,
				   strdup(fn), html__dom_user_data_handler,
				   &oldfile) == DOM_NO_ERR) {
		if (oldfile != NULL)
			free(oldfile);
	}

	/* Redraw box. */
	html__redraw_a_box(html, file_box);
}

void html_set_file_gadget_filename(struct hlcache_handle *hl,
	struct form_control *gadget, const char *fn)
{
	html__set_file_gadget_filename(hlcache_handle_get_content(hl),
		gadget, fn);
}

/**
 * Drop a file onto a content at a particular point, or determine if a file
 * may be dropped onto the content at given point.
 *
 * \param c	html content to look inside
 * \param x	x-coordinate of point of interest
 * \param y	y-coordinate of point of interest
 * \param file	path to file to be dropped, or NULL to know if drop allowed
 * \return true iff file drop has been handled, or if drop possible (NULL file)
 */
static bool html_drop_file_at_point(struct content *c, int x, int y, char *file)
{
	html_content *html = (html_content *) c;

	struct box *box = html->layout;
	struct box *next;
	struct box *file_box = NULL;
	struct box *text_box = NULL;
	int box_x = 0, box_y = 0;

	/* Scan box tree for boxes that can handle drop */
	while ((next = box_at_point(&html->unit_len_ctx, box, x, y,
			&box_x, &box_y)) != NULL) {
		box = next;

		if (box->style &&
		    css_computed_visibility(box->style) == CSS_VISIBILITY_HIDDEN)
			continue;

		if (box->iframe) {
			float scale = browser_window_get_scale(box->iframe);
			return browser_window_drop_file_at_point(
				box->iframe,
				(x - box_x) * scale,
				(y - box_y) * scale,
				file);
		}

		if (box->object &&
		    content_drop_file_at_point(box->object,
					x - box_x, y - box_y, file) == true)
			return true;

		if (box->gadget) {
			switch (box->gadget->type) {
				case GADGET_FILE:
					file_box = box;
				break;

				case GADGET_TEXTBOX:
				case GADGET_TEXTAREA:
				case GADGET_PASSWORD:
					text_box = box;
					break;

				default:	/* appease compiler */
					break;
			}
		}
	}

	if (!file_box && !text_box)
		/* No box capable of handling drop */
		return false;

	if (file == NULL)
		/* There is a box capable of handling drop here */
		return true;

	/* Handle the drop */
	if (file_box) {
		/* File dropped on file input */
		html__set_file_gadget_filename(c, file_box->gadget, file);

	} else {
		/* File dropped on text input */

		size_t file_len;
		FILE *fp = NULL;
		char *buffer;
		char *utf8_buff;
		nserror ret;
		unsigned int size;
		int bx, by;

		/* Open file */
		fp = fopen(file, "rb");
		if (fp == NULL) {
			/* Couldn't open file, but drop was for us */
			return true;
		}

		/* Get filesize */
		fseek(fp, 0, SEEK_END);
		file_len = ftell(fp);
		fseek(fp, 0, SEEK_SET);

		if ((long)file_len == -1) {
			/* unable to get file length, but drop was for us */
			fclose(fp);
			return true;
		}

		/* Allocate buffer for file data */
		buffer = malloc(file_len + 1);
		if (buffer == NULL) {
			/* No memory, but drop was for us */
			fclose(fp);
			return true;
		}

		/* Stick file into buffer */
		if (file_len != fread(buffer, 1, file_len, fp)) {
			/* Failed, but drop was for us */
			free(buffer);
			fclose(fp);
			return true;
		}

		/* Done with file */
		fclose(fp);

		/* Ensure buffer's string termination */
		buffer[file_len] = '\0';

		/* TODO: Sniff for text? */

		/* Convert to UTF-8 */
		ret = guit->utf8->local_to_utf8(buffer, file_len, &utf8_buff);
		if (ret != NSERROR_OK) {
			/* bad encoding shouldn't happen */
			NSLOG(netsurf, ERROR,
			      "local to utf8 encoding failed (%s)",
			      messages_get_errorcode(ret));
			assert(ret != NSERROR_BAD_ENCODING);
			free(buffer);
			return true;
		}

		/* Done with buffer */
		free(buffer);

		/* Get new length */
		size = strlen(utf8_buff);

		/* Simulate a click over the input box, to place caret */
		box_coords(text_box, &bx, &by);
		textarea_mouse_action(text_box->gadget->data.text.ta,
				BROWSER_MOUSE_PRESS_1, x - bx, y - by);

		/* Paste the file as text */
		textarea_drop_text(text_box->gadget->data.text.ta,
				utf8_buff, size);

		free(utf8_buff);
	}

	return true;
}


/**
 * set debug status.
 *
 * \param c The content to debug
 * \param op The debug operation type
 */
static nserror
html_debug(struct content *c, enum content_debug op)
{
	html_redraw_debug = !html_redraw_debug;

	return NSERROR_OK;
}


/**
 * Dump debug info concerning the html_content
 *
 * \param c The content to debug
 * \param f The file to dump to
 * \param op The debug dump type
 */
static nserror
html_debug_dump(struct content *c, FILE *f, enum content_debug op)
{
	html_content *htmlc = (html_content *)c;
	dom_node *html;
	dom_exception exc; /* returned by libdom functions */
	nserror ret;

	assert(htmlc != NULL);

	if (op == CONTENT_DEBUG_RENDER) {
		assert(htmlc->layout != NULL);
		box_dump(f, htmlc->layout, 0, true);
		ret = NSERROR_OK;
	} else {
		if (htmlc->document == NULL) {
			NSLOG(netsurf, INFO, "No document to dump");
			return NSERROR_DOM;
		}

		exc = dom_document_get_document_element(htmlc->document, (void *) &html);
		if ((exc != DOM_NO_ERR) || (html == NULL)) {
			NSLOG(netsurf, INFO, "Unable to obtain root node");
			return NSERROR_DOM;
		}

		ret = libdom_dump_structure(html, f, 0);

		NSLOG(netsurf, INFO, "DOM structure dump returning %d", ret);

		dom_node_unref(html);
	}

	return ret;
}


#if ALWAYS_DUMP_FRAMESET
/**
 * Print a frameset tree to stderr.
 */

static void
html_dump_frameset(struct content_html_frames *frame, unsigned int depth)
{
	unsigned int i;
	int row, col, index;
	const char *unit[] = {"px", "%", "*"};
	const char *scrolling[] = {"auto", "yes", "no"};

	assert(frame);

	fprintf(stderr, "%p ", frame);

	fprintf(stderr, "(%i %i) ", frame->rows, frame->cols);

	fprintf(stderr, "w%g%s ", frame->width.value, unit[frame->width.unit]);
	fprintf(stderr, "h%g%s ", frame->height.value,unit[frame->height.unit]);
	fprintf(stderr, "(margin w%i h%i) ",
			frame->margin_width, frame->margin_height);

	if (frame->name)
		fprintf(stderr, "'%s' ", frame->name);
	if (frame->url)
		fprintf(stderr, "<%s> ", frame->url);

	if (frame->no_resize)
		fprintf(stderr, "noresize ");
	fprintf(stderr, "(scrolling %s) ", scrolling[frame->scrolling]);
	if (frame->border)
		fprintf(stderr, "border %x ",
				(unsigned int) frame->border_colour);

	fprintf(stderr, "\n");

	if (frame->children) {
		for (row = 0; row != frame->rows; row++) {
			for (col = 0; col != frame->cols; col++) {
				for (i = 0; i != depth; i++)
					fprintf(stderr, "  ");
				fprintf(stderr, "(%i %i): ", row, col);
				index = (row * frame->cols) + col;
				html_dump_frameset(&frame->children[index],
						depth + 1);
			}
		}
	}
}

#endif

/**
 * Retrieve HTML document tree
 *
 * \param h  HTML content to retrieve document tree from
 * \return Pointer to document tree
 */
dom_document *html_get_document(hlcache_handle *h)
{
	html_content *c = (html_content *) hlcache_handle_get_content(h);

	assert(c != NULL);

	return c->document;
}

/**
 * Retrieve box tree
 *
 * \param h  HTML content to retrieve tree from
 * \return Pointer to box tree
 *
 * \todo This API must die, as must all use of the box tree outside of
 *         HTML content handler
 */
struct box *html_get_box_tree(hlcache_handle *h)
{
	html_content *c = (html_content *) hlcache_handle_get_content(h);

	assert(c != NULL);

	return c->layout;
}

/**
 * Retrieve the charset of an HTML document
 *
 * \param c Content to retrieve charset from
 * \param op The content encoding operation to perform.
 * \return Pointer to charset, or NULL
 */
static const char *html_encoding(const struct content *c, enum content_encoding_type op)
{
	html_content *html = (html_content *) c;
	static char enc_token[10] = "Encoding0";

	assert(html != NULL);

	if (op == CONTENT_ENCODING_SOURCE) {
		enc_token[8] = '0' + html->encoding_source;
		return messages_get(enc_token);
	}

	return html->encoding;
}


/**
 * Retrieve framesets used in an HTML document
 *
 * \param h  Content to inspect
 * \return Pointer to framesets, or NULL if none
 */
struct content_html_frames *html_get_frameset(hlcache_handle *h)
{
	html_content *c = (html_content *) hlcache_handle_get_content(h);

	assert(c != NULL);

	return c->frameset;
}

/**
 * Retrieve iframes used in an HTML document
 *
 * \param h  Content to inspect
 * \return Pointer to iframes, or NULL if none
 */
struct content_html_iframe *html_get_iframe(hlcache_handle *h)
{
	html_content *c = (html_content *) hlcache_handle_get_content(h);

	assert(c != NULL);

	return c->iframe;
}

/**
 * Retrieve an HTML content's base URL
 *
 * \param h  Content to retrieve base target from
 * \return Pointer to URL
 */
nsurl *html_get_base_url(hlcache_handle *h)
{
	html_content *c = (html_content *) hlcache_handle_get_content(h);

	assert(c != NULL);

	return c->base_url;
}

/**
 * Retrieve an HTML content's base target
 *
 * \param h  Content to retrieve base target from
 * \return Pointer to target, or NULL if none
 */
const char *html_get_base_target(hlcache_handle *h)
{
	html_content *c = (html_content *) hlcache_handle_get_content(h);

	assert(c != NULL);

	return c->base_target;
}


/**
 * Retrieve layout coordinates of box with given id
 *
 * \param h        HTML document to search
 * \param frag_id  String containing an element id
 * \param x        Updated to global x coord iff id found
 * \param y        Updated to global y coord iff id found
 * \return  true iff id found
 */
bool html_get_id_offset(hlcache_handle *h, lwc_string *frag_id, int *x, int *y)
{
	struct box *pos;
	struct box *layout;

	if (content_get_type(h) != CONTENT_HTML)
		return false;

	layout = html_get_box_tree(h);

	if ((pos = box_find_by_id(layout, frag_id)) != 0) {
		box_coords(pos, x, y);
		return true;
	}
	return false;
}

bool html_exec(struct content *c, const char *src, size_t srclen)
{
	html_content *htmlc = (html_content *)c;
	bool result = false;
	dom_exception err;
	dom_html_body_element *body_node;
	dom_string *dom_src;
	dom_text *text_node;
	dom_node *spare_node;
	dom_html_script_element *script_node;

	if (htmlc->document == NULL) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to exec, no document");
		goto out_no_string;
	}

	err = dom_string_create((const uint8_t *)src, srclen, &dom_src);
	if (err != DOM_NO_ERR) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to exec, could not create string");
		goto out_no_string;
	}

	err = dom_html_document_get_body(htmlc->document, &body_node);
	if (err != DOM_NO_ERR) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to retrieve body element");
		goto out_no_body;
	}

	err = dom_document_create_text_node(htmlc->document, dom_src, &text_node);
	if (err != DOM_NO_ERR) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to exec, could not create text node");
		goto out_no_text_node;
	}

	err = dom_document_create_element(htmlc->document, corestring_dom_SCRIPT, &script_node);
	if (err != DOM_NO_ERR) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to exec, could not create script node");
		goto out_no_script_node;
	}

	err = dom_node_append_child(script_node, text_node, &spare_node);
	if (err != DOM_NO_ERR) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to exec, could not insert code node into script node");
		goto out_unparented;
	}
	dom_node_unref(spare_node); /* We do not need the spare ref at all */

	err = dom_node_append_child(body_node, script_node, &spare_node);
	if (err != DOM_NO_ERR) {
		NSLOG(netsurf, DEEPDEBUG, "Unable to exec, could not insert script node into document body");
		goto out_unparented;
	}
	dom_node_unref(spare_node); /* Again no need for the spare ref */

	/* We successfully inserted the node into the DOM */

	result = true;

	/* Now we unwind, starting by removing the script from wherever it
	 * ended up parented
	 */

	err = dom_node_get_parent_node(script_node, &spare_node);
	if (err == DOM_NO_ERR && spare_node != NULL) {
		dom_node *second_spare;
		err = dom_node_remove_child(spare_node, script_node, &second_spare);
		if (err == DOM_NO_ERR) {
			dom_node_unref(second_spare);
		}
		dom_node_unref(spare_node);
	}

out_unparented:
	dom_node_unref(script_node);
out_no_script_node:
	dom_node_unref(text_node);
out_no_text_node:
	dom_node_unref(body_node);
out_no_body:
	dom_string_unref(dom_src);
out_no_string:
	return result;
}

/* See \ref content_saw_insecure_objects */
static bool
html_saw_insecure_objects(struct content *c)
{
	html_content *htmlc = (html_content *)c;
	struct content_html_object *obj = htmlc->object_list;

	/* Check through the object list */
	while (obj != NULL) {
		if (obj->content != NULL) {
			if (content_saw_insecure_objects(obj->content))
				return true;
		}
		obj = obj->next;
	}

	/* Now check the script list */
	if (html_saw_insecure_scripts(htmlc)) {
		return true;
	}

	/* Now check stylesheets */
	if (html_css_saw_insecure_stylesheets(htmlc)) {
		return true;
	}

	return false;
}

/**
 * Compute the type of a content
 *
 * \return CONTENT_HTML
 */
static content_type html_content_type(void)
{
	return CONTENT_HTML;
}


static void html_fini(void)
{
	html_css_fini();
}

/**
 * Finds all occurrences of a given string in an html box
 *
 * \param pattern   the string pattern to search for
 * \param p_len     pattern length
 * \param cur       pointer to the current box
 * \param case_sens whether to perform a case sensitive search
 * \param context   The search context to add the entry to.
 * \return true on success, false on memory allocation failure
 */
static nserror
find_occurrences_html_box(const char *pattern,
			  int p_len,
			  struct box *cur,
			  bool case_sens,
			  struct textsearch_context *context)
{
	struct box *a;
	nserror res = NSERROR_OK;

	/* ignore this box, if there's no visible text */
	if (!cur->object && cur->text) {
		const char *text = cur->text;
		unsigned length = cur->length;

		while (length > 0) {
			unsigned match_length;
			unsigned match_offset;
			const char *new_text;
			const char *pos;

			pos = content_textsearch_find_pattern(text,
					   length,
					   pattern,
					   p_len,
					   case_sens,
					   &match_length);
			if (!pos)
				break;

			/* found string in box => add to list */
			match_offset = pos - cur->text;

			res = content_textsearch_add_match(context,
					cur->byte_offset + match_offset,
					cur->byte_offset + match_offset + match_length,
					cur,
					cur);
			if (res != NSERROR_OK) {
				return res;
			}

			new_text = pos + match_length;
			length -= (new_text - text);
			text = new_text;
		}
	}

	/* and recurse */
	for (a = cur->children; a; a = a->next) {
		res = find_occurrences_html_box(pattern,
						p_len,
						a,
						case_sens,
						context);
		if (res != NSERROR_OK) {
			return res;
		}
	}

	return res;
}

/**
 * Finds all occurrences of a given string in the html box tree
 *
 * \param pattern   the string pattern to search for
 * \param p_len     pattern length
 * \param c The content to search
 * \param csens whether to perform a case sensitive search
 * \param context   The search context to add the entry to.
 * \return true on success, false on memory allocation failure
 */
static nserror
html_textsearch_find(struct content *c,
		     struct textsearch_context *context,
		     const char *pattern,
		     int p_len,
		     bool csens)
{
	html_content *html = (html_content *)c;

	if (html->layout == NULL) {
		return NSERROR_INVALID;
	}

	return find_occurrences_html_box(pattern,
					 p_len,
					 html->layout,
					 csens,
					 context);
}


static nserror
html_textsearch_bounds(struct content *c,
		       unsigned start_idx,
		       unsigned end_idx,
		       struct box *start_box,
		       struct box *end_box,
		       struct rect *bounds)
{
	/* get box position and jump to it */
	box_coords(start_box, &bounds->x0, &bounds->y0);
	/* \todo: move x0 in by correct idx */
	box_coords(end_box, &bounds->x1, &bounds->y1);
	/* \todo: move x1 in by correct idx */
	bounds->x1 += end_box->width;
	bounds->y1 += end_box->height;

	return NSERROR_OK;
}


/**
 * HTML content handler function table
 *
 * MacSurf: was a static const designated initializer; CW8 C89 has
 * neither, so populate the vtable at runtime in html_init().
 */
static content_handler html_content_handler;


/* exported function documented in html/html.h */
nserror html_init(void)
{
	uint32_t i;
	nserror error;

#ifdef __MACOS9__
	extern void macsurf_debug_log_writef(const char *fmt, ...);
	macsurf_debug_log_writef("html_init: entered, types=%ld",
		(long)NOF_ELEMENTS(html_types));
#endif

	memset(&html_content_handler, 0, sizeof(html_content_handler));
	html_content_handler.fini = html_fini;
	html_content_handler.create = html_create;
	html_content_handler.process_data = html_process_data;
	html_content_handler.data_complete = html_convert;
	html_content_handler.reformat = html_reformat;
	html_content_handler.destroy = html_destroy;
	html_content_handler.stop = html_stop;
	html_content_handler.mouse_track = html_mouse_track;
	html_content_handler.mouse_action = html_mouse_action;
	html_content_handler.keypress = html_keypress;
	html_content_handler.redraw = html_redraw;
	html_content_handler.open = html_open;
	html_content_handler.close = html_close;
	html_content_handler.get_selection = html_get_selection;
	html_content_handler.clear_selection = html_clear_selection;
	html_content_handler.get_contextual_content = html_get_contextual_content;
	html_content_handler.scroll_at_point = html_scroll_at_point;
	html_content_handler.drop_file_at_point = html_drop_file_at_point;
	html_content_handler.debug_dump = html_debug_dump;
	html_content_handler.debug = html_debug;
	html_content_handler.clone = html_clone;
	html_content_handler.get_encoding = html_encoding;
	html_content_handler.type = html_content_type;
	html_content_handler.exec = html_exec;
	html_content_handler.saw_insecure_objects = html_saw_insecure_objects;
	html_content_handler.textsearch_find = html_textsearch_find;
	html_content_handler.textsearch_bounds = html_textsearch_bounds;
	html_content_handler.textselection_redraw = html_textselection_redraw;
	html_content_handler.textselection_copy = html_textselection_copy;
	html_content_handler.textselection_get_end = html_textselection_get_end;
	html_content_handler.no_share = true;

	error = html_css_init();
	if (error != NSERROR_OK)
		goto error;

	for (i = 0; i < NOF_ELEMENTS(html_types); i++) {
		error = content_factory_register_handler(html_types[i],
				&html_content_handler);
#ifdef __MACOS9__
		macsurf_debug_log_writef("html_init: reg[%ld] err=%d",
			(long)i, (int)error);
#endif
		if (error != NSERROR_OK)
			goto error;
	}

#ifdef __MACOS9__
	macsurf_debug_log_writef("html_init: done OK");
#endif
	return NSERROR_OK;

error:
	html_fini();

	return error;
}
