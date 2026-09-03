/*
 * MacSurf - macos9_fetch.c
 * HTTP fetch via Open Transport.
 *
 * Uses the same sync+blocking endpoint plus OTUseSyncIdleEvents(ep, true)
 * notifier-yield pattern as the Retro68 OT demos, but in MacSurf's current
 * Carbon build the endpoint is opened through OTOpenEndpointInContext()
 * with the shared macos9_ot_context from main.c.
 *
 * InitOpenTransportInContext() is called ONCE in main() - not per-fetch.
 * This file only opens and closes individual endpoints.
 *
 * Also provides HTML tag stripping and word wrapping used by window.c
 * to turn a fetch body into displayable text.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "utils/ns_errors.h"
#include "utils/log.h"
#include "netsurf/fetch.h"
#include "content/fetch.h"   /* fixes721: struct fetch_multipart_data */
#include "macos9.h"

#ifdef __MACOS9__
#include <OpenTransport.h>
#include <OpenTptInternet.h>
#include <Threads.h>
extern OTClientContextPtr macos9_ot_context;
#endif

#include "macos9_useragent.h"
#include "macos9_blocklist.h"

/* fixes368 (#167)  -  per-host User-Agent table (Classilla "sitecontrol"
 * pattern). See macos9_useragent.h. To add a site override, add a
 * { suffix, ua } row; the suffix is matched on a dot boundary against the
 * tail of the host. This is the single source of truth that replaced the
 * duplicated macos9_ua_for_host statics in the two fetchers. */
static const char MACOS9_UA_DEFAULT[] =
	"MacSurf/2.0.5 (Macintosh; PPC Mac OS 9)";

/* fixes841 (#167 Facebook)  -  ONE Facebook device. Every facebook.com origin
 * (www, m, apex) AND the fbcdn.net / facebook.net asset origins present the
 * SAME KaiOS feature-phone UA, so FB sees a single consistent device rather
 * than a new one per UA experiment (fixes835 FF134 desktop, fixes838 Firefox-
 * Android, fixes839 Galaxy-S5 each looked like a DIFFERENT device  -  that
 * re-triggered new-device checkpoints every time and made it impossible to
 * tell whether cookie persistence was working). KaiOS is the string that gets
 * furthest end-to-end: light plain-HTML login form renders, login POST is
 * accepted (302), and the m two_factor checkpoint returns 200 HTML. www under
 * KaiOS 301s to the m mobile surface  -  fine, that's where the checkpoint
 * actually renders (www's is a JS-only 404 route). The "LYF Jio F90M" device
 * label is cosmetic; the device is remembered by the persistent datr cookie
 * (fixes838), not the UA. Do NOT append " MacSurf/..."  -  FB's KaiOS gate is
 * exact. The FF134 desktop-shell UA (fixes835) lives in git history if we
 * revisit the desktop path once login/persistence is nailed down. */
static const char MACOS9_UA_FB_KAIOS[] =
	"Mozilla/5.0 (Mobile; LYF/F90M/LYF-F90M-000-02-44-130319; rv:48.0) Gecko/48.0 Firefox/48.0 KAIOS/2.5";

/* fixes842 (#167 Facebook)  -  desktop Firefox 134, restored for the VIEWING
 * surface. The fixes841 one-KaiOS-device baseline proved the login stack: a
 * clean account logs in fully on m.facebook.com (KaiOS)  -  c_user + xs set, no
 * checkpoint, lands on the logged-in home  -  and the session persists (fixes838
 * cookies). BUT the KaiOS logged-in surface refuses the feed ("Facebook is not
 * available on this device  -  switch to a mobile or desktop device"), HW-seen
 * 2026-07-16. So we split again by ROLE: m.facebook.com stays KaiOS (the
 * reliable no-checkpoint LOGIN surface), while www/apex facebook.com + the
 * fbcdn/facebook.net asset origins use FF134 (the DESKTOP surface that renders
 * the real logged-in shell  -  the June baseline: nav + group-chats rail; the
 * feed itself is JS-built and awaits the JS work). The session cookies
 * (c_user/xs) are domain-wide .facebook.com, so a KaiOS login on m carries
 * straight over to an FF134 view on www. Sec-Fetch is synthesized per request
 * in build_request. */
static const char MACOS9_UA_FB_FF134[] =
	"Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:134.0) Gecko/20100101 Firefox/134.0";

struct macos9_ua_rule {
	const char *suffix;	/* host suffix, e.g. "facebook.com" */
	const char *ua;		/* User-Agent to send to that host */
};

static const struct macos9_ua_rule macos9_ua_rules[] = {
	/*
	 * fixes842 (#167): split by ROLE (see the MACOS9_UA_FB_FF134 note). The
	 * m.facebook.com row is MORE SPECIFIC and MUST stay first (first-match-
	 * wins): it keeps the KaiOS UA  -  the reliable no-checkpoint LOGIN surface
	 * (a clean account logs straight in, c_user+xs). www/apex facebook.com and
	 * the fbcdn/facebook.net assets use FF134  -  the DESKTOP surface that shows
	 * the logged-in shell (KaiOS logged-in = "not available on this device").
	 * Session cookies are domain-wide, so login on m carries to viewing on www.
	 * Do NOT append " MacSurf/..." to the KaiOS UA  -  FB's gate is exact.
	 */
	{ "m.facebook.com", MACOS9_UA_FB_KAIOS },
	{ "facebook.com",   MACOS9_UA_FB_FF134 },
	{ "fbcdn.net",      MACOS9_UA_FB_FF134 },
	{ "facebook.net",   MACOS9_UA_FB_FF134 },
	/*
	 * fixes821: Hacker News UA-gates /login (and other dynamic routes)
	 * at nginx: the honest "MacSurf/2.0.5 (Macintosh; PPC Mac OS 9)" UA
	 * gets 429 "Sorry." while a Chrome UA gets the form - PROVEN by a
	 * same-IP curl A/B from the maintainer's network (2026-07-14:
	 * MacSurf-UA 429 / Chrome-UA 200; the Jul-11 Chrome HAR shows the
	 * full login 200->302 works from that IP). Content routes (/,
	 * /news, /item) are NOT gated. Per-host spoof with the exact
	 * string the A/B proved; HN's login is pure HTML form POST
	 * (goto/acct/pw), no JS needed, so core form.c handles it.
	 */
	{ "news.ycombinator.com",
	  "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36" },
	/*
	 * fixes1115 - emaculation.com is behind Cloudflare bot-detection.
	 * The honest MacSurf UA triggers HTTP 403 with a JS challenge page,
	 * confirmed on hardware 2026-08-05: st=403, cdn-cgi/challenge-platform.
	 * A Chrome UA MAY bypass it (proven HN pattern); if not, TLS fingerprint
	 * (BearSSL) is the next suspect. Test and revert if no difference.
	 */
	{ "emaculation.com",
	  "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36" }
	/* add more host->UA overrides here */
	/*
	 * fixes741: macintoshrepository.org UA override REVERTED. fixes740 proved
	 * the https->http 301 IS UA-gated  -  a modern Firefox UA keeps the server
	 * on https  -  but the modern site it then serves is heavy and renders
	 * poorly on MacSurf, whereas the default-UA http surface is lighter and
	 * loads better. So we deliberately let it serve http. Do NOT re-add a
	 * modern-UA override here.
	 */
};

/* Release-performance host policy cache. Both fetchers ask the same two
 * questions for every resource: which UA applies, and is this host blocked?
 * Heavy pages issue tens or hundreds of requests to a small set of origins;
 * rescanning the UA table plus ~60 tracker suffixes for every one is wasted
 * string work on a G3. A tiny direct-mapped process cache preserves the exact
 * suffix-match semantics while reducing repeated-host lookups to one hash and
 * one case-insensitive equality check. Oversize host strings bypass the cache
 * and retain the old scan path. */
#define MACOS9_HOST_POLICY_CACHE_SLOTS 16
struct macos9_host_policy_cache_entry {
	unsigned long hash;
	size_t host_len;
	char host[256];
	const char *ua;
	int ua_valid;
	int tracker;
	int tracker_valid;
};
static struct macos9_host_policy_cache_entry
	macos9_host_policy_cache[MACOS9_HOST_POLICY_CACHE_SLOTS];

static unsigned long
macos9_host_hash_ci(const char *host, size_t len)
{
	unsigned long h;
	size_t i;
	unsigned char c;

	h = 0x811c9dc5UL;
	for (i = 0; i < len; i++) {
		c = (unsigned char)host[i];
		if (c >= (unsigned char)'A' && c <= (unsigned char)'Z')
			c = (unsigned char)(c - (unsigned char)'A' + (unsigned char)'a');
		h ^= (unsigned long)c;
		h *= 0x01000193UL;
	}
	return h;
}

static struct macos9_host_policy_cache_entry *
macos9_host_policy_get(const char *host, size_t len)
{
	struct macos9_host_policy_cache_entry *e;
	unsigned long h;
	unsigned long slot;

	if (host == NULL || len >= sizeof(macos9_host_policy_cache[0].host))
		return NULL;
	h = macos9_host_hash_ci(host, len);
	slot = h & (MACOS9_HOST_POLICY_CACHE_SLOTS - 1UL);
	e = &macos9_host_policy_cache[slot];
	if (e->hash != h || e->host_len != len ||
	    strncasecmp(e->host, host, len) != 0 || e->host[len] != '\0') {
		memcpy(e->host, host, len);
		e->host[len] = '\0';
		e->hash = h;
		e->host_len = len;
		e->ua = NULL;
		e->ua_valid = 0;
		e->tracker = 0;
		e->tracker_valid = 0;
	}
	return e;
}

const char *macos9_user_agent_default(void)
{
	return MACOS9_UA_DEFAULT;
}

const char *macos9_user_agent_for_host(const char *host)
{
	size_t hl;
	size_t n;
	size_t i;
	struct macos9_host_policy_cache_entry *e;
	const char *result;

	if (host == NULL) return MACOS9_UA_DEFAULT;
	hl = strlen(host);
	e = macos9_host_policy_get(host, hl);
	if (e != NULL && e->ua_valid) return e->ua;

	result = MACOS9_UA_DEFAULT;
	n = sizeof(macos9_ua_rules) / sizeof(macos9_ua_rules[0]);
	for (i = 0; i < n; i++) {
		size_t sl;
		sl = strlen(macos9_ua_rules[i].suffix);
		if (hl >= sl &&
		    strncasecmp(host + hl - sl,
				macos9_ua_rules[i].suffix, sl) == 0 &&
		    (hl == sl || host[hl - sl - 1] == '.')) {
			result = macos9_ua_rules[i].ua;
			break;
		}
	}
	if (e != NULL) {
		e->ua = result;
		e->ua_valid = 1;
	}
	return result;
}

/* fixes856 (#285)  -  tracker / ad-network / consent-platform host blocklist.
 * See macos9_blocklist.h for the full rationale, the measured hackaday.com
 * numbers (~908 KB of 2406 KB = ~38% of the page), the allow-policy for small
 * privacy-respecting analytics (umami/plausible/fathom/...), and the list of
 * origins deliberately left OUT because blocking them would break real work.
 *
 * Entries are host SUFFIXES matched on a dot boundary. Keep them grouped and
 * keep the comments: the next person needs to know why a row is here before
 * they add a neighbour that quietly breaks a site. */
static const char *const macos9_tracker_hosts[] = {
	/* --- Google analytics / tag management / ads --- */
	"google-analytics.com",
	"googletagmanager.com",		/* gtag/js = 475 KB on hackaday alone */
	"googlesyndication.com",
	"googleadservices.com",
	"doubleclick.net",
	"adservice.google.com",
	/* --- Consent-management platforms (banner = pure drag on OS 9) --- */
	"usercentrics.eu",		/* hackaday: loader + WebSdk + JSON ~433 KB */
	"cookielaw.org",		/* OneTrust */
	"onetrust.com",
	"cookiebot.com",
	"consensu.org",
	"trustarc.com",
	/* --- Analytics / RUM / session recording --- */
	"scorecardresearch.com",
	"quantserve.com",
	"quantcast.com",
	"chartbeat.com",
	"chartbeat.net",
	"newrelic.com",			/* js-agent.newrelic.com */
	"nr-data.net",
	"hotjar.com",
	"fullstory.com",
	"mouseflow.com",
	"crazyegg.com",
	"clarity.ms",			/* Microsoft Clarity */
	"mixpanel.com",
	"segment.com",
	"segment.io",
	"amplitude.com",
	"branch.io",
	/* --- WordPress.com telemetry ONLY.  s0/s1/i0.wp.com serve real assets
	 * and jetpack.wordpress.com hosts the comment iframe  -  never add those. */
	"stats.wp.com",
	"pixel.wp.com",
	/* --- Ad exchanges / servers --- */
	"adnxs.com",
	"criteo.com",
	"criteo.net",
	"taboola.com",
	"outbrain.com",
	"pubmatic.com",
	"rubiconproject.com",
	"openx.net",
	"adsrvr.org",
	"amazon-adsystem.com",
	"casalemedia.com",
	"indexexchange.com",
	"33across.com",
	"sharethrough.com",
	"adform.net",
	"smartadserver.com",
	"teads.tv",
	/* --- Social tracking pixels.  NOTE: facebook.net / fbcdn.net / fbsbx.com
	 * are deliberately ABSENT  -  #167 loads the real site through them. --- */
	"ads-twitter.com",
	"analytics.twitter.com",
	"analytics.tiktok.com",
	"px.ads.linkedin.com",
	"snap.licdn.com",
	"bat.bing.com",
	/* --- Ad servers seen on hackaday specifically --- */
	"rv-ads.supplyframe.com",
	"analytics.supplyframe.com"
	/* Add new rows here. Before adding: confirm the origin serves NOTHING the
	 * page needs to render, and that the suffix cannot swallow a sibling host
	 * that does (the facebook.net case above is the cautionary tale). */
};

int macos9_host_is_tracker(const char *host)
{
	size_t hl;
	size_t n;
	size_t i;
	struct macos9_host_policy_cache_entry *e;
	int result;

	if (host == NULL) return 0;
	hl = strlen(host);
	e = macos9_host_policy_get(host, hl);
	if (e != NULL && e->tracker_valid) return e->tracker;

	result = 0;
	n = sizeof(macos9_tracker_hosts) / sizeof(macos9_tracker_hosts[0]);
	for (i = 0; i < n; i++) {
		size_t sl;
		sl = strlen(macos9_tracker_hosts[i]);
		/* Dot-boundary suffix match, same spoof-guard as the UA table:
		 * "doubleclick.net" matches ad.doubleclick.net but never
		 * evildoubleclick.net. */
		if (hl >= sl &&
		    strncasecmp(host + hl - sl,
				macos9_tracker_hosts[i], sl) == 0 &&
		    (hl == sl || host[hl - sl - 1] == '.')) {
			result = 1;
			break;
		}
	}
	if (e != NULL) {
		e->tracker = result;
		e->tracker_valid = 1;
	}
	return result;
}

/* fixes1328 (#167)  -  request-kind Accept selection.
 *
 * 9b86becd ("Select and request WebP in normal browsing") gave EVERY
 * request -- documents, XHR and images alike -- the image-flavoured
 * Accept header: text/html, application/xhtml+xml, image/webp, then
 * image wildcard at q=0.8 and the full wildcard demoted to q=0.7.
 *
 * Facebook negotiates its bootstrap document server-side, so advertising
 * image/webp (and demoting the full wildcard from q=0.8 to q=0.7) on the
 * MAIN DOCUMENT request can change which bundle variant the edge serves.
 * Real browsers never do this: they send a document Accept for documents
 * and an image Accept for images.
 *
 * So: restore the exact pre-WebP document header for everything that is
 * not positively identified as an image request, and keep an explicit
 * image/webp advertisement for the ones that are.  WebP decoding and
 * srcset/picture selection are untouched -- and the restored document
 * header still carries the full wildcard at q=0.8, so a server that
 * sniffs rather than negotiates keeps serving WebP either way.
 *
 * NOTE: do not paste a literal Accept value into this comment.  Those
 * strings contain a star-slash pair, which closes the block comment
 * early and drops the remaining prose into the token stream. */
static int macos9_path_is_image(const char *path)
{
	size_t n;
	size_t cut;
	unsigned char a;
	unsigned char b;
	unsigned char c;
	unsigned char d;

	if (path == NULL) return 0;
	/* Measure the path only, never the query or fragment. */
	n = strlen(path);
	for (cut = 0; cut < n; cut++) {
		if (path[cut] == '?' || path[cut] == '#') break;
	}

	/* All supported image suffixes are three or four ASCII letters. Avoid
	 * ten strlen + strncasecmp calls for every network request. */
	if (cut >= 4 && path[cut - 4] == '.') {
		a = (unsigned char)path[cut - 3];
		b = (unsigned char)path[cut - 2];
		c = (unsigned char)path[cut - 1];
		if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
		if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
		if (a == 'p' && b == 'n' && c == 'g') return 1;
		if (a == 'j' && b == 'p' && c == 'g') return 1;
		if (a == 'g' && b == 'i' && c == 'f') return 1;
		if (a == 'b' && b == 'm' && c == 'p') return 1;
		if (a == 'i' && b == 'c' && c == 'o') return 1;
		if (a == 's' && b == 'v' && c == 'g') return 1;
		if (a == 't' && b == 'i' && c == 'f') return 1;
	}
	if (cut >= 5 && path[cut - 5] == '.') {
		a = (unsigned char)path[cut - 4];
		b = (unsigned char)path[cut - 3];
		c = (unsigned char)path[cut - 2];
		d = (unsigned char)path[cut - 1];
		if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
		if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
		if (d >= 'A' && d <= 'Z') d = (unsigned char)(d - 'A' + 'a');
		if (a == 'w' && b == 'e' && c == 'b' && d == 'p') return 1;
		if (a == 'j' && b == 'p' && c == 'e' && d == 'g') return 1;
		if (a == 't' && b == 'i' && c == 'f' && d == 'f') return 1;
	}
	return 0;
}

const char *macos9_accept_for_path(const char *path)
{
	if (macos9_path_is_image(path)) {
		return "image/webp,image/png,image/jpeg,image/gif,*/*;q=0.8";
	}
	return "text/html,application/xhtml+xml,*/*;q=0.8";
}

/* fixes835 (#167 Facebook M1)  -  case-insensitive substring test. Used by
 * the fetchers to decide whether the caller already supplied a Sec-Fetch
 * / Origin header (which then WINS over the synthesized one). */
int macos9_hdr_has_ci(const char *hay, const char *needle)
{
	size_t nl;
	size_t hl;
	size_t i;
	unsigned char first;
	unsigned char hc;

	if (hay == NULL || needle == NULL) return 0;
	nl = strlen(needle);
	if (nl == 0) return 0;
	hl = strlen(hay);
	if (hl < nl) return 0;
	first = (unsigned char)needle[0];
	if (first >= 'A' && first <= 'Z')
		first = (unsigned char)(first - 'A' + 'a');
	for (i = 0; i + nl <= hl; i++) {
		hc = (unsigned char)hay[i];
		if (hc >= 'A' && hc <= 'Z')
			hc = (unsigned char)(hc - 'A' + 'a');
		if (hc != first) continue;
		if (strncasecmp(hay + i, needle, nl) == 0) return 1;
	}
	return 0;
}

/* fixes835 (#167 Facebook M1)  -  copy NetSurf core's additional request
 * headers (a NULL-terminated array of "Name: value" strings) into one
 * ready-to-splice buffer, "Name: value\r\n" per KEPT header. Dropped:
 * every header the fetcher emits itself (Host, User-Agent, the Accept
 * family, Content-Length, Content-Type, Connection), all hop-by-hop
 * headers, and Cookie (the jar wins).
 *
 * fixes979  -  If-None-Match / If-Modified-Since are KEPT again. fixes835
 * dropped them for a real reason ("we can't answer 304, so we must not
 * ask"): neither fetcher had a FETCH_NOTMODIFIED branch, so a 304 arrived as
 * an empty body and blanked the cached resource, worst on revalidated images
 * and fonts. Both fetchers now have that branch, so the premise is gone --
 * and the workaround was expensive. Without conditional headers an llcache
 * object needing freshness validation can only be revalidated by refetching
 * the WHOLE body over TLS, on a 400 MHz machine, even when the bytes we hold
 * are still good. Asking is the entire point of holding them.
 *
 * KEPT: Referer, Sec-Fetch-*, Origin, etc. dst is always NUL-terminated; a
 * header that would overflow is skipped whole, never truncated. */
void macos9_capture_extra_headers(const char **h, char *dst, size_t cap)
{
	struct macos9_drop_header {
		const char *name;
		size_t len;
	};
	static const struct macos9_drop_header drop[] = {
		{ "host:", sizeof("host:") - 1 },
		{ "cookie:", sizeof("cookie:") - 1 },
		{ "connection:", sizeof("connection:") - 1 },
		{ "keep-alive:", sizeof("keep-alive:") - 1 },
		{ "proxy-connection:", sizeof("proxy-connection:") - 1 },
		{ "transfer-encoding:", sizeof("transfer-encoding:") - 1 },
		{ "te:", sizeof("te:") - 1 },
		{ "trailer:", sizeof("trailer:") - 1 },
		{ "upgrade:", sizeof("upgrade:") - 1 },
		{ "content-length:", sizeof("content-length:") - 1 },
		{ "content-type:", sizeof("content-type:") - 1 },
		{ "accept-encoding:", sizeof("accept-encoding:") - 1 },
		{ "user-agent:", sizeof("user-agent:") - 1 },
		{ "accept:", sizeof("accept:") - 1 },
		{ "accept-language:", sizeof("accept-language:") - 1 }
	};
	size_t nd;
	size_t used;
	int i;

	nd = sizeof(drop) / sizeof(drop[0]);
	used = 0;
	if (dst == NULL || cap == 0) return;
	dst[0] = '\0';
	if (h == NULL) return;
	for (i = 0; h[i] != NULL; i++) {
		const char *line;
		size_t ll;
		size_t j;
		int drop_it;

		line = h[i];
		drop_it = 0;
		if (line[0] == '\0') continue;
		for (j = 0; j < nd; j++) {
			if (strncasecmp(line, drop[j].name, drop[j].len) == 0) {
				drop_it = 1;
				break;
			}
		}
		if (drop_it) continue;
		ll = strlen(line);
		if (used + ll + 3 > cap) continue;
		memcpy(dst + used, line, ll);
		used += ll;
		dst[used++] = '\r';
		dst[used++] = '\n';
		dst[used] = '\0';
	}
}

static const char *macos9_fetch_filetype(const char *unix_path)
{
	const char *ext;
	if (unix_path == NULL) return "application/octet-stream";
	ext = strrchr(unix_path, '.');
	if (ext == NULL) return "application/octet-stream";
	ext++;
	if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0)
		return "text/html";
	if (strcasecmp(ext, "css") == 0) return "text/css";
	if (strcasecmp(ext, "png") == 0) return "image/png";
	if (strcasecmp(ext, "webp") == 0) return "image/webp";
	if (strcasecmp(ext, "gif") == 0) return "image/gif";
	if (strcasecmp(ext, "txt") == 0) return "text/plain";
	return "application/octet-stream";
}

static struct nsurl *macos9_fetch_get_resource_url(const char *path)
{
	(void)path;
	return NULL;
}

static char *macos9_fetch_mimetype(const char *ro_path)
{
	return strdup(macos9_fetch_filetype(ro_path));
}


/*
 * Strip HTML tags, decode a handful of entities, collapse whitespace.
 * Produces a plain-text version of the input into dst, up to dst_cap-1 chars.
 */
long
macos9_strip_html(const char *src, long src_len,
		char *dst, long dst_cap)
{
	long si;
	long di;
	int in_tag;
	int in_script;
	int in_style;
	int last_space;
	char tag_name[16];
	int tag_name_len;

	si = 0;
	di = 0;
	in_tag = 0;
	in_script = 0;
	in_style = 0;
	last_space = 1;
	tag_name_len = 0;
	tag_name[0] = '\0';

	while (si < src_len && di < dst_cap - 1) {
		char c;
		c = src[si];

		if (in_tag) {
			if (c == '>') {
				in_tag = 0;
				if (tag_name_len > 0) {
					tag_name[tag_name_len] = '\0';
					if (strcasecmp(tag_name, "script") == 0)
						in_script = 1;
					else if (strcasecmp(tag_name, "/script") == 0)
						in_script = 0;
					else if (strcasecmp(tag_name, "style") == 0)
						in_style = 1;
					else if (strcasecmp(tag_name, "/style") == 0)
						in_style = 0;
					else if (strcasecmp(tag_name, "br") == 0 ||
					         strcasecmp(tag_name, "br/") == 0 ||
					         strcasecmp(tag_name, "p") == 0 ||
					         strcasecmp(tag_name, "/p") == 0 ||
					         strcasecmp(tag_name, "/div") == 0 ||
					         strcasecmp(tag_name, "/tr") == 0 ||
					         strcasecmp(tag_name, "/h1") == 0 ||
					         strcasecmp(tag_name, "/h2") == 0 ||
					         strcasecmp(tag_name, "/h3") == 0 ||
					         strcasecmp(tag_name, "/h4") == 0 ||
					         strcasecmp(tag_name, "/li") == 0) {
						if (di > 0 && dst[di-1] != '\n')
							dst[di++] = '\n';
						last_space = 1;
					}
				}
				tag_name_len = 0;
			} else if (tag_name_len < 15 && c != ' ' && c != '\t') {
				tag_name[tag_name_len++] = c;
			} else if (c == ' ' || c == '\t') {
				if (tag_name_len < 15)
					tag_name[tag_name_len] = '\0';
			}
			si++;
			continue;
		}

		if (c == '<') {
			in_tag = 1;
			tag_name_len = 0;
			si++;
			continue;
		}

		if (in_script || in_style) {
			si++;
			continue;
		}

		if (c == '&') {
			if (si + 4 < src_len && strncmp(src + si, "&amp;", 5) == 0) {
				dst[di++] = '&'; si += 5; last_space = 0; continue;
			}
			if (si + 3 < src_len && strncmp(src + si, "&lt;", 4) == 0) {
				dst[di++] = '<'; si += 4; last_space = 0; continue;
			}
			if (si + 3 < src_len && strncmp(src + si, "&gt;", 4) == 0) {
				dst[di++] = '>'; si += 4; last_space = 0; continue;
			}
			if (si + 5 < src_len && strncmp(src + si, "&quot;", 6) == 0) {
				dst[di++] = '"'; si += 6; last_space = 0; continue;
			}
			if (si + 5 < src_len && strncmp(src + si, "&apos;", 6) == 0) {
				dst[di++] = '\''; si += 6; last_space = 0; continue;
			}
			if (si + 5 < src_len && strncmp(src + si, "&nbsp;", 6) == 0) {
				if (!last_space) { dst[di++] = ' '; last_space = 1; }
				si += 6; continue;
			}
			dst[di++] = c;
			si++;
			last_space = 0;
			continue;
		}

		if (c == '\r' || c == '\n') {
			if (di > 0 && dst[di-1] != '\n')
				dst[di++] = '\n';
			last_space = 1;
			si++;
			continue;
		}

		if (c == ' ' || c == '\t') {
			if (!last_space) {
				dst[di++] = ' ';
				last_space = 1;
			}
			si++;
			continue;
		}

		if ((unsigned char)c < 0x20) {
			si++;
			continue;
		}

		dst[di++] = c;
		last_space = 0;
		si++;
	}

	dst[di] = '\0';
	return di;
}

/*
 * Word-wrap a plain-text buffer into fixed-width lines. Populates
 * line_offsets[] with byte offsets into text[] and line_lengths[] with
 * byte lengths. Breaks on spaces when possible, hard-breaks otherwise.
 * Honors existing '\n' characters as forced line breaks.
 */
long
macos9_word_wrap(const char *text, long text_len,
		long *line_offsets, short *line_lengths,
		long max_lines, short max_chars_per_line)
{
	long i;
	long line_start;
	long last_space;
	long count;

	count = 0;
	line_start = 0;
	last_space = -1;

	if (max_chars_per_line < 8)
		max_chars_per_line = 8;

	for (i = 0; i <= text_len && count < max_lines; i++) {
		int hard_break;
		long width;

		hard_break = 0;
		if (i == text_len) {
			hard_break = 1;
		} else if (text[i] == '\n') {
			hard_break = 1;
		} else if (text[i] == ' ') {
			last_space = i;
		}

		width = i - line_start;

		if (hard_break) {
			line_offsets[count] = line_start;
			line_lengths[count] = (short)(i - line_start);
			count++;
			line_start = i + 1;
			last_space = -1;
			continue;
		}

		if (width >= max_chars_per_line) {
			long break_at;
			break_at = (last_space > line_start) ? last_space : i;
			line_offsets[count] = line_start;
			line_lengths[count] = (short)(break_at - line_start);
			count++;
			line_start = break_at;
			if (line_start < text_len && text[line_start] == ' ')
				line_start++;
			last_space = -1;
			i = line_start - 1;
		}
	}

	if (count == 0) {
		line_offsets[0] = 0;
		line_lengths[0] = 0;
		count = 1;
	}

	return count;
}

/* === Shared request-header helpers (cleanup 2026-08-05) ==============
 *
 * These replace ~30 lines of cookie-header assembly and ~35 lines of
 * Sec-Fetch synthesis that were duplicated verbatim in both fetchers.
 * Called from build_request (TLS) and mfs_open (HTTP).
 * ===================================================================== */

#include "macsurf_debug.h"	/* for the "cookie hdr too big" log line */

void macos9_build_cookie_header(char *cookie_hdr, size_t cap,
		const char *cookie_str)
{
	size_t cl;

	if (cookie_hdr == NULL || cap == 0) return;
	cookie_hdr[0] = '\0';
	if (cookie_str == NULL) return;
	cl = strlen(cookie_str);
	if (cl > 0 && cl + 11 <= cap) {
		memcpy(cookie_hdr, "Cookie: ", 8);
		memcpy(cookie_hdr + 8, cookie_str, cl);
		cookie_hdr[8 + cl] = '\r';
		cookie_hdr[9 + cl] = '\n';
		cookie_hdr[10 + cl] = '\0';
	} else if (cl > 0) {
		macsurf_debug_log_writef(
			"cookie hdr too big cl=%ld cap=%ld",
			(long)cl, (long)cap);
	}
}

void macos9_build_sec_fetch(char *synth, size_t cap,
		int verifiable, int is_post,
		const char *scheme, const char *host)
{
	const char *base;
	size_t bl;
	size_t used;
	size_t scheme_len;
	size_t host_len;
	char *p;

	if (synth == NULL || cap == 0) return;
	if (verifiable && is_post) {
		base =
			"Sec-Fetch-Dest: document\r\n"
			"Sec-Fetch-Mode: navigate\r\n"
			"Sec-Fetch-Site: same-origin\r\n"
			"Sec-Fetch-User: ?1\r\n"
			"Upgrade-Insecure-Requests: 1\r\n";
	} else if (verifiable) {
		base =
			"Sec-Fetch-Dest: document\r\n"
			"Sec-Fetch-Mode: navigate\r\n"
			"Sec-Fetch-Site: none\r\n"
			"Sec-Fetch-User: ?1\r\n"
			"Upgrade-Insecure-Requests: 1\r\n";
	} else {
		base =
			"Sec-Fetch-Dest: empty\r\n"
			"Sec-Fetch-Mode: no-cors\r\n"
			"Sec-Fetch-Site: same-origin\r\n";
	}
	bl = strlen(base);
	if (bl >= cap) {
		memcpy(synth, base, cap - 1);
		synth[cap - 1] = '\0';
		return;
	}
	memcpy(synth, base, bl + 1);

	/* Append Origin without invoking printf formatting on every POST. */
	if (is_post && host != NULL && scheme != NULL) {
		used = bl;
		scheme_len = strlen(scheme);
		host_len = strlen(host);
		if (used + 8 + scheme_len + 3 + host_len + 2 < cap) {
			p = synth + used;
			memcpy(p, "Origin: ", 8); p += 8;
			memcpy(p, scheme, scheme_len); p += scheme_len;
			memcpy(p, "://", 3); p += 3;
			memcpy(p, host, host_len); p += host_len;
			*p++ = '\r';
			*p++ = '\n';
			*p = '\0';
		}
	}
}

int macos9_ot_init(void) { return 0; }
const char *macos9_ot_get_error(void) { return NULL; }

struct gui_fetch_table macos9_fetch_table = {
	macos9_fetch_filetype,
	macos9_fetch_get_resource_url,
	NULL,
	NULL,
	macos9_fetch_mimetype,
	NULL,
	NULL
};

/* ==================================================================
 * fixes721 (#207 tooling)  -  multipart/form-data body builder, shared by
 * the HTTP and HTTPS fetchers so an <input type=file> upload (e.g. the
 * debug-log page) works from MacSurf. NetSurf core hands the fetcher a
 * fetch_multipart_data list; we serialise it into a real
 * multipart/form-data body, reading each file part off disk via fopen
 * (the path the NavServices picker stored on the gadget). Returns a
 * malloc'd body (caller frees), fills *out_len and the caller's
 * boundary buffer (>= 64 bytes). NULL on OOM.
 * ================================================================== */
static int macos9_mp_append(char **buf, long *len, long *cap,
		const char *src, long add)
{
	long ncap;
	char *nb;

	if (add <= 0) return 0;
	if (*len + add <= *cap) {
		memcpy(*buf + *len, src, (size_t)add);
		*len += add;
		return 0;
	}
	ncap = (*cap == 0) ? 8192L : *cap;
	while (ncap < *len + add) ncap *= 2L;
	nb = (char *)realloc(*buf, (size_t)ncap);
	if (nb == NULL) return -1;
	*buf = nb;
	*cap = ncap;
	memcpy(*buf + *len, src, (size_t)add);
	*len += add;
	return 0;
}

char *macos9_build_multipart(const struct fetch_multipart_data *pm,
		char *boundary_out, long *out_len)
{
	static unsigned long mp_ctr = 0UL;
	char *body;
	long len;
	long cap;
	char hdr[4096];
	char boundary[64];
	const struct fetch_multipart_data *n;
	int hl;

	body = NULL;
	len = 0L;
	cap = 0L;
	*out_len = 0L;
	mp_ctr++;
	sprintf(boundary, "----------MacSurfFormBoundary4D53%lu", mp_ctr);
	if (boundary_out != NULL) strcpy(boundary_out, boundary);

	for (n = pm; n != NULL; n = n->next) {
		if (n->file) {
			hl = sprintf(hdr,
				"--%s\r\nContent-Disposition: form-data; "
				"name=\"%s\"; filename=\"%s\"\r\n"
				"Content-Type: application/octet-stream\r\n\r\n",
				boundary, (n->name != NULL) ? n->name : "",
				(n->value != NULL) ? n->value : "file");
		} else {
			hl = sprintf(hdr,
				"--%s\r\nContent-Disposition: form-data; "
				"name=\"%s\"\r\n\r\n",
				boundary, (n->name != NULL) ? n->name : "");
		}
		if (hl < 0 || macos9_mp_append(&body, &len, &cap, hdr, hl) != 0)
			goto fail;

		if (n->file) {
			FILE *f;
			f = fopen((n->rawfile != NULL) ? n->rawfile : "", "rb");
			if (f != NULL) {
				char rb[4096];
				size_t got;
				while ((got = fread(rb, 1, sizeof rb, f)) > 0) {
					if (macos9_mp_append(&body, &len, &cap,
							rb, (long)got) != 0) {
						fclose(f);
						goto fail;
					}
				}
				fclose(f);
			}
			/* fopen failure -> an empty file part; server still gets the
			 * form, just with no file bytes. */
		} else if (n->value != NULL) {
			if (macos9_mp_append(&body, &len, &cap,
					n->value, (long)strlen(n->value)) != 0)
				goto fail;
		}
		if (macos9_mp_append(&body, &len, &cap, "\r\n", 2L) != 0)
			goto fail;
	}

	hl = sprintf(hdr, "--%s--\r\n", boundary);
	if (hl < 0 || macos9_mp_append(&body, &len, &cap, hdr, hl) != 0)
		goto fail;

	*out_len = len;
	return body;

fail:
	if (body != NULL) free(body);
	*out_len = 0L;
	return NULL;
}

/* Shared by both fetchers: find the next CRLF-terminated line in a buffer.
 * NUL-terminates at '\r', advances *buf past the '\n', and decrements *len.
 * Returns pointer to the start of the line, or NULL if no complete line. */
char *macos9_find_line(char **buf, long *len)
{
	char *p;
	long n;
	long i;

	p = *buf;
	n = *len;
	for (i = 0; i + 1 < n; i++) {
		if (p[i] == '\r' && p[i+1] == '\n') {
			p[i] = 0;
			*buf = p + i + 2;
			*len = n - (i + 2);
			return p;
		}
	}
	return NULL;
}
