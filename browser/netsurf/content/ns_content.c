/*
 * Copyright 2005-2007 James Bursa <bursa@users.sourceforge.net>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * Content handling implementation.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <nsutils/time.h>

#include "netsurf/inttypes.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/corestrings.h"
#include "netsurf/browser_window.h"
#include "netsurf/bitmap.h"
#include "netsurf/content.h"
#include "desktop/knockout.h"

#include "content/content_protected.h"
#include "content/textsearch.h"
#include "content/content_debug.h"
#include "content/hlcache.h"
#include "content/urldb.h"

#include "macsurf_debug.h"
#include "macos9_deathrow.h"

/* fixes517: frontend scheduler cancellation. Forward-declared here (core
 * file, Mac-only fork) rather than pulling the macos9 frontend header into
 * content core - same approach as the macsurf_debug_log_writef forward decl
 * in content_protected.h.  No-op on non-Mac syntax-check builds. */
#ifdef __MACOS9__
extern void macos9_schedule_cancel_owner(void *p);
/* fixes533: out-of-band live-content registry (anti-UAF).  A content
 * registers at content__init and deregisters as the first action of
 * content_destroy, so a scheduled callback can validate it by registry
 * membership instead of reading its (reusable) bytes.  Implemented in
 * frontends/macos9/macos9_content_registry.c. */
extern void macos9_content_register(struct content *c);
extern void macos9_content_unregister(struct content *c);
#else
#define macos9_schedule_cancel_owner(p) ((void)0)
#define macos9_content_register(c) ((void)0)
#define macos9_content_unregister(c) ((void)0)
#endif

extern int macsurf_ptr_is_heap(const void *);
extern int macsurf_ptr_is_valid(const void *);

#define URL_FMT_SPC "%.140s"

const char * const content_status_name[] = {
	"LOADING",
	"READY",
	"DONE",
	"ERROR"
};

/* fixes515: GLOBAL content_broadcast reentrancy guard.
 *
 * The per-content c->broadcast_in_progress flag (fixes500) only stops a
 * content re-broadcasting ITSELF during its own walk.  But the real crash
 * stacks show two content_broadcast frames on TWO DIFFERENT contents:
 * broadcast(A) -> hlcache_content_callback -> content_convert(B) ->
 * content_set_ready/done(B) -> broadcast(B) -> convert_script_sync_cb ->
 * use-after-free.  A per-content flag lets B's broadcast through because B
 * != A.  This module-global flag serialises ALL content broadcasts: any
 * broadcast that begins while another is mid-walk - on any content - is
 * deferred (its message stored on the content) and replayed on the next
 * event-loop tick via hlcache's catch-up pump.  Mirrors llcache's pairing
 * of object->notify_in_progress with the global llcache_notify_in_progress
 * (fixes459/460). */
static bool content_broadcast_active = false;


/**
 * All data has arrived, convert for display.
 *
 * Calls the convert function for the content.
 *
 * - If the conversion succeeds, but there is still some processing required
 *   (eg. loading images), the content gets status CONTENT_STATUS_READY, and a
 *   CONTENT_MSG_READY is sent to all users.
 * - If the conversion succeeds and is complete, the content gets status
 *   CONTENT_STATUS_DONE, and CONTENT_MSG_READY then CONTENT_MSG_DONE are sent.
 * - If the conversion fails, CONTENT_MSG_ERROR is sent. The content will soon
 *   be destroyed and must no longer be used.
 */
static void content_convert(struct content *c)
{
	assert(c);
	if (c->handler == NULL) {
		macsurf_debug_log_writef(
			"DESTROYED CONTENT in content_convert content=%p",
			(void *)c);
		return;
	}
	assert(c->status == CONTENT_STATUS_LOADING ||
	       c->status == CONTENT_STATUS_ERROR);

	if (c->status != CONTENT_STATUS_LOADING)
		return;

	if (c->locked == true)
		return;

	nslog_log(__FILE__, "", __LINE__,
		      "content "URL_FMT_SPC" (%p)",
		      (c != NULL && c->llcache != NULL && llcache_handle_get_url(c->llcache) != NULL ? nsurl_access_log(llcache_handle_get_url(c->llcache)) : "(null)"),
		      c);

	if (c->handler->data_complete != NULL) {
		c->locked = true;
		if (c->handler->data_complete(c) == false) {
			content_set_error(c);
			content_broadcast_error(c, NSERROR_UNKNOWN, "Data complete failed");
		}
		/* Conversion to the READY state will unlock the content */
	} else {
		content_set_ready(c);
		content_set_done(c);
	}
}


/**
 * Handler for low-level cache events
 *
 * \param llcache  Low-level cache handle
 * \param event	   Event details
 * \param pw	   Pointer to our context
 * \return NSERROR_OK on success, appropriate error otherwise
 */
static nserror
content_llcache_callback(llcache_handle *llcache,
			 const llcache_event *event, void *pw)
{
	struct content *c = pw;
	union content_msg_data msg_data;
	nserror error = NSERROR_OK;

	switch (event->type) {
	case LLCACHE_EVENT_GOT_CERTS:
		/* Will never happen: handled in hlcache */
		break;
	case LLCACHE_EVENT_HAD_HEADERS:
		/* Will never happen: handled in hlcache */
		break;
	case LLCACHE_EVENT_HAD_DATA:
		if (c->handler->process_data != NULL) {
			if (c->handler->process_data(c,
						     (const char *) event->data.data.buf,
						     event->data.data.len) == false) {
				llcache_handle_abort(c->llcache);
				c->status = CONTENT_STATUS_ERROR;
				/** \todo It's not clear what error this is */
				error = NSERROR_NOMEM;
				content_broadcast_error(c, error, "Data processing failed");
			}
		}
		break;
	case LLCACHE_EVENT_DONE:
		{
			size_t source_size;

			(void) llcache_handle_get_source_data(llcache, &source_size);

			content_set_status(c, messages_get("Processing"));
			msg_data.explicit_status_text = NULL;
			content_broadcast(c, CONTENT_MSG_STATUS, &msg_data);

			content_convert(c);
		}
		break;
	case LLCACHE_EVENT_ERROR:
		/** \todo Error page? */
		c->status = CONTENT_STATUS_ERROR;
		msg_data.errordata.errorcode = event->data.error.code;
		msg_data.errordata.errormsg = event->data.error.msg;
		content_broadcast(c, CONTENT_MSG_ERROR, &msg_data);
		break;
	case LLCACHE_EVENT_PROGRESS:
		content_set_status(c, event->data.progress_msg);
		msg_data.explicit_status_text = NULL;
		content_broadcast(c, CONTENT_MSG_STATUS, &msg_data);
		break;
	case LLCACHE_EVENT_REDIRECT:
		msg_data.redirect.from = event->data.redirect.from;
		msg_data.redirect.to = event->data.redirect.to;
		content_broadcast(c, CONTENT_MSG_REDIRECT, &msg_data);
		break;
	}

	return error;
}


/**
 * update content status message
 *
 * \param c the content to update.
 */
static void content_update_status(struct content *c)
{
	if (c->handler == NULL) return; /* destroyed sentinel */
	if (c->status == CONTENT_STATUS_LOADING ||
	    c->status == CONTENT_STATUS_READY) {
		/* Not done yet */
		snprintf(c->status_message, sizeof (c->status_message),
			 "%s%s%s", messages_get("Fetching"),
			 c->sub_status[0] != '\0' ? ", " : " ",
			 c->sub_status);
	} else {
		snprintf(c->status_message, sizeof (c->status_message),
			 "%s (%.1fs)", messages_get("Done"),
			 (float) c->time / 1000);
	}
}


/* exported interface documented in content/protected.h */
nserror
content__init(struct content *c,
	      const content_handler *handler,
	      lwc_string *imime_type,
	      const struct http_parameter *params,
	      llcache_handle *llcache,
	      const char *fallback_charset,
	      bool quirks)
{
	struct content_user *user_sentinel;
	nserror error;

	nslog_log(__FILE__, "", __LINE__,
		      "url "URL_FMT_SPC" -> %p",
		      nsurl_access_log(llcache_handle_get_url(llcache)),
		      c);

	user_sentinel = calloc(1, sizeof(struct content_user));
	if (user_sentinel == NULL) {
		return NSERROR_NOMEM;
	}

	if (fallback_charset != NULL) {
		c->fallback_charset = strdup(fallback_charset);
		if (c->fallback_charset == NULL) {
			free(user_sentinel);
			return NSERROR_NOMEM;
		}
	}

	c->llcache = llcache;
	c->mime_type = lwc_string_ref(imime_type);
	c->handler = handler;

	/* fixes533: register as live the instant the destroyed-sentinel (handler)
	 * is set, so any scheduled callback that resolves to this content can
	 * validate it by registry membership rather than reading its bytes. */
	macos9_content_register(c);
	c->status = CONTENT_STATUS_LOADING;
	c->width = 0;
	c->height = 0;
	c->available_width = 0;
	c->available_height = 0;
	c->quirks = quirks;
	c->refresh = 0;
	nsu_getmonotonic_ms(&c->time);
	c->size = 0;
	c->title = NULL;
	c->active = 0;
	user_sentinel->callback = NULL;
	user_sentinel->pw = NULL;
	user_sentinel->next = NULL;
	c->user_list = user_sentinel;
	c->sub_status[0] = 0;
	c->locked = false;
	c->broadcast_in_progress = false;
	c->broadcast_pending_count = 0;
	c->total_size = 0;
	c->http_code = 0;

	c->textsearch.string = NULL;
	c->textsearch.context = NULL;

	content_set_status(c, messages_get("Loading"));

	/* Finally, claim low-level cache events */
	error = llcache_handle_change_callback(llcache,
					       content_llcache_callback, c);
	if (error != NSERROR_OK) {
		/* fixes534: unregister before the caller frees us on this
		 * create-time failure, so no stale 'live' slot is left behind. */
		macos9_content_unregister(c);
		lwc_string_unref(c->mime_type);
		return error;
	}

	return NSERROR_OK;
}


/* exported interface documented in content/content.h */
bool content_can_reformat(hlcache_handle *h)
{
	struct content *c = hlcache_handle_get_content(h);

	if (c == NULL)
		return false;

	return (c->handler->reformat != NULL);
}


/* exported interface documented in content/protected.h */
void content_set_status(struct content *c, const char *status_message)
{
	size_t len;

	CONTENT_CHECK_VOID(c);

	len = strlen(status_message);

	if (len >= sizeof(c->sub_status)) {
		len = sizeof(c->sub_status) - 1;
	}
	memcpy(c->sub_status, status_message, len);
	c->sub_status[len] = '\0';

	content_update_status(c);
}


/* exported interface documented in content/protected.h */
void content_set_ready(struct content *c)
{
	/* destroyed sentinel - content_destroy clears handler before freeing */
	if (c->handler == NULL) {
		macsurf_debug_log_writef(
			"DESTROYED CONTENT in content_set_ready content=%p",
			(void *)c);
		return;
	}
	/* The content must be locked at this point, as it can only
	 * become READY after conversion. */
	assert(c->locked);
	c->locked = false;

	c->status = CONTENT_STATUS_READY;
	content_update_status(c);
	content_broadcast(c, CONTENT_MSG_READY, NULL);
}


/* exported interface documented in content/protected.h */
void content_set_done(struct content *c)
{
	uint64_t now_ms;

	/* destroyed sentinel */
	if (c->handler == NULL) {
		macsurf_debug_log_writef(
			"DESTROYED CONTENT in content_set_done content=%p",
			(void *)c);
		return;
	}

	nsu_getmonotonic_ms(&now_ms);

	c->status = CONTENT_STATUS_DONE;
	c->time = now_ms - c->time;
	content_update_status(c);
	content_broadcast(c, CONTENT_MSG_DONE, NULL);
}


/* exported interface documented in content/protected.h */
void content_set_error(struct content *c)
{
	CONTENT_CHECK_VOID(c);
	c->locked = false;
	c->status = CONTENT_STATUS_ERROR;
}


/* exported interface documented in content/content.h */
void content_reformat(hlcache_handle *h, bool background,
		      int width, int height)
{
	content__reformat(hlcache_handle_get_content(h), background,
			  width, height);
}


/* exported interface documented in content/protected.h */
void
content__reformat(struct content *c, bool background, int width, int height)
{
	union content_msg_data data;
	/* fixes513: last-resort NULL guard; browser_window_content_ready
	 * catches the reentrant case first, but guard here too since
	 * content_reformat(hlcache_handle_get_content(h),...) can reach us
	 * with NULL if the handle was released mid-broadcast. */
	if (c == NULL) {
		macsurf_debug_log_writef("content__reformat: NULL content, skip");
		return;
	}
	if (c->handler == NULL) {
		macsurf_debug_log_writef(
			"content__reformat: DESTROYED content=%p skip",
			(void *)c);
		return;
	}
	assert(c->status == CONTENT_STATUS_READY ||
	       c->status == CONTENT_STATUS_DONE);
	assert(c->locked == false);

	c->available_width = width;
	c->available_height = height;
	if (c->handler->reformat != NULL) {

		c->locked = true;
		c->handler->reformat(c, width, height);
		c->locked = false;

		data.background = background;
		content_broadcast(c, CONTENT_MSG_REFORMAT, &data);
	}
}


#ifdef __MACOS9__
/* fixes552 - writer-side deferred-destroy list.  When a box walk currently on
 * the stack still references a content (macos9_box_walk_owns_content, defined in
 * box_construct.c), freeing it now crashes the walk; queue it here and let the
 * walk drain it the instant the batch returns (macos9_content_drain_deferred),
 * once the references are gone.  Bounded + deduped; a full table leaks rather
 * than free under a live walk.  NOTE (fixes565-577 arc): content_destroy now
 * routes every free through the op-depth-gated death row (below), so this
 * writer-side list is a secondary guard for the box-walk-owns case referenced
 * from content_destroy_now; the drain hook is retained for that path. */
extern int macos9_box_walk_owns_content(struct content *c);

#define MACOS9_DEFER_MAX 32
static struct content *macos9_defer_list[MACOS9_DEFER_MAX];
static int macos9_defer_count = 0;

void macos9_content_drain_deferred(void)
{
	/* fixes901 - suppress the deferred-content free-drain while a reconvert box
	 * build is in flight. The box-convert wrapper calls this between batches;
	 * a content_destroy running in that gap on a half-rebuilt tree is the
	 * box-node-80 reconvert crash. The frees accumulate here and drain on the
	 * first poll after html_reconvert_done clears the flag (via the poll-level
	 * call added in macos9_deathrow_drain). */
	{
		extern int macsurf_reconvert_in_progress;
		if (macsurf_reconvert_in_progress) {
			return;
		}
	}
	while (macos9_defer_count > 0) {
		struct content *dc = macos9_defer_list[--macos9_defer_count];
		macos9_defer_list[macos9_defer_count] = NULL;
		content_destroy(dc);   /* walk is off-stack now -> owns()=0 -> proceeds */
	}
}
#endif


/* Stage 1 death-row: content_destroy now DEFERS. It unlinks/ownership has
 * already happened at the call site (hlcache_clean unlinks from
 * content_list synchronously); here we only enqueue the real teardown,
 * which runs at the quiescent drain (no walk/convert on the stack, no
 * scheduled continuation still referencing c). Idempotent via c->dr_queued
 * so a double content_destroy can never double-free. */
static void content_destroy_now(struct content *c);

/* The JS DOM keeps a raw current-content pointer (macsurf_js_dom.c);
 * Seam 1 requires NULLing it before the content's memory is reclaimed. */
extern void macsurf_js_notify_content_freed(struct content *c);

static void
content_deathrow_teardown(void *p)
{
	struct content *c = (struct content *) p;
	macsurf_js_notify_content_freed(c);
	content_destroy_now(c);
}

#ifdef __MACOS9__
/* fixes600 - teardown for a deferred content_user free. content_remove_user is
 * reached from inside a content_broadcast callback (via
 * hlcache_handle_release), so freeing the content_user node synchronously there
 * - while op_depth>0 and the broadcast loop still holds it as its captured
 * `next` (or current iterator) - frees a 12/16-byte node whose size class
 * aliases sched_entry/dom_string, smashing the free-list. The death-row defers
 * it to the quiescent drain (op_depth==0), matching how the spine nodes are
 * freed. */
static void
content_user_deathrow_teardown(void *p)
{
	free(p);
}
#endif

/* exported interface documented in content/content.h */
void content_destroy(struct content *c)
{
	if (c == NULL || c->dr_queued) {
		return;
	}
	c->dr_queued = 1;
	macos9_deathrow_add(c, content_deathrow_teardown, c);
}

static void
content_destroy_now(struct content *c)
{
	struct content_rfc5988_link *link;
	const struct content_handler *h;

	/* fixes508: NULL guard and double-destroy sentinel must be the
	 * absolute first operations - before any assert, log, or field
	 * access.  On a double-destroy c is a valid non-NULL address (just
	 * freed/reused memory), so assert(c) passes but c->handler is NULL
	 * (we zeroed it on the first call).  We use c->handler as the
	 * sentinel: set at creation, cleared on first destroy, bail if NULL.
	 *
	 * Cannot use c->status == CONTENT_STATUS_ERROR: hlcache_clean's
	 * force-clean path calls content_set_error() before content_destroy,
	 * so ERROR at entry is a valid first-destroy, not a double.
	 *
	 * Crash signature prevented: hlcache_clean fires 100ms after the
	 * first content_destroy ran via content_remove_user -> zero-users.
	 * By the second call c->handler has been freed/reused (heap reuse
	 * pattern A6875475 in r3/r19 in MacsBug) and the vtable dispatch
	 * crashes.  The sentinel bails before any field is touched. */
	if (c == NULL) return;

#ifdef __MACOS9__
	/* fixes552 - WRITER-SIDE free guard, the systemic cure for the
	 * free-during-box-walk crash family (fixes547 box->style, fixes550 parser):
	 * if a box walk currently on the stack still references this content, do NOT
	 * free it now - defer until the walk's batch returns and drains it.  Placed
	 * BEFORE the double-destroy sentinel (so the deferred re-call still sees a
	 * live handler and actually destroys) and BEFORE the registry unregister (so
	 * the content stays correctly live during the deferral; it is not freed yet).
	 * macos9_box_walk_owns_content gen-checks against ABA, so a freed+reused
	 * address is NOT deferred and frees normally. */
	if (macos9_box_walk_owns_content(c)) {
		int i;
		for (i = 0; i < macos9_defer_count; i++) {
			if (macos9_defer_list[i] == c) break;
		}
		if (i == macos9_defer_count) {
			if (macos9_defer_count < MACOS9_DEFER_MAX) {
				macos9_defer_list[macos9_defer_count++] = c;
				macsurf_debug_log_writef(
					"content_destroy: DEFERRED (walk live) c=%p", (void *)c);
			} else {
				macsurf_debug_log_writef(
					"content_destroy: DEFER TABLE FULL leak c=%p", (void *)c);
			}
		}
		return;
	}
#endif

	h = c->handler;
	if (h == NULL) {
		macsurf_debug_log_writef(
			"content_destroy: DOUBLE DESTROY BLOCKED content=%p",
			(void *)c);
		return;
	}
	c->handler = NULL; /* sentinel: any re-entrant call bails above */

	/* fixes533: deregister from the live-content registry as the first
	 * mutating action after the sentinel is cleared, BEFORE anything is
	 * freed.  From here macos9_content_is_live(c) is false, so a scheduled
	 * callback that resolves to this content (directly or via a sub-context's
	 * parent pointer) is rejected at fire time without ever dereferencing
	 * this soon-to-be-freed struct. */
	macos9_content_unregister(c);

	/* fixes517: UNIVERSAL anti-UAF - cancel every scheduled callback owned
	 * by this content BEFORE the handler destroy runs or anything is freed.
	 * This is the systemic cure for the whole "scheduled callback fires
	 * after its owner was freed" crash family (deferred_parser_unpause, box
	 * conversion, object refresh, css modified styles, ...), replacing the
	 * per-callback schedule(-1, fn, ctx) cancels that had to enumerate every
	 * callback and kept missing new ones.  html_content embeds struct
	 * content as its FIRST member, so callbacks scheduled with the
	 * html_content pointer share this address and are cancelled here too. */
	macos9_schedule_cancel_owner(c);

	/* Now safe to assert and log */
	macsurf_debug_log_writef("content_destroy: content=%p", (void *)c);
	assert(c->locked == false);

	if (h->destroy != NULL)
		h->destroy(c);

	llcache_handle_release(c->llcache);
	c->llcache = NULL;

	lwc_string_unref(c->mime_type);

	/* release metadata links */
	link = c->links;
	while (link != NULL) {
		link = content__free_rfc5988_link(link);
	}

	/* free the user list; NULL it so content_broadcast's guard catches
	 * any re-entrant broadcast on this content after destroy. */
	if (c->user_list != NULL) {
		free(c->user_list);
		c->user_list = NULL;
	}

	/* free the title */
	if (c->title != NULL) {
		free(c->title);
	}

	/* free the fallback characterset */
	if (c->fallback_charset != NULL) {
		free(c->fallback_charset);
	}

	free(c);
}


/* exported interface documented in content/content.h */
void
content_mouse_track(hlcache_handle *h,
		    struct browser_window *bw,
		    browser_mouse_state mouse,
		    int x, int y)
{
	struct content *c;
	unsigned long ha;
	/* fixes501: stale-handle guard */
	if (h == NULL) return;
	c = hlcache_handle_get_content(h);
	if (c == NULL) return;
	ha = (unsigned long)(void *)c->handler;
	/* fixes1191 - see the matching comment in content_mouse_action below;
	 * same wrong-function bug (macsurf_ptr_is_heap on a static vtable),
	 * same fix (macsurf_ptr_is_valid). */
	if (!macsurf_ptr_is_valid((const void *)c->handler)) {
		macsurf_debug_log_writef(
			"LIFE fixes1191 mouse: handler invalid ha=%p -- skip",
			(void *)ha);
		return;
	}

	if (c->handler->mouse_track != NULL) {
		c->handler->mouse_track(c, bw, mouse, x, y);
	} else {
		union content_msg_data msg_data;
		msg_data.pointer = BROWSER_POINTER_AUTO;
		content_broadcast(c, CONTENT_MSG_POINTER, &msg_data);
	}


	return;
}


/* exported interface documented in content/content.h */
void
content_mouse_action(hlcache_handle *h,
		     struct browser_window *bw,
		     browser_mouse_state mouse,
		     int x, int y)
{
	struct content *c;
	unsigned long ha;
	/* fixes501: stale-handle guard */
	if (h == NULL) return;
	c = hlcache_handle_get_content(h);
	if (c == NULL) return;
	ha = (unsigned long)(void *)c->handler;
	/* fixes719b: the old hardcoded 0x01000000..0x20000000 ceiling rejected a
	 * VALID content_handler vtable on a high-mapped partition (0x4B..), so
	 * every click/hover bailed -> "can't click on anything" on high-RAM Macs
	 * (fine on the low-mapped dev iMac). c is already validated in-heap with a
	 * non-NULL handler by the fixes719 hlcache_handle_get_content gate, and the
	 * render path calls c->handler fine, so accept any handler inside the REAL
	 * app partition.
	 *
	 * fixes1191 - this was macsurf_ptr_is_heap(), the STRICT malloc'd-object
	 * test (see its doc comment in macsurf_memory.c: "for malloc'd objects...
	 * always come from MSL malloc / NewPtr backed by the app zone"). c->handler
	 * is a STATIC CONST content_handler vtable (e.g. &html_content_handler) -
	 * it is never malloc'd, so it belongs to the OTHER function,
	 * macsurf_ptr_is_valid(), documented for exactly this case: "pointers that
	 * may legitimately live OUTSIDE the partition heap window (e.g. a static
	 * const vtable...)". On OS 9 the mismatch was silently masked
	 * (g_ptr_bounds_ok=1 skips the narrower OS-X-only check entirely). On OS X,
	 * fixes956's observed-allocator-window tightening of macsurf_ptr_is_heap
	 * made it reject a static vtable address whenever that address falls
	 * outside the malloc'd range it happens to have observed so far -
	 * environment/allocation-history dependent, which is why this read as an
	 * intermittent "can't click on anything" bug rather than a clean always-on
	 * one. is_valid safely no-ops (never a wild call) on out-of-partition
	 * garbage; log the address if it ever rejects so we can widen. */
	if (!macsurf_ptr_is_valid((const void *)c->handler)) {
		macsurf_debug_log_writef(
			"LIFE fixes1191 mouse: handler invalid ha=%p -- skip",
			(void *)ha);
		return;
	}

	if (c->handler->mouse_action != NULL)
		c->handler->mouse_action(c, bw, mouse, x, y);

	return;
}


/* exported interface documented in content/content.h */
bool content_keypress(struct hlcache_handle *h, uint32_t key)
{
	struct content *c = hlcache_handle_get_content(h);
	CONTENT_CHECK_RETURN(c, false);

	if (c->handler->keypress != NULL)
		return c->handler->keypress(c, key);

	return false;
}


/* exported interface documented in content/content.h */
void content_request_redraw(struct hlcache_handle *h,
			    int x, int y, int width, int height)
{
	content__request_redraw(hlcache_handle_get_content(h),
				x, y, width, height);
}


/* exported interface, documented in content/protected.h */
void content__request_redraw(struct content *c,
			     int x, int y, int width, int height)
{
	union content_msg_data data;

	CONTENT_CHECK_VOID(c);

	data.redraw.x = x;
	data.redraw.y = y;
	data.redraw.width = width;
	data.redraw.height = height;

	content_broadcast(c, CONTENT_MSG_REDRAW, &data);
}


/* exported interface, documented in content/content.h */
bool content_exec(struct hlcache_handle *h, const char *src, size_t srclen)
{
	struct content *c = hlcache_handle_get_content(h);

	CONTENT_CHECK_RETURN(c, false);

	if (c->locked) {
		/* Not safe to do stuff */
		NSLOG(netsurf, DEEPDEBUG, "Unable to exec, content locked");
		return false;
	}

	if (c->handler->exec == NULL) {
		/* Can't exec something on this content */
		NSLOG(netsurf, DEEPDEBUG, "Unable to exec, no exec function");
		return false;
	}

	return c->handler->exec(c, src, srclen);
}


/* exported interface, documented in content/content.h */
bool content_saw_insecure_objects(struct hlcache_handle *h)
{
	struct content *c = hlcache_handle_get_content(h);
	struct nsurl *url = hlcache_handle_get_url(h);
	lwc_string *scheme = nsurl_get_component(url, NSURL_SCHEME);
	unsigned char match;

	/* Is this an internal scheme? If so, we trust here and stop */
	if ((lwc_string_isequal(scheme, corestring_lwc_about,
				&match) == lwc_error_ok &&
	     (match == true)) ||
	    (lwc_string_isequal(scheme, corestring_lwc_data,
				&match) == lwc_error_ok &&
	     (match == true)) ||
	    (lwc_string_isequal(scheme, corestring_lwc_resource,
				&match) == lwc_error_ok &&
	     (match == true)) ||
	    /* Our internal x-ns-css scheme is secure */
	    (lwc_string_isequal(scheme, corestring_lwc_x_ns_css,
				&match) == lwc_error_ok &&
	     (match == true)) ||
	    /* We also treat file: as "not insecure" here */
	    (lwc_string_isequal(scheme, corestring_lwc_file,
				&match) == lwc_error_ok &&
	     (match == true))) {
		/* No insecurity to find */
		lwc_string_unref(scheme);
		return false;
	}

	/* Okay, not internal, am *I* secure? */
	if ((lwc_string_isequal(scheme, corestring_lwc_https,
				&match) == lwc_error_ok)
	    && (match == false)) {
		/* I did see something insecure -- ME! */
		lwc_string_unref(scheme);
		return true;
	}

	lwc_string_unref(scheme);
	/* I am supposed to be secure, but was I overridden */
	if (urldb_get_cert_permissions(url)) {
		/* I was https:// but I was overridden, that's no good */
		return true;
	}

	/* Otherwise try and chain through the handler */
	if (!CONTENT_IS_DEAD(c) && c->handler->saw_insecure_objects != NULL) {
		return c->handler->saw_insecure_objects(c);
	}

	/* If we can't see insecure objects, we can't see them */
	return false;
}


/* exported interface, documented in content/content.h */
bool
content_redraw(hlcache_handle *h,
	       struct content_redraw_data *data,
	       const struct rect *clip,
	       const struct redraw_context *ctx)
{
	struct content *c = hlcache_handle_get_content(h);

	/* fixes516: a redraw queued before navigation can fire after the old
	 * content is destroyed; the dead sentinel skips it (return true = "no
	 * draw needed") instead of dispatching through a freed handler. */
	CONTENT_CHECK_RETURN(c, true);

	if (c->locked) {
		/* not safe to attempt redraw */
		return true;
	}

	/* ensure we have a redrawable content */
	if (c->handler->redraw == NULL) {
		return true;
	}

	return c->handler->redraw(c, data, clip, ctx);
}


/* exported interface, documented in content/content.h */
bool
content_scaled_redraw(struct hlcache_handle *h,
		      int width, int height,
		      const struct redraw_context *ctx)
{
	struct content *c = hlcache_handle_get_content(h);
	struct redraw_context new_ctx = *ctx;
	struct rect clip;
	struct content_redraw_data data;
	bool plot_ok = true;

	CONTENT_CHECK_RETURN(c, true);

	/* ensure it is safe to attempt redraw */
	if (c->locked) {
		return true;
	}

	/* ensure we have a redrawable content */
	if (c->handler->redraw == NULL) {
		return true;
	}

	nslog_log(__FILE__, "", __LINE__, "Content %p %dx%d ctx:%p", c, width, height, ctx);

	if (ctx->plot->option_knockout) {
		knockout_plot_start(ctx, &new_ctx);
	}

	/* Set clip rectangle to required thumbnail size */
	clip.x0 = 0;
	clip.y0 = 0;
	clip.x1 = width;
	clip.y1 = height;

	new_ctx.plot->clip(&new_ctx, &clip);

	/* Plot white background */
	plot_ok &= (new_ctx.plot->rectangle(&new_ctx,
					    plot_style_fill_white,
					    &clip) == NSERROR_OK);

	/* Set up content redraw data */
	data.x = 0;
	data.y = 0;
	data.width = width;
	data.height = height;

	data.background_colour = 0xFFFFFF;
	data.repeat_x = false;
	data.repeat_y = false;
	data.nearest = false;  /* fixes829 (#256) */
	data.background_blend_mode = 0;

	/* Find the scale factor to use if the content has a width */
	if (c->width) {
		data.scale = (float)width / (float)c->width;
	} else {
		data.scale = 1.0;
	}

	/* Render the content */
	plot_ok &= c->handler->redraw(c, &data, &clip, &new_ctx);

	if (ctx->plot->option_knockout) {
		knockout_plot_end(ctx);
	}

	return plot_ok;
}


/* exported interface documented in content/content.h */
bool
content_add_user(struct content *c,
		 void (*callback)(
				  struct content *c,
				  content_msg msg,
				  const union content_msg_data *data,
				  void *pw),
		 void *pw)
{
	struct content_user *user;

	CONTENT_CHECK_RETURN(c, false);

	nslog_log(__FILE__, "", __LINE__,
		      "content "URL_FMT_SPC" (%p), user %p %p",
		      (c != NULL && c->llcache != NULL && llcache_handle_get_url(c->llcache) != NULL ? nsurl_access_log(llcache_handle_get_url(c->llcache)) : "(null)"),
		      c,
		      callback,
		      pw);
	user = malloc(sizeof(struct content_user));
	if (!user)
		return false;
	user->callback = callback;
	user->pw = pw;
	user->next = c->user_list->next;
	c->user_list->next = user;

	if (c->handler->add_user != NULL)
		c->handler->add_user(c);

	return true;
}


/* exported interface documented in content/content.h */
void
content_remove_user(struct content *c,
		    void (*callback)(
				     struct content *c,
				     content_msg msg,
				     const union content_msg_data *data,
				     void *pw),
		    void *pw)
{
	struct content_user *user, *next;

	/* fixes508: guard against calling content_remove_user on a content
	 * that content_destroy has already run on.  After content_destroy:
	 *   c->handler is NULL (sentinel), c->llcache is NULL.
	 * A stale hlcache_handle that survived cleanup reaches here via
	 * hlcache_handle_release -> content_remove_user.  The nslog_log
	 * call below would dereference c->llcache (NULL) to get the URL,
	 * crashing at content_remove_user+000D8.  Line 766 would then
	 * dereference c->handler (NULL) for remove_user vtable dispatch.
	 * Both are fatal on a destroyed content struct. */
	if (c->handler == NULL) {
		macsurf_debug_log_writef(
			"content_remove_user: SKIP destroyed content=%p cb=%p",
			(void *)c, (void *)callback);
		return;
	}

	nslog_log(__FILE__, "", __LINE__,
		      "content "URL_FMT_SPC" (%p), user %p %p",
		      (c != NULL && c->llcache != NULL && llcache_handle_get_url(c->llcache) != NULL ? nsurl_access_log(llcache_handle_get_url(c->llcache)) : "(null)"),
		      c,
		      callback,
		      pw);

	/* user_list starts with a sentinel */
	for (user = c->user_list; user->next != 0 &&
		     !(user->next->callback == callback &&
		       user->next->pw == pw); user = user->next)
		;
	if (user->next == 0) {
		NSLOG(netsurf, INFO, "user not found in list");
		assert(0);
		return;
	}

	if (c->handler->remove_user != NULL)
		c->handler->remove_user(c);

	next = user->next;
	user->next = next->next;   /* unlink synchronously so the list is consistent */
#ifdef __MACOS9__
	/* fixes600 - defer the free (see content_user_deathrow_teardown). Unlinked
	 * above, so it is out of the list; the death-row frees it once no walk is on
	 * the stack. dr_queued gates against a double-enqueue. */
	if (!next->dr_queued) {
		next->dr_queued = 1;
		macos9_deathrow_add(next, content_user_deathrow_teardown, c);
	}
#else
	free(next);
#endif
}


/* exported interface documented in content/content.h */
uint32_t content_count_users(struct content *c)
{
	struct content_user *user;
	uint32_t counter = 0;

	CONTENT_CHECK_RETURN(c, 0);

	for (user = c->user_list; user != NULL; user = user->next)
		counter += 1;

	assert(counter > 0);

	return counter - 1; /* Subtract 1 for the sentinel */
}


/* exported interface documented in content/content.h */
bool content_matches_quirks(struct content *c, bool quirks)
{
	CONTENT_CHECK_RETURN(c, false);

	if (c->handler->matches_quirks == NULL)
		return true;

	return c->handler->matches_quirks(c, quirks);
}


/* exported interface documented in content/content.h */
bool content_is_shareable(struct content *c)
{
	CONTENT_CHECK_RETURN(c, false);

	return c->handler->no_share == false;
}


/* exported interface documented in content/protected.h */
/*
 * fixes954 - is this message safe to REPLAY with no data?
 *
 * The deferred-broadcast queue (see content_broadcast below) stores only the
 * message TYPE. hlcache_content_callback zero-fills the event and copies data
 * only when a pointer is supplied, so a replayed message arrives with an
 * all-zero union rather than a wild one. That is safe ONLY for handlers which
 * never look inside it -- any handler that pulls a POINTER out of the zeroed
 * union and dereferences it gets NULL.
 *
 * Audited against every case in browser_window_callback: of 25 message types,
 * exactly THREE never touch event->data. Every other case reads
 * event->data.<something>, and several of those are pointers used
 * immediately -- data.chain (cert_chain_dup), data.log.msg, data.redirect.from
 * (urldb_add_url), data.errordata.errormsg.
 *
 * NOTE the comment that used to sit in content_broadcast claimed
 * "READY/DONE/STATUS/REDRAW" tolerate NULL data. That was WRONG about STATUS
 * and REDRAW -- both dereference the union, and both are among the most
 * frequently broadcast messages in the engine. Queueing them was the crash:
 *   browser_window_callback <- hlcache_content_callback <- content_broadcast
 *     <- content_broadcast <- hlcache_broadcast_catchup <- macos9_schedule_run
 * reproducible on Mac OS X, NULL dereference, ~10 s into a page load.
 */
static int content_broadcast_replay_safe(content_msg msg)
{
	return msg == CONTENT_MSG_LOADING ||
	       msg == CONTENT_MSG_READY ||
	       msg == CONTENT_MSG_DONE;
}

void content_broadcast(struct content *c, content_msg msg,
		       const union content_msg_data *data)
{
	struct content_user *user, *next;
	content_msg pending_msg;

	assert(c);

	/* Destroyed sentinel.  content_destroy sets c->handler = NULL before
	 * freeing any fields.  A reentrant call that arrives after destroy
	 * (e.g. hlcache_content_callback -> content_set_done -> here) would
	 * walk the already-freed user_list; the sentinel stops it before any
	 * field is touched. */
	if (c == NULL || c->handler == NULL) {
		macsurf_debug_log_writef(
			"content_broadcast: SKIP dead/destroyed c=%p msg=%d",
			(void *)c, (int)msg);
		return;
	}

	/* Reentrancy guard - global + per-content (mirrors fixes459/460).
	 * A user callback invoked during ANY broadcast walk can call
	 * content_set_ready / content_set_done - on the same content OR a
	 * different one - which calls content_broadcast again.  Nested walks
	 * find partially-invalidated state and crash (NULL GrafPort write,
	 * __ptr_glue through a freed callback TVector, etc., all confirmed in
	 * MacsBug stacks with content_broadcast appearing 2-3 times).
	 *
	 * Defer instead of recursing: store the message on the content and
	 * ask hlcache to pump pending broadcasts on the next event-loop tick.
	 * The content's status field is already updated by the caller before
	 * it broadcasts, so only the NOTIFICATION is delayed, never the state
	 * transition.  Data pointers are not stored (often stack-allocated and
	 * invalid by replay time), so replay always passes NULL data -- and
	 * fixes954 restricts the queue to the messages that actually survive
	 * that.  See content_broadcast_replay_safe(); the previous claim here
	 * that STATUS and REDRAW tolerate it was wrong and was the crash.
	 *
	 * Pending is last-wins for terminal messages: DONE/ERROR overwrite a
	 * stored READY/STATUS so a deferred load still reaches completion, but
	 * a stored terminal message is never downgraded by a later cosmetic
	 * one. */
	if (content_broadcast_active || c->broadcast_in_progress) {
		macsurf_debug_log_writef(
			"content_broadcast: REENTRANT BLOCKED msg=%d c=%p (global=%d self=%d)",
			(int)msg, (void *)c,
			(int)content_broadcast_active, (int)c->broadcast_in_progress);
		/* Append to the FIFO, preserving order.  Dedup only against the
		 * immediately-previous queued message so repeated REDRAW/STATUS
		 * don't flood the queue, while READY-then-DONE is kept intact. */
		if (!content_broadcast_replay_safe(msg)) {
			/* fixes954 - cannot be replayed without its data, so it
			 * is dropped rather than queued.  Dropping a NOTIFICATION
			 * is survivable: the caller already applied the state
			 * change before broadcasting, and a dropped STATUS/REDRAW
			 * costs at most a stale status line or a repaint that the
			 * next paint cycle makes good.  Replaying it with a zeroed
			 * union is NOT survivable -- the handler dereferences a
			 * NULL out of it.
			 * KNOWN COST: a dropped CONTENT_MSG_ERROR means a failed
			 * load is not reported, so the throbber can spin on.  That
			 * is a bug, but a far smaller one than the crash it
			 * replaces, and it only arises for an error that lands
			 * during a reentrant broadcast.  Storing the data for the
			 * messages that need it is the real fix and is a larger
			 * change. */
			macsurf_debug_log_writef(
				"content_broadcast: msg=%d not replay-safe, dropped c=%p",
				(int)msg, (void *)c);
		} else if (c->broadcast_pending_count >= CONTENT_BROADCAST_PENDING_MAX) {
			macsurf_debug_log_writef(
				"content_broadcast: pending FIFO full, drop msg=%d c=%p",
				(int)msg, (void *)c);
		} else if (c->broadcast_pending_count > 0 &&
			   c->broadcast_pending[c->broadcast_pending_count - 1] == msg) {
			/* consecutive duplicate; ignore */
		} else {
			c->broadcast_pending[c->broadcast_pending_count++] = msg;
		}
		hlcache_request_broadcast_catchup();
		return;
	}
	content_broadcast_active = true;
	c->broadcast_in_progress = true;

	/* fixes97: per-broadcast MS_LOG dropped. The READY transition is
	 * useful but fires once per content load - we log content_broadcast
	 * READY only and let the rest be silent.
	 *
	 * fixes161c - expand READY broadcast logging into entry / per-user /
	 * exit so we can localize a crash that lands somewhere inside the
	 * notify loop. Apple's post-READY crash on 2026-05-21 truncated
	 * the log right at "content broadcast READY" with no following
	 * gw_event entry - telling us the loop never finished. Tagging the
	 * loop bounds and each user callback gives the next log a stage
	 * marker. The body of the loop is unchanged; only logging added. */
	if (msg == CONTENT_MSG_READY) {
		struct content_user *probe_u;
		long n_users = 0;
		for (probe_u = c->user_list->next; probe_u != 0;
				probe_u = probe_u->next) {
			n_users++;
		}
		macsurf_debug_log_writef("broadcast_ready: content=%p users=%ld", (void*)c, n_users);

		/* fixes449: zombie guard.  users=0 at READY means all hlcache
		 * handles that referenced this content were released before it
		 * finished converting - the browser window navigated away while
		 * the fetch was still in flight.  Firing the callback loop into
		 * a NULL user list is harmless (the loop is a no-op), but the
		 * content object itself is now unreachable and will linger as a
		 * zombie.  Log its identity so we can correlate it with later
		 * stale-pointer crashes: the content pointer and status are the
		 * safest fields to read on a potentially-orphaned object. */
		if (n_users == 0) {
			macsurf_debug_log_writef(
				"content broadcast READY: ZOMBIE c=%p status=%d",
				(void *)c, (c != NULL ? (int)c->status : -1));
			/* No return: let the (empty) loop run so READY exit fires. */
		}
	}

	/* fixes511: guard c->user_list against NULL (freed by content_destroy)
	 * and each user/next pointer against heap-reuse garbage.
	 * content_destroy frees c->user_list but does not null it.  If a
	 * re-entrant path (hlcache_content_callback -> content_set_done ->
	 * content_broadcast) arrives after the first destroy, user_list is
	 * dangling.  Also guard each user and next: a callback may call
	 * content_remove_user on a sibling entry, freeing it; if the
	 * allocator immediately reuses that memory (e.g. for a JS source
	 * buffer), next->next reads garbage.  Symptoms: r4=NULL or r4/r5 =
	 * CBB3A1B4/CBB3A1B5 heap-reuse bytes at lbzu in content_broadcast. */
	if (c->user_list == NULL) {
		macsurf_debug_log_writef(
			"content_broadcast: NULL user_list c=%p msg=%d",
			(void *)c, (int)msg);
		c->broadcast_in_progress = false;
		content_broadcast_active = false;
		return;
	}
	nslog_log(__FILE__, "", __LINE__, "%p -> msg:%d", c, msg);
	/* Stage 1 drain-gate: a user callback can navigate away and tear down
	 * content mid-walk; hold macos9_op_depth>0 so the death-row drain
	 * cannot fire while this broadcast loop is on the stack and free an
	 * object the loop still iterates. This is NOT the same-frame reentrant
	 * broadcast guard (that is broadcast_in_progress / Stage 3) - its only
	 * job here is to keep the drain out until the walk unwinds. */
	macos9_op_depth++;
	for (user = c->user_list->next; user != 0; user = next) {
		unsigned long ua = (unsigned long)(void *)user;
		if (!macsurf_ptr_is_heap((const void *)(user))) {
			macsurf_debug_log_writef(
				"content_broadcast: wild user=%p c=%p msg=%d, stopping",
				(void *)user, (void *)c, (int)msg);
			break;
		}
		next = user->next;
		/* validate next before the loop uses it */
		if (next != NULL) {
			unsigned long na = (unsigned long)(void *)next;
			if (!macsurf_ptr_is_heap((const void *)(next))) {
				macsurf_debug_log_writef(
					"content_broadcast: wild next=%p c=%p msg=%d, stopping after this user",
					(void *)next, (void *)c, (int)msg);
				next = NULL; /* stop after this iteration */
			}
		}
		if (user->callback != NULL) {
			user->callback(c, msg, data, user->pw);
		}
	}
	/* Stage 1 death-row (fixes565-577 arc): the walk is now off the stack,
	 * so release the drain gate before the inline replay below.  The replay
	 * issues a recursive content_broadcast, which brackets its own
	 * op_depth++/-- around its walk, so the gate is correctly re-entered
	 * there if there is more work. */
	macos9_op_depth--;

	/* Clear both reentrancy flags.  The global flag MUST be cleared before
	 * the inline replay below, otherwise the replay would see the global
	 * flag still set and defer itself forever. */
	c->broadcast_in_progress = false;
	content_broadcast_active = false;

	/* Replay this content's deferred queue, in FIFO order, with NULL data
	 * (fast path; saves a scheduler round-trip).  Pop the front message
	 * and re-issue it; the recursive call's own exit-drain handles the
	 * remainder, so order is preserved.  Re-check handler in case a
	 * callback destroyed this content during the walk (handler == NULL,
	 * content freed) - then discard the queue.  Deferred messages for
	 * OTHER contents are replayed by hlcache's catch-up pump next tick. */
	if (c->handler == NULL) {
		if (c->broadcast_pending_count != 0) {
			macsurf_debug_log_writef(
				"content_broadcast: discard %d pending, c=%p destroyed during walk",
				c->broadcast_pending_count, (void *)c);
			c->broadcast_pending_count = 0;
		}
	} else if (c->broadcast_pending_count > 0) {
		int k;
		pending_msg = c->broadcast_pending[0];
		for (k = 1; k < c->broadcast_pending_count; k++) {
			c->broadcast_pending[k - 1] = c->broadcast_pending[k];
		}
		c->broadcast_pending_count--;
		content_broadcast(c, pending_msg, NULL);
	}
}


/* exported interface documented in content_protected.h */
void
content_broadcast_error(struct content *c, nserror errorcode, const char *msg)
{
	struct content_user *user, *next;
	union content_msg_data data;

	CONTENT_CHECK_VOID(c);

	data.errordata.errorcode = errorcode;
	data.errordata.errormsg = msg;

	if (c->user_list == NULL) return;
	for (user = c->user_list->next; user != 0; user = next) {
		unsigned long ua = (unsigned long)(void *)user;
		if (!macsurf_ptr_is_heap((const void *)(user))) break;
		next = user->next;
		if (next != NULL) {
			unsigned long na = (unsigned long)(void *)next;
			if (!macsurf_ptr_is_heap((const void *)(next))) next = NULL;
		}
		if (user->callback != 0) {
			user->callback(c, CONTENT_MSG_ERROR, &data, user->pw);
		}
	}
}


/* exported interface, documented in content/content.h */
nserror
content_open(hlcache_handle *h,
	     struct browser_window *bw,
	     struct content *page,
	     struct object_params *params)
{
	struct content *c;
	nserror res;

	c = hlcache_handle_get_content(h);
	CONTENT_CHECK_RETURN(c, NSERROR_OK);
	nslog_log(__FILE__, "", __LINE__,
		      "content %p %s",
		      c,
		      (c != NULL && c->llcache != NULL && llcache_handle_get_url(c->llcache) != NULL ? nsurl_access_log(llcache_handle_get_url(c->llcache)) : "(null)"));
	if (c->handler->open != NULL) {
		res = c->handler->open(c, bw, page, params);
	} else {
		res = NSERROR_OK;
	}
	return res;
}


/* exported interface, documented in content/content.h */
nserror content_close(hlcache_handle *h)
{
	struct content *c;
	nserror res;

	c = hlcache_handle_get_content(h);
	if (c == NULL) {
		return NSERROR_BAD_PARAMETER;
	}
	if (c->handler == NULL) {
		macsurf_debug_log_writef(
			"content_close: DEAD content=%p, skip", (void *)c);
		return NSERROR_OK;
	}

	if ((c->status != CONTENT_STATUS_READY) &&
	    (c->status != CONTENT_STATUS_DONE)) {
		/* status is not read or done so nothing to do */
		return NSERROR_INVALID;
	}

	nslog_log(__FILE__, "", __LINE__,
		      "content %p %s",
		      c,
		      (c != NULL && c->llcache != NULL && llcache_handle_get_url(c->llcache) != NULL ? nsurl_access_log(llcache_handle_get_url(c->llcache)) : "(null)"));

	if (c->textsearch.context != NULL) {
		content_textsearch_destroy(c->textsearch.context);
		c->textsearch.context = NULL;
	}

	if (c->handler->close != NULL) {
		res = c->handler->close(c);
	} else {
		res = NSERROR_OK;
	}
	return res;
}


/* exported interface, documented in content/content.h */
void content_clear_selection(hlcache_handle *h)
{
	struct content *c = hlcache_handle_get_content(h);
	CONTENT_CHECK_VOID(c);

	/* fixes516: dispatch clear_selection only if the vtable slot for it
	 * (not get_selection) is populated - a handler may implement one but
	 * not the other, and calling a NULL fn-ptr is its own crash class. */
	if (c->handler->clear_selection != NULL)
		c->handler->clear_selection(c);
}


/* exported interface, documented in content/content.h */
char * content_get_selection(hlcache_handle *h)
{
	struct content *c = hlcache_handle_get_content(h);
	CONTENT_CHECK_RETURN(c, NULL);

	if (c->handler->get_selection != NULL)
		return c->handler->get_selection(c);
	else
		return NULL;
}


/* exported interface documented in content/content.h */
nserror
content_get_contextual_content(struct hlcache_handle *h,
			       int x, int y,
			       struct browser_window_features *data)
{
	struct content *c = hlcache_handle_get_content(h);
	CONTENT_CHECK_RETURN(c, NSERROR_BAD_PARAMETER);

	if (c->handler->get_contextual_content != NULL) {
		return c->handler->get_contextual_content(c, x, y, data);
	}

	data->object = h;
	return NSERROR_OK;
}


/* exported interface, documented in content/content.h */
bool
content_scroll_at_point(struct hlcache_handle *h,
			int x, int y,
			int scrx, int scry)
{
	struct content *c = hlcache_handle_get_content(h);
	CONTENT_CHECK_RETURN(c, false);

	if (c->handler->scroll_at_point != NULL)
		return c->handler->scroll_at_point(c, x, y, scrx, scry);

	return false;
}


/* exported interface, documented in content/content.h */
bool
content_drop_file_at_point(struct hlcache_handle *h,
			   int x, int y,
			   char *file)
{
	struct content *c = hlcache_handle_get_content(h);
	CONTENT_CHECK_RETURN(c, false);

	if (c->handler->drop_file_at_point != NULL)
		return c->handler->drop_file_at_point(c, x, y, file);

	return false;
}


/* exported interface documented in content/content.h */
nserror
content_debug_dump(struct hlcache_handle *h, FILE *f, enum content_debug op)
{
	struct content *c = hlcache_handle_get_content(h);
	CONTENT_CHECK_RETURN(c, NSERROR_BAD_PARAMETER);

	if (c->handler->debug_dump == NULL) {
		return NSERROR_NOT_IMPLEMENTED;
	}

	return c->handler->debug_dump(c, f, op);
}


/* exported interface documented in content/content.h */
nserror content_debug(struct hlcache_handle *h, enum content_debug op)
{
	struct content *c = hlcache_handle_get_content(h);

	CONTENT_CHECK_RETURN(c, NSERROR_BAD_PARAMETER);

	if (c->handler->debug == NULL) {
		return NSERROR_NOT_IMPLEMENTED;
	}

	return c->handler->debug(c, op);
}


/* exported interface documented in content/content.h */
struct content_rfc5988_link *
content_find_rfc5988_link(hlcache_handle *h, lwc_string *rel)
{
	struct content *c = hlcache_handle_get_content(h);
	struct content_rfc5988_link *link;
	unsigned char rel_match = 0;

	/* fixes516: guard before touching c->links (freed at destroy). */
	CONTENT_CHECK_RETURN(c, NULL);
	link = c->links;

	while (link != NULL) {
		if (lwc_string_caseless_isequal(link->rel, rel,
						&rel_match) == lwc_error_ok && rel_match) {
			break;
		}
		link = link->next;
	}
	return link;
}


/* exported interface documented in content/protected.h */
struct content_rfc5988_link *
content__free_rfc5988_link(struct content_rfc5988_link *link)
{
	struct content_rfc5988_link *next;

	next = link->next;

	nsurl_unref(link->href);
	lwc_string_unref(link->rel);
	lwc_string_unref(link->hreflang);
	lwc_string_unref(link->type);
	lwc_string_unref(link->media);
	lwc_string_unref(link->sizes);
	free(link);

	return next;
}


/* exported interface documented in content/protected.h */
bool
content__add_rfc5988_link(struct content *c,
			  const struct content_rfc5988_link *link)
{
	struct content_rfc5988_link *newlink;
	union content_msg_data msg_data;

	CONTENT_CHECK_RETURN(c, false);

	/* a link relation must be present for it to be a link */
	if (link->rel == NULL) {
		return false;
	}

	/* a link href must be present for it to be a link */
	if (link->href == NULL) {
		return false;
	}

	newlink = calloc(1, sizeof(struct content_rfc5988_link));
	if (newlink == NULL) {
		return false;
	}

	/* copy values */
	newlink->rel = lwc_string_ref(link->rel);
	newlink->href = nsurl_ref(link->href);
	if (link->hreflang != NULL) {
		newlink->hreflang = lwc_string_ref(link->hreflang);
	}
	if (link->type != NULL) {
		newlink->type = lwc_string_ref(link->type);
	}
	if (link->media != NULL) {
		newlink->media = lwc_string_ref(link->media);
	}
	if (link->sizes != NULL) {
		newlink->sizes = lwc_string_ref(link->sizes);
	}

	/* add to metadata link to list */
	newlink->next = c->links;
	c->links = newlink;

	/* broadcast the data */
	msg_data.rfc5988_link = newlink;
	content_broadcast(c, CONTENT_MSG_LINK, &msg_data);

	return true;
}


/* exported interface documented in content/content.h */
nsurl *content_get_url(struct content *c)
{
	CONTENT_CHECK_RETURN(c, NULL);

	/* fixes1025 - a content that is not (or no longer) backed by the
	 * low-level cache has no URL, and llcache_handle_get_url dereferences
	 * its argument without checking. Every caller here already handles a
	 * NULL nsurl; a crash inside the accessor is never the better answer.
	 * Same guard macos9_reconvert_host_allowed and the fixes846 JS path
	 * already apply for the same reason. */
	if (c->llcache == NULL)
		return NULL;

	return llcache_handle_get_url(c->llcache);
}


/* exported interface documented in content/content.h */
content_type content_get_type(hlcache_handle *h)
{
	struct content *c = hlcache_handle_get_content(h);

	CONTENT_CHECK_RETURN(c, CONTENT_NONE);

	return c->handler->type();
}


/* exported interface documented in content/content.h */
lwc_string *content_get_mime_type(hlcache_handle *h)
{
	return content__get_mime_type(hlcache_handle_get_content(h));
}


/* exported interface documented in content/content_protected.h */
lwc_string *content__get_mime_type(struct content *c)
{
	CONTENT_CHECK_RETURN(c, NULL);

	return lwc_string_ref(c->mime_type);
}


/* exported interface documented in content/content_protected.h */
bool content__set_title(struct content *c, const char *title)
{
	char *new_title = strdup(title);
	if (new_title == NULL)
		return false;

	if (c->title != NULL)
		free(c->title);

	c->title = new_title;

	return true;
}


/* exported interface documented in content/content.h */
const char *content_get_title(hlcache_handle *h)
{
	return content__get_title(hlcache_handle_get_content(h));
}


/* exported interface documented in content/content_protected.h */
const char *content__get_title(struct content *c)
{
	CONTENT_CHECK_RETURN(c, NULL);

	return c->title != NULL ? c->title :
		nsurl_access(llcache_handle_get_url(c->llcache));
}


/* exported interface documented in content/content.h */
content_status content_get_status(hlcache_handle *h)
{
	return content__get_status(hlcache_handle_get_content(h));
}


/* exported interface documented in content/content_protected.h */
content_status content__get_status(struct content *c)
{
	if (c == NULL)
		return CONTENT_STATUS_ERROR;

	return c->status;
}


/* exported interface documented in content/content.h */
const char *content_get_status_message(hlcache_handle *h)
{
	return content__get_status_message(hlcache_handle_get_content(h));
}


/* exported interface documented in content/content_protected.h */
const char *content__get_status_message(struct content *c)
{
	CONTENT_CHECK_RETURN(c, NULL);

	return c->status_message;
}


/* exported interface documented in content/content.h */
int content_get_width(hlcache_handle *h)
{
	return content__get_width(hlcache_handle_get_content(h));
}


/* exported interface documented in content/content_protected.h */
int content__get_width(struct content *c)
{
	if (c == NULL)
		return 0;

	return c->width;
}


/* exported interface documented in content/content.h */
int content_get_height(hlcache_handle *h)
{
	return content__get_height(hlcache_handle_get_content(h));
}


/* exported interface documented in content/content_protected.h */
int content__get_height(struct content *c)
{
	if (c == NULL)
		return 0;

	return c->height;
}


/* exported interface documented in content/content.h */
int content_get_available_width(hlcache_handle *h)
{
	return content__get_available_width(hlcache_handle_get_content(h));
}


/* exported interface documented in content/content_protected.h */
int content__get_available_width(struct content *c)
{
	if (c == NULL)
		return 0;

	return c->available_width;
}


/* exported interface documented in content/content.h */
const uint8_t *content_get_source_data(hlcache_handle *h, size_t *size)
{
	return content__get_source_data(hlcache_handle_get_content(h), size);
}


/* exported interface documented in content/content_protected.h */
const uint8_t *content__get_source_data(struct content *c, size_t *size)
{
	assert(size != NULL);

	/** \todo check if the content check should be an assert */
	CONTENT_CHECK_RETURN(c, NULL);

	return llcache_handle_get_source_data(c->llcache, size);
}


/* exported interface documented in content/content.h */
void content_invalidate_reuse_data(hlcache_handle *h)
{
	content__invalidate_reuse_data(hlcache_handle_get_content(h));
}


/* exported interface documented in content/content_protected.h */
void content__invalidate_reuse_data(struct content *c)
{
	if (c == NULL || c->llcache == NULL)
		return;

	/* Invalidate low-level cache data */
	llcache_handle_invalidate_cache_data(c->llcache);
}


/* exported interface documented in content/content.h */
nsurl *content_get_refresh_url(hlcache_handle *h)
{
	return content__get_refresh_url(hlcache_handle_get_content(h));
}


/* exported interface documented in content/content_protected.h */
nsurl *content__get_refresh_url(struct content *c)
{
	if (c == NULL)
		return NULL;

	return c->refresh;
}


/* exported interface documented in content/content.h */
struct bitmap *content_get_bitmap(hlcache_handle *h)
{
	return content__get_bitmap(hlcache_handle_get_content(h));
}


/* exported interface documented in content/content_protected.h */
struct bitmap *content__get_bitmap(struct content *c)
{
	struct bitmap *bitmap = NULL;

	if ((c != NULL) &&
	    (c->handler != NULL) &&
	    (c->handler->type != NULL) &&
	    (c->handler->type() == CONTENT_IMAGE) &&
	    (c->handler->get_internal != NULL) ) {
		bitmap = c->handler->get_internal(c, NULL);
	}

	return bitmap;
}


/* exported interface documented in content/content.h */
bool content_get_opaque(hlcache_handle *h)
{
	return content__get_opaque(hlcache_handle_get_content(h));
}


/* exported interface documented in content/content_protected.h */
bool content__get_opaque(struct content *c)
{
	if ((c != NULL) &&
	    (c->handler != NULL) &&
	    (c->handler->is_opaque != NULL)) {
		return c->handler->is_opaque(c);
	}

	return false;
}


/* exported interface documented in content/content.h */
bool content_get_quirks(hlcache_handle *h)
{
	struct content *c = hlcache_handle_get_content(h);

	if (c == NULL)
		return false;

	return c->quirks;
}


/* exported interface documented in content/content.h */
const char *
content_get_encoding(hlcache_handle *h, enum content_encoding_type op)
{
	return content__get_encoding(hlcache_handle_get_content(h), op);
}


/* exported interface documented in content/content_protected.h */
const char *
content__get_encoding(struct content *c, enum content_encoding_type op)
{
	const char *encoding = NULL;

	if ((c != NULL) &&
	    (c->handler != NULL) &&
	    (c->handler->get_encoding != NULL) ) {
		encoding = c->handler->get_encoding(c, op);
	}

	return encoding;
}


/* exported interface documented in content/content.h */
bool content_is_locked(hlcache_handle *h)
{
	return content__is_locked(hlcache_handle_get_content(h));
}


/* exported interface documented in content/content_protected.h */
bool content__is_locked(struct content *c)
{
	/* fixes516: dead/NULL content -> report locked so callers that gate
	 * work behind "is this content busy?" skip it rather than operate on
	 * a torn-down struct. */
	CONTENT_CHECK_RETURN(c, true);

	return c->locked;
}


/* exported interface documented in content/content.h */
const llcache_handle *content_get_llcache_handle(struct content *c)
{
	if (c == NULL)
		return NULL;

	return c->llcache;
}


/* exported interface documented in content/protected.h */
struct content *content_clone(struct content *c)
{
	struct content *nc;
	nserror error;

	CONTENT_CHECK_RETURN(c, NULL);

	error = c->handler->clone(c, &nc);
	if (error != NSERROR_OK)
		return NULL;

	return nc;
};


/* exported interface documented in content/protected.h */
nserror content__clone(const struct content *c, struct content *nc)
{
	nserror error;

	error = llcache_handle_clone(c->llcache, &(nc->llcache));
	if (error != NSERROR_OK) {
		return error;
	}

	llcache_handle_change_callback(nc->llcache,
				       content_llcache_callback, nc);

	nc->mime_type = lwc_string_ref(c->mime_type);
	nc->handler = c->handler;

	nc->status = c->status;

	nc->width = c->width;
	nc->height = c->height;
	nc->available_width = c->available_width;
	nc->quirks = c->quirks;

	if (c->fallback_charset != NULL) {
		nc->fallback_charset = strdup(c->fallback_charset);
		if (nc->fallback_charset == NULL) {
			return NSERROR_NOMEM;
		}
	}

	if (c->refresh != NULL) {
		nc->refresh = nsurl_ref(c->refresh);
		if (nc->refresh == NULL) {
			return NSERROR_NOMEM;
		}
	}

	nc->time = c->time;
	nc->reformat_time = c->reformat_time;
	nc->size = c->size;

	if (c->title != NULL) {
		nc->title = strdup(c->title);
		if (nc->title == NULL) {
			return NSERROR_NOMEM;
		}
	}

	nc->active = c->active;

	nc->user_list = calloc(1, sizeof(struct content_user));
	if (nc->user_list == NULL) {
		return NSERROR_NOMEM;
	}

	memcpy(&(nc->status_message), &(c->status_message), 120);
	memcpy(&(nc->sub_status), &(c->sub_status), 80);

	nc->locked = c->locked;
	nc->broadcast_in_progress = false;
	nc->broadcast_pending_count = 0;
	nc->total_size = c->total_size;
	nc->http_code = c->http_code;

	return NSERROR_OK;
}


/* exported interface documented in content/content.h */
nserror content_abort(struct content *c)
{
	CONTENT_CHECK_RETURN(c, NSERROR_OK);

	nslog_log(__FILE__, "", __LINE__, "Aborting %p", c);

	if (c->handler->stop != NULL)
		c->handler->stop(c);

	/* And for now, abort our llcache object */
	return llcache_handle_abort(c->llcache);
}
