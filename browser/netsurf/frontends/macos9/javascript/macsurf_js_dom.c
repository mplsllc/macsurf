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

/* Forward declarations for libdom public API.  Avoid pulling in the
 * internal src/ headers — they're the ones that break C89. */
struct dom_document;
struct dom_element;
struct dom_node;
struct dom_string;
typedef struct dom_element dom_element;
typedef struct dom_document dom_document;
typedef struct dom_node dom_node;
typedef struct dom_string dom_string;

typedef unsigned int dom_exception;
#define DOM_NO_ERR 0

extern dom_exception dom_string_create(const unsigned char *ptr, size_t len,
		dom_string **str);
extern dom_exception dom_string_unref(dom_string *str);
extern const char *dom_string_data(const dom_string *str);
extern size_t dom_string_length(const dom_string *str);

extern dom_exception dom_document_get_element_by_id(dom_document *doc,
		dom_string *id, dom_element **element);
extern dom_exception dom_document_create_element(dom_document *doc,
		dom_string *tag_name, dom_element **element);

extern dom_exception dom_element_get_tag_name(dom_element *el, dom_string **name);
extern dom_exception dom_element_get_attribute(dom_element *el,
		dom_string *name, dom_string **value);
extern dom_exception dom_element_set_attribute(dom_element *el,
		dom_string *name, dom_string *value);

extern dom_exception dom_node_append_child(dom_node *parent,
		dom_node *new_child, dom_node **result);
extern dom_exception dom_node_unref(dom_node *node);
extern dom_exception dom_node_ref(dom_node *node);

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

	duk_push_c_function(duk, element_finalizer, 1);
	duk_set_finalizer(duk, -2);
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

static duk_ret_t
macsurf_console_log(duk_context *duk)
{
	duk_idx_t n = duk_get_top(duk);
	duk_idx_t i;
	for (i = 0; i < n; i++) {
		const char *s = duk_safe_to_string(duk, i);
		nslog_log(__FILE__, "", __LINE__,
				"console.log: %s", s != NULL ? s : "(null)");
	}
	return 0;
}

/* ----------------------------------------------------------------- */
/* alert(msg) — stage 8.  Shows a Mac OS 9 dialog; no-op on Linux.    */
/* ----------------------------------------------------------------- */

#ifdef __MWERKS__
#include <Dialogs.h>
static duk_ret_t
macsurf_alert(duk_context *duk)
{
	const char *msg = duk_safe_to_string(duk, 0);
	unsigned char pstr[256];
	size_t len;

	len = strlen(msg);
	if (len > 255) len = 255;
	pstr[0] = (unsigned char)len;
	memcpy(pstr + 1, msg, len);

	ParamText(pstr, (const unsigned char *)"\p",
			(const unsigned char *)"\p",
			(const unsigned char *)"\p");
	/* Generic note alert (DITL 128 from MacSurf.rsrc); falls back to
	 * StopAlert if that resource isn't present. */
	NoteAlert(128, NULL);
	return 0;
}
#else
static duk_ret_t
macsurf_alert(duk_context *duk)
{
	const char *msg = duk_safe_to_string(duk, 0);
	nslog_log(__FILE__, "", __LINE__,
			"alert: %s", msg != NULL ? msg : "(null)");
	return 0;
}
#endif

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
/* Event dispatch (stage 9 skeleton)                                  */
/* ----------------------------------------------------------------- */

bool
macsurf_js_dispatch_event(struct jscontext *ctx,
		struct dom_element *el, const char *event_type)
{
	/* TODO stage 9: resolve el -> JS wrapper from a C-side hash,
	 * read __listeners[event_type], call each handler.
	 *
	 * For now, inline handlers are evaluated via the attribute
	 * value when the click lands (see plotters.c). */
	(void)ctx; (void)el; (void)event_type;
	return false;
}
