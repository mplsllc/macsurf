/*
 * MacSurf — macsurf_js.c
 *
 * Real implementation of NetSurf's js_thread API using Duktape.
 */

#include <stdlib.h>
#include <string.h>
#include "utils/errors.h"
#include "utils/log.h"
#include "duktape.h"
#include "macsurf_js.h"

#ifdef WITH_DUKTAPE

struct jsheap {
        duk_context *ctx;
        int timeout;
};

struct jsthread {
        struct jsheap *heap;
        duk_context *ctx;
        void *win_priv;
        void *doc_priv;
};

static struct jsheap *global_heap = NULL;

extern void macsurf_js_fatal(void *udata, const char *msg);

static duk_ret_t native_console_log(duk_context *ctx) {
        int n = duk_get_top(ctx), i;
        char log_buf[512];
        size_t pos = 0;
        log_buf[0] = '\0';

        for (i = 0; i < n; i++) {
                const char *s = duk_safe_to_string(ctx, i);
                size_t slen = strlen(s);
                if (pos + slen + 2 < 512) {
                        if (pos > 0) log_buf[pos++] = ' ';
                        memcpy(log_buf + pos, s, slen);
                        pos += slen;
                }
        }
        log_buf[pos] = '\0';
        MS_LOG(log_buf);
        return 0;
}

void js_initialise(void) {
}

void js_finalise(void) {
}

nserror js_newheap(int timeout, jsheap **heap) {
        struct jsheap *h = (struct jsheap *)calloc(1, sizeof(*h));
        if (!h) return NSERROR_NOMEM;

        h->timeout = timeout;
        h->ctx = duk_create_heap(NULL, NULL, NULL, NULL, macsurf_js_fatal);
        if (!h->ctx) {
                free(h);
                return NSERROR_NOMEM;
        }

        duk_push_global_object(h->ctx);
        duk_push_object(h->ctx);
        duk_push_c_function(h->ctx, native_console_log, DUK_VARARGS);
        duk_put_prop_string(h->ctx, -2, "log");
        duk_put_prop_string(h->ctx, -2, "console");
        duk_pop(h->ctx);

        macsurf_js_init_dom(h->ctx);

        global_heap = h;
        *heap = h;
        return NSERROR_OK;
}

void js_destroyheap(jsheap *heap) {
        if (!heap) return;
        if (heap->ctx) duk_destroy_heap(heap->ctx);
        if (global_heap == heap) global_heap = NULL;
        free(heap);
}

nserror js_newthread(jsheap *heap, void *win_priv,
                void *doc_priv, jsthread **thread) {
        struct jsthread *t = (struct jsthread *)calloc(1, sizeof(*t));
        if (!t) return NSERROR_NOMEM;

        t->heap = heap;
        t->ctx = heap->ctx;
        t->win_priv = win_priv;
        t->doc_priv = doc_priv;

        *thread = t;
        return NSERROR_OK;
}

nserror js_closethread(jsthread *thread) {
        (void)thread;
        return NSERROR_OK;
}

void js_destroythread(jsthread *thread) {
        free(thread);
}

unsigned char js_exec(jsthread *thread,
                const unsigned char *txt, size_t txtlen,
                const char *name) {
        if (!thread || !txt) return 0;

        duk_push_lstring(thread->ctx, (const char *)txt, (duk_size_t)txtlen);
        duk_push_string(thread->ctx, name ? name : "anonymous");

        if (duk_pcompile(thread->ctx, 0) != 0) {
                MS_LOG(duk_safe_to_string(thread->ctx, -1));
                duk_pop(thread->ctx);
                return 0;
        }

        if (duk_pcall(thread->ctx, 0) != 0) {
                MS_LOG(duk_safe_to_string(thread->ctx, -1));
                duk_pop(thread->ctx);
                return 0;
        }

        duk_pop(thread->ctx);
        return 1;
}

unsigned char js_fire_event(jsthread *thread, const char *type,
                struct dom_document *doc, struct dom_node *target) {
        (void)thread; (void)type; (void)doc; (void)target;
        return 0;
}

void js_handle_new_element(jsthread *thread, struct dom_element *node) {
        (void)thread; (void)node;
}

void js_event_cleanup(jsthread *thread, struct dom_event *evt) {
        (void)thread; (void)evt;
}

void macsurf_js_pump_all(void) {
        if (global_heap && global_heap->ctx) {
                duk_gc(global_heap->ctx, 0);
        }
}

#endif /* WITH_DUKTAPE */
