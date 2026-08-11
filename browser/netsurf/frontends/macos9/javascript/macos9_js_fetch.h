/*
 * MacSurf — macos9_js_fetch.h
 *
 * Native XMLHttpRequest/fetch backend (S3, #167). See macos9_js_fetch.c
 * for the full design comment.
 */

#ifndef MACOS9_JS_FETCH_H
#define MACOS9_JS_FETCH_H

#ifdef WITH_QUICKJS

#include "quickjs.h"

/* Registers __xhrNativeSend / __xhrNativeAbort / __beaconSend on `global`.
 * Called once from register_browser_globals() per realm (js_newthread
 * rebuilds the realm every navigation, so this runs again each time). */
void macos9_js_fetch_install(JSContext *ctx, JSValueConst global);

/* Aborts every in-flight XHR, frees its dup'd JSValue against old_ctx, and
 * cancels any pending scheduled delivery. Must be called BEFORE old_ctx is
 * freed, from the same navigation-teardown point as qjs_flush_timers() —
 * mirrors that function exactly (see its comment in macsurf_qjs.c). */
void macos9_js_fetch_flush(JSContext *old_ctx);

#endif /* WITH_QUICKJS */

#endif /* MACOS9_JS_FETCH_H */
