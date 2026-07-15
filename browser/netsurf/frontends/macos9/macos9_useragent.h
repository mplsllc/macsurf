/*
 * MacSurf - macos9_useragent.h
 *
 * fixes368 (#167) — per-host User-Agent selection (the Classilla
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

/* MacSurf's honest default User-Agent (every host not in the override
 * table). Exposed so callers / diagnostics can reference it. */
const char *macos9_user_agent_default(void);

/* Returns the User-Agent string to send to `host`. Suffix-matches the
 * override table on a dot boundary (so mbasic./m./www./touch. subdomains
 * are covered but spoof hosts like "evilfacebook.com" are not). `host` may
 * be NULL (returns the default). The returned string is static storage and
 * must not be freed or modified; never NULL. */
const char *macos9_user_agent_for_host(const char *host);

/* fixes835 (#167 M1) — request-header helpers (shared, impl in
 * macos9_fetch.c). See that file for the drop-list contract. */
int  macos9_hdr_has_ci(const char *hay, const char *needle);
void macos9_capture_extra_headers(const char **h, char *dst, size_t cap);

#endif /* MACOS9_USERAGENT_H */
