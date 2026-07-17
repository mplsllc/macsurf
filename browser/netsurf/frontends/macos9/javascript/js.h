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
/* fixes881 (Phase 0.7) — set document.readyState='complete' and fire `load` at
 * BOTH document and window, once the box tree exists AND every subresource has
 * settled (html_proceed_to_done's READY->DONE transition).
 *
 * Previously html_finish_conversion fired `load` ~30 lines BEFORE dom_to_box,
 * so the observed order was window-load -> DOMContentLoaded -> document-load:
 * the reverse of spec, with `load` arriving before the box tree existed and
 * never reaching window at all afterwards. Implemented in macsurf_qjs.c. */
unsigned char js_fire_window_load(jsthread *thread, struct dom_document *doc);
/* fixes869 (#295) — fire `load` (ok!=0) or `error` (ok==0) at a <script> element
 * once its fetch+execute completes.  The universal dynamic-loader idiom
 * (createElement('script'); s.onload = () => resolve(); appendChild) resolves a
 * Promise from that event, so with no event the caller's chain stalls forever.
 * `thread` MUST be the script's OWNING content's js_thread -- a JSValue is only
 * valid in the runtime that made it (fixes854).  Implemented in macsurf_qjs.c.
 * NOTE: this declaration must live in the WITH_QUICKJS branch; the #else below
 * is the no-op-stub build and is not what MacSurf compiles. */
unsigned char js_fire_script_load(jsthread *thread, struct dom_node *node,
		int ok);
/* fixes873 (#301) — set document.currentScript for the duration of a script's
 * execution (NULL clears it).  webpack's publicPath runtime reads it first and
 * throws "Automatic publicPath is not supported in this browser" if it and the
 * getElementsByTagName("script") fallback both come up empty -- which kills the
 * bundle on its own prologue, before any application code. */
void js_set_current_script(jsthread *thread, struct dom_node *node);
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

/* fixes869 (#295) — fire `load` (ok!=0) or `error` (ok==0) at a <script>
 * element once its fetch+execute completes.  The dynamic-loader idiom
 * (createElement('script'); s.onload = () => resolve(); appendChild) resolves a
 * Promise from that event, so without it the caller's chain stalls forever.
 * `thread` MUST be the script's owning content's js_thread -- a JSValue is only
 * valid in the runtime that made it (see fixes854). */
unsigned char js_fire_script_load(jsthread *thread, struct dom_node *node,
		int ok);

void js_handle_new_element(jsthread *thread, struct dom_element *node);
void js_event_cleanup(jsthread *thread, struct dom_event *evt);

#endif /* NETSURF_JAVASCRIPT_JS_H_ */
#endif /* WITH_QUICKJS */

#endif /* MACSURF_SHIM_JAVASCRIPT_JS_H */
