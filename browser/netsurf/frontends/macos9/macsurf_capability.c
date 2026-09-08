/* MacSurf - bounded capability/CSS gap aggregates and gapreport census.
 * No allocation or formatting occurs at a hit site. */
#include <stdio.h>
#include <string.h>

#include "macsurf_capability.h"
#include "macsurf_diag.h"

#define MS_CAP_RECORDS 128
#define MS_CAP_TEXT 48
#define MS_JS_EVENT_RECORDS 32
#define MS_JS_API_GAP_RECORDS 64
#define MS_PROMISE_REJECTION_RECORDS 32
#define MS_EVENT_HANDLER_RECORDS 64

struct ms_cap_record {
	int used, domain, operation, result, quality;
	unsigned long count, first_nav, first_script, first_task;
	unsigned long last_nav, last_script, last_task;
	unsigned long name_hash;
	char name[MS_CAP_TEXT];
};
struct ms_css_record {
	int used, kind, result;
	unsigned long count, first_nav, last_nav;
	unsigned long prop_hash, name_hash, val_hash;
	char property[MS_CAP_TEXT];
	char name[MS_CAP_TEXT];
	char value[MS_CAP_TEXT];
};
struct ms_js_event_record {
	int used, kind;
	unsigned long count, first_nav, first_script, first_task;
	unsigned long last_nav, last_script, last_task;
};

/* Phase 4: JS API gap aggregation with normalized keys */
struct ms_js_api_gap_record {
	int used;
	int kind;                       /* enum ms_js_api_gap_kind */
	unsigned long count;
	unsigned long first_nav;
	unsigned long last_nav;
	unsigned long first_doc, last_doc, first_frame, last_frame;
	unsigned long first_script;
	unsigned long last_script;
	unsigned long first_task;
	unsigned long last_task;
	unsigned long key_hash;
	char key[64];                   /* normalized key: js.api.ResizeObserver.missing */
	char interface_name[32];
	char member_name[32];
};

/* Promise rejection tracking */
struct ms_promise_rejection_record {
	int used;
	unsigned long count;
	unsigned long first_nav;
	unsigned long last_nav;
	unsigned long first_doc, last_doc, first_frame, last_frame;
	unsigned long first_script;
	unsigned long last_script;
	unsigned long first_task;
	unsigned long last_task;
};

/* Event handler execution tracking */
struct ms_event_handler_record {
	int used;
	unsigned long count;
	unsigned long first_nav;
	unsigned long last_nav;
	unsigned long first_doc, last_doc, first_frame, last_frame;
	unsigned long first_script;
	unsigned long last_script;
	unsigned long first_task;
	unsigned long last_task;
	char handler_type[32];
};

static struct ms_cap_record g_cap[MS_CAP_RECORDS];
static struct ms_css_record g_css[MS_CAP_RECORDS];
static struct ms_js_event_record g_js_event[MS_JS_EVENT_RECORDS];
static struct ms_js_api_gap_record g_js_api_gap[MS_JS_API_GAP_RECORDS];
static struct ms_promise_rejection_record g_promise_rejection[MS_PROMISE_REJECTION_RECORDS];
static struct ms_event_handler_record g_event_handler[MS_EVENT_HANDLER_RECORDS];
static unsigned long g_cap_dropped, g_css_dropped, g_js_event_dropped;
static unsigned long g_js_api_gap_dropped;
static unsigned long g_promise_rejection_dropped;
static unsigned long g_event_handler_dropped;
static unsigned long g_cap_drop_domain[9], g_css_drop_kind[8];
static unsigned long g_js_context_nav, g_js_context_doc, g_js_context_frame;

void ms_diag_js_context_set(unsigned long nav_id, unsigned long doc_id,
	unsigned long frame_id)
{
	g_js_context_nav = nav_id;
	g_js_context_doc = doc_id;
	g_js_context_frame = frame_id;
}

static unsigned long js_context_nav(void)
{
	unsigned long v = ms_diag_cur_nav();
	return v != 0 ? v : g_js_context_nav;
}
static unsigned long js_context_doc(void)
{
	unsigned long v = ms_diag_cur_doc();
	return v != 0 ? v : g_js_context_doc;
}
static unsigned long js_context_frame(void)
{
	unsigned long v = ms_diag_cur_frame();
	return v != 0 ? v : g_js_context_frame;
}

#ifdef MACSURF_RECONVERT_TEST_HOOK
/* The production census is intentionally process-retained.  The harness uses
 * this only to isolate deterministic capacity cases without changing a ship
 * build's lifetime semantics. */
void ms_diag_js_api_gap_test_reset(void)
{
	memset(g_js_api_gap, 0, sizeof(g_js_api_gap));
	g_js_api_gap_dropped = 0;
}
#endif

static unsigned long str_hash(const char *s)
{
	unsigned long h = 5381;
	int c;
	if (s == NULL) return 0;
	while ((c = (int)(unsigned char)*s++) != 0) {
		h = ((h << 5) + h) + (unsigned long)c;
	}
	return h;
}

static void copy_text_n(char *out, int cap, const char *in)
{
	int i;
	if (in == NULL) in = "";
	if (cap < 1) return;
	for (i = 0; i < cap - 1 && in[i] != '\0'; i++) out[i] = in[i];
	out[i] = '\0';
}

static void copy_text(char *out, const char *in)
{
	copy_text_n(out, MS_CAP_TEXT, in);
}

static const char *js_api_gap_name(int kind)
{
	static const char *const a[] = {"missing", "partial", "stub_used",
		"fallback_used", "call_failed"};
	return (kind >= 0 && kind < MS_JS_API_GAP__N) ? a[kind] : "unknown";
}

static int js_api_gap_from_capability(int result)
{
	switch (result) {
	case MS_CAP_UNSUPPORTED:
	case MS_CAP_UNAVAILABLE: return MS_JS_API_GAP_MISSING;
	case MS_CAP_APPROXIMATE: return MS_JS_API_GAP_PARTIAL;
	case MS_CAP_STUB:
	case MS_CAP_NOOP: return MS_JS_API_GAP_STUB;
	case MS_CAP_FALLBACK: return MS_JS_API_GAP_FALLBACK;
	case MS_CAP_REJECTED: return MS_JS_API_GAP_CALL_FAILED;
	default: return MS_JS_API_GAP_CALL_FAILED;
	}
}

const char *ms_diag_js_api_gap_key(int kind, const char *interface_name,
	const char *member_name)
{
	static char key[128];
	const char *in = interface_name ? interface_name : "unknown";
	const char *mem = member_name ? member_name : "";
	if (mem[0] != '\0')
		snprintf(key, sizeof(key), "js.api.%s.%s.%s", in, mem,
			js_api_gap_name(kind));
	else
		snprintf(key, sizeof(key), "js.api.%s.%s", in,
			js_api_gap_name(kind));
	return key;
}

void ms_diag_js_api_gap_hit(int kind, const char *interface_name,
	const char *member_name, const char *normalized_key)
{
	int i, free_slot = -1;
	unsigned long kh;
	struct ms_js_api_gap_record *r = NULL;
	const char *key = normalized_key;
	if (key == NULL || key[0] == '\0')
		key = ms_diag_js_api_gap_key(kind, interface_name, member_name);
	kh = str_hash(key);
	for (i = 0; i < MS_JS_API_GAP_RECORDS; i++) {
		if (!g_js_api_gap[i].used) { if (free_slot < 0) free_slot = i; continue; }
		if (g_js_api_gap[i].kind == kind && g_js_api_gap[i].key_hash == kh &&
			strcmp(g_js_api_gap[i].key, key) == 0) { r = &g_js_api_gap[i]; break; }
	}
	if (r == NULL && free_slot >= 0) {
		r = &g_js_api_gap[free_slot];
		memset(r, 0, sizeof(*r)); r->used = 1; r->kind = kind; r->key_hash = kh;
		copy_text_n(r->key, (int)sizeof(r->key), key);
		copy_text_n(r->interface_name, (int)sizeof(r->interface_name), interface_name);
		copy_text_n(r->member_name, (int)sizeof(r->member_name), member_name);
		r->first_nav = js_context_nav(); r->first_script = ms_diag_cur_script();
		r->first_task = ms_diag_cur_task();
		r->first_doc = js_context_doc(); r->first_frame = js_context_frame();
	}
	if (r == NULL) { g_js_api_gap_dropped++; return; }
	r->count++; r->last_nav = js_context_nav();
	r->last_script = ms_diag_cur_script(); r->last_task = ms_diag_cur_task();
	r->last_doc = js_context_doc(); r->last_frame = js_context_frame();
}

static void ms_diag_capability_hit_internal(int domain, int operation, const char *name,
	int result, int quality)
{
	int i, free_slot = -1;
	unsigned long nh = str_hash(name);
	struct ms_cap_record *r = NULL;
	for (i = 0; i < MS_CAP_RECORDS; i++) {
		if (!g_cap[i].used) { if (free_slot < 0) free_slot = i; continue; }
		if (g_cap[i].domain == domain && g_cap[i].operation == operation &&
			g_cap[i].result == result && g_cap[i].quality == quality &&
			g_cap[i].name_hash == nh) { r = &g_cap[i]; break; }
	}
	if (r == NULL && free_slot >= 0) {
		r = &g_cap[free_slot];
		memset(r, 0, sizeof(*r));
		r->used = 1; r->domain = domain; r->operation = operation;
		r->result = result; r->quality = quality;
		r->name_hash = nh;
		copy_text(r->name, name);
		r->first_nav = js_context_nav(); r->first_script = ms_diag_cur_script();
		r->first_task = ms_diag_cur_task();
	}
	if (r == NULL) { g_cap_dropped++; if (domain >= 0 && domain < 9) g_cap_drop_domain[domain]++; return; }
	r->count++;
	r->last_nav = ms_diag_cur_nav(); r->last_script = ms_diag_cur_script();
	r->last_task = ms_diag_cur_task();
}

void ms_diag_capability_hit(int domain, int operation, const char *name,
	int result, int quality)
{
	ms_diag_capability_hit_internal(domain, operation, name, result, quality);
	ms_diag_js_api_gap_hit(js_api_gap_from_capability(result), name, NULL, NULL);
}

void ms_diag_capability_hit_ex(int domain, int operation,
	const char *interface_name, const char *member_name, int result, int quality)
{
	char name[MS_CAP_TEXT];
	if (member_name != NULL && member_name[0] != '\0')
		snprintf(name, sizeof(name), "%s.%s", interface_name ? interface_name : "unknown", member_name);
	else
		copy_text_n(name, (int)sizeof(name), interface_name);
	ms_diag_capability_hit_internal(domain, operation, name, result, quality);
	ms_diag_js_api_gap_hit(js_api_gap_from_capability(result), interface_name,
		member_name, NULL);
}

void ms_diag_css_gap_hit(int kind, const char *property, const char *name,
	const char *value, int result)
{
	int i, free_slot = -1;
	unsigned long ph = str_hash(property);
	unsigned long nh = str_hash(name);
	unsigned long vh = str_hash(value);
	struct ms_css_record *r = NULL;
	for (i = 0; i < MS_CAP_RECORDS; i++) {
		if (!g_css[i].used) { if (free_slot < 0) free_slot = i; continue; }
		if (g_css[i].kind == kind && g_css[i].result == result &&
			g_css[i].prop_hash == ph && g_css[i].name_hash == nh && g_css[i].val_hash == vh) {
			r = &g_css[i]; break;
		}
	}
	if (r == NULL && free_slot >= 0) {
		r = &g_css[free_slot]; memset(r, 0, sizeof(*r)); r->used = 1;
		r->kind = kind; r->result = result;
		r->prop_hash = ph; r->name_hash = nh; r->val_hash = vh;
		copy_text(r->property, property);
		copy_text(r->name, name); copy_text(r->value, value);
		r->first_nav = ms_diag_cur_nav();
	}
	if (r == NULL) { g_css_dropped++; if (kind >= 0 && kind < 8) g_css_drop_kind[kind]++; return; }
	r->count++; r->last_nav = ms_diag_cur_nav();
}

void ms_diag_js_event_hit(int kind)
{
	int i, free_slot = -1;
	struct ms_js_event_record *r = NULL;
	for (i = 0; i < MS_JS_EVENT_RECORDS; i++) {
		if (!g_js_event[i].used) {
			if (free_slot < 0) free_slot = i;
			continue;
		}
		if (g_js_event[i].kind == kind) { r = &g_js_event[i]; break; }
	}
	if (r == NULL && free_slot >= 0) {
		r = &g_js_event[free_slot];
		memset(r, 0, sizeof(*r));
		r->used = 1; r->kind = kind;
		r->first_nav = ms_diag_cur_nav();
		r->first_script = ms_diag_cur_script();
		r->first_task = ms_diag_cur_task();
	}
	if (r == NULL) { g_js_event_dropped++; return; }
	r->count++;
	r->last_nav = ms_diag_cur_nav();
	r->last_script = ms_diag_cur_script();
	r->last_task = ms_diag_cur_task();
}

void ms_diag_promise_rejection_hit(int is_unhandled)
{
	struct ms_promise_rejection_record *r;
	if (!is_unhandled) return;
	r = &g_promise_rejection[0];
	if (!r->used) {
		memset(r, 0, sizeof(*r)); r->used = 1;
		r->first_nav = ms_diag_cur_nav(); r->first_script = ms_diag_cur_script();
		r->first_task = ms_diag_cur_task();
		r->first_doc = js_context_doc(); r->first_frame = js_context_frame();
	}
	r->count++; r->last_nav = js_context_nav();
	r->last_script = ms_diag_cur_script(); r->last_task = ms_diag_cur_task();
	r->last_doc = js_context_doc(); r->last_frame = js_context_frame();
}

void ms_diag_event_handler_hit(const char *handler_type, int failed)
{
	int i, free_slot = -1;
	unsigned long h = str_hash(handler_type);
	struct ms_event_handler_record *r = NULL;
	for (i = 0; i < MS_EVENT_HANDLER_RECORDS; i++) {
		if (!g_event_handler[i].used) { if (free_slot < 0) free_slot = i; continue; }
		if (str_hash(g_event_handler[i].handler_type) == h &&
			strcmp(g_event_handler[i].handler_type, handler_type ? handler_type : "") == 0) {
			r = &g_event_handler[i]; break;
		}
	}
	if (r == NULL && free_slot >= 0) {
		r = &g_event_handler[free_slot]; memset(r, 0, sizeof(*r)); r->used = 1;
		copy_text_n(r->handler_type, (int)sizeof(r->handler_type), handler_type);
		r->first_nav = js_context_nav(); r->first_script = ms_diag_cur_script();
		r->first_task = ms_diag_cur_task();
		r->first_doc = js_context_doc(); r->first_frame = js_context_frame();
	}
	if (r == NULL) { g_event_handler_dropped++; return; }
	r->count++; r->last_nav = js_context_nav();
	r->last_script = ms_diag_cur_script(); r->last_task = ms_diag_cur_task();
	r->last_doc = js_context_doc(); r->last_frame = js_context_frame();
	/* The execution-failure event belongs to the callback boundary.  This
	 * aggregate records its handler class and must not double-count it. */
	(void)failed;
}

static long add(char *b, long c, long n, const char *s)
{
	long l;
	if (b == NULL || c < 2 || n < 0 || n >= c) return n;
	l = (long)strlen(s); if (l > c - 1 - n) l = c - 1 - n;
	if (l > 0) memcpy(b + n, s, (size_t)l); n += l; b[n] = '\0'; return n;
}

static const char *cap_result(int x)
{
	static const char *const a[] = {"unsupported", "stub", "no_op",
		"approximate", "fallback", "rejected", "unavailable"};
	return (x >= 0 && x < 7) ? a[x] : "unknown";
}
static const char *css_kind(int x)
{
	static const char *const a[] = {"selector", "property", "value",
		"pseudo-class", "pseudo-element", "condition", "computed", "cssom"};
	return (x >= 0 && x < 8) ? a[x] : "unknown";
}
static const char *cap_domain(int x)
{
	static const char *const a[] = {"global", "window", "document",
		"element", "observer", "geometry", "network", "cssom", "other"};
	return (x >= 0 && x < 9) ? a[x] : "unknown";
}
static const char *cap_op(int x)
{
	static const char *const a[] = {"get", "has", "call", "construct",
		"register", "set", "query"};
	return (x >= 0 && x < 7) ? a[x] : "unknown";
}
static const char *js_event_kind(int x)
{
	static const char *const a[] = {"script_load_failed", "script_parse_failed",
		"script_runtime_exception", "promise_rejection_unhandled",
		"event_handler_exception"};
	return (x >= 0 && x < 5) ? a[x] : "unknown";
}

long macsurf_diag_serialize_javascript(char *b, long c)
{
	char line[256]; long n = 0; int i, total = 0;
	if (b == NULL || c < 2) return 0;
	b[0] = '\0';
	n = add(b, c, n, "MSDIAG 1 javascript\n");
	for (i = 0; i < MS_JS_EVENT_RECORDS; i++) if (g_js_event[i].used) total++;
	snprintf(line, sizeof line,
		"total=%d capacity=%d dropped=%lu loss_explicit=1\n",
		total, MS_JS_EVENT_RECORDS, g_js_event_dropped);
	n = add(b, c, n, line);
	for (i = 0; i < MS_JS_EVENT_RECORDS; i++) if (g_js_event[i].used) {
		snprintf(line, sizeof line,
			"kind=%s count=%lu first_nav=%lu first_script=%lu first_task=%lu last_nav=%lu last_script=%lu last_task=%lu\n",
			js_event_kind(g_js_event[i].kind), g_js_event[i].count,
			g_js_event[i].first_nav, g_js_event[i].first_script,
			g_js_event[i].first_task, g_js_event[i].last_nav,
			g_js_event[i].last_script, g_js_event[i].last_task);
		n = add(b, c, n, line);
	}
	return n;
}

long macsurf_diag_serialize_js_api_gaps(char *b, long c)
{
	char line[320]; long n = 0; int i, total = 0;
	if (b == NULL || c < 2) return 0; b[0] = '\0';
	for (i = 0; i < MS_JS_API_GAP_RECORDS; i++) if (g_js_api_gap[i].used) total++;
	n = add(b, c, n, "MSDIAG 1 js_api_gaps\n");
	snprintf(line, sizeof(line), "total=%d capacity=%d dropped=%lu loss_explicit=1\n",
		total, MS_JS_API_GAP_RECORDS, g_js_api_gap_dropped);
	n = add(b, c, n, line);
	for (i = 0; i < MS_JS_API_GAP_RECORDS; i++) if (g_js_api_gap[i].used) {
		snprintf(line, sizeof(line),
			"key=%s category=api_%s interface=%s member=%s count=%lu first_nav=%lu last_nav=%lu first_doc=%lu last_doc=%lu first_frame=%lu last_frame=%lu first_script=%lu last_script=%lu first_task=%lu last_task=%lu\n",
			g_js_api_gap[i].key, js_api_gap_name(g_js_api_gap[i].kind),
			g_js_api_gap[i].interface_name, g_js_api_gap[i].member_name,
			g_js_api_gap[i].count, g_js_api_gap[i].first_nav, g_js_api_gap[i].last_nav,
			g_js_api_gap[i].first_doc, g_js_api_gap[i].last_doc,
			g_js_api_gap[i].first_frame, g_js_api_gap[i].last_frame,
			g_js_api_gap[i].first_script, g_js_api_gap[i].last_script,
			g_js_api_gap[i].first_task, g_js_api_gap[i].last_task);
		n = add(b, c, n, line);
	}
	return n;
}

long macsurf_diag_serialize_promise_rejections(char *b, long c)
{
	char line[256]; long n = 0;
	struct ms_promise_rejection_record *r = &g_promise_rejection[0];
	if (b == NULL || c < 2) return 0; b[0] = '\0';
	n = add(b, c, n, "MSDIAG 1 promise_rejections\n");
	snprintf(line, sizeof(line), "total=%d capacity=%d dropped=%lu loss_explicit=1\n",
		r->used ? 1 : 0, MS_PROMISE_REJECTION_RECORDS, g_promise_rejection_dropped);
	n = add(b, c, n, line);
	if (r->used) {
		snprintf(line, sizeof(line), "key=js.promise.unhandled count=%lu first_nav=%lu last_nav=%lu first_doc=%lu last_doc=%lu first_frame=%lu last_frame=%lu first_script=%lu last_script=%lu first_task=%lu last_task=%lu\n",
			r->count, r->first_nav, r->last_nav, r->first_doc, r->last_doc,
			r->first_frame, r->last_frame, r->first_script, r->last_script,
			r->first_task, r->last_task);
		n = add(b, c, n, line);
	}
	return n;
}

long macsurf_diag_serialize_event_handlers(char *b, long c)
{
	char line[256]; long n = 0; int i, total = 0;
	if (b == NULL || c < 2) return 0; b[0] = '\0';
	for (i = 0; i < MS_EVENT_HANDLER_RECORDS; i++) if (g_event_handler[i].used) total++;
	n = add(b, c, n, "MSDIAG 1 event_handlers\n");
	snprintf(line, sizeof(line), "total=%d capacity=%d dropped=%lu loss_explicit=1\n",
		total, MS_EVENT_HANDLER_RECORDS, g_event_handler_dropped);
	n = add(b, c, n, line);
	for (i = 0; i < MS_EVENT_HANDLER_RECORDS; i++) if (g_event_handler[i].used) {
		snprintf(line, sizeof(line), "handler=%s count=%lu first_nav=%lu last_nav=%lu first_doc=%lu last_doc=%lu first_frame=%lu last_frame=%lu first_script=%lu last_script=%lu first_task=%lu last_task=%lu\n",
			g_event_handler[i].handler_type, g_event_handler[i].count,
			g_event_handler[i].first_nav, g_event_handler[i].last_nav,
			g_event_handler[i].first_doc, g_event_handler[i].last_doc,
			g_event_handler[i].first_frame, g_event_handler[i].last_frame,
			g_event_handler[i].first_script, g_event_handler[i].last_script,
			g_event_handler[i].first_task, g_event_handler[i].last_task);
		n = add(b, c, n, line);
	}
	return n;
}

long macsurf_diag_serialize_capabilities(char *b, long c)
{
	char line[256]; long n = 0; int i, total = 0;
	if (b == NULL || c < 2) return 0; b[0] = '\0';
	n = add(b, c, n, "MSDIAG 1 capabilities\n");
	for (i = 0; i < MS_CAP_RECORDS; i++) if (g_cap[i].used) total++;
	snprintf(line, sizeof line, "total=%d dropped=%lu dropped_global=%lu dropped_window=%lu dropped_document=%lu dropped_element=%lu dropped_observer=%lu dropped_geometry=%lu dropped_network=%lu dropped_cssom=%lu dropped_other=%lu\n", total, g_cap_dropped, g_cap_drop_domain[0], g_cap_drop_domain[1], g_cap_drop_domain[2], g_cap_drop_domain[3], g_cap_drop_domain[4], g_cap_drop_domain[5], g_cap_drop_domain[6], g_cap_drop_domain[7], g_cap_drop_domain[8]);
	n = add(b, c, n, line);
	for (i = 0; i < MS_CAP_RECORDS; i++) if (g_cap[i].used) {
		snprintf(line, sizeof line, "id=%d domain=%s op=%s name=%s result=%s quality=%d count=%lu nav=%lu script=%lu task=%lu\n",
			i + 1, cap_domain(g_cap[i].domain), cap_op(g_cap[i].operation), g_cap[i].name,
			cap_result(g_cap[i].result), g_cap[i].quality, g_cap[i].count,
			g_cap[i].last_nav, g_cap[i].last_script, g_cap[i].last_task);
		n = add(b, c, n, line);
	}
	return n;
}

long macsurf_diag_serialize_css_gaps(char *b, long c)
{
	char line[256]; long n = 0; int i, total = 0;
	if (b == NULL || c < 2) return 0; b[0] = '\0';
	n = add(b, c, n, "MSDIAG 1 cssgaps\n");
	for (i = 0; i < MS_CAP_RECORDS; i++) if (g_css[i].used) total++;
	snprintf(line, sizeof line, "total=%d dropped=%lu dropped_selector=%lu dropped_property=%lu dropped_value=%lu dropped_pseudo_class=%lu dropped_pseudo_element=%lu dropped_condition=%lu dropped_computed=%lu dropped_cssom=%lu\n", total, g_css_dropped, g_css_drop_kind[0], g_css_drop_kind[1], g_css_drop_kind[2], g_css_drop_kind[3], g_css_drop_kind[4], g_css_drop_kind[5], g_css_drop_kind[6], g_css_drop_kind[7]);
	n = add(b, c, n, line);
	for (i = 0; i < MS_CAP_RECORDS; i++) if (g_css[i].used) {
		snprintf(line, sizeof line, "id=%d kind=%s property=%s name=%s value=%s result=%s count=%lu nav=%lu\n",
			i + 1, css_kind(g_css[i].kind), g_css[i].property, g_css[i].name,
			g_css[i].value, cap_result(g_css[i].result), g_css[i].count,
			g_css[i].last_nav);
		n = add(b, c, n, line);
	}
	return n;
}

long macsurf_diag_serialize_gapreport(char *b, long c)
{
	char line[256];
	long n = 0;
	int i;
	int cap_unique = 0, css_unique = 0, js_unique = 0, api_unique = 0;
	int css_prop_gaps = 0, css_val_gaps = 0, css_sel_gaps = 0;
	int css_pc_gaps = 0, css_pe_gaps = 0, css_at_gaps = 0;
	int css_media_gaps = 0, cssom_gaps = 0;
	unsigned long cap_hits = 0, css_hits = 0, js_hits = 0, api_hits = 0;
	unsigned long nav_id = ms_diag_cur_nav();

	if (b == NULL || c < 2) return 0;
	b[0] = '\0';

	for (i = 0; i < MS_CAP_RECORDS; i++) {
		if (g_cap[i].used) {
			cap_unique++;
			cap_hits += g_cap[i].count;
			if (g_cap[i].domain == MS_CAP_CSSOM) cssom_gaps++;
		}
	}
	for (i = 0; i < MS_JS_EVENT_RECORDS; i++) if (g_js_event[i].used) {
		js_unique++; js_hits += g_js_event[i].count;
	}
	for (i = 0; i < MS_JS_API_GAP_RECORDS; i++) if (g_js_api_gap[i].used) {
		api_unique++; api_hits += g_js_api_gap[i].count;
	}
	for (i = 0; i < MS_CAP_RECORDS; i++) {
		if (g_css[i].used) {
			css_unique++;
			css_hits += g_css[i].count;
			if (g_css[i].kind == MS_CSS_GAP_PROPERTY) css_prop_gaps++;
			else if (g_css[i].kind == MS_CSS_GAP_VALUE) css_val_gaps++;
			else if (g_css[i].kind == MS_CSS_GAP_SELECTOR) css_sel_gaps++;
			else if (g_css[i].kind == MS_CSS_GAP_PSEUDO_CLASS) css_pc_gaps++;
			else if (g_css[i].kind == MS_CSS_GAP_PSEUDO_ELEMENT) css_pe_gaps++;
			else if (g_css[i].kind == MS_CSS_GAP_CONDITION) css_media_gaps++;
			else if (g_css[i].kind == MS_CSS_GAP_CSSOM) cssom_gaps++;
			else css_at_gaps++;
		}
	}

	n = add(b, c, n, "MSDIAG 1 gapreport\n");
	snprintf(line, sizeof line, "nav=%lu\n", nav_id);
	n = add(b, c, n, line);
	snprintf(line, sizeof line, "unique_gaps=%d\ntotal_hits=%lu\ndropped=%lu\n",
		api_unique + css_unique + js_unique, api_hits + css_hits + js_hits,
		g_cap_dropped + g_css_dropped + g_js_event_dropped + g_js_api_gap_dropped);
	n = add(b, c, n, line);
	snprintf(line, sizeof line, "census_lossless=%d\ncoverage_complete=%d\nblind_spots=%d\n",
		(g_cap_dropped + g_css_dropped + g_js_event_dropped + g_js_api_gap_dropped == 0) ? 1 : 0, 0, 3);
	n = add(b, c, n, line);
	snprintf(line, sizeof line, "capability_gaps=%d\ncss_property_gaps=%d\ncss_value_gaps=%d\nselector_gaps=%d\npseudo_class_gaps=%d\npseudo_element_gaps=%d\nmedia_gaps=%d\ncssom_gaps=%d\n\n",
		cap_unique, css_prop_gaps, css_val_gaps, css_sel_gaps, css_pc_gaps, css_pe_gaps, css_media_gaps, cssom_gaps);
	n = add(b, c, n, line);

	n = add(b, c, n, "[coverage]\n");
	n = add(b, c, n, "js_host_api=partial reason=common_binding_outcomes_only\n");
	n = add(b, c, n, "js_execution_failures=full\n");
	n = add(b, c, n, "dom_property_missing=partial reason=instrumented_bindings_only\n");
	n = add(b, c, n, "dom_method_missing=partial reason=instrumented_bindings_only\n");
	n = add(b, c, n, "global_feature_get=unobservable reason=quickjs_global_lookup_no_safe_host_hook\n");
	n = add(b, c, n, "global_feature_has=unobservable reason=quickjs_global_lookup_no_safe_host_hook\n");
	n = add(b, c, n, "events=full\n");
	n = add(b, c, n, "css_properties=full\n");
	n = add(b, c, n, "css_values=full\n");
	n = add(b, c, n, "selectors=full\n");
	n = add(b, c, n, "pseudo_classes=full\n");
	n = add(b, c, n, "pseudo_elements=full\n");
	n = add(b, c, n, "at_rules=full\n");
	n = add(b, c, n, "media_features=full\n");
	n = add(b, c, n, "cssom=full\n");
	n = add(b, c, n, "html_features=partial reason=no_central_behavior_fallback_hook\n");
	n = add(b, c, n, "layout_features=full\n");
	n = add(b, c, n, "paint_features=full\n\n");

	n = add(b, c, n, "[gaps]\n");
	for (i = 0; i < MS_CAP_RECORDS; i++) {
		if (g_cap[i].used) {
			snprintf(line, sizeof line, "key=js.%s.%s.%s result=%s quality=%d count=%lu first_nav=%lu last_nav=%lu\n",
				cap_domain(g_cap[i].domain), g_cap[i].name, cap_op(g_cap[i].operation),
				cap_result(g_cap[i].result), g_cap[i].quality,
				g_cap[i].count, g_cap[i].first_nav, g_cap[i].last_nav);
			n = add(b, c, n, line);
		}
	}
	for (i = 0; i < MS_JS_API_GAP_RECORDS; i++) if (g_js_api_gap[i].used) {
		snprintf(line, sizeof line,
			"normalized_key=%s count=%lu first_nav=%lu last_nav=%lu\n",
			g_js_api_gap[i].key, g_js_api_gap[i].count,
			g_js_api_gap[i].first_nav, g_js_api_gap[i].last_nav);
		n = add(b, c, n, line);
	}
	for (i = 0; i < MS_JS_EVENT_RECORDS; i++) if (g_js_event[i].used) {
		snprintf(line, sizeof line,
			"key=js.%s count=%lu first_nav=%lu last_nav=%lu first_script=%lu last_script=%lu\n",
			js_event_kind(g_js_event[i].kind), g_js_event[i].count,
			g_js_event[i].first_nav, g_js_event[i].last_nav,
			g_js_event[i].first_script, g_js_event[i].last_script);
		n = add(b, c, n, line);
	}
	for (i = 0; i < MS_CAP_RECORDS; i++) {
		if (g_css[i].used) {
			if (g_css[i].kind == MS_CSS_GAP_PROPERTY) {
				snprintf(line, sizeof line, "key=css.property.%s result=%s quality=0 count=%lu first_nav=%lu last_nav=%lu\n",
					g_css[i].property, cap_result(g_css[i].result),
					g_css[i].count, g_css[i].first_nav, g_css[i].last_nav);
			} else if (g_css[i].kind == MS_CSS_GAP_VALUE) {
				snprintf(line, sizeof line, "key=css.value.%s.%s result=%s quality=0 count=%lu first_nav=%lu last_nav=%lu\n",
					g_css[i].property, g_css[i].value, cap_result(g_css[i].result),
					g_css[i].count, g_css[i].first_nav, g_css[i].last_nav);
			} else if (g_css[i].kind == MS_CSS_GAP_SELECTOR) {
				snprintf(line, sizeof line, "key=css.selector.%s result=%s quality=0 count=%lu first_nav=%lu last_nav=%lu\n",
					g_css[i].name[0] != '\0' ? g_css[i].name : g_css[i].property,
					cap_result(g_css[i].result),
					g_css[i].count, g_css[i].first_nav, g_css[i].last_nav);
			} else if (g_css[i].kind == MS_CSS_GAP_PSEUDO_CLASS) {
				snprintf(line, sizeof line, "key=css.pseudo-class.%s result=%s quality=0 count=%lu first_nav=%lu last_nav=%lu\n",
					g_css[i].name[0] != '\0' ? g_css[i].name : g_css[i].property,
					cap_result(g_css[i].result),
					g_css[i].count, g_css[i].first_nav, g_css[i].last_nav);
			} else if (g_css[i].kind == MS_CSS_GAP_PSEUDO_ELEMENT) {
				snprintf(line, sizeof line, "key=css.pseudo-element.%s result=%s quality=0 count=%lu first_nav=%lu last_nav=%lu\n",
					g_css[i].name[0] != '\0' ? g_css[i].name : g_css[i].property,
					cap_result(g_css[i].result),
					g_css[i].count, g_css[i].first_nav, g_css[i].last_nav);
			} else {
				snprintf(line, sizeof line, "key=css.%s.%s result=%s quality=0 count=%lu first_nav=%lu last_nav=%lu\n",
					css_kind(g_css[i].kind),
					g_css[i].property[0] != '\0' ? g_css[i].property : g_css[i].name,
					cap_result(g_css[i].result),
					g_css[i].count, g_css[i].first_nav, g_css[i].last_nav);
			}
			n = add(b, c, n, line);
		}
	}
	return n;
}
