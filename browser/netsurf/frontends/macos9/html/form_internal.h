/*
 * MacSurf shim -- html/form_internal.h
 *
 * Forwards to the real form_internal.h so CW8 gets the full struct form
 * definition (action, prev, etc.). This shim does NOT set the real guard
 * NETSURF_HTML_FORM_INTERNAL_H so the canonical file owns it.
 */

#ifndef MACSURF_SHIM_FORM_INTERNAL_H
#define MACSURF_SHIM_FORM_INTERNAL_H
#include "content/handlers/html/form_internal.h"
#endif
