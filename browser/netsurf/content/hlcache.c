/*
 * Copyright 2009 John-Mark Bell <jmb@netsurf-browser.org>
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
 * High-level resource cache implementation.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "utils/http.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/ring.h"
#include "utils/utils.h"
#include "netsurf/inttypes.h"
#include "netsurf/misc.h"
#include "netsurf/content.h"
#include "desktop/gui_internal.h"

#include "content/mimesniff.h"
#include "content/hlcache.h"
// Note, this is *ONLY* so that we can abort cleanly during shutdown of the cache
#include "content/content_protected.h"
#include "content/content_factory.h"

#include "macsurf_debug.h"
#include "macos9_deathrow.h"

#ifdef __MACOS9__
/* fixes553 — defined in content/handlers/html/box_construct.c.  Returns non-zero
 * when the content is pinned by the box walk currently on the stack (the walked
 * html_content or any sub-resource content in its object_list).  hlcache_clean
 * consults this to SKIP evicting a content the live walk still dereferences,
 * which is the source-side cure for the convert_xml_to_box free-during-walk UAF
 * (box_image -> box_image_resolve_url -> html_fetch_object ->
 * hlcache_handle_retrieve byte-scan on a wild pointer). */
extern int macos9_box_walk_owns_content(struct content *c);
/* fixes559 — defined in frontends/macos9/macos9_content_registry.c.  Returns
 * non-zero while the content is registered (alive): registered at
 * content__init, deregistered as the FIRST action of content_destroy.  The
 * deferred-broadcast catch-up pump (hlcache_broadcast_catchup) re-derives its
 * targets from the live content_list each tick rather than from a baked-in
 * pointer, and hlcache_clean nulls entry->content + unlinks the entry before
 * freeing (fixes502), so a destroyed content is normally never visited.  This
 * check is the belt-and-suspenders against any future free-path that frees a
 * list-resident content without unlinking: it validates registry membership
 * (which does NOT read the content's reusable bytes) instead of trusting the
 * weak c->handler == NULL sentinel (which does).  Same mechanism as the
 * parser-unpause guard — not a parallel one. */
extern int macos9_content_is_live(struct content *c);
#else
#define macos9_content_is_live(c) (1)
#endif

typedef struct hlcache_entry hlcache_entry;
typedef struct hlcache_retrieval_ctx hlcache_retrieval_ctx;

/** High-level cache retrieval context */
struct hlcache_retrieval_ctx {
	struct hlcache_retrieval_ctx *r_prev; /**< Previous retrieval context in the ring */
	struct hlcache_retrieval_ctx *r_next; /**< Next retrieval context in the ring */

	llcache_handle *llcache;	/**< Low-level cache handle */

	hlcache_handle *handle;		/**< High-level handle for object */

	uint32_t flags;			/**< Retrieval flags */

	content_type accepted_types;	/**< Accepted types */

	hlcache_child_context child;	/**< Child context */

	bool migrate_target;		/**< Whether this context is the migration target */
	int dr_queued;			/**< fixes600: on macos9 death-row */
};

/** High-level cache handle */
struct hlcache_handle {
	hlcache_entry *entry;		/**< Pointer to cache entry */

	hlcache_handle_callback cb;	/**< Client callback */
	void *pw;			/**< Client data */

	bool dr_queued;			/**< Stage 1: on the death row */
};

/** Entry in high-level cache */
struct hlcache_entry {
	struct content *content;	/**< Pointer to associated content */

	hlcache_entry *next;		/**< Next sibling */
	hlcache_entry *prev;		/**< Previous sibling */

	bool dr_queued;			/**< Stage 1: on the death row */
};

/** Current state of the cache.
 *
 * Global state of the cache.
 */
struct hlcache_s {
	struct hlcache_parameters params;

	/** List of cached content objects */
	hlcache_entry *content_list;

	/** Ring of retrieval contexts */
	hlcache_retrieval_ctx *retrieval_ctx_ring;

	/* statistics */
	unsigned int hit_count;
	unsigned int miss_count;
};

/** high level cache state */
static struct hlcache_s *hlcache = NULL;

/** fixes515: set while a broadcast catch-up pump is scheduled but not yet
 * run, so repeated requests coalesce into a single pass. */
static bool hlcache_broadcast_catchup_scheduled = false;
static void hlcache_broadcast_catchup(void *unused);


/******************************************************************************
 * High-level cache internals						      *
 ******************************************************************************/


/* Stage 1 death-row: an hlcache_entry / hlcache_handle freed from the
 * scheduler-driven teardown path defers its free() to the quiescent drain.
 * Idempotent via dr_queued (double-free impossible, ABA-safe). The node is
 * gated on its content's pending scheduled continuations (pin_key) so a
 * still-in-flight box walk can never have the node yanked from under it.
 * Teardown is a bare free() — the node owns no further resources. */
static void
hlcache_node_deathrow_teardown(void *p)
{
	free(p);
}

static void
hlcache_entry_deferred_free(hlcache_entry *entry)
{
	if (entry == NULL || entry->dr_queued) {
		return;
	}
	entry->dr_queued = 1;
	macos9_deathrow_add(entry, hlcache_node_deathrow_teardown,
			entry->content);
}

static void
hlcache_handle_deferred_free(hlcache_handle *handle)
{
	if (handle == NULL || handle->dr_queued) {
		return;
	}
	handle->dr_queued = 1;
	macos9_deathrow_add(handle, hlcache_node_deathrow_teardown,
			(handle->entry != NULL) ? handle->entry->content : NULL);
}

/* fixes600 — a nascent retrieval context is freed synchronously from the
 * retrieval-abort path when a sibling handle is released mid-completion-burst
 * (RING_REMOVE then free, inside a callback). Under tinkerdifferent's ~50
 * concurrent sub-resources that small free lands on the general free-list while
 * walks are on the stack, in the aliasing size class. Defer it and its charset
 * to the quiescent drain, matching the entry/handle nodes above. */
static void
hlcache_rctx_deathrow_teardown(void *p)
{
	hlcache_retrieval_ctx *ctx = (hlcache_retrieval_ctx *) p;
	free((char *) ctx->child.charset);
	free(ctx);
}

static void
hlcache_rctx_deferred_free(hlcache_retrieval_ctx *ctx)
{
	if (ctx == NULL || ctx->dr_queued) {
		return;
	}
	ctx->dr_queued = 1;
	macos9_deathrow_add(ctx, hlcache_rctx_deathrow_teardown, NULL);
}


/**
 * Attempt to clean the cache
 */
static void hlcache_clean(void *force_clean_flag)
{
	hlcache_entry *entry, *next;
	bool force_clean = (force_clean_flag != NULL);

	for (entry = hlcache->content_list; entry != NULL; entry = next) {
		next = entry->next;

		if (entry->content == NULL)
			continue;

		if (content_count_users(entry->content) != 0)
			continue;

		if (content__get_status(entry->content) == CONTENT_STATUS_LOADING) {
			if (force_clean == false)
				continue;
			NSLOG(netsurf, DEBUG, "Forcing content cleanup during shutdown");
			content_abort(entry->content);
			content_set_error(entry->content);
		}

		/** \todo This is over-zealous: all unused contents
		 * will be immediately destroyed. Ideally, we want to
		 * purge all unused contents that are using stale
		 * source data, and enough fresh contents such that
		 * the cache fits in the configured cache size limit.
		 */

#ifdef __MACOS9__
		/* fixes553 — do NOT evict a content the box walk currently on the
		 * stack still references (the walked html_content, or any content in
		 * its object_list).  Freeing it here, mid-walk, is the
		 * convert_xml_to_box UAF: box_image -> box_image_resolve_url ->
		 * html_fetch_object -> hlcache_handle_retrieve byte-scans a wild
		 * pointer.  A content with a live user must not be in the eviction
		 * set.  Leave the entry intact in the cache; the next bg_clean pass
		 * re-evaluates it once the walk is off-stack.  Generation-validated
		 * inside macos9_box_walk_owns_content, so a freed+reused walked
		 * content cannot ABA-false-positive a skip. */
		if (macos9_box_walk_owns_content(entry->content)) {
			macsurf_debug_log_writef(
				"clean: PINNED (walk live) c=%p",
				(void *)entry->content);
			continue;
		}
#endif

		/* Remove entry from cache */
		if (entry->prev == NULL)
			hlcache->content_list = entry->next;
		else
			entry->prev->next = entry->next;

		if (entry->next != NULL)
			entry->next->prev = entry->prev;

		/* fixes502: null entry->content BEFORE destroy so any
		 * hlcache_handle still pointing at this entry reads NULL
		 * from entry->content rather than freed/reused memory.
		 * fixes504: belt-and-suspenders guard — if c->handler is
		 * already NULL the sentinel in content_destroy would catch
		 * the double-destroy, but we skip the call entirely here
		 * to avoid even entering content_destroy on freed memory. */
		{
			struct content *c_to_destroy = entry->content;
			if (c_to_destroy->handler == NULL) {
				macsurf_debug_log_writef(
					"hlcache: skip already-destroyed entry=%p content=%p",
					(void*)entry, (void*)c_to_destroy);
				entry->content = NULL;
			} else {
				entry->content = NULL;
				macsurf_debug_log_writef(
					"hlcache: clean destroy entry=%p content=%p",
					(void*)entry, (void*)c_to_destroy);
				content_destroy(c_to_destroy);
			}
		}

		/* Destroy entry (Stage 1: deferred to the quiescent drain,
		 * gated on its content's pending continuations) */
		hlcache_entry_deferred_free(entry);
	}

	/* Attempt to clean the llcache */
	llcache_clean(false);

	/* Re-schedule ourselves */
	guit->misc->schedule(hlcache->params.bg_clean_time, hlcache_clean, NULL);
}

/**
 * Determine if the specified MIME type is acceptable
 *
 * \param mime_type       MIME type to consider
 * \param accepted_types  Array of acceptable types, or NULL for any
 * \param computed_type	  Pointer to location to receive computed type of object
 * \return True if the type is acceptable, false otherwise
 */
static bool hlcache_type_is_acceptable(lwc_string *mime_type,
		content_type accepted_types, content_type *computed_type)
{
	content_type type;

	type = content_factory_type_from_mime_type(mime_type);

	*computed_type = type;

	return ((accepted_types & type) != 0);
}

/**
 * Veneer between content callback API and hlcache callback API
 *
 * \param c	Content to emit message for
 * \param msg	Message to emit
 * \param data	Data for message
 * \param pw	Pointer to private data (hlcache_handle)
 */
static void hlcache_content_callback(struct content *c, content_msg msg,
		const union content_msg_data *data, void *pw)
{
	hlcache_handle *handle = pw;
	nserror error = NSERROR_OK;
	hlcache_event event;

	/* fixes97: dropped — fires for every broadcast forward, dominant. */

	memset(&event, 0, sizeof(event));
	event.type = msg;

	if (data != NULL) {
		event.data = *data;
	}

	if (handle->cb != NULL)
		error = handle->cb(handle, &event, handle->pw);

	if (error != NSERROR_OK)
		nslog_log(__FILE__, "", __LINE__, "Error in callback: %d", error);
}

/**
 * Find a content for the high-level cache handle
 *
 * \param ctx             High-level cache retrieval context
 * \param effective_type  Effective MIME type of content
 * \return NSERROR_OK on success,
 *         NSERROR_NEED_DATA on success where data is needed,
 *         appropriate error otherwise
 *
 * \pre handle::state == HLCACHE_HANDLE_NEW
 * \pre Headers must have been received for associated low-level handle
 * \post Low-level handle is either released, or associated with new content
 * \post High-level handle is registered with content
 */
static nserror hlcache_find_content(hlcache_retrieval_ctx *ctx,
		lwc_string *effective_type)
{
	hlcache_entry *entry;
	hlcache_event event;
	nserror error = NSERROR_OK;

	/* Search list of cached contents for a suitable one */
	for (entry = hlcache->content_list; entry != NULL; entry = entry->next) {
		hlcache_handle entry_handle = { entry, NULL, NULL };
		const llcache_handle *entry_llcache;

		if (entry->content == NULL)
			continue;

		/* Ignore contents in the error state */
		if (content_get_status(&entry_handle) == CONTENT_STATUS_ERROR)
			continue;

		/* Ensure that content is shareable */
		if (content_is_shareable(entry->content) == false)
			continue;

		/* Ensure that quirks mode is acceptable */
		if (content_matches_quirks(entry->content,
				ctx->child.quirks) == false)
			continue;

		/* Ensure that content uses same low-level object as
		 * low-level handle */
		entry_llcache = content_get_llcache_handle(entry->content);

		if (llcache_handle_references_same_object(entry_llcache,
				ctx->llcache))
			break;
	}

	if (entry == NULL) {
		/* No existing entry, so need to create one */
		entry = malloc(sizeof(hlcache_entry));
		if (entry == NULL)
			return NSERROR_NOMEM;

		/* Create content using llhandle. fixes97: success-path
		 * MS_LOGs dropped; the failure log stays for diagnostics. */
		entry->content = content_factory_create_content(ctx->llcache,
				ctx->child.charset, ctx->child.quirks,
				effective_type);
		if (entry->content == NULL) {
			MS_LOG("hlcache content create FAILED");
			free(entry);
			return NSERROR_NOMEM;
		}

		/* Insert into cache */
		entry->prev = NULL;
		entry->next = hlcache->content_list;
		if (hlcache->content_list != NULL)
			hlcache->content_list->prev = entry;
		hlcache->content_list = entry;

		/* Signal to caller that we created a content */
		error = NSERROR_NEED_DATA;

		hlcache->miss_count++;
	} else {
		/* Found a suitable content: no longer need low-level handle.
		 * fixes97: cache-hit MS_LOG dropped (very frequent). */
		llcache_handle_release(ctx->llcache);
		hlcache->hit_count++;
	}

	/* Associate handle with content */
	if (content_add_user(entry->content,
			hlcache_content_callback, ctx->handle) == false)
		return NSERROR_NOMEM;

	/* Associate cache entry with handle */
	ctx->handle->entry = entry;

	/* Catch handle up with state of content */
	if (ctx->handle->cb != NULL) {
		content_status status = content_get_status(ctx->handle);

		if (status == CONTENT_STATUS_LOADING) {
			/* fixes97: per-content MS_LOG dropped. */
			event.type = CONTENT_MSG_LOADING;
			ctx->handle->cb(ctx->handle, &event, ctx->handle->pw);
		} else if (status == CONTENT_STATUS_READY) {
			event.type = CONTENT_MSG_LOADING;
			ctx->handle->cb(ctx->handle, &event, ctx->handle->pw);

			if (ctx->handle->cb != NULL) {
				event.type = CONTENT_MSG_READY;
				ctx->handle->cb(ctx->handle, &event,
						ctx->handle->pw);
			}
		} else if (status == CONTENT_STATUS_DONE) {
			event.type = CONTENT_MSG_LOADING;
			ctx->handle->cb(ctx->handle, &event, ctx->handle->pw);

			if (ctx->handle->cb != NULL) {
				event.type = CONTENT_MSG_READY;
				ctx->handle->cb(ctx->handle, &event,
						ctx->handle->pw);
			}

			if (ctx->handle->cb != NULL) {
				event.type = CONTENT_MSG_DONE;
				ctx->handle->cb(ctx->handle, &event,
						ctx->handle->pw);
			}
		}
	}

	return error;
}

/**
 * Migrate a retrieval context into its final destination content
 *
 * \param ctx             Context to migrate
 * \param effective_type  The effective MIME type of the content, or NULL
 * \return NSERROR_OK on success,
 *         NSERROR_NEED_DATA on success where data is needed,
 *         appropriate error otherwise
 */
static nserror hlcache_migrate_ctx(hlcache_retrieval_ctx *ctx,
		lwc_string *effective_type)
{
	content_type type = CONTENT_NONE;
	nserror error = NSERROR_OK;

	ctx->migrate_target = true;

	if ((effective_type != NULL) &&
	    hlcache_type_is_acceptable(effective_type,
				       ctx->accepted_types,
				       &type)) {
		/* fixes97: migrate trace dropped. */
		error = hlcache_find_content(ctx, effective_type);
		if (error != NSERROR_OK && error != NSERROR_NEED_DATA) {
			if (ctx->handle->cb != NULL) {
				hlcache_event hlevent;

				hlevent.type = CONTENT_MSG_ERROR;
				hlevent.data.errordata.errorcode = NSERROR_UNKNOWN;
				hlevent.data.errordata.errormsg = messages_get("MiscError");

				ctx->handle->cb(ctx->handle, &hlevent,
						ctx->handle->pw);
			}

			llcache_handle_abort(ctx->llcache);
			llcache_handle_release(ctx->llcache);
		}
	} else if (type == CONTENT_NONE &&
			(ctx->flags & HLCACHE_RETRIEVE_MAY_DOWNLOAD)) {
		/* Unknown type, and we can download, so convert */
		llcache_handle_force_stream(ctx->llcache);

		if (ctx->handle->cb != NULL) {
			hlcache_event hlevent;

			hlevent.type = CONTENT_MSG_DOWNLOAD;
			hlevent.data.download = ctx->llcache;

			ctx->handle->cb(ctx->handle, &hlevent,
					ctx->handle->pw);
		}

		/* Ensure caller knows we need data */
		error = NSERROR_NEED_DATA;
	} else {
		/* fixes245 — silenced unaccept counter. Was useful during early
		 * content-handler wiring; now zero diagnostic value to users
		 * and was leaking to the title bar in some builds. The error
		 * dispatch below still fires normally so NetSurf core handles
		 * the unacceptable-type case correctly. */
		/* Unacceptable type: report error */
		if (ctx->handle->cb != NULL) {
			hlcache_event hlevent;

			hlevent.type = CONTENT_MSG_ERROR;
			hlevent.data.errordata.errorcode = NSERROR_UNKNOWN;
			hlevent.data.errordata.errormsg = messages_get("UnacceptableType");

			ctx->handle->cb(ctx->handle, &hlevent,
					ctx->handle->pw);
		}

		llcache_handle_abort(ctx->llcache);
		llcache_handle_release(ctx->llcache);
	}

	ctx->migrate_target = false;

	/* No longer require retrieval context */
	RING_REMOVE(hlcache->retrieval_ctx_ring, ctx);
	hlcache_rctx_deferred_free(ctx);   /* fixes600: deferred, was free() */

	return error;
}

/*
 * fixes571 -- guard the Content-Type header read on the mimesniff path.
 *
 * llcache_handle_get_header returns handle->object->headers[i].value. Under
 * re-entrant script-driven fetch churn (convert_script_sync_cb runs js_exec
 * synchronously, then unpauses the parser -> nested fetch/convert) an llcache
 * object can be destroyed while a retrieval ctx's handle still points at it;
 * handle->object is not NULL'd, so this returns a dangling/reused header value
 * (observed 0x3DBE2F6E, a code-space callback from a reused block) that the
 * content-type parser strlen's -> crash. The real cure is deferring the
 * llcache object free (the swap Stage 1 held out); until then, reject a
 * wild value here. mimesniff_compute_effective_type(NULL,...) is a supported
 * path (sniffs from data / defaults), so NULL is safe.
 */
static const char *
hlcache_safe_content_type(const llcache_handle *handle)
{
	const char *ct = llcache_handle_get_header(handle, "Content-Type");
	unsigned long a = (unsigned long) ct;

	if (ct != NULL && (a < 4UL || a >= 0x28000000UL)) {
		extern void macsurf_debug_log_writef(const char *fmt, ...);
		macsurf_debug_log_writef(
			"fixes571 CTYPE: wild content-type=%p (dangling llcache object)",
			(void *) ct);
		return NULL;
	}
	return ct;
}

/**
 * Handler for low-level cache events
 *
 * \param handle  Handle for which event is issued
 * \param event	  Event data
 * \param pw	  Pointer to client-specific data
 * \return NSERROR_OK on success, appropriate error otherwise
 */
static nserror
hlcache_llcache_callback(llcache_handle *handle,
			 const llcache_event *event,
			 void *pw)
{
	hlcache_retrieval_ctx *ctx = pw;
	lwc_string *effective_type = NULL;
	nserror error;

	/* fixes97: per-callback MS_LOGs dropped. They fired on every
	 * llcache event for every consumer — heaviest medium-noise
	 * contributor after llcache "fetch HEADER" / "fetch DATA". */

	assert(ctx->llcache == handle);

	switch (event->type) {
	case LLCACHE_EVENT_GOT_CERTS:
		/* Pass them on upward */
		if (ctx->handle->cb != NULL) {
			hlcache_event hlevent;

			hlevent.type = CONTENT_MSG_SSL_CERTS;
			hlevent.data.chain = event->data.chain;

			ctx->handle->cb(ctx->handle, &hlevent, ctx->handle->pw);
		}
		break;
	case LLCACHE_EVENT_HAD_HEADERS:
		error = mimesniff_compute_effective_type(hlcache_safe_content_type(handle), NULL, 0,
				ctx->flags & HLCACHE_RETRIEVE_SNIFF_TYPE,
				ctx->accepted_types == CONTENT_IMAGE,
				&effective_type);
		if (error == NSERROR_OK || error == NSERROR_NOT_FOUND) {
			/* If the sniffer was successful or failed to find
			 * a Content-Type header when sniffing was
			 * prohibited, we must migrate the retrieval context. */
			error = hlcache_migrate_ctx(ctx, effective_type);

			lwc_string_unref(effective_type);
		} else {
			MS_LOG("cb hdr sniff FAIL");
		}

		/* No need to report that we need data:
		 * we'll get some anyway if there is any */
		if (error == NSERROR_NEED_DATA)
			error = NSERROR_OK;

		return error;

		break;
	case LLCACHE_EVENT_HAD_DATA:
		error = mimesniff_compute_effective_type(hlcache_safe_content_type(handle),
				event->data.data.buf, event->data.data.len,
				ctx->flags & HLCACHE_RETRIEVE_SNIFF_TYPE,
				ctx->accepted_types == CONTENT_IMAGE,
				&effective_type);
		if (error != NSERROR_OK) {
			assert(0 && "MIME sniff failed with data");
		}

		error = hlcache_migrate_ctx(ctx, effective_type);

		lwc_string_unref(effective_type);

		return error;

		break;
	case LLCACHE_EVENT_DONE:
		/* DONE event before we could determine the effective MIME type.
		 */
		error = mimesniff_compute_effective_type(hlcache_safe_content_type(handle),
				NULL, 0, false, false, &effective_type);
		if (error == NSERROR_OK || error == NSERROR_NOT_FOUND) {
			error = hlcache_migrate_ctx(ctx, effective_type);

			lwc_string_unref(effective_type);
			return error;
		}

		if (ctx->handle->cb != NULL) {
			hlcache_event hlevent;

			hlevent.type = CONTENT_MSG_ERROR;
			hlevent.data.errordata.errorcode = error;
			hlevent.data.errordata.errormsg = NULL;

			ctx->handle->cb(ctx->handle, &hlevent, ctx->handle->pw);
		}
		break;
	case LLCACHE_EVENT_ERROR:
		if (ctx->handle->cb != NULL) {
			hlcache_event hlevent;

			hlevent.type = CONTENT_MSG_ERROR;
			hlevent.data.errordata.errorcode = event->data.error.code;
			hlevent.data.errordata.errormsg = event->data.error.msg;

			ctx->handle->cb(ctx->handle, &hlevent, ctx->handle->pw);
		}
		break;
	case LLCACHE_EVENT_PROGRESS:
		break;
	case LLCACHE_EVENT_REDIRECT:
		if (ctx->handle->cb != NULL) {
			hlcache_event hlevent;

			hlevent.type = CONTENT_MSG_REDIRECT;
			hlevent.data.redirect.from = event->data.redirect.from;
			hlevent.data.redirect.to = event->data.redirect.to;

			ctx->handle->cb(ctx->handle, &hlevent, ctx->handle->pw);
		}
		break;
	}

	return NSERROR_OK;
}


/******************************************************************************
 * Public API								      *
 ******************************************************************************/


nserror
hlcache_initialise(const struct hlcache_parameters *hlcache_parameters)
{
	nserror ret;

	hlcache = calloc(1, sizeof(struct hlcache_s));
	if (hlcache == NULL) {
		return NSERROR_NOMEM;
	}

	ret = llcache_initialise(&hlcache_parameters->llcache);
	if (ret != NSERROR_OK) {
		free(hlcache);
		hlcache = NULL;
		return ret;
	}

	hlcache->params = *hlcache_parameters;

	/* Schedule the cache cleanup */
	guit->misc->schedule(hlcache->params.bg_clean_time, hlcache_clean, NULL);

	return NSERROR_OK;
}

/* See hlcache.h for documentation */
void hlcache_stop(void)
{
	/* Remove the hlcache_clean schedule */
	guit->misc->schedule(-1, hlcache_clean, NULL);
}

/* See hlcache.h for documentation */
void hlcache_finalise(void)
{
	uint32_t num_contents, prev_contents;
	hlcache_entry *entry;
	hlcache_retrieval_ctx *ctx, *next;

	/* Obtain initial count of contents remaining */
	for (num_contents = 0, entry = hlcache->content_list;
			entry != NULL; entry = entry->next) {
		num_contents++;
	}

	nslog_log(__FILE__, "", __LINE__,
		      "%"PRIu32" contents remain before cache drain",
		      num_contents);

	/* Drain cache */
	do {
		prev_contents = num_contents;

		hlcache_clean(NULL);

		for (num_contents = 0, entry = hlcache->content_list;
				entry != NULL; entry = entry->next) {
			num_contents++;
		}
	} while (num_contents > 0 && num_contents != prev_contents);

	nslog_log(__FILE__, "", __LINE__,
		      "%"PRIu32" contents remaining after being polite",
		      num_contents);

	/* Drain cache again, forcing the matter */
	do {
		prev_contents = num_contents;

		hlcache_clean(&entry); // Any non-NULL pointer will do

		for (num_contents = 0, entry = hlcache->content_list;
				entry != NULL; entry = entry->next) {
			num_contents++;
		}
	} while (num_contents > 0 && num_contents != prev_contents);

	nslog_log(__FILE__, "", __LINE__, "%"PRIu32" contents remaining:", num_contents);
	for (entry = hlcache->content_list; entry != NULL; entry = entry->next) {
		hlcache_handle entry_handle = { entry, NULL, NULL };

		if (entry->content != NULL) {
			nslog_log(__FILE__, "", __LINE__,
				      "	%p : %s (%"PRIu32" users)",
				      entry,
				      nsurl_access(hlcache_handle_get_url(&entry_handle)),
				      content_count_users(entry->content));
		} else {
			nslog_log(__FILE__, "", __LINE__, "	%p", entry);
		}
	}

	/* Clean up retrieval contexts */
	if (hlcache->retrieval_ctx_ring != NULL) {
		ctx = hlcache->retrieval_ctx_ring;

		do {
			next = ctx->r_next;

			if (ctx->llcache != NULL)
				llcache_handle_release(ctx->llcache);

			if (ctx->handle != NULL)
				free(ctx->handle);

			if (ctx->child.charset != NULL)
				free((char *) ctx->child.charset);

			free(ctx);

			ctx = next;
		} while (ctx != hlcache->retrieval_ctx_ring);

		hlcache->retrieval_ctx_ring = NULL;
	}

	nslog_log(__FILE__, "", __LINE__,
		      "hit/miss %d/%d",
		      hlcache->hit_count,
		      hlcache->miss_count);

	/* De-schedule ourselves */
	guit->misc->schedule(-1, hlcache_clean, NULL);
	/* fixes515: drop any pending broadcast catch-up pass too */
	guit->misc->schedule(-1, hlcache_broadcast_catchup, NULL);
	hlcache_broadcast_catchup_scheduled = false;

	free(hlcache);
	hlcache = NULL;

	NSLOG(netsurf, INFO, "Finalising low-level cache");
	llcache_finalise();
}

/* See hlcache.h for documentation */
nserror
hlcache_handle_retrieve(nsurl *url,
			uint32_t flags,
			nsurl *referer,
			llcache_post_data *post,
			hlcache_handle_callback cb, void *pw,
			hlcache_child_context *child,
			content_type accepted_types,
			hlcache_handle **result)
{
	hlcache_retrieval_ctx *ctx;
	nserror error;

	assert(cb != NULL);

	ctx = calloc(1, sizeof(hlcache_retrieval_ctx));
	if (ctx == NULL) {
		return NSERROR_NOMEM;
	}

	ctx->handle = calloc(1, sizeof(hlcache_handle));
	if (ctx->handle == NULL) {
		free(ctx);
		return NSERROR_NOMEM;
	}

	if (child != NULL) {
		if (child->charset != NULL) {
			/* fixes570: child->charset is the parent html_content's
			 * c->encoding, read raw. Under heavy churn (68kmla) that
			 * field can be clobbered to a wild value (observed
			 * 0x2C7B7060) -> strdup's strlen walks unmapped memory and
			 * crashes in the Script Manager. Same range guard as the
			 * dom_string accessors; a wild charset is treated as
			 * no-charset (NULL is already handled below) since a
			 * child/image fetch needs none. This is a SEPARATE vector
			 * from the sched_entry aliasing -- the value is not a
			 * code-space callback -- so it also serves as the diagnostic
			 * log for locating the clobber source. */
			unsigned long ca_ = (unsigned long) child->charset;
			if (ca_ < 4UL || ca_ >= 0x28000000UL) {
				extern void macsurf_debug_log_writef(
					const char *fmt, ...);
				macsurf_debug_log_writef(
					"fixes570 CHARSET: wild child->charset=%p quirks=%d",
					(void *) ca_, (int) child->quirks);
				ctx->child.charset = NULL;
			} else {
				ctx->child.charset = strdup(child->charset);
				if (ctx->child.charset == NULL) {
					free(ctx->handle);
					free(ctx);
					return NSERROR_NOMEM;
				}
			}
		}
		ctx->child.quirks = child->quirks;
	}

	ctx->flags = flags;
	ctx->accepted_types = accepted_types;

	ctx->handle->cb = cb;
	ctx->handle->pw = pw;

	error = llcache_handle_retrieve(url, flags, referer, post,
			hlcache_llcache_callback, ctx,
			&ctx->llcache);
	if (error != NSERROR_OK) {
		/* error retrieving handle so free context and return error */
		free((char *) ctx->child.charset);
		free(ctx->handle);
		free(ctx);
	} else {
		/* successfully started fetch so add new context to list */
		RING_INSERT(hlcache->retrieval_ctx_ring, ctx);

		*result = ctx->handle;
	}
	return error;
}

/* See hlcache.h for documentation */
nserror hlcache_handle_release(hlcache_handle *handle)
{
	/* fixes499g — entry guard. This release path is reached during
	 * teardown (html_script_free -> here) where the script's handle slot
	 * may be a stale/freed pointer that was reallocated for other data.
	 * The crash that exposed this had r30 (handle) pointing at recycled
	 * heap holding ASCII text ("xy-q" = 0x78792D71) — dereferencing
	 * handle->entry for the log line below then read garbage and the
	 * writef call crashed at +0x24. Bail on a NULL handle, and on a handle
	 * whose address is outside the malloc heap range used on this G3
	 * (legitimate handles are ~0x06000000–0x09000000; a code/recycled
	 * pointer like 0x3E6846D4 is wild). Belt-and-suspenders with the
	 * fixes499e NULL-after-release in html_script_free. */
	{
		unsigned long ha = (unsigned long)handle;
		/* fixes572: also reject an UNALIGNED handle. A real hlcache_handle*
		 * is 4-byte aligned; a corrupted scripts-array entry can hold an
		 * in-range but unaligned garbage value (observed 0x07513EC1) that
		 * passes the range test yet faults when handle->entry is read. */
		if (handle == NULL || ha < 0x01000000UL || ha >= 0x20000000UL ||
		    (ha & 3) != 0) {
			macsurf_debug_log_writef(
				"hlcache_release: SKIP wild handle=%p", (void *)handle);
			return NSERROR_OK;
		}
	}

	/* fixes450: log every release so we can cross-ref against ZOMBIE / crash */
	macsurf_debug_log_writef(
		"hlcache_release: handle=%p entry=%p",
		(void *)handle,
		(void *)(handle->entry));

	if (handle->entry != NULL) {
		/* fixes576: verify handle->entry is still a LIVE cache entry
		 * before dereferencing entry->content. hlcache_clean unlinks a
		 * reaped entry from content_list (hlcache.c:183-198) then defers
		 * its free; during a re-entrant mass-close (browser_window_content
		 * _ready -> content_close(old) fired mid-READY-broadcast) an object
		 * handle can still point at a reaped+reused entry (observed entry=
		 * 0x0649ACB0, a freed llcache_object_user block now holding text) ->
		 * entry->content derefs freed memory -> crash. A range/alignment
		 * guard cannot catch this (a reaped entry is a valid-range aligned
		 * pointer), so verify LIVENESS by content_list membership. If the
		 * entry is gone, its content was destroyed with it, so skip the
		 * remove; the handle is still freed below. */
		hlcache_entry *le;
		bool entry_live = false;

		for (le = hlcache->content_list; le != NULL; le = le->next) {
			if (le == handle->entry) {
				entry_live = true;
				break;
			}
		}

		if (entry_live) {
			content_remove_user(handle->entry->content,
					hlcache_content_callback, handle);
		} else {
			macsurf_debug_log_writef(
				"hlcache_release: STALE entry=%p handle=%p (reaped, skip)",
				(void *) handle->entry, (void *) handle);
		}
	} else {
		RING_ITERATE_START(struct hlcache_retrieval_ctx,
				   hlcache->retrieval_ctx_ring,
				   ictx) {
			if (ictx->handle == handle &&
					ictx->migrate_target == false) {
				/* This is the nascent context for us,
				 * so abort the fetch */
				llcache_handle_abort(ictx->llcache);
				llcache_handle_release(ictx->llcache);
				/* Remove us from the ring */
				RING_REMOVE(hlcache->retrieval_ctx_ring, ictx);
				/* Throw us away (fixes600: deferred, was free()) */
				hlcache_rctx_deferred_free(ictx);
				/* And stop */
				RING_ITERATE_STOP(hlcache->retrieval_ctx_ring,
						ictx);
			}
		} RING_ITERATE_END(hlcache->retrieval_ctx_ring, ictx);
	}

	handle->cb = NULL;
	handle->pw = NULL;

	/* Stage 1: defer the handle free to the quiescent drain so a box
	 * walk / redraw still holding this handle can never read freed
	 * memory (the fixes428/429 box->object UAF). */
	hlcache_handle_deferred_free(handle);

	return NSERROR_OK;
}

/* See hlcache.h for documentation */
void safe_hlcache_handle_release(hlcache_handle **handle_ptr)
{
	hlcache_handle *h;

	if (handle_ptr == NULL || *handle_ptr == NULL)
		return;

	/* NULL the variable BEFORE releasing so any re-entrant read during
	 * the release sees NULL and skips, rather than racing on a handle
	 * that is being freed. */
	h = *handle_ptr;
	*handle_ptr = NULL;

	hlcache_handle_release(h);
}

/**
 * Re-issue every deferred content broadcast (fixes515).
 *
 * Walks the content cache and, for each live content still carrying a
 * pending (deferred) broadcast message, calls content_broadcast again.
 * content_broadcast clears the content's pending flag and, being called
 * from a non-reentrant context here, runs the walk normally.  If that
 * walk defers anything further, content_broadcast re-arms the pump.
 */
static void hlcache_broadcast_catchup(void *unused)
{
	hlcache_entry *entry, *next;

	(void) unused;

	hlcache_broadcast_catchup_scheduled = false;

	if (hlcache == NULL)
		return;

	for (entry = hlcache->content_list; entry != NULL; entry = next) {
		struct content *c;
		content_msg msg;
		int k;

		next = entry->next;

		c = entry->content;
		if (c == NULL)
			continue;
		/* destroyed sentinel — skip freed/destroyed contents */
		if (c->handler == NULL)
			continue;
		/* fixes559 — registry-membership guard.  The handler==NULL
		 * sentinel above reads c's (reusable) bytes; under heap reuse a
		 * dangling entry->content could read non-NULL garbage and slip
		 * through.  macos9_content_is_live consults the out-of-band
		 * registry instead, which deregisters as content_destroy's first
		 * action — so a freed/ABA-reused content fails here even when its
		 * bytes look like a live handler.  Expected never to fire given
		 * the unlink-before-free invariant (fixes502); if it ever logs a
		 * catch, a new free-path is leaving a list-resident entry dangling
		 * and must be fixed at the source. */
		if (!macos9_content_is_live(c)) {
			macsurf_debug_log_writef(
				"hlcache_catchup: STALE c=%p not registered, dropping",
				(void *)c);
			continue;
		}
		if (c->broadcast_pending_count <= 0)
			continue;

		/* Pop the FIFO front; content_broadcast's own exit-drain replays
		 * the remainder of this content's queue in order. */
		msg = c->broadcast_pending[0];
		for (k = 1; k < c->broadcast_pending_count; k++) {
			c->broadcast_pending[k - 1] = c->broadcast_pending[k];
		}
		c->broadcast_pending_count--;
		macsurf_debug_log_writef(
			"hlcache_catchup: replay msg=%d c=%p",
			(int)msg, (void *)c);
		content_broadcast(c, msg, NULL);
	}
}

/* See hlcache.h for documentation */
void hlcache_request_broadcast_catchup(void)
{
	if (hlcache_broadcast_catchup_scheduled)
		return;
	hlcache_broadcast_catchup_scheduled = true;
	guit->misc->schedule(0, hlcache_broadcast_catchup, NULL);
}

/* See hlcache.h for documentation */
bool hlcache_content_is_live(const struct content *c)
{
	hlcache_entry *entry;

	/* fixes520: liveness by REGISTRY MEMBERSHIP, not by reading the
	 * content's own fields.  A scheduled callback (deferred_parser_unpause,
	 * convert_xml_to_box, ...) can fire a tick after its content was
	 * destroyed in a bulk hlcache_clean pass; by then the struct is freed
	 * and reused, so its handler/aborted/status bytes read as plausible
	 * garbage and every field-based guard passes.  hlcache_clean NULLs
	 * entry->content before content_destroy and unlinks the entry after, so
	 * a content that is no longer reachable from content_list is provably
	 * gone — checked here without dereferencing the suspect pointer. */
	if (c == NULL || hlcache == NULL)
		return false;

	for (entry = hlcache->content_list; entry != NULL; entry = entry->next) {
		if (entry->content == c)
			return true;
	}

	return false;
}

/* See hlcache.h for documentation */
struct content *hlcache_handle_get_content(const hlcache_handle *handle)
{
	if (handle == NULL)
		return NULL;

	/* fixes508/Fix5: guard handle->entry against freed+reused memory.
	 * If hlcache_clean freed this entry and the allocator reused it for
	 * a CSS source buffer, handle->entry is a valid-looking address
	 * pointing at raw CSS bytes.  handle->entry->content then reads the
	 * first 4 bytes of that text (e.g. "@cha" = 0x40636861) as a
	 * struct content pointer.  The caller (nscss_register_import) casts
	 * the result to nscss_content* and reads data.sheet from it, getting
	 * the raw CSS bytes at that offset as a css_stylesheet pointer.
	 * css_stylesheet_register_import stores it in i->sheet; later select
	 * reads i->sheet->rule_list and crashes in css__selector_hash_insert.
	 * Confirmed: dm 067CC240 = "@charset \"UTF-8\"" in MacsBug.
	 * Legitimate hlcache_entry pointers live in 0x01000000–0x20000000;
	 * a reused-for-source-data pointer will be in the same range, so the
	 * range check alone is insufficient — also check entry->content for
	 * the same heap range to catch the common reuse pattern. */
	if (handle->entry != NULL) {
		unsigned long ea = (unsigned long)(void *)handle->entry;
		if (ea >= 0x01000000UL && ea < 0x20000000UL) {
			struct content *c = handle->entry->content;
			if (c != NULL) {
				unsigned long ca = (unsigned long)(void *)c;
				/* Also verify c is heap-range before returning */
				if (ca >= 0x01000000UL && ca < 0x20000000UL) {
					/* Final guard: if content_destroy already
					 * ran, c->handler is NULL. Callers that
					 * need a live content get NULL. */
					if (c->handler != NULL)
						return c;
					macsurf_debug_log_writef(
						"hlcache_get_content: content=%p already destroyed",
						(void *)c);
					return NULL;
				}
			}
		}
	}

	return NULL;
}

/* See hlcache.h for documentation */
nserror hlcache_handle_abort(hlcache_handle *handle)
{
	struct hlcache_entry *entry = handle->entry;
	struct content *c;

	if (entry == NULL) {
		/* This handle is not yet associated with a cache entry.
		 * The implication is that the fetch for the handle has
		 * not progressed to the point where the entry can be
		 * created. */

		RING_ITERATE_START(struct hlcache_retrieval_ctx,
				   hlcache->retrieval_ctx_ring,
				   ictx) {
			if (ictx->handle == handle &&
					ictx->migrate_target == false) {
				/* This is the nascent context for us,
				 * so abort the fetch */
				llcache_handle_abort(ictx->llcache);
				llcache_handle_release(ictx->llcache);
				/* Remove us from the ring */
				RING_REMOVE(hlcache->retrieval_ctx_ring, ictx);
				/* Throw us away (fixes600: deferred, was free()) */
				hlcache_rctx_deferred_free(ictx);
				/* And stop */
				RING_ITERATE_STOP(hlcache->retrieval_ctx_ring,
						ictx);
			}
		} RING_ITERATE_END(hlcache->retrieval_ctx_ring, ictx);

		return NSERROR_OK;
	}

	c = entry->content;

	if (content_count_users(c) > 1) {
		/* We are not the only user of 'c' so clone it. */
		struct content *clone = content_clone(c);

		if (clone == NULL)
			return NSERROR_NOMEM;

		entry = calloc(1, sizeof(struct hlcache_entry));

		if (entry == NULL) {
			content_destroy(clone);
			return NSERROR_NOMEM;
		}

		if (content_add_user(clone,
				hlcache_content_callback, handle) == false) {
			content_destroy(clone);
			free(entry);
			return NSERROR_NOMEM;
		}

		content_remove_user(c, hlcache_content_callback, handle);

		entry->content = clone;
		handle->entry = entry;
		entry->prev = NULL;
		entry->next = hlcache->content_list;
		if (hlcache->content_list != NULL)
			hlcache->content_list->prev = entry;
		hlcache->content_list = entry;

		c = clone;
	}

	return content_abort(c);
}

/* See hlcache.h for documentation */
nserror hlcache_handle_replace_callback(hlcache_handle *handle,
		hlcache_handle_callback cb, void *pw)
{
	handle->cb = cb;
	handle->pw = pw;

	return NSERROR_OK;
}

nserror hlcache_handle_clone(hlcache_handle *handle, hlcache_handle **result)
{
	*result = NULL;
	return NSERROR_CLONE_FAILED;
}

/* See hlcache.h for documentation */
nsurl *hlcache_handle_get_url(const hlcache_handle *handle)
{
	nsurl *result = NULL;

	/* fixes515: mirror the hlcache_handle_get_content guard.  The crash
	 * stack was convert_script_sync_cb -> hlcache_handle_get_url ->
	 * __ptr_glue with a bad r12, i.e. this ran on a handle whose memory
	 * had been freed and reused, so handle->entry / entry->content were
	 * wild pointers and content_get_url dereferenced garbage.  Validate
	 * the handle, the entry, the content, and the destroyed sentinel
	 * before touching any of them; return NULL (callers already tolerate
	 * a NULL URL) rather than dereferencing wild memory. */
	{
		unsigned long ha = (unsigned long)(const void *)handle;
		if (handle == NULL || ha < 0x01000000UL || ha >= 0x20000000UL) {
			macsurf_debug_log_writef(
				"hlcache_get_url: wild handle=%p", (void *)handle);
			return NULL;
		}
	}

	if (handle->entry != NULL) {
		unsigned long ea = (unsigned long)(void *)handle->entry;
		if (ea < 0x01000000UL || ea >= 0x20000000UL) {
			macsurf_debug_log_writef(
				"hlcache_get_url: wild entry=%p handle=%p",
				(void *)handle->entry, (void *)handle);
			return NULL;
		}
		{
			struct content *c = handle->entry->content;
			if (c != NULL) {
				unsigned long ca = (unsigned long)(void *)c;
				if (ca < 0x01000000UL || ca >= 0x20000000UL ||
				    c->handler == NULL) {
					macsurf_debug_log_writef(
						"hlcache_get_url: dead/wild content=%p",
						(void *)c);
					return NULL;
				}
			}
		}
		result = content_get_url(handle->entry->content);
	} else {
		RING_ITERATE_START(struct hlcache_retrieval_ctx,
				   hlcache->retrieval_ctx_ring,
				   ictx) {
			if (ictx->handle == handle) {
				/* This is the nascent context for us */
				result = llcache_handle_get_url(ictx->llcache);

				/* And stop */
				RING_ITERATE_STOP(hlcache->retrieval_ctx_ring,
						ictx);
			}
		} RING_ITERATE_END(hlcache->retrieval_ctx_ring, ictx);
	}

	return result;
}
