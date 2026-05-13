/*
 * MacSurf — misc_stub.c
 * Stub implementations for NetSurf subsystems whose real .c files
 * are not yet linked into the project. Anything that has a real
 * implementation linked is removed from here to avoid duplicate
 * symbol errors.
 * Licensed under GPL v2.
 */

#include <stddef.h>
#include "utils/errors.h"

struct nsurl;
struct bitmap;
struct cert_chain;
struct download_context;
struct gui_download_window;
struct llcache_handle;
struct image_cache_parameters;

/* fetch_poll — NetSurf core expects a fetcher polling tick.
 * Our HTTP fetcher polls itself via macos9_http_fetcher_active() in
 * the event loop, so the core's fetch_poll is just a stub. */
void fetch_poll(void *unused) { (void)unused; }

/* netsurf_poll — desktop layer polling tick. No-op on Mac OS 9; our
 * cooperative event loop drives reformat / scheduler directly. */
void netsurf_poll(void) {}

/* MSL Console support — InstallConsole / RemoveConsole /
 * WriteCharsToConsole / ReadCharsFromConsole are provided by
 * MSL_All_Carbon.Lib. Do not stub them — our stubs would shadow
 * MSL's real implementations and break __start's stdio init,
 * preventing main() from ever running. */

/* image_cache_init / image_cache_fini — image content handler files
 * not yet linked. */
nserror image_cache_init(const struct image_cache_parameters *p)
{
	(void)p;
	return NSERROR_OK;
}

nserror image_cache_fini(void) { return NSERROR_OK; }

/* DOM namespace — NetSurf core calls dom_namespace_initialise (public).
 * libdom's namespace.c provides dom_namespace_finalise; the public
 * dom_namespace_initialise has no real definition we link, so stub it. */
nserror dom_namespace_initialise(void) { return NSERROR_OK; }

/* Content handler init — image / textplain handlers not yet linked. */
nserror textplain_init(void) { return NSERROR_OK; }
nserror image_init(void) { return NSERROR_OK; }

/* nsutils base64 — used only by ssl_certs.c for cert query strings.
 * MacSurf strips TLS at the proxy, so cert chain queries never fire.
 * Returns BAD_INPUT (NSUERROR=2). */
int nsu_base64_encode_url(const unsigned char *input, unsigned long input_length,
		unsigned char **output, unsigned long *output_length)
{
	(void)input; (void)input_length; (void)output; (void)output_length;
	return 2;
}

/* nsutils monotonic clock — Carbon TickCount (1/60s), good enough
 * for layout-cycle deadlines. */
#ifdef __MACOS__
extern unsigned long TickCount(void);
unsigned long nsu_getmonotonic_ms(unsigned long *now)
{
	unsigned long t = TickCount() * 1000UL / 60UL;
	if (now) *now = t;
	return 0;
}
#else
unsigned long nsu_getmonotonic_ms(unsigned long *now)
{
	(void)now; return 0;
}
#endif

/* libcss helpers that upstream libcss puts in utils.c — we don't have
 * that file in our libcss source tree, so define them here. */
#include <stdint.h>
typedef int css_error_t_;
typedef int lwc_error_t_;
typedef int parserutils_error_t_;
int isDigit(unsigned char c) { return c >= '0' && c <= '9'; }
int isHex(unsigned char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
unsigned long charToHex(unsigned char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return 0;
}
/* css_error: 0=CSS_OK 1=CSS_NOMEM 2=CSS_BADPARM 3=CSS_INVALID
 * lwc_error: 0=ok 1=oom 2=range
 * parserutils_error: 0=ok 1=nomem 2=badparm 3=invalid 4=filenotfound
 *                    5=needdata 6=badencoding 7=eof */
int css_error_from_lwc_error(int err) {
	switch (err) { case 0: return 0; case 1: return 1; default: return 2; }
}
int css_error_from_parserutils_error(int err) {
	switch (err) { case 0: return 0; case 1: return 1; case 2: return 2;
		case 3: return 3; case 4: return 4; case 5: return 5;
		default: return 3; }
}

/* MSL_All_Carbon.Lib provides strdup, strcasecmp, strncasecmp,
 * mkdir, stat, uname — DO NOT shadow them here.
 * Only strtof is genuinely missing. */
#include <stdlib.h>

float strtof(const char *str, char **endptr) {
	return (float)strtod(str, endptr);
}

/* strtold lives in MSL_C_Carbon.Lib — do not stub it here or the
 * linker raises a "previously defined" warning and ignores the lib
 * copy. (Earlier rounds stubbed strtold when MSL_All_Carbon.Lib's
 * strtold.o reported as Invalid object code; after swapping to
 * MSL_C_Carbon.Lib the stub became redundant and conflicts.) */

/* MSL_C_Carbon.Lib doesn't ship strdup / strcasecmp / strncasecmp —
 * stub them here so the swap from MSL_All_Carbon is drop-in. */
char *strdup(const char *s) {
	size_t n;
	char *r;
	if (s == NULL) return NULL;
	n = strlen(s);
	r = (char *)malloc(n + 1);
	if (r == NULL) return NULL;
	memcpy(r, s, n + 1);
	return r;
}

int strcasecmp(const char *a, const char *b) {
	unsigned char ca, cb;
	for (;;) {
		ca = (unsigned char)*a++;
		cb = (unsigned char)*b++;
		if (ca >= 'A' && ca <= 'Z') ca += 32;
		if (cb >= 'A' && cb <= 'Z') cb += 32;
		if (ca != cb) return (int)ca - (int)cb;
		if (ca == 0) return 0;
	}
}

int strncasecmp(const char *a, const char *b, size_t n) {
	unsigned char ca, cb;
	while (n-- > 0) {
		ca = (unsigned char)*a++;
		cb = (unsigned char)*b++;
		if (ca >= 'A' && ca <= 'Z') ca += 32;
		if (cb >= 'A' && cb <= 'Z') cb += 32;
		if (ca != cb) return (int)ca - (int)cb;
		if (ca == 0) return 0;
	}
	return 0;
}

/* CW8 strips `inline` (macsurf_prefix.h #define inline), leaving
 * `static` accessors in autogenerated_propget.h that some TUs reference
 * without inlining. Provide concrete external symbols mirroring the
 * static-inline bodies for the three properties libcss leaves needing
 * non-static linkage. Bit layouts match autogenerated_propget.h:430-735.
 *
 * border_radius: 7 bits = uuuuutt (unit:5, type:2)
 * box_shadow:    2 bits = tt (type)
 * macsurf_gradient: 2 bits = tt (type)
 *
 * We don't need to compute the "set" branch fields — callers in
 * s_*.c only reach this via copy/compose, and 0 fall-through is safe.
 */
struct css_computed_style_;
typedef struct css_computed_style_ css_computed_style__;

unsigned char get_border_radius(const void *style, int *length, int *unit)
{
	(void)style; (void)length; (void)unit;
	return 0; /* CSS_BORDER_RADIUS_INHERIT */
}
unsigned char get_box_shadow(const void *style, int *integer)
{
	(void)style; (void)integer;
	return 0;
}
unsigned char get_macsurf_gradient(const void *style, int *integer)
{
	(void)style; (void)integer;
	return 0;
}

/* css__number_from_lwc_string — provided by libcss/src/utils/css_utils.c. */

/* === fixes12 additions: previously-missing globals === */

#include <string.h>

/* lwc_string_caseless_hash_value — declared extern in libwapcaplet.h but
 * not defined in our libwapcaplet.c port. Use the real typedefs. */
#include <libwapcaplet/libwapcaplet.h>

lwc_error lwc_string_caseless_hash_value(lwc_string *str, lwc_hash *hash)
{
	const char *s;
	size_t i, len;
	lwc_hash h = 0;
	if (str == NULL || hash == NULL) return lwc_error_range;
	s = lwc_string_data(str);
	len = lwc_string_length(str);
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c >= 'A' && c <= 'Z') c += 32;
		h = (h * 31) + c;
	}
	*hash = h;
	return lwc_error_ok;
}

/* strcasestr — POSIX, not in MSL */
char *strcasestr(const char *haystack, const char *needle)
{
	size_t nl;
	if (!haystack || !needle) return NULL;
	nl = strlen(needle);
	if (nl == 0) return (char *)haystack;
	for (; *haystack; haystack++) {
		size_t i;
		for (i = 0; i < nl; i++) {
			char a = haystack[i], b = needle[i];
			if (a >= 'A' && a <= 'Z') a += 32;
			if (b >= 'A' && b <= 'Z') b += 32;
			if (a != b) break;
		}
		if (i == nl) return (char *)haystack;
	}
	return NULL;
}

/* strndup — POSIX, not in MSL */
#include <stdlib.h>
char *strndup(const char *s, size_t n)
{
	size_t l = 0; char *r;
	while (l < n && s[l]) l++;
	r = malloc(l + 1);
	if (!r) return NULL;
	memcpy(r, s, l);
	r[l] = '\0';
	return r;
}

/* clamp — NetSurf utility, may not be linked */
int clamp(int v, int lo, int hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

/* cnv_space2nbsp — convert ASCII space to non-breaking space (0xA0 in
 * Latin-1; we leave it as a passthrough no-op stub for now). */
char *cnv_space2nbsp(const char *s)
{
	return strdup(s);
}

/* squash_whitespace — collapse runs of whitespace into single spaces. */
char *squash_whitespace(const char *s)
{
	char *r, *w;
	int in_ws = 0;
	if (!s) return NULL;
	r = malloc(strlen(s) + 1);
	if (!r) return NULL;
	w = r;
	while (*s) {
		if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
			if (!in_ws) { *w++ = ' '; in_ws = 1; }
		} else {
			*w++ = *s; in_ws = 0;
		}
		s++;
	}
	*w = '\0';
	return r;
}

/* NOF_ELEMENTS is a macro defined in macsurf_prefix.h.
 * Variable definition removed — was causing html_init to call 0(html_types). */

/* inet_aton — POSIX, not in MSL */
struct in_addr_stub_ { unsigned long s_addr; };
int inet_aton(const char *cp, void *inp)
{
	unsigned long parts[4] = {0,0,0,0};
	int i;
	const char *p = cp;
	for (i = 0; i < 4 && *p; i++) {
		while (*p >= '0' && *p <= '9') { parts[i] = parts[i]*10 + (*p - '0'); p++; }
		if (i < 3) { if (*p != '.') return 0; p++; }
	}
	if (inp) ((struct in_addr_stub_ *)inp)->s_addr =
		(parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
	return 1;
}

/* vsnstrjoin — used by ns_file.c posix_vmkpath. Simple stub that
 * concatenates with separator. */
#include <stdarg.h>
nserror vsnstrjoin(char **str, size_t *size, char sep, size_t nelm, va_list ap)
{
	size_t totlen = 0, i;
	char *arg, *out, *p;
	va_list ap2;
	va_copy(ap2, ap);
	for (i = 0; i < nelm; i++) {
		arg = va_arg(ap, char *);
		if (arg) totlen += strlen(arg) + 1;
	}
	out = malloc(totlen + 1);
	if (!out) { va_end(ap2); return NSERROR_NOMEM; }
	p = out;
	for (i = 0; i < nelm; i++) {
		arg = va_arg(ap2, char *);
		if (!arg) continue;
		if (p != out) *p++ = sep;
		strcpy(p, arg); p += strlen(arg);
	}
	*p = '\0';
	va_end(ap2);
	*str = out;
	if (size) *size = totlen;
	return NSERROR_OK;
}

/* nsmkdir / stat — POSIX stubs. mkdir not actually meaningful on OS 9
 * without HFS plumbing; return success and let later open() decide. */
int nsmkdir(const char *path, int mode)
{
	(void)path; (void)mode;
	return 0;
}

#include "stat.h"
int stat(const char *path, struct stat *buf)
{
	(void)path; (void)buf;
	return -1; /* not implemented; caller falls back */
}

/* nsu_base64_encode_url — already defined at line 58 above. */

/* Console stubs for MSL_C_Carbon.Lib's __read_console / __write_console.
 * MacSurf has no console UI; the runtime needs these symbols defined. */
short InstallConsole(short fd)        { (void)fd; return 0; }
void  RemoveConsole(void)             { }
long  WriteCharsToConsole(char *buf, long n) { (void)buf; return n; }
long  ReadCharsFromConsole(char *buf, long n) { (void)buf; (void)n; return 0; }
