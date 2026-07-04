/*
 * MacSurf shim — javascript/js.h
 *
 * Routes to the real NetSurf js_thread API header when the QuickJS engine
 * is active.  When WITH_QUICKJS is not defined the no-op stubs below are used.
 */

#ifndef MACSURF_SHIM_JAVASCRIPT_JS_H
#define MACSURF_SHIM_JAVASCRIPT_JS_H

#ifdef WITH_QUICKJS
#  include "content/handlers/javascript/js.h"
/* MacSurf GATE 3 extension: fire DOMContentLoaded+load into the JS document's
 * registered listeners once the initial box tree exists (drains XF.ready and
 * runs XF.activate(document)).  Implemented in javascript/macsurf_qjs.c. */
unsigned char js_fire_dom_ready(jsthread *thread, struct dom_document *doc);
#else

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
void    js_destroyheap(jsheap *heap);

nserror js_newthread(jsheap *heap, void *win_priv,
		void *doc_priv, jsthread **thread);
nserror js_closethread(jsthread *thread);
void    js_destroythread(jsthread *thread);

unsigned char js_exec(jsthread *thread,
		const unsigned char *txt, size_t txtlen,
		const char *name);
unsigned char js_fire_event(jsthread *thread, const char *type,
		struct dom_document *doc, struct dom_node *target);
unsigned char js_fire_dom_ready(jsthread *thread, struct dom_document *doc);

void js_handle_new_element(jsthread *thread, struct dom_element *node);
void js_event_cleanup(jsthread *thread, struct dom_event *evt);

#endif /* NETSURF_JAVASCRIPT_JS_H_ */
#endif /* WITH_QUICKJS */

#endif /* MACSURF_SHIM_JAVASCRIPT_JS_H */
