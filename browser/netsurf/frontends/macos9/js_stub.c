/*
 * MacSurf — js_stub.c
 * Stub implementations for JavaScript subsystem.
 * JavaScript is disabled on Mac OS 9 (WITHOUT_DUKTAPE).
 * Licensed under GPL v2.
 */

#include <stddef.h>
#include "utils/errors.h"

struct dom_event;
struct dom_document;
struct dom_node;
struct dom_element;
struct dom_string;

typedef struct jsheap jsheap;
typedef struct jsthread jsthread;

void js_initialise(void) {}
void js_finalise(void) {}

nserror js_newheap(int timeout, jsheap **heap)
{
	*heap = NULL;
	return NSERROR_OK;
}

void js_destroyheap(jsheap *heap) {}

nserror js_newthread(jsheap *heap, void *win_priv,
		void *doc_priv, jsthread **thread)
{
	*thread = NULL;
	return NSERROR_OK;
}

nserror js_closethread(jsthread *thread) { return NSERROR_OK; }
void js_destroythread(jsthread *thread) {}

unsigned char js_exec(jsthread *thread,
		const unsigned char *txt, unsigned long txtlen,
		const char *name)
{
	return 0;
}

unsigned char js_fire_event(jsthread *thread, const char *type,
		struct dom_document *doc, struct dom_node *target)
{
	return 0;
}

void js_handle_new_element(jsthread *thread,
		struct dom_element *node) {}

void js_event_cleanup(jsthread *thread,
		struct dom_event *evt) {}
