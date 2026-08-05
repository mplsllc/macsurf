/*
 * MacSurf - macos9_https_fetcher.c
 *
 * Native HTTPS fetcher backed by macTLS (BearSSL + Open Transport).
 * Drives TLS through the OSTLS_New/Start/Pump/Read/Write async API
 * and delivers decrypted HTTP body bytes into NetSurf core via the
 * fetcher_operation_table.
 *
 * V1 scope:
 *   - GET only (POST queued, see macos9_http_fetcher.c for the form
 *     encoding pattern when wired)
 *   - No keep-alive pool (every fetch opens a fresh TLS endpoint)
 *   - No auto-redirect (FETCH_REDIRECT dispatched on 3xx; llcache
 *     restarts the fetch)
 *   - Content-Length and chunked Transfer-Encoding both decoded
 *   - 4 concurrent slots per V1 cap (heap budget on a 16 MB partition,
 *     each OSTLSConnection is ~32 KB)
 *   - 15s no-progress timeout (mirrors fixes107)
 *
 * Self-frees via fetch_remove_from_queues + fetch_free at every
 * terminal callback per fixes102-105 discipline.
 */

#include "utils/ns_errors.h"
#include "utils/nsurl.h"
#include "utils/log.h"
#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/urldb.h"	/* fixes367 (#167) — cookie jar: urldb_get_cookie */
#include "macos9_useragent.h"	/* fixes368 (#167) — per-host UA table */
#include "macos9_blocklist.h"
#include "macos9_gzip.h"	/* fixes965 — streaming Content-Encoding: gzip */	/* fixes856 (#285) — tracker/ad blocklist */
#include "macsurf_debug.h"
#include "macsurf_osver.h"	/* fixes936 (OS X tier 1) — macsurf_os_is_osx() */

#include <string.h>
#include <stdlib.h>

#ifdef __MACOS9__
#include <Events.h>
#endif

#include "ostls_async.h"
#include "ostls_http.h"
#include "macos9_disk_cache.h"

#define MAX_HTTPS_F        128  /* macTLS#196: bumped to 128 for GitHub (65+ in-flight)
                                 * fixes241: was 32; image-heavy pages
                                 * hit ~10 NO FREE SLOTS / cold load at 32
                                 * because setup() is called for every
                                 * queued fetch up-front (start gating
                                    only governs dispatch, not setup
                                    allocation). Heavy mactrove front page
                                    has ~40+ sub-resources in flight; 64
                                    is the comfortable headroom. Memory
                                    cost: ~3.7 KB/slot static + OSTLS
                                    connection only when active, so 64
                                    slots cost ~240 KB resident at idle. */
/* fixes372 (#167) — 4096 was too small for Facebook's response header
 * block. FB sends ~5.4 KB of headers to the KaiOS login surface (the
 * content-security-policy alone is ~2.2 KB, plus permissions-policy ~1 KB,
 * report-to, reporting-endpoints, and several Set-Cookie lines), and login
 * 302 responses carry even more Set-Cookies. The old 4 KB cap tripped
 * "header buffer overflow" and the whole page fell back to fetcherror.
 * fixes422 — bumped 16384→65536: 68kmla.org/bb/ sends >16 KB of headers
 * (XenForo CSP + security headers grew past the old cap). Buffer is grown
 * on demand so this is only a ceiling, not a per-fetch allocation.
 * See macos9_useragent.h for the shared constant (MACSURF_HDR_BUF_MAX). */
#define HDR_BUF_MAX  MACSURF_HDR_BUF_MAX
/* fixes234 — bumped from 1024 to 8192. With sleep=0 in main.c we poll
 * ~hundreds of times per second, but at 1 KB per drain the body delivery
 * rate ceilinged at ~60 KB/s and a 59 KB mactrove home page took ~2.5 s
 * of dead pump time. 8 KB matches BearSSL's typical record size; one
 * Pump+Read cycle now drains 6-8× more decrypted body per pass.
 * See macos9_useragent.h for the shared constant (MACSURF_READ_CHUNK). */
#define READ_CHUNK    MACSURF_READ_CHUNK
/* fixes234 — bumped pump steps from 8 to 32. Each "step" is one BearSSL
 * engine state transition; on a single core G3 yielding every 8 steps
 * means we cycle through OS 9 cooperative-multitask hand-offs faster
 * than crypto can complete. 32 steps lets BearSSL process a full TLS
 * record (decrypt + integrity check) without an artificial yield mid-
 * decrypt, which is the dominant cost of body delivery for ChaCha20-
 * Poly1305 on a 233 MHz G3. */
#define PUMP_STEPS         32
/* fixes235 — drop no-progress timeout 900 ticks (15s) -> 240 (4s). The
 * dominant cold-load timesink on a typical Drupal/Wordpress page is a
 * foreign-host stylesheet whose origin rejects our TLS ClientHello
 * fingerprint (fonts.googleapis.com is the canonical case). Pre-235 we
 * waited 15s for the fetch to time out, then NetSurf core retried it,
 * and waited another 15s for the same failure — 30+ seconds of dead
 * time on every cold load while finish_conversion blocked on the
 * missing stylesheet. On ethernet to a working origin, any sub-resource
 * that hangs >2s mid-transfer is effectively dead anyway; 4s gives us
 * comfortable headroom for the slowest legitimate hosts while killing
 * the foreign-fingerprint-reject case before NetSurf renders. */
#define NO_PROGRESS_TICKS  240   /* 4s at 60Hz — connect/handshake stage */

/* fixes718 (#207) — once the TLS handshake has completed AND the origin has
 * begun answering (HS_HEADERS/HS_BODY), it is demonstrably alive and speaking
 * HTTPS; a stall now means "slow origin", not "dead host". A cold-cache backend
 * that fetches feeds/weather server-side legitimately takes 5-10s to produce
 * the first body bytes, and the tight 4s GET window was killing it mid-response,
 * then dead-hosting the site and cascading into the HTTP-fallback -> 301 -> dead
 * -> about:fetcherror blank page (captured on the minitower). Give a RESPONDING
 * origin 15s before we give up. The connect/handshake stage keeps the 4s window
 * so a truly dead or macTLS-incompatible host still fast-fails. */
#define RESP_NO_PROGRESS_TICKS  900   /* 15s at 60Hz — headers/body arriving */

/* fixes375 (#167) — a POST gets a far longer no-progress budget. A login
 * POST is NOT a dead sub-resource: Facebook's no-JS login-approval ("2FA")
 * holds the HTTP response open (long-poll) after sending its TLS session
 * tickets, waiting for the user to tap "Yes, it's me" on their phone — on
 * a script-less surface that hold is the only way it CAN do login-approval.
 * Observed on the G3: the POST sends fully (1390B), FB returns ~380B of
 * NewSessionTickets, then goes silent for the entire 4s window and we
 * abort before the user can approve, so c_user/xs never arrive and it
 * "pushes 2FA every time." GETs keep the tight 4s (a hung CSS file really
 * is dead); only a POST, which is a deliberate user action worth waiting
 * on, gets the long budget. */
#define POST_NO_PROGRESS_TICKS  3600   /* 60s at 60Hz — login-approval hold */

/* fixes231 — keep-alive pool. Each OSTLSConnection is ~32 KB heap
 * (BearSSL bidi buffer + plaintext rings + state). A 16-entry pool
 * holds ~512 KB max, well within the 16 MB partition. fixes232
 * bumped from 4 to 16 after the log showed 29 evict-FULL events on
 * a cold mactrove load: with max_fetchers_per_host=4, ~4 connections
 * per host go through the pool simultaneously, so 4 hosts × 4 = 16
 * is the natural fit. Back-nav, intra-site link clicks, and CDN-mix
 * pages (origin + fonts + analytics) all benefit. */
#define HTTPS_POOL_SIZE    16
#define HTTPS_POOL_KEY_LEN 280   /* matches host[256] + ":port" */

enum hs_state {
	HS_IDLE = 0,
	HS_QUEUED,
	HS_STARTING,
	HS_TLSING,
	HS_SEND_REQ,
	HS_HEADERS,
	HS_BODY,
	HS_CACHEHIT,    /* fixes218 — serve from on-disk cache, no TLS */
	HS_DONE,
	HS_FAIL
};

struct macos9_https_ctx {
	/* fixes965 — non-NULL when the response carried Content-Encoding:
	 * gzip. Body bytes are pushed through it and the DECOMPRESSED output
	 * is what reaches NetSurf and the disk cache. */
	struct macos9_gunzip *gz;
	struct fetch    *parent;
	struct nsurl    *url;
	char             host[256];
	unsigned short   port;
	char             path[1024];

	int              state;
	int              aborted;
	int              done;
	const char      *err;
	int              status;

	OSTLSConnection *conn;
	/* fixes367 (#167) — enlarged 1024→8192 to hold a full Cookie:
	 * request header. Logged-in Facebook sends ~1KB of session cookies
	 * (c_user, xs, datr, sb, fr, presence, wd, …); the old 1024 buffer
	 * would fail build_request once the jar filled.
	 * fixes835 (#167 M1) — enlarged 8192→12288: build_request has no
	 * pre-sprintf guard (only the post-hoc rn>=sizeof check), and the
	 * worst case Cookie(6144)+caller_hdrs(512)+synth(512)+request-line+
	 * host+UA+Accept lines can exceed 8192 and overrun BEFORE that check
	 * fires. 12288 keeps the worst realistic case (~8.8KB) inside the
	 * buffer with margin; the rn>=sizeof guard remains the backstop. This
	 * is a per-slot ctx field, not stack, so the extra 4KB is negligible. */
	char             req_buf[12288];
	unsigned long    req_len;
	unsigned long    req_sent;

	char            *hdr_buf;
	long             hdr_len;
	long             hdr_cap;

	int              chunked;
	long             content_length;
	long             body_bytes;

	OSTLSChunkDecoder chunk;

	char             mime[128];
	char             redirect_url[1024];

	unsigned long    progress_ticks;
	unsigned long    last_poll_tick; /* fixes548 — tick of the
	                                  * previous hctx_poll for this conn; a
	                                  * large gap means the event loop was
	                                  * blocked (cold-startup TSM, long sync
	                                  * op), NOT the peer stalling, so that
	                                  * gap must not count as no-progress. */
	UInt32           last_rx_bytes;  /* fixes414 — last OT recv-byte count
	                                  * seen, to credit raw wire progress to
	                                  * the no-progress watchdog. */
	unsigned long    rxlog_ticks;    /* fixes735 — throttle for the in-flight
	                                  * RX-progress log (RECON RX), ~every 2s. */

	/* fixes218 — disk cache. cache_eligible flips on after parse_headers
	 * sees a 200 OK with a whitelisted MIME. cache_capture accumulates
	 * the body bytes (raw, post-chunk-decode if applicable); on
	 * FETCH_FINISHED we write to disk via macos9_cache_store. If a
	 * lookup at setup time succeeds, we route through HS_CACHEHIT and
	 * never touch the network. */
	int              cache_eligible;
	char            *cache_capture;
	long             cache_cap_len;
	long             cache_cap_cap;
	int              cache_overflow;
	/* fixes987 — streaming-store handle (0 = not caching this response).
	 * Replaces cache_capture as the live path; the buffer fields stay only
	 * because hctx_clear still frees them defensively. */
	int              cache_stream;
	char            *cache_hit_body;
	long             cache_hit_len;
	char             cache_hit_mime[128];
	int              cache_hit_status;
	/* fixes981 — freshness/validator headers: captured while parsing a
	 * response we intend to cache, replayed verbatim on a hit so llcache
	 * sees the same Date/Cache-Control/ETag it would have seen from the
	 * network. */
	char             cache_hdrs[MACSURF_CACHE_HDRS_MAX];
	char             cache_hit_hdrs[MACSURF_CACHE_HDRS_MAX];

	/* fixes228 — auto-retry on benign peer-close. CF and Google CDN
	 * close TLS connections aggressively after handshake; one retry
	 * with a fresh connection usually succeeds. retries counts attempts
	 * BEYOND the first; capped at HTTPS_MAX_RETRIES so we don't loop
	 * forever on a genuinely-dead host. */
	int              retries;

	/* fixes231 — keep-alive pool. pool_key is "host:port" used as the
	 * lookup key. keep_alive_ok defaults to 1 in setup, cleared on
	 * server "Connection: close" response header, abort, or fail.
	 * from_pool flags that c->conn came out of the pool (currently
	 * informational; future use for "fall back to fresh on first
	 * write/read error from a pooled conn"). */
	char             pool_key[HTTPS_POOL_KEY_LEN];
	int              keep_alive_ok;
	int              from_pool;

	/* fixes232a — NetSurf core calls ops.setup for EVERY queued fetch
	 * up-front, but only calls ops.start for the max_fetchers_per_host
	 * subset that fits inside the dispatch gate. Without tracking which
	 * slots are actually dispatched, our hctx_poll opens TLS for every
	 * set-up slot and bypasses NetSurf's per-host throttle entirely.
	 * `started` is set in macos9_https_start; hctx_poll gates HS_QUEUED
	 * entry on it. Survives hctx_reset_for_retry. */
	int              started;

	/* fixes312 (#144) — POST body.
	 *   post_body / post_body_len: heap-owned copy of the urlencoded
	 *     form payload captured at setup time. NULL → GET.
	 *   post_body_sent: bytes written across one or more OSTLS_Write
	 *     calls during HS_SEND_REQ (after the header buffer is fully
	 *     written). */
	char            *post_body;
	UInt32           post_body_len;
	UInt32           post_body_sent;
	/* fixes721 (#144 file upload) — POST Content-Type. Empty = urlencoded
	 * default; "multipart/form-data; boundary=..." for a file upload. */
	char             post_ctype[128];
	/* fixes835 (#167 M1) — NetSurf core's additional request headers,
	 * sanitized and captured at setup (see macos9_capture_extra_headers).
	 * In-struct fixed buffer: zero-inited by the setup memset, nothing to
	 * free. Empty ("") splices as a no-op.
	 * fixes846 (#167 S3): 512 -> 2048. A single XHR call setting FB's
	 * GraphQL X-FB-* header set (X-FB-Friendly-Name, X-FB-LSD,
	 * X-ASBD-ID, X-FB-Client-IP, ...) plus a Sec-Fetch override plus
	 * Origin easily exceeds 512, and macsurf_capture_extra_headers()
	 * SKIPS a header whole rather than truncating it on overflow --
	 * silently dropping exactly the headers XHR needs to be believed. */
	char             caller_hdrs[2048];
};

#define HTTPS_MAX_RETRIES 2

static struct macos9_https_ctx https_slots[MAX_HTTPS_F];

/* fixes374 (#167) — forward decl; defined near build_request. Used by the
 * disk-cache lookup/store paths to bypass caching for Facebook hosts. */
static int host_is_fb_asset(const char *host);

/* ---------- auto-upgrade fallback (fixes249b) ----------
 * When the user types "example.com" with no scheme, window.c prepends
 * "https://" (fixes249) so modern HTTPS-default sites work. For retro
 * HTTP-only sites the upgrade fails. Rather than show about:fetcherror,
 * we want to retry as plain http. The flow:
 *   1. Submit handler calls macsurf_auto_upgrade_mark(host_port).
 *   2. Fetch fires; if it fails (timeout / dead-host / peer-close),
 *      hctx_fail consults auto_upgrade_check(c->pool_key).
 *   3.5 fixes299: lookup is NON-destructive — every failed HTTPS fetch
 *      for a marked host falls back to HTTP, not just the first.
 *   3. If marked, emit FETCH_REDIRECT to the http:// equivalent so
 *      NetSurf core re-issues via the HTTP fetcher.
 * Mark is consumed (single-shot) so we don't redirect-loop.
 *
 * fixes249c — track host:port instead of full URL string. The mark
 * was set with the raw form ("https://example.com" sans trailing slash)
 * but nsurl_access(c->url) returns the NetSurf-normalised form
 * ("https://example.com/" with slash). strcmp mismatched and the
 * fallback never fired. host:port matches across all path/query
 * variants and is exactly what we want anyway (the host is the unit
 * of "is this server HTTPS-capable"). */
#define HTTPS_AUTO_UP_MAX 8
static char auto_upgrade_list[HTTPS_AUTO_UP_MAX][HTTPS_POOL_KEY_LEN];
static int  auto_upgrade_count = 0;

/* Extract host:port out of "scheme://host[:port]/...". Writes into out
 * (cap chars). Returns 1 on success, 0 if the URL has no recognizable
 * scheme://host shape. */
static int auto_upgrade_extract_key(const char *url, char *out, int cap,
		int default_port)
{
	const char *p;
	const char *host_start;
	const char *host_end;
	int port;
	char host[256];
	size_t host_len;

	if (url == NULL || out == NULL || cap <= 0) return 0;
	p = strstr(url, "://");
	if (p == NULL) return 0;
	host_start = p + 3;
	host_end = host_start;
	while (*host_end != '\0' && *host_end != '/' &&
	       *host_end != '?' && *host_end != '#') host_end++;
	host_len = (size_t)(host_end - host_start);
	if (host_len == 0 || host_len >= sizeof host) return 0;

	/* Split off :port if present. */
	{
		const char *colon = NULL;
		size_t i;
		for (i = 0; i < host_len; i++) {
			if (host_start[i] == ':') colon = host_start + i;
		}
		if (colon != NULL) {
			size_t hlen = (size_t)(colon - host_start);
			if (hlen >= sizeof host) return 0;
			memcpy(host, host_start, hlen);
			host[hlen] = '\0';
			port = atoi(colon + 1);
			if (port <= 0 || port > 65535) port = default_port;
		} else {
			memcpy(host, host_start, host_len);
			host[host_len] = '\0';
			port = default_port;
		}
	}

	if ((int)strlen(host) + 8 >= cap) return 0;
	sprintf(out, "%s:%d", host, port);
	return 1;
}

void macsurf_auto_upgrade_mark(const char *url)
{
	char key[HTTPS_POOL_KEY_LEN];
	int i;
	if (!auto_upgrade_extract_key(url, key, sizeof key, 443)) return;
	for (i = 0; i < auto_upgrade_count; i++) {
		if (strcmp(auto_upgrade_list[i], key) == 0) return;
	}
	if (auto_upgrade_count >= HTTPS_AUTO_UP_MAX) {
		for (i = 0; i < HTTPS_AUTO_UP_MAX - 1; i++) {
			strncpy(auto_upgrade_list[i],
				auto_upgrade_list[i + 1],
				HTTPS_POOL_KEY_LEN);
		}
		auto_upgrade_count--;
	}
	strncpy(auto_upgrade_list[auto_upgrade_count], key,
		HTTPS_POOL_KEY_LEN - 1);
	auto_upgrade_list[auto_upgrade_count][HTTPS_POOL_KEY_LEN - 1] = '\0';
	auto_upgrade_count++;
}

/* fixes299 / #141 — non-destructive lookup.  Retained for backward
 * compatibility; not consulted by the fixes317 fallback logic which
 * always falls back to the other scheme regardless of mark state. */
static int auto_upgrade_check(const char *key)
{
	int i;
	if (key == NULL || key[0] == '\0') return 0;
	for (i = 0; i < auto_upgrade_count; i++) {
		if (strcmp(auto_upgrade_list[i], key) == 0) return 1;
	}
	return 0;
}

/* ---------- fixes317 — per-host scheme-attempt tracker --------------
 * Replaces the asymmetric fixes249b/249c/299 mechanism (which only
 * fell back from HTTPS→HTTP for no-scheme-typed URLs). New rule:
 *
 *   - First attempt for a host uses whatever scheme the URL specifies
 *     (HTTPS by default for no-scheme typing, per fixes249).
 *   - If that scheme fails, FETCH_REDIRECT to the OTHER scheme.
 *   - If the other scheme ALSO fails (or a server-side 301 bounces
 *     back to the originally-failed scheme), surface FETCH_ERROR.
 *
 * Implementation: per-host (host:port) flags https_tried, http_tried.
 * Set when that scheme's fetch fails for the host. Both fetchers
 * consult macsurf_scheme_was_X_tried() and skip the redirect emit
 * if the alternate scheme has already failed.
 *
 * State is reset by macsurf_site_navigation_reset() so each top-level
 * navigation gets a fresh per-host budget. Sub-resource fetches and
 * embedded redirects within the same nav share the budget — exactly
 * what we want for bounce-loop prevention. */
#define HTTPS_SCHEME_TRACK_MAX 32
struct macsurf__scheme_track {
	char host_port[HTTPS_POOL_KEY_LEN];
	int  https_failed;
	int  http_failed;
};
static struct macsurf__scheme_track scheme_track_list[HTTPS_SCHEME_TRACK_MAX];
static int scheme_track_count = 0;

static struct macsurf__scheme_track *scheme_track_find_or_add(const char *key)
{
	int i;
	if (key == NULL || key[0] == '\0') return NULL;
	for (i = 0; i < scheme_track_count; i++) {
		if (strcmp(scheme_track_list[i].host_port, key) == 0)
			return &scheme_track_list[i];
	}
	if (scheme_track_count >= HTTPS_SCHEME_TRACK_MAX) {
		/* FIFO evict slot 0 */
		for (i = 0; i < HTTPS_SCHEME_TRACK_MAX - 1; i++) {
			scheme_track_list[i] = scheme_track_list[i + 1];
		}
		scheme_track_count--;
	}
	strncpy(scheme_track_list[scheme_track_count].host_port, key,
		HTTPS_POOL_KEY_LEN - 1);
	scheme_track_list[scheme_track_count].host_port[HTTPS_POOL_KEY_LEN - 1] = '\0';
	scheme_track_list[scheme_track_count].https_failed = 0;
	scheme_track_list[scheme_track_count].http_failed = 0;
	scheme_track_count++;
	return &scheme_track_list[scheme_track_count - 1];
}

int macsurf_scheme_was_https_tried(const char *key)
{
	int i;
	if (key == NULL || key[0] == '\0') return 0;
	for (i = 0; i < scheme_track_count; i++) {
		if (strcmp(scheme_track_list[i].host_port, key) == 0)
			return scheme_track_list[i].https_failed;
	}
	return 0;
}

int macsurf_scheme_was_http_tried(const char *key)
{
	int i;
	if (key == NULL || key[0] == '\0') return 0;
	for (i = 0; i < scheme_track_count; i++) {
		if (strcmp(scheme_track_list[i].host_port, key) == 0)
			return scheme_track_list[i].http_failed;
	}
	return 0;
}

void macsurf_scheme_mark_https_failed(const char *key)
{
	struct macsurf__scheme_track *t = scheme_track_find_or_add(key);
	if (t != NULL) t->https_failed = 1;
}

void macsurf_scheme_mark_http_failed(const char *key)
{
	struct macsurf__scheme_track *t = scheme_track_find_or_add(key);
	if (t != NULL) t->http_failed = 1;
}

void macsurf_scheme_reset_all(void)
{
	scheme_track_count = 0;
}

/* Build a host:port key from "host" (already-parsed) + port int. */
static int macsurf_scheme_key_from_host_port(char *out, int cap,
	const char *host, int port)
{
	int n;
	if (out == NULL || cap <= 0 || host == NULL || host[0] == '\0') return 0;
	n = (int)strlen(host);
	if (n + 8 >= cap) return 0;
	sprintf(out, "%s:%d", host, port);
	return 1;
}

/* ---------- session-scope dead-host blocklist (fixes236) ----------
 * When a host:port times out (connection timed out, peer closed before
 * complete, etc.) it's almost always either fingerprint-blocked (Google
 * / Facebook reject BearSSL's JA3) or genuinely down. Either way, the
 * outcome doesn't change within a session, so every retry NetSurf core
 * issues just costs another NO_PROGRESS_TICKS=4s of dead time blocking
 * finish_conversion. A 16-entry session-lifetime blocklist short-
 * circuits second and subsequent attempts: setup still allocates a
 * slot so NetSurf core sees a clean FETCH_ERROR path, but hctx_poll
 * fast-fails on first tick instead of retrying handshake+timeout. */
#define HTTPS_DEADHOSTS    16
static char dead_hosts[HTTPS_DEADHOSTS][HTTPS_POOL_KEY_LEN];
static int  dead_hosts_count = 0;

/* fixes244 — parallel "has-ever-succeeded" set. Critical safety net:
 * a single transient timeout on a host that's been working fine all
 * session would otherwise add the host to dead_hosts and route all
 * future requests to about:fetcherror. With this list, dead_host_add
 * refuses to add a host that we've successfully fetched at any point
 * in the session. Populated from hctx_finish on any 2xx/3xx delivery. */
#define HTTPS_SUCCESS_HOSTS 32
static char success_hosts[HTTPS_SUCCESS_HOSTS][HTTPS_POOL_KEY_LEN];
static int  success_hosts_count = 0;

static int success_host_check(const char *key)
{
	int i;
	if (key == NULL || key[0] == '\0') return 0;
	for (i = 0; i < success_hosts_count; i++) {
		if (strcmp(success_hosts[i], key) == 0) return 1;
	}
	return 0;
}

static void success_host_add(const char *key)
{
	int i;
	if (key == NULL || key[0] == '\0') return;
	if (success_host_check(key)) return;
	if (success_hosts_count >= HTTPS_SUCCESS_HOSTS) {
		/* FIFO evict slot 0 */
		for (i = 0; i < HTTPS_SUCCESS_HOSTS - 1; i++) {
			strncpy(success_hosts[i], success_hosts[i + 1],
				HTTPS_POOL_KEY_LEN);
		}
		success_hosts_count--;
	}
	strncpy(success_hosts[success_hosts_count], key,
		HTTPS_POOL_KEY_LEN - 1);
	success_hosts[success_hosts_count][HTTPS_POOL_KEY_LEN - 1] = '\0';
	success_hosts_count++;
}

static int dead_host_check(const char *key)
{
	int i;
	if (key == NULL || key[0] == '\0') return 0;
	for (i = 0; i < dead_hosts_count; i++) {
		if (strcmp(dead_hosts[i], key) == 0) return 1;
	}
	return 0;
}

/* fixes463: public wrapper so macos9_http_fetcher can check the dead-host
 * list before following a 301 redirect to https://. Breaks the loop:
 * http-fallback -> 301->https -> dead-host fast-fail -> http-fallback */
int macos9_https_host_is_dead(const char *host, int port)
{
	char key[HTTPS_POOL_KEY_LEN];
	snprintf(key, sizeof key, "%s:%d", host, port);
	return dead_host_check(key);
}

/* ---------- per-URL terminal-fail set (fixes554) ----------
 * The dead-host list (above) fast-fails a HOST, but hctx_fail still emits the
 * http scheme-fallback, the server answers 301 back to https, and the cycle
 * repeats.  On cdn.jsdelivr.net (TLS-dead for macTLS) each emoji / avatar /
 * attachment URL therefore generated several fetch attempts per page — the
 * unsequenced subresource storm that manufactured the cache-pressure eviction
 * that triggered the convert_xml_to_box UAF (fixes553).
 *
 * This set marks an individual RESOURCE URL terminally failed on its FIRST
 * dead-host fast-fail.  A terminal URL renders alt text and is NEVER retried:
 * no http fallback, no 301 follow, no re-queue handshake.  It is PER-URL (the
 * full URL string), distinct from the per-HOST dead_hosts list, so it survives
 * dead_hosts FIFO eviction and collapses the storm to one FETCH_ERROR per URL.
 */
#define HTTPS_TERMINAL_URLS   128
#define HTTPS_TERMINAL_URL_LEN 256
static char terminal_urls[HTTPS_TERMINAL_URLS][HTTPS_TERMINAL_URL_LEN];
static int  terminal_urls_count = 0;

static int terminal_url_check(const char *url)
{
	int i;
	if (url == NULL || url[0] == '\0') return 0;
	for (i = 0; i < terminal_urls_count; i++) {
		if (strcmp(terminal_urls[i], url) == 0) return 1;
	}
	return 0;
}

/* fixes1003 — a USER navigation is a retry, so clear both fail sets.
 *
 * The dead-host list and the terminal-URL set exist to collapse a subresource
 * storm inside ONE page load (fixes554: dozens of jsdelivr URLs each looping
 * fast-fail -> http -> 301). For that they are right. Applied to the page the
 * user actually asked for, they are a trap: hardware hit ONE transient TLS
 * timeout on 68kmla.org (215-byte ClientHello sent, 54,081 recv calls, zero
 * bytes back — the host answers fine from elsewhere), and after that the URL
 * was terminal and the host dead for the WHOLE SESSION. Every reload
 * fast-failed without touching the network, then fell back to http and hit
 * "redirect loop: https host unreachable". A site that would have worked on
 * the next attempt was unreachable until quit.
 *
 * Clicking Go/Reload on a top-level URL is an explicit "try again", and that
 * is exactly the signal these sets lack. Clearing them there costs at most one
 * re-attempt per navigation on a genuinely dead host — which then re-populates
 * the sets in the same load and collapses the storm as before. The intra-load
 * protection is unchanged; only the ACROSS-navigation memory goes.
 *
 * Deliberately not a TTL: a timer would guess at how long "transient" lasts,
 * while the user's own retry is unambiguous. */
void macos9_https_clear_fail_sets(void);
void macos9_https_clear_fail_sets(void)
{
	int had_dead = dead_hosts_count;
	int had_term = terminal_urls_count;
	dead_hosts_count = 0;
	terminal_urls_count = 0;
	if (had_dead != 0 || had_term != 0) {
		macsurf_debug_log_writef(
			"LIFE navretry: cleared dead-hosts=%d terminal-urls=%d",
			had_dead, had_term);
	}
}

/* Add url to the terminal set.  Returns 1 if NEWLY added (caller logs the
 * TERMINAL FAIL line exactly once per URL), 0 if already present or untracked. */
static int terminal_url_add(const char *url)
{
	int i;
	if (url == NULL || url[0] == '\0') return 0;
	/* too long to store verbatim: let it fail the normal (per-host) way
	 * rather than store a truncated key that could collide. */
	if (strlen(url) >= HTTPS_TERMINAL_URL_LEN) return 0;
	for (i = 0; i < terminal_urls_count; i++) {
		if (strcmp(terminal_urls[i], url) == 0) return 0;
	}
	if (terminal_urls_count >= HTTPS_TERMINAL_URLS) {
		/* FIFO evict slot 0 */
		for (i = 0; i < HTTPS_TERMINAL_URLS - 1; i++) {
			strncpy(terminal_urls[i], terminal_urls[i + 1],
				HTTPS_TERMINAL_URL_LEN);
		}
		terminal_urls_count--;
	}
	strncpy(terminal_urls[terminal_urls_count], url,
		HTTPS_TERMINAL_URL_LEN - 1);
	terminal_urls[terminal_urls_count][HTTPS_TERMINAL_URL_LEN - 1] = '\0';
	terminal_urls_count++;
	return 1;
}

/* fixes554: public wrapper so macos9_http_fetcher can refuse to follow a 301 /
 * scheme-fallback onto a URL already marked terminally failed. */
int macos9_https_url_is_terminal(const char *url)
{
	return terminal_url_check(url);
}

static void dead_host_add(const char *key)
{
	int i;

	if (key == NULL || key[0] == '\0') return;
	/* fixes244 — skip blocklist if host has ever succeeded this session.
	 * Transient timeouts on healthy origins (mactrove during a long
	 * browsing session, etc.) must not poison future requests. */
	if (success_host_check(key)) {
		macsurf_debug_log_writef(
			"https: dead-host SKIP (previously succeeded) %s",
			key);
		return;
	}
	for (i = 0; i < dead_hosts_count; i++) {
		if (strcmp(dead_hosts[i], key) == 0) return;
	}
	if (dead_hosts_count >= HTTPS_DEADHOSTS) {
		/* FIFO evict slot 0 */
		for (i = 0; i < HTTPS_DEADHOSTS - 1; i++) {
			strncpy(dead_hosts[i], dead_hosts[i + 1],
				HTTPS_POOL_KEY_LEN);
		}
		dead_hosts_count--;
	}
	strncpy(dead_hosts[dead_hosts_count], key, HTTPS_POOL_KEY_LEN - 1);
	dead_hosts[dead_hosts_count][HTTPS_POOL_KEY_LEN - 1] = '\0';
	dead_hosts_count++;
	macsurf_debug_log_writef("https: dead-host ADD %s count=%d",
		key, dead_hosts_count);

	/* fixes705 — persistence to disk REMOVED. The list is now purely
	 * session-scoped (in-memory). It still fast-fails a genuinely-dead host
	 * for the rest of THIS session (killing the jsdelivr subresource storm
	 * and the per-subresource 4s NO_PROGRESS timeout), but it NEVER survives
	 * a relaunch. The old deadhosts.txt (written into the Cache folder,
	 * loaded at every launch, no TTL) let a single transient TLS failure on
	 * a site's MAIN document poison that host permanently — the site then
	 * fast-failed to a BLANK page before any fetch was even attempted, and
	 * the only cure was deleting the cache. success_host_check only ever
	 * covered same-session successes, so a host whose first-ever event was a
	 * transient failure could never self-heal. Removing persistence (plus
	 * macos9_https_forget_host on explicit navigation, below) closes that.
	 * Cost: fonts.googleapis.com etc. eat one 4s timeout per session again —
	 * cheap next to permanently-blank sites. */
}

/* fixes705 — remove a host:port from the in-session dead list (and any
 * terminal-URL entries under it), so an EXPLICIT user navigation always gets
 * a fresh attempt even if that host fast-failed earlier this session.
 * Subresource storms don't route through here, so their in-page fast-fail
 * protection is untouched. Only ever REMOVES entries — cannot break a fetch. */
void macos9_https_forget_host_key(const char *key)
{
	int i, j;
	if (key == NULL || key[0] == '\0') return;
	for (i = 0; i < dead_hosts_count; i++) {
		if (strcmp(dead_hosts[i], key) != 0) continue;
		for (j = i; j < dead_hosts_count - 1; j++)
			strncpy(dead_hosts[j], dead_hosts[j + 1], HTTPS_POOL_KEY_LEN);
		dead_hosts_count--;
		macsurf_debug_log_writef("https: dead-host FORGET %s", key);
		break;
	}
	/* forget terminal-URL entries whose host part matches this key's host */
	{
		size_t klen = strlen(key);
		const char *colon = strrchr(key, ':');
		size_t hlen = colon ? (size_t)(colon - key) : klen;
		i = 0;
		while (i < terminal_urls_count) {
			const char *u = terminal_urls[i];
			const char *h = NULL;
			/* find host start after scheme "://" */
			const char *p = strstr(u, "://");
			if (p != NULL) h = p + 3; else h = u;
			if (h != NULL && strncmp(h, key, hlen) == 0 &&
			    (h[hlen] == '/' || h[hlen] == ':' || h[hlen] == '\0')) {
				for (j = i; j < terminal_urls_count - 1; j++)
					strcpy(terminal_urls[j], terminal_urls[j + 1]);
				terminal_urls_count--;
				continue;   /* re-check the slot we shifted down */
			}
			i++;
		}
	}
}

/* Public: forget ALL session dead-host + terminal-URL state (used by the
 * Clear Cache menu item so a wipe takes effect immediately, no relaunch). */
void macos9_https_forget_all(void)
{
	dead_hosts_count = 0;
	terminal_urls_count = 0;
	macsurf_debug_log_writef("https: dead-host FORGET ALL");
}

/* Public: forget the dead/terminal state for the host of an explicit
 * navigation URL. Parses "scheme://host[:port]" and defaults the port.
 * Called from macos9_window_navigate. Silent no-op if it can't parse. */
void macos9_https_forget_host(const char *url)
{
	const char *h, *p, *e;
	char host[HTTPS_POOL_KEY_LEN];
	char key[HTTPS_POOL_KEY_LEN];
	size_t hlen;
	int port;
	int https;
	if (url == NULL) return;
	https = (strncmp(url, "https://", 8) == 0);
	p = strstr(url, "://");
	h = (p != NULL) ? p + 3 : url;
	/* host runs until ':' (port), '/' (path), or end */
	e = h;
	while (*e != '\0' && *e != '/' && *e != ':') e++;
	hlen = (size_t)(e - h);
	/* host[256] matches c->host; the +":port" then fits key[HTTPS_POOL_KEY_LEN
	 * =280] with room to spare (no sprintf overflow). A longer host can't
	 * match any real pool_key anyway. */
	if (hlen == 0 || hlen >= 256) return;
	memcpy(host, h, hlen);
	host[hlen] = '\0';
	port = https ? 443 : 80;
	if (*e == ':') port = atoi(e + 1);
	if (sprintf(key, "%s:%d", host, port) < 0) return;
	macos9_https_forget_host_key(key);
}

/* ---------- keep-alive pool (fixes231) ---------- */

struct https_pool_entry {
	OSTLSConnection *conn;
	char             key[HTTPS_POOL_KEY_LEN];
	unsigned long    stored_ticks;   /* fixes246 */
};
static struct https_pool_entry https_pool[HTTPS_POOL_SIZE];
static int https_pool_count = 0;

/* fixes246 — pool entry TTL. Servers commonly close idle connections
 * after 30-60s (nginx default keepalive_timeout is 75s; CF is ~30s,
 * Apache is 5-15s, Drupal/PHP-FPM behind nginx inherits nginx). A
 * conservative TTL of 20s lets us reuse during typical click-around-
 * a-site cadence while not handing out a connection the server has
 * already silently closed. The previous "Pump 1 step then check state"
 * check at take-time caught some stale entries but not all — if the
 * server closes via TCP FIN that hasn't arrived in our notifier yet,
 * the entry looks fine on take but the next OSTLS_Write fails. */
#define HTTPS_POOL_TTL_TICKS 4500  /* 75s at 60Hz — matches nginx default keepalive_timeout */

/* Try to take a usable connection out of the pool for `key`. Returns
 * NULL if no match or if the matched entry's state is no longer Open
 * (server-side idle close, RST, etc.). Discards stale entries it walks
 * past. Compacts on success by moving the tail entry into the freed
 * slot. */
static OSTLSConnection *
https_pool_take(const char *key)
{
	int i;
	OSTLSConnection *conn;
	OSTLSState st;
	OSTLSEvent ev;
	OSErr e;

	if (key == NULL || key[0] == '\0') return NULL;

	for (i = https_pool_count - 1; i >= 0; i--) {
		unsigned long age;
		if (strcmp(https_pool[i].key, key) != 0) continue;
		conn = https_pool[i].conn;

		/* fixes246 — TTL check before any state probe. Pool entries
		 * older than HTTPS_POOL_TTL_TICKS are presumed dead because
		 * the server has likely closed them silently. */
		age = (unsigned long)TickCount() - https_pool[i].stored_ticks;
		https_pool[i] = https_pool[https_pool_count - 1];
		https_pool_count--;

		if (age > HTTPS_POOL_TTL_TICKS) {
			macsurf_debug_log_writef(
				"https_pool: discard TTL key=%s age=%ld",
				key, (long)age);
			OSTLS_Close(conn);
			OSTLS_Dispose(conn);
			i = https_pool_count;
			continue;
		}

		/* Pump 1 step so any pending notifier events (peer close
		 * via OT) get folded into BearSSL state before we test it. */
		ev = kOSTLSEventNone;
		e = OSTLS_Pump(conn, 1, &ev);
		st = OSTLS_GetState(conn);
		if (e == kOSTLSAsync_OK && st == kOSTLSStateOpen &&
		    ev != kOSTLSEventClosed && ev != kOSTLSEventFailed) {
			macsurf_debug_log_writef(
				"https_pool: REUSE key=%s age=%ld remaining=%d",
				key, (long)age, https_pool_count);
			return conn;
		}
		/* Stale: dispose and keep scanning for another match. */
		macsurf_debug_log_writef(
			"https_pool: discard stale key=%s state=%d ev=%d",
			key, (int)st, (int)ev);
		OSTLS_Close(conn);
		OSTLS_Dispose(conn);
		i = https_pool_count;   /* restart from new top after compaction */
	}
	return NULL;
}

/* Return a connection to the pool. Caller MUST have verified it's idle
 * (response complete, no pending body bytes, state still Open). If the
 * pool is full, dispose the oldest entry to make room (LRU). */
static void
https_pool_return(const char *key, OSTLSConnection *conn)
{
	if (conn == NULL || key == NULL || key[0] == '\0') return;

	if (https_pool_count >= HTTPS_POOL_SIZE) {
		/* Pool full — evict slot 0 (oldest by insertion order). */
		OSTLSConnection *evict = https_pool[0].conn;
		macsurf_debug_log_writef(
			"https_pool: FULL evict key=%s for key=%s",
			https_pool[0].key, key);
		OSTLS_Close(evict);
		OSTLS_Dispose(evict);
		/* Shift down */
		{
			int j;
			for (j = 0; j < https_pool_count - 1; j++) {
				https_pool[j] = https_pool[j + 1];
			}
		}
		https_pool_count--;
	}

	https_pool[https_pool_count].conn = conn;
	strncpy(https_pool[https_pool_count].key, key, HTTPS_POOL_KEY_LEN - 1);
	https_pool[https_pool_count].key[HTTPS_POOL_KEY_LEN - 1] = '\0';
	https_pool[https_pool_count].stored_ticks = (unsigned long)TickCount();
	https_pool_count++;
	macsurf_debug_log_writef(
		"https_pool: STORED key=%s count=%d",
		key, https_pool_count);
}

/* fixes449: flush all idle pooled connections at navigation time.
 * Called from browser_window_stop via extern.  Eliminates the vector
 * where a pooled OSTLSConnection's OT notifier fires a late event
 * during the new page's fetch and calls back into stale context.
 * Diagnostic step: if the use-after-free crash family disappears, the
 * pool is confirmed as a contributing source. */
void https_pool_flush_all(void)
{
	int i;
	int flushed = 0;
	for (i = 0; i < https_pool_count; i++) {
		if (https_pool[i].conn != NULL) {
			OSTLS_Close(https_pool[i].conn);
			OSTLS_Dispose(https_pool[i].conn);
			https_pool[i].conn = NULL;
			flushed++;
		}
	}
	https_pool_count = 0;
	if (flushed > 0) {
		macsurf_debug_log_writef(
			"https_pool: flush_on_navigate flushed=%d", flushed);
	}
}

/* ---------- helpers ---------- */

static unsigned long now_ticks(void)
{
#ifdef __MACOS9__
	return (unsigned long)TickCount();
#else
	return 0;
#endif
}

/* fixes965 — forward declarations: the header-parse loop creates the gzip
 * decoder well before feed_body and its helpers are defined. */
static void hctx_gz_emit(void *cbctx, const char *data, long len);
static void hctx_deliver(struct macos9_https_ctx *c, const char *data, long len);
static int hctx_body_out(struct macos9_https_ctx *c, const char *data, long len);

static void hctx_clear(struct macos9_https_ctx *c)
{
	macsurf_debug_log_writef("https_teardown: host=%s state=%d",
		c->host[0] ? c->host : "(null)", (int)c->state);
	if (c->hdr_buf) { free(c->hdr_buf); c->hdr_buf = NULL; }
	c->hdr_len = 0;
	c->hdr_cap = 0;
	if (c->conn) {
		OSTLS_Close(c->conn);
		OSTLS_Dispose(c->conn);
		c->conn = NULL;
		macsurf_debug_log_writef("https_teardown: OT closed host=%s",
			c->host[0] ? c->host : "(null)");
	}
	if (c->gz) { macos9_gunzip_destroy(c->gz); c->gz = NULL; }   /* fixes965 */
	if (c->cache_capture) { free(c->cache_capture); c->cache_capture = NULL; }
	c->cache_cap_len = 0;
	c->cache_cap_cap = 0;
	/* fixes987 — any stream still open here never reached FETCH_FINISHED,
	 * so it is an abort: close and DELETE. This is the single teardown
	 * point every failure path already funnels through, which is why the
	 * file handle can be owned across the fetch at all. */
	if (c->cache_stream != 0) {
		macos9_cache_stream_end(c->cache_stream, 0);
		c->cache_stream = 0;
	}
	if (c->cache_hit_body) { free(c->cache_hit_body); c->cache_hit_body = NULL; }
	c->cache_hit_len = 0;
	c->cache_hdrs[0] = '\0';       /* fixes981 */
	c->cache_hit_hdrs[0] = '\0';
	/* fixes312 (#144) — release captured POST body. */
	if (c->post_body) { free(c->post_body); c->post_body = NULL; }
	c->post_body_len = 0;
	c->post_body_sent = 0;
	if (c->url) {
		nsurl_unref(c->url);
		c->url = NULL;
		macsurf_debug_log_writef("https_teardown: url released host=%s",
			c->host[0] ? c->host : "(null)");
	}
	c->state = HS_IDLE;
}

/* fixes228 — tear down JUST the TLS connection so the slot can be
 * retried. Keeps c->parent, c->url, c->host, c->port, c->path so the
 * next poll-loop pass can reopen with the same target. Resets header
 * + body capture state so we don't mix data from the failed attempt. */
static void hctx_reset_for_retry(struct macos9_https_ctx *c)
{
	if (c->conn) {
		OSTLS_Close(c->conn);
		OSTLS_Dispose(c->conn);
		c->conn = NULL;
	}
	if (c->hdr_buf) { free(c->hdr_buf); c->hdr_buf = NULL; }
	c->hdr_len = 0;
	c->hdr_cap = 0;
	if (c->gz) { macos9_gunzip_destroy(c->gz); c->gz = NULL; }   /* fixes965 */
	if (c->cache_capture) { free(c->cache_capture); c->cache_capture = NULL; }
	c->cache_cap_len = 0;
	c->cache_cap_cap = 0;
	c->cache_overflow = 0;
	c->cache_eligible = 0;
	if (c->cache_stream != 0) {       /* fixes987 */
		macos9_cache_stream_end(c->cache_stream, 0);
		c->cache_stream = 0;
	}
	c->req_len = 0;
	c->req_sent = 0;
	/* fixes312 (#144) — keep post_body intact so the retry sends the
	 * same payload; just rewind the send counter. */
	c->post_body_sent = 0;
	c->status = 0;
	c->body_bytes = 0;
	c->content_length = -1;
	c->chunked = 0;
	c->mime[0] = 0;
	c->redirect_url[0] = 0;
	c->state = HS_QUEUED;
	/* Do NOT clear c->aborted: if NetSurf aborted during the first
	 * attempt, we should NOT retry. The first thing the next poll
	 * pass checks is c->aborted; it'll fail cleanly. */
	c->progress_ticks = now_ticks();
}

/* fixes218 — append body bytes to the per-fetch capture buffer.
 * Same geometric-growth + overflow-latch discipline as the HTTP
 * fetcher's cache_capture_append. */
static void hctx_cache_capture(struct macos9_https_ctx *c,
		const char *buf, long len)
{
	long want;
	long cap;
	char *grown;

	if (c == NULL || buf == NULL || len <= 0) return;
	if (c->cache_overflow) return;
	if (!c->cache_eligible) return;

	want = c->cache_cap_len + len;
	if (want > MACSURF_CACHE_MAX_BYTES) {
		c->cache_overflow = 1;
		if (c->cache_capture != NULL) {
			free(c->cache_capture);
			c->cache_capture = NULL;
			c->cache_cap_len = 0;
			c->cache_cap_cap = 0;
		}
		return;
	}

	if (want > c->cache_cap_cap) {
		cap = c->cache_cap_cap == 0 ? 4096 : c->cache_cap_cap * 2;
		while (cap < want) cap *= 2;
		if (cap > MACSURF_CACHE_MAX_BYTES) cap = MACSURF_CACHE_MAX_BYTES;
		grown = (char *)realloc(c->cache_capture, cap);
		if (grown == NULL) {
			c->cache_overflow = 1;
			if (c->cache_capture != NULL) {
				free(c->cache_capture);
				c->cache_capture = NULL;
				c->cache_cap_len = 0;
				c->cache_cap_cap = 0;
			}
			return;
		}
		c->cache_capture = grown;
		c->cache_cap_cap = cap;
	}

	memcpy(c->cache_capture + c->cache_cap_len, buf, len);
	c->cache_cap_len += len;
}

/* fixes369b (#167) — decode a BearSSL error code (OSTLSDiagnostics.br_err)
 * to its name so a failed handshake is diagnosable from the log at a glance
 * instead of as a bare number. The Facebook page-load suspects map straight
 * to names here: X509_NOT_TRUSTED (62) = the chain's root anchor isn't in
 * our CA bundle; X509_BAD_SIGNATURE (52) = a cert-chain signature (e.g. the
 * RSA intermediate signing FB's leaf) didn't verify; INVALID_ALGORITHM (26)
 * = a signature algorithm we don't offer/handle. BearSSL: SSL errors 0-31,
 * X.509 errors are 32+, fatal-alert ranges at 256/512. */
/* fixes957 — what date does this Mac think it is?
 *
 * Written into the failure reason (below) so about:query/fetcherror can show
 * it.  This lives HERE, not in the about: fetcher, for two reasons: this TU
 * already has the Toolbox, and macos9_fetcher_stubs.c does not (adding Carbon
 * to it would drag in the header chains macos9.h spends 60 lines suppressing).
 * One code path builds the date line for every case that needs it.
 *
 * Mac seconds are from 1904-01-01; 1904-01-01 to 1970-01-01 is 24107 days, so
 * shift into days-since-1970 and run the standard civil-from-days conversion
 * (all long arithmetic -- no 64-bit multiply, which CW8 PPC miscompiles). */
static void macos9_believed_date(char *out, size_t cap)
{
	long z, era, doe, yoe, y, doy, mp, d, m;
	unsigned long secs = 0;

	if (out == NULL || cap < 11) return;
	out[0] = '\0';
#ifdef __MACOS9__
	GetDateTime(&secs);
#endif
	if (secs == 0) return;

	z = (long)(secs / 86400UL) - 24107L;
	z += 719468L;
	era = (z >= 0 ? z : z - 146096L) / 146097L;
	doe = z - era * 146097L;
	yoe = (doe - doe / 1460L + doe / 36524L - doe / 146096L) / 365L;
	y   = yoe + era * 400L;
	doy = doe - (365L * yoe + yoe / 4L - yoe / 100L);
	mp  = (5L * doy + 2L) / 153L;
	d   = doy - (153L * mp + 2L) / 5L + 1L;
	m   = mp + (mp < 10L ? 3L : -9L);
	if (m <= 2L) y++;

	sprintf(out, "%ld-%02ld-%02ld", y, m, d);
}

static const char *ostls_br_err_name(int e)
{
	switch (e) {
	case 0:  return "OK";
	case 1:  return "BAD_PARAM";
	case 2:  return "BAD_STATE";
	case 3:  return "UNSUPPORTED_VERSION";
	case 4:  return "BAD_VERSION";
	case 5:  return "BAD_LENGTH";
	case 7:  return "BAD_MAC";
	case 8:  return "NO_RANDOM";
	case 10: return "UNEXPECTED";
	case 13: return "BAD_ALERT";
	case 14: return "BAD_HANDSHAKE";
	case 16: return "BAD_CIPHER_SUITE";
	case 19: return "BAD_SECRENEG";
	case 21: return "BAD_SNI";
	case 24: return "BAD_FINISHED";
	case 26: return "INVALID_ALGORITHM";
	case 27: return "BAD_SIGNATURE";
	case 28: return "WRONG_KEY_USAGE";
	case 31: return "IO";
	case 32: return "X509_OK";
	case 34: return "X509_TRUNCATED";
	case 35: return "X509_EMPTY_CHAIN";
	case 49: return "X509_UNSUPPORTED";
	case 50: return "X509_LIMIT_EXCEEDED";
	case 51: return "X509_WRONG_KEY_TYPE";
	case 52: return "X509_BAD_SIGNATURE";
	case 53: return "X509_TIME_UNKNOWN";
	case 54: return "X509_EXPIRED";
	case 55: return "X509_DN_MISMATCH";
	case 56: return "X509_BAD_SERVER_NAME";
	case 58: return "X509_NOT_CA";
	case 59: return "X509_FORBIDDEN_KEY_USAGE";
	case 60: return "X509_WEAK_PUBLIC_KEY";
	case 62: return "X509_NOT_TRUSTED";
	default: break;
	}
	if (e >= 512) return "SEND_FATAL_ALERT";
	if (e >= 256) return "RECV_FATAL_ALERT";
	return "?";
}

/* fixes957 — is this BearSSL error a CERTIFICATE REJECTION (as opposed to a
 * transport/handshake failure)?
 *
 * BearSSL packs every X.509 validation error into 33..62 (32 is X509_OK; see
 * bearssl_x509.h).  A code in that band means the server DID speak TLS and
 * presented a certificate we refused: expired, not-yet-valid, self-signed,
 * hostname mismatch, untrusted root.  Everything OUTSIDE the band means we
 * never got a usable certificate at all -- no TLS on the port, a macTLS
 * incompatibility, a truncated connection.
 *
 * That distinction is the whole point.  hctx_fail's http:// fallback exists
 * for the outside-the-band case (fixes249b/317: retro HTTP-only hosts reached
 * by typing a bare hostname).  Applying it to a REJECTED CERTIFICATE is a
 * silent downgrade to cleartext -- MacSurf validating a cert, refusing it,
 * and then fetching the same page in the clear anyway.  Nothing else in the
 * browser misrepresents itself that way, and it is exactly what a packet
 * capture would expose.  So: cert rejections fail closed, everything else
 * keeps today's behaviour. */
static int cert_rejected(struct macos9_https_ctx *c, const OSTLSDiagnostics *diag)
{
	int e;
	if (c == NULL || c->conn == NULL || diag == NULL) return 0;
	e = (int)diag->br_err;
	return (e >= 33 && e <= 62) ? 1 : 0;
}

/* fixes962 — did the PEER FAIL TO AUTHENTICATE ITSELF, as distinct from
 * presenting a certificate we refused?
 *
 * fixes957 keyed the no-downgrade decision on the X.509 band alone (33..62),
 * which is the space of "a chain reached the validator and was refused". That
 * is correct about what it means and incomplete about what matters. Consider
 * an on-path attacker who replays the target's REAL, currently-valid,
 * correctly-named certificate chain but does not hold the private key:
 *
 *   - X.509 validation passes. br_err never enters 33..62.
 *   - The handshake dies later, at CertificateVerify, because the peer cannot
 *     sign the transcript: BR_ERR_BAD_SIGNATURE (27), raised at
 *     ostls_tls13_handshake.c:2181 and :2231.
 *
 * Under fixes957 that produced is_cert_fail == 0 and the cleartext refetch ran
 * -- against precisely the adversary the change was written to stop. Same hole
 * for the TLS 1.2 ServerKeyExchange signature and for a tampered Finished
 * (BR_ERR_BAD_FINISHED, 24), which is a MAC failure over the whole handshake.
 *
 * These are kept SEPARATE from cert_rejected() rather than folded into one
 * band because the user-facing message must differ: "this certificate has
 * expired" is wrong and misleading for a Finished MAC failure. */
static int peer_auth_failed(struct macos9_https_ctx *c,
			    const OSTLSDiagnostics *diag)
{
	int e;
	if (c == NULL || c->conn == NULL || diag == NULL) return 0;
	e = (int)diag->br_err;
	/* 24 BAD_FINISHED, 26 INVALID_ALGORITHM, 27 BAD_SIGNATURE,
	 * 28 WRONG_KEY_USAGE -- see macTLS/bearssl/inc/bearssl_ssl.h. */
	return (e == 24 || e == 26 || e == 27 || e == 28) ? 1 : 0;
}

/* fixes957 — a cert rejection whose cause is PLAUSIBLY THE MAC'S CLOCK.
 * BR_ERR_X509_EXPIRED (54) covers both "expired" and "not yet valid", so it
 * is what a Mac reports whether its clock is years behind (dead PRAM battery
 * -> 1904/1956) or years ahead.  53 is TIME_UNKNOWN.  We deliberately do NOT
 * try to decide whether the clock is really wrong -- see the note on the
 * error page, which states the ambiguity and prints the believed date so the
 * user (who can see their own calendar) decides. */
static int cert_time_class(const OSTLSDiagnostics *diag)
{
	int e;
	if (diag == NULL) return 0;
	e = (int)diag->br_err;
	return (e == 53 || e == 54) ? 1 : 0;
}

static void hctx_fail(struct macos9_https_ctx *c, const char *why)
{
	struct fetch *p;
	fetch_msg msg;
	/* fixes718 (#207) — whether HTTPS demonstrably worked. Captured BEFORE
	 * c->state is overwritten with HS_FAIL below. Reaching HS_HEADERS/HS_BODY
	 * means the handshake completed and BearSSL decrypted a response, so the
	 * origin speaks valid HTTPS and this failure is a slow/flaky stall — NOT
	 * an HTTPS-incompatible or dead host. When set, we must NOT fall back to
	 * http:// (which 301s back to https, now dead-hosted -> redirect loop ->
	 * about:fetcherror) and must NOT dead-host a working origin (which blocks
	 * an immediate reload for the whole FIFO window). True dead / retro
	 * HTTP-only / macTLS-incompatible hosts fail at HS_TLSING (pre-response),
	 * so https_worked stays 0 and the existing fallback/dead-host path runs. */
	int https_worked;
	/* fixes957 — hoisted to function scope (was local to the diag-dump
	 * block below).  The http-fallback gate now has to consult br_err to
	 * tell a rejected certificate from a dead port, so the diagnostics are
	 * fetched once here and reused rather than queried twice. */
	OSTLSDiagnostics diag;
	int is_cert_fail;
	int is_auth_fail;   /* fixes962 — peer could not prove it owns the key */
	int no_downgrade;   /* fixes962 — either of the above forbids cleartext */

	if (c->state == HS_FAIL || c->state == HS_DONE) return;

	memset(&diag, 0, sizeof diag);
	if (c->conn != NULL) {
		OSTLS_GetDiagnostics(c->conn, &diag);
	}
	is_cert_fail = cert_rejected(c, &diag);
	is_auth_fail = peer_auth_failed(c, &diag);
	no_downgrade = (is_cert_fail || is_auth_fail);

	https_worked = (c->state == HS_HEADERS || c->state == HS_BODY);
	if (https_worked)
		macsurf_debug_log_writef(
			"https: worked-but-stalled state=%d host=%s — skip "
			"fallback+dead-host", c->state,
			c->host[0] ? c->host : "(unset)");

	/* fixes226 — full diag dump on every fail. We need:
	 *  - host being fetched (so we know WHICH sites fail)
	 *  - BearSSL error code (br_err) on handshake fails
	 *  - OT error code (ot_err) on TCP-level fails
	 *  - cipher suite if handshake completed (0 if not)
	 *  - pump_calls + ot_recv_bytes to see how far we got
	 */
	macsurf_debug_log_writef("https: FAIL state=%d status=%d body=%ld why=%s",
		c->state, c->status, c->body_bytes, why ? why : "(null)");
	macsurf_debug_log_writef("  FAIL host=%s port=%d path=%s",
		c->host[0] ? c->host : "(unset)",
		(int)c->port,
		c->path[0] ? c->path : "(unset)");
	if (c->conn != NULL) {
		/* fixes227 — macsurf_debug_log_writef supports only %d %ld %p %s %%
		 * (see project_macsurf_debug_log_specifiers memory). Cipher
		 * gets printed as decimal; 0xCCA9 = 52393 (ChaCha20-Poly1305),
		 * 0xC02B = 49195 (ECDHE-ECDSA-AES128-GCM-SHA256). */
		macsurf_debug_log_writef(
			"  FAIL diag os_err=%d ot_err=%ld br_err=%d(%s) state=%d cipher_dec=%d",
			(int)diag.os_err, (long)diag.ot_err, (int)diag.br_err,
			ostls_br_err_name((int)diag.br_err),
			(int)diag.state, (int)diag.cipher_suite);
		macsurf_debug_log_writef(
			"  FAIL diag pumps=%ld br_state=%ld",
			(long)diag.pump_calls,
			(long)diag.br_state_last);
		macsurf_debug_log_writef(
			"  FAIL diag ot_send: calls=%ld bytes=%ld zero=%ld flow=%ld",
			(long)diag.ot_send_calls,
			(long)diag.ot_send_bytes,
			(long)diag.ot_send_zero,
			(long)diag.ot_send_flow);
		macsurf_debug_log_writef(
			"  FAIL diag ot_recv: calls=%ld bytes=%ld nodata=%ld",
			(long)diag.ot_recv_calls,
			(long)diag.ot_recv_bytes,
			(long)diag.ot_recv_nodata);
	} else {
		macsurf_debug_log_writef("  FAIL diag conn=NULL (never opened)");
	}

	c->err = why;
	c->state = HS_FAIL;

	/* fixes249b — if this fetch's host was auto-upgraded from a no-scheme
	 * typing (e.g. user typed "retro.example.com" and we made it
	 * "https://retro.example.com"), emit a FETCH_REDIRECT to the
	 * http:// equivalent so retro HTTP-only sites still work. Consumed
	 * (single-shot) to avoid redirect loops. Suppresses the dead-host
	 * add for this URL so a future retry isn't fast-failed.
	 *
	 * fixes249c — match by c->pool_key (host:port) instead of full URL.
	 * The original full-URL match silently failed because NetSurf
	 * normalises bare-host URLs to add a trailing slash, but window.c's
	 * mark used the raw form. */
	/* fixes314 — never emit FETCH_REDIRECT from hctx_fail if NetSurf
	 * has already aborted this fetch. ops.abort (driven by NetSurf
	 * core fetch_abort) nulls llcache's object->fetch.fetch before
	 * returning. If we send a synthetic redirect now, llcache_fetch_
	 * redirect calls fetch_abort(NULL) → crash. Aborts happen on page
	 * navigation, manual cancel, and resource-pressure cancellation;
	 * the fallback is only meaningful when WE detected the failure
	 * (peer-close, timeout, handshake error) — not when NetSurf gave
	 * up on us. */
	/* fixes317 — record this HTTPS failure for the host. Used by both
	 * fetchers to (a) decide whether the OTHER scheme is still worth
	 * trying, and (b) refuse a server-side 3xx that bounces back to a
	 * scheme we've already failed at for this host this navigation. */
	if (c->pool_key[0] != '\0') {
		macsurf_scheme_mark_https_failed(c->pool_key);
	}

	/* fixes317 — always fall back to HTTP when HTTPS fails, regardless
	 * of whether the URL was no-scheme typed (the old auto_upgrade
	 * mark). Gated by the per-host scheme tracker so a host whose HTTP
	 * has ALSO already failed this navigation surfaces FETCH_ERROR
	 * instead of bouncing.
	 * fixes835 (#167 M1) — NEVER downgrade a Facebook host to http. FB is
	 * https-only; an http fallback would (a) get a 301 back to https and
	 * (b) drop the Secure session cookies (xs/datr) on the http hop,
	 * silently breaking a logged-in session ("login succeeds, session
	 * doesn't stick"). host_is_fb_asset covers facebook.com/fbcdn.net/
	 * fbsbx.com/cdninstagram.com on a dot boundary. A transient TLS glitch
	 * on FB surfaces FETCH_ERROR (retryable) instead. */
	/* fixes957 — a REJECTED CERTIFICATE never falls back to cleartext.
	 * This is the one case where the old unconditional fallback turned a
	 * successful security decision into a silent downgrade: BearSSL
	 * validated the chain, refused it, and MacSurf then refetched the same
	 * page over http:// as if nothing had happened.  Record it loudly and
	 * let the fetch fail. */
	/* fixes962 — a transport that failed peer authentication or certificate
	 * validation is tainted, whatever happens next. This is the caller
	 * fixes958 added the API for: llcache_hsts_update_policy now adopts it
	 * and stops believing a Strict-Transport-Security header that arrived
	 * over a connection we could not authenticate. */
	if (no_downgrade && c->parent != NULL) {
		fetch_set_tainted_tls(c->parent, true);
	}

	/* fixes963 — macTLS reported a CLOCK failure. Two very different
	 * situations arrive here as the same code, and they need opposite
	 * responses, so decide with the date rather than assuming.
	 *
	 * kOSTLSAsync_ClockBefore2000 (2002) is returned from OSTLS_Start and
	 * from the handshake entry, and BOTH sites collapse *every*
	 * OSTLS_GetBearSSLTime failure into it (ostls_async.c:521-531 and
	 * :740-743), not just a pre-2000 clock. So:
	 *
	 *   - date really is pre-2000: the dead-PRAM case. The connection dies
	 *     before any certificate is seen, so br_err is 0 and neither
	 *     cert_rejected() nor peer_auth_failed() fires -- meaning fixes957b's
	 *     believed-date message, which was WRITTEN for this case, never
	 *     reached the user. Say it here instead.
	 *
	 *   - date is sane: the time helper failed for some other reason and is
	 *     being misreported as a clock problem. A hardware log from
	 *     2026-07-21 shows macintoshgarden.org and lite.duckduckgo.com
	 *     failing this way REPEATEDLY on a Mac whose clock was demonstrably
	 *     correct (the same session printed date=2026-07-21 on another
	 *     fetch), so this is a real open bug and possibly a live cause of
	 *     failed page loads. Do not show the user clock advice for it --
	 *     mark it loudly instead so the next log identifies it. */
	if (!no_downgrade && (int)diag.os_err == 2002) {
		char clk[24];
		clk[0] = '\0';
		macos9_believed_date(clk, sizeof clk);
		if (clk[0] != '\0' && strncmp(clk, "20", 2) != 0) {
			static char clock_why[192];
			sprintf(clock_why,
				"this Mac's clock reads %s, which is before "
				"2000 -- set the date in the Date & Time control "
				"panel, then reload", clk);
			why = clock_why;
			c->err = why;
			no_downgrade = 1;   /* nothing to gain from cleartext */
			macsurf_debug_log_writef(
				"https: clock INVALID date=%s host=%s", clk,
				c->host[0] ? c->host : "(unset)");
		} else {
			macsurf_debug_log_writef(
				"https: FAIL time-helper reported clock-bad but "
				"date reads %s host=%s -- NOT a clock problem",
				clk[0] ? clk : "(unreadable)",
				c->host[0] ? c->host : "(unset)");
		}
	}

	if (is_auth_fail && !is_cert_fail) {
		/* Deliberately NOT worded as a certificate problem: the
		 * certificate was fine, the peer just could not prove it holds
		 * the matching private key. Telling the user their clock or the
		 * site's cert is at fault would send them somewhere useless. */
		static char auth_why[192];
		sprintf(auth_why,
			"the site could not prove it owns its certificate "
			"(TLS %s) -- refusing to continue unencrypted",
			ostls_br_err_name((int)diag.br_err));
		why = auth_why;
		c->err = why;
		macsurf_debug_log_writef(
			"https: peer auth INVALID br_err=%d(%s) host=%s -- "
			"refusing http fallback", (int)diag.br_err,
			ostls_br_err_name((int)diag.br_err),
			c->host[0] ? c->host : "(unset)");
	}

	if (is_cert_fail) {
		/* Replace the generic "handshake/transport failed" with a reason
		 * that says WHAT was wrong and what date this Mac believes it is.
		 * Core forwards this verbatim to about:query/fetcherror as the
		 * "reason" multipart field, so the page can explain itself instead
		 * of offering three generic guesses.
		 *
		 * Static buffer: OS 9 is cooperative and fetch_send_callback ->
		 * fetch_multipart_data_new_kv copies the string immediately, so it
		 * cannot outlive the next failure. */
		static char cert_why[192];
		char believed[24];

		believed[0] = '\0';
		macos9_believed_date(believed, sizeof believed);

		if (cert_time_class(&diag)) {
			/* 54 covers expired AND not-yet-valid, so this fires
			 * whether the clock is behind (dead PRAM -> 1904/1956)
			 * or ahead.  Deliberately NOT decided here: we state the
			 * ambiguity and print the date, and the user -- who can
			 * see their own calendar -- settles it.  A heuristic
			 * anchored to the build date would rot as the binary
			 * ages and would start giving clock advice for genuinely
			 * expired certificates. */
			sprintf(cert_why,
				"certificate expired or not yet valid; this Mac's "
				"date reads %s",
				believed[0] ? believed : "(unreadable)");
		} else {
			sprintf(cert_why,
				"certificate rejected (%s); this Mac's date reads %s",
				ostls_br_err_name((int)diag.br_err),
				believed[0] ? believed : "(unreadable)");
		}
		why = cert_why;
		c->err = why;   /* keep ctx state and the callback in step */

		macsurf_debug_log_writef(
			"https: cert INVALID br_err=%d(%s) host=%s date=%s -- "
			"refusing http fallback", (int)diag.br_err,
			ostls_br_err_name((int)diag.br_err),
			c->host[0] ? c->host : "(unset)",
			believed[0] ? believed : "?");
	}

	if (c->aborted == 0 &&
	    !https_worked &&   /* fixes718: HTTPS answered — don't fall back to http */
	    !no_downgrade &&   /* fixes957/962: cert or peer-auth failure fails closed */
	    !host_is_fb_asset(c->host) &&
	    c->url != NULL && c->pool_key[0] != '\0' &&
	    !terminal_url_check(nsurl_access(c->url)) &&
	    !macsurf_scheme_was_http_tried(c->pool_key)) {
		const char *u = nsurl_access(c->url);
		if (u != NULL && strncmp(u, "https://", 8) == 0) {
			fetch_msg rm;
			struct fetch *parent_save;
			int n;
			/* fixes262 — write the redirect URL into c->redirect_url
			 * (the per-ctx field that the parse_headers 3xx path
			 * already uses) instead of a stack buffer. NetSurf's
			 * llcache holds the pointer past our return, so a stack
			 * buffer goes dangling and NetSurf routes to
			 * about:fetcherror instead of following the redirect. */
			n = sprintf(c->redirect_url, "http://%s", u + 8);
			if (n > 0 && (size_t)n < sizeof c->redirect_url) {
				macsurf_debug_log_writef(
					"https: auto-upgrade FALLBACK -> %s",
					c->redirect_url);
				/* fixes263 — NetSurf's llcache_fetch_redirect
				 * reads fetch_http_code() and rejects redirects
				 * whose code isn't a recognized 3xx (301/302/
				 * 303/307/308). Default is 0 → "unsupported
				 * redirect" → NSERROR_BAD_REDIRECT → the new
				 * fetch never starts. Set 301 (Moved
				 * Permanently) so llcache treats this as a
				 * normal redirect: change method to GET if
				 * we were posting, follow the new URL. */
				(void)fetch_set_http_code(c->parent, 301);
				rm.type = FETCH_REDIRECT;
				rm.data.redirect = c->redirect_url;
				fetch_send_callback(&rm, c->parent);
				parent_save = c->parent;
				c->parent = NULL; /* fixes447: null before OT teardown */
				/* fixes448: dead-host the :443 key before the HTTP
				 * fallback fires.  Without this, the chain
				 * HTTPS-fail -> HTTP -> server 301 -> HTTPS loops
				 * forever because the second HTTPS attempt is not
				 * blocked.  Cert failures (X509_NOT_TRUSTED) are
				 * session-permanent; success_host_check still guards
				 * against poisoning hosts that actually worked. */
				if (c->pool_key[0] != '\0') {
					dead_host_add(c->pool_key);
				}
				hctx_clear(c);
				fetch_remove_from_queues(parent_save);
				fetch_free(parent_save);
				return;
			}
		}
	}

	/* fixes236 — register dead host so retries skip the timeout. We
	 * blocklist on timeout and on "peer closed before complete" (the
	 * fingerprint-rejection signature). We do NOT blocklist on aborts
	 * (NetSurf cancelling a duplicate fetch) or on transient errors
	 * that might genuinely recover.
	 *
	 * fixes410 — also blocklist on "handshake/transport failed". A host
	 * macTLS cannot complete a TLS handshake with (e.g. cdn.jsdelivr.net,
	 * which a normal TLS-1.2 client reaches fine, so this is macTLS-side)
	 * otherwise gets a fresh handshake attempt for EVERY subresource it
	 * serves — observed as 12 back-to-back jsdelivr handshake failures
	 * costing ~48s of dead time on a single mactrove load. Blocklisting
	 * after the first handshake failure fails the rest FAST; the page is
	 * missing that host's resources either way (the handshake can't
	 * succeed this session), so this only removes the per-subresource
	 * retry cost. The list is per-session and fixes244 refuses to persist
	 * any host that ever succeeded, so a transient failure self-heals
	 * next session. */
	/* fixes957 — a REJECTED CERTIFICATE must NOT dead-host the origin, and
	 * this is stated explicitly rather than left to fall out of the string
	 * comparison below (the cert path now rewrites `why`, so these strcmps
	 * would silently stop matching -- correct, but not something the next
	 * reader should have to deduce).
	 *
	 * Why it matters: the commonest cause is a wrong clock, and we now tell
	 * the user so. If the host were dead-hosted, the user would set the date
	 * in Date & Time, reload, and get an instant fast-fail with no network
	 * activity at all -- making the advice we just gave them look wrong. The
	 * cost of not dead-hosting is that every subresource re-attempts its
	 * handshake while the clock is broken; on a machine where nothing loads
	 * anyway that is the right trade, and it self-corrects the moment the
	 * clock is fixed, with no clock-change detection needed. */
	if (why != NULL && c->pool_key[0] != '\0' && !https_worked &&
	    !no_downgrade &&
	    (strcmp(why, "https: connection timed out") == 0 ||
	     strcmp(why, "https: peer closed before complete") == 0 ||
	     strcmp(why, "https: handshake/transport failed") == 0)) {
		/* fixes718: !https_worked — a host that already spoke valid HTTPS
		 * (HS_HEADERS/HS_BODY) is alive; a stall must not poison it, or an
		 * immediate reload is blocked for the whole dead-host FIFO window. */
		dead_host_add(c->pool_key);
	}

	msg.type = FETCH_ERROR;
	msg.data.error = why ? why : "https: fetch failed";
	fetch_send_callback(&msg, c->parent);

	p = c->parent;
	c->parent = NULL; /* fixes447: null before OT teardown so re-entrant notifier finds NULL */
	hctx_clear(c);
	fetch_remove_from_queues(p);
	fetch_free(p);
}

/* fixes936 (OS X tier 1) — one-shot latch for the completed-fetch summary. */
static int g_recon_ot_done = 0;

/*
 * fixes936 (OS X tier 1) — live Open Transport snapshot, called from the
 * event-loop heartbeat in main.c (~2 s throttle) via a bare extern.
 *
 * Lives HERE, not in main.c, so OSTLSConnection stays private to this TU:
 * the event loop holds no connection pointer and should not be given one.
 * We walk our own slot array exactly like macos9_https_fetcher_active() does.
 *
 * WHY THIS EXISTS: the failure it detects (a notifier that never fires, so
 * ot_recv_nodata climbs while zero bytes are delivered) is precisely the case
 * where the connection NEVER reaches teardown -- so the completed-fetch
 * summary below would never print. A hang is only readable if something
 * brackets it.
 *
 * Capped PER CONNECTION, not per session: a per-session budget could be spent
 * by an early slow-but-successful fetch and then print nothing for the later
 * hang, which is the only event this is for. Each slot gets its own budget,
 * reset when the slot leaves the live set.
 */
#define RECON_OT_TICK_MAX 6   /* snapshots per connection; a stall is obvious
                               * once the counters fail to move across 2-3 */

void macos9_https_recon_ot_tick(void)
{
	static unsigned char tick_budget[MAX_HTTPS_F];
	int i;
	int live = 0;
	int first = -1;

	for (i = 0; i < MAX_HTTPS_F; i++) {
		int st = https_slots[i].state;
		if (st != HS_IDLE && st != HS_DONE && st != HS_FAIL) {
			live++;
			/* Lowest-index live slot that still has budget AND a real
			 * connection. Skipping exhausted slots matters: otherwise a
			 * long-lived slot 0 would spend its budget and then mask a
			 * genuinely hung slot 3 forever. */
			if (first < 0 &&
			    https_slots[i].conn != NULL &&
			    tick_budget[i] < RECON_OT_TICK_MAX) {
				first = i;
			}
		} else {
			/* Slot left the live set -- rearm its budget for next time. */
			tick_budget[i] = 0;
		}
	}

	if (first < 0) return;   /* nothing in flight, or all budgets spent */
	tick_budget[first]++;

	{
		OSTLSDiagnostics od;
		memset(&od, 0, sizeof od);
		OSTLS_GetDiagnostics(https_slots[first].conn, &od);
		/* Lowest-index live slot, deliberately: it is deterministic AND it
		 * converges on the interesting one -- in a hang the other slots
		 * complete and go HS_IDLE, leaving the stuck connection as the only
		 * live slot. slot=/n= make a changing pick visible rather than
		 * looking like interleaved corruption.
		 *
		 * No host= here on purpose: this variant must stay well inside
		 * macsurf_debug_log_writef's silent 255-byte truncation. */
		macsurf_debug_log_writef(
			"RECON OT live slot=%d n=%d st=%d osx=%d ot_err=%ld "
			"snd=%ld/%ld zero=%ld flow=%ld rcv=%ld/%ld nod=%ld pumps=%ld",
			first, live, (int)https_slots[first].state,
			macsurf_os_is_osx(),
			(long)od.ot_err,
			(long)od.ot_send_calls, (long)od.ot_send_bytes,
			(long)od.ot_send_zero,  (long)od.ot_send_flow,
			(long)od.ot_recv_calls, (long)od.ot_recv_bytes,
			(long)od.ot_recv_nodata,
			(long)od.pump_calls);
	}
}

static void hctx_finish(struct macos9_https_ctx *c)
{
	struct fetch *p;
	fetch_msg msg;

	if (c->state == HS_FAIL || c->state == HS_DONE) return;

	/* fixes965 — verify the gzip trailer before calling the body complete.
	 *
	 * Without this a TRUNCATED gzip response would look like a successful
	 * short page: everything decoded so far has already been delivered
	 * (that is what streaming means), and nothing would ever check the
	 * CRC32/ISIZE that says whether the stream actually ended. The Linux
	 * test covers exactly this -- a halved body must report
	 * "gzip stream truncated" rather than succeeding.
	 *
	 * Deliberately fails the fetch rather than salvaging: the bytes already
	 * handed over cannot be recalled, so a caller that treated this as a
	 * warning would render a silently-incomplete page. */
	if (c->gz != NULL && macos9_gunzip_total_in(c->gz) > 0) {
		if (macos9_gunzip_finish(c->gz) != MACOS9_GUNZIP_DONE) {
			macsurf_debug_log_writef(
				"https: FAIL gzip incomplete host=%s: %s",
				c->host[0] ? c->host : "(unset)",
				macos9_gunzip_status(c->gz));
			hctx_fail(c, "https: gzip stream incomplete");
			return;
		}
		macsurf_debug_log_writef("LIFE gzip ok host=%s in=%ld out=%ld",
			c->host[0] ? c->host : "(unset)",
			macos9_gunzip_total_in(c->gz),
			macos9_gunzip_total_out(c->gz));
	}

	macsurf_debug_log_writef("https: done body=%ld status=%d",
		c->body_bytes, c->status);

	/* fixes936 (OS X tier 1) — ONE Open Transport health line per session,
	 * taken at the first HTTPS fetch that actually COMPLETES.
	 *
	 * Why the SUCCESS path and not the failure path: hctx_fail already dumps
	 * the full OSTLSDiagnostics across five "FAIL diag" lines, all of which
	 * survive the crash-only gate because they contain "FAIL". So a broken
	 * network is already legible. The success path is the blind spot -- the
	 * "https: done ..." line above matches NO whitelist keyword, so today a
	 * working fetch and a fetch that never started produce an IDENTICAL
	 * (empty) log. That is what this closes, and it is why the ABSENCE of
	 * this line is itself a reading: nothing ever completed.
	 *
	 * Read it as: snd/rcv byte counts non-zero and nod= modest -> OT is
	 * genuinely moving bytes on this host OS. flow= climbing -> the
	 * kOTFlowErr / T_GODATA async notifier path is exercised and works.
	 * zero= climbing with rcv=0 -> OT took the endpoint but isn't moving data.
	 *
	 * host= is LAST on purpose: macsurf_debug_log_writef truncates SILENTLY
	 * at 255 bytes, and a truncated counter reads as a real value, so the
	 * counters must all precede anything variable-length. */
	if (!g_recon_ot_done && c->conn != NULL) {
		OSTLSDiagnostics od;
		g_recon_ot_done = 1;
		memset(&od, 0, sizeof od);
		OSTLS_GetDiagnostics(c->conn, &od);
		macsurf_debug_log_writef(
			"RECON OT done osx=%d ot_err=%ld snd=%ld/%ld zero=%ld flow=%ld "
			"rcv=%ld/%ld nod=%ld pumps=%ld suite=%d host=%s",
			macsurf_os_is_osx(),
			(long)od.ot_err,
			(long)od.ot_send_calls, (long)od.ot_send_bytes,
			(long)od.ot_send_zero,  (long)od.ot_send_flow,
			(long)od.ot_recv_calls, (long)od.ot_recv_bytes,
			(long)od.ot_recv_nodata,
			(long)od.pump_calls,
			(int)od.cipher_suite,
			c->host[0] ? c->host : "(unset)");
		macsurf_debug_log_flush();
	}
	macsurf_profile_stamp("fetch-finished");
	/* fixes369 (#167) — page-weight accounting: fold this completed
	 * sub-resource's body size + 1 into the per-load totals. */
	macsurf_profile_add_bytes(c->body_bytes);
	macsurf_profile_count_resource();

	/* fixes987 — the body is already on disk; commit patches in its
	 * length, which is what makes the file readable at all. */
	if (c->cache_stream != 0) {
		macos9_cache_stream_end(c->cache_stream, 1);
		c->cache_stream = 0;
	}

	c->state = HS_DONE;
	msg.type = FETCH_FINISHED;
	fetch_send_callback(&msg, c->parent);

	/* fixes244 — mark host as "ever-succeeded" so future timeouts on
	 * this host won't add it to the dead-host blocklist. */
	if (c->pool_key[0] != '\0' && c->status >= 200 && c->status < 400) {
		success_host_add(c->pool_key);
	}

	/* fixes231 — return the OSTLSConnection to the pool while it's
	 * still known idle. Eligibility: keep_alive_ok still set (server
	 * didn't say "Connection: close"), not aborted, connection is
	 * present and state is still Open. Setting c->conn = NULL after
	 * pool-return makes hctx_clear's Close+Dispose path a no-op. */
	if (c->keep_alive_ok && !c->aborted &&
	    c->conn != NULL && c->pool_key[0] != '\0' &&
	    OSTLS_GetState(c->conn) == kOSTLSStateOpen) {
		https_pool_return(c->pool_key, c->conn);
		c->conn = NULL;
	}

	p = c->parent;
	c->parent = NULL; /* fixes447: null before OT teardown */
	hctx_clear(c);
	fetch_remove_from_queues(p);
	fetch_free(p);
}

static char *find_line(char **buf, long *len)
{
	char *p = *buf;
	long  n = *len;
	long  i;
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

/* Parse the accumulated header block. Returns 1 if headers were
 * fully parsed (\r\n\r\n found), 0 if we need more bytes. On parse
 * the function emits FETCH_HEADER callbacks, sets status / mime /
 * content_length / chunked, and on 3xx with Location: also emits
 * FETCH_REDIRECT and self-finishes. */
static int parse_headers(struct macos9_https_ctx *c, long *body_off)
{
	char *sep;
	char *p, *cur;
	long  cur_len;
	fetch_msg msg;

	if (c->hdr_len < 4) return 0;
	sep = NULL;
	{
		long i;
		for (i = 0; i + 3 < c->hdr_len; i++) {
			if (c->hdr_buf[i] == '\r' && c->hdr_buf[i+1] == '\n' &&
			    c->hdr_buf[i+2] == '\r' && c->hdr_buf[i+3] == '\n') {
				sep = c->hdr_buf + i;
				break;
			}
		}
	}
	if (!sep) return 0;

	/* fixes641 (#193): do NOT NUL the '\r' at sep. sep points at the '\r'
	 * that ENDS the last header line (the first '\r' of the terminating
	 * \r\n\r\n). find_line is length-bounded by cur_len below (it does not
	 * need a NUL terminator) and NULs each line's own '\r' as it emits it.
	 * The old `*sep = 0` clobbered the final header's '\r' so find_line
	 * could never match its \r\n -> the LAST header line was silently
	 * dropped. When that last header is Set-Cookie (login 302), the session
	 * cookie was lost and logins never stuck. body_off uses sep+4 pointer
	 * math and is unaffected. */
	cur = c->hdr_buf;
	cur_len = (long)(sep - c->hdr_buf) + 2;
	*body_off = (long)((sep + 4) - c->hdr_buf);

	p = find_line(&cur, &cur_len);
	if (p && strncmp(p, "HTTP/", 5) == 0) {
		char *sp = strchr(p, ' ');
		if (sp) c->status = atoi(sp + 1);
		msg.type = FETCH_HEADER;
		msg.data.header_or_data.buf = (const uint8_t*)p;
		msg.data.header_or_data.len = strlen(p);
		fetch_send_callback(&msg, c->parent);
	}
	/* fixes836 (#167 M1 diag) — Facebook response status line, WORK-gated.
	 * A login POST that re-shows the form (status 200) vs a real success
	 * (302 -> Set-Cookie c_user/xs) is the whole diagnosis for "wrong
	 * password". */
	if (host_is_fb_asset(c->host)) {
		macsurf_debug_log_writef("WORK fbresp status=%d host=%s%s",
			c->status, c->host, c->path);
	}
	fetch_set_http_code(c->parent, c->status);
	macsurf_debug_log_writef("https: status=%d mime='%s' clen=%ld chunked=%d",
		c->status, c->mime, c->content_length, c->chunked);

	/* fixes313b — defer header forwarding so we can override Content-Type
	 * when Content-Disposition: attachment is present. NetSurf's
	 * llcache_handle_get_header returns the FIRST matching header, so
	 * inserting our synthetic Content-Type AFTER the server's wouldn't
	 * stick. Collect the lines, parse them locally, then replay with
	 * substitution. Cap at 64 lines (typical response is 10–20). */
	{
		char *header_lines[64];
		int   n_header_lines = 0;
		int   force_download = 0;
		int   force_html = 0;   /* fixes770 (#232) */
		int   i;
		static const char forced_ct[] =
			"Content-Type: application/octet-stream";
		static const char forced_html_ct[] =
			"Content-Type: text/html; charset=utf-8";

		while ((p = find_line(&cur, &cur_len)) != NULL) {
			if (p[0] == 0) break;
			if (n_header_lines < 64) {
				header_lines[n_header_lines++] = p;
			}
			/* fixes981 — freshness/validator headers, kept for the
			 * disk copy. Cheap enough to run for every response;
			 * only used if this one turns out cacheable. */
			macos9_cache_capture_hdr(p, c->cache_hdrs,
					sizeof c->cache_hdrs);
			if (strncasecmp(p, "Content-Type:", 13) == 0) {
				char *v = p + 13; while (*v == ' ') v++;
				strncpy(c->mime, v, 127); c->mime[127] = 0;
			}
			if (strncasecmp(p, "Content-Length:", 15) == 0) {
				char *v = p + 15; while (*v == ' ') v++;
				c->content_length = atol(v);
			}
			if (strncasecmp(p, "Transfer-Encoding:", 18) == 0) {
				char *v = p + 18; while (*v == ' ') v++;
				if (strncasecmp(v, "chunked", 7) == 0) c->chunked = 1;
			}
			/* fixes965 — Content-Encoding: gzip.
			 *
			 * Distinct from Transfer-Encoding above and layered
			 * INSIDE it: a response can be both chunked and gzip,
			 * in which case chunk framing is removed first and the
			 * de-chunked bytes are what the decoder sees. feed_body
			 * already runs in that order, so routing the decoder at
			 * hctx_body_out (after chunk decode, before delivery)
			 * gets both cases right with one seam.
			 *
			 * Only gzip. "deflate" is not accepted: servers
			 * disagree about whether it means raw DEFLATE or zlib
			 * -wrapped, and guessing wrong renders binary garbage.
			 * We do not advertise it, so a compliant server will
			 * not send it. */
			/* fixes965b — never attach a decoder to a response that
			 * cannot carry a body. A 304 or 204 legitimately sends
			 * Content-Encoding with zero bytes after it, and a 3xx
			 * usually sends an empty (or tiny) body it does not
			 * expect anyone to read. */
			if (c->gz == NULL &&
			    c->status != 204 && c->status != 304 &&
			    !(c->status >= 300 && c->status <= 399) &&
			    strncasecmp(p, "Content-Encoding:", 17) == 0) {
				char *v = p + 17; while (*v == ' ') v++;
				if (strncasecmp(v, "gzip", 4) == 0 ||
				    strncasecmp(v, "x-gzip", 6) == 0) {
					c->gz = macos9_gunzip_create(
						hctx_gz_emit, c);
					if (c->gz == NULL) {
						macsurf_debug_log_writef(
							"https: FAIL gzip alloc host=%s",
							c->host[0] ? c->host : "(unset)");
					} else {
						macsurf_debug_log_writef(
							"LIFE gzip host=%s", c->host);
					}
				}
			}
			/* fixes231 — disable pool when server says close. */
			if (strncasecmp(p, "Connection:", 11) == 0) {
				char *v = p + 11; while (*v == ' ') v++;
				if (strncasecmp(v, "close", 5) == 0) c->keep_alive_ok = 0;
			}
			if (strncasecmp(p, "Location:", 9) == 0) {
				char *v = p + 9; size_t lv;
				while (*v == ' ' || *v == '\t') v++;
				lv = strlen(v);
				if (lv >= sizeof(c->redirect_url)) lv = sizeof(c->redirect_url) - 1;
				memcpy(c->redirect_url, v, lv);
				c->redirect_url[lv] = 0;
				/* fixes836 (#167 M1 diag) — where FB is sending us
				 * after the login POST (checkpoint / 2FA / home). */
				if (host_is_fb_asset(c->host)) {
					macsurf_debug_log_writef(
						"WORK fbloc %s", c->redirect_url);
				}
			}
			/* fixes313b (#150) — Content-Disposition: attachment forces
			 * download regardless of Content-Type. Servers commonly serve
			 * downloads with Content-Type: text/html (CMS default) and
			 * mark them as attachments only via this header. Detect with
			 * a case-insensitive prefix check on the value. */
			if (strncasecmp(p, "Content-Disposition:", 20) == 0) {
				char *v = p + 20;
				while (*v == ' ' || *v == '\t') v++;
				if (strncasecmp(v, "attachment", 10) == 0) {
					force_download = 1;
				}
			}
			/* fixes367 (#167) — cookie jar: store Set-Cookie. Handled
			 * here in the header loop (not after) so a login POST's
			 * 302 carries its c_user/xs cookies into urldb BEFORE the
			 * FETCH_REDIRECT below tears the fetch down — the very next
			 * GET (the redirect target) then sends them back. One call
			 * per Set-Cookie line; Facebook emits several. Mirrors
			 * curl.c: fetch_set_cookie → urldb_set_cookie(value, url). */
			if (strncasecmp(p, "Set-Cookie:", 11) == 0) {
				char *v = p + 11;
				while (*v == ' ' || *v == '\t') v++;
				/* fixes378 (#167) — refuse Facebook's 'noscript=1'
				 * marker. We HAVE JavaScript (QuickJS + the fixes377
				 * fills), so we never want FB to think otherwise:
				 * storing noscript=1 makes every later request announce
				 * "no JS", locking us onto FB's no-JS surface where the
				 * 2FA/checkpoint step is the "not available on this
				 * device" dead end. Drop it on the floor. */
				if (strncasecmp(v, "noscript=", 9) == 0) {
					macsurf_debug_log_writef(
						"https: refused 'noscript' Set-Cookie "
						"(we have JS) for %s", c->host);
				} else {
					fetch_set_cookie(c->parent, v);
					/* fixes658 (#193) login cache-staleness fix. A login is a POST
					 * whose response sets session cookies then 303/302-redirects to a
					 * page we very likely CACHED from BEFORE login (the logged-OUT
					 * copy). Serving that stale copy makes a good login look
					 * failed/errored. So when a POST response stores a Set-Cookie,
					 * force the next main-document fetch (the redirect target) to
					 * bypass cache and refetch fresh WITH the new session cookies.
					 * Static sub-resources still serve from cache (one-shot, clears
					 * on the next cache store). GET responses setting analytics
					 * cookies do NOT trip this (guarded on post_body). */
					if (c->post_body != NULL) {
						extern int macsurf_http_skip_next_cache;
						/* fixes750 (#213): purge cached bodies ONCE per
						 * login (on the skip 0->1 edge) so a page cached
						 * logged-OUT isn't re-served on a later visit.
						 * deadhosts.txt is preserved. */
						if (macsurf_http_skip_next_cache == 0) {
							extern long macos9_cache_clear_bodies(void);
							macos9_cache_clear_bodies();
						}
						macsurf_http_skip_next_cache = 1;
						macsurf_debug_log_writef(
							"https: POST set-cookie -> skip stale "
							"cache (login) host=%s", c->host);
					}
					/* fixes367 (#167) — log the cookie NAME only (up
					 * to '='), never the value, so the hardware
					 * bring-up can confirm c_user/xs land without
					 * leaking the secret. */
					{
						char nm[40];
						int k = 0;
						while (v[k] != '\0' && v[k] != '=' &&
						       k < 39) {
							nm[k] = v[k];
							k++;
						}
						nm[k] = '\0';
						macsurf_debug_log_writef(
							"https: stored cookie '%s' for %s",
							nm, c->host);
						/* fixes836 (#167 M1 diag) — WORK-gated
						 * copy for Facebook so the login Set-Cookie
						 * (c_user/xs = success) survives the log
						 * gate. Name only, never the value. */
						if (host_is_fb_asset(c->host)) {
							macsurf_debug_log_writef(
								"WORK fbsc %s %s", nm, c->host);
						}
					}
				}
			}
		}

		if (force_download) {
			strcpy(c->mime, "application/octet-stream");
			macsurf_debug_log_writef(
				"https: Content-Disposition attachment "
				"→ force download (mime override)");
		}

		/* fixes768 (#232) — RECON: log the resolved mime + attachment
		 * flag for EVERY response so the failures-only gate still shows
		 * why a page downloads. mime=(empty) => Content-Type never
		 * parsed (no handler => download); mime=application/octet-stream
		 * with cd=1 => Content-Disposition forced it. */
		/* fixes768/770 (#232) — log the resolved mime only for the
		 * INTERESTING cases (an error status, an empty/parse-failed
		 * mime, or a forced download), so we can see WHY a page
		 * downloads without spamming a synced log line per image. */
		if (force_download || c->status >= 400 || c->mime[0] == 0) {
			macsurf_debug_log_writef(
				"RECON MIME net host=%s path=%s mime=%s cd=%d st=%d",
				c->host, c->path,
				c->mime[0] ? c->mime : "(empty)",
				force_download, c->status);
		}

		/* fixes770 (#232) — MacSurf has no standalone text/plain content
		 * handler (misc_stub.c stubs textplain_init), so a text/plain
		 * response has no handler and NetSurf routes it to the
		 * DOWNLOADER: an HN 429 "rate limited" page, robots.txt, or a
		 * .txt file all dump to disk instead of displaying. Browsers
		 * show text/plain INLINE. Until the real textplain.c handler is
		 * ported (#233), relabel text/plain as text/html so it renders
		 * through the HTML pipeline. Prose / error pages read fine;
		 * embedded markup is imperfect (unescaped) — acceptable stopgap.
		 * Skipped when a real attachment already forced a download. */
		if (!force_download &&
		    strncasecmp(c->mime, "text/plain", 10) == 0) {
			strcpy(c->mime, "text/html");
			force_html = 1;
			macsurf_debug_log_writef(
				"RECON MIME text/plain->text/html (no textplain "
				"handler) host=%s path=%s st=%d",
				c->host, c->path, c->status);
		}

		/* fixes979 — 304 Not Modified.
		 *
		 * Until now NEITHER fetcher had this branch, and because of
		 * that macos9_capture_extra_headers deliberately STRIPPED
		 * If-None-Match / If-Modified-Since ("we can't answer 304, so
		 * we must not ask"). The consequence is the expensive half: an
		 * llcache object that needs freshness validation could only be
		 * revalidated by refetching the WHOLE body over TLS, on a
		 * 400 MHz machine, even when the bytes on disk were still good.
		 * With the branch here the conditional headers go back, and a
		 * revalidation costs a request and an empty response.
		 *
		 * Mirrors curl.c exactly (fetch_curl_process_headers): send
		 * FETCH_NOTMODIFIED and stop, WITHOUT forwarding the response
		 * headers -- llcache_fetch_notmodified moves the users onto the
		 * candidate object and refreshes its cache data itself, so a
		 * header replay here would apply to the wrong object. GET only,
		 * same guard as curl: a 304 answering a POST is malformed, and
		 * treating it as "your cached copy is fine" would serve the
		 * wrong thing for a form submission.
		 *
		 * Cookies from the 304 have already been captured by the parse
		 * loop above -- that is why this sits after it rather than
		 * beside the status line. */
		if (c->status == 304 && c->post_body == NULL) {
			struct fetch *parent_save;
			/* fixes979b — "LIFE " prefix, or the failures-only gate
			 * drops it: the gate matches literal strings, and a
			 * line that never reaches disk is worse than no line,
			 * because it reads like instrumentation that exists. */
			macsurf_debug_log_writef(
				"LIFE 304 not-modified host=%s path=%s",
				c->host, c->path);
			msg.type = FETCH_NOTMODIFIED;
			fetch_send_callback(&msg, c->parent);
			parent_save = c->parent;
			hctx_clear(c);
			fetch_remove_from_queues(parent_save);
			fetch_free(parent_save);
			return 2;	/* terminal */
		}

		/* Now forward all headers, substituting Content-Type when
		 * force_download is true. */
		for (i = 0; i < n_header_lines; i++) {
			const char *line = header_lines[i];
			if (force_download &&
			    strncasecmp(line, "Content-Type:", 13) == 0) {
				line = forced_ct;
			} else if (force_html &&
			    strncasecmp(line, "Content-Type:", 13) == 0) {
				line = forced_html_ct;   /* fixes770 (#232) */
			}
			msg.type = FETCH_HEADER;
			msg.data.header_or_data.buf = (const uint8_t *)line;
			msg.data.header_or_data.len = strlen(line);
			fetch_send_callback(&msg, c->parent);
		}
	}

	if (c->status >= 300 && c->status < 400 && c->redirect_url[0] != 0) {
		struct fetch *parent_save;
		msg.type = FETCH_REDIRECT;
		msg.data.redirect = c->redirect_url;
		fetch_send_callback(&msg, c->parent);
		/* fixes368a (#167) — log the redirect TARGET, not just "redirect".
		 * The Facebook login chain is GET → POST → 302 → save-device →
		 * home; seeing each hop's destination is how we troubleshoot a
		 * login that stalls or loops. */
		macsurf_debug_log_writef("https: %d redirect -> %s",
			c->status, c->redirect_url);
		parent_save = c->parent;
		hctx_clear(c);
		fetch_remove_from_queues(parent_save);
		fetch_free(parent_save);
		return 2;	/* terminal */
	}

	/* fixes218 — cache eligibility decided once mime has been parsed.
	 * fixes312 (#144) — POST responses are not cacheable: the URL alone
	 * doesn't identify the response (different bodies → different
	 * results), so caching would serve stale or wrong data on subsequent
	 * GETs for the same URL.
	 * fixes374 (#167) — NEVER cache Facebook. FB serves login/checkpoint
	 * pages with Cache-Control: private,no-cache,no-store; caching them
	 * served STALE pages with dead lsd/jazoest CSRF tokens AND meant the
	 * fresh Set-Cookie (datr/c_user/xs, device-trust) was never seen —
	 * so the login looped and 2FA was demanded every time. Always go to
	 * network for FB so tokens are fresh and cookies are captured. */
	if (c->post_body == NULL &&
	    !host_is_fb_asset(c->host) &&
	    macos9_cache_mime_eligible(c->status, c->mime)) {
		c->cache_eligible = 1;
		/* fixes987 — open the cache file now, so the body can be
		 * written through as it arrives. */
		if (c->url != NULL) {
			const char *cu_ = nsurl_access(c->url);
			if (cu_ != NULL) {
				c->cache_stream = macos9_cache_stream_begin(
					cu_, c->status, c->mime, c->cache_hdrs);
			}
		}
	}

	if (c->chunked) OSTLS_HTTP_ChunkDecoderInit(&c->chunk);

	/* fixes644 (#198): a response with NO Content-Length and NOT chunked is
	 * connection-close-delimited — the ONLY way to know the body ended is the
	 * server closing the connection. If we leave keep_alive_ok set, our pool
	 * logic waits for more bytes on a socket the server considers done, and
	 * the no-progress watchdog eventually truncates the body (the salvage
	 * path), corrupting a large download. Clearing keep_alive_ok makes the
	 * peer-close path at the bottom terminate the body cleanly. Mirrors the
	 * HTTP fetcher; only affects close-delimited responses, so no regression
	 * to Content-Length / chunked transfers. */
	if (c->content_length < 0 && !c->chunked)
		c->keep_alive_ok = 0;

	return 1;
}

/* fixes965 — the single place body bytes reach NetSurf and the disk cache.
 *
 * Extracted from feed_body's two branches (chunked / raw), which were doing
 * the identical three steps, so that gzip has exactly one seam to sit in.
 * Whatever arrives here is what the page is made of: for a gzip response
 * these are the DECOMPRESSED bytes, which is what makes the cache correct --
 * a cached body must render identically to a fresh one, and storing the
 * compressed form would mean a cache hit replayed gzip bytes to a content
 * handler that never asked for them. Same reasoning as fixes770 relabelling
 * text/plain before the cache-eligibility check rather than after. */
static void hctx_deliver(struct macos9_https_ctx *c, const char *data, long len)
{
	fetch_msg msg;
	if (len <= 0) return;
	msg.type = FETCH_DATA;
	msg.data.header_or_data.buf = (const uint8_t *)data;
	msg.data.header_or_data.len = (size_t)len;
	fetch_send_callback(&msg, c->parent);
	/* fixes987 — write through instead of accumulating. */
	if (c->cache_stream != 0) {
		if (!macos9_cache_stream_data(c->cache_stream, data, len)) {
			c->cache_stream = 0;   /* already closed + deleted */
		}
	}
}

/* fixes965 — gunzip output callback. */
static void hctx_gz_emit(void *cbctx, const char *data, long len)
{
	hctx_deliver((struct macos9_https_ctx *)cbctx, data, len);
}

/* fixes965 — route body bytes through the decoder when one is active.
 * Returns 0 on success, -1 if the gzip stream is bad (caller fails the fetch).
 *
 * NOTE the accounting split that matters: the CALLER still adds the RAW byte
 * count to c->body_bytes, because body_bytes is compared against
 * content_length, and Content-Length describes the COMPRESSED entity. Adding
 * decompressed lengths there would end the body early or never at all. */
static int hctx_body_out(struct macos9_https_ctx *c, const char *data, long len)
{
	if (len <= 0) return 0;
	if (c->gz == NULL) {
		hctx_deliver(c, data, len);
		return 0;
	}
	if (macos9_gunzip_push(c->gz, data, len) == MACOS9_GUNZIP_ERROR) {
		macsurf_debug_log_writef("https: FAIL gzip decode host=%s: %s",
			c->host[0] ? c->host : "(unset)",
			macos9_gunzip_status(c->gz));
		return -1;
	}
	return 0;
}

/* Pump body bytes — either chunked-decoded or raw. Returns 1 if body
 * is complete, 0 if more bytes expected. */
static int feed_body(struct macos9_https_ctx *c, const char *buf, long n)
{
	fetch_msg msg;
	if (n <= 0) return 0;

	/* fixes368e (#167) — one-shot: on the FIRST body bytes (body_bytes==0)
	 * scan for the HTML <title> and log it, so the trace says WHICH page
	 * came back — "Log in to Facebook" vs "Facebook" (logged in) vs
	 * "Security check" (checkpoint) vs an error — not just its size. The
	 * <title> lives in <head> so it's in the first chunk. Titles aren't
	 * secrets. Bounded scan; safe on chunk-framed bytes (substring search). */
	if (c->body_bytes == 0) {
		long ti;
		for (ti = 0; ti + 6 < n; ti++) {
			if (buf[ti] == '<' &&
			    strncasecmp(buf + ti, "<title", 6) == 0) {
				const char *t = buf + ti + 6;
				const char *end = buf + n;
				char title[160];
				int tp = 0;
				while (t < end && *t != '>') t++;	/* past attrs */
				if (t < end) t++;			/* past '>' */
				while (t < end && *t != '<' &&
				       tp < (int)sizeof(title) - 1) {
					title[tp++] = *t++;
				}
				title[tp] = '\0';
				macsurf_debug_log_writef(
					"https: page title: %s", title);
				break;
			}
		}
	}

	/* fixes320 — robust chunked detection by body framing. Some responses
	 * (observed: 68kmla.org's soft-404 served for /favicon.ico, which
	 * carries a large cookie/header block) send Transfer-Encoding: chunked
	 * that the header scan missed AND no Content-Length. Per RFC 7230
	 * §3.3.3 an HTTP/1.1 keep-alive response with no Content-Length is
	 * REQUIRED to be chunked — there is no other way to delimit it — and we
	 * always send Connection: keep-alive, so the peer never closes. Without
	 * recognizing the framing the fetch stalls until the no-progress
	 * timeout and the raw "<hex>\r\n" chunk-size line leaks into the body.
	 *
	 * Sniff the first body bytes once (body_bytes == 0, nothing delivered
	 * yet): a hex run immediately followed by CRLF (or a ';' chunk-ext)
	 * is chunk framing — switch to the decoder. Safe because no real
	 * no-Content-Length payload begins that way: HTML starts '<', CSS
	 * '@'/'.'/'/', PNG 0x89, JPEG 0xFF, GIF 'G'. */
	if (!c->chunked && c->content_length < 0 && c->body_bytes == 0) {
		long i = 0;
		while (i < n && i < 16) {
			char ch = buf[i];
			if ((ch >= '0' && ch <= '9') ||
			    (ch >= 'a' && ch <= 'f') ||
			    (ch >= 'A' && ch <= 'F')) {
				i++;
			} else {
				break;
			}
		}
		if (i > 0 && i < n &&
		    ((buf[i] == '\r' && i + 1 < n && buf[i+1] == '\n') ||
		     buf[i] == ';')) {
			c->chunked = 1;
			OSTLS_HTTP_ChunkDecoderInit(&c->chunk);
			macsurf_debug_log_writef(
				"https: chunk-framing sniffed (clen<0, TE missed) "
				"— enabling decoder");
		}
	}

	if (c->chunked) {
		const char *in = buf;
		UInt32 in_left = (UInt32)n;
		char decode_out[READ_CHUNK];
		while (in_left > 0 && c->chunk.state != kOSTLSChunkStateDone) {
			UInt32 out_w = 0, in_c = 0;
			OSErr e = OSTLS_HTTP_ChunkDecoderProcess(
				&c->chunk, in, in_left,
				decode_out, sizeof decode_out,
				&out_w, &in_c);
			if (e != kOSTLSAsync_OK) {
				hctx_fail(c, "chunked decode error");
				return 1;
			}
			if (out_w > 0) {
				c->body_bytes += out_w;
				if (hctx_body_out(c, decode_out,
						  (long)out_w) < 0) {
					hctx_fail(c, "https: gzip decode error");
					return 1;
				}
			}
			if (in_c == 0 && out_w == 0) break;	/* would-loop guard */
			in += in_c;
			in_left -= in_c;
		}
		if (c->chunk.state == kOSTLSChunkStateDone) return 1;
		return 0;
	} else {
		long deliver = n;
		if (c->content_length >= 0 &&
		    c->body_bytes + deliver > c->content_length) {
			deliver = c->content_length - c->body_bytes;
		}
		if (deliver > 0) {
			c->body_bytes += deliver;
			if (hctx_body_out(c, buf, deliver) < 0) {
				hctx_fail(c, "https: gzip decode error");
				return 1;
			}
		}
		if (c->content_length >= 0 &&
		    c->body_bytes >= c->content_length) return 1;
		return 0;
	}
}

/* fixes368 (#167) — per-host UA moved to macos9_useragent.h /
 * macos9_fetch.c (macos9_user_agent_for_host) so both fetchers share one
 * source of truth and the override table is extensible. */

/* fixes368c (#167) — extract cookie NAMES (never values) from a urldb
 * cookie string "n1=v1; n2=v2; ..." into a space-separated "n1 n2 ..." for
 * the diagnostic log. Lets the login chain confirm that c_user/xs actually
 * ride along on the post-login requests, not just that some bytes went out.
 * Values are dropped entirely — they are session secrets. */
static void cookie_names_only(const char *src, char *dst, int dstcap)
{
	int dp;
	const char *q;
	if (dstcap <= 0) return;
	dst[0] = '\0';
	if (src == NULL) return;
	dp = 0;
	q = src;
	while (*q != '\0' && dp < dstcap - 1) {
		while (*q == ' ' || *q == ';') q++;	/* skip sep/space */
		if (*q == '\0') break;
		if (dp > 0 && dp < dstcap - 1) dst[dp++] = ' ';
		while (*q != '\0' && *q != '=' && *q != ';' && dp < dstcap - 1)
			dst[dp++] = *q++;
		while (*q != '\0' && *q != ';') q++;	/* skip =value */
	}
	dst[dp] = '\0';
}

/* Append bytes into hdr_buf growing as needed. */
/* fixes231 — build the request line + headers into c->req_buf. Returns
 * 0 on success, -1 if the formatted request didn't fit. Called from both
 * the cold-handshake path (HS_TLSING → HS_SEND_REQ) and the warm-pool
 * path (HS_QUEUED hit → HS_SEND_REQ direct). */
/* fixes373 (#167) — Facebook serves its HTTP/1.1 responses with
 * Content-Length + Connection: keep-alive, but under MacSurf's pooled-
 * connection reuse the Content-Length is not being honored (the fetch logs
 * clen=-1), so each FB sub-resource stalls to the 4s no-progress timeout and
 * its body is discarded — the login page and the login POST's Set-Cookie
 * (c_user/xs) drown in ~30+ timeouts. For Facebook's own hosts we instead
 * request Connection: close: FB closes promptly after each response, the body
 * is close-delimited (kOSTLSEventClosed -> hctx_finish, content_length<0
 * path, verified present), and there is no pooled-reuse framing ambiguity.
 * Costs a fresh TLS handshake per resource, but ONLY for FB; every other
 * origin keeps keep-alive + connection pooling. Covers facebook.com plus the
 * asset/CDN domains the login surface pulls (fbcdn.net, fbsbx.com,
 * cdninstagram.com). */
static int host_is_fb_asset(const char *host)
{
	static const char *const fb_suffixes[] = {
		"facebook.com", "fbcdn.net", "fbsbx.com", "cdninstagram.com"
	};
	size_t hl, n, i, sl;
	if (host == NULL) return 0;
	hl = strlen(host);
	n = sizeof(fb_suffixes) / sizeof(fb_suffixes[0]);
	for (i = 0; i < n; i++) {
		sl = strlen(fb_suffixes[i]);
		if (hl >= sl &&
		    strncasecmp(host + hl - sl, fb_suffixes[i], sl) == 0 &&
		    (hl == sl || host[hl - sl - 1] == '.')) {
			return 1;
		}
	}
	return 0;
}

/* fixes378 (#167) — strip any persisted 'noscript=...' token out of an
 * outgoing Cookie: header value (in place). Neutralises a noscript=1 cookie
 * that an earlier no-JS render already stored, WITHOUT forcing the user to
 * wipe the whole jar (which would also lose datr, the device id). Rebuilds the
 * "n1=v1; n2=v2" string minus the noscript token. Output is never longer than
 * the input, so the in-place strcpy back is safe. */
static void cookie_strip_noscript(char *s)
{
	char out[6144];
	char *p = s;
	int first = 1;
	out[0] = '\0';
	while (*p != '\0') {
		char  *semi = strchr(p, ';');
		size_t toklen = (semi != NULL) ? (size_t)(semi - p) : strlen(p);
		if (strncasecmp(p, "noscript=", 9) != 0) {
			if (!first && strlen(out) + 2 < sizeof out)
				strcat(out, "; ");
			if (strlen(out) + toklen < sizeof out) {
				strncat(out, p, toklen);
				first = 0;
			}
		}
		if (semi == NULL)
			break;
		p = semi + 1;
		while (*p == ' ')
			p++;
	}
	strcpy(s, out);
}

/* fixes836 (#167 M1 diag) — build a REDACTED field map of a urlencoded POST
 * body: "name=VALUELEN" per field. Values (password, email, tokens) are
 * NEVER logged — only each field's name and the byte-length of its value, so
 * the login POST can be verified well-formed (pass present + non-empty, lsd/
 * jazoest present) without leaking secrets. Output bounded to outcap-1. */
static void fb_body_fieldmap(const char *body, long len, char *out, size_t outcap)
{
	long i = 0;
	size_t used = 0;
	if (out == NULL || outcap == 0) return;
	out[0] = '\0';
	if (body == NULL) return;
	while (i < len && used + 2 < outcap) {
		long ns = i, ne, vl = 0;
		char num[24];
		size_t nl, k;
		while (i < len && body[i] != '=' && body[i] != '&') i++;
		ne = i;
		if (i < len && body[i] == '=') {
			i++;
			while (i < len && body[i] != '&') { i++; vl++; }
		}
		if (i < len && body[i] == '&') i++;
		nl = (size_t)(ne - ns);
		if (nl > 24) nl = 24;
		if (used + nl + 2 >= outcap) break;
		memcpy(out + used, body + ns, nl); used += nl;
		out[used++] = '=';
		sprintf(num, "%ld", vl);
		k = strlen(num);
		if (used + k + 2 >= outcap) { out[used] = '\0'; break; }
		memcpy(out + used, num, k); used += k;
		out[used++] = ' ';
		out[used] = '\0';
	}
}

static int build_request(struct macos9_https_ctx *c)
{
	int rn;
	const char *ua = macos9_user_agent_for_host(c->host);
	const char *conn = host_is_fb_asset(c->host) ? "close" : "keep-alive";
	/* fixes367 (#167) — cookie jar: pull the stored cookies for this URL
	 * and emit them as a Cookie: header. urldb_get_cookie returns a
	 * malloc'd "name=val; name2=val2" string (include_http_only=true so
	 * Facebook's HttpOnly session cookies — c_user/xs — are sent; this
	 * matches curl.c's fetcher). NULL when the jar has nothing for this
	 * origin. Secure cookies are returned here because c->url is https. */
	char  cookie_hdr[MACSURF_COOKIE_HDR_CAP];
	char *cookie_str;
	int   verifiable;      /* fixes835 (#167 M1) */
	char  synth[512];      /* fixes835 — Sec-Fetch + Origin, built below */
	cookie_str = (c->url != NULL) ? urldb_get_cookie(c->url, true) : NULL;
	if (cookie_str != NULL) {
		/* fixes378 — drop any stuck noscript=1 before it reaches FB. */
		cookie_strip_noscript(cookie_str);
		/* fixes368c (#167) — names only, never values. */
		{
			char cknames[256];
			cookie_names_only(cookie_str, cknames,
				(int)sizeof cknames);
			macsurf_debug_log_writef("https: cookies sent: %s",
				cknames);
			/* fixes838 (#167 diag) — WORK-gated copy for Facebook so
			 * we can SEE which cookies go out. If 'datr' is present and
			 * the SAME across relaunches, the device persists; if it is
			 * absent / different every session, the jar isn't persisting
			 * (the new-device-every-login bug). Names only, never values. */
			if (host_is_fb_asset(c->host)) {
				macsurf_debug_log_writef("WORK fbcksent %s", cknames);
			}
		}
	}
	macos9_build_cookie_header(cookie_hdr, sizeof cookie_hdr, cookie_str);
	if (cookie_str != NULL) free(cookie_str);
	/* fixes368a (#167) — one request-summary line per fetch: the host, the
	 * User-Agent we chose for it, and the Cookie: header size. The UA is the
	 * key Facebook diagnostic — if FB serves the wrong page or 301-bounces,
	 * this confirms whether the vintage UA or the default went out. Cookie
	 * BYTES only, never the values (c_user/xs are session secrets). Logged
	 * unconditionally so the very first (no-cookie) GET is covered too. */
	macsurf_debug_log_writef("https: REQ %s %s%s ck=%ldB pb=%ldB ua=%s",
		(c->post_body != NULL) ? "POST" : "GET",
		c->host, c->path,
		(long)strlen(cookie_hdr),
		(long)((c->post_body != NULL) ? (long)c->post_body_len : 0L),
		ua);
	/* fixes835 (#167 Facebook M1) — synthesize the Sec-Fetch request
	 * metadata a real browser sends (FB returns HTTP 400 to a modern UA
	 * that omits these), keyed on request kind so the claimed identity is
	 * coherent: a form POST is a same-origin document submit; a top-level
	 * GET navigation (typed/bookmarked) has no initiator origin (Site:
	 * none); a sub-resource (script/css/font/img) is Dest:empty /
	 * Mode:no-cors / Site:same-origin. The POST/GET nav split matches the
	 * proven fixes400 values. A caller that already supplied its own
	 * Sec-Fetch set (future M3 XHR) wins. Ships globally (browser-standard).
	 * This is the https fetcher, so Origin is always https and c->host has
	 * no port (FB is :443). */
	verifiable = fetch_get_verifiable(c->parent);
	macos9_build_sec_fetch(synth, sizeof synth, verifiable,
		(c->post_body != NULL), "https", c->host);
	/* fixes836 (#167 M1 diag) — dump the shape of a Facebook POST (the login
	 * submit) so we can see EXACTLY what goes on the wire when FB says "wrong
	 * password". WORK-prefixed so it survives the failures-only log gate.
	 * fbreq = request shape (cookie bytes, body bytes, Origin/Sec-Fetch/Referer
	 * present?, UA tail); fbbody = redacted field map (names + value LENGTHS,
	 * never the values). Confirms pass is present/non-empty and lsd/jazoest are
	 * there. host_is_fb_asset keeps this to Facebook only. */
	if (c->post_body != NULL && host_is_fb_asset(c->host)) {
		char fmap[200];
		size_t ual = strlen(ua);
		fb_body_fieldmap(c->post_body, (long)c->post_body_len,
			fmap, sizeof fmap);
		macsurf_debug_log_writef(
			"WORK fbreq POST %s%s ckB=%ld pbB=%ld org=%d sf=%d ref=%d uatail=%s",
			c->host, c->path,
			(long)strlen(cookie_hdr), (long)c->post_body_len,
			(int)(synth[0] != '\0'
			      && macos9_hdr_has_ci(synth, "origin:")),
			(int)(synth[0] != '\0'),
			(int)macos9_hdr_has_ci(c->caller_hdrs, "referer:"),
			(ual > 12) ? (ua + ual - 12) : ua);
		macsurf_debug_log_writef("WORK fbbody %s", fmap);
	}
	if (c->post_body != NULL) {
		/* fixes312 (#144) — POST. Body goes out in a second
		 * OSTLS_Write after these headers; req_buf carries
		 * headers only.
		 * fixes367 (#167) — UA chosen per-host (macos9_user_agent_for_host):
		 * facebook.com gets a vintage Mozilla/4.0 Mac string to unlock
		 * the no-JS mbasic page; everything else keeps MacSurf's UA.
		 * fixes721 (#144 file upload) — Content-Type is multipart (with
		 * boundary) for a file upload, else the urlencoded default. */
		const char *ct = (c->post_ctype[0] != '\0')
			? c->post_ctype
			: "application/x-www-form-urlencoded";
		rn = sprintf(c->req_buf,
			"POST %s HTTP/1.1\r\n"
			"Host: %s\r\n"
			"User-Agent: %s\r\n"
			"Accept: text/html,application/xhtml+xml,*/*;q=0.8\r\n"
			"Accept-Language: en-US,en;q=0.5\r\n"
			"Accept-Encoding: gzip\r\n"
			"%s"                        /* cookie_hdr  */
			"%s"                        /* caller_hdrs (fixes835) */
			"%s"                        /* synth       (fixes835) */
			"Content-Type: %s\r\n"
			"Content-Length: %lu\r\n"
			"Connection: keep-alive\r\n"
			"\r\n",
			c->path, c->host, ua, cookie_hdr, c->caller_hdrs, synth, ct,
			(unsigned long)c->post_body_len);
	} else {
		rn = sprintf(c->req_buf,
			"GET %s HTTP/1.1\r\n"
			"Host: %s\r\n"
			"User-Agent: %s\r\n"
			"Accept: text/html,application/xhtml+xml,*/*;q=0.8\r\n"
			"Accept-Language: en-US,en;q=0.5\r\n"
			"Accept-Encoding: gzip\r\n"
			"%s"                    /* cookie_hdr  */
			"%s"                    /* caller_hdrs (fixes835) */
			"%s"                    /* synth       (fixes835) */
			"Connection: %s\r\n"
			"\r\n",
			c->path, c->host, ua, cookie_hdr, c->caller_hdrs, synth, conn);
	}
	if (rn <= 0 || (unsigned long)rn >= sizeof c->req_buf) return -1;
	c->req_len = (unsigned long)rn;
	c->req_sent = 0;
	c->post_body_sent = 0;
	return 0;
}

static int hdr_append(struct macos9_https_ctx *c, const char *buf, long n)
{
	if (c->hdr_len + n > HDR_BUF_MAX) {
		macsurf_debug_log_writef("hdr_append OVERFLOW: hdr_len=%ld n=%ld max=%ld",
			c->hdr_len, n, (long)HDR_BUF_MAX);
		hctx_fail(c, "https: header buffer overflow");
		return -1;
	}
	if (c->hdr_buf == NULL) {
		c->hdr_cap = 1024;
		c->hdr_buf = (char *)malloc(c->hdr_cap);
		if (!c->hdr_buf) { hctx_fail(c, "https: out of memory"); return -1; }
	}
	while (c->hdr_len + n > c->hdr_cap) {
		long new_cap = c->hdr_cap * 2;
		char *nb;
		if (new_cap > HDR_BUF_MAX) new_cap = HDR_BUF_MAX;
		nb = (char *)realloc(c->hdr_buf, new_cap);
		if (!nb) { hctx_fail(c, "https: out of memory"); return -1; }
		c->hdr_buf = nb;
		c->hdr_cap = new_cap;
	}
	memcpy(c->hdr_buf + c->hdr_len, buf, n);
	c->hdr_len += n;
	return 0;
}

/* ---------- per-slot pump ---------- */

static void hctx_poll(struct macos9_https_ctx *c)
{
	OSTLSEvent ev;
	OSErr      e;
	UInt32     written, got;
	char       rd[READ_CHUNK];
	int        loop_count;

	if (c->state == HS_IDLE || c->state == HS_DONE || c->state == HS_FAIL)
		return;

	if (c->aborted) {
		hctx_fail(c, "https: aborted");
		return;
	}

	/* fixes237 — serve from disk cache. Mirrors the HTTP fetcher's
	 * working cache-hit pattern exactly (macos9_http_fetcher.c lines
	 * ~895-925). The fixes218 implementation emitted an extra
	 * "HTTP/1.1 200" header line as the first FETCH_HEADER callback,
	 * which html_create rejected (it expects header callbacks to be
	 * "Name: Value" tuples, not status lines). The working pattern:
	 *
	 *   1. fetch_set_http_code(parent, status)  — sets status internally
	 *   2. ONE FETCH_HEADER with "Content-Type: <mime>" (and nothing else)
	 *   3. ONE FETCH_DATA with the body
	 *   4. hctx_finish handles FETCH_FINISHED + cleanup
	 *
	 * No status line, no extra headers. NetSurf core's hlcache fills in
	 * the rest from the cached MIME + body + status code.
	 */
	if (c->state == HS_CACHEHIT) {
		fetch_msg msg;
		char ct_line[160];
		int rn;

		c->status = c->cache_hit_status;
		fetch_set_http_code(c->parent, c->status);

		/* fixes981 — replay the stored freshness/validator headers
		 * FIRST, so llcache builds this object's cache data exactly as
		 * it would from a network response: it can then decide the copy
		 * is fresh (serve, no request), or stale-but-validatable (one
		 * conditional request, 304, no body). Without them a disk hit
		 * was served unconditionally forever AND could never be
		 * revalidated -- measured as reval=14 cond=0. Each stored line
		 * is CRLF-terminated; FETCH_HEADER wants one line at a time. */
		if (c->cache_hit_hdrs[0] != '\0') {
			char *hp = c->cache_hit_hdrs;
			while (*hp != '\0') {
				char *eol = strstr(hp, "\r\n");
				if (eol == NULL) break;
				*eol = '\0';
				if (hp[0] != '\0') {
					msg.type = FETCH_HEADER;
					msg.data.header_or_data.buf =
						(const uint8_t *)hp;
					msg.data.header_or_data.len = strlen(hp);
					fetch_send_callback(&msg, c->parent);
				}
				hp = eol + 2;
			}
		}

		if (c->cache_hit_mime[0] != 0) {
			rn = sprintf(ct_line, "Content-Type: %s",
				c->cache_hit_mime);
			if (rn > 0) {
				msg.type = FETCH_HEADER;
				msg.data.header_or_data.buf =
					(const uint8_t *)ct_line;
				msg.data.header_or_data.len = (size_t)rn;
				fetch_send_callback(&msg, c->parent);
			}
		}

		if (c->cache_hit_body != NULL && c->cache_hit_len > 0) {
			msg.type = FETCH_DATA;
			msg.data.header_or_data.buf =
				(const uint8_t *)c->cache_hit_body;
			msg.data.header_or_data.len =
				(size_t)c->cache_hit_len;
			fetch_send_callback(&msg, c->parent);
			c->body_bytes = c->cache_hit_len;
		}

		macsurf_debug_log_writef(
			"https: CACHE served body=%ld status=%d",
			c->body_bytes, c->status);

		/* Disable cache-store so hctx_finish doesn't try to re-store
		 * what we just served from disk. */
		c->cache_eligible = 0;
		hctx_finish(c);
		return;
	}

	if (c->state == HS_QUEUED) {
		OSTLSConfig cfg;
		OSTLSConnection *pooled;

		/* fixes232a — wait for NetSurf core to dispatch us via ops.start
		 * before we open any TLS connection. setup() fires for every
		 * queued fetch up-front; start() respects max_fetchers_per_host.
		 * Without this gate, parallel sub-resource page loads cold-
		 * handshake EVERY URL in setup order regardless of per-host
		 * throttle, killing the fixes231 keep-alive pool. */
		if (!c->started) return;

		/* fixes856 (#285) — refuse trackers / ad networks / consent
		 * platforms outright.  Checked FIRST: this host is never going to
		 * contribute a pixel, so there is no reason to pay for a TLS
		 * handshake, the bytes, or the QuickJS parse+exec of a bundle built
		 * for a modern desktop CPU.  On hackaday.com this drops ~908 KB of
		 * 2406 KB (~38%) — gtag/js alone is 475 KB, one request.  Marked
		 * terminal so hctx_fail suppresses the http scheme-fallback and core
		 * never re-queues it: one clean FETCH_ERROR per URL, exactly the
		 * fixes554 contract.  Blocked scripts simply never run, which is what
		 * an ad blocker does everywhere; the page's own JS is unaffected
		 * because these are independent <script> tags.  See
		 * macos9_blocklist.h for the policy (umami/plausible/... allowed) and
		 * for the origins deliberately NOT in the table. */
		if (c->url != NULL && macos9_host_is_tracker(c->host)) {
			macsurf_debug_log_writef(
				"LIFE blocked tracker host=%s", c->host);
			(void)terminal_url_add(nsurl_access(c->url));
			hctx_fail(c, "https: tracker/ad host blocked");
			return;
		}

		/* fixes554 — per-URL terminal fail: a resource URL that already
		 * fast-failed once renders alt text and is NEVER retried.  Checked
		 * BEFORE the per-host dead_host list so it survives dead_hosts FIFO
		 * eviction — honours "do not re-queue" when NetSurf core re-issues
		 * the fetch after FETCH_ERROR.  hctx_fail sees the URL in the
		 * terminal set and suppresses the http scheme-fallback, so this
		 * does not relaunch the storm. */
		if (c->url != NULL && terminal_url_check(nsurl_access(c->url))) {
			macsurf_debug_log_writef(
				"https: terminal-URL FAST-FAIL %s",
				nsurl_access(c->url));
			hctx_fail(c, "https: resource terminally failed");
			return;
		}

		/* fixes236 — short-circuit hosts that already failed this
		 * session. NetSurf core re-issues a fresh fetch after every
		 * FETCH_ERROR; without this check the retry pays the full
		 * NO_PROGRESS_TICKS timeout (4s) for a host that's never going
		 * to work (fonts.googleapis.com fingerprint-blocking is the
		 * canonical case). Fast-fail here saves ~5s per dead host on
		 * retries. */
		if (dead_host_check(c->pool_key)) {
			macsurf_debug_log_writef(
				"https: dead-host FAST-FAIL %s",
				c->pool_key);
			/* fixes554 — mark THIS resource URL terminally failed on
			 * the first dead-host fast-fail: render alt text once, no
			 * http fallback, no 301 follow, no re-queue.  Per-URL, so
			 * the cdn.jsdelivr.net emoji storm (dozens of distinct URLs
			 * each looping fast-fail -> http -> 301 -> fast-fail)
			 * collapses to one FETCH_ERROR per URL.  hctx_fail reads the
			 * terminal set and skips the scheme-fallback below. */
			if (c->url != NULL) {
				const char *uu = nsurl_access(c->url);
				if (terminal_url_add(uu)) {
					macsurf_debug_log_writef(
						"resource: TERMINAL FAIL url=%s", uu);
				}
			}
			hctx_fail(c, "https: host previously failed");
			return;
		}

		/* fixes231 — try the keep-alive pool first. If we get a hit,
		 * skip OSTLS_New + handshake entirely and jump straight to
		 * sending the request on the warm connection. ~700 ms of
		 * ECDHE keygen + cert chain validation saved per pool hit. */
		pooled = https_pool_take(c->pool_key);
		if (pooled != NULL) {
			c->conn = pooled;
			c->from_pool = 1;
			if (build_request(c) < 0) {
				hctx_fail(c, "https: request too large");
				return;
			}
			c->progress_ticks = now_ticks();
			c->state = HS_SEND_REQ;
			MS_LOG("https: pool reuse");
			return;
		}

		memset(&cfg, 0, sizeof cfg);
		cfg.host = c->host;
		cfg.port = c->port;
		cfg.server_name = c->host;
		e = OSTLS_New(&c->conn, &cfg);
		if (e != kOSTLSAsync_OK || c->conn == NULL) {
			hctx_fail(c, "https: OSTLS_New failed");
			return;
		}
		e = OSTLS_Start(c->conn);
		if (e != kOSTLSAsync_OK) {
			hctx_fail(c, "https: OSTLS_Start failed");
			return;
		}
		c->progress_ticks = now_ticks();
		c->state = HS_TLSING;
		MS_LOG("https: started");
		macsurf_profile_stamp("tls-handshake-start");
		return;
	}

	/* Pump up to PUMP_STEPS atomic steps per poll tick (fixes234). */
	ev = kOSTLSEventNone;
	{
		/* fixes640 — accumulate the CPU inside the TLS engine (excludes
		 * WaitNextEvent idle between polls). Attribute by pre-pump state:
		 * handshake crypto (X25519/ECDHE/verify — the G3's real cost) ->
		 * tls; record decrypt during transfer -> net. */
		extern double macos9_micros(void);
		extern void macsurf_profile_accum_tls(long us);
		extern void macsurf_profile_accum_net(long us);
		double t_pump = macos9_micros();
		int was_handshaking = (c->state == HS_TLSING);
		long pump_us;
		e = OSTLS_Pump(c->conn, PUMP_STEPS, &ev);
		pump_us = (long)(macos9_micros() - t_pump);
		if (was_handshaking)
			macsurf_profile_accum_tls(pump_us);
		else
			macsurf_profile_accum_net(pump_us);
	}
	if (e != kOSTLSAsync_OK) {
		hctx_fail(c, "https: pump error");
		return;
	}

	/* fixes414 — credit RAW WIRE progress to the no-progress watchdog.
	 * Previously c->progress_ticks was refreshed only when OSTLS_Read handed
	 * the fetcher a header/body byte (below), so a connection that was
	 * actively receiving encrypted bytes -- a slow transfer, a handshake
	 * under contention, or a reused keep-alive still waiting on the response
	 * -- hit NO_PROGRESS_TICKS (4s) and was killed, then retried or HTTP-
	 * fallback re-fetched. Measured: ~23 such kills + 40 fallbacks = ~90s of
	 * dead time on one load (the "really struggling" symptom). Refresh
	 * whenever OT has delivered new bytes into macTLS, so the 4s timer means
	 * "no RAW progress for 4s"; a genuinely dead/parked server (e.g. a
	 * fingerprint-reject that ACKs then sends nothing) still times out. The
	 * recv-byte counter is cumulative across a pooled connection's life, so
	 * the first poll after a pool reuse sees a jump and grants one fresh
	 * window -- correct for a reused conn. */
	if (c->conn != NULL) {
		OSTLSDiagnostics pdiag;
		memset(&pdiag, 0, sizeof pdiag);
		OSTLS_GetDiagnostics(c->conn, &pdiag);
		if (pdiag.ot_recv_bytes != c->last_rx_bytes) {
			c->last_rx_bytes = pdiag.ot_recv_bytes;
			c->progress_ticks = now_ticks();
		}
		/* fixes735 — throttled in-flight RX progress so a slow/stuck fetch is
		 * VISIBLE. The perf filter drops the per-poll fetch lines, leaving the
		 * multi-second "black holes" (the 47s/135s gaps on 68kmla); the RECON
		 * prefix survives the filter. Emitted ~every 2s while a request is in
		 * flight. Read it as: body=/raw= climbing → progressing; body=/raw=
		 * frozen while nodata= and idle= climb → STUCK on that resource. */
		{
			unsigned long nowt = now_ticks();
			if ((c->state == HS_TLSING || c->state == HS_SEND_REQ ||
			     c->state == HS_HEADERS || c->state == HS_BODY) &&
			    (c->rxlog_ticks == 0 || nowt - c->rxlog_ticks >= 120)) {
				c->rxlog_ticks = nowt;
				macsurf_debug_log_writef(
					"RECON RX host=%s st=%d body=%ld/%ld raw=%ld nodata=%ld idle=%ldms",
					c->host[0] ? c->host : "(unset)",
					(int)c->state, (long)c->body_bytes,
					(long)c->content_length,
					(long)pdiag.ot_recv_bytes,
					(long)pdiag.ot_recv_nodata,
					(long)((nowt - c->progress_ticks) * 1000UL / 60UL));
			}
		}
	}

	if (ev == kOSTLSEventFailed) {
		/* fixes962 — hoisted out of the logging block below so the
		 * no-retry check further down can reuse the same read instead of
		 * querying a connection the retry path is about to dispose. */
		OSTLSDiagnostics fd;
		memset(&fd, 0, sizeof fd);
		/* fixes701 (#206) — surface the exact macTLS failure so a hardware
		 * log pinpoints why a P-384/HRR handshake dies. br_err is the tls13
		 * hs->error; hs_fail=TLS13_FAIL_* names the step (6=ECDH math,
		 * 5=group mismatch, 7=keygen, 3=no key_share, ...); hs_state is the
		 * tls13 handshake state. "TLS-FAIL" survives the crash-only gate. */
		if (c->conn != NULL) {
			OSTLS_GetDiagnostics(c->conn, &fd);
			macsurf_debug_log_writef(
				"https: TLS-FAIL host=%s br_err=%d hs_fail=%d hs_state=%d "
				"os_err=%d ot_err=%ld br_state=%ld suite=%d",
				c->host[0] ? c->host : "(unset)",
				(int)fd.br_err, (int)fd.hs_fail_site, (int)fd.hs_state,
				(int)fd.os_err, (long)fd.ot_err,
				(long)fd.br_state_last,
				(int)fd.cipher_suite);
		}
		/* fixes962 — NEVER retry a certificate rejection or a peer
		 * authentication failure, and go straight to hctx_fail while
		 * br_err is still readable.
		 *
		 * Two separate bugs, one fix. First: hctx_reset_for_retry
		 * disposes the connection that HOLDS br_err, so attempt 2 begins
		 * with br_err = 0. If hctx_fail is then reached by any other
		 * route -- the watchdog, an OSTLS_New failure, a peer reset --
		 * cert_rejected() sees 0, the cleartext fallback runs, and there
		 * is no "cert INVALID" line to show it happened. The security
		 * decision was being thrown away by a retry meant for flaky
		 * CDNs. Second: a rejected certificate is deterministic, so the
		 * retry could never have helped; on a Mac with a wrong clock,
		 * where every host fails this way, it cost three full ECDHE
		 * handshakes per URL instead of one.
		 *
		 * fd is already populated above for the diagnostic dump. */
		if (cert_rejected(c, &fd) || peer_auth_failed(c, &fd)) {
			macsurf_debug_log_writef(
				"https: no retry, peer INVALID br_err=%d(%s) host=%s",
				(int)fd.br_err, ostls_br_err_name((int)fd.br_err),
				c->host[0] ? c->host : "(unset)");
			hctx_fail(c, "https: handshake/transport failed");
			return;
		}
		/* fixes228 — retry once on early-stage handshake failure.
		 * CF / Google CDN sometimes drop the first connection but
		 * accept the second cleanly. Only retry if no app data yet. */
		if (c->retries < HTTPS_MAX_RETRIES &&
		    (c->state == HS_TLSING || c->state == HS_STARTING ||
		     c->state == HS_QUEUED)) {
			c->retries++;
			macsurf_debug_log_writef(
				"https: RETRY %d after Failed state=%d host=%s",
				c->retries, c->state,
				c->host[0] ? c->host : "(unset)");
			hctx_reset_for_retry(c);
			return;
		}
		hctx_fail(c, "https: handshake/transport failed");
		return;
	}
	/* fixes230 — close-retry decision DEFERRED to after the Read block.
	 * Previously this fired here, before OSTLS_Read got a chance to drain
	 * decrypted bytes BearSSL was already holding. nginx for small
	 * responses (404 / 304 / redirects / short bodies) sends the full
	 * response AND close_notify in one batch; BearSSL decrypts both,
	 * pump reports kOSTLSEventClosed, the old code retried and threw
	 * the response away. status stayed 0, retries exhausted, FETCH_ERROR
	 * → about:fetcherror. The new post-read check below only retries if
	 * Read truly produced nothing (state still pre-body, status still 0). */

	if (c->state == HS_TLSING) {
		if (ev == kOSTLSEventHandshakeDone ||
		    OSTLS_GetState(c->conn) == kOSTLSStateOpen) {
			/* fixes735 — was "https: handshake done…", which the perf
			 * filter drops (it suppresses every "https: handshake*" line),
			 * so TLS resumption status was invisible. RECON survives the
			 * filter; resumed=1 means the abbreviated (session-ticket)
			 * handshake fired, resumed=0 a full one. */
			macsurf_debug_log_writef(
				"RECON TLS host=%s resumed=%d cipher=%d",
				c->host, OSTLS_GetResumed(c->conn),
				(int)OSTLS_GetCipherSuite(c->conn));
			if (build_request(c) < 0) {
				hctx_fail(c, "https: request too large");
				return;
			}
			c->state = HS_SEND_REQ;
			c->progress_ticks = now_ticks();
			MS_LOG("https: handshake done");
			macsurf_profile_stamp("tls-handshake-done");
		}
	}

	if (c->state == HS_SEND_REQ) {
		if (c->req_sent < c->req_len) {
			written = 0;
			e = OSTLS_Write(c->conn,
				c->req_buf + c->req_sent,
				(UInt32)(c->req_len - c->req_sent),
				&written);
			if (e != kOSTLSAsync_OK) {
				/* fixes461: stale pool connection — retry cold.
				 * Server closed the idle connection while it sat
				 * in our pool; OSTLS_Write fails immediately.
				 * Dispose the dead conn and open a fresh one
				 * rather than erroring to the user. */
				if (c->from_pool) {
					macsurf_debug_log_writef(
						"https: pool stale write-fail, retry cold host=%s",
						c->host);
					OSTLS_Close(c->conn);
					OSTLS_Dispose(c->conn);
					c->conn = NULL;
					c->from_pool = 0;
					hctx_reset_for_retry(c);
					return;
				}
				hctx_fail(c, "https: write failed");
				return;
			}
			if (written > 0) {
				c->req_sent += written;
				c->progress_ticks = now_ticks();
			} else if (OSTLS_WantWrite(c->conn)) {
				/* fixes686 (Path B) — OT send buffer is full
				 * (kOTFlowErr / nf_want_write set). The notifier
				 * will receive T_GODATA when the buffer drains and
				 * will clear the flag; the next hctx_poll call will
				 * retry OSTLS_Write automatically. Nothing to do
				 * here except avoid busy-spinning on req_sent. */
				macsurf_debug_log_writef(
					"https: flow-ctrl WANT_WRITE req_sent=%ld/%ld host=%s",
					(long)c->req_sent, (long)c->req_len, c->host);
			} else {
				/* T_GODATA already cleared the flag this tick —
				 * kick the pump once more to flush any staged TLS
				 * record bytes before the next outer poll. */
				OSTLSEvent xev = kOSTLSEventNone;
				OSTLS_Pump(c->conn, PUMP_STEPS, &xev);
				(void)xev;
			}
		}
		/* fixes312 (#144) — POST body. After the header block is
		 * fully written, stream the captured post_body before
		 * transitioning to HS_HEADERS. */
		if (c->req_sent >= c->req_len &&
		    c->post_body != NULL &&
		    c->post_body_sent < c->post_body_len) {
			written = 0;
			e = OSTLS_Write(c->conn,
				c->post_body + c->post_body_sent,
				c->post_body_len - c->post_body_sent,
				&written);
			if (e != kOSTLSAsync_OK) {
				hctx_fail(c, "https: post body write failed");
				return;
			}
			if (written > 0) {
				c->post_body_sent += written;
				c->progress_ticks = now_ticks();
			}
		}
		if (c->req_sent >= c->req_len &&
		    (c->post_body == NULL ||
		     c->post_body_sent >= c->post_body_len)) {
			c->state = HS_HEADERS;
			MS_LOG("https: request sent");
		}
	}

	loop_count = 0;
	while ((c->state == HS_HEADERS || c->state == HS_BODY) && loop_count < 16) {
		loop_count++;
		got = 0;
		e = OSTLS_Read(c->conn, rd, sizeof rd, &got);
		if (e != kOSTLSAsync_OK) {
			hctx_fail(c, "https: read failed");
			return;
		}
		if (got > 0) {
			c->progress_ticks = now_ticks();

			if (c->state == HS_HEADERS) {
				long body_off = 0;
				int  r;
				if (c->hdr_len == 0 && got >= 4) {
					char tmp[81];
					long show = got < 80 ? got : 80;
					long j;
					for (j = 0; j < show; j++)
						tmp[j] = (rd[j] >= 32 && rd[j] < 127) ? rd[j] : '.';
					tmp[show] = '\0';
					macsurf_debug_log_writef("hdr_first80: [%s]", tmp);
				}
				if (hdr_append(c, rd, (long)got) < 0) return;
				r = parse_headers(c, &body_off);
				if (r == 2) return;	/* redirect terminal */
				if (r == 1) {
					long leftover = c->hdr_len - body_off;
					c->state = HS_BODY;
					if (leftover > 0) {
						if (feed_body(c,
						    c->hdr_buf + body_off,
						    leftover)) {
							hctx_finish(c);
							return;
						}
					}
				}
			} else {
				if (feed_body(c, rd, (long)got)) {
					hctx_finish(c);
					return;
				}
			}
		} else {
			if (ev == kOSTLSEventClosed && c->state == HS_BODY) {
				/* peer closed mid-body — only OK if no content-length
				 * (HTTP/1.0 style). chunked must have seen final 0. */
				if (c->content_length < 0 && !c->chunked) {
					hctx_finish(c);
					return;
				}
				/* fixes243 — salvage partial body. If we got some body
				 * bytes before the peer hung up early, deliver what we
				 * have via hctx_finish instead of routing to
				 * about:fetcherror. NetSurf core renders truncated
				 * HTML gracefully. Disable cache-store so a partial
				 * body doesn't poison the disk cache.
				 *
				 * fixes255 — raise threshold to 512 bytes. Tiny bodies
				 * (e.g. fonts.googleapis.com's ~200-byte JA3-reject
				 * response) aren't useful content; they're failure
				 * signatures. Salvaging them hides the failure from
				 * the dead-host blocklist and lets the bad host stay
				 * in the keep-alive pool forever, retrying with the
				 * same fingerprint that already failed.
				 *
				 * Also clear keep_alive_ok so the just-stalled
				 * connection doesn't get pooled. */
				if (c->body_bytes >= 512) {
					macsurf_debug_log_writef(
						"https: peer-close SALVAGE body=%ld of clen=%ld chunked=%d",
						c->body_bytes,
						c->content_length, (int)c->chunked);
					c->cache_eligible = 0;
					c->keep_alive_ok = 0;
					hctx_finish(c);
					return;
				}
				hctx_fail(c, "https: truncated body");
				return;
			}
			/* got == 0 and no close event -> break loop */
			break;
		}
	}

	/* fixes230 — close-retry, deferred from before-pump. After the Read
	 * block has had a chance to consume pending decrypted bytes, we can
	 * decide cleanly whether this close was "peer closed without sending
	 * a response" (retry) or "peer closed mid-body" (handled inside the
	 * Read block above). State == HS_BODY means parse_headers succeeded
	 * this tick or earlier — that path is owned by the in-block check.
	 * Any other not-yet-terminal state is genuine pre-body close. */
	if (ev == kOSTLSEventClosed &&
	    c->state != HS_BODY &&
	    c->state != HS_DONE &&
	    c->state != HS_FAIL) {
		if (c->retries < HTTPS_MAX_RETRIES) {
			c->retries++;
			macsurf_debug_log_writef(
				"https: RETRY %d after Closed state=%d host=%s",
				c->retries, c->state,
				c->host[0] ? c->host : "(unset)");
			hctx_reset_for_retry(c);
			return;
		}
		hctx_fail(c, "https: peer closed before complete");
		return;
	}

	/* fixes548 — credit event-loop-blocked gaps so they don't count as
	 * connection no-progress.  At cold startup the first WaitNextEvent
	 * blocks on TSM init, so this fetcher isn't polled for seconds; the
	 * wall-clock watchdog below would then trip on the very next poll
	 * (observed: first-open fetcherror, pumps=1, ot_send=0, ot_err=INT_MAX).
	 * On the first drive, or after a >30-tick (0.5s) gap since the last
	 * poll, restart the no-progress window from this poll.  Healthy polling
	 * is ~1 tick/poll, so a large gap is loop-blocked time, not a silent
	 * peer -- genuine peer stalls (healthy loop, no bytes) still time out. */
	if (c->state != HS_IDLE && c->state != HS_DONE && c->state != HS_FAIL) {
		unsigned long pnow = now_ticks();
		if (c->last_poll_tick == 0 || (pnow - c->last_poll_tick) > 30) {
			c->progress_ticks = pnow;
		}
		c->last_poll_tick = pnow;
	}

	/* No-progress timeout. */
	if (c->state != HS_IDLE && c->state != HS_DONE && c->state != HS_FAIL) {
		unsigned long now = now_ticks();
		/* fixes375 — POST waits ~60s (login-approval long-poll).
		 * fixes718 — a GET that has reached HS_HEADERS/HS_BODY has a live,
		 * responding origin: give it 15s (RESP_NO_PROGRESS_TICKS) so a
		 * slow-but-alive backend isn't killed. Connect/handshake stage
		 * keeps the tight 4s so truly dead hosts still fast-fail. */
		unsigned long limit;
		if (c->post_body != NULL)
			limit = (unsigned long)POST_NO_PROGRESS_TICKS;
		else if (c->state == HS_HEADERS || c->state == HS_BODY)
			limit = (unsigned long)RESP_NO_PROGRESS_TICKS;
		else
			limit = (unsigned long)NO_PROGRESS_TICKS;
		if (now - c->progress_ticks > limit) {
			/* fixes243 — salvage partial body on timeout too.
			 * Servers that send some response then stall (e.g.
			 * Google's JA3-blocked reset path) leave us with a
			 * usable partial document. Better to render truncated
			 * HTML than show about:fetcherror.
			 *
			 * fixes255 — same 512-byte threshold as the peer-close
			 * salvage. Anything smaller is a failure signature
			 * (TLS-fingerprint reject, server error response), and
			 * salvaging it hides the failure from the dead-host
			 * blocklist + lets us pool a stalled connection.
			 *
			 * Also clear keep_alive_ok to keep the stalled
			 * connection out of the pool. */
			if (c->state == HS_BODY && c->body_bytes >= 512) {
				macsurf_debug_log_writef(
					"https: timeout SALVAGE body=%ld",
					c->body_bytes);
				c->cache_eligible = 0;
				c->keep_alive_ok = 0;
				hctx_finish(c);
				return;
			}
			hctx_fail(c, "https: connection timed out");
			return;
		}
	}
}

/* ---------- fetcher_operation_table impl ---------- */

static bool macos9_https_initialise(lwc_string *s) { (void)s; return true; }
static void macos9_https_finalise(lwc_string *s)   { (void)s; }
/* fixes499d — refuse to fetch pure-analytics / never-executed bundles so
 * we don't pay their download cost on every page load. These are JS we
 * already SKIP in js_exec, so downloading them is wasted bandwidth+time:
 *   - googletagmanager / gtag : Google Analytics, ~366 KB, never run.
 * Returning false here makes NetSurf treat the subresource as
 * unavailable (it simply doesn't load — nothing on the page depends on
 * it). Only matches analytics; all functional resources still fetch.
 * Kept narrow on purpose: the XF upload bundles (exif/attachment) are
 * NOT blocked here because XF's loader may probe their load state. */
static bool macos9_https_acceptable(const struct nsurl *u)
{
	lwc_string *host_lwc;
	const char *host;
	bool block = false;

	if (u == NULL) return true;
	host_lwc = nsurl_get_component((struct nsurl *)u, NSURL_HOST);
	if (host_lwc != NULL) {
		host = lwc_string_data(host_lwc);
		if (host != NULL &&
		    (strstr(host, "googletagmanager") != NULL ||
		     strstr(host, "google-analytics") != NULL)) {
			block = true;
		}
		lwc_string_unref(host_lwc);
	}
	if (block) {
		macsurf_debug_log_writef("https: BLOCK analytics host");
		return false;
	}
	return true;
}

static void *macos9_https_setup(struct fetch *p, struct nsurl *u,
	bool o, bool d, const char *pu,
	const struct fetch_multipart_data *pm, const char **h)
{
	int i, slot = -1;
	lwc_string *host_lwc, *path_lwc, *port_lwc, *query_lwc;
	const char *hs, *ps;
	size_t hs_n, ps_n;
	struct macos9_https_ctx *c;

	(void)o; (void)d; (void)pm;

	for (i = 0; i < MAX_HTTPS_F; i++) {
		if (https_slots[i].state == HS_IDLE) { slot = i; break; }
	}
	if (slot < 0) {
		MS_LOG("https_setup: NO FREE SLOTS");
		return NULL;
	}
	c = &https_slots[slot];
	memset(c, 0, sizeof *c);
	c->parent = p;
	c->url = nsurl_ref(u);
	/* fixes835 (#167 M1) — capture core's additional request headers now
	 * (the h[] array is only valid for this setup call). */
	macos9_capture_extra_headers(h, c->caller_hdrs, sizeof c->caller_hdrs);
	c->state = HS_QUEUED;
	c->content_length = -1;
	c->status = 0;
	c->port = 443;
	c->keep_alive_ok = 1;   /* fixes231 — default eligible; cleared on
	                         * "Connection: close" response or any error */

	/* fixes312 (#144) — capture POST body. NetSurf core owns `pu` only
	 * for the lifetime of the fetch_start call; copy so the fetcher
	 * can stream it later when the TLS handshake completes. */
	if (pu != NULL) {
		size_t pu_len = strlen(pu);
		c->post_body = (char *)malloc(pu_len);
		if (c->post_body == NULL) {
			MS_LOG("https_setup: post_body alloc failed");
			c->state = HS_IDLE;
			nsurl_unref(c->url); c->url = NULL;
			return NULL;
		}
		memcpy(c->post_body, pu, pu_len);
		c->post_body_len = (UInt32)pu_len;
		c->post_body_sent = 0;
		/* POST responses are not safely poolable in the keep-alive
		 * pool — many servers close the connection after a POST. */
		c->keep_alive_ok = 0;
	} else if (pm != NULL) {
		/* fixes721 (#144 file upload) — multipart POST: serialise the
		 * parts (reading file parts off disk) into a multipart/form-data
		 * body; record the boundary in the Content-Type. */
		extern char *macos9_build_multipart(
			const struct fetch_multipart_data *pm_,
			char *boundary_out, long *out_len);
		char boundary[64];
		long body_len = 0;
		char *body = macos9_build_multipart(pm, boundary, &body_len);
		if (body != NULL && body_len > 0) {
			c->post_body = body;
			c->post_body_len = (UInt32)body_len;
			c->post_body_sent = 0;
			c->keep_alive_ok = 0;
			sprintf(c->post_ctype,
				"multipart/form-data; boundary=%s", boundary);
		} else if (body != NULL) {
			free(body);
		}
	}

	host_lwc = nsurl_get_component(u, NSURL_HOST);
	path_lwc = nsurl_get_component(u, NSURL_PATH);
	port_lwc = nsurl_get_component(u, NSURL_PORT);
	if (host_lwc == NULL) {
		MS_LOG("https_setup: no host");
		c->state = HS_IDLE;
		nsurl_unref(c->url); c->url = NULL;
		if (c->post_body != NULL) {
			free(c->post_body);
			c->post_body = NULL;
		}
		return NULL;
	}
	hs = lwc_string_data(host_lwc);
	hs_n = lwc_string_length(host_lwc);
	if (hs_n >= sizeof c->host) hs_n = sizeof c->host - 1;
	memcpy(c->host, hs, hs_n);
	c->host[hs_n] = 0;
	lwc_string_unref(host_lwc);

	if (port_lwc) {
		c->port = (unsigned short)atoi(lwc_string_data(port_lwc));
		if (c->port == 0) c->port = 443;
		lwc_string_unref(port_lwc);
	}

	/* fixes231 — build pool key. sprintf is safe: host[256] fits in
	 * 280-byte pool_key with ":65535" + NUL spare. */
	sprintf(c->pool_key, "%s:%d", c->host, (int)c->port);

	if (path_lwc) {
		ps = lwc_string_data(path_lwc);
		ps_n = lwc_string_length(path_lwc);
		if (ps_n == 0) { c->path[0] = '/'; c->path[1] = 0; }
		else {
			if (ps_n >= sizeof c->path) ps_n = sizeof c->path - 1;
			memcpy(c->path, ps, ps_n);
			c->path[ps_n] = 0;
		}
		lwc_string_unref(path_lwc);
	} else {
		c->path[0] = '/'; c->path[1] = 0;
	}

	/* Append ?query if present — Drupal et al. cache-bust assets with
	 * query strings and respond 400 / 404 if the query is stripped. */
	query_lwc = nsurl_get_component(u, NSURL_QUERY);
	if (query_lwc) {
		const char *qs = lwc_string_data(query_lwc);
		size_t qs_n = lwc_string_length(query_lwc);
		size_t cur = strlen(c->path);
		/* nsurl_get_component(QUERY) returns the query WITHOUT the
		 * leading '?'. Add it ourselves. */
		if (qs_n > 0 && cur + 1 + qs_n < sizeof c->path) {
			c->path[cur] = '?';
			memcpy(c->path + cur + 1, qs, qs_n);
			c->path[cur + 1 + qs_n] = 0;
		}
		lwc_string_unref(query_lwc);
	}

	/* fixes237 — re-enable disk cache HIT path. fixes222 disabled this
	 * because the synthetic FETCH_HEADER stream included an "HTTP/1.1
	 * 200" status line that confused html_create. The HS_CACHEHIT
	 * branch in hctx_poll now matches the working HTTP fetcher pattern
	 * exactly (Content-Type only + fetch_set_http_code), so re-arm the
	 * lookup. If we get a hit, we route through HS_CACHEHIT and skip
	 * TLS + network entirely. ~700-1500 ms saved per cached resource
	 * on a warm reload. */
	{
		const char *url_str = nsurl_access(u);
		/* fixes312 (#144) — POST responses are not cacheable
		 * (non-idempotent). Skip the lookup so search forms etc.
		 * always go to network.
		 * fixes374 (#167) — never SERVE a cached Facebook page either:
		 * a stale login page (dead CSRF tokens, no Set-Cookie) is what
		 * broke login. Always hit the network for FB hosts. This also
		 * evicts any FB pages a pre-fixes374 build already cached. */
		/* fixes981 — a CONDITIONAL request must never be answered
		 * from our own disk copy. Once a disk hit carries real
		 * Cache-Control/ETag (above), llcache can decide the copy is
		 * stale and ask "has this changed?" -- and serving the same
		 * stale bytes back answers nothing, leaves the object just as
		 * stale, and asks again on the next request. The conditional
		 * headers ARE the signal that core already holds this body, so
		 * the only useful answer comes from the network. caller_hdrs is
		 * captured earlier in this same setup(), so it is populated. */
		if (c->post_body == NULL &&
		    url_str != NULL &&
		    !host_is_fb_asset(c->host) &&
		    !macos9_hdr_has_ci(c->caller_hdrs, "if-none-match:") &&
		    !macos9_hdr_has_ci(c->caller_hdrs, "if-modified-since:") &&
		    macos9_cache_lookup_hdrs(url_str, &c->cache_hit_body,
				&c->cache_hit_len,
				c->cache_hit_mime,
				sizeof(c->cache_hit_mime),
				&c->cache_hit_status,
				c->cache_hit_hdrs,
				sizeof(c->cache_hit_hdrs))) {
			c->state = HS_CACHEHIT;
			strncpy(c->mime, c->cache_hit_mime,
				sizeof(c->mime) - 1);
			c->mime[sizeof(c->mime) - 1] = '\0';
			c->status = c->cache_hit_status;
			c->content_length = c->cache_hit_len;
			c->keep_alive_ok = 0;   /* no connection to pool */
			macsurf_debug_log_writef(
				"https_setup CACHE hit url=%s mime=%s len=%ld",
				url_str, c->cache_hit_mime, c->cache_hit_len);
			/* fixes768 (#232) — RECON: only flag a cache entry with a
			 * bad (empty) mime, which would download every time. */
			if (c->cache_hit_mime[0] == 0) {
				macsurf_debug_log_writef(
					"RECON MIME cache host=%s path=%s "
					"mime=(empty)", c->host, c->path);
			}
		}
	}

	macsurf_debug_log_writef("https_setup: host=%s port=%d cache_hit=%d path=%s",
		c->host, (int)c->port,
		(c->state == HS_CACHEHIT) ? 1 : 0,
		c->path);
	return c;
}

static bool macos9_https_start(void *ctx)
{
	struct macos9_https_ctx *c = (struct macos9_https_ctx *)ctx;
	if (c) {
		c->started = 1;   /* fixes232a — unblocks HS_QUEUED in hctx_poll */
		if (c->state == HS_QUEUED) {
			c->progress_ticks = now_ticks();
		}
	}
	return true;
}

static void macos9_https_abort(void *ctx)
{
	struct macos9_https_ctx *c = (struct macos9_https_ctx *)ctx;
	if (c) c->aborted = 1;
}

static void macos9_https_free(void *ctx)
{
	struct macos9_https_ctx *c = (struct macos9_https_ctx *)ctx;
	if (!c) return;
	if (c->state != HS_IDLE) hctx_clear(c);
}

static void macos9_https_poll(lwc_string *s)
{
	int i;
	(void)s;
	for (i = 0; i < MAX_HTTPS_F; i++) {
		if (https_slots[i].state == HS_IDLE) continue;
		if (https_slots[i].aborted &&
		    (https_slots[i].state == HS_QUEUED)) {
			hctx_fail(&https_slots[i], "https: aborted-queued");
			continue;
		}
		hctx_poll(&https_slots[i]);
	}
}

int macos9_https_fetcher_active(void)
{
	int i, n = 0;
	for (i = 0; i < MAX_HTTPS_F; i++) {
		if (https_slots[i].state != HS_IDLE &&
		    https_slots[i].state != HS_DONE &&
		    https_slots[i].state != HS_FAIL) n++;
	}
	return n;
}

nserror macos9_https_fetcher_register(void)
{
	struct fetcher_operation_table ops;
	lwc_string *ss;
	/* fixes705 — no dead-host preload from disk. The blocklist is now
	 * session-only (see dead_host_add) so a stale persisted entry can no
	 * longer blank a working site on launch. Purge any deadhosts.txt left by
	 * an older build so already-poisoned installs self-heal on first launch. */
	macos9_deadhost_clear();
	ops.initialise = macos9_https_initialise;
	ops.acceptable = macos9_https_acceptable;
	ops.setup      = macos9_https_setup;
	ops.start      = macos9_https_start;
	ops.abort      = macos9_https_abort;
	ops.free       = macos9_https_free;
	ops.poll       = macos9_https_poll;
	ops.fdset      = NULL;
	ops.finalise   = macos9_https_finalise;
	lwc_intern_string("https", 5, &ss);
	fetcher_add(ss, &ops);
	return NSERROR_OK;
}
