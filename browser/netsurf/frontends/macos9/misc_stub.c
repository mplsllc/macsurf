/*
 * MacSurf - misc_stub.c
 * Stub implementations for NetSurf subsystems whose real .c files
 * are not yet linked into the project. Anything that has a real
 * implementation linked is removed from here to avoid duplicate
 * symbol errors.
 * Licensed under GPL v2.
 */

#include <stddef.h>
#include "utils/ns_errors.h"

struct nsurl;
struct bitmap;
struct cert_chain;
struct download_context;
struct gui_download_window;
struct llcache_handle;
struct image_cache_parameters;

/* fetch_poll - NetSurf core expects a fetcher polling tick.
 * Our HTTP fetcher polls itself via macos9_http_fetcher_active() in
 * the event loop, so the core's fetch_poll is just a stub. */
void fetch_poll(void *unused) { (void)unused; }

/* netsurf_poll - desktop layer polling tick. No-op on Mac OS 9; our
 * cooperative event loop drives reformat / scheduler directly. */
void netsurf_poll(void) {}

/* MSL Console support - InstallConsole / RemoveConsole /
 * WriteCharsToConsole / ReadCharsFromConsole are provided by
 * MSL_All_Carbon.Lib. Do not stub them - our stubs would shadow
 * MSL's real implementations and break __start's stdio init,
 * preventing main() from ever running. */

/* image_cache_init / image_cache_fini - image content handler files
 * not yet linked. */
nserror image_cache_init(const struct image_cache_parameters *p)
{
	(void)p;
	return NSERROR_OK;
}

nserror image_cache_fini(void) { return NSERROR_OK; }

/* DOM namespace - NetSurf core calls dom_namespace_initialise (public).
 * libdom's namespace.c provides dom_namespace_finalise; the public
 * dom_namespace_initialise has no real definition we link, so stub it. */
nserror dom_namespace_initialise(void) { return NSERROR_OK; }

/* textplain_init (#233) lives in the REAL handler,
 * content/handlers/text/textplain.c - it must be in MacSurf.mcp.
 * This stub was removed (fixes1137) so the real one links without a
 * duplicate-symbol collision in CW8's flat namespace.
 * image_init lives in macos9_image.c (fixes78 -- QuickTime Graphics
 * Importers handler). */

/* nsutils base64 - used only by ssl_certs.c for cert query strings.
 * MacSurf handles TLS natively via macTLS, so cert-chain queries never fire.
 * Returns BAD_INPUT (NSUERROR=2). */
int nsu_base64_encode_url(const unsigned char *input, unsigned long input_length,
		unsigned char **output, unsigned long *output_length)
{
	(void)input; (void)input_length; (void)output; (void)output_length;
	return 2;
}

/* nsutils monotonic clock - Carbon TickCount (1/60s), used for layout-cycle
 * deadlines (content reformat_time + the html/object.c min_reflow_period
 * throttle).
 *
 * fixes409 - this was doubly broken and was the root cause of the "reflow
 * storm" (a full page relayout on nearly every subresource arrival, ~31
 * reformats per page, mactrove layout-done ~52s):
 *
 *   1. WRONG GUARD. The real implementation was gated on __MACOS__, which
 *      CodeWarrior does NOT define (the project defines __MACOS9__). So the
 *      Mac build compiled the no-op #else stub and never wrote *now at all.
 *   2. WRONG SIGNATURE. Both variants took 'unsigned long *' (32-bit), but
 *      every caller (object.c, html.c) passes a 64-bit nsutils_ms_t
 *      (uint64_t). On big-endian PPC a 32-bit write lands in the HIGH word,
 *      leaving the low word as stack garbage.
 *
 * Either way ms_now came back garbage, so 'ms_now > reformat_time' was
 * effectively random and the throttle never engaged. Fix: correct guard
 * (__MACOS9__) and the full 64-bit signature/write. The ms value is computed
 * in 32-bit (it fits for any sane uptime, and this dodges the CW8 PPC
 * long-long multiply-by-constant miscompile) then widened on store.
 *
 * nsutils_ms_t is 'unsigned long long' (see nsutils/time.h); we spell it out
 * here rather than include the header to avoid an access-path dependency. */
#ifdef __MACOS9__
extern unsigned long TickCount(void);
int nsu_getmonotonic_ms(unsigned long long *now)
{
	if (now != NULL) {
		*now = (unsigned long long)(TickCount() * 1000UL / 60UL);
	}
	return 0;
}
#else
int nsu_getmonotonic_ms(unsigned long long *now)
{
	(void)now; return 0;
}
#endif

/* libcss helpers that upstream libcss puts in utils.c - we don't have
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
 * mkdir, stat, uname - DO NOT shadow them here.
 * Only strtof is genuinely missing. */
#include <stdlib.h>

float strtof(const char *str, char **endptr) {
	return (float)strtod(str, endptr);
}

/* strtold lives in MSL_C_Carbon.Lib - do not stub it here or the
 * linker raises a "previously defined" warning and ignores the lib
 * copy. (Earlier rounds stubbed strtold when MSL_All_Carbon.Lib's
 * strtold.o reported as Invalid object code; after swapping to
 * MSL_C_Carbon.Lib the stub became redundant and conflicts.) */

/* MSL_C_Carbon.Lib doesn't ship strdup / strcasecmp / strncasecmp -
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
 * We don't need to compute the "set" branch fields - callers in
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

/* css__number_from_lwc_string - provided by libcss/src/utils/css_utils.c. */

/* === fixes12 additions: previously-missing globals === */

/* lwc_string_caseless_hash_value - moved to macsurf_lwc_compat.c
 * (2026-08-05 cleanup).  That function was the single most critical
 * piece of code in this file: a wrong hash broke every CSS selector
 * in every parsed sheet.  It now lives in its own TU so it is
 * discoverable and auditable on its own. */

/* clamp - NetSurf utility, may not be linked */
int clamp(int v, int lo, int hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

/* NOF_ELEMENTS is a macro defined in macsurf_prefix.h.
 * Variable definition removed - was causing html_init to call 0(html_types). */

/* strcasestr, strndup, cnv_space2nbsp, squash_whitespace, inet_aton, vsnstrjoin
 * were once stubs here. They now live in netsurf/utils/utils.c, which is part
 * of the project. The stub copies were removed in fixes63 to resolve
 * multiply-defined link errors. */

/* nsmkdir / stat - POSIX stubs. mkdir not actually meaningful on OS 9
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

/* nsu_base64_encode_url - already defined at line 58 above. */

/* Console stubs for MSL_C_Carbon.Lib's __read_console / __write_console.
 * MacSurf has no console UI; the runtime needs these symbols defined. */
short InstallConsole(short fd)        { (void)fd; return 0; }
void  RemoveConsole(void)             { }
long  WriteCharsToConsole(char *buf, long n) { (void)buf; return n; }
long  ReadCharsFromConsole(char *buf, long n) { (void)buf; (void)n; return 0; }
