/*
 * MacSurf — libdom_c89_helpers.c
 *
 * Out-of-line bodies for functions that were static inline in libdom
 * internal headers.  CW8 C89 mode does not reliably emit per-TU
 * copies of static inline functions, so we provide one global
 * definition of each here.  The headers now have extern declarations
 * pointing at these.
 *
 * Functions:
 *   list_init, list_append, list_del           (from utils/list.h)
 *   dom_node_destroy_impl, dom_node_copy_impl  (from core/node.h)
 *   dom_element_parse_attribute_impl           (from core/element.h)
 *   dom_event_destroy_impl                     (from events/event.h)
 */

#include <stddef.h>

#include "utils/list.h"
#include "core/node.h"
#include "core/element.h"
#include "events/event.h"

/* ---- list.h functions ---- */

void list_init(struct list_entry *ent)
{
	ent->prev = ent;
	ent->next = ent;
}

void list_append(struct list_entry *head, struct list_entry *new_entry)
{
	new_entry->next = head;
	new_entry->prev = head->prev;
	head->prev->next = new_entry;
	head->prev = new_entry;
}

void list_del(struct list_entry *ent)
{
	ent->prev->next = ent->next;
	ent->next->prev = ent->prev;

	ent->prev = ent;
	ent->next = ent;
}

/* ---- node.h functions ---- */

void dom_node_destroy_impl(struct dom_node_internal *node)
{
	((dom_node_protect_vtable *) node->vtable)->destroy(node);
}

dom_exception dom_node_copy_impl(struct dom_node_internal *old,
		struct dom_node_internal **copy)
{
	return ((dom_node_protect_vtable *) old->vtable)->copy(old, copy);
}

/* ---- element.h functions ---- */

/* Undefine the macro so we can reference the vtable field directly
 * without triggering the macro that remaps it to our _impl name. */
#undef dom_element_parse_attribute

dom_exception dom_element_parse_attribute_impl(dom_element *ele,
		dom_string *name, dom_string *value, dom_string **parsed)
{
	struct dom_node_internal *node = (struct dom_node_internal *) ele;
	return ((dom_element_protected_vtable *) node->vtable)->
			dom_element_parse_attribute(ele, name, value, parsed);
}

/* ---- event.h functions ---- */

void dom_event_destroy_impl(dom_event *evt)
{
	evt->vtable->destroy(evt);
}
