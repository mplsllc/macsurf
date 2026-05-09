/*
 * MacSurf stub -- javascript/js.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 * JavaScript is disabled on Mac OS 9 (WITHOUT_DUKTAPE).
 * Licensed under GPL v2.
 */

#ifndef NETSURF_JAVASCRIPT_JS_H_
#define NETSURF_JAVASCRIPT_JS_H_

#include <stddef.h>
#include "utils/ns_errors.h"

struct dom_event;
struct dom_document;
struct dom_node;
struct dom_element;
struct dom_string;

typedef struct jsheap jsheap;
typedef struct jsthread jsthread;

void js_initialise(void);
void js_finalise(void);

nserror js_newheap(int timeout, jsheap **heap);
void js_destroyheap(jsheap *heap);

nserror js_newthread(jsheap *heap, void *win_priv,
		void *doc_priv, jsthread **thread);
nserror js_closethread(jsthread *thread);
void js_destroythread(jsthread *thread);

unsigned char js_exec(jsthread *thread,
		const unsigned char *txt, size_t txtlen,
		const char *name);
unsigned char js_fire_event(jsthread *thread, const char *type,
		struct dom_document *doc, struct dom_node *target);

void js_handle_new_element(jsthread *thread,
		struct dom_element *node);
void js_event_cleanup(jsthread *thread,
		struct dom_event *evt);

#endif
