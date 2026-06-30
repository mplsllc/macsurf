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
			if (script_handler == NULL)
				continue; /* unsupported type */

			if (content_get_status(s->data.handle) ==
					CONTENT_STATUS_DONE) {
				/* external script is now available */
				const uint8_t *data;
				size_t size;
				data = content_get_source_data(
						s->data.handle, &size );
				script_handler(c->js_thread, data, size,
					       nsurl_access(hlcache_handle_get_url(s->data.handle)));
				have_run_something = true;
				/* We have to re-acquire this here since the
				 * c->scripts array may have been reallocated
				 * as a result of executing this script.
				 */
				s = &(c->scripts[i]);

				s->already_started = true;

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

	/* fixes557: ENTRY instrumentation — logged BEFORE the token check and
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

	/* fixes550: GENERATION-TOKEN validation — the actual fix for the
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
	 * longer any scheduled resume for this content — regardless of whether the
	 * lifetime guards below let us actually unpause (DEAD / aborted / PARSER
	 * CHANGED / WILD all bail) or we resume successfully. Clearing here (not
	 * only on the success path) keeps the invariant exact: "unpause_pending"
	 * means a resume is still scheduled. If any bail returned without clearing,
	 * the completion check (html_begin_conversion) would wait forever on a
	 * resume that already decided not to happen — trading Fix A's crash for a
	 * hang. This stays correct no matter how Fix A's bail logic evolves. */
	parent->unpause_pending = false;

	/* fixes521: the fixes520 hlcache_content_is_live() gate was removed here
	 * — a false-negative leaves the parser paused forever (page stuck
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
	/* fixes557: PARSER-IDENTITY check — the second lifetime gate this crash
	 * needs.  Static teardown analysis (html_finish_conversion at html.c:341-343,
	 * also :675-676 / :880-881) shows dom_hubbub_parser_destroy(c->parser);
	 * c->parser = NULL runs on the NORMAL conversion-completion path while the
	 * html_content stays LIVE and REGISTERED.  The fixes550 content-generation
	 * token still validates as live in that window (it keys on &parent->base,
	 * not on the parser), so a stale unpause can sail past it and re-enter a
	 * freed hubbub tokeniser / inputstream — the emit_current_tag lbzu r4-wild
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
	 * r3=B9921C91) or a small non-NULL value (observed r4=1), NOT NULL — so
	 * a "== NULL" guard sails straight through and dom_hubbub_parser_pause
	 * dereferences parser->parser into the tokenizer and dies.  A heap-range
	 * test rejects NULL, 1, and reuse garbage alike; legitimate parser
	 * objects live in the malloc heap (~0x06000000-0x0A000000 on this G3,
	 * 0x01000000-0x20000000 with headroom).  This is the same by-value
	 * pointer gate used in content_broadcast and hlcache_handle_release. */
	{
		unsigned long pa = (unsigned long)(void *)parent->parser;
		if (pa < 0x01000000UL || pa >= 0x20000000UL) {
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

	/* fixes556 — re-drive post-parse conversion now that the parser has
	 * actually resumed.  Upstream NetSurf unpauses the parser and then
	 * completes the parse in the SAME call (convert_script_*_cb); fixes512
	 * split that ordering by deferring the unpause to this scheduler tick,
	 * so the synchronous html_begin_conversion that ran back in the script
	 * callback saw a still-paused parser.  With the fixes556 change in
	 * html_begin_conversion that case now returns true (wait) instead of
	 * spuriously erroring to about:query/fetcherror — but something must
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

	/* fixes535: registry-membership liveness guard FIRST (see async cb). */
	if (macos9_content_is_live(&parent->base) == 0) {
		macsurf_debug_log_writef(
			"convert_script_sync_cb: parent NOT LIVE parent=%p", (void *)parent);
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
			script_handler(parent->js_thread, data, size,
				       nsurl_access(hlcache_handle_get_url(s->data.handle)));
		}

		/* continue parse -- deferred to avoid re-entering the tokenizer
		 * from inside the content_broadcast notification walk. */
		if (parent->parser != NULL && active_sync_scripts == 0) {
			macos9_schedule_unpause(parent);
		}

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
	unsigned int i;

	for (i = 0; i != html->scripts_count; i++) {
		if (html->scripts[i].mimetype != NULL) {
			dom_string_unref(html->scripts[i].mimetype);
		}

		switch (html->scripts[i].type) {
		case HTML_SCRIPT_INLINE:
			if (html->scripts[i].data.string != NULL) {
				dom_string_unref(html->scripts[i].data.string);
			}
			break;
		case HTML_SCRIPT_SYNC:
			/* fallthrough */
		case HTML_SCRIPT_ASYNC:
			/* fallthrough */
		case HTML_SCRIPT_DEFER:
			if (html->scripts[i].data.handle != NULL) {
				/* fixes499e/501x — NULL the handle BEFORE release
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
					&html->scripts[i].data.handle);
			}
			break;
		}
	}
	free(html->scripts);
	/* fixes499e — also NULL the array + count so a duplicate
	 * html_script_free / html_destroy can't walk freed `scripts` memory.
	 * The loop guard (i != scripts_count) then runs zero times. */
	html->scripts = NULL;
	html->scripts_count = 0;

	return NSERROR_OK;
}
