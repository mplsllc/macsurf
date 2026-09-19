/*
 * MacSurf - macos9_useragent.h
 *
 * fixes368 (#167)  -  per-host User-Agent selection (the Classilla
 * "sitecontrol" / TenFourFox per-site-override pattern, the proven
 * approach on this exact platform). Most sites get MacSurf's honest
 * default UA; a small override table maps specific hosts to a UA that
 * unlocks their lightweight surface.
 *
 * The motivating case is Facebook: a modern/MacSurf UA is 301-bounced to
 * the ~400 KB www SPA (unrenderable here), while a vintage
 * "Mozilla/4.0 ... Mac_PowerPC" string returns the ~6 KB pure-HTML
 * no-JavaScript mbasic page MacSurf renders natively. We override ONLY the
 * listed hosts so every other site keeps the default UA and cannot regress
 * (DIRECTIVE #5).
 *
 * This replaces the duplicated `macos9_ua_for_host` statics that fixes367
 * shipped in each fetcher. The implementation lives in macos9_fetch.c (an
 * existing build TU) so adding the module needs no MacSurf.mcp change; both
 * fetchers just include this header and call macos9_user_agent_for_host().
 */

#ifndef MACOS9_USERAGENT_H
#define MACOS9_USERAGENT_H

#include <stddef.h>	/* size_t (fixes835 header-capture helper) */

/* === Shared fetch buffer-size constants ==================================
 *
 * These were historically inline magic numbers in each fetcher, bumped
 * independently as header/cookie sizes grew.  Centralised here so a single
 * change reaches both the HTTP and HTTPS fetchers.
 *
 * Sizes chosen for worst-case real pages (Facebook's X-FB-* GraphQL set
 * overflows 512, a login 302 can carry 6+ Set-Cookie lines, etc.).
 * ======================================================================= */

#define MACSURF_HDR_BUF_MAX      65536  /* response header accumulation cap   */
#define MACSURF_READ_CHUNK        8192  /* OT read size per poll tick         */
#define MACSURF_COOKIE_HDR_CAP    6144  /* worst-case Cookie: request header  */
#define MACSURF_CALLER_HDRS_CAP   2048  /* XHR/Fetch extra request headers    */
#define MACSURF_REDIRECT_URL_CAP  1024  /* Location: header value buffer      */

/* User-editable per-site User-Agent rules. Kept deliberately small and
 * fixed-size for CW8 / Classic Mac OS: no allocator ownership crosses the
 * fetch/preferences boundary, and a corrupt preferences file cannot grow an
 * unbounded rule set. */
#define MACSURF_UA_USER_RULE_MAX  16
#define MACSURF_UA_HOST_MAX       128
#define MACSURF_UA_STRING_MAX     512

struct macos9_user_ua_rule {
	char suffix[MACSURF_UA_HOST_MAX];
	char ua[MACSURF_UA_STRING_MAX];
};

enum macos9_ua_preset {
	MACOS9_UA_PRESET_DEFAULT = 0,
	MACOS9_UA_PRESET_FIREFOX134,
	MACOS9_UA_PRESET_CHROME149,
	MACOS9_UA_PRESET_KAIOS25,
	MACOS9_UA_PRESET_CUSTOM,
	MACOS9_UA_PRESET_COUNT
};

/* === Function declarations ============================================= */

/* MacSurf's honest default User-Agent (every host not in the override
 * table). Exposed so callers / diagnostics can reference it. */
const char *macos9_user_agent_default(void);

/* Returns the User-Agent string to send to `host`. Suffix-matches the
 * override table on a dot boundary (so mbasic./m./www./touch. subdomains
 * are covered but spoof hosts like "evilfacebook.com" are not). `host` may
 * be NULL (returns the default). The returned string is static storage and
 * must not be freed or modified; never NULL. */
const char *macos9_user_agent_for_host(const char *host);

/* Runtime-editable rules are loaded from/saved to "MacSurf User Agents" in
 * MacSurfData. User rules are consulted before the built-in compatibility
 * table and therefore can intentionally override a built-in rule. */
void macos9_user_agent_rules_load(void);
void macos9_user_agent_rules_save(void);
const struct macos9_user_ua_rule *macos9_user_agent_user_rules(int *count);
int macos9_user_agent_user_rules_replace(
		const struct macos9_user_ua_rule *rules, int count);
int macos9_user_agent_normalize_host(const char *input, char *out, size_t cap);
const char *macos9_user_agent_preset_value(int preset);
int macos9_user_agent_preset_for_value(const char *ua);

/* fixes835 (#167 M1)  -  request-header helpers (shared, impl in
 * macos9_fetch.c). See that file for the drop-list contract. */
int  macos9_hdr_has_ci(const char *hay, const char *needle);

/* fixes1328 (#167) - Accept header chosen by request kind: the pre-WebP
 * document header for documents/XHR, an image header (advertising
 * image/webp) for image requests. See macos9_fetch.c. */
const char *macos9_accept_for_path(const char *path);
void macos9_capture_extra_headers(const char **h, char *dst, size_t cap);

/* Shared request-header helpers (cleanup 2026-08-05, impl in macos9_fetch.c).
 * Replace the ~30-line cookie assembly and ~35-line Sec-Fetch synthesis
 * blocks that were duplicated in both fetchers. */
void macos9_build_cookie_header(char *cookie_hdr, size_t cap,
		const char *cookie_str);
void macos9_build_sec_fetch(char *synth, size_t cap,
		int verifiable, int is_post,
		const char *scheme, const char *host);

/* Shared line-finder (cleanup 2026-08-05, impl in macos9_fetch.c).
 * Find next CRLF-terminated line in a buffer. NUL-terminates at '\r',
 * advances *buf past '\n', decrements *len. Returns start of line or NULL. */
char *macos9_find_line(char **buf, long *len);

#endif /* MACOS9_USERAGENT_H */
