/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * macsurf_dom_compat.h — CW8 CFM broken-vtable workaround for tag-type lookup.
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 *
 * dom_html_element_get_tag_type() dispatches through DOM HTML vtable slot 75,
 * a broken CodeWarrior 8 CFM transition vector that redirects through ROM into
 * LNGInitFonts / LNGGetProcessScript and crashes (Unimplemented Trap, wild
 * strlen) on 68kmla.org / XenForo render paths. fixes429/431 worked around it
 * in hints.c by avoiding the broken slot; this header generalises that fix so
 * every remaining call site can use the WORKING dom_element_get_tag_name slot
 * instead. Drop-in: same signature, same DOM_HTML_ELEMENT_TYPE__UNKNOWN
 * fallback for the error / unmatched cases.
 */

#ifndef MACSURF_DOM_COMPAT_H
#define MACSURF_DOM_COMPAT_H

#include <dom/dom.h>

/* node is a const void * (not dom_node *) so this is a true drop-in for the
 * dom_html_element_get_tag_type MACRO it replaces: that macro cast its first
 * argument to (const dom_html_element *), so the call sites pass a mix of
 * dom_node *, const dom_node * (layout.c) and dom_event_target * (dom_event.c).
 * A concrete pointer-typed parameter would reject those under strict CW8. */
dom_exception macsurf_html_element_get_tag_type(const void *node,
		dom_html_element_type *type);

#endif
