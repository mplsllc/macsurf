#ifndef MACSURF_CAPABILITY_H
#define MACSURF_CAPABILITY_H
enum ms_cap_domain { MS_CAP_GLOBAL = 0, MS_CAP_WINDOW, MS_CAP_DOCUMENT,
	MS_CAP_ELEMENT, MS_CAP_OBSERVER, MS_CAP_GEOMETRY, MS_CAP_NETWORK,
	MS_CAP_CSSOM, MS_CAP_OTHER };
enum ms_cap_operation { MS_CAP_GET = 0, MS_CAP_HAS, MS_CAP_CALL,
	MS_CAP_CONSTRUCT, MS_CAP_REGISTER, MS_CAP_SET, MS_CAP_QUERY };
enum ms_cap_result { MS_CAP_UNSUPPORTED = 0, MS_CAP_STUB, MS_CAP_NOOP,
	MS_CAP_APPROXIMATE, MS_CAP_FALLBACK, MS_CAP_REJECTED, MS_CAP_UNAVAILABLE };
enum ms_css_gap_kind { MS_CSS_GAP_SELECTOR = 0, MS_CSS_GAP_PROPERTY,
	MS_CSS_GAP_VALUE, MS_CSS_GAP_PSEUDO_CLASS, MS_CSS_GAP_PSEUDO_ELEMENT,
	MS_CSS_GAP_CONDITION, MS_CSS_GAP_COMPUTED, MS_CSS_GAP_CSSOM };
enum ms_js_event_kind { MS_JS_EVENT_SCRIPT_LOAD_FAILED = 0,
	MS_JS_EVENT_PARSE_FAILED, MS_JS_EVENT_RUNTIME_FAILED,
	MS_JS_EVENT_PROMISE_REJECTION, MS_JS_EVENT_HANDLER_FAILED };
enum ms_js_api_gap_kind { MS_JS_API_GAP_MISSING = 0, MS_JS_API_GAP_PARTIAL,
	MS_JS_API_GAP_STUB, MS_JS_API_GAP_FALLBACK, MS_JS_API_GAP_CALL_FAILED,
	MS_JS_API_GAP__N };
void ms_diag_capability_hit(int, int, const char *, int, int);
void ms_diag_capability_hit_ex(int, int, const char *, const char *, int, int);
void ms_diag_css_gap_hit(int, const char *, const char *, const char *, int);
void ms_diag_js_event_hit(int);
void ms_diag_js_api_gap_hit(int, const char *, const char *, const char *);
void ms_diag_promise_rejection_hit(int);
void ms_diag_event_handler_hit(const char *, int);
void ms_diag_js_context_set(unsigned long, unsigned long, unsigned long);
long macsurf_diag_serialize_capabilities(char *, long);
long macsurf_diag_serialize_css_gaps(char *, long);
long macsurf_diag_serialize_gapreport(char *, long);
long macsurf_diag_serialize_javascript(char *, long);
long macsurf_diag_serialize_js_api_gaps(char *, long);
long macsurf_diag_serialize_promise_rejections(char *, long);
long macsurf_diag_serialize_event_handlers(char *, long);
#ifdef MACSURF_RECONVERT_TEST_HOOK
void ms_diag_js_api_gap_test_reset(void);
#endif
#endif
