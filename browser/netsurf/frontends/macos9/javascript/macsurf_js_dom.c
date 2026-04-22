/*
 * MacSurf — macsurf_js_dom.c
 *
 * Duktape/C bindings that bridge JS calls to libdom.  Priority order
 * from the plan:
 *   1.  document.getElementById
 *   2.  element.innerHTML setter/getter
 *   3.  element.style setter  (TODO stage 10+)
 *   4.  element.addEventListener  (wired via js_dom_event_add_listener)
 *   5.  document.createElement
 *   6.  element.appendChild
 *   7.  window.location.href  (TODO)
 *   8.  XMLHttpRequest  (macsurf_js_xhr.c)
 *   9.  document.querySelector  (basic: #id / tag only for now)
 *  10.  console.log
 *
 * Every dom_* call must null-check its return.  A malformed DOM is a
 * normal runtime condition, not a bug.
 *
 * The JS representation of a DOM element is a plain JS object with
 * these internal properties:
 *   __el        : pointer to dom_element (via duk_push_pointer)
 *   __listeners : { "click": [fn, fn, ...], ... }
 * Plus accessor properties: tagName, innerHTML, ... registered via
 * duk_def_prop.
 *
 * A Duktape finalizer on the wrapper object calls dom_node_unref when
 * the JS object is garbage collected, matching libdom's refcount model.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "utils/log.h"
#include "duktape.h"
#include "macsurf_js.h"

/*
 * libdom is in the source tree but not all the implementation .c files
 * are in the CW8 project file list, and the public header relies on
 * static-inline helpers that CW8 doesn't always emit out-of-line bodies
 * for.  Until the full libdom binding lands (deferred — see
 * dom_parser.c port note in the moonshot plan), we provide local
 * forward decls + no-op stubs in this TU so the JS DOM bindings
 * compile, link, and gracefully return null when no document is set.
 */

struct dom_document;
struct dom_element;
struct dom_node;
struct dom_string;
typedef struct dom_element  dom_element;
typedef struct dom_document dom_document;
typedef struct dom_node     dom_node;
typedef struct dom_string   dom_string;

typedef int dom_exception;
#define DOM_NO_ERR 0

/* Forward decls — real bodies live in macsurf_js_dom_stubs.c (fixes164).
 * Both prior attempts to put the definitions in THIS TU (fixes162 as
 * static, fixes163 as non-static external) still left the Mac build
 * with undefined-symbol link errors. Splitting into a separate .c
 * file forces cross-.o resolution, which is the path CW8 handles
 * reliably. */
extern dom_exception dom_string_create(const unsigned char *ptr, size_t len,
		dom_string **str);
extern void          dom_string_unref(dom_string *str);
extern const char   *dom_string_data(const dom_string *str);
extern size_t        dom_string_length(const dom_string *str);

extern void          dom_node_ref(dom_node *node);
extern void          dom_node_unref(dom_node *node);

extern dom_exception dom_document_get_element_by_id(dom_document *doc,
		dom_string *id, dom_element **element);
extern dom_exception dom_document_create_element(dom_document *doc,
		dom_string *tag_name, dom_element **element);
extern dom_exception dom_element_get_tag_name(dom_element *el,
		dom_string **name);
extern dom_exception dom_element_get_attribute(dom_element *el,
		dom_string *name, dom_string **value);
extern dom_exception dom_element_set_attribute(dom_element *el,
		dom_string *name, dom_string *value);
extern dom_exception dom_node_append_child(dom_node *parent,
		dom_node *new_child, dom_node **result);

/* ----------------------------------------------------------------- */
/* Current document — set by the html handler on page load.          */
/* ----------------------------------------------------------------- */

static dom_document *macsurf_js_current_document = NULL;

void
macsurf_js_set_document(dom_document *doc)
{
	macsurf_js_current_document = doc;
}

/* ----------------------------------------------------------------- */
/* Element wrapper push/finalizer                                     */
/* ----------------------------------------------------------------- */

static duk_ret_t
element_finalizer(duk_context *duk)
{
	dom_element *el;

	duk_get_prop_string(duk, 0, "__el");
	el = (dom_element *)duk_get_pointer(duk, -1);
	duk_pop(duk);

	if (el != NULL) {
		dom_node_unref((dom_node *)el);
	}
	return 0;
}

/* Forward decls for helpers defined later in this TU. */
static duk_ret_t macsurf_getTagName(duk_context *duk);
static duk_ret_t macsurf_getInnerHTML(duk_context *duk);
static duk_ret_t macsurf_setInnerHTML(duk_context *duk);
static duk_ret_t macsurf_getAttribute(duk_context *duk);
static duk_ret_t macsurf_setAttribute(duk_context *duk);
static duk_ret_t macsurf_appendChild(duk_context *duk);
static duk_ret_t macsurf_addEventListener(duk_context *duk);
static void wrapper_register(duk_context *duk, dom_element *el,
		duk_idx_t wrapper_idx);

void
macsurf_push_element(duk_context *duk, dom_element *el)
{
	if (el == NULL) {
		duk_push_null(duk);
		return;
	}

	/* Take an extra ref — the finalizer will drop it. */
	dom_node_ref((dom_node *)el);

	duk_push_object(duk);
	duk_push_pointer(duk, el);
	duk_put_prop_string(duk, -2, "__el");

	duk_push_object(duk);
	duk_put_prop_string(duk, -2, "__listeners");

	/* DOM methods on the element wrapper. */
	duk_push_c_function(duk, macsurf_getTagName, 0);
	duk_put_prop_string(duk, -2, "tagName");
	duk_push_c_function(duk, macsurf_getAttribute, 1);
	duk_put_prop_string(duk, -2, "getAttribute");
	duk_push_c_function(duk, macsurf_setAttribute, 2);
	duk_put_prop_string(duk, -2, "setAttribute");
	duk_push_c_function(duk, macsurf_appendChild, 1);
	duk_put_prop_string(duk, -2, "appendChild");
	duk_push_c_function(duk, macsurf_addEventListener, 3);
	duk_put_prop_string(duk, -2, "addEventListener");

	/* innerHTML accessor. */
	duk_push_string(duk, "innerHTML");
	duk_push_c_function(duk, macsurf_getInnerHTML, 0);
	duk_push_c_function(duk, macsurf_setInnerHTML, 1);
	duk_def_prop(duk, -4,
			DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
			DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_ENUMERABLE |
			DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);

	duk_push_c_function(duk, element_finalizer, 1);
	duk_set_finalizer(duk, -2);

	/* Register the wrapper in the pointer-keyed stash so
	 * dispatch_event can find it from a raw dom_element*. */
	wrapper_register(duk, el, duk_get_top_index(duk));
}

static dom_element *
element_from_this(duk_context *duk)
{
	dom_element *el;
	duk_push_this(duk);
	duk_get_prop_string(duk, -1, "__el");
	el = (dom_element *)duk_get_pointer(duk, -1);
	duk_pop_2(duk);
	return el;
}

/* ----------------------------------------------------------------- */
/* document.getElementById                                            */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_getElementById(duk_context *duk)
{
	const char *id;
	dom_string *id_str = NULL;
	dom_element *el = NULL;

	if (macsurf_js_current_document == NULL) {
		duk_push_null(duk);
		return 1;
	}

	id = duk_require_string(duk, 0);
	if (id == NULL) {
		duk_push_null(duk);
		return 1;
	}

	if (dom_string_create((const unsigned char *)id, strlen(id), &id_str)
			!= DOM_NO_ERR || id_str == NULL) {
		duk_push_null(duk);
		return 1;
	}

	if (dom_document_get_element_by_id(macsurf_js_current_document,
			id_str, &el) != DOM_NO_ERR) {
		el = NULL;
	}
	dom_string_unref(id_str);

	macsurf_push_element(duk, el);
	return 1;
}

/* ----------------------------------------------------------------- */
/* element.tagName                                                    */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_getTagName(duk_context *duk)
{
	dom_element *el;
	dom_string *tag = NULL;

	el = element_from_this(duk);
	if (el == NULL) {
		duk_push_string(duk, "");
		return 1;
	}

	if (dom_element_get_tag_name(el, &tag) != DOM_NO_ERR || tag == NULL) {
		duk_push_string(duk, "");
		return 1;
	}

	duk_push_lstring(duk, dom_string_data(tag), dom_string_length(tag));
	dom_string_unref(tag);
	return 1;
}

/* ----------------------------------------------------------------- */
/* element.innerHTML — getter / setter                                */
/*                                                                    */
/* Full HTML serialisation needs libdom's HTML serialiser which is    */
/* not yet wired up.  Stage 4 returns empty string for the getter     */
/* and stores the raw assignment on the wrapper object so subsequent  */
/* reads see the last-written value.  True DOM mutation is TODO.      */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_getInnerHTML(duk_context *duk)
{
	duk_push_this(duk);
	if (duk_get_prop_string(duk, -1, "__innerHTML") == 0) {
		duk_pop(duk);
		duk_push_string(duk, "");
	}
	/* leave value on stack */
	return 1;
}

static duk_ret_t
macsurf_setInnerHTML(duk_context *duk)
{
	const char *html = duk_safe_to_string(duk, 0);
	duk_push_this(duk);
	duk_push_string(duk, html);
	duk_put_prop_string(duk, -2, "__innerHTML");
	duk_pop(duk);
	/* TODO stage 10+: actually parse html and replace element children. */
	return 0;
}

/* ----------------------------------------------------------------- */
/* element.getAttribute / setAttribute                                */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_getAttribute(duk_context *duk)
{
	dom_element *el;
	dom_string *name_str = NULL;
	dom_string *val_str = NULL;
	const char *name;

	el = element_from_this(duk);
	if (el == NULL) {
		duk_push_null(duk);
		return 1;
	}

	name = duk_require_string(duk, 0);
	if (dom_string_create((const unsigned char *)name, strlen(name),
			&name_str) != DOM_NO_ERR || name_str == NULL) {
		duk_push_null(duk);
		return 1;
	}

	if (dom_element_get_attribute(el, name_str, &val_str) != DOM_NO_ERR
			|| val_str == NULL) {
		dom_string_unref(name_str);
		duk_push_null(duk);
		return 1;
	}

	duk_push_lstring(duk, dom_string_data(val_str),
			dom_string_length(val_str));
	dom_string_unref(val_str);
	dom_string_unref(name_str);
	return 1;
}

static duk_ret_t
macsurf_setAttribute(duk_context *duk)
{
	dom_element *el;
	dom_string *name_str = NULL;
	dom_string *val_str = NULL;
	const char *name;
	const char *val;

	el = element_from_this(duk);
	if (el == NULL) {
		return 0;
	}

	name = duk_require_string(duk, 0);
	val  = duk_safe_to_string(duk, 1);

	if (dom_string_create((const unsigned char *)name, strlen(name),
			&name_str) != DOM_NO_ERR || name_str == NULL) {
		return 0;
	}
	if (dom_string_create((const unsigned char *)val, strlen(val),
			&val_str) != DOM_NO_ERR || val_str == NULL) {
		dom_string_unref(name_str);
		return 0;
	}

	dom_element_set_attribute(el, name_str, val_str);
	dom_string_unref(val_str);
	dom_string_unref(name_str);
	return 0;
}

/* ----------------------------------------------------------------- */
/* element.appendChild                                                */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_appendChild(duk_context *duk)
{
	dom_element *parent;
	dom_element *child = NULL;
	dom_node *result = NULL;

	parent = element_from_this(duk);
	if (parent == NULL) {
		duk_push_null(duk);
		return 1;
	}

	duk_get_prop_string(duk, 0, "__el");
	child = (dom_element *)duk_get_pointer(duk, -1);
	duk_pop(duk);

	if (child == NULL) {
		duk_push_null(duk);
		return 1;
	}

	if (dom_node_append_child((dom_node *)parent, (dom_node *)child,
			&result) != DOM_NO_ERR) {
		duk_push_null(duk);
		return 1;
	}

	/* Return the appended child (same as input by spec). */
	duk_dup(duk, 0);
	return 1;
}

/* ----------------------------------------------------------------- */
/* document.createElement                                             */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_createElement(duk_context *duk)
{
	const char *tag;
	dom_string *tag_str = NULL;
	dom_element *el = NULL;

	if (macsurf_js_current_document == NULL) {
		duk_push_null(duk);
		return 1;
	}

	tag = duk_require_string(duk, 0);
	if (dom_string_create((const unsigned char *)tag, strlen(tag),
			&tag_str) != DOM_NO_ERR || tag_str == NULL) {
		duk_push_null(duk);
		return 1;
	}

	if (dom_document_create_element(macsurf_js_current_document,
			tag_str, &el) != DOM_NO_ERR) {
		el = NULL;
	}
	dom_string_unref(tag_str);

	macsurf_push_element(duk, el);
	return 1;
}

/* ----------------------------------------------------------------- */
/* document.querySelector — ID-only and tag-only support.             */
/* Supports "#foo" (getElementById) and bare tag names.  Compound     */
/* selectors fall through to null for now.                            */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_querySelector(duk_context *duk)
{
	const char *sel = duk_require_string(duk, 0);

	if (sel == NULL || sel[0] == '\0') {
		duk_push_null(duk);
		return 1;
	}

	if (sel[0] == '#') {
		/* Delegate to getElementById with the rest of the string. */
		duk_push_string(duk, sel + 1);
		duk_replace(duk, 0);
		return macsurf_getElementById(duk);
	}

	/* TODO stage 10: tag-name lookup via dom_document_get_elements_by_tag_name. */
	duk_push_null(duk);
	return 1;
}

/* ----------------------------------------------------------------- */
/* document.title — getter/setter backed by an internal string prop.  */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_document_get_title(duk_context *duk)
{
	duk_push_this(duk);
	if (duk_get_prop_string(duk, -1, "__title") == 0) {
		duk_pop(duk);
		duk_push_string(duk, "");
	}
	return 1;
}

static duk_ret_t
macsurf_document_set_title(duk_context *duk)
{
	const char *title = duk_safe_to_string(duk, 0);
	duk_push_this(duk);
	duk_push_string(duk, title);
	duk_put_prop_string(duk, -2, "__title");
	duk_pop(duk);
	return 0;
}

/* ----------------------------------------------------------------- */
/* console.log — routes to NSLOG.                                     */
/* ----------------------------------------------------------------- */

extern void macsurf_js_console_append(const char *line);

static duk_ret_t
macsurf_console_log(duk_context *duk)
{
	duk_idx_t n = duk_get_top(duk);
	duk_idx_t i;
	/* Concatenate args with spaces — closest to console.log semantics. */
	if (n == 0) {
		macsurf_js_console_append("");
		return 0;
	}
	duk_push_string(duk, " ");
	duk_insert(duk, 0);
	duk_join(duk, n);
	{
		const char *line = duk_safe_to_string(duk, -1);
		nslog_log(__FILE__, "", __LINE__,
				"console.log: %s", line != NULL ? line : "(null)");
		macsurf_js_console_append(line);
	}
	duk_pop(duk);
	return 0;
}

/* ----------------------------------------------------------------- */
/* alert(msg) — stage 8.  Shows a Mac OS 9 dialog; no-op on Linux.    */
/* ----------------------------------------------------------------- */

/*
 * alert(msg) — logs to NSLOG only.  We avoid pulling Dialogs.h into
 * this TU (it cascades into MacWindows.h and AliasHandle compile
 * errors on CW8's Universal Interfaces; the Files.h + Aliases.h
 * workaround used in macos9.h is fragile here).  A real Carbon
 * NoteAlert wrapper will land in a separate file once the rest of
 * the JS path is stable.
 */
static duk_ret_t
macsurf_alert(duk_context *duk)
{
	const char *msg = duk_safe_to_string(duk, 0);
	nslog_log(__FILE__, "", __LINE__,
			"alert: %s", msg != NULL ? msg : "(null)");
	return 0;
}

/* ----------------------------------------------------------------- */
/* setup_globals — register document / window / console / alert /    */
/* setTimeout / setInterval / clearTimeout / XMLHttpRequest on the    */
/* Duktape global object.                                             */
/* ----------------------------------------------------------------- */

/* From macsurf_js_timers.c */
extern duk_ret_t macsurf_js_settimeout(duk_context *duk);
extern duk_ret_t macsurf_js_setinterval(duk_context *duk);
extern duk_ret_t macsurf_js_cleartimeout(duk_context *duk);

/* From macsurf_js_xhr.c */
extern duk_ret_t macsurf_js_xhr_constructor(duk_context *duk);

void
macsurf_js_setup_globals(duk_context *duk)
{
	if (duk == NULL) return;

	/* window === the Duktape global object itself (alias). */
	duk_push_global_object(duk);
	duk_push_string(duk, "window");
	duk_push_global_object(duk);
	duk_def_prop(duk, -3,
			DUK_DEFPROP_HAVE_VALUE |
			DUK_DEFPROP_HAVE_WRITABLE |   DUK_DEFPROP_WRITABLE |
			DUK_DEFPROP_HAVE_ENUMERABLE |
			DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);

	/* ---- document ---- */
	duk_push_object(duk);                             /* [global, document] */

	duk_push_c_function(duk, macsurf_getElementById, 1);
	duk_put_prop_string(duk, -2, "getElementById");

	duk_push_c_function(duk, macsurf_createElement, 1);
	duk_put_prop_string(duk, -2, "createElement");

	duk_push_c_function(duk, macsurf_querySelector, 1);
	duk_put_prop_string(duk, -2, "querySelector");

	/* title accessor */
	duk_push_string(duk, "title");
	duk_push_c_function(duk, macsurf_document_get_title, 0);
	duk_push_c_function(duk, macsurf_document_set_title, 1);
	duk_def_prop(duk, -4,
			DUK_DEFPROP_HAVE_GETTER |
			DUK_DEFPROP_HAVE_SETTER |
			DUK_DEFPROP_HAVE_ENUMERABLE |
			DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);

	duk_put_prop_string(duk, -2, "document");

	/* ---- console ---- */
	duk_push_object(duk);
	duk_push_c_function(duk, macsurf_console_log, DUK_VARARGS);
	duk_put_prop_string(duk, -2, "log");
	duk_push_c_function(duk, macsurf_console_log, DUK_VARARGS);
	duk_put_prop_string(duk, -2, "warn");
	duk_push_c_function(duk, macsurf_console_log, DUK_VARARGS);
	duk_put_prop_string(duk, -2, "error");
	duk_push_c_function(duk, macsurf_console_log, DUK_VARARGS);
	duk_put_prop_string(duk, -2, "info");
	duk_put_prop_string(duk, -2, "console");

	/* ---- alert ---- */
	duk_push_c_function(duk, macsurf_alert, 1);
	duk_put_prop_string(duk, -2, "alert");

	/* ---- timers ---- */
	duk_push_c_function(duk, macsurf_js_settimeout, 2);
	duk_put_prop_string(duk, -2, "setTimeout");
	duk_push_c_function(duk, macsurf_js_setinterval, 2);
	duk_put_prop_string(duk, -2, "setInterval");
	duk_push_c_function(duk, macsurf_js_cleartimeout, 1);
	duk_put_prop_string(duk, -2, "clearTimeout");
	duk_push_c_function(duk, macsurf_js_cleartimeout, 1);
	duk_put_prop_string(duk, -2, "clearInterval");

	/* ---- XMLHttpRequest constructor ---- */
	duk_push_c_function(duk, macsurf_js_xhr_constructor, 1);
	duk_put_prop_string(duk, -2, "XMLHttpRequest");

	duk_pop(duk); /* global object */
}

/* ----------------------------------------------------------------- */
/* Event dispatch                                                     */
/*                                                                    */
/* Stage 9 implementation:                                            */
/*   1. Inline on<event> handlers:                                    */
/*      Read the element's `on<type>` attribute via libdom and        */
/*      duk_peval_lstring it.  Backwards-compatible with classic      */
/*      "<button onclick='alert(1)'>" markup, which is exactly what   */
/*      pre-2010 sites use heavily.                                   */
/*   2. addEventListener-registered handlers:                         */
/*      Stored in __listeners[type] = [fn, fn, ...] on the wrapper.   */
/*      We pcall each in order.  Errors per-handler are isolated.     */
/*                                                                    */
/* Returns true iff at least one handler was invoked successfully.    */
/* ----------------------------------------------------------------- */

#define MACSURF_JS_WRAPPER_STASH "macsurf_el_wrappers"

static void
wrapper_stash_key(char *buf, size_t buflen, void *p)
{
	/* Use the pointer's hex form as a stash key.  Stable for the
	 * pointer's lifetime; dom_element addresses don't move. */
	unsigned long v = (unsigned long)p;
	if (buflen >= 11) {
		buf[0] = '0'; buf[1] = 'x';
		{
			int i;
			for (i = 9; i >= 2; i--) {
				int nyb = (int)(v & 0xF);
				buf[i] = (char)(nyb < 10 ? '0' + nyb : 'a' + nyb - 10);
				v >>= 4;
			}
		}
		buf[10] = '\0';
	} else if (buflen > 0) {
		buf[0] = '\0';
	}
}

static void
wrapper_register(duk_context *duk, dom_element *el, duk_idx_t wrapper_idx)
{
	char key[12];
	wrapper_stash_key(key, sizeof(key), el);

	duk_push_global_stash(duk);
	if (duk_get_prop_string(duk, -1, MACSURF_JS_WRAPPER_STASH) == 0) {
		duk_pop(duk);
		duk_push_object(duk);
		duk_dup_top(duk);
		duk_put_prop_string(duk, -3, MACSURF_JS_WRAPPER_STASH);
	}
	duk_dup(duk, wrapper_idx);
	duk_put_prop_string(duk, -2, key);
	duk_pop_2(duk);
}

static bool
wrapper_lookup_push(duk_context *duk, dom_element *el)
{
	char key[12];
	wrapper_stash_key(key, sizeof(key), el);

	duk_push_global_stash(duk);
	if (duk_get_prop_string(duk, -1, MACSURF_JS_WRAPPER_STASH) == 0) {
		duk_pop_2(duk);
		return false;
	}
	if (duk_get_prop_string(duk, -1, key) == 0) {
		duk_pop_3(duk);
		return false;
	}
	duk_remove(duk, -2); /* the wrappers stash */
	duk_remove(duk, -2); /* the global stash */
	return true;
}

bool
macsurf_js_dispatch_event(struct jscontext *ctx,
		struct dom_element *el, const char *event_type)
{
	dom_string *attr_name = NULL;
	dom_string *attr_val  = NULL;
	char attr_buf[32];
	size_t et_len;
	bool fired = false;

	if (ctx == NULL || ctx->duk == NULL || el == NULL ||
	    event_type == NULL) {
		return false;
	}

	/* --- 1. Inline on<type> handler from attribute. --- */
	et_len = strlen(event_type);
	if (et_len + 2 + 1 <= sizeof(attr_buf)) {
		attr_buf[0] = 'o'; attr_buf[1] = 'n';
		memcpy(attr_buf + 2, event_type, et_len);
		attr_buf[2 + et_len] = '\0';

		if (dom_string_create((const unsigned char *)attr_buf,
				et_len + 2, &attr_name) == DOM_NO_ERR &&
		    attr_name != NULL) {
			if (dom_element_get_attribute(el, attr_name,
					&attr_val) == DOM_NO_ERR &&
			    attr_val != NULL) {
				if (duk_peval_lstring(ctx->duk,
						dom_string_data(attr_val),
						dom_string_length(attr_val))
						== 0) {
					fired = true;
				} else {
					nslog_log(__FILE__, "", __LINE__,
						"inline %s handler error: %s",
						event_type,
						duk_safe_to_string(ctx->duk, -1));
				}
				duk_pop(ctx->duk);
				dom_string_unref(attr_val);
			}
			dom_string_unref(attr_name);
		}
	}

	/* --- 2. addEventListener handlers from wrapper.__listeners. --- */
	if (wrapper_lookup_push(ctx->duk, el)) {
		if (duk_get_prop_string(ctx->duk, -1, "__listeners") &&
		    duk_get_prop_string(ctx->duk, -1, event_type) &&
		    duk_is_array(ctx->duk, -1)) {

			duk_size_t len = duk_get_length(ctx->duk, -1);
			duk_size_t i;
			for (i = 0; i < len; i++) {
				duk_get_prop_index(ctx->duk, -1, (duk_uarridx_t)i);
				if (duk_is_callable(ctx->duk, -1)) {
					duk_dup(ctx->duk, -4); /* this = wrapper */
					if (duk_pcall_method(ctx->duk, 0) == 0) {
						fired = true;
					}
				}
				duk_pop(ctx->duk);
			}
			duk_pop(ctx->duk);  /* listeners[type] */
			duk_pop(ctx->duk);  /* __listeners */
		} else {
			/* unwind whatever we pushed */
			duk_pop(ctx->duk);
			duk_pop(ctx->duk);
		}
		duk_pop(ctx->duk);          /* wrapper */
	}

	return fired;
}

/* ----------------------------------------------------------------- */
/* element.addEventListener                                           */
/* ----------------------------------------------------------------- */

static duk_ret_t
macsurf_addEventListener(duk_context *duk)
{
	const char *type;

	if (!duk_is_string(duk, 0) || !duk_is_function(duk, 1)) {
		return 0;
	}
	type = duk_get_string(duk, 0);

	duk_push_this(duk);
	if (duk_get_prop_string(duk, -1, "__listeners") == 0) {
		duk_pop(duk);
		duk_push_object(duk);
		duk_dup_top(duk);
		duk_put_prop_string(duk, -3, "__listeners");
	}
	/* stack: ... this listeners */
	if (duk_get_prop_string(duk, -1, type) == 0 ||
	    !duk_is_array(duk, -1)) {
		duk_pop(duk);
		duk_push_array(duk);
		duk_dup_top(duk);
		duk_put_prop_string(duk, -3, type);
	}
	/* stack: ... this listeners arr */
	{
		duk_size_t len = duk_get_length(duk, -1);
		duk_dup(duk, 1);
		duk_put_prop_index(duk, -2, (duk_uarridx_t)len);
	}
	duk_pop_3(duk);
	return 0;
}

/* Re-register macsurf_push_element to also store the wrapper in the
 * pointer-keyed stash so dispatch_event can find it later.  This is a
 * thin wrapper around the stage-4 helper; we forward-declared above. */
void
macsurf_register_element_wrapper(duk_context *duk, dom_element *el,
		duk_idx_t wrapper_idx)
{
	if (duk == NULL || el == NULL) return;
	wrapper_register(duk, el, wrapper_idx);
}
