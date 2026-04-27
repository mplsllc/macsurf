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

/* css__number_from_lwc_string — convert lwc_string contents to a fixed
 * via the byte-buffer helper that DOES exist (css__number_from_string). */
struct lwc_string_s_;
extern int css__number_from_string(const unsigned char *data, unsigned long len,
		int int_only, int *consumed);
int css__number_from_lwc_string(struct lwc_string_s_ *s, int int_only, int *consumed)
{
	const unsigned char *data; unsigned long len;
	if (!s) { if (consumed) *consumed = 0; return 0; }
	/* lwc_string layout: void *(refcnt), const char *data, size_t len, ...
	 * Be conservative — read via shim funcs. */
	{
		extern const char *lwc_string_data(struct lwc_string_s_ *);
		extern unsigned long lwc_string_length(struct lwc_string_s_ *);
		data = (const unsigned char *)lwc_string_data(s);
		len  = lwc_string_length(s);
	}
	return css__number_from_string(data, len, int_only, consumed);
}
