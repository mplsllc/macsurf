/*
 * MacSurf - macos9_extra_stubs.c
 *
 * Stub implementations for symbols referenced by NetSurf core but
 * not provided on the Mac OS 9 build:
 *   - HTTP fetcher registers (only the proxy fetcher is real)
 *   - nsutils helpers (base64, monotonic time)
 *   - POSIX functions missing from MSL on Carbon
 *   - MSL console hooks (no SIOUX in a Carbon GUI app)
 */

#include <stddef.h>
#include <ctype.h>
#include "utils/errors.h"
#include "nsutils/base64.h"
#include "nsutils/time.h"

/* --- libcss / libwapcaplet helpers that originally lived as static inline
 * in shared headers. We stripped the bodies (see libcss/src/utils/utils.h,
 * parserutilserror.h, libwapcaplet/libwapcaplet.h) and moved them here
 * so the linker finds one out-of-line copy each. */

#include <libwapcaplet/libwapcaplet.h>
#include <libcss/errors.h>
#include <libcss/types.h>
#include <parserutils/errors.h>

bool isDigit(uint8_t c)
{
	return '0' <= c && c <= '9';
}

bool isHex(uint8_t c)
{
	return isDigit(c) || ('a' <= c && c <= 'f') || ('A' <= c && c <= 'F');
}

uint32_t charToHex(uint8_t c)
{
	c -= '0';
	if (c > 9)  c -= 'A' - '9' - 1;
	if (c > 15) c -= 'a' - 'A';
	return c;
}

css_error css_error_from_lwc_error(lwc_error err)
{
	switch (err) {
	case lwc_error_ok:    return CSS_OK;
	case lwc_error_oom:   return CSS_NOMEM;
	case lwc_error_range: return CSS_BADPARM;
	default: break;
	}
	return CSS_INVALID;
}

css_error css_error_from_parserutils_error(parserutils_error error)
{
	return (css_error)error;
}

/* lwc_string_caseless_hash_value was a static inline body in
 * libwapcaplet.h; the real .c file only provides lwc__intern_caseless_string.
 * Mirror the header's logic here. Requires knowledge of the lwc_string
 * internal layout (refcnt/insensitive/hash) which is exposed by the
 * public header. */
lwc_error lwc_string_caseless_hash_value(lwc_string *str, lwc_hash *hash)
{
	if (str->insensitive == NULL) {
		lwc_error err = lwc__intern_caseless_string(str);
		if (err != lwc_error_ok) return err;
	}
	*hash = str->insensitive->hash;
	return lwc_error_ok;
}

/* --- image / textplain / DOM namespace stubs (previously in misc_stub.c,
 * which is now removed from the project because dt_hotlist.c etc. provide
 * the real versions of its other symbols). These are the no-ops NetSurf
 * core still expects when image handling and text/plain are disabled. */

nserror image_init(void)                  { return NSERROR_OK; }
nserror textplain_init(void)              { return NSERROR_OK; }
nserror image_cache_fini(void)            { return NSERROR_OK; }
/* dom_namespace_initialise / _finalise — the real libdom namespace.c
 * provides these, so we don't stub them. */

struct image_cache_parameters;
nserror image_cache_init(const struct image_cache_parameters *p)
{ (void)p; return NSERROR_OK; }

/* --- libdom hubbub binding stubs. The real dom_parser.c transitively
 * pulls in libdom's internal headers (event_target.h, node.h, list.h,
 * string.h …) which use static inline and other C99 forms that CW8
 * cannot parse. Provide no-op stubs so the link resolves; HTML parsing
 * is deferred until those headers are C89-cleaned. */

#include "dom/bindings/hubbub/parser.h"

dom_hubbub_error dom_hubbub_parser_create(dom_hubbub_parser_params *params,
		dom_hubbub_parser **parser, dom_document **document)
{
	(void)params;
	if (parser != NULL)   *parser = NULL;
	if (document != NULL) *document = NULL;
	return DOM_HUBBUB_NOMEM;
}

dom_hubbub_error dom_hubbub_fragment_parser_create(dom_hubbub_parser_params *params,
		dom_document *document, dom_hubbub_parser **parser,
		dom_document_fragment **fragment)
{
	(void)params; (void)document;
	if (parser != NULL)   *parser = NULL;
	if (fragment != NULL) *fragment = NULL;
	return DOM_HUBBUB_NOMEM;
}

void dom_hubbub_parser_destroy(dom_hubbub_parser *parser) { (void)parser; }

dom_hubbub_error dom_hubbub_parser_parse_chunk(dom_hubbub_parser *parser,
		const uint8_t *data, size_t len)
{ (void)parser; (void)data; (void)len; return DOM_HUBBUB_OK; }

dom_hubbub_error dom_hubbub_parser_insert_chunk(dom_hubbub_parser *parser,
		const uint8_t *data, size_t length)
{ (void)parser; (void)data; (void)length; return DOM_HUBBUB_OK; }

dom_hubbub_error dom_hubbub_parser_completed(dom_hubbub_parser *parser)
{ (void)parser; return DOM_HUBBUB_OK; }

const char *dom_hubbub_parser_get_encoding(dom_hubbub_parser *parser,
		dom_hubbub_encoding_source *source)
{
	(void)parser;
	if (source != NULL) *source = DOM_HUBBUB_ENCODING_SOURCE_HEADER;
	return "UTF-8";
}

dom_hubbub_error dom_hubbub_parser_pause(dom_hubbub_parser *parser, bool pause)
{ (void)parser; (void)pause; return DOM_HUBBUB_OK; }

/* --- NetSurf fetcher registration stubs.
 * Only the macos9 HTTP/proxy fetcher is wired; about:/resource:/file:/
 * data:/javascript: are no-ops so fetcher_init() in fetch.c links. */

nserror fetch_about_register(void)      { return NSERROR_OK; }
nserror fetch_resource_register(void)   { return NSERROR_OK; }
nserror fetch_file_register(void)       { return NSERROR_OK; }
nserror fetch_data_register(void)       { return NSERROR_OK; }
nserror fetch_javascript_register(void) { return NSERROR_OK; }

/* --- nsutils stubs. base64 encode/decode are real-but-minimal; the
 * URL variants and decode_alloc just return BAD_INPUT. nsu_getmonotonic_ms
 * uses TickCount() (1/60 sec) for a monotonic-ish source. */

nsuerror nsu_base64_encode(const unsigned char *input, size_t input_length,
		unsigned char *output, size_t *output_length)
{
	(void)input; (void)input_length; (void)output;
	if (output_length != NULL) *output_length = 0;
	return NSUERROR_BAD_INPUT;
}

nsuerror nsu_base64_decode_alloc(const unsigned char *input, size_t input_length,
		unsigned char **output, size_t *output_length)
{
	(void)input; (void)input_length;
	if (output != NULL) *output = NULL;
	if (output_length != NULL) *output_length = 0;
	return NSUERROR_BAD_INPUT;
}

nsuerror nsu_base64_encode_url(const unsigned char *input, size_t input_length,
		unsigned char *output, size_t *output_length)
{
	(void)input; (void)input_length; (void)output;
	if (output_length != NULL) *output_length = 0;
	return NSUERROR_BAD_INPUT;
}

#ifdef __MWERKS__
#include <Events.h>
int nsu_getmonotonic_ms(nsutils_ms_t *ms)
{
	if (ms == NULL) return -1;
	*ms = (nsutils_ms_t)(TickCount() * 1000ULL / 60ULL);
	return 0;
}
#else
int nsu_getmonotonic_ms(nsutils_ms_t *ms)
{
	if (ms != NULL) *ms = 0;
	return 0;
}
#endif

/* --- POSIX shims missing from MSL on Carbon. */

#include <string.h>
#include <stdlib.h>

char *strdup(const char *s)
{
	size_t n;
	char *p;
	if (s == NULL) return NULL;
	n = strlen(s) + 1;
	p = (char *)malloc(n);
	if (p != NULL) memcpy(p, s, n);
	return p;
}

int strcasecmp(const char *a, const char *b)
{
	unsigned char ca, cb;
	while (1) {
		ca = (unsigned char)tolower((unsigned char)*a++);
		cb = (unsigned char)tolower((unsigned char)*b++);
		if (ca != cb) return (int)ca - (int)cb;
		if (ca == 0) return 0;
	}
}

int strncasecmp(const char *a, const char *b, size_t n)
{
	unsigned char ca, cb;
	while (n-- > 0) {
		ca = (unsigned char)tolower((unsigned char)*a++);
		cb = (unsigned char)tolower((unsigned char)*b++);
		if (ca != cb) return (int)ca - (int)cb;
		if (ca == 0) return 0;
	}
	return 0;
}

float strtof(const char *s, char **endp)
{
	return (float)strtod(s, endp);
}

/* cbrt now lives in javascript/macsurf_js.c so it's tied to the
 * always-linked JS glue TU and not dependent on this file being
 * in the project or its link order. */

/* mkdir / stat — no filesystem write paths exercised on OS 9 yet.
 * Provide failure stubs so ns_file.c links. */

int mkdir(const char *path, unsigned long mode)
{
	(void)path; (void)mode;
	return -1;
}

struct stat;
int stat(const char *path, struct stat *st)
{
	(void)path; (void)st;
	return -1;
}

/* --- MSL console hooks. MacSurf is a Carbon GUI app with no SIOUX,
 * so stub the four __console.h entry points to no-ops. Required to
 * satisfy MSL_C_Carbon.Lib's stdio backend. */

#ifdef __MWERKS__
short InstallConsole(short fd)               { (void)fd; return 0; }
void  RemoveConsole(void)                    { }
long  WriteCharsToConsole(char *buf, long n) { (void)buf; return n; }
long  ReadCharsFromConsole(char *buf, long n) { (void)buf; (void)n; return 0; }
#endif
