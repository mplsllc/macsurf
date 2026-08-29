/* MacSurf - bounded capability/CSS gap aggregates and gapreport census.
 * No allocation or formatting occurs at a hit site. */
#include <stdio.h>
#include <string.h>

#include "macsurf_capability.h"
#include "macsurf_diag.h"

#define MS_CAP_RECORDS 128
#define MS_CAP_TEXT 48

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

static struct ms_cap_record g_cap[MS_CAP_RECORDS];
static struct ms_css_record g_css[MS_CAP_RECORDS];
static unsigned long g_cap_dropped, g_css_dropped;
static unsigned long g_cap_drop_domain[9], g_css_drop_kind[8];

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

static void copy_text(char *out, const char *in)
{
	int i;
	if (in == NULL) in = "";
	for (i = 0; i < MS_CAP_TEXT - 1 && in[i] != '\0'; i++) out[i] = in[i];
	out[i] = '\0';
}

void ms_diag_capability_hit(int domain, int operation, const char *name,
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
		r->first_nav = ms_diag_cur_nav(); r->first_script = ms_diag_cur_script();
		r->first_task = ms_diag_cur_task();
	}
	if (r == NULL) { g_cap_dropped++; if (domain >= 0 && domain < 9) g_cap_drop_domain[domain]++; return; }
	r->count++;
	r->last_nav = ms_diag_cur_nav(); r->last_script = ms_diag_cur_script();
	r->last_task = ms_diag_cur_task();
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
	int cap_unique = 0, css_unique = 0;
	int css_prop_gaps = 0, css_val_gaps = 0, css_sel_gaps = 0;
	int css_pc_gaps = 0, css_pe_gaps = 0, css_at_gaps = 0;
	int css_media_gaps = 0, cssom_gaps = 0;
	unsigned long cap_hits = 0, css_hits = 0;
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
		cap_unique + css_unique, cap_hits + css_hits, g_cap_dropped + g_css_dropped);
	n = add(b, c, n, line);
	snprintf(line, sizeof line, "census_lossless=%d\ncoverage_complete=%d\nblind_spots=%d\n",
		(g_cap_dropped + g_css_dropped == 0) ? 1 : 0, 0, 2);
	n = add(b, c, n, line);
	snprintf(line, sizeof line, "capability_gaps=%d\ncss_property_gaps=%d\ncss_value_gaps=%d\nselector_gaps=%d\npseudo_class_gaps=%d\npseudo_element_gaps=%d\nmedia_gaps=%d\ncssom_gaps=%d\n\n",
		cap_unique, css_prop_gaps, css_val_gaps, css_sel_gaps, css_pc_gaps, css_pe_gaps, css_media_gaps, cssom_gaps);
	n = add(b, c, n, line);

	n = add(b, c, n, "[coverage]\n");
	n = add(b, c, n, "js_host_api=full\n");
	n = add(b, c, n, "dom_property_missing=full\n");
	n = add(b, c, n, "dom_method_missing=full\n");
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
