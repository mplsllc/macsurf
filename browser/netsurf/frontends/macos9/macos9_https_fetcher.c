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
#include "macsurf_debug.h"

#include <string.h>
#include <stdlib.h>

#ifdef __MACOS9__
#include <Events.h>
#endif

#include "ostls_async.h"
#include "ostls_http.h"
#include "macos9_disk_cache.h"

#define MAX_HTTPS_F        64   /* fixes241: was 32; image-heavy pages
                                    hit ~10 NO FREE SLOTS / cold load at 32
                                    because setup() is called for every
                                    queued fetch up-front (start gating
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
 * on demand so this is only a ceiling, not a per-fetch allocation. */
#define HDR_BUF_MAX        65536
/* fixes234 — bumped from 1024 to 8192. With sleep=0 in main.c we poll
 * ~hundreds of times per second, but at 1 KB per drain the body delivery
 * rate ceilinged at ~60 KB/s and a 59 KB mactrove home page took ~2.5 s
 * of dead pump time. 8 KB matches BearSSL's typical record size; one
 * Pump+Read cycle now drains 6-8× more decrypted body per pass. */
#define READ_CHUNK         8192
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
#define NO_PROGRESS_TICKS  240   /* 4s at 60Hz */

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
	 * would fail build_request once the jar filled. */
	char             req_buf[8192];
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
	UInt32           last_rx_bytes;  /* fixes414 — last OT recv-byte count
	                                  * seen, to credit raw wire progress to
	                                  * the no-progress watchdog. */

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
	char            *cache_hit_body;
	long             cache_hit_len;
	char             cache_hit_mime[128];
	int              cache_hit_status;

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

	/* fixes256 — re-enable persistence to disk. fixes244 disabled this
	 * because fixes238 had no notion of "host has never succeeded"; a
	 * transient timeout on mactrove late in a session was persisted and
	 * poisoned subsequent sessions. The success_host_check guard added
	 * in fixes244 now ensures dead_host_add is refused for any host
	 * we've successfully fetched at any point, so the file only ever
	 * accumulates truly-dead hosts (fonts.googleapis.com etc.). Saves
	 * the 4s fonts.googleapis timeout on every cold-load of every
	 * subsequent session. */
	{
		char ser[HTTPS_DEADHOSTS * (HTTPS_POOL_KEY_LEN + 2) + 4];
		long pos = 0;
		int j;
		for (j = 0; j < dead_hosts_count; j++) {
			long elen = (long)strlen(dead_hosts[j]);
			if (pos + elen + 1 >= (long)sizeof ser) break;
			memcpy(ser + pos, dead_hosts[j], elen);
			pos += elen;
			ser[pos++] = '\n';
		}
		if (pos < (long)sizeof ser) ser[pos] = '\0';
		macos9_deadhost_save(ser, pos);
	}
}

/* fixes256 — load persisted dead-host list at startup. Parses
 * "host:port\n" lines and populates the in-memory array. Called from
 * macos9_https_fetcher_register. */
static void dead_host_load_from_disk(void)
{
	char buf[HTTPS_DEADHOSTS * (HTTPS_POOL_KEY_LEN + 2) + 4];
	long blen;
	long pos = 0;

	blen = macos9_deadhost_load(buf, (long)sizeof(buf));
	if (blen <= 0) return;

	while (pos < blen && dead_hosts_count < HTTPS_DEADHOSTS) {
		long line_start = pos;
		long line_end;
		long line_len;
		while (pos < blen && buf[pos] != '\n' && buf[pos] != '\r')
			pos++;
		line_end = pos;
		while (pos < blen && (buf[pos] == '\n' || buf[pos] == '\r'))
			pos++;
		line_len = line_end - line_start;
		if (line_len <= 0) continue;
		if (line_len >= HTTPS_POOL_KEY_LEN)
			line_len = HTTPS_POOL_KEY_LEN - 1;
		memcpy(dead_hosts[dead_hosts_count], buf + line_start,
			line_len);
		dead_hosts[dead_hosts_count][line_len] = '\0';
		macsurf_debug_log_writef(
			"https: dead-host PRELOAD %s",
			dead_hosts[dead_hosts_count]);
		dead_hosts_count++;
	}
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
#define HTTPS_POOL_TTL_TICKS 1200  /* 20s at 60Hz */

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

/* ---------- helpers ---------- */

static unsigned long now_ticks(void)
{
#ifdef __MACOS9__
	return (unsigned long)TickCount();
#else
	return 0;
#endif
}

static void hctx_clear(struct macos9_https_ctx *c)
{
	if (c->hdr_buf) { free(c->hdr_buf); c->hdr_buf = NULL; }
	c->hdr_len = 0;
	c->hdr_cap = 0;
	if (c->conn) {
		OSTLS_Close(c->conn);
		OSTLS_Dispose(c->conn);
		c->conn = NULL;
	}
	if (c->cache_capture) { free(c->cache_capture); c->cache_capture = NULL; }
	c->cache_cap_len = 0;
	c->cache_cap_cap = 0;
	if (c->cache_hit_body) { free(c->cache_hit_body); c->cache_hit_body = NULL; }
	c->cache_hit_len = 0;
	/* fixes312 (#144) — release captured POST body. */
	if (c->post_body) { free(c->post_body); c->post_body = NULL; }
	c->post_body_len = 0;
	c->post_body_sent = 0;
	if (c->url) { nsurl_unref(c->url); c->url = NULL; }
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
	if (c->cache_capture) { free(c->cache_capture); c->cache_capture = NULL; }
	c->cache_cap_len = 0;
	c->cache_cap_cap = 0;
	c->cache_overflow = 0;
	c->cache_eligible = 0;
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

static void hctx_fail(struct macos9_https_ctx *c, const char *why)
{
	struct fetch *p;
	fetch_msg msg;

	if (c->state == HS_FAIL || c->state == HS_DONE) return;

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
		OSTLSDiagnostics diag;
		memset(&diag, 0, sizeof diag);
		OSTLS_GetDiagnostics(c->conn, &diag);
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
	 * instead of bouncing. */
	if (c->aborted == 0 &&
	    c->url != NULL && c->pool_key[0] != '\0' &&
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
	if (why != NULL && c->pool_key[0] != '\0' &&
	    (strcmp(why, "https: connection timed out") == 0 ||
	     strcmp(why, "https: peer closed before complete") == 0 ||
	     strcmp(why, "https: handshake/transport failed") == 0)) {
		dead_host_add(c->pool_key);
	}

	msg.type = FETCH_ERROR;
	msg.data.error = why ? why : "https: fetch failed";
	fetch_send_callback(&msg, c->parent);

	p = c->parent;
	hctx_clear(c);
	fetch_remove_from_queues(p);
	fetch_free(p);
}

static void hctx_finish(struct macos9_https_ctx *c)
{
	struct fetch *p;
	fetch_msg msg;

	if (c->state == HS_FAIL || c->state == HS_DONE) return;

	macsurf_debug_log_writef("https: done body=%ld status=%d",
		c->body_bytes, c->status);
	macsurf_profile_stamp("fetch-finished");
	/* fixes369 (#167) — page-weight accounting: fold this completed
	 * sub-resource's body size + 1 into the per-load totals. */
	macsurf_profile_add_bytes(c->body_bytes);
	macsurf_profile_count_resource();

	/* fixes218 — write to disk before tearing down the slot. */
	if (c->cache_eligible && !c->cache_overflow &&
	    c->cache_capture != NULL && c->cache_cap_len > 0 &&
	    c->url != NULL) {
		const char *u = nsurl_access(c->url);
		if (u != NULL) {
			macos9_cache_store(u, c->status, c->mime,
				c->cache_capture, c->cache_cap_len);
		}
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

	*sep = 0;
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
		int   i;
		int   fb_host = 0;	/* fixes368d — gate the full-header dump */
		size_t hh;
		static const char forced_ct[] =
			"Content-Type: application/octet-stream";

		/* fixes368d (#167) — for facebook.com only, dump EVERY response
		 * header (except Set-Cookie, logged name-only elsewhere so no
		 * session secret hits disk) so the live login test shows exactly
		 * what FB returned — Location, x-fb-debug, vary, content-type, etc.
		 * Gated to FB so other sites stay quiet. */
		hh = strlen(c->host);
		if (hh >= 12 &&
		    strncasecmp(c->host + hh - 12, "facebook.com", 12) == 0 &&
		    (hh == 12 || c->host[hh - 13] == '.'))
			fb_host = 1;

		while ((p = find_line(&cur, &cur_len)) != NULL) {
			if (p[0] == 0) break;
			if (n_header_lines < 64) {
				header_lines[n_header_lines++] = p;
			}
			if (fb_host &&
			    strncasecmp(p, "Set-Cookie:", 11) != 0) {
				macsurf_debug_log_writef("https: < %s", p);
			}
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
				 * marker. We HAVE JavaScript (Duktape + the fixes377
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

		/* Now forward all headers, substituting Content-Type when
		 * force_download is true. */
		for (i = 0; i < n_header_lines; i++) {
			const char *line = header_lines[i];
			if (force_download &&
			    strncasecmp(line, "Content-Type:", 13) == 0) {
				line = forced_ct;
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
	}

	if (c->chunked) OSTLS_HTTP_ChunkDecoderInit(&c->chunk);

	/* fixes425 — abort image downloads whose body would exceed 512 KB.
	 * Large forum attachment PNGs (1.4-1.7 MB) corrupt the heap when
	 * 4+ are in-flight simultaneously (llcache source_data + compressed
	 * Handles accumulate past a threshold that triggers a system crash on
	 * the G3 iMac). Avatars (1-5 KB), logos (~4 KB), and typical inline
	 * images are well under 512 KB and are unaffected.
	 * Only applies when Content-Length is known and status is 200. */
	if (c->status == 200 &&
	    c->content_length > 512L * 1024L &&
	    strncasecmp(c->mime, "image/", 6) == 0) {
		macsurf_debug_log_writef(
			"https: image too large (%ld B > 512KB), skip",
			c->content_length);
		c->aborted = 1; /* suppress HTTP fallback — we don't want it over HTTP either */
		hctx_fail(c, "image too large");
		return 2;
	}

	return 1;
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
				msg.type = FETCH_DATA;
				msg.data.header_or_data.buf = (const uint8_t*)decode_out;
				msg.data.header_or_data.len = out_w;
				fetch_send_callback(&msg, c->parent);
				c->body_bytes += out_w;
				hctx_cache_capture(c, decode_out, (long)out_w);
			}
			if (in_c == 0 && out_w == 0) break;	/* would-loop guard */
			in += in_c;
			in_left -= in_c;
		}
		/* CHUNKDIAG — remove after Google-Fonts chunked stall is fixed. */
		macsurf_debug_log_writef(
			"CHUNKDIAG feed: in=%ld state=%d remain=%ld bodyb=%ld",
			(long)n, (int)c->chunk.state,
			(long)c->chunk.remaining, (long)c->body_bytes);
		if (c->chunk.state == kOSTLSChunkStateDone) return 1;
		return 0;
	} else {
		long deliver = n;
		if (c->content_length >= 0 &&
		    c->body_bytes + deliver > c->content_length) {
			deliver = c->content_length - c->body_bytes;
		}
		if (deliver > 0) {
			msg.type = FETCH_DATA;
			msg.data.header_or_data.buf = (const uint8_t*)buf;
			msg.data.header_or_data.len = (size_t)deliver;
			fetch_send_callback(&msg, c->parent);
			c->body_bytes += deliver;
			hctx_cache_capture(c, buf, deliver);
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
	char  cookie_hdr[6144];
	char *cookie_str;
	cookie_hdr[0] = '\0';
	cookie_str = (c->url != NULL) ? urldb_get_cookie(c->url, true) : NULL;
	if (cookie_str != NULL) {
		size_t cl;
		/* fixes378 — drop any stuck noscript=1 before it reaches FB. */
		cookie_strip_noscript(cookie_str);
		cl = strlen(cookie_str);
		/* "Cookie: " (8) + value + "\r\n" (2) + NUL (1) = cl + 11 */
		if (cl > 0 && cl + 11 <= sizeof cookie_hdr) {
			strcpy(cookie_hdr, "Cookie: ");
			strcat(cookie_hdr, cookie_str);
			strcat(cookie_hdr, "\r\n");
		} else {
			/* Refuse to truncate — a half cookie header is worse
			 * than none (FB would reject the session). Logged so
			 * the cap can be raised if a real jar ever exceeds it. */
			macsurf_debug_log_writef(
				"https: cookie hdr too big cl=%ld cap=%ld",
				(long)cl,
				(long)sizeof cookie_hdr);
		}
		/* fixes368c (#167) — names only, never values. */
		{
			char cknames[256];
			cookie_names_only(cookie_str, cknames,
				(int)sizeof cknames);
			macsurf_debug_log_writef("https: cookies sent: %s",
				cknames);
		}
		free(cookie_str);
	}
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
	if (c->post_body != NULL) {
		/* fixes312 (#144) — POST. Body goes out in a second
		 * OSTLS_Write after these headers; req_buf carries
		 * headers only.
		 * fixes367 (#167) — UA chosen per-host (macos9_user_agent_for_host):
		 * facebook.com gets a vintage Mozilla/4.0 Mac string to unlock
		 * the no-JS mbasic page; everything else keeps MacSurf's UA. */
		rn = sprintf(c->req_buf,
			"POST %s HTTP/1.1\r\n"
			"Host: %s\r\n"
			"User-Agent: %s\r\n"
			"Accept: text/html,application/xhtml+xml,*/*;q=0.8\r\n"
			"Accept-Language: en-US,en;q=0.5\r\n"
			"Accept-Encoding: identity\r\n"
			"%s"
			"Content-Type: application/x-www-form-urlencoded\r\n"
			"Content-Length: %lu\r\n"
			"Connection: keep-alive\r\n"
			"\r\n",
			c->path, c->host, ua, cookie_hdr,
			(unsigned long)c->post_body_len);
	} else {
		rn = sprintf(c->req_buf,
			"GET %s HTTP/1.1\r\n"
			"Host: %s\r\n"
			"User-Agent: %s\r\n"
			"Accept: text/html,application/xhtml+xml,*/*;q=0.8\r\n"
			"Accept-Language: en-US,en;q=0.5\r\n"
			"Accept-Encoding: identity\r\n"
			"%s"
			"Connection: %s\r\n"
			"\r\n",
			c->path, c->host, ua, cookie_hdr, conn);
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
	e = OSTLS_Pump(c->conn, PUMP_STEPS, &ev);
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
	}

	if (ev == kOSTLSEventFailed) {
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
				hctx_fail(c, "https: write failed");
				return;
			}
			if (written > 0) {
				c->req_sent += written;
				c->progress_ticks = now_ticks();
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
				if (hdr_append(c, rd, (long)got) < 0) return;
				r = parse_headers(c, &body_off);
				if (r == 2) return;	/* redirect terminal */
				if (r == 1) {
					long leftover = c->hdr_len - body_off;
					c->state = HS_BODY;
					/* CHUNKDIAG — dump the first body bytes the
					 * decoder is fed, so we can see if it starts
					 * at "c1\r\n" (correct) or a wrong offset.
					 * Remove after the chunked stall is fixed. */
					{
						static const char hexd[] = "0123456789abcdef";
						char hxb[80];
						long k, hn = (leftover < 24) ? leftover : 24;
						int hp = 0;
						for (k = 0; k < hn; k++) {
							unsigned char ch =
							 (unsigned char)c->hdr_buf[body_off + k];
							hxb[hp++] = hexd[ch >> 4];
							hxb[hp++] = hexd[ch & 15];
							hxb[hp++] = ' ';
						}
						hxb[hp] = '\0';
						macsurf_debug_log_writef(
						 "CHUNKDIAG split: chunked=%d clen=%ld off=%ld leftover=%ld body0: %s",
						 c->chunked, c->content_length,
						 body_off, leftover, hxb);
					}
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

	/* No-progress timeout. */
	if (c->state != HS_IDLE && c->state != HS_DONE && c->state != HS_FAIL) {
		unsigned long now = now_ticks();
		/* fixes375 — POST waits ~60s (login-approval long-poll); GET 4s. */
		unsigned long limit = (c->post_body != NULL)
			? (unsigned long)POST_NO_PROGRESS_TICKS
			: (unsigned long)NO_PROGRESS_TICKS;
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
static bool macos9_https_acceptable(const struct nsurl *u) { (void)u; return true; }

static void *macos9_https_setup(struct fetch *p, struct nsurl *u,
	bool o, bool d, const char *pu,
	const struct fetch_multipart_data *pm, const char **h)
{
	int i, slot = -1;
	lwc_string *host_lwc, *path_lwc, *port_lwc, *query_lwc;
	const char *hs, *ps;
	size_t hs_n, ps_n;
	struct macos9_https_ctx *c;

	(void)o; (void)d; (void)pm; (void)h;

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
	}

	host_lwc = nsurl_get_component(u, NSURL_HOST);
	path_lwc = nsurl_get_component(u, NSURL_PATH);
	port_lwc = nsurl_get_component(u, NSURL_PORT);
	if (host_lwc == NULL) {
		MS_LOG("https_setup: no host");
		c->state = HS_IDLE;
		nsurl_unref(c->url); c->url = NULL;
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
		if (c->post_body == NULL &&
		    url_str != NULL &&
		    !host_is_fb_asset(c->host) &&
		    macos9_cache_lookup(url_str, &c->cache_hit_body,
				&c->cache_hit_len,
				c->cache_hit_mime,
				sizeof(c->cache_hit_mime),
				&c->cache_hit_status)) {
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
		}
	}

	/* macsurf_debug_log_writef supports only %d %ld %p %s %% — no precision
	 * specifier. Print the path as a plain %s; if it's huge, so be it. */
	macsurf_debug_log_writef("https_setup OK host=%s port=%d path=%s",
		c->host, (int)c->port, c->path);
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
	/* fixes256 — preload the dead-host blocklist from disk so the first
	 * fetch of a previously-broken host (fonts.googleapis.com etc.)
	 * fast-fails instead of paying the 4s timeout. Safe because
	 * dead_host_add (paired with success_host_check) refuses to
	 * persist any host that ever succeeded in any session. */
	dead_host_load_from_disk();
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
