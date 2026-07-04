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

/* Bridge called from content/handlers/html/interaction.c on a real click.
 * No JS engine -> nothing to dispatch, and nothing ever prevents default. */
int macsurf_qjs_dispatch_dom_click(struct dom_node *target)
{
	(void)target;
	return 0;
}

#endif /* !WITH_QUICKJS */
