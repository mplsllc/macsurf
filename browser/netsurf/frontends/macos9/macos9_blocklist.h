/*
 * MacSurf - macos9_blocklist.h
 *
 * fixes856 (#285)  -  tracker / ad-network host blocklist.
 *
 * RATIONALE (maintainer directive, 2026-07-16): "we need to block trackers and
 * known ad systems as a general rule - if only because of the drag they cause
 * (small stuff like umami is ok)".  This is a PERFORMANCE feature first.  On a
 * G3/G4 a tracker costs three times over: the bytes on the wire, the QuickJS
 * parse+exec of a bundle written for a 2026 desktop CPU, and a fetch slot held
 * open against a host we do not care about.
 *
 * Measured on hackaday.com (one article page, MacSurf's real UA, live 2026-07-16):
 *   total subresources                          2406 KB / 29 requests
 *   googletagmanager.com/gtag/js                 475 KB  <- ONE request, 20%
 *   web.cmp.usercentrics.eu/ui/loader.js          55 KB  -> then pulls
 *     WebSdk.lib (312 KB) + UsCmpController (35 KB) + CMP JSON (31 KB)
 *   ------------------------------------------------------------------
 *   tracker/consent total                       ~908 KB  = ~38% of the page
 * None of it draws a single pixel of article content.
 *
 * POLICY  -  what goes in the table:
 *   BLOCK: analytics suites, ad exchanges/servers, consent-management
 *          platforms (a CMP banner is pure drag on OS 9), session recorders,
 *          and tag managers.
 *   ALLOW: small/privacy-respecting, self-hostable analytics  -  umami,
 *          plausible, fathom, matomo, goatcounter, simpleanalytics, and
 *          Cloudflare Insights.  Per the directive, "small stuff like umami is
 *          ok".  They are a few KB, they do not fingerprint, and macsurf.org's
 *          own stats.mp.ls is umami.  Blocking is an explicit list, so
 *          anything not named here is allowed by default and nothing is
 *          blocked by accident.
 *
 * DELIBERATELY NOT BLOCKED (would break live work  -  do not "tidy" these in):
 *   - facebook.net / fbcdn.net / fbsbx.com  -  #167 is active and the per-host
 *     UA table (macos9_useragent.h) drives real facebook.com asset loads
 *     through exactly these origins.  A tracking-pixel suffix like
 *     connect.facebook.net would match the same tail and take the real site
 *     with it.
 *   - s0.wp.com / s1.wp.com / i0.wp.com  -  these serve REAL Jetpack JS/CSS and
 *     the Photon image CDN.  Only stats.wp.com / pixel.wp.com are telemetry.
 *   - jetpack.wordpress.com  -  hosts the comment form iframe (real UI).
 *
 * The match is a dot-boundary host SUFFIX, sharing the semantics (and the
 * spoof-guard) of macos9_user_agent_for_host(): "doubleclick.net" matches
 * ad.doubleclick.net but NOT evildoubleclick.net.
 *
 * The implementation lives in macos9_fetch.c  -  an existing build TU  -  so
 * adding this costs NO MacSurf.mcp change; both fetchers just include this
 * header, exactly the arrangement fixes368 used for the UA table.
 */

#ifndef MACOS9_BLOCKLIST_H
#define MACOS9_BLOCKLIST_H

/* Returns 1 if `host` is a known tracker / ad / consent-management origin that
 * MacSurf should refuse to fetch, else 0.  `host` may be NULL (returns 0).
 * Suffix-matched on a dot boundary; see the policy note above for what is in
 * the table and, just as importantly, what is deliberately left out. */
int macos9_host_is_tracker(const char *host);

#endif /* MACOS9_BLOCKLIST_H */
