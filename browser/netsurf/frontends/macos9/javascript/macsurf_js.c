/*
 * MacSurf — macsurf_js.c
 *
 * Duktape integration for the NetSurf js_thread API.
 *
 * fixes316 (#115/#116/#117/#118/#119/#120/#121/#123/#124/#125 unblock):
 * route NetSurf core's <script>-tag dispatch (js_newheap / js_newthread /
 * js_exec / ...) into Duktape. Pre-fixes316 these names resolved to the
 * no-op stubs in js_stub.c — Duktape was linked into the binary but
 * never reached because the MacSurf-side wrapper exported js_new_heap
 * (with an underscore) and an internal macsurf_js_exec_script, neither
 * of which NetSurf core ever called. Result: every <script> tag returned
 * 0 and the script body leaked through as visible text on the page.
 *
 * fixes316 retries the bridge from fixes289 ISOLATED — no bundled
 * Tier 1 globals (navigator / atob / encodeURI / alert / location /
 * history / document.title / setTimeout / fetch / etc.). Tier 1 stacks
 * one issue at a time once mactrove.com is regression-confirmed on the
 * bridge alone, per DIRECTIVE #5.
 *
 * Preserves the pre-existing in-heap console.log registration (it lived
 * inside the old js_new_heap as part of the recovery sprint and predates
 * the fixes282-289c chain that fixes290 reverted). console.log is
 * effectively dead pre-fix316 because nothing can reach it; post-fix316
 * the same code path becomes live. That's not a Tier 1 add — it's the
 * bridge waking up code that was already in the tree.
 */

#include <stdio.h>
#include <time.h>
#include <string.h>

#include "utils/ns_errors.h"
#include "macos9.h"
#include "duktape.h"
#include "macsurf_debug.h"
#include "macsurf_js.h"

#ifdef WITH_DUKTAPE

/* Forward declarations for the NetSurf DOM types so js_fire_event /
 * js_handle_new_element / js_event_cleanup can take them by pointer
 * without dragging in libdom headers (which need their own port pass). */
struct dom_event;
struct dom_document;
struct dom_node;
struct dom_element;
struct dom_string;

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

/* fixes342a — forward declaration so call sites in
 * register_browser_globals (above the definition further down) don't
 * trigger CW8 implicit-int and mismatch the definition's void return. */
void macsurf_js__safe_eval(duk_context *ctx, const char *src);

/* fixes319 (#115) — console.{log,warn,error,info,debug}.
 * All five route to MS_LOG with a level-tagged prefix. Real browsers
 * differentiate streams; on OS 9 we only have one log channel, so the
 * level shows up as a `[js:warn]` etc. prefix instead.
 *
 * Shared formatter: space-separated stringification, 512-byte cap. */
static void macsurf_js__console_emit(duk_context *ctx, const char *prefix)
{
	int n = duk_get_top(ctx);
	int i;
	char log_buf[512];
	size_t pos = 0;
	size_t prefix_len = strlen(prefix);
	log_buf[0] = '\0';
	if (prefix_len + 1 < 512) {
		memcpy(log_buf, prefix, prefix_len);
		pos = prefix_len;
	}
	for (i = 0; i < n; i++) {
		const char *s = duk_safe_to_string(ctx, i);
		size_t slen = strlen(s);
		if (pos + slen + 2 < 512) {
			if (i > 0 || prefix_len > 0) log_buf[pos++] = ' ';
			memcpy(log_buf + pos, s, slen);
			pos += slen;
		}
	}
	log_buf[pos] = '\0';
	MS_LOG(log_buf);
}

static duk_ret_t native_console_log(duk_context *ctx)
{
	macsurf_js__console_emit(ctx, "[js]");
	return 0;
}

static duk_ret_t native_console_warn(duk_context *ctx)
{
	macsurf_js__console_emit(ctx, "[js:warn]");
	return 0;
}

static duk_ret_t native_console_error(duk_context *ctx)
{
	macsurf_js__console_emit(ctx, "[js:error]");
	return 0;
}

static duk_ret_t native_console_info(duk_context *ctx)
{
	macsurf_js__console_emit(ctx, "[js:info]");
	return 0;
}

static duk_ret_t native_console_debug(duk_context *ctx)
{
	macsurf_js__console_emit(ctx, "[js:debug]");
	return 0;
}

/* fixes319 (#116) — alert / confirm / prompt via Carbon StandardAlert.
 * V1 behaviour:
 *   - alert(msg)      → StandardAlert(kAlertNoteAlert, ...), blocks
 *   - confirm(msg)    → StandardAlert with OK/Cancel buttons; returns
 *                       true if user clicked OK, false otherwise.
 *   - prompt(msg, def) → V1 returns the default (or null if none).
 *                       Real text-input prompt needs a custom DLOG
 *                       resource + ModalDialog plumbing; deferred.
 *
 * String input is converted to MacRoman (no UTF-8 in StandardAlert).
 * Length is clamped to 255 chars (Pascal string limit). */
#ifdef __MACOS9__
extern size_t macos9_utf8_to_macroman(const char *utf8, size_t len,
		char *mac_out, size_t max_out);

static void macsurf_js__cstr_to_pstr(const char *src, unsigned char *dst,
		size_t cap)
{
	size_t len = strlen(src);
	if (len > 255) len = 255;
	if (len > cap - 1) len = cap - 1;
	dst[0] = (unsigned char)len;
	{
		size_t i;
		for (i = 0; i < len; i++) dst[1 + i] = (unsigned char)src[i];
	}
}
#endif

static duk_ret_t native_alert(duk_context *ctx)
{
#ifdef __MACOS9__
	const char *msg = duk_safe_to_string(ctx, 0);
	char macroman[256];
	unsigned char pstr[256];
	short item;
	macos9_utf8_to_macroman(msg, strlen(msg), macroman, sizeof macroman);
	macsurf_js__cstr_to_pstr(macroman, pstr, sizeof pstr);
	StandardAlert(kAlertNoteAlert, pstr, "\p", NULL, &item);
#else
	const char *msg = duk_safe_to_string(ctx, 0);
	MS_LOG(msg);
#endif
	return 0;
}

static duk_ret_t native_confirm(duk_context *ctx)
{
#ifdef __MACOS9__
	const char *msg = duk_safe_to_string(ctx, 0);
	char macroman[256];
	unsigned char pstr[256];
	short item;
	/* fixes320e (#116) — pass an AlertStdAlertParamRec so the dialog
	 * actually shows OK + Cancel. Without params, StandardAlert uses
	 * defaults which is OK-only for both Note and Caution kinds. */
	AlertStdAlertParamRec params;
	macos9_utf8_to_macroman(msg, strlen(msg), macroman, sizeof macroman);
	macsurf_js__cstr_to_pstr(macroman, pstr, sizeof pstr);
	params.movable      = false;
	params.helpButton   = false;
	params.filterProc   = NULL;
	params.defaultText  = (StringPtr)"\pOK";
	params.cancelText   = (StringPtr)"\pCancel";
	params.otherText    = NULL;
	params.defaultButton = kAlertStdAlertOKButton;
	params.cancelButton  = kAlertStdAlertCancelButton;
	params.position     = kWindowDefaultPosition;
	StandardAlert(kAlertCautionAlert, pstr, "\p", &params, &item);
	/* Item 1 = OK, 2 = Cancel. */
	duk_push_boolean(ctx, item == kAlertStdAlertOKButton ? 1 : 0);
#else
	(void)ctx;
	duk_push_boolean(ctx, 0);
#endif
	return 1;
}

static duk_ret_t native_prompt(duk_context *ctx)
{
	/* V1: return the default value if supplied, else null. Full text
	 * input prompt deferred to a later round (needs DLOG resource +
	 * TextEdit + ModalDialog). */
	if (duk_get_top(ctx) >= 2 && !duk_is_undefined(ctx, 1)) {
		duk_dup(ctx, 1);
	} else {
		duk_push_null(ctx);
	}
	return 1;
}

/* fixes319 (#121) — atob / btoa via Duktape's duk_base64_*.
 * encodeURI / decodeURI / encodeURIComponent / decodeURIComponent are
 * provided by Duktape natively (DUK_USE_ENCODING_BUILTINS=1 in
 * duk_config.h). */
static duk_ret_t native_btoa(duk_context *ctx)
{
	duk_base64_encode(ctx, 0);
	return 1;
}

static duk_ret_t native_atob(duk_context *ctx)
{
	duk_base64_decode(ctx, 0);
	/* duk_base64_decode leaves a buffer on the stack; convert to
	 * binary string for compatibility with the HTML5 atob spec. */
	{
		duk_size_t blen = 0;
		const char *bytes = (const char *)duk_get_buffer(ctx, -1,
				&blen);
		if (bytes != NULL) {
			duk_push_lstring(ctx, bytes, blen);
			duk_remove(ctx, -2);
		}
	}
	return 1;
}

/* fixes319 (#120) — navigator object with userAgent, appVersion,
 * platform. Tracks the User-Agent string emitted by the HTTPS fetcher
 * so author scripts that sniff by UA see the same identity the network
 * layer is sending.
 *
 * MacSurf's UA is `MacSurf/<ver> (Macintosh; PPC Mac OS 9)` per the v1.x
 * release notes. Hardcoded here; future rounds may parameterise. */
#define MACSURF_JS_UA      "MacSurf/1.3 (Macintosh; PPC Mac OS 9)"
#define MACSURF_JS_PLATFORM "MacPPC"
#define MACSURF_JS_APPVER   "5.0 (Macintosh; PPC Mac OS 9)"

/* fixes319 (#119) — document.title getter / setter.
 * Getter reads through the active gui_window's title via NetSurf's
 * content_get_title; setter calls macos9_gw_set_title. The
 * document object lives elsewhere (macsurf_js_dom.c) and may already
 * have a .title slot; we hook into the global `document` if one is
 * present, otherwise we create a minimal one. */
#ifdef __MACOS9__
extern struct gui_window *initial_win;
extern void macos9_gw_set_title(struct gui_window *gw, const char *title);
#endif

static duk_ret_t native_document_title_get(duk_context *ctx)
{
	/* MVP: return empty string. Wiring through to NetSurf's
	 * content_get_title requires a per-thread browser-window pointer
	 * which we don't currently stash on the duk_context. The setter
	 * (below) does work via initial_win. Improving the getter is a
	 * follow-up. */
	duk_push_string(ctx, "");
	return 1;
}

static duk_ret_t native_document_title_set(duk_context *ctx)
{
#ifdef __MACOS9__
	const char *title = duk_safe_to_string(ctx, 0);
	if (initial_win != NULL && title != NULL) {
		macos9_gw_set_title(initial_win, title);
	}
#else
	(void)ctx;
#endif
	return 0;
}

/* Master registration: install all globals onto the heap's context.
 * Replaces the original register_console_log. */
static void register_browser_globals(duk_context *ctx)
{
	duk_push_global_object(ctx);

	/* console */
	duk_push_object(ctx);
	duk_push_c_function(ctx, native_console_log, DUK_VARARGS);
	duk_put_prop_string(ctx, -2, "log");
	duk_push_c_function(ctx, native_console_warn, DUK_VARARGS);
	duk_put_prop_string(ctx, -2, "warn");
	duk_push_c_function(ctx, native_console_error, DUK_VARARGS);
	duk_put_prop_string(ctx, -2, "error");
	duk_push_c_function(ctx, native_console_info, DUK_VARARGS);
	duk_put_prop_string(ctx, -2, "info");
	duk_push_c_function(ctx, native_console_debug, DUK_VARARGS);
	duk_put_prop_string(ctx, -2, "debug");
	duk_put_prop_string(ctx, -2, "console");

	/* alert / confirm / prompt */
	duk_push_c_function(ctx, native_alert, 1);
	duk_put_prop_string(ctx, -2, "alert");
	duk_push_c_function(ctx, native_confirm, 1);
	duk_put_prop_string(ctx, -2, "confirm");
	duk_push_c_function(ctx, native_prompt, 2);
	duk_put_prop_string(ctx, -2, "prompt");

	/* atob / btoa (encodeURI etc. are Duktape built-ins) */
	duk_push_c_function(ctx, native_btoa, 1);
	duk_put_prop_string(ctx, -2, "btoa");
	duk_push_c_function(ctx, native_atob, 1);
	duk_put_prop_string(ctx, -2, "atob");

	/* fixes321 (#103) — setTimeout / setInterval / clearTimeout /
	 * clearInterval. Backed by macsurf_js_timers.c. clearInterval is
	 * an alias for clearTimeout (the per-timer "repeating" flag in
	 * the arena distinguishes; clearing by id works for either). */
	{
		extern duk_ret_t macsurf_js_settimeout(duk_context *duk);
		extern duk_ret_t macsurf_js_setinterval(duk_context *duk);
		extern duk_ret_t macsurf_js_cleartimeout(duk_context *duk);
		duk_push_c_function(ctx, macsurf_js_settimeout, 2);
		duk_put_prop_string(ctx, -2, "setTimeout");
		duk_push_c_function(ctx, macsurf_js_setinterval, 2);
		duk_put_prop_string(ctx, -2, "setInterval");
		duk_push_c_function(ctx, macsurf_js_cleartimeout, 1);
		duk_put_prop_string(ctx, -2, "clearTimeout");
		duk_push_c_function(ctx, macsurf_js_cleartimeout, 1);
		duk_put_prop_string(ctx, -2, "clearInterval");
	}

	/* fixes322 (#122) — requestAnimationFrame. Falls back to
	 * setTimeout(fn, 16) ≈ 60fps. cancelAnimationFrame is an alias
	 * for clearTimeout since both share id space. */
	{
		extern duk_ret_t macsurf_js_settimeout(duk_context *duk);
		extern duk_ret_t macsurf_js_cleartimeout(duk_context *duk);
		macsurf_js__safe_eval(ctx,
			"function requestAnimationFrame(fn){"
				"return setTimeout(fn,16);"
			"}"
			"function cancelAnimationFrame(id){"
				"clearTimeout(id);"
			"}");
		(void)macsurf_js_settimeout; (void)macsurf_js_cleartimeout;
	}

	/* fixes323 (#117) — window.location. Read returns the current
	 * URL via the bw stored in win_priv. Write triggers a navigation.
	 * Reload via location.reload(). */
	{
		extern duk_ret_t macsurf_js_location_get(duk_context *duk);
		extern duk_ret_t macsurf_js_location_set(duk_context *duk);
		extern duk_ret_t macsurf_js_location_reload(duk_context *duk);
		duk_push_object(ctx); /* location */
		duk_push_string(ctx, "href");
		duk_push_c_function(ctx, macsurf_js_location_get, 0);
		duk_push_c_function(ctx, macsurf_js_location_set, 1);
		duk_def_prop(ctx, -4,
			DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
			DUK_DEFPROP_SET_CONFIGURABLE);
		duk_push_c_function(ctx, macsurf_js_location_reload, 0);
		duk_put_prop_string(ctx, -2, "reload");
		duk_put_prop_string(ctx, -2, "location");
	}

	/* fixes350 (#118) — add the rest of the Location interface
	 * properties (protocol, host, hostname, port, pathname, search,
	 * hash, origin) by URL-parsing the href getter. Implemented in JS
	 * because the existing href C-getter already routes through the
	 * bw and we want every read to reflect the LIVE current URL. */
	macsurf_js__safe_eval(ctx,
		"(function(){"
		"  if(typeof location==='undefined')return;"
		"  function parse(){"
		"    var s=location.href||'';"
		"    var p={protocol:'',host:'',hostname:'',port:'',"
		"           pathname:'',search:'',hash:'',origin:''};"
		"    var hi=s.indexOf('#');"
		"    if(hi>=0){p.hash=s.substring(hi);s=s.substring(0,hi);}"
		"    var qi=s.indexOf('?');"
		"    if(qi>=0){p.search=s.substring(qi);s=s.substring(0,qi);}"
		"    var ci=s.indexOf('://');"
		"    var rest=s;"
		"    if(ci>=0){p.protocol=s.substring(0,ci)+':';rest=s.substring(ci+3);}"
		"    var pi=rest.indexOf('/');"
		"    var authority=pi>=0?rest.substring(0,pi):rest;"
		"    p.pathname=pi>=0?rest.substring(pi):'/';"
		"    p.host=authority;"
		"    var coli=authority.indexOf(':');"
		"    if(coli>=0){p.hostname=authority.substring(0,coli);"
		"      p.port=authority.substring(coli+1);}"
		"    else{p.hostname=authority;}"
		"    p.origin=p.protocol+'//'+p.host;"
		"    return p;"
		"  }"
		"  ['protocol','host','hostname','port','pathname','search',"
		"   'hash','origin'].forEach(function(k){"
		"    Object.defineProperty(location,k,{"
		"      get:function(){return parse()[k];},"
		"      configurable:true"
		"    });"
		"  });"
		"  location.assign=location.assign||function(u){location.href=u;};"
		"  location.replace=location.replace||function(u){location.href=u;};"
		"  location.toString=location.toString||function(){return location.href;};"
		"})();");

	/* fixes339 — Event + CustomEvent constructors. Common pattern is
	 * `new Event('click')` or `new CustomEvent('foo', {detail:bar})`.
	 * Required by many scripts for dispatchEvent. */
	macsurf_js__safe_eval(ctx,
		"function Event(type,opts){"
			"opts=opts||{};"
			"this.type=String(type);"
			"this.bubbles=!!opts.bubbles;"
			"this.cancelable=!!opts.cancelable;"
			"this.composed=!!opts.composed;"
			"this.defaultPrevented=false;"
			"this.target=null;this.currentTarget=null;"
			"this.preventDefault=function(){this.defaultPrevented=true;};"
			"this.stopPropagation=function(){};"
			"this.stopImmediatePropagation=function(){};"
		"}"
		"this.Event=Event;"
		"function CustomEvent(type,opts){"
			"Event.call(this,type,opts);"
			"this.detail=opts&&opts.detail!==undefined?opts.detail:null;"
		"}"
		"CustomEvent.prototype=Object.create(Event.prototype);"
		"this.CustomEvent=CustomEvent;"
		"function MouseEvent(type,opts){Event.call(this,type,opts);"
			"opts=opts||{};"
			"this.clientX=opts.clientX||0;this.clientY=opts.clientY||0;"
			"this.screenX=opts.screenX||0;this.screenY=opts.screenY||0;"
			"this.button=opts.button||0;"
			"this.shiftKey=!!opts.shiftKey;"
			"this.ctrlKey=!!opts.ctrlKey;this.altKey=!!opts.altKey;"
			"this.metaKey=!!opts.metaKey;"
		"}"
		"MouseEvent.prototype=Object.create(Event.prototype);"
		"this.MouseEvent=MouseEvent;"
		"function KeyboardEvent(type,opts){Event.call(this,type,opts);"
			"opts=opts||{};"
			"this.key=opts.key||'';this.code=opts.code||'';"
			"this.keyCode=opts.keyCode||0;this.which=opts.which||0;"
			"this.shiftKey=!!opts.shiftKey;this.ctrlKey=!!opts.ctrlKey;"
			"this.altKey=!!opts.altKey;this.metaKey=!!opts.metaKey;"
		"}"
		"KeyboardEvent.prototype=Object.create(Event.prototype);"
		"this.KeyboardEvent=KeyboardEvent;");

	/* fixes341 — document shims for commonly-used properties. The DOM
	 * bindings already provide getElementById/createElement/querySelector;
	 * we add stubs for the remaining ones so site code that walks them
	 * doesn't ReferenceError. */
	macsurf_js__safe_eval(ctx,
		"if(typeof document!=='undefined'){"
			"document.body=document.body||null;"
			"document.head=document.head||null;"
			"document.documentElement=document.documentElement||null;"
			"document.readyState='complete';"
			"document.cookie='';"
			"document.URL=document.URL||(typeof location!=='undefined'?location.href:'');"
			"document.referrer='';"
			"document.domain='';"
			"document.createTextNode=document.createTextNode||function(t){"
				"return {nodeValue:String(t),textContent:String(t),"
					"appendChild:function(){},data:String(t)};};"
			"document.createDocumentFragment=function(){"
				"return {appendChild:function(){},childNodes:[],"
					"firstChild:null,lastChild:null};};"
			"document.getElementsByTagName=document.getElementsByTagName||"
				"function(){return [];};"
			"document.getElementsByClassName=document.getElementsByClassName||"
				"function(){return [];};"
			"document.getElementsByName=document.getElementsByName||"
				"function(){return [];};"
			"document.querySelectorAll=document.querySelectorAll||"
				"function(){return [];};"
			"document.addEventListener=document.addEventListener||"
				"function(t,fn){"
					"if(!document._listeners)document._listeners={};"
					"if(!document._listeners[t])document._listeners[t]=[];"
					"document._listeners[t].push(fn);};"
			"document.removeEventListener=document.removeEventListener||"
				"function(t,fn){"
					"var L=document._listeners&&document._listeners[t];if(!L)return;"
					"for(var i=0;i<L.length;i++)if(L[i]===fn){L.splice(i,1);return;}};"
			"document.dispatchEvent=document.dispatchEvent||"
				"function(ev){"
					"var L=document._listeners&&document._listeners[ev&&ev.type];"
					"if(L)L.forEach(function(f){try{f(ev);}catch(e){}});return true;};"
		"}");

	macsurf_js__safe_eval(ctx,
		"if(typeof navigator!=='undefined'){"
			"navigator.cookieEnabled=false;"
			"navigator.onLine=true;"
			"navigator.languages=navigator.languages||[navigator.language||'en-US'];"
			"navigator.doNotTrack='1';"
			"navigator.connection={effectiveType:'3g',downlink:1.5,rtt:300};"
			"navigator.hardwareConcurrency=1;"
			"navigator.deviceMemory=0.5;"
			"navigator.vendor='Anthropic/MPLS';"
			"navigator.product='MacSurf';"
			"navigator.productSub='20260531';"
			"navigator.javaEnabled=function(){return false;};"
			"navigator.sendBeacon=function(){return false;};"
		"}");

	/* fixes340 — MutationObserver / IntersectionObserver / ResizeObserver
	 * stubs. Many sites guard their setup with `if(MutationObserver)`.
	 * The stub satisfies the guard; observe() is a no-op so the page
	 * doesn't pile up callbacks on a non-firing observer. */
	macsurf_js__safe_eval(ctx,
		"function _Observer(cb){this._cb=cb;}"
		"_Observer.prototype.observe=function(){};"
		"_Observer.prototype.unobserve=function(){};"
		"_Observer.prototype.disconnect=function(){};"
		"_Observer.prototype.takeRecords=function(){return [];};"
		"this.MutationObserver=_Observer;"
		"this.IntersectionObserver=_Observer;"
		"this.ResizeObserver=_Observer;"
		"this.PerformanceObserver=_Observer;");

	/* fixes338 — common window properties + event helpers. Stubs
	 * for the most-feature-detected window APIs:
	 *   - scrollTo / scrollBy / scroll
	 *   - addEventListener / removeEventListener (window-level)
	 *   - getComputedStyle (returns empty object with getPropertyValue)
	 *   - matchMedia (returns matches:false)
	 *   - dispatchEvent
	 *   - requestIdleCallback / cancelIdleCallback
	 *   - innerWidth / innerHeight / outerWidth / outerHeight
	 *   - scrollY / scrollX / pageYOffset / pageXOffset
	 */
	macsurf_js__safe_eval(ctx,
		"this.scrollTo=function(){};"
		"this.scrollBy=function(){};"
		"this.scroll=function(){};"
		"this._winListeners={};"
		"this.addEventListener=function(t,fn){"
			"if(!this._winListeners[t])this._winListeners[t]=[];"
			"this._winListeners[t].push(fn);};"
		"this.removeEventListener=function(t,fn){"
			"var arr=this._winListeners[t];if(!arr)return;"
			"for(var i=0;i<arr.length;i++)if(arr[i]===fn){arr.splice(i,1);return;}};"
		"this.dispatchEvent=function(ev){"
			"var t=ev&&ev.type;var arr=t&&this._winListeners[t];"
			"if(arr)arr.forEach(function(f){try{f(ev);}catch(e){}});return true;};"
		"this.getComputedStyle=function(el){"
			"return {"
				"getPropertyValue:function(p){"
					"if(el&&el.style&&el.style.getPropertyValue)"
						"return el.style.getPropertyValue(p);"
					"return '';},"
				"cssText:''"
			"};};"
		"this.matchMedia=function(q){"
			"return {matches:false,media:q||'',"
				"addListener:function(){},removeListener:function(){},"
				"addEventListener:function(){},removeEventListener:function(){}};};"
		"this.requestIdleCallback=function(fn){return setTimeout(fn,0);};"
		"this.cancelIdleCallback=function(id){clearTimeout(id);};"
		"this.innerWidth=949;this.innerHeight=613;"
		"this.outerWidth=949;this.outerHeight=613;"
		"this.scrollY=0;this.scrollX=0;"
		"this.pageYOffset=0;this.pageXOffset=0;"
		"this.devicePixelRatio=1;"
		"this.screen={width:1024,height:768,availWidth:1024,availHeight:740,colorDepth:24};"
		"this.performance={now:function(){return Date.now();}};"
		"this.Promise=this.Promise||function(executor){"
			"var self=this;self._then=[];self._catch=[];"
			"self.then=function(cb){self._then.push(cb);return self;};"
			"self.catch=function(cb){self._catch.push(cb);return self;};"
			"function resolve(v){self._then.forEach(function(f){try{f(v);}catch(e){}});}"
			"function reject(e){self._catch.forEach(function(f){try{f(e);}catch(_){}});}"
			"try{executor(resolve,reject);}catch(e){reject(e);}"
		"};");

	/* fixes331 (#125) — DOMParser. V1 returns a plain object with
	 * a single nominal document property. Real DOM tree construction
	 * from a string would require libhubbub-on-string + dom_document
	 * wrapper, deferred. Many scripts feature-check DOMParser via
	 * `typeof DOMParser !== 'undefined'`; this satisfies that. */
	macsurf_js__safe_eval(ctx,
		"function DOMParser(){}"
		"DOMParser.prototype.parseFromString=function(s,m){"
			"return {body:{innerHTML:s||''},documentElement:null,"
				"querySelector:function(){return null;},"
				"querySelectorAll:function(){return [];},"
				"getElementById:function(){return null;},"
				"getElementsByTagName:function(){return [];}};"
		"};");

	/* fixes332 (#124) — FormData. Stores key/value pairs and exposes
	 * append / get / set / has / delete / forEach. Lacks file-input
	 * iteration; future work when multipart/form-data is supported
	 * end-to-end. */
	macsurf_js__safe_eval(ctx,
		"function FormData(form){"
			"this._k=[];this._v=[];"
			"if(form&&form.elements){"
				"var els=form.elements;"
				"for(var i=0;i<els.length;i++){"
					"var e=els[i];"
					"if(e.name&&e.value!==undefined){"
						"this._k.push(e.name);"
						"this._v.push(e.value);"
					"}"
				"}"
			"}"
		"}"
		"FormData.prototype.append=function(k,v){"
			"this._k.push(String(k));this._v.push(String(v));};"
		"FormData.prototype.get=function(k){"
			"for(var i=0;i<this._k.length;i++)"
				"if(this._k[i]==k)return this._v[i];"
			"return null;};"
		"FormData.prototype.getAll=function(k){"
			"var r=[];for(var i=0;i<this._k.length;i++)"
				"if(this._k[i]==k)r.push(this._v[i]);return r;};"
		"FormData.prototype.set=function(k,v){"
			"this.delete(k);this.append(k,v);};"
		"FormData.prototype.has=function(k){return this.get(k)!==null;};"
		"FormData.prototype.delete=function(k){"
			"for(var i=this._k.length-1;i>=0;i--)"
				"if(this._k[i]==k){this._k.splice(i,1);this._v.splice(i,1);}};"
		"FormData.prototype.forEach=function(cb){"
			"for(var i=0;i<this._k.length;i++)cb(this._v[i],this._k[i],this);};");

	/* fixes334 (#104) — minimal fetch() shim wrapping XMLHttpRequest.
	 * Returns a thenable (not a real Promise — Duktape is ES5). The
	 * .then(cb) fires synchronously when the XHR completes; .catch is
	 * supported. Sufficient for fetch(url).then(r=>r.text()).then(...)
	 * patterns common in inline scripts. */
	macsurf_js__safe_eval(ctx,
		"this.fetch=function(url,opts){"
			"opts=opts||{};"
			"var ok=false,status=0,respText='',respHeaders='';"
			"try{"
				"var xhr=new XMLHttpRequest();"
				"xhr.open(opts.method||'GET',url,false);"
				"if(opts.headers){"
					"for(var h in opts.headers)"
						"xhr.setRequestHeader(h,opts.headers[h]);"
				"}"
				"xhr.send(opts.body||null);"
				"status=xhr.status;"
				"ok=status>=200&&status<300;"
				"respText=xhr.responseText||'';"
				"respHeaders=xhr.getAllResponseHeaders?xhr.getAllResponseHeaders():'';"
			"}catch(e){}"
			"var resp={"
				"ok:ok,status:status,"
				"text:function(){"
					"var t={then:function(cb){cb(respText);return t;}};return t;},"
				"json:function(){"
					"var j={then:function(cb){"
						"try{cb(JSON.parse(respText));}catch(_){}return j;}};return j;}"
			"};"
			"var thenable={"
				"then:function(cb){if(cb)cb(resp);return thenable;},"
				"catch:function(){return thenable;}"
			"};"
			"return thenable;"
		"};");

	/* fixes333 (#46) — localStorage / sessionStorage shim. V1 is
	 * RAM-only (lost on quit). Future round persists localStorage to
	 * a file in the Preferences folder. */
	macsurf_js__safe_eval(ctx,
		"function _Storage(){this._m={};}"
		"_Storage.prototype.getItem=function(k){"
			"return k in this._m?this._m[k]:null;};"
		"_Storage.prototype.setItem=function(k,v){this._m[k]=String(v);};"
		"_Storage.prototype.removeItem=function(k){delete this._m[k];};"
		"_Storage.prototype.clear=function(){this._m={};};"
		"_Storage.prototype.key=function(i){"
			"var ks=Object.keys(this._m);return ks[i]||null;};"
		"Object.defineProperty(_Storage.prototype,'length',{"
			"get:function(){return Object.keys(this._m).length;}});"
		"this.localStorage=new _Storage();"
		"this.sessionStorage=new _Storage();");

	/* fixes325 (#123) — URL / URLSearchParams. Minimal in-language
	 * implementations sufficient for `new URL(str)` access to
	 * .href / .pathname / .search / .hash / .host / .protocol and
	 * `new URLSearchParams(str)` with .get / .set / .has / .toString.
	 * Full WHATWG URL parsing isn't needed for the common usage. */
	macsurf_js__safe_eval(ctx,
		"(function(){"
		"function _parseURL(u){"
			"var s=String(u);"
			"var m=s.match(/^([a-z][a-z0-9+\\-.]*):/i);"
			"var proto=m?m[1]+':':'';"
			"var rest=m?s.substr(m[0].length):s;"
			"var hash='';var search='';var host='';var path='';"
			"var h=rest.indexOf('#');"
			"if(h>=0){hash=rest.substr(h);rest=rest.substr(0,h);}"
			"var q=rest.indexOf('?');"
			"if(q>=0){search=rest.substr(q);rest=rest.substr(0,q);}"
			"if(rest.indexOf('//')==0){"
				"rest=rest.substr(2);"
				"var p=rest.indexOf('/');"
				"if(p>=0){host=rest.substr(0,p);path=rest.substr(p);}"
				"else{host=rest;path='';}"
			"}else{path=rest;}"
			"return {protocol:proto,host:host,pathname:path,"
				"search:search,hash:hash};"
		"}"
		"function URL(u,base){"
			"var s=String(u);"
			"if(base){"
				"if(s.indexOf('://')<0&&s.charAt(0)!='/'){"
					"var b=_parseURL(base);"
					"s=b.protocol+'//'+b.host+'/'+s;"
				"}else if(s.charAt(0)=='/'){"
					"var b2=_parseURL(base);"
					"s=b2.protocol+'//'+b2.host+s;"
				"}"
			"}"
			"var p=_parseURL(s);"
			"this.href=s;this.protocol=p.protocol;this.host=p.host;"
			"this.hostname=p.host.split(':')[0];"
			"this.pathname=p.pathname;this.search=p.search;"
			"this.hash=p.hash;"
		"}"
		"URL.prototype.toString=function(){return this.href;};"
		"this.URL=URL;"
		"function URLSearchParams(init){"
			"this._m={};"
			"if(init){"
				"var s=String(init);"
				"if(s.charAt(0)=='?')s=s.substr(1);"
				"var parts=s.split('&');"
				"for(var i=0;i<parts.length;i++){"
					"if(!parts[i])continue;"
					"var eq=parts[i].indexOf('=');"
					"var k=eq>=0?parts[i].substr(0,eq):parts[i];"
					"var v=eq>=0?parts[i].substr(eq+1):'';"
					"k=decodeURIComponent(k);"
					"v=decodeURIComponent(v);"
					"this._m[k]=v;"
				"}"
			"}"
		"}"
		"URLSearchParams.prototype.get=function(k){"
			"return this._m[k]!==undefined?this._m[k]:null;};"
		"URLSearchParams.prototype.set=function(k,v){this._m[k]=String(v);};"
		"URLSearchParams.prototype.has=function(k){return k in this._m;};"
		"URLSearchParams.prototype.delete=function(k){delete this._m[k];};"
		"URLSearchParams.prototype.toString=function(){"
			"var out=[];"
			"for(var k in this._m){"
				"out.push(encodeURIComponent(k)+'='+encodeURIComponent(this._m[k]));"
			"}return out.join('&');};"
		"this.URLSearchParams=URLSearchParams;"
		"}).call(this);");

	/* fixes324 (#118) — window.history. back / forward go through
	 * the same gui_window list as macos9_window_back / _forward.
	 * fixes350 (#122) — add pushState / replaceState / state stubs so
	 * SPA libraries that feature-detect them stop bailing. We don't
	 * yet have a real session-history model to update so they're
	 * no-ops; if/when a router actually drives navigation through
	 * them we'll route the URL to location.assign. */
	{
		extern duk_ret_t macsurf_js_history_back(duk_context *duk);
		extern duk_ret_t macsurf_js_history_forward(duk_context *duk);
		extern duk_ret_t macsurf_js_history_go(duk_context *duk);
		duk_push_object(ctx); /* history */
		duk_push_c_function(ctx, macsurf_js_history_back, 0);
		duk_put_prop_string(ctx, -2, "back");
		duk_push_c_function(ctx, macsurf_js_history_forward, 0);
		duk_put_prop_string(ctx, -2, "forward");
		duk_push_c_function(ctx, macsurf_js_history_go, 1);
		duk_put_prop_string(ctx, -2, "go");
		duk_push_int(ctx, 0); /* length — placeholder until we
		                        plumb history list size */
		duk_put_prop_string(ctx, -2, "length");
		duk_put_prop_string(ctx, -2, "history");
	}
	/* fixes350 (#122) — pushState / replaceState / state on history.
	 * Done in JS so the closure can capture the latest state object
	 * across calls without C-side storage. */
	macsurf_js__safe_eval(ctx,
		"(function(){"
		"  if(typeof history==='undefined')return;"
		"  var _state=null;"
		"  history.pushState=history.pushState||function(s,t,u){"
		"    _state=s;"
		"    if(u&&typeof location!=='undefined')location.href=u;"
		"  };"
		"  history.replaceState=history.replaceState||function(s,t,u){"
		"    _state=s;"
		"    if(u&&typeof location!=='undefined')location.href=u;"
		"  };"
		"  Object.defineProperty(history,'state',{"
		"    get:function(){return _state;},"
		"    configurable:true"
		"  });"
		"  history.scrollRestoration=history.scrollRestoration||'auto';"
		"})();");

	/* navigator */
	duk_push_object(ctx);
	duk_push_string(ctx, MACSURF_JS_UA);
	duk_put_prop_string(ctx, -2, "userAgent");
	duk_push_string(ctx, MACSURF_JS_APPVER);
	duk_put_prop_string(ctx, -2, "appVersion");
	duk_push_string(ctx, MACSURF_JS_PLATFORM);
	duk_put_prop_string(ctx, -2, "platform");
	duk_push_string(ctx, "Netscape");
	duk_put_prop_string(ctx, -2, "appName");
	duk_push_string(ctx, "en-US");
	duk_put_prop_string(ctx, -2, "language");
	duk_put_prop_string(ctx, -2, "navigator");

	/* fixes320e — document.title is now owned by macsurf_document_set_title
	 * in macsurf_js_dom.c (fixes320c wired it through macos9_gw_set_title,
	 * fixes320d added logging). Previously we tried to override the
	 * accessor here, which silently overrode the macsurf_document_set_title
	 * installed by macsurf_js_setup_globals — but our redefine call
	 * had no logging and used different DUK_DEFPROP flags, so the title
	 * setter that actually ran wasn't doing the chrome update. Drop the
	 * override and let setup_globals' setter run as-is. */

	/* fixes377 (#167) — FB-runtime polyfills + module loader. The JS-on
	 * runs showed Facebook's scripts dying on: requireLazy/__d/require
	 * (FB's module system, the keystone), Set, Map, Array.from, Image.
	 * These are standard ES6 builtins plus FB's loader, all fillable
	 * in-house (DIRECTIVE #2 — don't route around the engine, fill it).
	 * ES5-only, no-throw, behaviour-verified against node. Installed onto
	 * the global (g=this) here, AFTER navigator exists, so the sendBeacon
	 * ensure can attach. Runs before any page <script>. */
	macsurf_js__safe_eval(ctx,
		"(function(g){"
		"if(!Array.from){"
		"Array.from=function(src,mapFn,thisArg){"
		"var out=[],i,n,v,idx;"
		"if(src==null)return out;"
		"if(typeof src.length==='number'){"
		"n=src.length>>>0;"
		"for(i=0;i<n;i++){v=src[i];out.push(mapFn?mapFn.call(thisArg,v,i):v);}"
		"}else if(typeof src.forEach==='function'){"
		"idx=0;src.forEach(function(v){out.push(mapFn?mapFn.call(thisArg,v,idx):v);idx++;});"
		"}"
		"return out;"
		"};"
		"}"
		"if(!Array.of){Array.of=function(){return Array.prototype.slice.call(arguments);};}"
		"if(!Array.prototype.includes){Array.prototype.includes=function(x){return this.indexOf(x)>=0;};}"
		"if(!Array.prototype.find){Array.prototype.find=function(f,t){var i;for(i=0;i<this.length;i++){if(f.call(t,this[i],i,this))return this[i];}return undefined;};}"
		"if(!Array.prototype.findIndex){Array.prototype.findIndex=function(f,t){var i;for(i=0;i<this.length;i++){if(f.call(t,this[i],i,this))return i;}return -1;};}"
		"if(!Object.assign){"
		"Object.assign=function(target){"
		"var i,k,s;"
		"for(i=1;i<arguments.length;i++){s=arguments[i];if(s==null)continue;for(k in s){if(Object.prototype.hasOwnProperty.call(s,k))target[k]=s[k];}}"
		"return target;"
		"};"
		"}"
		"if(typeof g.Set==='undefined'){"
		"var MSet=function(it){this._k=[];this.size=0;var self=this;if(it){if(typeof it.forEach==='function')it.forEach(function(v){self.add(v);});else if(typeof it.length==='number'){var i;for(i=0;i<it.length;i++)self.add(it[i]);}}};"
		"MSet.prototype.add=function(v){if(this._k.indexOf(v)<0){this._k.push(v);this.size=this._k.length;}return this;};"
		"MSet.prototype.has=function(v){return this._k.indexOf(v)>=0;};"
		"MSet.prototype['delete']=function(v){var i=this._k.indexOf(v);if(i<0)return false;this._k.splice(i,1);this.size=this._k.length;return true;};"
		"MSet.prototype.clear=function(){this._k=[];this.size=0;};"
		"MSet.prototype.forEach=function(cb,t){var i;for(i=0;i<this._k.length;i++)cb.call(t,this._k[i],this._k[i],this);};"
		"MSet.prototype.values=function(){return this._k.slice();};"
		"MSet.prototype.keys=MSet.prototype.values;"
		"g.Set=MSet;"
		"if(typeof g.WeakSet==='undefined')g.WeakSet=MSet;"
		"}"
		"if(typeof g.Map==='undefined'){"
		"var MMap=function(it){this._k=[];this._v=[];this.size=0;var self=this;if(it){if(typeof it.forEach==='function')it.forEach(function(p){self.set(p[0],p[1]);});else if(typeof it.length==='number'){var i;for(i=0;i<it.length;i++)self.set(it[i][0],it[i][1]);}}};"
		"MMap.prototype.set=function(k,v){var i=this._k.indexOf(k);if(i<0){this._k.push(k);this._v.push(v);this.size=this._k.length;}else{this._v[i]=v;}return this;};"
		"MMap.prototype.get=function(k){var i=this._k.indexOf(k);return i<0?undefined:this._v[i];};"
		"MMap.prototype.has=function(k){return this._k.indexOf(k)>=0;};"
		"MMap.prototype['delete']=function(k){var i=this._k.indexOf(k);if(i<0)return false;this._k.splice(i,1);this._v.splice(i,1);this.size=this._k.length;return true;};"
		"MMap.prototype.clear=function(){this._k=[];this._v=[];this.size=0;};"
		"MMap.prototype.forEach=function(cb,t){var i;for(i=0;i<this._k.length;i++)cb.call(t,this._v[i],this._k[i],this);};"
		"MMap.prototype.keys=function(){return this._k.slice();};"
		"MMap.prototype.values=function(){return this._v.slice();};"
		"g.Map=MMap;"
		"if(typeof g.WeakMap==='undefined')g.WeakMap=MMap;"
		"}"
		"if(typeof g.Image==='undefined'){"
		"var MImage=function(w,h){this.width=w||0;this.height=h||0;this.naturalWidth=0;this.naturalHeight=0;this.complete=false;this.src='';this.onload=null;this.onerror=null;this.style={};};"
		"MImage.prototype.setAttribute=function(){};"
		"MImage.prototype.addEventListener=function(){};"
		"g.Image=MImage;"
		"}"
		"if(typeof g.navigator!=='undefined'&&typeof g.navigator.sendBeacon!=='function'){g.navigator.sendBeacon=function(){return false;};}"
		"if(typeof g.__d==='undefined'){"
		"var registry={},cache={};"
		"g.__d=function(name,deps,factory){"
		"if(typeof name==='function'){var f=name;name=deps;deps=factory;factory=f;}"
		"registry[name]={deps:(deps&&deps.length)?deps:[],factory:factory};"
		"};"
		"var _require=function(name){"
		"if(cache.hasOwnProperty(name))return cache[name].exports;"
		"var mod=registry[name];"
		"if(!mod){var stub={exports:{}};cache[name]=stub;return stub.exports;}"
		"var module={exports:{}};cache[name]=module;"
		"try{"
		"if(typeof mod.factory==='function'){"
		"var args=[g,_require,_require,g.requireLazy,module,module.exports],i;"
		"for(i=0;i<mod.deps.length;i++){try{args.push(_require(mod.deps[i]));}catch(e){args.push(undefined);}}"
		"mod.factory.apply(g,args);"
		"}"
		"}catch(e){}"
		"return module.exports;"
		"};"
		"g.require=g.require||_require;"
		"g.requireDynamic=_require;"
		"g.__r=_require;"
		"g.requireLazy=function(names,cb){"
		"var r=[],i;"
		"if(names&&names.length){for(i=0;i<names.length;i++){try{r.push(_require(names[i]));}catch(e){r.push(undefined);}}}"
		"if(typeof cb==='function'){try{cb.apply(null,r);}catch(e){}}"
		"return r;"
		"};"
		"}"
		"})(this);");

	duk_pop(ctx); /* pop global */
}

/* ------------------------------------------------------------------ */
/* fixes316 — NetSurf-API surface.                                    */
/* These names are the actual symbols NetSurf core links against from */
/* desktop/browser_window.c, content/handlers/html/script.c, etc.     */
/* Pre-fix316 these resolved to no-ops in js_stub.c; with the !WITH_  */
/* DUKTAPE gate in that file, the symbols now belong to us.           */
/* ------------------------------------------------------------------ */

/* fixes319b — minimal content_handler so the factory recognises
 * `text/javascript` (and friends) as CONTENT_JS. Without this,
 * select_script_handler() in script.c returns NULL on every <script>
 * tag and js_exec is never called. The official path in upstream
 * NetSurf is dukky.c → javascript_init() → CONTENT_FACTORY_REGISTER_TYPES
 * macro in js_content.c, but neither file is in MacSurf.mcp — that
 * was the missing link. We register the same MIME types here with a
 * stub handler whose only job is to make `type()` return CONTENT_JS;
 * inline scripts then route through exec_inline_script which reads
 * the source straight from the dom_string and never touches the
 * content_handler's other callbacks. */

#include "content/content_factory.h"
#include "content/content_protected.h"

static nserror macsurf_js__content_create(const struct content_handler *handler,
		lwc_string *imime_type,
		const struct http_parameter *params,
		struct llcache_handle *llcache, const char *fallback_charset,
		bool quirks, struct content **c)
{
	struct content *content;
	nserror error;
	(void)params;
	content = (struct content *)calloc(1, sizeof(struct content));
	if (content == NULL) return NSERROR_NOMEM;
	error = content__init(content, handler, imime_type, NULL, llcache,
			fallback_charset, quirks);
	if (error != NSERROR_OK) {
		free(content);
		return error;
	}
	*c = content;
	return NSERROR_OK;
}

static bool macsurf_js__content_convert(struct content *c)
{
	content_set_ready(c);
	content_set_done(c);
	return true;
}

static void macsurf_js__content_destroy(struct content *c)
{
	(void)c;
}

static nserror macsurf_js__content_clone(const struct content *old,
		struct content **newc)
{
	(void)old; (void)newc;
	return NSERROR_CLONE_FAILED;
}

static content_type macsurf_js__content_type(void)
{
	return CONTENT_JS;
}

static const struct content_handler macsurf_js__content_handler = {
	.create = macsurf_js__content_create,
	.data_complete = macsurf_js__content_convert,
	.destroy = macsurf_js__content_destroy,
	.clone = macsurf_js__content_clone,
	.type = macsurf_js__content_type,
	.no_share = false
};

static const char * const macsurf_js__content_types[] = {
	"application/javascript",
	"application/ecmascript",
	"application/x-javascript",
	"text/javascript",
	"text/ecmascript"
};

/* fixes323/324 (#117 #118) — window.location and window.history
 * native implementations. All route through the active gui_window's
 * bw pointer via macos9_window_list_head(). */
#ifdef __MACOS9__
extern struct gui_window *macos9_window_list_head(void);
extern struct browser_window *macos9_gw_bw(struct gui_window *g);
extern void macos9_window_navigate(struct gui_window *g, const char *url);
extern void macos9_window_back(struct gui_window *g);
extern void macos9_window_forward(struct gui_window *g);
extern void macos9_window_reload(struct gui_window *g);
#endif

duk_ret_t macsurf_js_location_get(duk_context *ctx)
{
#ifdef __MACOS9__
	struct gui_window *win = macos9_window_list_head();
	struct browser_window *bw = win ? macos9_gw_bw(win) : NULL;
	const char *href = "about:blank";
	if (bw != NULL) {
		extern struct nsurl *browser_window_access_url(
			const struct browser_window *bw);
		struct nsurl *u = browser_window_access_url(bw);
		if (u != NULL) {
			extern const char *nsurl_access(const struct nsurl *u);
			const char *s = nsurl_access(u);
			if (s != NULL) href = s;
		}
	}
	duk_push_string(ctx, href);
#else
	duk_push_string(ctx, "about:blank");
#endif
	return 1;
}

duk_ret_t macsurf_js_location_set(duk_context *ctx)
{
#ifdef __MACOS9__
	const char *url = duk_safe_to_string(ctx, 0);
	struct gui_window *win = macos9_window_list_head();
	if (win != NULL && url != NULL) {
		macos9_window_navigate(win, url);
	}
#else
	(void)ctx;
#endif
	return 0;
}

duk_ret_t macsurf_js_location_reload(duk_context *ctx)
{
#ifdef __MACOS9__
	struct gui_window *win = macos9_window_list_head();
	if (win != NULL) macos9_window_reload(win);
#else
	(void)ctx;
#endif
	return 0;
}

duk_ret_t macsurf_js_history_back(duk_context *ctx)
{
#ifdef __MACOS9__
	struct gui_window *win = macos9_window_list_head();
	if (win != NULL) macos9_window_back(win);
#else
	(void)ctx;
#endif
	return 0;
}

duk_ret_t macsurf_js_history_forward(duk_context *ctx)
{
#ifdef __MACOS9__
	struct gui_window *win = macos9_window_list_head();
	if (win != NULL) macos9_window_forward(win);
#else
	(void)ctx;
#endif
	return 0;
}

duk_ret_t macsurf_js_history_go(duk_context *ctx)
{
#ifdef __MACOS9__
	int delta = duk_to_int(ctx, 0);
	struct gui_window *win = macos9_window_list_head();
	if (win == NULL) return 0;
	while (delta < 0) { macos9_window_back(win); delta++; }
	while (delta > 0) { macos9_window_forward(win); delta--; }
#else
	(void)ctx;
#endif
	return 0;
}

/* fixes342 — safe JS-init eval helper. Any of the polyfill shims
 * below could fail to parse on an exotic Duktape build; without this
 * helper a single bad shim throws up through register_browser_globals
 * and breaks ALL bindings. peval + log + pop isolates per-shim.
 * Non-static so macsurf_js_dom.c can use it for its per-element
 * wrapper polyfill installs. */
void macsurf_js__safe_eval(duk_context *ctx, const char *src)
{
	if (duk_peval_string_noresult(ctx, src) != 0) {
		const char *err = duk_safe_to_string(ctx, -1);
		macsurf_debug_log_writef("js init eval failed: %s",
			err ? err : "(null)");
		duk_pop(ctx);
	}
}

/* fixes321 (#103) — pump bridge. macos9_poll calls this every event
 * loop tick. Builds a temporary jscontext from global_heap and fires
 * any due timers. No-op if no JS heap is alive yet. */
void macsurf_js_pump_timers(void)
{
	extern void macsurf_js_run_timers(struct jscontext *ctx);
	if (global_heap == NULL || global_heap->ctx == NULL) return;
	{
		struct jscontext tmp;
		tmp.duk = global_heap->ctx;
		tmp.win_priv = NULL;
		tmp.doc_priv = NULL;
		macsurf_js_run_timers(&tmp);
	}
}

/* fixes319g — link-stub for macsurf_js_dom.c's macsurf_console_log.
 * The DOM file's pre-existing single-stream console.log routes through
 * macsurf_js_console_append, which was never defined anywhere. Now
 * that fixes319f calls macsurf_js_setup_globals (which references
 * macsurf_console_log), the dead symbol becomes a live link error.
 *
 * In practice macsurf_console_log is overwritten by my register_
 * browser_globals' level-distinguished console.* bindings, so it never
 * runs at runtime — but the symbol still has to link. Route to MS_LOG
 * with a generic prefix as a safety net. */
void macsurf_js_console_append(const char *line)
{
	MS_LOG(line ? line : "");
}

void js_initialise(void)
{
	size_t i;
	MS_LOG("js: initialise");
	for (i = 0; i < sizeof macsurf_js__content_types /
			sizeof macsurf_js__content_types[0]; i++) {
		nserror e = content_factory_register_handler(
			macsurf_js__content_types[i],
			&macsurf_js__content_handler);
		if (e != NSERROR_OK) {
			macsurf_debug_log_writef(
				"js: register %s failed err=%d",
				macsurf_js__content_types[i], (int)e);
		}
	}
	MS_LOG("js: content types registered");
}

void js_finalise(void)
{
	MS_LOG("js: finalise");
}

nserror js_newheap(int timeout, struct jsheap **out_heap)
{
	struct jsheap *heap;
	if (out_heap == NULL) return NSERROR_BAD_PARAMETER;
	*out_heap = NULL;
	heap = (struct jsheap *)calloc(1, sizeof(*heap));
	if (heap == NULL) return NSERROR_NOMEM;
	heap->ctx = duk_create_heap_default();
	if (heap->ctx == NULL) {
		free(heap);
		return NSERROR_NOMEM;
	}
	heap->timeout = timeout;
	/* fixes319f — call the DOM-bindings setup function FIRST so
	 * document.getElementById / querySelector / createElement are
	 * available. Previously it was dead code (declared in the .h,
	 * defined in macsurf_js_dom.c, never invoked anywhere). After
	 * setup_globals runs, register_browser_globals overlays the
	 * level-distinguished console.* plus navigator, atob/btoa,
	 * etc. on top of whatever setup_globals installed. */
	macsurf_js_setup_globals(heap->ctx);
	register_browser_globals(heap->ctx);
	global_heap = heap;
	*out_heap = heap;
	MS_LOG("js: heap created");
	return NSERROR_OK;
}

void js_destroyheap(struct jsheap *heap)
{
	if (heap == NULL) return;
	if (heap->ctx != NULL) duk_destroy_heap(heap->ctx);
	if (global_heap == heap) global_heap = NULL;
	free(heap);
}

/* fixes319c — pull the dom_document out of the html content and wire
 * it to the DOM bindings via macsurf_js_set_document. Without this,
 * macsurf_js_current_document stays NULL and every document.* call
 * (getElementById, querySelector, createElement) early-returns null,
 * which is why the t.html probe rows stayed at "(not run)" — the
 * console.* output landed in the log fine, but the DOM-write side
 * couldn't find #console-out / #nav-out / #b64-out.
 *
 * doc_priv comes from browser_window.c as hlcache_handle_get_content(c)
 * — i.e. struct content *. For HTML content this is actually an
 * html_content with a dom_document field inside. The public accessor
 * html_get_document() takes hlcache_handle*, not struct content*, so
 * we reach in directly via the private header. The accessor itself
 * just does `return ((html_content*)content)->document` so we use
 * the same pattern. */
#include "html/private.h"
extern void macsurf_js_set_document(dom_document *doc);

nserror js_newthread(struct jsheap *heap, void *win_priv, void *doc_priv,
		struct jsthread **out_thread)
{
	struct jsthread *thread;
	if (out_thread == NULL) return NSERROR_BAD_PARAMETER;
	*out_thread = NULL;
	if (heap == NULL || heap->ctx == NULL) return NSERROR_BAD_PARAMETER;
	thread = (struct jsthread *)calloc(1, sizeof(*thread));
	if (thread == NULL) return NSERROR_NOMEM;
	thread->heap = heap;
	thread->ctx = heap->ctx;
	thread->win_priv = win_priv;
	thread->doc_priv = doc_priv;
	*out_thread = thread;
	if (doc_priv != NULL) {
		html_content *htmlc = (html_content *)doc_priv;
		macsurf_js_set_document(htmlc->document);
		MS_LOG("js: thread document wired");
	}
	return NSERROR_OK;
}

nserror js_closethread(struct jsthread *thread)
{
	(void)thread;
	return NSERROR_OK;
}

void js_destroythread(struct jsthread *thread)
{
	free(thread);
}

/* NetSurf signature:
 *   bool js_exec(jsthread *, const uint8_t *, size_t, const char *name)
 * On CW8 PPC: bool == unsigned char, uint8_t == unsigned char,
 * size_t == unsigned long. Returns 1 on success (script ran cleanly),
 * 0 on parse/runtime error. Errors get logged via MS_LOG; the heap
 * stack stays clean either way. */
unsigned char js_exec(struct jsthread *thread,
		const unsigned char *txt, unsigned long txtlen,
		const char *name)
{
	int rc;
	if (thread == NULL || thread->ctx == NULL || txt == NULL) return 0;
	if (txtlen == 0) return 1;
	macsurf_profile_stamp("js-start");
	duk_push_lstring(thread->ctx, (const char *)txt,
			(duk_size_t)txtlen);
	rc = duk_peval(thread->ctx);
	if (rc != 0) {
		/* fixes377 — name the failing script + its size so a parse/run
		 * error can be tied to a specific <script> (the FB JS punch-list
		 * work). MS_LOG dual-channels to title bar + log file. */
		macsurf_debug_log_writef("js err [%s len=%ld]: %s",
			(name != NULL) ? name : "(anon)",
			(long)txtlen,
			duk_safe_to_string(thread->ctx, -1));
		duk_pop(thread->ctx);
		macsurf_profile_stamp("js-end");
		return 0;
	}
	duk_pop(thread->ctx);
	macsurf_profile_stamp("js-end");
	return 1;
}

/* DOM event hooks. Body-stubbed for the bridge round — NetSurf core
 * doesn't call these on inline-script-only pages, so leaving them as
 * no-ops doesn't break anything that the bridge enables. Full event
 * dispatch wires up alongside the DOM-binding work in a separate
 * round. */

unsigned char js_fire_event(struct jsthread *thread, const char *type,
		struct dom_document *doc, struct dom_node *target)
{
	/* fixes328 (#31) — route NetSurf core's event-fire (load,
	 * DOMContentLoaded, etc.) to the JS bridge. If target is an
	 * element, dispatch listeners via macsurf_js_dispatch_event.
	 * For document/window-level events (target == NULL), fire any
	 * window.on<type> handler by peval'ing it. */
	(void)doc;
	if (thread == NULL || thread->ctx == NULL || type == NULL) return 0;
	if (target != NULL) {
		extern bool macsurf_js_dispatch_event(struct jscontext *ctx,
				struct dom_element *el, const char *event_type);
		struct jscontext jc;
		jc.duk = thread->ctx;
		jc.win_priv = thread->win_priv;
		jc.doc_priv = thread->doc_priv;
		(void)macsurf_js_dispatch_event(&jc,
			(struct dom_element *)target, type);
	} else {
		/* window-level: dispatch listeners registered via
		 * window.addEventListener (stored in window._winListeners by
		 * fixes338) AND any window.on<type> handler. fixes350 (#31):
		 * pre-fix this branch only invoked the inline on<type>
		 * handler, so listeners attached via addEventListener never
		 * saw 'load' / 'DOMContentLoaded' / etc. and the J8 probe
		 * stayed red. */
		char buf[64];
		size_t tl = strlen(type);
		duk_context *dctx = thread->ctx;
		duk_push_global_object(dctx);
		/* (a) walk _winListeners[type] if present. */
		if (duk_get_prop_string(dctx, -1, "_winListeners") != 0 &&
		    duk_is_object(dctx, -1)) {
			if (duk_get_prop_string(dctx, -1, type) != 0 &&
			    duk_is_array(dctx, -1)) {
				duk_size_t n = duk_get_length(dctx, -1);
				duk_size_t i;
				for (i = 0; i < n; i++) {
					duk_get_prop_index(dctx, -1,
						(duk_uarridx_t)i);
					if (duk_is_callable(dctx, -1)) {
						if (duk_pcall(dctx, 0) != 0) {
							MS_LOG(
							  duk_safe_to_string(
							    dctx, -1));
						}
					}
					duk_pop(dctx);
				}
			}
			duk_pop(dctx); /* array (or undef) */
		}
		duk_pop(dctx); /* _winListeners (or undef) */
		/* (b) inline window.on<type>. */
		if (tl + 2 < sizeof buf) {
			buf[0] = 'o'; buf[1] = 'n';
			memcpy(buf + 2, type, tl);
			buf[2 + tl] = '\0';
			if (duk_get_prop_string(dctx, -1, buf) != 0 &&
			    duk_is_callable(dctx, -1)) {
				if (duk_pcall(dctx, 0) != 0) {
					MS_LOG(duk_safe_to_string(
						dctx, -1));
				}
			}
			duk_pop(dctx); /* fn (or undef) */
		}
		duk_pop(dctx); /* global */
	}
	return 1;
}

void js_handle_new_element(struct jsthread *thread, struct dom_element *node)
{
	(void)thread; (void)node;
}

void js_event_cleanup(struct jsthread *thread, struct dom_event *evt)
{
	(void)thread; (void)evt;
}

/* macsurf_js_exec_script — internal alias kept for any caller that
 * still uses the pre-fix316 internal name. Forwards to js_exec. No
 * known call sites in tree but cheap to retain. */
bool macsurf_js_exec_script(struct jsthread *thread, const char *txt,
		size_t len)
{
	return (bool)js_exec(thread, (const unsigned char *)txt,
			(unsigned long)len, NULL);
}

#endif /* WITH_DUKTAPE */
