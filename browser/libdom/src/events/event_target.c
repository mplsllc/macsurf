/*
 * This file is part of libdom.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2009 Bo Yang <struggleyb.nku@gmail.com>
 */

#include <assert.h>
#include <stdlib.h>

#include "event_i.h"
#include "event_listener_i.h"
#include "event_target_i.h"

#include "document_i.h"
#include "node_i.h"
#include "dom_internal_string.h"

#include "utils.h"
#include "validate.h"

static void event_target_destroy_listener(struct listener_entry *e)
{
	list_del(&e->list);
	dom_event_listener_unref(e->listener);
	dom_string_unref(e->type);
	free(e);
}
static void event_target_destroy_listeners(struct listener_entry *list)
{
	struct listener_entry *next;

	while (list != (struct listener_entry *) list->list.next) {
		next = (struct listener_entry *) list->list.next;
		event_target_destroy_listener(list);
		list = next;
	}

	event_target_destroy_listener(list);
}

/* Initialise this EventTarget */
dom_exception _dom_event_target_internal_initialise(
		dom_event_target_internal *eti)
{
	eti->listeners = NULL;

	return DOM_NO_ERR;
}

/* Finalise this EventTarget */
void _dom_event_target_internal_finalise(dom_event_target_internal *eti)
{
	if (eti->listeners != NULL) {
		event_target_destroy_listeners(eti->listeners);
		eti->listeners = NULL;
	}
}

/*-------------------------------------------------------------------------*/
/* The public API */

/**
 * Add an EventListener to the EventTarget
 *
 * \param et        The EventTarget object
 * \param type      The event type which this event listener listens for
 * \param listener  The event listener object
 * \param capture   Whether add this listener in the capturing phase
 * \return DOM_NO_ERR on success, appropriate dom_exception on failure.
 */
dom_exception _dom_event_target_add_event_listener(
		dom_event_target_internal *eti,
		dom_string *type, struct dom_event_listener *listener, 
		bool capture)
{
	struct listener_entry *le = NULL;

	le = malloc(sizeof(struct listener_entry));
	if (le == NULL)
		return DOM_NO_MEM_ERR;
	
	/* Initialise the listener_entry */
	list_init(&le->list);
	le->type = dom_string_ref(type);
	le->listener = listener;
	dom_event_listener_ref(listener);
	le->capture = capture;

	if (eti->listeners == NULL) {
		eti->listeners = le;
	} else {
		list_append(&eti->listeners->list, &le->list);
	}

	return DOM_NO_ERR;
}

/**
 * Remove an EventListener from the EventTarget
 *
 * (LibDOM extension: If type is NULL, remove all listener registrations
 * regardless of type and cature)
 *
 * \param et        The EventTarget object
 * \param type      The event type this listener is registered for 
 * \param listener  The listener object
 * \param capture   Whether the listener is registered at the capturing phase
 * \return DOM_NO_ERR on success, appropriate dom_exception on failure.
 */
dom_exception _dom_event_target_remove_event_listener(
		dom_event_target_internal *eti,
		dom_string *type, struct dom_event_listener *listener, 
		bool capture)
{
	if (eti->listeners != NULL) {
		struct listener_entry *le = eti->listeners;

		do {
			bool matches;
			if (type == NULL) {
				matches = (le->listener == listener);
			} else {
				matches = dom_string_isequal(le->type, type) &&
					(le->listener == listener) &&
					(le->capture == capture);
			}
			if (matches) {
				if (le->list.next == &le->list) {
					eti->listeners = NULL;
				} else {
					eti->listeners =
						(struct listener_entry *)
						le->list.next;
				}
				list_del(&le->list);
				dom_event_listener_unref(le->listener);
				dom_string_unref(le->type);
				free(le);
				break;
			}

			le = (struct listener_entry *) le->list.next;
		} while (eti->listeners != NULL && le != eti->listeners);
	}

	return DOM_NO_ERR;
}

/**
 * Add an EventListener
 *
 * \param et         The EventTarget object
 * \param namespace  The namespace of this listener
 * \param type       The event type which this event listener listens for
 * \param listener   The event listener object
 * \param capture    Whether add this listener in the capturing phase
 * \return DOM_NO_ERR on success, appropriate dom_exception on failure.
 *
 * We don't support this API now, so it always return DOM_NOT_SUPPORTED_ERR.
 */
dom_exception _dom_event_target_add_event_listener_ns(
		dom_event_target_internal *eti,
		dom_string *namespace, dom_string *type, 
		struct dom_event_listener *listener, bool capture)
{
	UNUSED(eti);
	UNUSED(namespace);
	UNUSED(type);
	UNUSED(listener);
	UNUSED(capture);

	return DOM_NOT_SUPPORTED_ERR;
}

/**
 * Remove an EventListener
 *
 * \param et         The EventTarget object
 * \param namespace  The namespace of this listener
 * \param type       The event type which this event listener listens for
 * \param listener   The event listener object
 * \param capture    Whether add this listener in the capturing phase
 * \return DOM_NO_ERR on success, appropriate dom_exception on failure.
 *
 * We don't support this API now, so it always return DOM_NOT_SUPPORTED_ERR.
 */
dom_exception _dom_event_target_remove_event_listener_ns(
		dom_event_target_internal *eti,
		dom_string *namespace, dom_string *type, 
		struct dom_event_listener *listener, bool capture)
{
	UNUSED(eti);
	UNUSED(namespace);
	UNUSED(type);
	UNUSED(listener);
	UNUSED(capture);

	return DOM_NOT_SUPPORTED_ERR;
}

/*-------------------------------------------------------------------------*/

/**
 * Dispatch an event on certain EventTarget
 *
 * \param et       The EventTarget object
 * \param eti      Internal EventTarget object
 * \param evt      The event object
 * \param success  Indicates whether any of the listeners which handled the 
 *                 event called Event.preventDefault(). If 
 *                 Event.preventDefault() was called the returned value is 
 *                 false, else it is true.
 * \return DOM_NO_ERR on success, appropriate dom_exception on failure.
 */
dom_exception _dom_event_target_dispatch(dom_event_target *et,
		dom_event_target_internal *eti, 
		struct dom_event *evt, dom_event_flow_phase phase,
		bool *success)
{
	if (eti->listeners != NULL) {
		struct listener_entry *le = eti->listeners;

		evt->current = et;

		do {
			if (dom_string_isequal(le->type, evt->type)) {
				assert(le->listener->handler != NULL);

				/* ==== MACSURF LOCAL PATCH (fixes1005) ============
				 * grep MACSURF LOCAL PATCH before updating vendored
				 * libdom -- this is not upstream and a vendor bump
				 * will silently revert it.
				 *
				 * THE TARGET WAS VISITED TWICE, IN BOTH DIRECTIONS.
				 *
				 * node.c's path walk builds targets[] starting AT the
				 * target (`for (; target != NULL; target =
				 * target->parent)`), so targets[0] IS the target. The
				 * capture loop then runs targets[ntargets-1]..targets[0]
				 * and the bubble loop runs targets[0]..targets[ntargets-1]
				 * -- both include the target -- and the AT_TARGET call
				 * visits it a third time. Net effect before this patch:
				 *   non-capture listener on target: AT_TARGET + BUBBLING
				 *   capture     listener on target: CAPTURING + AT_TARGET
				 * i.e. EVERY listener on the target fired exactly twice.
				 *
				 * Per DOM spec the capture and bubble passes cover
				 * ANCESTORS ONLY; the target is visited exactly once, in
				 * AT_TARGET, where capture and non-capture listeners both
				 * run in registration order. So both the capture and the
				 * bubble clause gain `evt->target != evt->current`. It is
				 * deliberately fixed HERE rather than by trimming
				 * targets[] in node.c: the capture pass legitimately needs
				 * the target in the array (a descendant's dispatch must
				 * still see it as an ancestor), and the phase filter is
				 * the single place that decides what a phase means.
				 *
				 * Why MacSurf and not upstream: nothing in this tree ever
				 * called dom_event_target_add_event_listener until
				 * fixes989 wired JS addEventListener to libdom, so the
				 * double-visit was inert for the whole life of the port.
				 * Since fixes989 it has been live on hardware -- double
				 * form submits, double toggles, double counters, on every
				 * click, submit and keydown (interaction.c:1785/1857/2024).
				 *
				 * Found by harness Test 38, the Phase 0 control, which is
				 * also its regression test (parts 1b/1c/1d). Every prior
				 * event test dispatched through el.dispatchEvent -- the
				 * JS-local path -- and asserted a boolean, so none of them
				 * could observe a count of 2.
				 *
				 * Reported upstream to NetSurf; if taken, this patch can
				 * be dropped on the next vendor bump.
				 * ================================================= */
				if ((le->capture &&
						phase == DOM_CAPTURING_PHASE &&
						evt->target != evt->current) ||
				    (le->capture == false &&
						phase == DOM_BUBBLING_PHASE &&
						evt->target != evt->current) ||
				    (evt->target == evt->current &&
						phase == DOM_AT_TARGET)) {
					le->listener->handler(evt, 
							le->listener->pw);
					/* If the handler called
					 * stopImmediatePropagation, we should
					 * break */
					if (evt->stop_now == true)
						break;
				}
			}

			le = (struct listener_entry *) le->list.next;
		} while (le != eti->listeners);
	}

	if (evt->prevent_default == true)
		*success = false;

	return DOM_NO_ERR;
}

