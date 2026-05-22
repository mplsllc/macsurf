#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "utils/errors.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "content/fetch.h"
#include "content/fetchers.h"
#include "macsurf_debug.h"
#ifdef __MACOS9__
#include <Files.h>
#include <Folders.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>
#include <Threads.h>
extern OTClientContextPtr macos9_ot_context;
#endif

/* ============================================================
 * fixes172 — Persistent HTTP body cache.
 *
 * Real browsers cache the bytes they fetch so that a re-attempt
 * after a crash, a page refresh, or a back-button navigation
 * does not redo the entire network + parse work. MacSurf has
 * 16 MB of partition and the boot HD has gigabytes free — there
 * is no good reason to re-download a page on every retry.
 *
 * Storage: a flat folder `MacSurf Cache:` on the boot volume's
 * Desktop, one file per cached body. Filename is a 12-hex-char
 * FNV-1a 32-bit hash of the absolute URL plus an 8-char prefix
 * derived from the first chars of the URL for human readability:
 *
 *   "h_a1b2c3d4e5f6"  →  unambiguous, fits in HFS 31-char limit
 *
 * File format:
 *   [8 bytes ] magic 'MSCACHE\x01'
 *   [4 bytes ] big-endian uint32 status_code
 *   [4 bytes ] big-endian uint32 mime_len
 *   [4 bytes ] big-endian uint32 body_len
 *   [4 bytes ] big-endian uint32 reserved (TTL placeholder = 0)
 *   [mime_len] mime string (no NUL terminator on disk)
 *   [body_len] body bytes
 *
 * Eligibility: HTTP 200 OK responses with mime starting with
 * "text/html", "text/css", "text/plain", "application/xhtml",
 * "application/javascript", "application/json". Cap per entry
 * at 1 MB so the cache folder doesn't grow unbounded.
 *
 * Read-on-open: mfs_open looks up by URL hash. On hit, the
 * fetch flips into MFS_CACHEHIT and serves the cached body
 * directly without touching OT.
 * ============================================================ */

#define MACSURF_CACHE_MAGIC0 'M'
#define MACSURF_CACHE_MAGIC1 'S'
#define MACSURF_CACHE_MAGIC2 'C'
#define MACSURF_CACHE_MAGIC3 'A'
#define MACSURF_CACHE_MAGIC4 'C'
#define MACSURF_CACHE_MAGIC5 'H'
#define MACSURF_CACHE_MAGIC6 'E'
#define MACSURF_CACHE_MAGIC7 0x01

/* Cap any single cached body at 1 MB. Bigger responses are
 * still served live, just not cached. */
#define MACSURF_CACHE_MAX_BYTES (1L * 1024L * 1024L)

/* ---- Cache helpers (Carbon File Manager) ---- */

/* FNV-1a 32-bit hash. Cheap, well-distributed, no allocation. */
static unsigned long cache_hash_url(const char *url)
{
	unsigned long h = 0x811c9dc5UL;
	const unsigned char *p;
	if (url == NULL) return h;
	for (p = (const unsigned char *)url; *p != 0; p++) {
		h ^= (unsigned long)(*p);
		h = (h * 0x01000193UL) & 0xFFFFFFFFUL;
	}
	return h;
}

/* Build a Pascal string filename for the URL. fname is at least
 * 32 bytes. Format: "h_xxxxxxxxxxxx" (12-hex chars). */
static void cache_filename_for_url(const char *url, unsigned char *fname)
{
	unsigned long h = cache_hash_url(url);
	const char *hex = "0123456789abcdef";
	char tmp[16];
	int i;
	tmp[0] = 'h';
	tmp[1] = '_';
	for (i = 0; i < 8; i++) {
		tmp[2 + i] = hex[(h >> (28 - i * 4)) & 0xF];
	}
	tmp[10] = '\0';
	fname[0] = 10;
	memcpy(fname + 1, tmp, 10);
}

/* Resolve the cache folder FSSpec. Creates "MacSurf Cache" on the
 * boot Desktop if missing. Returns noErr on success. */
#ifdef __MACOS9__
static OSErr cache_dir_get(short *vRef, long *dirID)
{
	OSErr err;
	short desk_vref;
	long desk_dir;
	FSSpec spec;
	unsigned char fname[32];
	const char *name = "MacSurf Cache";
	size_t nlen;
	long new_dir;

	err = FindFolder(kOnSystemDisk, kDesktopFolderType,
			kDontCreateFolder, &desk_vref, &desk_dir);
	if (err != noErr) return err;

	nlen = strlen(name);
	if (nlen > 31) nlen = 31;
	fname[0] = (unsigned char)nlen;
	memcpy(fname + 1, name, nlen);

	err = FSMakeFSSpec(desk_vref, desk_dir, fname, &spec);
	if (err == fnfErr) {
		err = FSpDirCreate(&spec, smSystemScript, &new_dir);
		if (err != noErr) return err;
		err = FSMakeFSSpec(desk_vref, desk_dir, fname, &spec);
		if (err != noErr) return err;
	} else if (err != noErr) {
		return err;
	}

	/* Resolve the directory ID for the cache folder. We need a
	 * second FSMakeFSSpec on a sentinel name to coerce it. The
	 * canonical pattern: pass the directory's spec name to
	 * FSpGetCatInfo. For simplicity we just open the directory
	 * via FSpGetFInfo's parent route — actually easier: the
	 * spec.parID we just got IS the parent (Desktop), and the
	 * dir ID we want is the one FSpDirCreate returned or that
	 * a directory's catalog info exposes. Use PBGetCatInfo. */
	{
		CInfoPBRec pb;
		Str63 nm;
		memcpy(nm, spec.name, spec.name[0] + 1);
		memset(&pb, 0, sizeof(pb));
		pb.dirInfo.ioNamePtr = nm;
		pb.dirInfo.ioVRefNum = spec.vRefNum;
		pb.dirInfo.ioDrDirID = spec.parID;
		err = PBGetCatInfoSync(&pb);
		if (err != noErr) return err;
		*vRef = spec.vRefNum;
		*dirID = pb.dirInfo.ioDrDirID;
	}
	return noErr;
}
#endif /* __MACOS9__ */

/* Big-endian write helpers — keep the cache format portable. */
static void cache_write_be32(unsigned char *p, unsigned long v)
{
	p[0] = (unsigned char)((v >> 24) & 0xFF);
	p[1] = (unsigned char)((v >> 16) & 0xFF);
	p[2] = (unsigned char)((v >>  8) & 0xFF);
	p[3] = (unsigned char)( v        & 0xFF);
}
static unsigned long cache_read_be32(const unsigned char *p)
{
	return ((unsigned long)p[0] << 24) |
	       ((unsigned long)p[1] << 16) |
	       ((unsigned long)p[2] <<  8) |
	       ((unsigned long)p[3]);
}

/* Decide whether a (status, mime) response is worth caching. */
static int cache_mime_eligible(int status, const char *mime)
{
	if (status != 200) return 0;
	if (mime == NULL || mime[0] == '\0') return 0;
	if (strncmp(mime, "text/html", 9) == 0) return 1;
	if (strncmp(mime, "text/css", 8) == 0) return 1;
	if (strncmp(mime, "text/plain", 10) == 0) return 1;
	if (strncmp(mime, "application/xhtml", 17) == 0) return 1;
	if (strncmp(mime, "application/javascript", 22) == 0) return 1;
	if (strncmp(mime, "application/json", 16) == 0) return 1;
	return 0;
}

/* Store one response to disk. Body is body_len bytes pointed to
 * by body_ptr (caller retains ownership). Errors are silent —
 * cache write is best-effort. */
static void cache_store(const char *url, int status, const char *mime,
		const char *body_ptr, long body_len)
{
#ifdef __MACOS9__
	OSErr err;
	short vRef;
	long dirID;
	FSSpec spec;
	unsigned char fname[32];
	short ref = 0;
	unsigned char hdr[24];
	long count;
	size_t mime_len;

	if (url == NULL || body_ptr == NULL) return;
	if (body_len <= 0 || body_len > MACSURF_CACHE_MAX_BYTES) return;
	if (!cache_mime_eligible(status, mime)) return;

	err = cache_dir_get(&vRef, &dirID);
	if (err != noErr) return;

	cache_filename_for_url(url, fname);
	err = FSMakeFSSpec(vRef, dirID, fname, &spec);
	if (err == fnfErr) {
		err = FSpCreate(&spec, '????', '????', smSystemScript);
		if (err != noErr) return;
		err = FSMakeFSSpec(vRef, dirID, fname, &spec);
	}
	if (err != noErr) return;

	if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) return;
	SetFPos(ref, fsFromStart, 0);

	mime_len = strlen(mime);
	if (mime_len > 127) mime_len = 127;

	hdr[0] = MACSURF_CACHE_MAGIC0;
	hdr[1] = MACSURF_CACHE_MAGIC1;
	hdr[2] = MACSURF_CACHE_MAGIC2;
	hdr[3] = MACSURF_CACHE_MAGIC3;
	hdr[4] = MACSURF_CACHE_MAGIC4;
	hdr[5] = MACSURF_CACHE_MAGIC5;
	hdr[6] = MACSURF_CACHE_MAGIC6;
	hdr[7] = MACSURF_CACHE_MAGIC7;
	cache_write_be32(hdr + 8,  (unsigned long)status);
	cache_write_be32(hdr + 12, (unsigned long)mime_len);
	cache_write_be32(hdr + 16, (unsigned long)body_len);
	cache_write_be32(hdr + 20, 0UL);

	count = sizeof(hdr);
	FSWrite(ref, &count, hdr);
	if (mime_len > 0) {
		count = (long)mime_len;
		FSWrite(ref, &count, mime);
	}
	count = body_len;
	FSWrite(ref, &count, body_ptr);
	SetEOF(ref, (long)sizeof(hdr) + (long)mime_len + body_len);
	FSClose(ref);
	(void)FlushVol(NULL, vRef);

	macsurf_debug_log_writef(
		"CACHE store url=%s mime=%s len=%ld",
		url, mime, body_len);
#else
	(void)url; (void)status; (void)mime; (void)body_ptr; (void)body_len;
#endif
}

/* Append delivered body bytes to the per-fetch capture buffer.
 * Grows the buffer geometrically up to MACSURF_CACHE_MAX_BYTES; if
 * the body would exceed that, set cache_overflow and free the
 * partial buffer (the cache write is then skipped at FINISHED).
 *
 * Forward declaration: definition needs the macos9_fetch_ctx
 * struct which is below. We emit a forward decl here and put the
 * body inline at the bottom of this header section. */
struct macos9_fetch_ctx;
static void cache_capture_append(struct macos9_fetch_ctx *c,
		const char *buf, long len);

/* Read a cached response if present. Returns 1 on hit, 0 on miss
 * or any I/O error. On hit, *body_out is a malloc'd buffer the
 * caller must free; *body_len_out is its length; mime_out (cap >=
 * 128) receives the MIME string; *status_out is the HTTP status. */
static int cache_lookup(const char *url, char **body_out,
		long *body_len_out, char *mime_out, int mime_cap,
		int *status_out)
{
#ifdef __MACOS9__
	OSErr err;
	short vRef;
	long dirID;
	FSSpec spec;
	unsigned char fname[32];
	short ref = 0;
	unsigned char hdr[24];
	long count;
	unsigned long status_v;
	unsigned long mime_len;
	unsigned long body_len;
	char mime_buf[128];
	char *body;

	*body_out = NULL;
	*body_len_out = 0;
	if (mime_out != NULL && mime_cap > 0) mime_out[0] = '\0';
	*status_out = 0;

	if (url == NULL) return 0;

	err = cache_dir_get(&vRef, &dirID);
	if (err != noErr) return 0;

	cache_filename_for_url(url, fname);
	err = FSMakeFSSpec(vRef, dirID, fname, &spec);
	if (err != noErr) return 0;

	if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return 0;

	count = sizeof(hdr);
	if (FSRead(ref, &count, hdr) != noErr || count != sizeof(hdr)) {
		FSClose(ref);
		return 0;
	}
	if (hdr[0] != MACSURF_CACHE_MAGIC0 ||
	    hdr[1] != MACSURF_CACHE_MAGIC1 ||
	    hdr[2] != MACSURF_CACHE_MAGIC2 ||
	    hdr[3] != MACSURF_CACHE_MAGIC3 ||
	    hdr[4] != MACSURF_CACHE_MAGIC4 ||
	    hdr[5] != MACSURF_CACHE_MAGIC5 ||
	    hdr[6] != MACSURF_CACHE_MAGIC6 ||
	    hdr[7] != MACSURF_CACHE_MAGIC7) {
		FSClose(ref);
		return 0;
	}
	status_v = cache_read_be32(hdr + 8);
	mime_len = cache_read_be32(hdr + 12);
	body_len = cache_read_be32(hdr + 16);
	if (mime_len > 127 || body_len == 0 ||
			body_len > MACSURF_CACHE_MAX_BYTES) {
		FSClose(ref);
		return 0;
	}

	if (mime_len > 0) {
		count = (long)mime_len;
		if (FSRead(ref, &count, mime_buf) != noErr ||
				count != (long)mime_len) {
			FSClose(ref);
			return 0;
		}
	}
	mime_buf[mime_len] = '\0';

	body = (char *)malloc(body_len);
	if (body == NULL) {
		FSClose(ref);
		return 0;
	}
	count = (long)body_len;
	if (FSRead(ref, &count, body) != noErr ||
			count != (long)body_len) {
		free(body);
		FSClose(ref);
		return 0;
	}
	FSClose(ref);

	*body_out = body;
	*body_len_out = (long)body_len;
	*status_out = (int)status_v;
	if (mime_out != NULL && mime_cap > 0) {
		size_t n = mime_len;
		if (n >= (size_t)mime_cap) n = (size_t)mime_cap - 1;
		memcpy(mime_out, mime_buf, n);
		mime_out[n] = '\0';
	}
	macsurf_debug_log_writef(
		"CACHE hit url=%s mime=%s len=%ld status=%d",
		url, mime_buf, (long)body_len, (int)status_v);
	return 1;
#else
	(void)url; (void)body_out; (void)body_len_out;
	(void)mime_out; (void)mime_cap; (void)status_out;
	return 0;
#endif
}

#define PROXY_H "116.202.231.103"
#define PROXY_P 8765
#define MAX_F 64
/* fixes92 — bigger OTRcv buffer cuts the syscall count for large bodies.
 * 32 KB picks ~1 OTRcv per typical sub-resource response and ~3-4 per
 * the main HTML on MacTrove. Mac OS 9 stack frames are fine with this. */
#define RECV_B 32768
/* fixes92 — pool 16 simultaneous keep-alive sockets to the proxy.
 * NetSurf may have up to 16 fetches per host in flight after fixes91's
 * max_fetchers_per_host bump; 16 here matches. */
#define POOL_SIZE 16

/* fixes91 — new state MFS_QUEUED:
 * NetSurf core's fetch_dispatch_jobs() gates on max_fetchers / per-host
 * limits before calling ops.start. The HTTP fetcher previously sat in
 * MFS_INIT from setup-time and mfs_poll_one would run the OT state
 * machine regardless — so its slots stayed non-IDLE for fetches NetSurf
 * had never dispatched, eventually saturating the slot table. Slots
 * now start in MFS_QUEUED (no OT activity) and only transition to
 * MFS_INIT when ops.start is called, so slot accounting matches
 * NetSurf's fetch_ring/queue_ring view. */
enum mfs_state {
	MFS_IDLE,
	MFS_QUEUED,
	MFS_INIT,
	MFS_HEADERS,
	MFS_BODY,
	MFS_DONE,
	MFS_FAIL,
	MFS_NOTIFIED
};

/* fixes98 — chunked transfer-encoding decoder states.
 *   CS_SIZE    — reading hex chunk-size line up to \n
 *   CS_DATA    — copying chunk_remaining bytes as FETCH_DATA
 *   CS_CRLF    — consuming the \r\n that terminates a chunk's data
 *   CS_TRAILER — after final 0-size chunk, eating trailer headers
 *                until an empty line
 *   CS_DONE    — full body received; mfs_poll_one will transition the
 *                fetch to MFS_DONE on the next pass
 */
enum chunk_state {
	CS_SIZE = 0,
	CS_DATA,
	CS_CRLF,
	CS_TRAILER,
	CS_DONE
};

/* fixes94 — pool_key remembers which (host, port) this slot's endpoint
 * was opened to. Returned to the matching bucket on close. Direct
 * origins ("frogfind.com:80") and the proxy ("PROXY") each get their
 * own pool entries so reuse only happens on identical targets. */
#define POOL_KEY_LEN 96
struct macos9_fetch_ctx {
	struct fetch *parent; struct nsurl *url; enum mfs_state state;
	int redirects; int aborted;
#ifdef __MACOS9__
	EndpointRef ep;
#endif
	char *h_buf; long h_len; long h_cap;
	int status; char mime[128]; const char *err;
	long body_bytes; /* fixes311: total bytes delivered as FETCH_DATA after headers */
	long content_length; /* fixes91: -1 = unknown */
	int keep_alive_ok;   /* fixes91: 1 if endpoint can be reused after response */
	int chunked;         /* fixes91: 1 if Transfer-Encoding: chunked */
	char pool_key[POOL_KEY_LEN]; /* fixes94: host:port of this fetch's endpoint */
	/* fixes107 — last activity TickCount for the no-progress timeout. Set
	 * when ops.start transitions QUEUED→INIT, and reset whenever OTRcv
	 * delivers bytes. If 900 ticks (15s at 60Hz) elapse without any
	 * progress, the poll loop forces MFS_FAIL with a "Connection timed
	 * out" error so a silently-stalled proxy/origin surfaces as a real
	 * fetch failure rather than a frozen URL bar. */
	unsigned long progress_ticks;
	/* fixes98 — 3xx auto-follow. Location header captured during header
	 * parsing; if status is 3xx we emit FETCH_REDIRECT instead of body
	 * delivery and NetSurf's llcache opens a fresh fetch against this
	 * URL (relative resolution happens in llcache). */
	char redirect_url[512];
	/* fixes98 — chunked decoder. Active when chunked==1. */
	int chunk_state;            /* enum chunk_state */
	long chunk_remaining;       /* bytes left in current chunk's data */
	char chunk_size_buf[16];    /* hex line buffer */
	int chunk_size_len;
	int trailer_just_after_eol; /* tracks empty-line in CS_TRAILER */
	/* fixes160c/fixes161a — resource class assigned at setup time, used
	 * by the resource governor to apply per-class active-fetch caps. */
	int rclass;
	/* fixes172 — disk cache state.
	 *   cache_eligible : set to 1 by mfs_parse_headers when status 200
	 *                    + mime is in the text/* / xhtml / json /
	 *                    javascript whitelist.
	 *   cache_capture  : accumulator buffer for the body bytes while
	 *                    they stream in. Written to disk at FETCH_FINISHED.
	 *   cache_cap_len  : bytes currently in cache_capture.
	 *   cache_cap_cap  : allocated size of cache_capture.
	 *   cache_overflow : 1 if body exceeded MACSURF_CACHE_MAX_BYTES;
	 *                    when set we stop capturing and skip the write.
	 *   cache_hit_*    : cache_hit==1 means we're serving from disk;
	 *                    OT is never opened. The buffer + len are
	 *                    delivered via fetch_send_callback in the poll
	 *                    loop and the slot transitions straight to
	 *                    MFS_DONE. */
	int cache_eligible;
	char *cache_capture;
	long cache_cap_len;
	long cache_cap_cap;
	int cache_overflow;
	int cache_hit;
	char *cache_hit_body;
	long cache_hit_len;
	char cache_hit_mime[128];
	int cache_hit_status;
};
static struct macos9_fetch_ctx f_slots[MAX_F];

/* fixes172 — body capture appender. Called from the three places
 * where FETCH_DATA fires (mfs_parse_headers initial-body branch,
 * mfs_poll_one plain-body branch, process_chunked_bytes CS_DATA
 * branch). Geometric growth from 4 KB up to MACSURF_CACHE_MAX_BYTES;
 * overflow latches and frees the partial buffer. */
static void cache_capture_append(struct macos9_fetch_ctx *c,
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
			/* realloc failed — abandon caching for this fetch
			 * but don't disturb live delivery. */
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

	memcpy(c->cache_capture + c->cache_cap_len, buf, (size_t)len);
	c->cache_cap_len = want;
}

/* fixes161a — resource governor classes. Used by the URL classifier
 * and the per-class active-fetch caps. Each new fetch is classified
 * once at setup and the class is stored on the slot for cheap counting
 * during subsequent setups. */
#define MACOS9_RC_DOCUMENT 0
#define MACOS9_RC_CSS      1
#define MACOS9_RC_IMAGE    2
#define MACOS9_RC_SCRIPT   3
#define MACOS9_RC_FONT     4
#define MACOS9_RC_OTHER    5
#define MACOS9_RC__COUNT   6

/* fixes161a — per-class concurrent-fetch caps. MAX_F=64 stays as the
 * hard slot array size for all fetch types; the governor refuses new
 * setups when either the class cap or the global active cap is hit,
 * so the array is rarely full in practice. Documents (priority 0) get
 * 2 active to allow back-to-back navigation. CSS gets 4 (Apple ships
 * 8-12 sheets per page after the size gate). Images stay at 8 from
 * fixes160c. Scripts get the same peer-class 4 active that CSS gets —
 * Duktape is a capable engine and anything it can't run yet we'll fill
 * in-house, so JS is not a deferable second-tier resource. Fonts get
 * 2. OTHER gets 4 for XHR/manifest/etc. Global cap of 16 keeps total
 * active fetch pressure bounded regardless of which classes are
 * firing. Heavy-site mode (fixes161f) tightens caps proportionally
 * across ALL classes; it does not single scripts out. */
#define MAX_DOC_F     2
#define MAX_CSS_F     4
#define MAX_IMG_F     8
#define MAX_SCRIPT_F  4
#define MAX_FONT_F    2
#define MAX_OTHER_F   4
#define MAX_GLOBAL_F  16

/* fixes161a — set by macos9_http_mark_next_as_document() before a top-
 * level navigation. The next setup() call consumes the flag and
 * classifies its URL as DOCUMENT regardless of suffix. Falls back to
 * URL-suffix classification when the flag is clear. */
static int macos9_http_pending_document = 0;

void macos9_http_mark_next_as_document(void)
{
	macos9_http_pending_document = 1;
}

#ifdef __MACOS9__
/* fixes94 — keyed endpoint pool. Previously all entries were assumed
 * to be proxy connections; now we route http:// directly to the
 * origin, so each pool entry needs to remember its (host, port). */
struct ep_pool_entry {
	EndpointRef ep;
	char key[POOL_KEY_LEN];
};
static struct ep_pool_entry ep_pool[POOL_SIZE];
static int ep_pool_count = 0;

static EndpointRef
ep_pool_take(const char *key)
{
	int i;
	for (i = ep_pool_count - 1; i >= 0; i--) {
		OTResult look;
		EndpointRef ep;

		if (strcmp(ep_pool[i].key, key) != 0) continue;
		ep = ep_pool[i].ep;
		/* Compact: move tail entry into this slot. */
		ep_pool[i] = ep_pool[ep_pool_count - 1];
		ep_pool_count--;

		/* OTLook reports any pending event without consuming
		 * data. 0 means the endpoint is in T_DATAXFER with no
		 * pending events — healthy idle, safe to reuse. */
		look = OTLook(ep);
		if (look == 0) {
			macsurf_debug_log_writef(
				"ep_pool: REUSE key=%s remaining=%d",
				key, ep_pool_count);
			return ep;
		}
		macsurf_debug_log_writef(
			"ep_pool: discard key=%s look=%ld remaining=%d",
			key, (long)look, ep_pool_count);
		if (look == T_ORDREL) {
			OTRcvOrderlyDisconnect(ep);
		} else if (look == T_DISCONNECT) {
			OTRcvDisconnect(ep, NULL);
		}
		OTSndOrderlyDisconnect(ep);
		OTCloseProvider(ep);
		/* Restart scan from new top — index i may now refer to a
		 * different entry after compaction. */
		i = ep_pool_count;
	}
	return NULL;
}

static void
ep_pool_return(const char *key, EndpointRef ep)
{
	char drain[256];
	OTResult n;
	long drained = 0;

	if (ep == NULL) return;
	if (ep_pool_count >= POOL_SIZE) {
		macsurf_debug_log_writef(
			"ep_pool: full, closing (count=%d key=%s)",
			ep_pool_count, key);
		OTSndOrderlyDisconnect(ep);
		OTCloseProvider(ep);
		return;
	}

	/* Drain any trailing bytes so the next user of this endpoint
	 * sees a clean stream starting at the next response. */
	OTSetNonBlocking(ep);
	do {
		n = OTRcv(ep, drain, sizeof(drain), NULL);
		if (n > 0) drained += n;
	} while (n > 0);

	ep_pool[ep_pool_count].ep = ep;
	strncpy(ep_pool[ep_pool_count].key, key, POOL_KEY_LEN - 1);
	ep_pool[ep_pool_count].key[POOL_KEY_LEN - 1] = '\0';
	ep_pool_count++;
	macsurf_debug_log_writef(
		"ep_pool: STORED key=%s count=%d drained=%ld",
		key, ep_pool_count, drained);
}
#endif /* __MACOS9__ */

static char *mfs_find_line(char **buf, long *len) {
	char *start = *buf, *p = *buf, *end = *buf + *len;
	while(p < end-1) {
		if(p[0]=='\r' && p[1]=='\n') { *p = 0; *buf = p + 2; *len = (long)(end - *buf); return start; }
		p++;
	}
	return NULL;
}

static void mfs_close(struct macos9_fetch_ctx *c) {
#ifdef __MACOS9__
	if (c->ep) {
		/* fixes92 — fixes91's keep-alive was DOA. mfs_close runs from
		 * macos9_http_free, by which time macos9_http_poll has already
		 * transitioned state MFS_DONE → MFS_NOTIFIED, so the old check
		 * `state == MFS_DONE` always failed and every endpoint got
		 * closed regardless of keep_alive_ok. That means fixes91 paid
		 * 44 fresh OTConnects per MacTrove page (~the dial-up feel).
		 * The real eligibility test is just keep_alive_ok && !aborted. */
		if (c->keep_alive_ok && !c->aborted && c->pool_key[0] != '\0') {
			ep_pool_return(c->pool_key, c->ep);
		} else {
			OTSndOrderlyDisconnect(c->ep);
			OTCloseProvider(c->ep);
		}
		c->ep = NULL;
	}
#endif
}

static int mfs_open(struct macos9_fetch_ctx *c) {
#ifdef __MACOS9__
	OSStatus e; OTConfigurationRef cfg; DNSAddress dns; TCall call;
	char target[96];        /* host:port for OTInitDNSAddress + pool key */
	char path_buf[1024];    /* path?query for direct-mode request line */
	char req[2048];
	OTResult r;
	lwc_string *scheme_lwc;
	lwc_string *host_lwc;
	lwc_string *port_lwc;
	lwc_string *path_lwc;
	lwc_string *query_lwc;
	const char *scheme_str;
	const char *host_str;
	size_t host_len;
	size_t scheme_len;
	int port_num;
	int use_proxy;
	EndpointRef pooled;
	const char *u_full;

	scheme_lwc = NULL;
	host_lwc = NULL;
	port_lwc = NULL;
	path_lwc = NULL;
	query_lwc = NULL;

	/* fixes172 — disk-cache lookup BEFORE OT setup. If we have a
	 * fresh body on disk for this URL, populate c->cache_hit_* and
	 * skip everything below. mfs_poll_one's cache-hit fast path
	 * delivers headers + body + finished in one cycle. */
	{
		const char *url_str = nsurl_access(c->url);
		if (url_str != NULL && c->cache_hit == 0) {
			if (cache_lookup(url_str, &c->cache_hit_body,
					&c->cache_hit_len,
					c->cache_hit_mime,
					sizeof(c->cache_hit_mime),
					&c->cache_hit_status)) {
				c->cache_hit = 1;
				/* Populate the mime field expected by the
				 * existing finished-emit code. */
				strncpy(c->mime, c->cache_hit_mime,
						sizeof(c->mime) - 1);
				c->mime[sizeof(c->mime) - 1] = '\0';
				c->status = c->cache_hit_status;
				c->content_length = c->cache_hit_len;
				c->keep_alive_ok = 0;
				return 1; /* mfs_poll_one drives the rest */
			}
		}
	}

	scheme_lwc = nsurl_get_component(c->url, NSURL_SCHEME);
	host_lwc = nsurl_get_component(c->url, NSURL_HOST);
	if (scheme_lwc == NULL || host_lwc == NULL) {
		MS_LOG("mfs_open: missing scheme/host");
		goto fail_unref;
	}

	scheme_str = lwc_string_data(scheme_lwc);
	scheme_len = lwc_string_length(scheme_lwc);
	host_str = lwc_string_data(host_lwc);
	host_len = lwc_string_length(host_lwc);

	/* fixes94 — only HTTPS routes through the proxy (we have no TLS).
	 * Plain HTTP goes direct to the origin, saving the round-trip to
	 * the Hetzner box for ~every fetch on http-only sites. */
	use_proxy = (scheme_len == 5 && strncmp(scheme_str, "https", 5) == 0);

	if (use_proxy) {
		sprintf(target, "%s:%d", PROXY_H, PROXY_P);
		strcpy(c->pool_key, "PROXY");
	} else {
		port_lwc = nsurl_get_component(c->url, NSURL_PORT);
		if (port_lwc != NULL && lwc_string_length(port_lwc) > 0) {
			port_num = atoi(lwc_string_data(port_lwc));
			if (port_num <= 0 || port_num > 65535) port_num = 80;
		} else {
			port_num = 80;
		}
		if (host_len + 8 >= sizeof(target)) {
			MS_LOG("mfs_open: host too long");
			goto fail_unref;
		}
		sprintf(target, "%.*s:%d", (int)host_len, host_str, port_num);
		strncpy(c->pool_key, target, POOL_KEY_LEN - 1);
		c->pool_key[POOL_KEY_LEN - 1] = '\0';
	}

	MS_LOG("mfs_open: enter");
	pooled = ep_pool_take(c->pool_key);
	if (pooled != NULL) {
		c->ep = pooled;
		OTSetSynchronous(c->ep);
		OTSetBlocking(c->ep);
	} else {
		cfg = OTCreateConfiguration("tcp");
		if (!cfg) {
			macsurf_debug_log_writef(
				"mfs_open: OTCreateConfig FAIL target=%s", target);
			goto fail_unref;
		}
		c->ep = OTOpenEndpointInContext(cfg, 0, NULL, &e, macos9_ot_context);
		if (e != noErr || !c->ep) {
			macsurf_debug_log_writef(
				"mfs_open: OTOpenEndpoint err=%d target=%s",
				(int)e, target);
			goto fail_unref;
		}
		OTSetSynchronous(c->ep);
		OTSetBlocking(c->ep);
		if (OTBind(c->ep, NULL, NULL) != noErr) {
			MS_LOG("mfs_open: OTBind FAIL");
			goto fail_unref;
		}
		OTMemzero(&call, sizeof(TCall));
		call.addr.buf = (UInt8 *)&dns;
		call.addr.len = (short)OTInitDNSAddress(&dns, target);
		macsurf_debug_log_writef("mfs_open: OTConnect %s", target);
		if (OTConnect(c->ep, &call, NULL) != noErr) {
			macsurf_debug_log_writef(
				"mfs_open: OTConnect FAIL target=%s", target);
			goto fail_unref;
		}
	}

	/* Build the request line.
	 *   proxy: absolute-form  (GET http://example.com/foo HTTP/1.1)
	 *   direct: origin-form   (GET /foo HTTP/1.1)
	 * In both cases Host: holds the origin hostname.
	 *
	 * fixes95: direct fetches send `Connection: close` and skip the
	 * pool. The proxy buffers responses and emits Content-Length so
	 * we can frame them and keep-alive cleanly. Origin servers
	 * (e.g. nginx) reply with Transfer-Encoding: chunked + keep-alive
	 * for HTML; we don't have a chunked decoder yet, so without a
	 * Content-Length we can't tell where one response ends on a
	 * reused socket — the fetch just hangs. Connection: close makes
	 * the origin close after the response, which OTRcv reports as
	 * n==0 and we transition to MFS_DONE. Cost: one TCP setup per
	 * direct fetch. Still saves the Hetzner round-trip vs proxy. */
	/* fixes169 (SAFETY_REPORT §3) — bound the request-line write. The
	 * template constant is 86 chars; the variable part is the URL (or
	 * path?query) plus the host. Refuse the fetch if the combined
	 * length would exceed sizeof(req)-1 so sprintf cannot overrun the
	 * stack. CW8's MSL has no snprintf, so we size-check first and
	 * keep sprintf for the actual format. */
	{
		/* Length of the constant template (without %-substitutions). */
		size_t TEMPLATE_LEN = 86;
		if (use_proxy) {
			size_t u_len;
			u_full = nsurl_access(c->url);
			if (u_full == NULL) {
				MS_LOG("mfs_open: nsurl_access NULL");
				goto fail_unref;
			}
			u_len = strlen(u_full);
			if (TEMPLATE_LEN + u_len + host_len + 1 > sizeof(req)) {
				macsurf_debug_log_writef(
					"mfs_open: REJECT oversize URL "
					"u_len=%lu host_len=%lu cap=%lu",
					(unsigned long)u_len,
					(unsigned long)host_len,
					(unsigned long)sizeof(req));
				c->err = "URL too long";
				goto fail_unref;
			}
			macsurf_debug_log_writef("mfs_open: GET (proxy) %s",
					u_full);
			sprintf(req,
				"GET %s HTTP/1.1\r\n"
				"Host: %.*s\r\n"
				"User-Agent: MacSurf/0.2\r\n"
				"Accept: */*\r\n"
				"Connection: keep-alive\r\n\r\n",
				u_full, (int)host_len, host_str);
		} else {
			size_t pb_used;
			path_lwc = nsurl_get_component(c->url, NSURL_PATH);
			query_lwc = nsurl_get_component(c->url, NSURL_QUERY);
			{
				const char *p_str = NULL;
				size_t p_len = 0;
				const char *q_str = NULL;
				size_t q_len = 0;
				if (path_lwc != NULL && lwc_string_length(path_lwc) > 0) {
					p_str = lwc_string_data(path_lwc);
					p_len = lwc_string_length(path_lwc);
				}
				if (query_lwc != NULL && lwc_string_length(query_lwc) > 0) {
					q_str = lwc_string_data(query_lwc);
					q_len = lwc_string_length(query_lwc);
				}
				if (p_str == NULL) {
					strcpy(path_buf, "/");
					pb_used = 1;
				} else if (q_str == NULL) {
					if (p_len >= sizeof(path_buf))
						p_len = sizeof(path_buf) - 1;
					memcpy(path_buf, p_str, p_len);
					path_buf[p_len] = '\0';
					pb_used = p_len;
				} else {
					/* fixes169 (SAFETY_REPORT §3) — bound
					 * path?query writes to path_buf. Truncate
					 * path then query so the assembly fits in
					 * the buffer with room for '?' and NUL. */
					size_t cap = sizeof(path_buf) - 2;
					if (p_len > cap) p_len = cap;
					if (p_len + q_len > cap)
						q_len = cap - p_len;
					memcpy(path_buf, p_str, p_len);
					path_buf[p_len] = '?';
					memcpy(path_buf + p_len + 1, q_str, q_len);
					path_buf[p_len + 1 + q_len] = '\0';
					pb_used = p_len + 1 + q_len;
				}
			}
			if (TEMPLATE_LEN + pb_used + host_len + 1 > sizeof(req)) {
				macsurf_debug_log_writef(
					"mfs_open: REJECT oversize req "
					"pb=%lu host_len=%lu cap=%lu",
					(unsigned long)pb_used,
					(unsigned long)host_len,
					(unsigned long)sizeof(req));
				c->err = "URL too long";
				goto fail_unref;
			}
			macsurf_debug_log_writef("mfs_open: GET (direct) %s %s",
					target, path_buf);
			/* fixes98: direct requests now use keep-alive — the chunked
			 * decoder in mfs_poll_one / process_chunked_bytes frames
			 * Transfer-Encoding: chunked responses correctly so we don't
			 * need the server to close after each. Pool reuse on direct
			 * origins eliminates per-fetch TCP setup latency. */
			sprintf(req,
				"GET %s HTTP/1.1\r\n"
				"Host: %.*s\r\n"
				"User-Agent: MacSurf/0.2\r\n"
				"Accept: */*\r\n"
				"Connection: keep-alive\r\n\r\n",
				path_buf, (int)host_len, host_str);
		}
	}

	r = OTSnd(c->ep, req, (long)strlen(req), 0);
	if (r < 0) {
		macsurf_debug_log_writef("mfs_open: OTSnd err=%ld", (long)r);
		goto fail_unref;
	}
	OTSetNonBlocking(c->ep);

	if (scheme_lwc != NULL) lwc_string_unref(scheme_lwc);
	if (host_lwc != NULL) lwc_string_unref(host_lwc);
	if (port_lwc != NULL) lwc_string_unref(port_lwc);
	if (path_lwc != NULL) lwc_string_unref(path_lwc);
	if (query_lwc != NULL) lwc_string_unref(query_lwc);
	return 1;

fail_unref:
	/* fixes94 — if we opened an endpoint but failed before OTSnd,
	 * close it immediately so mfs_close doesn't try to pool a
	 * half-built connection (pool_key may already be set). */
	if (c->ep != NULL) {
		OTSndOrderlyDisconnect(c->ep);
		OTCloseProvider(c->ep);
		c->ep = NULL;
	}
	c->keep_alive_ok = 0;
	if (scheme_lwc != NULL) lwc_string_unref(scheme_lwc);
	if (host_lwc != NULL) lwc_string_unref(host_lwc);
	if (port_lwc != NULL) lwc_string_unref(port_lwc);
	if (path_lwc != NULL) lwc_string_unref(path_lwc);
	if (query_lwc != NULL) lwc_string_unref(query_lwc);
	return 0;
#else
	return 0;
#endif
}

/* fixes98 — chunked decoder. Drives the chunk_state state machine over
 * a buffer of raw post-header bytes. Emits FETCH_DATA for the data
 * regions; consumes the size lines, CRLFs, and trailer headers without
 * forwarding them. Reentrant across calls — partial chunks are
 * remembered in c->chunk_state / chunk_remaining / chunk_size_buf, so
 * an OTRcv that splits mid-line works correctly on the next pass.
 * When the terminating empty trailer line is seen, sets chunk_state to
 * CS_DONE; mfs_poll_one then transitions state to MFS_DONE. */
static void
process_chunked_bytes(struct macos9_fetch_ctx *c, const char *b, long len)
{
	fetch_msg msg;
	long pos;
	char ch;

	pos = 0;
	while (pos < len && c->chunk_state != CS_DONE) {
		switch (c->chunk_state) {
		case CS_SIZE: {
			while (pos < len) {
				ch = b[pos++];
				if (ch == '\n') {
					long sz;
					c->chunk_size_buf[c->chunk_size_len] = '\0';
					sz = strtol(c->chunk_size_buf, NULL, 16);
					c->chunk_size_len = 0;
					c->chunk_remaining = sz;
					if (sz == 0) {
						c->chunk_state = CS_TRAILER;
						c->trailer_just_after_eol = 1;
					} else {
						c->chunk_state = CS_DATA;
					}
					break;
				}
				if (ch == '\r') continue;
				/* Drop chunk extensions after ';' — stay
				 * inside the buffer so strtol stops at the
				 * first non-hex char anyway, but avoid
				 * overflowing chunk_size_buf on a long ext. */
				if (c->chunk_size_len <
				    (int)sizeof(c->chunk_size_buf) - 1) {
					c->chunk_size_buf[c->chunk_size_len++] = ch;
				}
			}
			break;
		}
		case CS_DATA: {
			long avail;
			long deliver;
			avail = len - pos;
			deliver = (avail < c->chunk_remaining) ? avail : c->chunk_remaining;
			if (deliver > 0) {
				msg.type = FETCH_DATA;
				msg.data.header_or_data.buf = (const uint8_t *)(b + pos);
				msg.data.header_or_data.len = (size_t)deliver;
				fetch_send_callback(&msg, c->parent);
				/* fixes172 — capture chunked-decoded body. */
				cache_capture_append(c, b + pos, deliver);
				c->body_bytes += deliver;
				pos += deliver;
				c->chunk_remaining -= deliver;
			}
			if (c->chunk_remaining == 0) {
				c->chunk_state = CS_CRLF;
			}
			break;
		}
		case CS_CRLF: {
			while (pos < len) {
				ch = b[pos++];
				if (ch == '\n') {
					c->chunk_state = CS_SIZE;
					break;
				}
				/* skip \r and any stray bytes */
			}
			break;
		}
		case CS_TRAILER: {
			while (pos < len) {
				ch = b[pos++];
				if (ch == '\r') continue;
				if (ch == '\n') {
					if (c->trailer_just_after_eol) {
						c->chunk_state = CS_DONE;
						break;
					}
					c->trailer_just_after_eol = 1;
				} else {
					c->trailer_just_after_eol = 0;
				}
			}
			break;
		}
		}
	}
}

static void mfs_parse_headers(struct macos9_fetch_ctx *c) {
	char *sep = strstr(c->h_buf, "\r\n\r\n"), *p, *cur; long cur_len; fetch_msg msg;
	if(!sep) return;
	*sep = 0; cur = c->h_buf; cur_len = (long)(sep - c->h_buf) + 2;
	p = mfs_find_line(&cur, &cur_len);
	if(p && strncmp(p,"HTTP/",5)==0) {
		char *sp=strchr(p,' '); if(sp) c->status=atoi(sp+1);
		/* UNLOCK HISTORY: NetSurf core needs the status line reported as a header */
		msg.type=FETCH_HEADER; msg.data.header_or_data.buf=(const uint8_t*)p;
		msg.data.header_or_data.len=strlen(p); fetch_send_callback(&msg,c->parent);
	}
	fetch_set_http_code(c->parent, c->status);
	while((p = mfs_find_line(&cur, &cur_len)) != NULL) {
		if(p[0]==0) break;
		if(strncasecmp(p,"Content-Type:",13)==0) {
			char *v=p+13; while(*v==' ')v++;
			strncpy(c->mime,v,127); c->mime[127]=0;
		}
		/* fixes91 — parse Content-Length so we know when the body
		 * ends without waiting for the server to close. */
		if(strncasecmp(p,"Content-Length:",15)==0) {
			char *v=p+15; while(*v==' ')v++;
			c->content_length = atol(v);
		}
		if(strncasecmp(p,"Transfer-Encoding:",18)==0) {
			char *v=p+18; while(*v==' ')v++;
			if(strncasecmp(v,"chunked",7)==0) c->chunked = 1;
		}
		if(strncasecmp(p,"Connection:",11)==0) {
			char *v=p+11; while(*v==' ')v++;
			if(strncasecmp(v,"close",5)==0) c->keep_alive_ok = 0;
		}
		/* fixes98 — capture Location: for 3xx auto-follow. */
		if(strncasecmp(p,"Location:",9)==0) {
			char *v=p+9; size_t lv;
			while(*v==' '||*v=='\t')v++;
			lv = strlen(v);
			if (lv >= sizeof(c->redirect_url)) lv = sizeof(c->redirect_url) - 1;
			memcpy(c->redirect_url, v, lv);
			c->redirect_url[lv] = '\0';
		}
		msg.type=FETCH_HEADER; msg.data.header_or_data.buf=(const uint8_t*)p;
		msg.data.header_or_data.len=strlen(p); fetch_send_callback(&msg,c->parent);
	}
	/* fixes98 — 3xx with Location: hand the redirect target to llcache
	 * via FETCH_REDIRECT and stop the body delivery. We don't bother
	 * draining the (usually-tiny "301 Moved Permanently") body because
	 * we're closing the socket — pool reuse on a 3xx isn't worth the
	 * decoder complexity. */
	if (c->status >= 300 && c->status < 400 && c->redirect_url[0] != '\0') {
		struct fetch *parent_save;
		msg.type = FETCH_REDIRECT;
		msg.data.redirect = c->redirect_url;
		fetch_send_callback(&msg, c->parent);
		macsurf_debug_log_writef("http: redirect %d -> %s",
			c->status, c->redirect_url);
		c->state = MFS_NOTIFIED;
		c->keep_alive_ok = 0;
		/* fixes103 — remove from fetch_ring AND free. Both calls
		 * are required (see comment in macos9_http_poll). h_buf
		 * is freed via ops.free; no double-free. Save parent
		 * before fetch_free destroys c. */
		parent_save = c->parent;
		fetch_remove_from_queues(parent_save);
		fetch_free(parent_save);
		return;
	}
	/* fixes98 — keep-alive is now compatible with chunked (the decoder
	 * tells us where the body ends). Only Content-Length-less *plain*
	 * responses force close. */
	if (c->content_length < 0 && !c->chunked) c->keep_alive_ok = 0;
	/* fixes98 — init chunked decoder state. */
	if (c->chunked) {
		c->chunk_state = CS_SIZE;
		c->chunk_size_len = 0;
		c->chunk_remaining = 0;
	}
	/* fixes172 — decide cache eligibility based on the final
	 * status + MIME. cache_capture is allocated lazily on the
	 * first body byte; cache_overflow latches if the body
	 * outgrows MACSURF_CACHE_MAX_BYTES. */
	if (cache_mime_eligible(c->status, c->mime)) {
		c->cache_eligible = 1;
	}
	c->state=MFS_BODY;
	if(sep+4 < c->h_buf+c->h_len) {
		long initial_body = (long)((c->h_buf+c->h_len)-(sep+4));
		if (c->chunked) {
			process_chunked_bytes(c, sep+4, initial_body);
			if (c->chunk_state == CS_DONE) c->state = MFS_DONE;
		} else {
			long deliver = initial_body;
			if (c->content_length >= 0 && c->body_bytes + deliver > c->content_length)
				deliver = c->content_length - c->body_bytes;
			if (deliver > 0) {
				msg.type=FETCH_DATA; msg.data.header_or_data.buf=(const uint8_t*)(sep+4);
				msg.data.header_or_data.len=(size_t)deliver;
				fetch_send_callback(&msg,c->parent);
				/* fixes172 — also capture for the disk cache. */
				cache_capture_append(c, sep+4, deliver);
				c->body_bytes += deliver;
			}
			if (c->content_length >= 0 && c->body_bytes >= c->content_length) {
				c->state = MFS_DONE;
			}
		}
	}
	free(c->h_buf); c->h_buf=NULL;
}

static void mfs_poll_one(struct macos9_fetch_ctx *c) {
#ifdef __MACOS9__
	/* fixes92 — static so we can afford a 32 KB buffer without stack
	 * pressure. macos9_http_poll calls mfs_poll_one sequentially in a
	 * single-threaded loop, and OTRcv in non-blocking mode never yields,
	 * so this is reentrancy-safe under MacSurf's cooperative model. */
	static char b[RECV_B];
	OTResult n; fetch_msg m;
	/* IDLE / NOTIFIED slots are truly inactive — nothing to do. */
	if(c->state==MFS_IDLE || c->state==MFS_NOTIFIED) return;
	/* fixes104 — check abort BEFORE the QUEUED early-return. Previously
	 * an aborted-while-queued fetch (NetSurf calls ops.abort on a
	 * fetch that hasn't yet been dispatched via ops.start — happens
	 * every navigation that has pending sub-resources) was permanently
	 * stranded: state stayed MFS_QUEUED, the outer state-check on the
	 * old line 635 returned early, and the aborted-cleanup branch was
	 * never reached. Those fetches leaked in fetch_ring forever; after
	 * ~3 pages of accumulation the per-host cap (16) tripped and
	 * NetSurf stopped dispatching new fetches. Now: aborted fetches
	 * always force MFS_DONE so the outer poll loop runs the terminal
	 * cleanup (fetch_remove_from_queues + fetch_free) regardless of
	 * which state they were in when abort came in. */
	if(c->aborted) { c->state=MFS_DONE; c->keep_alive_ok=0; return; }
	/* fixes91 — MFS_QUEUED means NetSurf hasn't dispatched yet (ops.start
	 * unfired); don't open OT until then. */
	if(c->state==MFS_QUEUED) return;
	if(c->state==MFS_INIT) { if(mfs_open(c)) c->state=MFS_HEADERS; else c->state=MFS_FAIL; return; }
	/* fixes172 — cache-hit fast path. mfs_open set cache_hit=1 and
	 * populated the body buffer; we deliver headers, the body, and
	 * finished, then transition straight to MFS_DONE without ever
	 * touching OT. */
	if (c->cache_hit && c->cache_hit_body != NULL &&
			c->state == MFS_HEADERS) {
		fetch_msg cm;
		char hline[160];
		long hlen;
		/* Synthetic Content-Type header so NetSurf core sees the
		 * MIME type the cached file was stored with. */
		if (c->cache_hit_mime[0] != '\0') {
			/* mime is capped at 127, "Content-Type: " is 14
			 * chars; hline is 160; sprintf is bounded. */
			sprintf(hline, "Content-Type: %s",
					c->cache_hit_mime);
			hlen = (long)strlen(hline);
			cm.type = FETCH_HEADER;
			cm.data.header_or_data.buf = (const uint8_t *)hline;
			cm.data.header_or_data.len = (size_t)hlen;
			fetch_send_callback(&cm, c->parent);
		}
		fetch_set_http_code(c->parent, c->cache_hit_status);
		/* Deliver the body in one shot. NetSurf's llcache will
		 * stream it onward; the fetch slot is single-pass. */
		cm.type = FETCH_DATA;
		cm.data.header_or_data.buf =
				(const uint8_t *)c->cache_hit_body;
		cm.data.header_or_data.len = (size_t)c->cache_hit_len;
		fetch_send_callback(&cm, c->parent);
		c->body_bytes = c->cache_hit_len;
		c->state = MFS_DONE;
		return;
	}
	n=OTRcv(c->ep,b,sizeof(b),NULL);
	if(n==kOTNoDataErr) return;
	if(n<0) {
		if(n==kOTLookErr) {
			OTResult l=OTLook(c->ep);
			if(l==T_ORDREL||l==T_DISCONNECT) {
				/* Server closed before we hit Content-Length —
				 * treat as done but don't pool. */
				c->keep_alive_ok = 0;
				c->state=MFS_DONE; return;
			}
		}
		c->err="OT error"; c->state=MFS_FAIL; return;
	}
	if(n==0) { c->keep_alive_ok=0; c->state=MFS_DONE; return; }
	/* fixes107 — bytes received, reset no-progress timer. */
	c->progress_ticks = (unsigned long)TickCount();
	if(c->state==MFS_HEADERS) {
		/* fixes169 (SAFETY_REPORT §4) — keep one trailing byte in
		 * h_buf reserved for a NUL terminator. strstr / strlen on
		 * h_buf must see network data terminated cleanly even when
		 * the origin sent no CRLF-CRLF yet. */
		long nl=c->h_len+n;
		if(nl+1>c->h_cap) { long nc=c->h_cap==0?4096:c->h_cap*2; while(nc<nl+1)nc*=2; c->h_buf=realloc(c->h_buf,nc); if(c->h_buf) c->h_cap=nc; }
		if(!c->h_buf) { c->err="OOM"; c->state=MFS_FAIL; return; }
		memcpy(c->h_buf+c->h_len,b,(size_t)n); c->h_len=nl;
		c->h_buf[c->h_len]='\0';
		if(strstr(c->h_buf,"\r\n\r\n")) mfs_parse_headers(c);
	} else {
		if (c->chunked) {
			process_chunked_bytes(c, b, (long)n);
			if (c->chunk_state == CS_DONE) c->state = MFS_DONE;
		} else {
			long deliver = n;
			/* fixes91 — cap delivery at Content-Length so we don't bleed
			 * into the next pipelined response. */
			if (c->content_length >= 0 && c->body_bytes + deliver > c->content_length)
				deliver = c->content_length - c->body_bytes;
			if (deliver > 0) {
				m.type=FETCH_DATA; m.data.header_or_data.buf=(const uint8_t*)b;
				m.data.header_or_data.len=(size_t)deliver;
				fetch_send_callback(&m,c->parent);
				/* fixes172 — capture for disk cache. */
				cache_capture_append(c, b, deliver);
				c->body_bytes += deliver;
			}
			if (c->content_length >= 0 && c->body_bytes >= c->content_length) {
				c->state = MFS_DONE;
			}
		}
	}
#endif
}

static void macos9_http_poll(lwc_string *s) {
	int i;
#ifdef __MACOS9__
	unsigned long now;
#endif
	(void)s;
#ifdef __MACOS9__
	now = (unsigned long)TickCount();
#endif
	for(i=0;i<MAX_F;i++) {
		struct macos9_fetch_ctx *c = &f_slots[i];
		/* fixes104 — only skip TRULY-inactive slots. MFS_QUEUED used
		 * to skip here but must be polled too so aborted-while-queued
		 * fetches can transition to MFS_DONE and get cleaned up.
		 * For non-aborted MFS_QUEUED, mfs_poll_one returns fast. */
		if(c->state==MFS_IDLE || c->state==MFS_NOTIFIED) continue;
#ifdef __MACOS9__
		/* fixes107 — 15-second no-progress timeout. Only active in
		 * MFS_HEADERS / MFS_BODY, i.e. after ops.start has dispatched
		 * and we are waiting on OTRcv. Queued slots are correctly
		 * idle (NetSurf hasn't dispatched yet); MFS_INIT only lasts
		 * the one cycle that calls mfs_open and is impossible to see
		 * between polls. If progress_ticks hasn't advanced in 900
		 * ticks (15s at 60Hz), the proxy / origin has stalled — fail
		 * the slot so the URL bar comes back alive and NetSurf core
		 * hears about the failure rather than waiting silently. */
		if ((c->state == MFS_HEADERS || c->state == MFS_BODY) &&
		    (now - c->progress_ticks) > 900) {
			macsurf_debug_log_writef(
				"http: timeout slot=%d state=%d body=%ld",
				i, (int)c->state, c->body_bytes);
			c->err = "Connection timed out";
			c->state = MFS_FAIL;
			c->keep_alive_ok = 0;
		} else {
			mfs_poll_one(c);
		}
#else
		mfs_poll_one(c);
#endif
		if(c->state==MFS_FAIL) {
			fetch_msg m; m.type=FETCH_ERROR; m.data.error=c->err;
			c->state=MFS_NOTIFIED; fetch_send_callback(&m,c->parent);
			macsurf_debug_log_writef("http: fail body_bytes=%ld status=%d", c->body_bytes, c->status);
			/* fixes103 — both calls, matching every reference
			 * fetcher (file.c:828-829, resource.c:469-470,
			 * about.c:731-732, data.c:314-315, css_fetcher.c,
			 * javascript/fetcher.c, curl.c). fetch_free does NOT
			 * call RING_REMOVE on its own; without
			 * fetch_remove_from_queues the freed struct stays in
			 * fetch_ring as a dangling pointer and the next
			 * RING_GETSIZE walks freed memory. */
			fetch_remove_from_queues(c->parent);
			fetch_free(c->parent);
		}
		else if(c->state==MFS_DONE) {
			fetch_msg m; m.type=FETCH_FINISHED;
			c->state=MFS_NOTIFIED; fetch_send_callback(&m,c->parent);
			macsurf_debug_log_writef("http: done body=%ld len=%ld status=%d ka=%d", c->body_bytes, c->content_length, c->status, c->keep_alive_ok);
			/* fixes172 — write captured body to the disk cache.
			 * cache_eligible is set in mfs_parse_headers when
			 * the response is a cacheable MIME type with
			 * status 200; cache_overflow is set if the body
			 * exceeded MACSURF_CACHE_MAX_BYTES. */
			if (c->cache_eligible && !c->cache_overflow &&
					!c->cache_hit &&
					c->cache_capture != NULL &&
					c->cache_cap_len > 0) {
				const char *u = nsurl_access(c->url);
				if (u != NULL) {
					cache_store(u, c->status, c->mime,
						c->cache_capture,
						c->cache_cap_len);
				}
			}
			/* fixes99 — pool the endpoint right now, while it is
			 * known idle and the next fetch may already be queued. */
#ifdef __MACOS9__
			if (c->keep_alive_ok && !c->aborted &&
			    c->ep != NULL && c->pool_key[0] != '\0') {
				ep_pool_return(c->pool_key, c->ep);
				c->ep = NULL;
			}
#endif
			/* fixes103 — release the fetcher slot completely.
			 * fetch_free alone is NOT enough: it does not remove
			 * the entry from fetch_ring, leaving a dangling
			 * pointer that the next RING_GETSIZE walks as freed
			 * memory. Every reference fetcher (file.c:828-829,
			 * resource.c:469-470, about.c:731-732, data.c:314-315,
			 * css_fetcher.c, javascript/fetcher.c, curl.c) calls
			 * fetch_remove_from_queues FIRST, then fetch_free. Do
			 * the same. After this point c (and c->parent) are
			 * freed memory — don't touch them. */
			fetch_remove_from_queues(c->parent);
			fetch_free(c->parent);
		}
	}
}

static bool macos9_http_initialise(lwc_string *s) { (void)s; return true; }
static void macos9_http_finalise(lwc_string *s) { (void)s; }
static bool macos9_http_acceptable(const struct nsurl *u) { (void)u; return true; }

/* fixes161a — URL-suffix resource classifier. Returns a MACOS9_RC_* enum.
 * Caller is responsible for handling the navigation-hint override before
 * calling this (the pending_document flag wins over suffix). Suffix is
 * inspected against the URL path portion only (everything before '?' or
 * '#'), with case-insensitive matching.
 *
 * fixes161a2 — tightened DOC matching. Apple's /wss/fonts?families=...
 * endpoint was being misclassified as DOCUMENT under the old no-dot
 * fallback, which both gave it a DOC slot reservation and confused
 * downstream layout-aware logic. DOC class now requires either an
 * explicit document extension (.html/.htm/.php/.aspx/.jsp/.shtml) or
 * a URL whose path is exactly "/" or empty (i.e. the bare domain root).
 * Anything else without a recognized extension falls back to OTHER. */
static int macos9_classify_url(struct nsurl *u) {
	const char *s;
	int n, i, last_slash;
	int path_start;
	char c4, c3, c2, c1;
	if (u == NULL) return MACOS9_RC_OTHER;
	s = nsurl_access(u);
	if (s == NULL) return MACOS9_RC_OTHER;
	n = (int)strlen(s);
	for (i = 0; i < n; i++) {
		if (s[i] == '?' || s[i] == '#') { n = i; break; }
	}
	if (n < 1) return MACOS9_RC_OTHER;

	/* 5-char suffixes (.jpeg / .webp / .tiff / .avif / .woff2) */
	if (n >= 6 && s[n-6] == '.') {
		char c5;
		c5 = (char)(s[n-5] | 0x20);
		c4 = (char)(s[n-4] | 0x20);
		c3 = (char)(s[n-3] | 0x20);
		c2 = (char)(s[n-2] | 0x20);
		c1 = (char)(s[n-1] | 0x20);
		if (c5=='w' && c4=='o' && c3=='f' && c2=='f' && c1=='2')
			return MACOS9_RC_FONT;
	}
	if (n >= 5 && s[n-5] == '.') {
		c4 = (char)(s[n-4] | 0x20);
		c3 = (char)(s[n-3] | 0x20);
		c2 = (char)(s[n-2] | 0x20);
		c1 = (char)(s[n-1] | 0x20);
		if (c4=='j' && c3=='p' && c2=='e' && c1=='g') return MACOS9_RC_IMAGE;
		if (c4=='w' && c3=='e' && c2=='b' && c1=='p') return MACOS9_RC_IMAGE;
		if (c4=='t' && c3=='i' && c2=='f' && c1=='f') return MACOS9_RC_IMAGE;
		if (c4=='a' && c3=='v' && c2=='i' && c1=='f') return MACOS9_RC_IMAGE;
		if (c4=='w' && c3=='o' && c2=='f' && c1=='f') return MACOS9_RC_FONT;
		if (c4=='h' && c3=='t' && c2=='m' && c1=='l') return MACOS9_RC_DOCUMENT;
		if (c4=='s' && c3=='h' && c2=='t' && c1=='m') return MACOS9_RC_DOCUMENT;
		if (c4=='a' && c3=='s' && c2=='p' && c1=='x') return MACOS9_RC_DOCUMENT;
	}

	/* 4-char suffixes */
	if (n >= 4 && s[n-4] == '.') {
		c3 = (char)(s[n-3] | 0x20);
		c2 = (char)(s[n-2] | 0x20);
		c1 = (char)(s[n-1] | 0x20);
		if (c3=='j' && c2=='p' && c1=='g') return MACOS9_RC_IMAGE;
		if (c3=='p' && c2=='n' && c1=='g') return MACOS9_RC_IMAGE;
		if (c3=='g' && c2=='i' && c1=='f') return MACOS9_RC_IMAGE;
		if (c3=='b' && c2=='m' && c1=='p') return MACOS9_RC_IMAGE;
		if (c3=='i' && c2=='c' && c1=='o') return MACOS9_RC_IMAGE;
		if (c3=='s' && c2=='v' && c1=='g') return MACOS9_RC_IMAGE;
		if (c3=='t' && c2=='i' && c1=='f') return MACOS9_RC_IMAGE;
		if (c3=='c' && c2=='s' && c1=='s') return MACOS9_RC_CSS;
		if (c3=='m' && c2=='j' && c1=='s') return MACOS9_RC_SCRIPT;
		if (c3=='t' && c2=='t' && c1=='f') return MACOS9_RC_FONT;
		if (c3=='o' && c2=='t' && c1=='f') return MACOS9_RC_FONT;
		if (c3=='e' && c2=='o' && c1=='t') return MACOS9_RC_FONT;
		if (c3=='h' && c2=='t' && c1=='m') return MACOS9_RC_DOCUMENT;
		if (c3=='p' && c2=='h' && c1=='p') return MACOS9_RC_DOCUMENT;
		if (c3=='j' && c2=='s' && c1=='p') return MACOS9_RC_DOCUMENT;
	}

	/* 3-char suffixes (.js) */
	if (n >= 3 && s[n-3] == '.') {
		c2 = (char)(s[n-2] | 0x20);
		c1 = (char)(s[n-1] | 0x20);
		if (c2=='j' && c1=='s') return MACOS9_RC_SCRIPT;
	}

	/* fixes161a2 — bare-domain-root → DOCUMENT, everything else → OTHER.
	 * Find the start of the path (skip past "scheme://host"). If the
	 * remaining path is empty or just "/", classify as DOC. Otherwise
	 * the URL has a real path; without a recognized extension it falls
	 * back to OTHER so endpoints like /wss/fonts, /api/x, /v3/info
	 * don't grab DOC slots. The navigation-hint flag is the source of
	 * truth for the real main document. */
	path_start = -1;
	last_slash = -1;
	for (i = 0; i + 2 < n; i++) {
		if (s[i] == ':' && s[i+1] == '/' && s[i+2] == '/') {
			path_start = i + 3;
			while (path_start < n && s[path_start] != '/') path_start++;
			break;
		}
	}
	if (path_start < 0) {
		/* No scheme://; treat as a relative URL. Use last-slash as
		 * a coarse path-start. */
		for (i = 0; i < n; i++) {
			if (s[i] == '/') last_slash = i;
		}
		if (last_slash < 0) return MACOS9_RC_OTHER;
		path_start = 0;
	}
	if (path_start >= n) return MACOS9_RC_DOCUMENT;
	if (path_start == n - 1 && s[path_start] == '/') return MACOS9_RC_DOCUMENT;
	return MACOS9_RC_OTHER;
}

static const char *macos9_rclass_name(int rc) {
	switch (rc) {
		case MACOS9_RC_DOCUMENT: return "DOC";
		case MACOS9_RC_CSS:      return "CSS";
		case MACOS9_RC_IMAGE:    return "IMG";
		case MACOS9_RC_SCRIPT:   return "SCR";
		case MACOS9_RC_FONT:     return "FNT";
		default:                 return "OTH";
	}
}

static int macos9_rclass_cap(int rc) {
	switch (rc) {
		case MACOS9_RC_DOCUMENT: return MAX_DOC_F;
		case MACOS9_RC_CSS:      return MAX_CSS_F;
		case MACOS9_RC_IMAGE:    return MAX_IMG_F;
		case MACOS9_RC_SCRIPT:   return MAX_SCRIPT_F;
		case MACOS9_RC_FONT:     return MAX_FONT_F;
		default:                 return MAX_OTHER_F;
	}
}

static void macos9_rgov_bump_skip(int rc) {
	extern long macsurf__site_rgov_skip_doc;
	extern long macsurf__site_rgov_skip_css;
	extern long macsurf__site_rgov_skip_img;
	extern long macsurf__site_rgov_skip_script;
	extern long macsurf__site_rgov_skip_font;
	extern long macsurf__site_rgov_skip_other;
	extern long macsurf__site_heavy;
	long total_skipped;
	switch (rc) {
		case MACOS9_RC_DOCUMENT: macsurf__site_rgov_skip_doc++; break;
		case MACOS9_RC_CSS:      macsurf__site_rgov_skip_css++; break;
		case MACOS9_RC_IMAGE:    macsurf__site_rgov_skip_img++; break;
		case MACOS9_RC_SCRIPT:   macsurf__site_rgov_skip_script++; break;
		case MACOS9_RC_FONT:     macsurf__site_rgov_skip_font++; break;
		default:                 macsurf__site_rgov_skip_other++; break;
	}
	/* fixes168f — heavy-mode latch. Trip once any one of the
	 * thresholds is exceeded. Cleared at navigation start by
	 * macsurf_site_navigation_reset(). Consumers read via the
	 * accessor below; future reader-fallback and overlay-rescue
	 * passes will gate on this. */
	if (macsurf__site_rgov_skip_img > 20) macsurf__site_heavy = 1;
	if (macsurf__site_rgov_skip_script > 10) macsurf__site_heavy = 1;
	total_skipped = macsurf__site_rgov_skip_img
		+ macsurf__site_rgov_skip_script
		+ macsurf__site_rgov_skip_other
		+ macsurf__site_rgov_skip_font;
	if (total_skipped > 30) macsurf__site_heavy = 1;
}

/* fixes168f — public accessor. Returns 1 once the current page has
 * tripped any heavy-mode threshold, 0 otherwise. Stable across calls;
 * resets only when macsurf_site_navigation_reset() is invoked. */
int macsurf_site_is_heavy(void) {
	extern long macsurf__site_heavy;
	return (macsurf__site_heavy != 0) ? 1 : 0;
}

/* fixes168f — navigation reset. Clears the per-page heavy latch and
 * the resource-governor skip counters so the next page starts from a
 * clean slate. Wire into the navigation entry point (window.c) so
 * heavy mode is per-page, not process-cumulative. */
void macsurf_site_navigation_reset(void) {
	extern long macsurf__site_heavy;
	extern long macsurf__site_rgov_skip_doc;
	extern long macsurf__site_rgov_skip_css;
	extern long macsurf__site_rgov_skip_img;
	extern long macsurf__site_rgov_skip_script;
	extern long macsurf__site_rgov_skip_font;
	extern long macsurf__site_rgov_skip_other;
	macsurf__site_heavy = 0;
	macsurf__site_rgov_skip_doc = 0;
	macsurf__site_rgov_skip_css = 0;
	macsurf__site_rgov_skip_img = 0;
	macsurf__site_rgov_skip_script = 0;
	macsurf__site_rgov_skip_font = 0;
	macsurf__site_rgov_skip_other = 0;
}

static void *macos9_http_setup(struct fetch *p, struct nsurl *u, bool o, bool d, const char *pu, const struct fetch_multipart_data *pm, const char **h) {
	int i;
	int rc;
	int class_active;
	int global_active;
	int slot_index = -1;
	(void)o;(void)d;(void)pu;(void)pm;(void)h;

	/* fixes161a — classify. Navigation-hint flag wins over URL suffix
	 * (top-level docs that look like /api/x or /foo.png still classify
	 * as DOCUMENT). The flag is single-shot: consumed by the first
	 * setup() call after the navigation. */
	if (macos9_http_pending_document) {
		rc = MACOS9_RC_DOCUMENT;
		macos9_http_pending_document = 0;
	} else {
		rc = macos9_classify_url(u);
	}

	/* Count active slots for governor cap checks. One pass tallies both
	 * the per-class active count and the global active count. */
	class_active = 0;
	global_active = 0;
	for (i = 0; i < MAX_F; i++) {
		if (f_slots[i].state == MFS_IDLE) continue;
		global_active++;
		if (f_slots[i].rclass == rc) class_active++;
	}

	/* fixes161a2 — only IMAGE class is safe to refuse via NULL. Refusing
	 * DOC/CSS/SCRIPT/FONT/OTHER setups causes NetSurf to treat the load
	 * as a critical fetch failure and the browser_window switches to
	 * about:query/fetcherror. For non-IMG classes we still log a
	 * "throttle" advisory so the cap pressure is visible in the log,
	 * but we let the setup go through and allocate a slot anyway.
	 * Real queueing (proper deferral with retry) is a V2 follow-up.
	 *
	 * IMG refusal is fine: NetSurf handles a NULL image fetch as a
	 * broken-image placeholder, no fetcherror escalation. */
	if (rc == MACOS9_RC_IMAGE) {
		if (global_active >= MAX_GLOBAL_F ||
		    class_active >= MAX_IMG_F) {
			macos9_rgov_bump_skip(rc);
			macsurf_debug_log_writef(
				"RGOV class=IMG action=defer "
				"global=%d/%d class=%d/%d",
				global_active, MAX_GLOBAL_F,
				class_active, MAX_IMG_F);
			return NULL;
		}
	} else {
		int cap;
		cap = macos9_rclass_cap(rc);
		if (class_active >= cap) {
			macsurf_debug_log_writef(
				"RGOV class=%s action=throttle "
				"active=%d cap=%d "
				"(non-IMG classes never refused)",
				macos9_rclass_name(rc), class_active, cap);
		}
	}

	/* Cap checks passed — allocate a slot from the array. */
	for (i = 0; i < MAX_F; i++) {
		if (f_slots[i].state == MFS_IDLE) { slot_index = i; break; }
	}
	if (slot_index < 0) {
		extern long macsurf__site_fetch_slot_fail;
		macsurf__site_fetch_slot_fail++;
		macsurf_debug_log_writef(
			"http_setup: NO FREE SLOTS (MAX_F=%d) - fetch will FAIL",
			MAX_F);
		return NULL;
	}
	memset(&f_slots[slot_index], 0, sizeof(f_slots[0]));
	f_slots[slot_index].parent = p;
	f_slots[slot_index].url = nsurl_ref(u);
	f_slots[slot_index].state = MFS_QUEUED;
	f_slots[slot_index].content_length = -1;
	f_slots[slot_index].keep_alive_ok = 1;
	f_slots[slot_index].rclass = rc;
	/* fetch_active_peak: high-water-mark across the page load.
	 * +1 because this allocation hasn't been counted yet. */
	{
		extern long macsurf__site_fetch_active_peak;
		if ((long)(global_active + 1) > macsurf__site_fetch_active_peak) {
			macsurf__site_fetch_active_peak = (long)(global_active + 1);
		}
	}
	macsurf_debug_log_writef(
		"RGOV class=%s action=fetch slot=%d global=%d class=%d",
		macos9_rclass_name(rc), slot_index,
		global_active + 1, class_active + 1);
	return &f_slots[slot_index];
}
static bool macos9_http_start(void *ctx) {
	struct macos9_fetch_ctx *c = (struct macos9_fetch_ctx*)ctx;
	/* fixes91 — transition QUEUED→INIT so mfs_poll_one will open OT. */
	if (c != NULL && c->state == MFS_QUEUED) {
		c->state = MFS_INIT;
#ifdef __MACOS9__
		/* fixes107 — stamp baseline for no-progress timeout. Set here
		 * (not at setup) so time spent queued waiting for NetSurf's
		 * fetch_dispatch_jobs doesn't count against the 15s budget. */
		c->progress_ticks = (unsigned long)TickCount();
#endif
	}
	return true;
}
static void macos9_http_abort(void *ctx) { ((struct macos9_fetch_ctx*)ctx)->aborted=1; }
static void macos9_http_free(void *ctx) {
	struct macos9_fetch_ctx *c = (struct macos9_fetch_ctx*)ctx;
	mfs_close(c);
	if (c->h_buf) free(c->h_buf);
	if (c->url) nsurl_unref(c->url);
	/* fixes172 — release cache state. */
	if (c->cache_capture) { free(c->cache_capture); c->cache_capture = NULL; }
	c->cache_cap_len = 0;
	c->cache_cap_cap = 0;
	c->cache_overflow = 0;
	c->cache_eligible = 0;
	if (c->cache_hit_body) { free(c->cache_hit_body); c->cache_hit_body = NULL; }
	c->cache_hit_len = 0;
	c->cache_hit = 0;
	c->cache_hit_mime[0] = '\0';
	c->cache_hit_status = 0;
	c->state = MFS_IDLE;
}

nserror macos9_http_fetcher_register(void) {
	struct fetcher_operation_table ops; lwc_string *sh, *ss;
	ops.initialise=macos9_http_initialise; ops.acceptable=macos9_http_acceptable;
	ops.setup=macos9_http_setup; ops.start=macos9_http_start; ops.abort=macos9_http_abort;
	ops.free=macos9_http_free; ops.poll=macos9_http_poll; ops.fdset=NULL; ops.finalise=macos9_http_finalise;
	lwc_intern_string("http",4,&sh); lwc_intern_string("https",5,&ss);
	fetcher_add(sh,&ops); fetcher_add(ss,&ops); return NSERROR_OK;
}
