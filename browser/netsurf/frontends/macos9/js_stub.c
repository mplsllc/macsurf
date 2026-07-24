/*
 * MacSurf — js_stub.c
 *
 * No-op stubs for the NetSurf js_thread API.  Active only when the QuickJS
 * engine is not built (WITH_QUICKJS undefined).  When QuickJS is on, every
 * symbol below is provided for real by macsurf_qjs.c and this whole file
 * compiles to nothing.
 *
 * Keep the file in the project file list so toggling JS off doesn't require
 * an .mcp edit.
 */



#include <stddef.h>
#include "utils/ns_errors.h"

#ifndef WITH_QUICKJS

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
	(void)timeout;
	*heap = NULL;
	return NSERROR_OK;
}

void js_destroyheap(jsheap *heap) { (void)heap; }

nserror js_newthread(jsheap *heap, void *win_priv,
		void *doc_priv, jsthread **thread)
{
	(void)heap; (void)win_priv; (void)doc_priv;
	*thread = NULL;
	return NSERROR_OK;
}

nserror js_closethread(jsthread *thread) { (void)thread; return NSERROR_OK; }
void    js_destroythread(jsthread *thread) { (void)thread; }

unsigned char js_exec(jsthread *thread,
		const unsigned char *txt, unsigned long txtlen,
		const char *name)
{
	(void)thread; (void)txt; (void)txtlen; (void)name;
	return 0;
}

unsigned char js_fire_event(jsthread *thread, const char *type,
		struct dom_document *doc, struct dom_node *target)
{
	(void)thread; (void)type; (void)doc; (void)target;
	return 0;
}

void js_handle_new_element(jsthread *thread, struct dom_element *node)
{
	(void)thread; (void)node;
}

void js_event_cleanup(jsthread *thread, struct dom_event *evt)
{
	(void)thread; (void)evt;
}

/* fixes1008 — event-bridge hooks for the no-JS build.
 *
 * event_type_live() returns 1 (FAILS OPEN) to match the engine's own
 * behaviour: the gate exists to skip pointless work, never to suppress
 * dispatch. With no JS engine the dispatch reaches an empty listener set and
 * costs nothing anyway, and returning 0 here would bake "gate closed" into the
 * one build where it can never be reopened. */
int macsurf_qjs_event_type_live(const char *type)
{
	(void)type;
	return 1;
}

void macsurf_qjs_set_event_detail(int x, int y, int button, int key, int mods)
{
	(void)x; (void)y; (void)button; (void)key; (void)mods;
}

void macsurf_qjs_clear_event_detail(void)
{
}

#endif /* !WITH_QUICKJS */
