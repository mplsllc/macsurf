/*
 * MacSurf shim — javascript/js.h
 *
 * Pre-stage-6 this header was a self-contained no-op stub for the
 * NetSurf js_thread API, used when JavaScript was compiled out
 * (WITHOUT_DUKTAPE).  Now that WITH_DUKTAPE is the default we forward
 * to the real NetSurf header so the prototype contract — including
 * `bool` return types vs the stub's `unsigned char` — is identical
 * across all TUs.  Keeping the file in place means CW8 access-path
 * resolution still finds *something* at `javascript/js.h`.
 */

#ifndef MACSURF_SHIM_JAVASCRIPT_JS_H
#define MACSURF_SHIM_JAVASCRIPT_JS_H

#ifdef WITH_DUKTAPE
#  include "content/handlers/javascript/js.h"
#else

#ifndef NETSURF_JAVASCRIPT_JS_H_
#define NETSURF_JAVASCRIPT_JS_H_

#include <stddef.h>
#include "utils/errors.h"

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

void js_handle_new_element(jsthread *thread, struct dom_element *node);
void js_event_cleanup(jsthread *thread, struct dom_event *evt);

#endif /* NETSURF_JAVASCRIPT_JS_H_ */
#endif /* WITH_DUKTAPE */

#endif /* MACSURF_SHIM_JAVASCRIPT_JS_H */
