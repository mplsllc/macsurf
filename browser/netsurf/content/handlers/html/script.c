/*
 * Copyright 2012 Vincent Sanders <vince@netsurf-browser.org>
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
 * implementation of content handling for text/html scripts.
 */

#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "utils/config.h"
#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "netsurf/content.h"
#include "javascript/js.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "content/fetch.h"
#include "content/hlcache.h"

#include "html/html.h"
#include "html/private.h"
#include "netsurf/misc.h"
#include "desktop/gui_internal.h"
#include "macsurf_debug.h"

extern int macsurf_ptr_is_heap(const void *);

/* fixes1117b (#265) - execute a script as an ES module. Defined in
 * frontends/macos9/javascript/macsurf_qjs.c, linked via the MacSurf.mcp
 * build. Returns 1 on success, 0 on compile/resolve/execute failure. */
extern unsigned char js_exec_module(struct jsthread *thread,
	const unsigned char *txt, size_t txtlen, const char *name);

typedef bool (script_handler_t)(struct jsthread *jsthread, const uint8_t *data, size_t size, const char *name);

/* fixes535: out-of-band live-content registry (anti-UAF).  The three
 * convert_script_*_cb hlcache callbacks carry the parent html_content as pw.
 * The reliable cancellation is the script handle release in html_script_free /
 * html_stop (content_remove_user deregisters the callback), but the bulk
 * hlcache_clean double-destroy can drive a sub-content to DONE/ERROR and
 * dispatch one of these callbacks against a parent that was freed and reused in
 * the same clean pass.  The reused struct's bytes read as plausible garbage so
 * the find-script loop and assert below walk freed memory.  Validate the parent
 * by registry membership (never dereferences it) before touching parent->scripts.
 * No-op (always live) on non-Mac syntax-check builds. */
#ifdef __MACOS9__
extern int macos9_content_is_live(struct content *c);
extern unsigned long macos9_content_token(struct content *c);
extern int macos9_content_token_valid(struct content *c, unsigned long token);
#else
#define macos9_content_is_live(c) (1)
#define macos9_content_token(c) (1UL)
#define macos9_content_token_valid(c, t) (1)
#endif

/* fixes550: scheduler payload for deferred_parser_unpause.  It carries the
 * content GENERATION TOKEN captured at SCHEDULE time so dispatch can reject a
 * freed-OR-freed-and-reused (ABA) html_content.  Pointer membership alone
 * (macos9_content_is_live, fixes549) cannot: a bulk hlcache_clean frees this
 * content between schedule and dispatch, the allocator hands the SAME address
 * to a new content which re-registers at content__init, and is_live then
 * returns 1 for the stale pointer (false positive) -> the resumed parser drives
 * the WRONG/garbage tokeniser -> wild dom_string interned in create_element
 * (the lbzu / r4-wild crash).  The registry's monotonic generation token
 * defeats this; see macos9_content_registry.h ("Capture the token at schedule
 * time"). */
struct macos9_unpause_payload {
	html_content  *parent;
	unsigned long  token;
	void          *parser;   /* fixes557: parser identity captured at schedule */
};


static script_handler_t *select_script_handler(content_type ctype)
{
	if (ctype == CONTENT_JS) {
		return js_exec;
	}
	return NULL;
}


/* exported internal interface documented in html/html_internal.h */
nserror html_script_exec(html_content *c, bool allow_defer)
{
	unsigned int i;
	struct html_script *s;
	script_handler_t *script_handler;
	bool have_run_something = false;

	if (c->js_thread == NULL) {
		return NSERROR_BAD_PARAMETER;
	}

	for (i = 0, s = c->scripts; i != c->scripts_count; i++, s++) {
		if (s->already_started) {
			continue;
		}

		if ((s->type == HTML_SCRIPT_ASYNC) ||
		    (allow_defer && (s->type == HTML_SCRIPT_DEFER))) {
			/* ensure script content is present */
			if (s->data.handle == NULL)
				continue;

			/* ensure script content fetch status is not an error */
			if (content_get_status(s->data.handle) ==
					CONTENT_STATUS_ERROR)
				continue;

			/* ensure script handler for content type */
			script_handler = select_script_handler(
					content_get_type(s->data.handle));
			if (script_handler == NULL) {
				/* fixes847 (#167 S1 census gap) - this is a silent
				 * script-execution skip with NO log of any kind
				 * before this fix: select_script_handler() only
				 * returns non-NULL for CONTENT_JS (0x40, see
				 * netsurf/content_type.h); if the fetched resource's
				 * detected content-type is anything else (a
				 * misdetected/unexpected Content-Type header,
				 * content sniffing choosing wrong, etc.) the script
				 * NEVER runs and nothing anywhere says so. Async/
				 * defer scripts are exactly the pattern modern
				 * sites (Facebook) use for their real bundles. */
				macsurf_debug_log_writef(
					"LIFE script_exec: SKIP unsupported "
					"content_type=%d url=%s",
					(int) content_get_type(s->data.handle),
					nsurl_access(
						hlcache_handle_get_url(s->data.handle)));
				continue; /* unsupported type */
			}

			if (content_get_status(s->data.handle) ==
					CONTENT_STATUS_DONE) {
				/* external script is now available */
				const uint8_t *data;
				size_t size;
				data = content_get_source_data(
						s->data.handle, &size );
				/* fixes847 (#167 S1 census gap) - positive
				 * confirmation a script is actually about to reach
				 * js_exec, WORK-gated (this whole function had zero
				 * log lines of any kind before this fix). */
				macsurf_debug_log_writef(
					"LIFE script_exec: RUN bytes=%ld url=%s",
					(long) size,
					nsurl_access(
						hlcache_handle_get_url(s->data.handle)));
				/* fixes873 (#301) - document.currentScript must name THIS
				 * script while it runs. webpack's publicPath runtime reads
				 * it first thing and throws "Automatic publicPath is not
				 * supported in this browser" if it (and the
				 * getElementsByTagName fallback) come up empty -- killing
				 * the bundle on its own prologue. Set before, cleared after,
				 * so it is null outside script execution as the spec says. */
				if (s->node != NULL) {
					js_set_current_script(c->js_thread, s->node);
				}
				script_handler(c->js_thread, data, size,
					       nsurl_access(hlcache_handle_get_url(s->data.handle)));
				/* Re-acquire BEFORE touching s again: script_handler is JS
				 * and can realloc c->scripts. */
				s = &(c->scripts[i]);
				if (s->node != NULL) {
					js_set_current_script(c->js_thread, NULL);
					s = &(c->scripts[i]);
				}
				have_run_something = true;
				/* We have to re-acquire this here since the
				 * c->scripts array may have been reallocated
				 * as a result of executing this script.
				 */
				s = &(c->scripts[i]);

				s->already_started = true;

				/* fixes869 (#295) - fire `load` at the element, AFTER
				 * the script has run (per spec, and what a loader
				 * waiting on it expects: the script's own globals must
				 * exist by the time onload resolves the promise).
				 *
				 * s->node is set only for JS-INSERTED scripts
				 * (exec_src_script), which are the only ones anything
				 * can be waiting on -- so parser-inserted scripts cost
				 * nothing here.
				 *
				 * Ordering is load-bearing: it must come AFTER the
				 * re-acquire above, because script_handler() can
				 * realloc c->scripts, and reading s->node through the
				 * stale pointer would be a use-after-free. */
				if (s->node != NULL) {
					js_fire_script_load(c->js_thread, s->node, 1);
					/* Re-acquire again: the load handler is JS and can
					 * itself insert more scripts (that is precisely the
					 * chain we are enabling), reallocating the array. */
					s = &(c->scripts[i]);
				}
			}
		}
	}

	if (have_run_something) {
		return html_proceed_to_done(c);
	}

	return NSERROR_OK;
}

/* create new html script entry */
static struct html_script *
html_process_new_script(html_content *c,
			dom_string *mimetype,
			enum html_script_type type)
{
	struct html_script *nscript;
	/* add space for new script entry */
	nscript = realloc(c->scripts,
			  sizeof(struct html_script) * (c->scripts_count + 1));
	if (nscript == NULL) {
		return NULL;
	}

	c->scripts = nscript;

	/* increment script entry count */
	nscript = &c->scripts[c->scripts_count];
	c->scripts_count++;

	nscript->already_started = false;
	nscript->parser_inserted = false;
	nscript->force_async = true;
	nscript->ready_exec = false;
	nscript->async = false;
	nscript->defer = false;
	/* fixes869 (#295) - see struct html_script. Callers that have the element
	 * (exec_src_script) set this; realloc above means every field must be
	 * initialised here or the new slot inherits garbage. */
	nscript->node = NULL;

	nscript->type = type;

	nscript->mimetype = dom_string_ref(mimetype); /* reference mimetype */

	return nscript;
}

/**
 * Callback for asyncronous scripts
 */
static nserror
convert_script_async_cb(hlcache_handle *script,
			  const hlcache_event *event,
			  void *pw)
{
	html_content *parent = pw;
	unsigned int i;
	struct html_script *s;

	/* fixes535: registry-membership liveness guard FIRST, before any field of
	 * parent is read.  Not live => the html_content was torn down; the script
	 * handle release that should have deregistered this callback raced behind a
	 * bulk hlcache_clean destroy, so unwind without touching freed state. */
	if (macos9_content_is_live(&parent->base) == 0) {
		macsurf_debug_log_writef(
			"convert_script_async_cb: parent NOT LIVE parent=%p", (void *)parent);
		return NSERROR_OK;
	}

	/* Find script */
	for (i = 0, s = parent->scripts; i != parent->scripts_count; i++, s++) {
		if (s->type == HTML_SCRIPT_ASYNC && s->data.handle == script)
			break;
	}

	assert(i != parent->scripts_count);

	switch (event->type) {
	case CONTENT_MSG_LOADING:
		break;

	case CONTENT_MSG_READY:
		break;

	case CONTENT_MSG_DONE:
		NSLOG(netsurf, INFO, "script %d done '%s'", i,
		      nsurl_access(hlcache_handle_get_url(script)));
		parent->base.active--;
		NSLOG(netsurf, INFO, "%d fetches active", parent->base.active);

		break;

	case CONTENT_MSG_ERROR:
		NSLOG(netsurf, INFO, "script %s failed: %s",
		      nsurl_access(hlcache_handle_get_url(script)),
		      event->data.errordata.errormsg);

		/* fixes869 (#295) - fire `error` at the element so a loader waiting
		 * on this script REJECTS rather than hanging forever.  A promise that
		 * never settles is strictly worse than a rejected one: the page gets
		 * no chance to fall back or report.  Fired BEFORE the release below,
		 * while s is still valid, and only for JS-inserted scripts
		 * (s->node != NULL).  The handler is JS and can realloc c->scripts,
		 * so re-acquire s afterwards. */
		if (s->node != NULL && parent->js_thread != NULL) {
			js_fire_script_load(parent->js_thread, s->node, 0);
			s = &(parent->scripts[i]);
		}

		/* fixes515: NULL before release so a reentrant callback finds
		 * NULL instead of a freed handle / freed callback TVector. */
		safe_hlcache_handle_release(&s->data.handle);
		parent->base.active--;
		NSLOG(netsurf, INFO, "%d fetches active", parent->base.active);

		break;

	default:
		break;
	}

	/* if there are no active fetches remaining begin post parse
	 * conversion
	 */
	if (html_can_begin_conversion(parent)) {
		html_begin_conversion(parent);
	}

	/* if we have already started converting though, then we can handle the
	 * scripts as they come in.
	 */
	else if (parent->conversion_begun) {
		return html_script_exec(parent, false);
	}

	return NSERROR_OK;
}

/**
 * Callback for defer scripts
 */
static nserror
convert_script_defer_cb(hlcache_handle *script,
			  const hlcache_event *event,
			  void *pw)
{
	html_content *parent = pw;
	unsigned int i;
	struct html_script *s;

	/* fixes535: registry-membership liveness guard FIRST (see async cb). */
	if (macos9_content_is_live(&parent->base) == 0) {
		macsurf_debug_log_writef(
			"convert_script_defer_cb: parent NOT LIVE parent=%p", (void *)parent);
		return NSERROR_OK;
	}

	/* Find script */
	for (i = 0, s = parent->scripts; i != parent->scripts_count; i++, s++) {
		if (s->type == HTML_SCRIPT_DEFER && s->data.handle == script)
			break;
	}

	assert(i != parent->scripts_count);

	switch (event->type) {

	case CONTENT_MSG_DONE:
		NSLOG(netsurf, INFO, "script %d done '%s'", i,
		      nsurl_access(hlcache_handle_get_url(script)));
		parent->base.active--;
		NSLOG(netsurf, INFO, "%d fetches active", parent->base.active);

		break;

	case CONTENT_MSG_ERROR:
		NSLOG(netsurf, INFO, "script %s failed: %s",
		      nsurl_access(hlcache_handle_get_url(script)),
		      event->data.errordata.errormsg);

		/* fixes869 (#295) - fire `error` at the element so a loader waiting
		 * on this script REJECTS rather than hanging forever.  A promise that
		 * never settles is strictly worse than a rejected one: the page gets
		 * no chance to fall back or report.  Fired BEFORE the release below,
		 * while s is still valid, and only for JS-inserted scripts
		 * (s->node != NULL).  The handler is JS and can realloc c->scripts,
		 * so re-acquire s afterwards. */
		if (s->node != NULL && parent->js_thread != NULL) {
			js_fire_script_load(parent->js_thread, s->node, 0);
			s = &(parent->scripts[i]);
		}

		/* fixes515: NULL before release so a reentrant callback finds
		 * NULL instead of a freed handle / freed callback TVector. */
		safe_hlcache_handle_release(&s->data.handle);
		parent->base.active--;
		NSLOG(netsurf, INFO, "%d fetches active", parent->base.active);

		break;

	default:
		break;
	}

	/* if there are no active fetches remaining begin post parse
	 * conversion
	 */
	if (html_can_begin_conversion(parent)) {
		html_begin_conversion(parent);
	}

	return NSERROR_OK;
}

/* fixes512: deferred parser unpause.
 * convert_script_sync_cb is invoked from inside a content_broadcast walk
 * (content_set_done -> content_broadcast -> hlcache_content_callback ->
 * convert_script_sync_cb).  Calling dom_hubbub_parser_pause(parser, false)
 * synchronously from that chain re-enters the hubbub tokenizer while it is
 * still mid-token from the original parse call.  The tokenizer's internal
 * buffer pointer becomes r4=1 (stale state from the interrupted parse) and
 * the next lbzu crashes.  Fix: schedule the unpause for the next event loop
 * tick (delay=0) so it fires after the notification walk has fully returned.
 * The html_content pointer is safe to hold across one scheduler tick because
 * html_destroy cancels all scheduled callbacks via cancel_dom_to_box and
 * html_close sets aborted=true; we guard both here. */
static void
deferred_parser_unpause(void *pw)
{
	struct macos9_unpause_payload *pl = (struct macos9_unpause_payload *)pw;
	html_content *parent;
	unsigned long saved_token;
	unsigned long cur_token;
	void *saved_parser;
	dom_hubbub_error err;

	if (pl == NULL)
		return;
	parent = pl->parent;
	saved_token = pl->token;
	saved_parser = pl->parser;
	free(pl);   /* our own allocation; consumed regardless of outcome below */

	/* fixes557: ENTRY instrumentation - logged BEFORE the token check and
	 * WITHOUT dereferencing parent (macos9_content_token is address-keyed and
	 * never reads parent's bytes, so it is safe even on a freed pointer).  This
	 * is the decisive G3 probe: if this line does NOT appear before the
	 * emit_current_tag crash, this script.c (the fixes550/557 guard) is not in
	 * the running binary and the answer is "build it", not "re-guard".  If it
	 * DOES appear and is followed by TOKEN INVALID / PARSER CHANGED with no
	 * crash, the guard caught the ABA/parser-free case. */
	cur_token = macos9_content_token(&parent->base);
	macsurf_debug_log_writef(
		"deferred_unpause: ENTRY parent=%p savedtok=%ld curtok=%ld savedparser=%p",
		(void *)parent, (long)saved_token, (long)cur_token, saved_parser);

	/* fixes550: GENERATION-TOKEN validation - the actual fix for the
	 * deferred_parser_unpause -> dom_hubbub_parser_pause -> hubbub_tokeniser_run
	 * -> create_element/intern -> wild dom_string (lbzu, r4 wild) crash.
	 *
	 * fixes549 gated on macos9_content_is_live (pointer membership), which a
	 * bulk hlcache_clean defeats via ABA: it frees this content between schedule
	 * and dispatch, the allocator reuses the SAME address for a new content that
	 * re-registers at content__init, and is_live then returns 1 for the stale
	 * pointer (a FALSE POSITIVE) -> we resume the wrong/garbage tokeniser ->
	 * crash.  The registry stamps each occupancy with a monotonically-increasing
	 * generation; captured at schedule time and validated here, it rejects both
	 * a freed content (absent) and a reused address (higher generation, token no
	 * longer matches).  macos9_content_token_valid is address-keyed and never
	 * dereferences parent, so a freed parent is safe to test.  Invalid =>
	 * freed/reused => bail BEFORE any hubbub call (no pause/setopt/tokeniser_run);
	 * the page it would have continued is gone.  This is strictly stronger than
	 * the is_live membership check it replaces and, like it, cannot
	 * false-negative a live parse (a live content keeps its generation), so it
	 * does not re-introduce the fixes521 parser-stuck-paused stall. */
	if (macos9_content_token_valid(&parent->base, saved_token) == 0) {
		macsurf_debug_log_writef(
			"deferred_unpause: TOKEN INVALID (freed/ABA) parent=%p tok=%ld, skip",
			(void *)parent, (long)saved_token);
		return;
	}

	/* fixes558: clear the pending-unpause flag UNCONDITIONALLY now that this
	 * scheduled callback is being dispatched. The token check above confirms
	 * &parent->base is this exact live occupancy, so the deref is safe. After
	 * this point the scheduler has dequeued this callback, so there is no
	 * longer any scheduled resume for this content - regardless of whether the
	 * lifetime guards below let us actually unpause (DEAD / aborted / PARSER
	 * CHANGED / WILD all bail) or we resume successfully. Clearing here (not
	 * only on the success path) keeps the invariant exact: "unpause_pending"
	 * means a resume is still scheduled. If any bail returned without clearing,
	 * the completion check (html_begin_conversion) would wait forever on a
	 * resume that already decided not to happen - trading Fix A's crash for a
	 * hang. This stays correct no matter how Fix A's bail logic evolves. */
	parent->unpause_pending = false;

	/* fixes521: the fixes520 hlcache_content_is_live() gate was removed here
	 * - a false-negative leaves the parser paused forever (page stuck
	 * "Loading").  The dead/aborted/by-value-parser guards below are the
	 * retained protection. */

	/* fixes517: dead-content guard, defense-in-depth behind the universal
	 * macos9_schedule_cancel_owner() called from content_destroy.  Cancel
	 * normally removes this callback before the content is freed; this guard
	 * covers the narrow race where the cooperative scheduler had already
	 * dequeued the callback before cancel ran.  &parent->base is the content
	 * (base is html_content's first member); if it has been destroyed its
	 * handler is NULL and we must not touch the parser. */
	if (CONTENT_IS_DEAD(&parent->base)) {
		macsurf_debug_log_writef(
			"deferred_unpause: DEAD content=%p, skip", (void *)parent);
		return;
	}
	if (parent->aborted) {
		macsurf_debug_log_writef("deferred_unpause: aborted, skip parser=%p",
			(void *)parent->parser);
		return;
	}
	/* fixes557: PARSER-IDENTITY check - the second lifetime gate this crash
	 * needs.  Static teardown analysis (html_finish_conversion at html.c:341-343,
	 * also :675-676 / :880-881) shows dom_hubbub_parser_destroy(c->parser);
	 * c->parser = NULL runs on the NORMAL conversion-completion path while the
	 * html_content stays LIVE and REGISTERED.  The fixes550 content-generation
	 * token still validates as live in that window (it keys on &parent->base,
	 * not on the parser), so a stale unpause can sail past it and re-enter a
	 * freed hubbub tokeniser / inputstream - the emit_current_tag lbzu r4-wild
	 * crash, whose freed bytes live INSIDE parser->parser, not inside the
	 * content struct.  The parser is created once in html_create and only ever
	 * transitions valid->NULL for a live content (it is never replaced), so
	 * requiring parent->parser to be unchanged from schedule time AND non-NULL
	 * is ABA-proof and cannot false-stall a legitimately-live parse: if the
	 * parser was destroyed (cur==NULL) or the content was freed-and-reused
	 * (cur!=saved), we bail BEFORE any hubbub call.  Safe to deref parent here:
	 * the token check above already confirmed this exact occupancy is live. */
	if ((void *)parent->parser != saved_parser || parent->parser == NULL) {
		macsurf_debug_log_writef(
			"deferred_unpause: PARSER CHANGED saved=%p cur=%p, skip",
			saved_parser, (void *)parent->parser);
		return;
	}
	/* fixes518: validate parser BY VALUE, not just against NULL.  The crash
	 * signature is the unpause firing after the html_content was freed and
	 * its memory reused: parent->parser reads back reuse garbage (observed
	 * r3=B9921C91) or a small non-NULL value (observed r4=1), NOT NULL - so
	 * a "== NULL" guard sails straight through and dom_hubbub_parser_pause
	 * dereferences parser->parser into the tokenizer and dies.  A heap-range
	 * test rejects NULL, 1, and reuse garbage alike; legitimate parser
	 * objects live in the malloc heap (~0x06000000-0x0A000000 on this G3,
	 * 0x01000000-0x20000000 with headroom).  This is the same by-value
	 * pointer gate used in content_broadcast and hlcache_handle_release. */
	{
		if (!macsurf_ptr_is_heap((const void *)(parent->parser))) {
			macsurf_debug_log_writef(
				"deferred_unpause: WILD parser=%p owner=%p, skip",
				(void *)parent->parser, (void *)parent);
			return;
		}
	}
	err = dom_hubbub_parser_pause(parent->parser, false);
	if (err != DOM_HUBBUB_OK) {
		macsurf_debug_log_writef("deferred_unpause: pause returned 0x%x",
			(int)err);
		return;
	}
	/* fixes558: unpause_pending was already cleared unconditionally at dispatch
	 * entry (just past the token check). The parser is resumed now; the
	 * re-drive below completes the parse with the parser no longer paused. */

	/* fixes556 - re-drive post-parse conversion now that the parser has
	 * actually resumed.  Upstream NetSurf unpauses the parser and then
	 * completes the parse in the SAME call (convert_script_*_cb); fixes512
	 * split that ordering by deferring the unpause to this scheduler tick,
	 * so the synchronous html_begin_conversion that ran back in the script
	 * callback saw a still-paused parser.  With the fixes556 change in
	 * html_begin_conversion that case now returns true (wait) instead of
	 * spuriously erroring to about:query/fetcherror - but something must
	 * still finish the job once the parser is live again, or the page
	 * stalls at "Loading".  That is this call.  Gated on
	 * html_can_begin_conversion so we never start conversion while a
	 * newly-revealed sync <script src> is still being fetched
	 * (base.active > 0); that script's own completion callback will
	 * re-drive instead. */
	if (html_can_begin_conversion(parent)) {
		html_begin_conversion(parent);
	}
}

/* fixes550: schedule a parser unpause carrying the content generation token
 * captured NOW, so deferred_parser_unpause can reject a freed/ABA-reused
 * content at dispatch (see struct macos9_unpause_payload).  On OOM we simply
 * skip the unpause: a stalled parse is recoverable; a bad deref is not. */
static void macos9_schedule_unpause(html_content *parent)
{
	struct macos9_unpause_payload *pl =
		(struct macos9_unpause_payload *)malloc(sizeof(*pl));
	if (pl == NULL)
		return;
	pl->parent = parent;
	pl->token = macos9_content_token(&parent->base);
	pl->parser = (void *)parent->parser;   /* fixes557: parser identity */
	guit->misc->schedule(0, deferred_parser_unpause, pl);
	/* fixes558: mark that a resume is now scheduled and imminent. The
	 * parse-completion check (html_begin_conversion) reads this to tell the
	 * cache-hit burst case (PAUSED + base.active==0 + unpause pending => wait)
	 * apart from a genuinely stuck parser (PAUSED + nothing scheduled => real
	 * error). Set only after the schedule succeeds; on the OOM skip above the
	 * flag stays false so the stuck parser is reported rather than hung. */
	parent->unpause_pending = true;
}

/**
 * Callback for syncronous scripts
 */
static nserror
convert_script_sync_cb(hlcache_handle *script,
			  const hlcache_event *event,
			  void *pw)
{
	html_content *parent = pw;
	unsigned int i;
	struct html_script *s;
	script_handler_t *script_handler;
	unsigned int active_sync_scripts = 0;

	/* fixes582 DIAG: unconditional entry probe + heap state. Pins whether the
	 * LAST script's DONE even reaches this callback (vs hanging in the content
	 * broadcast before it), and whether free/largest-block memory collapses by
	 * the final script (exhaustion/fragmentation hypothesis for tinkerdifferent
	 * wedging on script 6). */
	{
		extern long macos9_heap_free_bytes(void);
		extern long macos9_heap_max_block(void);
		macsurf_debug_log_writef(
			"sync_cb: ENTRY type=%d count=%ld free=%ld maxblk=%ld",
			(int)event->type, (long)parent->scripts_count,
			macos9_heap_free_bytes(), macos9_heap_max_block());
	}

	/* fixes535: registry-membership liveness guard FIRST (see async cb). */
	if (macos9_content_is_live(&parent->base) == 0) {
		macsurf_debug_log_writef(
			"convert_script_sync_cb: parent NOT LIVE parent=%p", (void *)parent);
		return NSERROR_OK;
	}

	/* fixes585 - freeze-proof the scripts[] walks below. tinkerdifferent
	 * hard-freezes here spinning `for (i != parent->scripts_count)` when
	 * scripts_count holds a garbage value (heap corruption / html_content
	 * UAF - the same corruptor that cycles sched_queue elsewhere). A real
	 * page has a handful of scripts; anything past a sane ceiling is
	 * corruption. Bail rather than spin the machine, and log the value so
	 * we can trace WHEN it goes bad (correlate against the prior op). */
	if (parent->scripts_count > 1024) {
		macsurf_debug_log_writef(
			"sync_cb: CORRUPT scripts_count=%ld parent=%p - bail",
			(long)parent->scripts_count, (void *)parent);
		return NSERROR_OK;
	}

	/* Count sync scripts which have yet to complete (other than us) */
	for (i = 0, s = parent->scripts; i != parent->scripts_count; i++, s++) {
		if (s->type == HTML_SCRIPT_SYNC &&
		    s->data.handle != script && s->already_started == false) {
			active_sync_scripts++;
		}
	}

	/* Find script */
	for (i = 0, s = parent->scripts; i != parent->scripts_count; i++, s++) {
		if (s->type == HTML_SCRIPT_SYNC && s->data.handle == script)
			break;
	}

	assert(i != parent->scripts_count);

	switch (event->type) {
	case CONTENT_MSG_DONE:
		NSLOG(netsurf, INFO, "script %d done '%s'", i,
		      nsurl_access(hlcache_handle_get_url(script)));
		parent->base.active--;
		NSLOG(netsurf, INFO, "%d fetches active", parent->base.active);

		s->already_started = true;

		/* attempt to execute script */
		script_handler = select_script_handler(content_get_type(s->data.handle));
		if (script_handler != NULL && parent->js_thread != NULL) {
			/* script has a handler */
			const uint8_t *data;
			size_t size;
			data = content_get_source_data(s->data.handle, &size );
			/* fixes847 (#167 S1 census gap) - the sync-script sibling of
			 * html_script_exec's RUN log below. */
			macsurf_debug_log_writef(
				"LIFE script_exec: RUN(sync) bytes=%ld url=%s",
				(long) size,
				nsurl_access(hlcache_handle_get_url(s->data.handle)));
			script_handler(parent->js_thread, data, size,
				       nsurl_access(hlcache_handle_get_url(s->data.handle)));
		} else {
			macsurf_debug_log_writef(
				"LIFE script_exec: SKIP(sync) handler=%p "
				"js_thread=%p content_type=%d url=%s",
				(void *) script_handler, (void *) parent->js_thread,
				(int) content_get_type(s->data.handle),
				nsurl_access(hlcache_handle_get_url(s->data.handle)));
		}

		/* fixes581 DIAG: js_exec returned. If the log ends here (after
		 * 'qjs: exec-return0' but before 'sync_cb: sched done'), the hang is
		 * in macos9_schedule_unpause. */
		macsurf_debug_log_writef("sync_cb: exec done i=%d active_sync=%d",
			(int)i, (int)active_sync_scripts);

		/* continue parse -- deferred to avoid re-entering the tokenizer
		 * from inside the content_broadcast notification walk. */
		if (parent->parser != NULL && active_sync_scripts == 0) {
			macos9_schedule_unpause(parent);
		}

		macsurf_debug_log_writef("sync_cb: sched done i=%d", (int)i);

		break;

	case CONTENT_MSG_ERROR:
		NSLOG(netsurf, INFO, "script %s failed: %s",
		      nsurl_access(hlcache_handle_get_url(script)),
		      event->data.errordata.errormsg);

		/* fixes515: NULL before release so a reentrant callback finds
		 * NULL instead of a freed handle / freed callback TVector. */
		safe_hlcache_handle_release(&s->data.handle);
		parent->base.active--;

		NSLOG(netsurf, INFO, "%d fetches active", parent->base.active);

		s->already_started = true;

		/* continue parse -- deferred, same reason as DONE case above. */
		if (parent->parser != NULL && active_sync_scripts == 0) {
			macos9_schedule_unpause(parent);
		}

		break;

	default:
		break;
	}

	/* if there are no active fetches remaining begin post parse
	 * conversion
	 */
	if (html_can_begin_conversion(parent)) {
		html_begin_conversion(parent);
	}

	return NSERROR_OK;
}

/**
 * process a script with a src tag
 */
static dom_hubbub_error
exec_src_script(html_content *c,
		dom_node *node,
		dom_string *mimetype,
		dom_string *src)
{
	nserror ns_error;
	nsurl *joined;
	hlcache_child_context child;
	struct html_script *nscript;
	bool async;
	bool defer;
	enum html_script_type script_type;
	hlcache_handle_callback script_cb;
	dom_hubbub_error ret = DOM_HUBBUB_OK;
	dom_exception exc; /* returned by libdom functions */

	/* src url */
	ns_error = nsurl_join(c->base_url, dom_string_data(src), &joined);
	if (ns_error != NSERROR_OK) {
		content_broadcast_error(&c->base, NSERROR_NOMEM, NULL);
		return DOM_HUBBUB_NOMEM;
	}

	NSLOG(netsurf, INFO, "script %i '%s'", c->scripts_count,
	      nsurl_access(joined));

	/* there are three ways to process the script tag at this point:
	 *
	 * Syncronously  pause the parent parse and continue after
	 *                 the script has downloaded and executed. (default)
	 * Async         Start the script downloading and execute it when it
	 *                 becomes available.
	 * Defered       Start the script downloading and execute it when
	 *                 the page has completed parsing, may be set along
	 *                 with async where it is ignored.
	 */

	/* we interpret the presence of the async and defer attribute
	 * as true and ignore its value, technically only the empty
	 * value or the attribute name itself are valid. However
	 * various browsers interpret this in various ways the most
	 * compatible approach is to be liberal and accept any
	 * value. Note setting the values to "false" still makes them true!
	 */
	exc = dom_element_has_attribute(node, corestring_dom_async, &async);
	if (exc != DOM_NO_ERR) {
		return DOM_HUBBUB_OK; /* dom error */
	}

	if (c->parse_completed) {
		/* After parse completed, all scripts are essentially async */
		async = true;
		defer = false;
	}

	if (async) {
		/* asyncronous script */
		script_type = HTML_SCRIPT_ASYNC;
		script_cb = convert_script_async_cb;

	} else {
		exc = dom_element_has_attribute(node,
						corestring_dom_defer, &defer);
		if (exc != DOM_NO_ERR) {
			return DOM_HUBBUB_OK; /* dom error */
		}

		if (defer) {
			/* defered script */
			script_type = HTML_SCRIPT_DEFER;
			script_cb = convert_script_defer_cb;
		} else {
			/* syncronous script */
			script_type = HTML_SCRIPT_SYNC;
			script_cb = convert_script_sync_cb;
		}
	}

	nscript = html_process_new_script(c, mimetype, script_type);
	if (nscript == NULL) {
		nsurl_unref(joined);
		content_broadcast_error(&c->base, NSERROR_NOMEM, NULL);
		return DOM_HUBBUB_NOMEM;
	}

	/* fixes869 (#295) - remember the element so html_script_exec can fire
	 * `load` at it (and convert_script_async_cb `error`).  Only worth it for a
	 * JS-INSERTED script: a parser-inserted one has no JS waiting on its
	 * onload, and skipping those keeps a ref off every <script> on every page.
	 * Owned ref; released in html_script_free. */
	if (c->parse_completed) {
		nscript->node = node;
		dom_node_ref(node);
	}

	/* set up child fetch encoding and quirks */
	child.charset = c->encoding;
	child.quirks = c->base.quirks;

	ns_error = hlcache_handle_retrieve(joined,
					   0,
					   content_get_url(&c->base),
					   NULL,
					   script_cb,
					   c,
					   &child,
					   CONTENT_SCRIPT,
					   &nscript->data.handle);


	nsurl_unref(joined);

	if (ns_error != NSERROR_OK) {
		/* @todo Deal with fetch error better. currently assume
		 * fetch never became active
		 */
		/* mark duff script fetch as already started */
		nscript->already_started = true;
		NSLOG(netsurf, INFO, "Fetch failed with error %d", ns_error);
	} else {
		/* update base content active fetch count */
		c->base.active++;
		NSLOG(netsurf, INFO, "%d fetches active", c->base.active);

		switch (script_type) {
		case HTML_SCRIPT_SYNC:
			ret =  DOM_HUBBUB_HUBBUB_ERR | HUBBUB_PAUSED;
			break;

		case HTML_SCRIPT_ASYNC:
			break;

		case HTML_SCRIPT_DEFER:
			break;

		default:
			assert(0);
		}
	}

	return ret;
}

static dom_hubbub_error
exec_inline_script(html_content *c, dom_node *node, dom_string *mimetype)
{
	dom_string *script;
	dom_exception exc; /* returned by libdom functions */
	struct lwc_string_s *lwcmimetype;
	script_handler_t *script_handler;
	struct html_script *nscript;

	/* does not appear to be a src so script is inline content */
	exc = dom_node_get_text_content(node, &script);
	if ((exc != DOM_NO_ERR) || (script == NULL)) {
		return DOM_HUBBUB_OK; /* no contents, skip */
	}

	nscript = html_process_new_script(c, mimetype, HTML_SCRIPT_INLINE);
	if (nscript == NULL) {
		dom_string_unref(script);

		content_broadcast_error(&c->base, NSERROR_NOMEM, NULL);
		return DOM_HUBBUB_NOMEM;

	}

	nscript->data.string = script;
	nscript->already_started = true;

	/* ensure script handler for content type */
	exc = dom_string_intern(mimetype, &lwcmimetype);
	if (exc != DOM_NO_ERR) {
		return DOM_HUBBUB_DOM;
	}

	script_handler = select_script_handler(content_factory_type_from_mime_type(lwcmimetype));
	lwc_string_unref(lwcmimetype);

	if (script_handler != NULL) {
		script_handler(c->js_thread,
			       (const uint8_t *)dom_string_data(script),
			       dom_string_byte_length(script),
			       "?inline script?");
	} else {
		/* Silent skip, same class as the fixes847 async/defer gap:
		 * an inline <script> whose declared type doesn't map to
		 * CONTENT_JS (e.g. type="application/json" data islands,
		 * common in modern bundlers) never runs and, until now,
		 * nothing said so. */
		macsurf_debug_log_writef(
			"LIFE script_exec: SKIP inline unsupported "
			"mimetype=%s len=%ld",
			dom_string_data(mimetype),
			(long) dom_string_byte_length(script));
	}
	return DOM_HUBBUB_OK;
}


/**
 * process script node parser callback
 *
 *
 */
dom_hubbub_error
html_process_script(void *ctx, dom_node *node)
{
	html_content *c = (html_content *)ctx;
	dom_exception exc; /* returned by libdom functions */
	dom_string *src, *mimetype;
	dom_hubbub_error err = DOM_HUBBUB_OK;

	/* ensure javascript context is available */
	/* We should only ever be here if scripting was enabled for this
	 * content so it's correct to make a javascript context if there
	 * isn't one already. */
	if (c->js_thread == NULL) {
		union content_msg_data msg_data;

		msg_data.jsthread = &c->js_thread;
		content_broadcast(&c->base, CONTENT_MSG_GETTHREAD, &msg_data);
		NSLOG(netsurf, INFO, "javascript context %p ", c->js_thread);
		if (c->js_thread == NULL) {
			/* no context and it could not be created, abort */
			return DOM_HUBBUB_OK;
		}
	}

	NSLOG(netsurf, INFO, "content %p parser %p node %p", c, c->parser,
	      node);

	exc = dom_element_get_attribute(node, corestring_dom_type, &mimetype);
	if (exc != DOM_NO_ERR || mimetype == NULL) {
		mimetype = dom_string_ref(corestring_dom_text_javascript);
	}

	/* Check for type="module" NOW, before the src attribute fetch,
	 * so we can detect inline module scripts. */
	{
		const char *mt = (const char *)dom_string_data(mimetype);
		if (mt != NULL && strcmp(mt, "module") == 0) {
				macsurf_debug_log_writef(
					"LIFE script: module script found");
			/* Get src to decide inline vs external */
			exc = dom_element_get_attribute(node,
				corestring_dom_src, &src);

			if (exc != DOM_NO_ERR || src == NULL) {
				/* INLINE module: execute with real module
				 * semantics via js_exec_module. */
				dom_string *modsrc;
				dom_exception mexc;
				mexc = dom_node_get_text_content(
					node, &modsrc);
				if (mexc == DOM_NO_ERR &&
						modsrc != NULL) {
					macsurf_debug_log_writef(
						"LIFE script: module exec inline "
						"len=%ld",
						(long)dom_string_byte_length(
							modsrc));
					js_exec_module(c->js_thread,
						(const unsigned char *)
							dom_string_data(modsrc),
						dom_string_byte_length(modsrc),
						"?inline module?");
					dom_string_unref(modsrc);
				} else {
					macsurf_debug_log_writef(
						"LIFE script: module inline "
						"FAILED mexc=%d modsrc=%p",
						(int)mexc, (void *)modsrc);
				}
				dom_string_unref(mimetype);
				return DOM_HUBBUB_OK;
			}

			/* EXTERNAL module: remap and let the regular
			 * src-script path handle it. js_exec_module
			 * threading through the async callback is
			 * deferred; external module scripts get
			 * regular-script treatment for now (still a
			 * massive improvement over the silent drop). */
			dom_string_unref(mimetype);
			mimetype = dom_string_ref(
				corestring_dom_text_javascript);
			err = exec_src_script(c, node, mimetype, src);
			dom_string_unref(src);
			dom_string_unref(mimetype);
			return err;
		}
	}

	exc = dom_element_get_attribute(node, corestring_dom_src, &src);
	if (exc != DOM_NO_ERR || src == NULL) {
		err = exec_inline_script(c, node, mimetype);
	} else {
		err = exec_src_script(c, node, mimetype, src);
		dom_string_unref(src);
	}

	dom_string_unref(mimetype);

	return err;
}

/* exported internal interface documented in html/html_internal.h */
bool html_saw_insecure_scripts(html_content *htmlc)
{
	struct html_script *s;
	unsigned int i;

	for (i = 0, s = htmlc->scripts; i != htmlc->scripts_count; i++, s++) {
		if (s->type == HTML_SCRIPT_INLINE) {
			/* Inline scripts are no less secure than their
			 * containing HTML content
			 */
			continue;
		}
		if (s->data.handle == NULL) {
			/* We've not begun loading this? */
			continue;
		}
		if (content_saw_insecure_objects(s->data.handle)) {
			return true;
		}
	}

	return false;
}

/* exported internal interface documented in html/html_internal.h */
nserror html_script_free(html_content *html)
{
	struct html_script *scripts = html->scripts;
	unsigned int count = html->scripts_count;
	unsigned int i;

	/* fixes600: DETACH the script list from the content BEFORE releasing
	 * any handle. Releasing a script handle can re-enter teardown
	 * (convert_script_async_cb / llcache_catch_up_all_users can drive a
	 * JS content to DONE and call back into this content), so a
	 * re-entrant or repeated html_script_free / content_destroy must not
	 * walk this list a second time -- doing so would free `scripts` out
	 * from under the outer loop and double-release an already-freed
	 * handle. With html->scripts NULL and the count 0 up front, any
	 * second reach is a no-op and the freed slots are unreachable (an
	 * ownership transfer, not a validity guard). The fixes499e/501x
	 * null-on-release via safe_hlcache_handle_release is preserved. */
	html->scripts = NULL;
	html->scripts_count = 0;

	if (scripts == NULL) {
		return NSERROR_OK;
	}

	for (i = 0; i != count; i++) {
		if (scripts[i].mimetype != NULL) {
			dom_string_unref(scripts[i].mimetype);
		}

		/* fixes869 (#295) - release the <script> element ref taken in
		 * exec_src_script.  NULL it first: this teardown is documented above
		 * as re-entrant (releasing a handle can drive a JS content to DONE and
		 * call back in), so the same null-before-release discipline the
		 * handle uses applies here. */
		if (scripts[i].node != NULL) {
			struct dom_node *n = scripts[i].node;
			scripts[i].node = NULL;
			dom_node_unref(n);
		}

		switch (scripts[i].type) {
		case HTML_SCRIPT_INLINE:
			if (scripts[i].data.string != NULL) {
				dom_string_unref(scripts[i].data.string);
			}
			break;
		case HTML_SCRIPT_SYNC:
			/* fallthrough */
		case HTML_SCRIPT_ASYNC:
			/* fallthrough */
		case HTML_SCRIPT_DEFER:
			if (scripts[i].data.handle != NULL) {
				/* fixes499e/501x - NULL the handle BEFORE release
				 * (via the safe wrapper). Without this, a second
				 * teardown pass (html_destroy fired from the scheduler
				 * after the content's handles were already released
				 * elsewhere) re-enters here with a stale pointer and
				 * hlcache_handle_release dereferences freed memory.
				 * Crash signature: unmapped-memory exception in
				 * hlcache_handle_release+24 reading off+0x30 of a bad
				 * handle, stack html_script_free -> html_destroy ->
				 * content_destroy -> macos9_schedule_run. NULLing
				 * before release makes it idempotent and closes the
				 * re-entrant-read window during the release itself. */
				safe_hlcache_handle_release(
					&scripts[i].data.handle);
			}
			break;
		}
	}
	free(scripts);
	/* fixes499e - also NULL the array + count so a duplicate
	 * html_script_free / html_destroy can't walk freed `scripts` memory.
	 * The loop guard (i != scripts_count) then runs zero times. */
	html->scripts = NULL;
	html->scripts_count = 0;

	return NSERROR_OK;
}
