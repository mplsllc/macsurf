/*
 * MacSurf — macsurf_js_xhr.c
 *
 * Minimal XMLHttpRequest stub.  The eventual shape fires requests
 * through the existing OT-backed macos9_fetch path, but stage 5 only
 * lands the JS-side object model and a synchronous path that returns
 * (readyState = 4, status = 0, responseText = "").  Enough to let
 * scripts that do `typeof XMLHttpRequest === "function"` probes pass,
 * and enough to surface basic API usage errors.
 */

#include <stddef.h>

#include "duktape.h"
#include "macsurf_js.h"

/* Property keys used on the XHR JS object. */
static const char *XHR_METHOD        = "__method";
static const char *XHR_URL           = "__url";
static const char *XHR_ASYNC         = "__async";
static const char *XHR_READY         = "readyState";
static const char *XHR_STATUS        = "status";
static const char *XHR_RESPONSE_TEXT = "responseText";
static const char *XHR_ONREADY       = "onreadystatechange";

static duk_ret_t
xhr_open(duk_context *duk)
{
	const char *method = duk_safe_to_string(duk, 0);
	const char *url    = duk_safe_to_string(duk, 1);
	int async          = (duk_get_top(duk) >= 3)
			? duk_to_boolean(duk, 2) : 1;

	duk_push_this(duk);
	duk_push_string(duk, method); duk_put_prop_string(duk, -2, XHR_METHOD);
	duk_push_string(duk, url);    duk_put_prop_string(duk, -2, XHR_URL);
	duk_push_boolean(duk, async); duk_put_prop_string(duk, -2, XHR_ASYNC);
	duk_push_int(duk, 1);         duk_put_prop_string(duk, -2, XHR_READY);
	duk_pop(duk);
	return 0;
}

static void
xhr_fire_onreadystatechange(duk_context *duk)
{
	duk_push_this(duk);
	if (duk_get_prop_string(duk, -1, XHR_ONREADY) &&
	    duk_is_callable(duk, -1)) {
		duk_dup(duk, -2);        /* this = xhr */
		if (duk_pcall_method(duk, 0) != 0) {
			/* Callback threw — swallow; log would go to NSLOG. */
		}
		duk_pop(duk);
	} else {
		duk_pop(duk);            /* undefined */
	}
	duk_pop(duk);                    /* this */
}

static duk_ret_t
xhr_send(duk_context *duk)
{
	(void)duk_get_top(duk);
	/* TODO stage 6+: queue via macos9_fetch_url on the OT path.
	 * For now, complete synchronously with an empty response so
	 * scripts that check readyState === 4 don't hang. */

	duk_push_this(duk);
	duk_push_int(duk, 4);
	duk_put_prop_string(duk, -2, XHR_READY);
	duk_push_int(duk, 0);
	duk_put_prop_string(duk, -2, XHR_STATUS);
	duk_push_string(duk, "");
	duk_put_prop_string(duk, -2, XHR_RESPONSE_TEXT);
	duk_pop(duk);

	xhr_fire_onreadystatechange(duk);
	return 0;
}

static duk_ret_t
xhr_abort(duk_context *duk)
{
	duk_push_this(duk);
	duk_push_int(duk, 0);
	duk_put_prop_string(duk, -2, XHR_READY);
	duk_pop(duk);
	return 0;
}

static duk_ret_t
xhr_set_request_header(duk_context *duk)
{
	/* Accepted and ignored.  Header wiring lands once the fetcher
	 * path is connected. */
	(void)duk;
	return 0;
}

duk_ret_t
macsurf_js_xhr_constructor(duk_context *duk)
{
	if (!duk_is_constructor_call(duk)) {
		return DUK_RET_TYPE_ERROR;
	}

	duk_push_this(duk);

	duk_push_int(duk, 0);
	duk_put_prop_string(duk, -2, XHR_READY);
	duk_push_int(duk, 0);
	duk_put_prop_string(duk, -2, XHR_STATUS);
	duk_push_string(duk, "");
	duk_put_prop_string(duk, -2, XHR_RESPONSE_TEXT);
	duk_push_null(duk);
	duk_put_prop_string(duk, -2, XHR_ONREADY);

	duk_push_c_function(duk, xhr_open, DUK_VARARGS);
	duk_put_prop_string(duk, -2, "open");
	duk_push_c_function(duk, xhr_send, 1);
	duk_put_prop_string(duk, -2, "send");
	duk_push_c_function(duk, xhr_abort, 0);
	duk_put_prop_string(duk, -2, "abort");
	duk_push_c_function(duk, xhr_set_request_header, 2);
	duk_put_prop_string(duk, -2, "setRequestHeader");

	duk_pop(duk);
	return 0;
}
