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

/* MSL Carbon Console support — InstallConsole / RemoveConsole /
 * WriteCharsToConsole / ReadCharsFromConsole are normally provided
 * by the MSL "Console" support file. We stub them — MacSurf has no
 * stdio terminal; printf is silently dropped. */
short InstallConsole(short fd) { (void)fd; return 0; }
void  RemoveConsole(void) {}
long  WriteCharsToConsole(char *buf, long n) { (void)buf; return n; }
long  ReadCharsFromConsole(char *buf, long n) { (void)buf; (void)n; return 0; }

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

/* POSIX functions MSL Carbon doesn't ship. Implementations are
 * minimal but functional. */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

char *strdup(const char *s) {
	size_t n; char *r;
	if (!s) return NULL;
	n = strlen(s) + 1;
	r = malloc(n);
	if (r) memcpy(r, s, n);
	return r;
}

int strcasecmp(const char *s1, const char *s2) {
	while (*s1 && *s2) {
		int c1 = tolower((unsigned char)*s1);
		int c2 = tolower((unsigned char)*s2);
		if (c1 != c2) return c1 - c2;
		s1++; s2++;
	}
	return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncasecmp(const char *s1, const char *s2, unsigned long n) {
	while (n-- > 0 && *s1 && *s2) {
		int c1 = tolower((unsigned char)*s1);
		int c2 = tolower((unsigned char)*s2);
		if (c1 != c2) return c1 - c2;
		s1++; s2++;
	}
	if (n == (unsigned long)-1) return 0;
	return (unsigned char)*s1 - (unsigned char)*s2;
}

float strtof(const char *str, char **endptr) {
	return (float)strtod(str, endptr);
}

/* mkdir / stat — file-system helpers used by ns_file.c posix paths.
 * MacSurf relies on a backing store cap of zero plus no-disk-cache;
 * stub returns non-existent so callers fall through. */
int mkdir(const char *path, unsigned long mode) {
	(void)path; (void)mode;
	return -1;
}
struct stat;
int stat(const char *path, struct stat *buf) {
	(void)path; (void)buf;
	return -1; /* always "doesn't exist" */
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
