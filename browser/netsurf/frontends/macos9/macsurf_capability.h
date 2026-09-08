/* MacSurf - bounded browser capability and CSS gap census. */
#ifndef MACSURF_CAPABILITY_H
#define MACSURF_CAPABILITY_H

enum ms_cap_domain {
	MS_CAP_GLOBAL = 0,
	MS_CAP_WINDOW,
	MS_CAP_DOCUMENT,
	MS_CAP_ELEMENT,
	MS_CAP_OBSERVER,
	MS_CAP_GEOMETRY,
	MS_CAP_NETWORK,
	MS_CAP_CSSOM,
	MS_CAP_OTHER
};

enum ms_cap_operation {
	MS_CAP_GET = 0, MS_CAP_HAS, MS_CAP_CALL, MS_CAP_CONSTRUCT,
	MS_CAP_REGISTER, MS_CAP_SET, MS_CAP_QUERY
};

enum ms_cap_result {
	MS_CAP_UNSUPPORTED = 0, MS_CAP_STUB, MS_CAP_NOOP,
	MS_CAP_APPROXIMATE, MS_CAP_FALLBACK, MS_CAP_REJECTED,
	MS_CAP_UNAVAILABLE
};

enum ms_css_gap_kind {
	MS_CSS_GAP_SELECTOR = 0, MS_CSS_GAP_PROPERTY, MS_CSS_GAP_VALUE,
	MS_CSS_GAP_PSEUDO_CLASS, MS_CSS_GAP_PSEUDO_ELEMENT,
	MS_CSS_GAP_CONDITION, MS_CSS_GAP_COMPUTED, MS_CSS_GAP_CSSOM
};

/* A JS execution failure is different from an unsupported host API: it says
 * the page attempted work which did not complete.  Keep this small and
 * normalized so retained evidence never needs an exception object, a source
 * pointer, or a script URL. */
enum ms_js_event_kind {
	MS_JS_EVENT_SCRIPT_LOAD_FAILED = 0,
	MS_JS_EVENT_PARSE_FAILED,
	MS_JS_EVENT_RUNTIME_FAILED,
	MS_JS_EVENT_PROMISE_REJECTION,
	MS_JS_EVENT_HANDLER_FAILED
};

/* --- JS API observability (Phase 4) --- */
enum ms_js_api_gap_kind {
	MS_JS_API_GAP_MISSING = 0,      /* API does not exist (global/property undefined) */
	MS_JS_API_GAP_PARTIAL,          /* API exists but is partial/limited */
	MS_JS_API_GAP_STUB,             /* API exists as compatibility stub/no-op */
	MS_JS_API_GAP_FALLBACK,         /* API exists but falls back to alternative */
	MS_JS_API_GAP_CALL_FAILED,      /* API exists but call failed at runtime */
	MS_JS_API_GAP__N
};

/* JS API gap recording with normalized semantic key */
void ms_diag_js_api_gap_hit(int kind, const char *interface_name,
	const char *member_name, const char *normalized_key);

/* Promise rejection recording */
void ms_diag_promise_rejection_hit(int is_unhandled);

/* Event handler execution recording */
void ms_diag_event_handler_hit(const char *handler_type, int failed);

/* Copy scalar realm ownership from a live QJS boundary. */
void ms_diag_js_context_set(unsigned long nav_id, unsigned long doc_id,
	unsigned long frame_id);

/* Normalized key builder for JS API gaps */
const char *ms_diag_js_api_gap_key(int kind, const char *interface_name,
	const char *member_name);

/* Capability hit with interface/member distinction for better keys */
void ms_diag_capability_hit_ex(int domain, int operation,
	const char *interface_name, const char *member_name,
	int result, int quality);

void ms_diag_capability_hit(int domain, int operation, const char *name,
	int result, int quality);
void ms_diag_css_gap_hit(int kind, const char *property, const char *name,
	const char *value, int result);
void ms_diag_js_event_hit(int kind);

long macsurf_diag_serialize_capabilities(char *buf, long cap);
long macsurf_diag_serialize_css_gaps(char *buf, long cap);
long macsurf_diag_serialize_gapreport(char *buf, long cap);
long macsurf_diag_serialize_javascript(char *buf, long cap);
long macsurf_diag_serialize_js_api_gaps(char *buf, long cap);
long macsurf_diag_serialize_promise_rejections(char *buf, long cap);
long macsurf_diag_serialize_event_handlers(char *buf, long cap);

#endif
