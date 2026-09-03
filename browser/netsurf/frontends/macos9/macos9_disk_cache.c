/*
 * MacSurf - macos9_disk_cache.c
 *
 * Shared persistent body cache for the HTTP + HTTPS fetchers.
 * Extracted from macos9_http_fetcher.c (fixes172) so the HTTPS fetcher
 * can hit the same on-disk store. See macos9_disk_cache.h for the
 * public API.
 *
 * Disk layout:
 *   Folder: "MacSurf Cache" on the boot Desktop (auto-created).
 *   File:   one per cached body, name = "h_xxxxxxxx" (FNV-1a hash of URL).
 *   Format: [8 bytes ] magic 'MSCACHE\x01'
 *           [4 bytes ] HTTP status (BE)
 *           [4 bytes ] mime length (BE)
 *           [4 bytes ] body length (BE)
 *           [4 bytes ] reserved (zero)
 *           [N bytes ] mime string (no NUL)
 *           [M bytes ] body
 *
 * Cap any single cached body at MACSURF_CACHE_MAX_BYTES. Bigger
 * responses are still served live, just not cached.
 */

#undef malloc

#include "macos9_disk_cache.h"
#include "macsurf_debug.h"

#include <string.h>
#include <stdlib.h>

#ifdef __MACOS9__
#include <Files.h>
#include <Folders.h>
#include <Script.h>
#include <Types.h>
#include <Processes.h>
#endif

/* main.c -- heap-state probes (fixes366j). */
extern long macos9_heap_max_block(void);

#define MACSURF_CACHE_MAGIC0 'M'
#define MACSURF_CACHE_MAGIC1 'S'
#define MACSURF_CACHE_MAGIC2 'C'
#define MACSURF_CACHE_MAGIC3 'A'
#define MACSURF_CACHE_MAGIC4 'C'
#define MACSURF_CACHE_MAGIC5 'H'
#define MACSURF_CACHE_MAGIC6 'E'
#define MACSURF_CACHE_MAGIC7 0x01

/* fixes665: ioDirMask (0x10 bit of ioFlAttrib = "this catalog entry is a
 * directory") is standard in Files.h; define defensively per the CW8
 * missing-constant pattern so the LRU sweep's dir-skip always compiles. */
#ifndef ioDirMask
#define ioDirMask 0x10
#endif

/* fixes181 - Reload sets this to 1 so the next lookup short-circuits
 * to miss. Cleared after a successful store so sub-resources resume
 * normal cache behaviour. */
int macsurf_http_skip_next_cache = 0;

/* ---- fixes1159 (#240): per-host POST-staleness window ---- */

/* A POST makes the same-origin disk cache suspect: the classic forum flow
 * POSTs an edit then 302-redirects back to the thread page, whose cached
 * copy predates the edit. Serving that copy hides the edit, and the next
 * edit then re-derives from the stale base (data loss). So every POST arms
 * a short "no disk cache for this origin" window: host-scoped (the redirect
 * GET to the same host must bypass), time-bounded (a few seconds - TLS
 * handshakes and server-side edit processing still fit inside), refreshed
 * on every POST send.
 *
 * Deliberately NOT the bare macsurf_http_skip_next_cache global: that
 * one-shot is Reload / URL-bar / login semantics - global across ALL hosts
 * and cleared by the first store. POST staleness needs per-origin scope
 * that also survives the POST fetch's teardown (the 302 target is a NEW
 * fetch in a NEW slot, so any state carried on the POST's own ctx would
 * be lost), so it lives here as a small fixed table shared by both
 * fetchers. */
#define MACSURF_SKIPCACHE_WINDOW_TICKS (5 * 60) /* 5 s at 60 Hz */

#define MACSURF_SKIPCACHE_ORIGINS 8

struct macsurf_post_bypass {
	unsigned long deadline;   /* TickCount() deadline; 0 = free slot */
	char origin[64];          /* lowercased host[:non-default-port] */
};
static struct macsurf_post_bypass g_post_bypass[MACSURF_SKIPCACHE_ORIGINS];

static unsigned long macsurf_cache_now(void)
{
#ifdef __MACOS9__
	return (unsigned long)TickCount();
#else
	/* Non-Mac builds (Linux syntax check / harness): no TickCount, so a
	 * monotonically increasing counter keeps the window logic alive. */
	static unsigned long n = 0;
	return ++n;
#endif
}

/* Extract the origin key - "host[:port]", lowercased, scheme-default
 * port dropped - from a URL string. Returns 1 on success, 0 when the
 * URL has no usable host (about:/data:/scheme-less etc.). Keys match
 * whenever a POST and its follow-up GET are to the same origin,
 * regardless of explicit vs default port spelling. */
static int macsurf_origin_from_url(const char *url, char *out, size_t cap)
{
	const char *p, *h, *e, *q;
	size_t n;
	int default_port;

	if (url == NULL || cap == 0) return 0;
	if (strncmp(url, "https://", 8) == 0) {
		p = url + 8; default_port = 443;
	} else if (strncmp(url, "http://", 7) == 0) {
		p = url + 7; default_port = 80;
	} else {
		p = strstr(url, "://");
		p = (p != NULL) ? p + 3 : url;
		default_port = 0;
	}
	h = p;
	e = h;
	while (*e != '\0' && *e != '/' && *e != '?' && *e != '#' &&
	       *e != ':') {
		e++;
	}
	n = (size_t)(e - h);
	if (n == 0) { out[0] = '\0'; return 0; }
	if (n >= cap) n = cap - 1;   /* absurdly long host: truncate */
	for (p = h; p < h + n; p++) {
		out[p - h] = (char)((*p >= 'A' && *p <= 'Z') ?
				(*p - 'A' + 'a') : *p);
	}
	out[n] = '\0';
	/* Optional ":port" - kept only when it is not the scheme default,
	 * so "http://h/x" and "http://h:80/x" key alike. */
	if (e[0] == ':' && n + 8 < cap) {
		long portv = 0;
		size_t i;
		q = e + 1;
		while (*q >= '0' && *q <= '9' &&
		       (size_t)(q - (e + 1)) < 5) {
			q++;
		}
		if (q > e + 1) {
			for (i = 0; i < (size_t)(q - (e + 1)); i++) {
				portv = portv * 10 + (e[1 + i] - '0');
			}
			if (portv != (long)default_port) {
				out[n++] = ':';
				for (i = 0; i < (size_t)(q - (e + 1)); i++) {
					out[n++] = e[1 + i];
				}
				out[n] = '\0';
			}
		}
	}
	return 1;
}

/* Arm (or refresh) the bypass window for the origin of url. Called by both
 * fetchers when a POST fetch is set up and again when the POST body hits
 * the wire, so the deadline counts from transmission, not from queuing. */
void macos9_cache_arm_post_bypass(const char *url)
{
	char origin[64];
	unsigned long deadline;
	int i, slot = -1;

	if (!macsurf_origin_from_url(url, origin, sizeof origin)) return;
	deadline = macsurf_cache_now() + MACSURF_SKIPCACHE_WINDOW_TICKS;
	for (i = 0; i < MACSURF_SKIPCACHE_ORIGINS; i++) {
		if (g_post_bypass[i].deadline != 0 &&
		    strcmp(g_post_bypass[i].origin, origin) == 0) {
			g_post_bypass[i].deadline = deadline;
			return;
		}
	}
	for (i = 0; i < MACSURF_SKIPCACHE_ORIGINS; i++) {
		if (g_post_bypass[i].deadline == 0) { slot = i; break; }
	}
	if (slot < 0) slot = 0;   /* table full: oldest slot wins */
	strcpy(g_post_bypass[slot].origin, origin);
	g_post_bypass[slot].deadline = deadline;
}

/* Returns 1 when an unexpired POST-bypass window covers url's origin. */
int macos9_cache_post_bypass_active(const char *url)
{
	char origin[64];
	unsigned long now;
	int i;

	if (!macsurf_origin_from_url(url, origin, sizeof origin)) return 0;
	now = macsurf_cache_now();
	for (i = 0; i < MACSURF_SKIPCACHE_ORIGINS; i++) {
		if (g_post_bypass[i].deadline != 0 &&
		    strcmp(g_post_bypass[i].origin, origin) == 0) {
			/* Wrap-safe: the deadline is at most WINDOW ticks
			 * ahead of the now it was computed from, so the
			 * modular difference is <= WINDOW exactly while
			 * inside the window (and astronomically larger
			 * after it - 32-bit TickCount wraps every ~828 days
			 * and a stale deadline near the wrap reads as far
			 * in the future, which merely costs one extra
			 * network fetch, never a wrong cache hit). */
			if ((unsigned long)(g_post_bypass[i].deadline - now) <=
			    (unsigned long)MACSURF_SKIPCACHE_WINDOW_TICKS) {
				return 1;
			}
			g_post_bypass[i].deadline = 0;   /* expired */
		}
	}
	return 0;
}

/* ---- file-local helpers ---- */

/* FNV-1a 32-bit. Cheap, well-distributed, no allocation. */
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

/* Pack URL hash into a 10-char Pascal-string filename. fname is
 * at least 32 bytes. Format: "h_xxxxxxxx". */
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

#ifdef __MACOS9__
/* Process-lifetime directory memoization. HFS directory IDs are stable for
 * the life of the app, and resolving MacSurfData/Cache used to repeat
 * GetProcessInformation + FSMakeFSSpec + PBGetCatInfoSync for every cache
 * hit/store. Keep the resolved IDs after the first successful lookup. */
static int g_data_dir_cached = 0;
static short g_data_dir_vref = 0;
static long g_data_dir_id = 0;
static int g_cache_dir_cached = 0;
static short g_cache_dir_vref = 0;
static long g_cache_dir_id = 0;

/* Build an FSSpec directly when the parent directory and Pascal leaf name
 * are already known. FSSpec contains exactly those fields; calling
 * FSMakeFSSpec here would add a redundant catalog lookup before the real
 * open/create/delete operation on every cache hit and store. */
static void macos9_spec_from_pname(short vRef, long dirID,
		const unsigned char *name, FSSpec *spec)
{
	memset(spec, 0, sizeof(*spec));
	spec->vRefNum = vRef;
	spec->parID = dirID;
	memcpy(spec->name, name, name[0] + 1);
}

static void macos9_spec_from_cname(short vRef, long dirID,
		const char *name, FSSpec *spec)
{
	unsigned char pname[32];
	size_t nlen;

	nlen = strlen(name);
	if (nlen > 31) nlen = 31;
	pname[0] = (unsigned char)nlen;
	memcpy(pname + 1, name, nlen);
	macos9_spec_from_pname(vRef, dirID, pname, spec);
}

/* fixes641 (#197): resolve the RUNNING APPLICATION's own directory (the folder
 * MacSurf.app lives in), so the cache + log go next to the app rather than the
 * boot volume's Desktop. Uses the app's own FSSpec from GetProcessInformation.
 * Returns noErr + the app's parent (vRefNum, dirID). */
static OSErr macos9_app_dir_get(short *vRef, long *dirID)
{
	ProcessSerialNumber psn;
	ProcessInfoRec      info;
	FSSpec              appSpec;
	OSErr               err;

	psn.highLongOfPSN = 0;
	psn.lowLongOfPSN  = kCurrentProcess;
	memset(&info, 0, sizeof(info));
	info.processInfoLength = sizeof(ProcessInfoRec);
	info.processName = NULL;
	info.processAppSpec = &appSpec;
	err = GetProcessInformation(&psn, &info);
	if (err != noErr) return err;
	/* appSpec identifies the application file; its parent (vRefNum, parID)
	 * is the folder the app lives in. */
	*vRef = appSpec.vRefNum;
	*dirID = appSpec.parID;
	return noErr;
}

/* Ensure a subfolder `name` exists under (base_vref, base_dir); return its
 * (vRefNum, dirID). Creates it if missing. */
static OSErr ensure_subdir(short base_vref, long base_dir, const char *name,
		short *vRef, long *dirID)
{
	OSErr err;
	FSSpec spec;
	unsigned char fname[32];
	size_t nlen;
	long new_dir;

	nlen = strlen(name);
	if (nlen > 31) nlen = 31;
	fname[0] = (unsigned char)nlen;
	memcpy(fname + 1, name, nlen);

	err = FSMakeFSSpec(base_vref, base_dir, fname, &spec);
	if (err == fnfErr) {
		err = FSpDirCreate(&spec, smSystemScript, &new_dir);
		if (err != noErr) return err;
		err = FSMakeFSSpec(base_vref, base_dir, fname, &spec);
		if (err != noErr) return err;
	} else if (err != noErr) {
		return err;
	}

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

/* Resolve <app-or-Desktop>/MacSurfData[/subfolder], creating folders as
 * needed. subfolder == NULL returns the MacSurfData folder itself.
 *
 * fixes647 (#197): consolidate everything MacSurf writes under ONE
 * "MacSurfData" folder next to the app - Cache/ and Downloads/ subfolders,
 * plus the Bookmarks and log files at its root. Two reasons: (1) users no
 * longer face a scatter of "MacSurf *" folders beside the app, and (2)
 * bookmarks live OUTSIDE Cache/, so clearing the cache can't delete them. */
static OSErr macsurfdata_dir_get(const char *subfolder,
		short *vRef, long *dirID)
{
	OSErr err;
	short base_vref;
	long base_dir;
	short desk_vref;
	long desk_dir;
	short data_vref;
	long data_dir;

	if (subfolder == NULL && g_data_dir_cached) {
		*vRef = g_data_dir_vref;
		*dirID = g_data_dir_id;
		return noErr;
	}
	if (subfolder != NULL && strcmp(subfolder, "Cache") == 0 &&
	    g_cache_dir_cached) {
		*vRef = g_cache_dir_vref;
		*dirID = g_cache_dir_id;
		return noErr;
	}

	if (g_data_dir_cached) {
		data_vref = g_data_dir_vref;
		data_dir = g_data_dir_id;
	} else {
		err = macos9_app_dir_get(&base_vref, &base_dir);
		if (err != noErr) {
			/* fixes680 (#207): DIAG. app-dir resolution failed -> Desktop. */
			macsurf_debug_log_writef("DIAG appdir FAIL err=%d (Desktop fallback)",
				(int)err);
			/* Fallback: boot-volume Desktop. */
			err = FindFolder(kOnSystemDisk, kDesktopFolderType,
					kDontCreateFolder, &desk_vref, &desk_dir);
			if (err != noErr) {
				macsurf_debug_log_writef("DIAG Desktop-fallback FAIL err=%d",
					(int)err);
				return err;
			}
			base_vref = desk_vref;
			base_dir = desk_dir;
		}
		err = ensure_subdir(base_vref, base_dir, "MacSurfData",
				&data_vref, &data_dir);
		if (err != noErr) {
			/* fixes680 (#207): DIAG. MacSurfData folder create/resolve failed
			 * - this would also break the log if it lives here, and cascade. */
			macsurf_debug_log_writef("DIAG MacSurfData FAIL err=%d", (int)err);
			return err;
		}
		g_data_dir_vref = data_vref;
		g_data_dir_id = data_dir;
		g_data_dir_cached = 1;
	}
	if (subfolder == NULL) {
		*vRef = data_vref;
		*dirID = data_dir;
		return noErr;
	}
	{
		OSErr serr = ensure_subdir(data_vref, data_dir, subfolder,
				vRef, dirID);
		if (serr != noErr) {
			macsurf_debug_log_writef("DIAG subdir '%s' FAIL err=%d",
				subfolder, (int)serr);
		} else if (strcmp(subfolder, "Cache") == 0) {
			g_cache_dir_vref = *vRef;
			g_cache_dir_id = *dirID;
			g_cache_dir_cached = 1;
		}
		return serr;
	}
}

/* Public wrapper so other TUs (the debug log) share the same MacSurfData
 * root. subfolder == NULL => the MacSurfData folder itself. */
OSErr macos9_data_dir_get(const char *subfolder, short *vRef, long *dirID)
{
	return macsurfdata_dir_get(subfolder, vRef, dirID);
}

/* Cache folder = MacSurfData/Cache. */
static OSErr cache_dir_get(short *vRef, long *dirID)
{
	return macsurfdata_dir_get("Cache", vRef, dirID);
}
#endif /* __MACOS9__ */

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

/* ---- public API ---- */

int macos9_cache_mime_eligible(int status, const char *mime)
{
	if (status != 200) return 0;
	if (mime == NULL || mime[0] == '\0') return 0;
	if (strncmp(mime, "text/html", 9) == 0) return 1;
	if (strncmp(mime, "text/css", 8) == 0) return 1;
	if (strncmp(mime, "text/plain", 10) == 0) return 1;
	if (strncmp(mime, "application/xhtml", 17) == 0) return 1;
	if (strncmp(mime, "application/javascript", 22) == 0) return 1;
	if (strncmp(mime, "application/json", 16) == 0) return 1;
	/* fixes985 - images and downloadable webfonts are cacheable again.
	 *
	 * fixes665 added them; fixes679 took them out again while #207 (the
	 * blank screen) was being chased, as one of several suspects eliminated
	 * at once. #207's real cause turned out to be somewhere else entirely --
	 * hardcoded pointer-ceiling guards rejecting valid pointers when the
	 * partition maps high (fixes716-719) -- so the reason for the revert has
	 * not applied for a long time, and the cost of keeping it has now been
	 * measured: 228 image retrievals in a nine-navigation session, none of
	 * them able to survive a relaunch, on a 400 MHz machine over TLS.
	 *
	 * These are the ideal candidates: large, static, and unchanged for
	 * months. The bound that makes it safe is the directory budget below,
	 * which came out with them in fixes679 and comes back with them here. */
#if MACSURF_CACHE_IMAGES
	if (strncmp(mime, "image/", 6) == 0) return 1;
	if (strncmp(mime, "font/", 5) == 0) return 1;
	if (strncmp(mime, "application/font", 16) == 0) return 1;
	if (strncmp(mime, "application/x-font", 18) == 0) return 1;
	if (strncmp(mime, "application/vnd.ms-fontobject", 29) == 0) return 1;
#endif
	return 0;
}

/* Persistent total + LRU budget. The old path performed a full directory
 * sweep on the first store after every launch. On a large HFS cache that is
 * seconds of synchronous PBGetCatInfo work in the middle of a page load.
 *
 * The current total is persisted in a tiny state file at the MacSurfData root
 * and updated from exact old/new file sizes. Existing installs have no state
 * file; migration starts untrusted at zero and counts new writes without
 * subtracting overwritten legacy files. Once that conservative counter
 * reaches the 64 MB budget, one real sweep establishes the exact total and
 * marks the state trusted. Thus an old <=64 MB cache can temporarily grow to
 * roughly <=128 MB during migration, but the first-page launch stall is gone
 * and subsequent launches start with an O(1) state read rather than a scan. */
#define CACHE_TOTAL_BUDGET   (64L * 1024L * 1024L)
#define CACHE_EVICT_BATCH    64
#define CACHE_TOTAL_STATE_SAVE_EVERY 16

static long g_cache_total = -1;
static int g_cache_total_loaded = 0;
static int g_cache_total_trusted = 0;
static int g_cache_total_dirty = 0;
static long g_store_ticks = 0;
static long g_store_n = 0;

#ifdef __MACOS9__
static void cache_total_state_load(void)
{
	short vRef;
	long dirID;
	FSSpec spec;
	short ref = 0;
	unsigned char buf[12];
	long count;
	unsigned long total_v;
	unsigned long flags_v;

	if (g_cache_total_loaded) return;
	g_cache_total_loaded = 1;
	g_cache_total = 0;
	g_cache_total_trusted = 0;
	g_cache_total_dirty = 0;

	if (macsurfdata_dir_get(NULL, &vRef, &dirID) != noErr) return;
	macos9_spec_from_cname(vRef, dirID, "MacSurf CacheState", &spec);
	if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return;
	count = (long)sizeof(buf);
	if (FSRead(ref, &count, buf) != noErr || count != (long)sizeof(buf)) {
		FSClose(ref);
		return;
	}
	FSClose(ref);
	if (buf[0] != 'M' || buf[1] != 'C' || buf[2] != 'T' || buf[3] != '1')
		return;
	total_v = cache_read_be32(buf + 4);
	flags_v = cache_read_be32(buf + 8);
	if (total_v > 0x7fffffffUL) return;
	g_cache_total = (long)total_v;
	g_cache_total_trusted = (flags_v & 1UL) ? 1 : 0;
}

static void cache_total_state_save(void)
{
	short vRef;
	long dirID;
	FSSpec spec;
	short ref = 0;
	unsigned char buf[12];
	long count;
	OSErr err;

	if (!g_cache_total_loaded || g_cache_total < 0) return;
	if (macsurfdata_dir_get(NULL, &vRef, &dirID) != noErr) return;
	macos9_spec_from_cname(vRef, dirID, "MacSurf CacheState", &spec);
	err = FSpOpenDF(&spec, fsRdWrPerm, &ref);
	if (err == fnfErr) {
		err = FSpCreate(&spec, 'MPLS', 'DATA', smSystemScript);
		if (err != noErr) return;
		err = FSpOpenDF(&spec, fsRdWrPerm, &ref);
	}
	if (err != noErr) return;
	buf[0] = 'M'; buf[1] = 'C'; buf[2] = 'T'; buf[3] = '1';
	cache_write_be32(buf + 4, (unsigned long)g_cache_total);
	cache_write_be32(buf + 8, g_cache_total_trusted ? 1UL : 0UL);
	(void)SetFPos(ref, fsFromStart, 0);
	count = (long)sizeof(buf);
	if (FSWrite(ref, &count, buf) == noErr && count == (long)sizeof(buf)) {
		(void)SetEOF(ref, (long)sizeof(buf));
		g_cache_total_dirty = 0;
	}
	FSClose(ref);
}
#endif

static long macos9_cache_sweep(void)
{
#ifdef __MACOS9__
	/* One directory pass: sum the cache and keep the CACHE_EVICT_BATCH
	 * oldest files (ascending by modification date); if over budget, delete
	 * oldest-first until back under. Bounded to one scan and at most BATCH
	 * deletes per call, so a very large legacy cache converges over several
	 * sweeps instead of stalling. Statics keep the batch arrays off the
	 * stack. Returns the resulting total. */
	static Str63 names[CACHE_EVICT_BATCH];
	static unsigned long dats[CACHE_EVICT_BATCH];
	static long sizes[CACHE_EVICT_BATCH];
	CInfoPBRec pb;
	Str63 nm;
	short vRef, idx;
	long dirID, total = 0;
	int nk = 0, k, j;
	int evicted = 0;

	if (cache_dir_get(&vRef, &dirID) != noErr) return 0;
	for (idx = 1; ; idx++) {
		unsigned long d;
		long sz;
		memset(&pb, 0, sizeof(pb));
		pb.hFileInfo.ioNamePtr = nm;
		pb.hFileInfo.ioVRefNum = vRef;
		pb.hFileInfo.ioDirID = dirID;
		pb.hFileInfo.ioFDirIndex = idx;
		if (PBGetCatInfoSync(&pb) != noErr) break;
		if (pb.hFileInfo.ioFlAttrib & ioDirMask) continue;
		/* Only body-cache files count against the body-cache budget. */
		if (nm[0] < 2 || nm[1] != 'h' || nm[2] != '_') continue;
		sz = pb.hFileInfo.ioFlLgLen;
		d = (unsigned long)pb.hFileInfo.ioFlMdDat;
		total += sz;
		if (nk < CACHE_EVICT_BATCH) {
			k = nk++;
		} else if (d < dats[CACHE_EVICT_BATCH - 1]) {
			k = CACHE_EVICT_BATCH - 1;
		} else {
			continue;
		}
		while (k > 0 && dats[k - 1] > d) {
			dats[k] = dats[k - 1];
			sizes[k] = sizes[k - 1];
			memcpy(names[k], names[k - 1], names[k - 1][0] + 1);
			k--;
		}
		dats[k] = d;
		sizes[k] = sz;
		memcpy(names[k], nm, nm[0] + 1);
	}
	if (total <= CACHE_TOTAL_BUDGET) return total;
	for (j = 0; j < nk && total > CACHE_TOTAL_BUDGET; j++) {
		FSSpec spec;
		macos9_spec_from_pname(vRef, dirID, names[j], &spec);
		if (FSpDelete(&spec) == noErr) {
			total -= sizes[j];
			evicted++;
		}
	}
	macsurf_debug_log_writef(
		"LIFE CACHE sweep evicted=%d total=%ld budget=%ld",
		evicted, total, (long)CACHE_TOTAL_BUDGET);
	return total;
#else
	return 0;
#endif
}

#ifdef __MACOS9__
static void cache_total_replace(long old_size, long new_size)
{
	long delta;

	if (old_size < 0) old_size = 0;
	if (new_size < 0) new_size = 0;
	cache_total_state_load();
	if (g_cache_total_trusted) {
		delta = new_size - old_size;
	} else {
		/* Migration from pre-state builds: old files are not represented in
		 * the persisted total, so never subtract them until a sweep has made
		 * the total authoritative. */
		delta = new_size;
	}
	if (delta < 0 && -delta > g_cache_total) {
		g_cache_total = 0;
	} else {
		g_cache_total += delta;
	}
	if (g_cache_total > CACHE_TOTAL_BUDGET) {
		g_cache_total = macos9_cache_sweep();
		g_cache_total_trusted = 1;
		g_cache_total_dirty = 0;
		cache_total_state_save();
		return;
	}
	g_cache_total_dirty++;
	if (g_cache_total_dirty >= CACHE_TOTAL_STATE_SAVE_EVERY)
		cache_total_state_save();
}

static void cache_total_reset(void)
{
	g_cache_total_loaded = 1;
	g_cache_total = 0;
	g_cache_total_trusted = 1;
	g_cache_total_dirty = 0;
	cache_total_state_save();
}
#endif

/* fixes981 - the freshness/validator headers a disk hit must carry.
 *
 * A disk hit used to hand core ONLY a Content-Type, so llcache had no Date,
 * no Cache-Control, no ETag and no Last-Modified for it. Two consequences,
 * both measured: the cached copy was served unconditionally and indefinitely
 * (nothing could ever mark it stale), and because there was no validator it
 * could never be revalidated cheaply either -- hardware showed reval=14 with
 * cond=0, i.e. fourteen objects needed checking and not one had anything to
 * check WITH. Persisting these six lines is what lets llcache make the
 * decision instead of the disk cache making it by omission.
 *
 * Exactly the set llcache_fetch_process_header consumes (llcache.c:793-846):
 * Age, Date, ETag, Expires, Cache-Control, Last-Modified.
 *
 * fixes1297 (#167, Track B) - added Vary. Not consumed by
 * llcache_fetch_process_header, but macos9_cache_response_persistable()
 * (macos9_tls_fetcher.c) needs it to decide whether a response is safe to
 * persist at all: this disk cache keys entries by URL alone, so a response
 * with a meaningful Vary can't be persisted as a single representation
 * without risking serving the wrong one back later. */
void macos9_cache_capture_hdr(const char *line, char *dst, size_t cap)
{
	static const char *const keep[] = {
		"age:", "date:", "etag:", "expires:",
		"cache-control:", "last-modified:", "vary:"
	};
	size_t nk = sizeof(keep) / sizeof(keep[0]);
	size_t used;
	size_t ll;
	size_t j;
	int wanted = 0;

	if (dst == NULL || cap == 0 || line == NULL || line[0] == '\0') return;
	for (j = 0; j < nk; j++) {
		if (strncasecmp(line, keep[j], strlen(keep[j])) == 0) {
			wanted = 1;
			break;
		}
	}
	if (!wanted) return;

	used = strlen(dst);
	ll = strlen(line);
	/* skip whole rather than truncate: a half header is worse than none */
	if (used + ll + 2 + 1 > cap) return;
	memcpy(dst + used, line, ll);
	dst[used + ll] = '\r';
	dst[used + ll + 1] = '\n';
	dst[used + ll + 2] = '\0';
}

/* ---- fixes987: streaming store ---- */

#ifdef __MACOS9__
struct cache_stream {
	int    used;
	short  ref;
	FSSpec spec;
	long   len;
	long   old_size;
	long   prefix_len;
};
static struct cache_stream g_streams[MACSURF_CACHE_STREAMS];

/* Close and forget a slot. Deletes the file unless it was committed. */
static void cache_stream_drop(struct cache_stream *st, int commit)
{
	long old_size;
	int deleted = 0;

	if (!st->used) return;
	old_size = st->old_size;
	if (st->ref != 0) {
		FSClose(st->ref);
		st->ref = 0;
	}
	if (!commit) {
		if (FSpDelete(&st->spec) == noErr) deleted = 1;
	}
	st->used = 0;
	st->len = 0;
	st->old_size = 0;
	st->prefix_len = 0;
	if (deleted && old_size > 0)
		cache_total_replace(old_size, 0);
}
#endif

int macos9_cache_stream_begin(const char *url, int status, const char *mime,
		const char *hdrs)
{
#ifdef __MACOS9__
	OSErr err;
	short vRef;
	long dirID;
	unsigned char fname[32];
	short ref = 0;
	unsigned char hdr[24];
	long count;
	long old_size = 0;
	size_t mime_len;
	size_t hdrs_len;
	int i;
	struct cache_stream *st = NULL;

	if (url == NULL || mime == NULL) return 0;
	if (!macos9_cache_mime_eligible(status, mime)) return 0;

	for (i = 0; i < MACSURF_CACHE_STREAMS; i++) {
		if (!g_streams[i].used) { st = &g_streams[i]; break; }
	}
	if (st == NULL) return 0;

	mime_len = strlen(mime);
	if (mime_len > 127) mime_len = 127;
	hdrs_len = (hdrs != NULL) ? strlen(hdrs) : 0;
	if (hdrs_len > MACSURF_CACHE_HDRS_MAX - 1) hdrs_len = 0;

	if (cache_dir_get(&vRef, &dirID) != noErr) return 0;
	cache_filename_for_url(url, fname);
	macos9_spec_from_pname(vRef, dirID, fname, &st->spec);
	err = FSpOpenDF(&st->spec, fsRdWrPerm, &ref);
	if (err == fnfErr) {
		err = FSpCreate(&st->spec, '????', '????', smSystemScript);
		if (err != noErr) return 0;
		err = FSpOpenDF(&st->spec, fsRdWrPerm, &ref);
	}
	if (err != noErr) return 0;
	if (GetEOF(ref, &old_size) != noErr) old_size = 0;
	SetFPos(ref, fsFromStart, 0);

	st->used = 1;
	st->ref = ref;
	st->len = 0;
	st->old_size = old_size;
	st->prefix_len = (long)sizeof(hdr) + (long)mime_len + (long)hdrs_len;

	hdr[0] = MACSURF_CACHE_MAGIC0; hdr[1] = MACSURF_CACHE_MAGIC1;
	hdr[2] = MACSURF_CACHE_MAGIC2; hdr[3] = MACSURF_CACHE_MAGIC3;
	hdr[4] = MACSURF_CACHE_MAGIC4; hdr[5] = MACSURF_CACHE_MAGIC5;
	hdr[6] = MACSURF_CACHE_MAGIC6; hdr[7] = MACSURF_CACHE_MAGIC7;
	cache_write_be32(hdr + 8,  (unsigned long)status);
	cache_write_be32(hdr + 12, (unsigned long)mime_len);
	cache_write_be32(hdr + 16, 0UL);
	cache_write_be32(hdr + 20, (unsigned long)hdrs_len);

	count = sizeof(hdr);
	if (FSWrite(ref, &count, hdr) != noErr || count != (long)sizeof(hdr)) {
		cache_stream_drop(st, 0); return 0;
	}
	if (mime_len > 0) {
		count = (long)mime_len;
		if (FSWrite(ref, &count, mime) != noErr || count != (long)mime_len) {
			cache_stream_drop(st, 0); return 0;
		}
	}
	if (hdrs_len > 0) {
		count = (long)hdrs_len;
		if (FSWrite(ref, &count, hdrs) != noErr || count != (long)hdrs_len) {
			cache_stream_drop(st, 0); return 0;
		}
	}

	return (int)(st - g_streams) + 1;
#else
	(void)url; (void)status; (void)mime; (void)hdrs;
	return 0;
#endif
}

int macos9_cache_stream_data(int h, const char *buf, long len)
{
#ifdef __MACOS9__
	struct cache_stream *st;
	long count;

	if (h <= 0 || h > MACSURF_CACHE_STREAMS) return 0;
	st = &g_streams[h - 1];
	if (!st->used) return 0;
	if (buf == NULL || len <= 0) return 1;

	if (st->len + len > MACSURF_CACHE_MAX_BYTES) {
		cache_stream_drop(st, 0);
		return 0;
	}
	count = len;
	if (FSWrite(st->ref, &count, buf) != noErr || count != len) {
		cache_stream_drop(st, 0);
		return 0;
	}
	st->len += len;
	return 1;
#else
	(void)h; (void)buf; (void)len;
	return 0;
#endif
}

void macos9_cache_stream_end(int h, int commit)
{
#ifdef __MACOS9__
	struct cache_stream *st;
	unsigned char lenbuf[4];
	long count;
	long old_size;
	long final_size;

	if (h <= 0 || h > MACSURF_CACHE_STREAMS) return;
	st = &g_streams[h - 1];
	if (!st->used) return;

	if (!commit || st->len <= 0) {
		cache_stream_drop(st, 0);
		return;
	}

	old_size = st->old_size;
	final_size = st->prefix_len + st->len;
	cache_write_be32(lenbuf, (unsigned long)st->len);
	if (SetFPos(st->ref, fsFromStart, 16) != noErr) {
		cache_stream_drop(st, 0);
		return;
	}
	count = 4;
	if (FSWrite(st->ref, &count, lenbuf) != noErr || count != 4) {
		cache_stream_drop(st, 0);
		return;
	}
	/* The old streaming path never truncated an overwritten cache file.
	 * Replacing a 900 KB object with a 200 KB one therefore left a 900 KB
	 * HFS file with a 200 KB logical body. Trim to the exact new record. */
	if (SetEOF(st->ref, final_size) != noErr) {
		cache_stream_drop(st, 0);
		return;
	}
	g_store_n++;
	macsurf_debug_log_writef(
		"LIFE CACHE stream url=(slot %d) len=%ld n=%ld",
		h, st->len, g_store_n);
	cache_stream_drop(st, 1);
	macsurf_http_skip_next_cache = 0;
	cache_total_replace(old_size, final_size);
#else
	(void)h; (void)commit;
#endif
}

void macos9_cache_store(const char *url, int status, const char *mime,
		const char *body_ptr, long body_len)
{
	macos9_cache_store_hdrs(url, status, mime, NULL, body_ptr, body_len);
}

void macos9_cache_store_hdrs(const char *url, int status, const char *mime,
		const char *hdrs, const char *body_ptr, long body_len)
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
	long old_size = 0;
	long new_size;
	size_t mime_len;
	size_t hdrs_len;
	long t0;

	if (url == NULL || body_ptr == NULL) return;
	if (body_len <= 0 || body_len > MACSURF_CACHE_MAX_BYTES) return;
	if (!macos9_cache_mime_eligible(status, mime)) return;

	t0 = (long)TickCount();

	err = cache_dir_get(&vRef, &dirID);
	if (err != noErr) return;

	cache_filename_for_url(url, fname);
	macos9_spec_from_pname(vRef, dirID, fname, &spec);
	err = FSpOpenDF(&spec, fsRdWrPerm, &ref);
	if (err == fnfErr) {
		err = FSpCreate(&spec, '????', '????', smSystemScript);
		if (err != noErr) return;
		err = FSpOpenDF(&spec, fsRdWrPerm, &ref);
	}
	if (err != noErr) return;
	if (GetEOF(ref, &old_size) != noErr) old_size = 0;
	SetFPos(ref, fsFromStart, 0);

	mime_len = strlen(mime);
	if (mime_len > 127) mime_len = 127;
	hdrs_len = (hdrs != NULL) ? strlen(hdrs) : 0;
	if (hdrs_len > MACSURF_CACHE_HDRS_MAX - 1) hdrs_len = 0;
	new_size = (long)sizeof(hdr) + (long)mime_len +
		(long)hdrs_len + body_len;

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
	cache_write_be32(hdr + 20, (unsigned long)hdrs_len);

	count = sizeof(hdr);
	if (FSWrite(ref, &count, hdr) != noErr || count != (long)sizeof(hdr)) {
		FSClose(ref);
		return;
	}
	if (mime_len > 0) {
		count = (long)mime_len;
		if (FSWrite(ref, &count, mime) != noErr || count != (long)mime_len) {
			FSClose(ref);
			return;
		}
	}
	if (hdrs_len > 0) {
		count = (long)hdrs_len;
		if (FSWrite(ref, &count, hdrs) != noErr || count != (long)hdrs_len) {
			FSClose(ref);
			return;
		}
	}
	count = body_len;
	if (FSWrite(ref, &count, body_ptr) != noErr || count != body_len) {
		FSClose(ref);
		return;
	}
	SetEOF(ref, new_size);
	FSClose(ref);

	{
		long dt = (long)TickCount() - t0;
		g_store_ticks += dt;
		g_store_n++;
		macsurf_debug_log_writef(
			"LIFE CACHE store url=%s mime=%s len=%ld hdrs=%ld"
			" t=%ld tot=%ld n=%ld",
			url, mime, body_len, (long)hdrs_len,
			dt, g_store_ticks, g_store_n);
	}
	macsurf_http_skip_next_cache = 0;
	cache_total_replace(old_size, new_size);
#else
	(void)url; (void)status; (void)mime; (void)hdrs;
	(void)body_ptr; (void)body_len;
#endif
}

int macos9_cache_lookup(const char *url, char **body_out,
		long *body_len_out, char *mime_out, int mime_cap,
		int *status_out)
{
	return macos9_cache_lookup_hdrs(url, body_out, body_len_out,
			mime_out, mime_cap, status_out, NULL, 0);
}

int macos9_cache_lookup_hdrs(const char *url, char **body_out,
		long *body_len_out, char *mime_out, int mime_cap,
		int *status_out, char *hdrs_out, int hdrs_cap)
{
#ifdef __MACOS9__
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
	unsigned long hdrs_len;
	char mime_buf[128];
	char hdrs_buf[MACSURF_CACHE_HDRS_MAX];
	char *body;

	*body_out = NULL;
	*body_len_out = 0;
	if (mime_out != NULL && mime_cap > 0) mime_out[0] = '\0';
	if (hdrs_out != NULL && hdrs_cap > 0) hdrs_out[0] = '\0';
	*status_out = 0;

	if (url == NULL) return 0;
	if (macsurf_http_skip_next_cache) return 0;
	if (macos9_cache_post_bypass_active(url)) return 0;

	if (cache_dir_get(&vRef, &dirID) != noErr) return 0;
	cache_filename_for_url(url, fname);
	macos9_spec_from_pname(vRef, dirID, fname, &spec);
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
	hdrs_len = cache_read_be32(hdr + 20);
	if (mime_len > 127 || body_len == 0 ||
			body_len > MACSURF_CACHE_MAX_BYTES ||
			hdrs_len > MACSURF_CACHE_HDRS_MAX - 1) {
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

	if (hdrs_len > 0) {
		count = (long)hdrs_len;
		if (FSRead(ref, &count, hdrs_buf) != noErr ||
				count != (long)hdrs_len) {
			FSClose(ref);
			return 0;
		}
	}
	hdrs_buf[hdrs_len] = '\0';

	body = (char *)malloc(body_len);
	if (body == NULL) {
		macsurf_debug_log_writef(
			"CACHE OOM url=%s need=%ld maxblock=%ld",
			url, (long)body_len, (long)macos9_heap_max_block());
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
	if (hdrs_out != NULL && hdrs_cap > 0) {
		size_t n = hdrs_len;
		if (n >= (size_t)hdrs_cap) n = (size_t)hdrs_cap - 1;
		memcpy(hdrs_out, hdrs_buf, n);
		hdrs_out[n] = '\0';
	}
	macsurf_debug_log_writef(
		"LIFE CACHE hit url=%s mime=%s len=%ld status=%d hdrs=%ld",
		url, mime_buf, (long)body_len, (int)status_v, (long)hdrs_len);
	return 1;
#else
	(void)url; (void)body_out; (void)body_len_out;
	(void)mime_out; (void)mime_cap; (void)status_out;
	(void)hdrs_out; (void)hdrs_cap;
	return 0;
#endif
}

/* fixes238 - dead-host file persistence. File "deadhosts.txt" lives in
 * the same MacSurf Cache folder as the body cache. Plain text, one
 * "host:port" entry per line. */

long macos9_deadhost_load(char *out_buf, long buf_cap)
{
#ifdef __MACOS9__
	short vRef;
	long dirID;
	FSSpec spec;
	short ref = 0;
	long count;

	if (out_buf == NULL || buf_cap <= 0) return 0;
	out_buf[0] = '\0';

	if (cache_dir_get(&vRef, &dirID) != noErr) return 0;
	macos9_spec_from_cname(vRef, dirID, "deadhosts.txt", &spec);
	if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return 0;

	count = buf_cap - 1;
	if (FSRead(ref, &count, out_buf) != noErr && count == 0) {
		FSClose(ref);
		return 0;
	}
	FSClose(ref);
	if (count < 0) count = 0;
	if (count >= buf_cap) count = buf_cap - 1;
	out_buf[count] = '\0';
	macsurf_debug_log_writef(
		"deadhost LOAD count=%ld bytes", count);
	return count;
#else
	(void)out_buf; (void)buf_cap;
	return 0;
#endif
}

void macos9_deadhost_save(const char *buf, long len)
{
#ifdef __MACOS9__
	OSErr err;
	short vRef;
	long dirID;
	FSSpec spec;
	short ref = 0;
	long count;

	if (buf == NULL || len < 0) return;
	if (cache_dir_get(&vRef, &dirID) != noErr) return;
	macos9_spec_from_cname(vRef, dirID, "deadhosts.txt", &spec);
	err = FSpOpenDF(&spec, fsWrPerm, &ref);
	if (err == fnfErr) {
		err = FSpCreate(&spec, '????', '????', smSystemScript);
		if (err != noErr) return;
		err = FSpOpenDF(&spec, fsWrPerm, &ref);
	}
	if (err != noErr) return;
	(void)SetEOF(ref, 0);

	if (len > 0) {
		count = len;
		(void)FSWrite(ref, &count, buf);
	}
	SetEOF(ref, len);
	FSClose(ref);
	(void)FlushVol(NULL, vRef);
	macsurf_debug_log_writef(
		"deadhost SAVE len=%ld", len);
#else
	(void)buf; (void)len;
#endif
}

void macos9_deadhost_clear(void)
{
#ifdef __MACOS9__
	short vRef;
	long dirID;
	FSSpec spec;

	if (cache_dir_get(&vRef, &dirID) != noErr) return;
	macos9_spec_from_cname(vRef, dirID, "deadhosts.txt", &spec);
	(void)FSpDelete(&spec);
#endif
}

/* fixes750 (#213) - purge only the cached BODY files ("h_xxxxxxxx"), leaving
 * deadhosts.txt (and any other non-body file) in place. Called when a login
 * POST establishes a session: every page we cached logged-OUT is now stale,
 * so drop the bodies and let later navigations refetch WITH the session
 * cookie. Preserving deadhosts.txt matters - a full clear would re-enable the
 * fast-fail hosts (jsdelivr / fonts.googleapis) and restart the fetch storm
 * the perf work fixed. Returns the number of body files deleted. */
long macos9_cache_clear_bodies(void)
{
#ifdef __MACOS9__
	short vRef;
	long dirID;
	short idx;
	long deleted = 0;

	if (cache_dir_get(&vRef, &dirID) != noErr) return 0;

	idx = 1;
	while (idx > 0 && deleted < 100000L) {
		CInfoPBRec pb;
		Str63 nm;
		FSSpec spec;
		OSErr err;

		nm[0] = 0;
		memset(&pb, 0, sizeof pb);
		pb.hFileInfo.ioNamePtr = nm;
		pb.hFileInfo.ioVRefNum = vRef;
		pb.hFileInfo.ioDirID = dirID;
		pb.hFileInfo.ioFDirIndex = idx;
		err = PBGetCatInfoSync(&pb);
		if (err != noErr) break;

		if ((pb.hFileInfo.ioFlAttrib & 0x10) != 0 ||
		    nm[0] < 2 || nm[1] != 'h' || nm[2] != '_') {
			idx++;
			continue;
		}
		macos9_spec_from_pname(vRef, dirID, nm, &spec);
		if (FSpDelete(&spec) == noErr) {
			deleted++;
		} else {
			idx++;
		}
	}
	(void)FlushVol(NULL, vRef);
	cache_total_reset();
	macsurf_debug_log_writef("cache CLEAR-BODIES deleted=%ld files", deleted);
	return deleted;
#else
	return 0;
#endif
}

/* fixes706 (#Delete Cache) - empty the disk cache: delete every file in the
 * MacSurfData/Cache folder (cached bodies "h_xxxxxxxx" + deadhosts.txt).
 * Bookmarks / history / cookies live at the root are untouched.
 * Returns the number of files deleted. */
long macos9_cache_clear(void)
{
#ifdef __MACOS9__
	short vRef;
	long dirID;
	short idx;
	long deleted = 0;

	if (cache_dir_get(&vRef, &dirID) != noErr) return 0;

	idx = 1;
	while (idx > 0 && deleted < 100000L) {
		CInfoPBRec pb;
		Str63 nm;
		FSSpec spec;
		OSErr err;

		nm[0] = 0;
		memset(&pb, 0, sizeof pb);
		pb.hFileInfo.ioNamePtr = nm;
		pb.hFileInfo.ioVRefNum = vRef;
		pb.hFileInfo.ioDirID = dirID;
		pb.hFileInfo.ioFDirIndex = idx;
		err = PBGetCatInfoSync(&pb);
		if (err != noErr) break;

		if ((pb.hFileInfo.ioFlAttrib & 0x10) != 0) {
			idx++;
			continue;
		}
		macos9_spec_from_pname(vRef, dirID, nm, &spec);
		if (FSpDelete(&spec) == noErr) {
			deleted++;
		} else {
			idx++;
		}
	}
	(void)FlushVol(NULL, vRef);
	cache_total_reset();
	macsurf_debug_log_writef("cache CLEAR deleted=%ld files", deleted);
	return deleted;
#else
	return 0;
#endif
}

/* ------------------------------------------------------------------ */
/* fixes645 (#48) - bookmark persistence across launches.             */
/*                                                                    */
/* Raw-buffer read/write of a "MacSurf Bookmarks" text file next to   */
/* the app (or Desktop fallback), using the same FSSpec binary I/O as */
/* the dead-host list - NOT the flaky MSL fopen path. chrome_extras.c */
/* owns the (de)serialization (one "URL\tlabel\n" record per line);   */
/* these two just move the bytes. Every failure is a silent no-op.    */
/* ------------------------------------------------------------------ */

long macos9_bookmarks_load(char *out_buf, long buf_cap)
{
#ifdef __MACOS9__
	OSErr err;
	short vRef;
	long dirID;
	FSSpec spec;
	unsigned char fname[32];
	short ref = 0;
	long count;
	const char *name = "MacSurf Bookmarks";
	size_t nlen;

	if (out_buf == NULL || buf_cap <= 0) return 0;
	out_buf[0] = '\0';

	err = macsurfdata_dir_get(NULL, &vRef, &dirID);
	if (err != noErr) return 0;

	nlen = strlen(name);
	if (nlen > 31) nlen = 31;
	fname[0] = (unsigned char)nlen;
	memcpy(fname + 1, name, nlen);

	err = FSMakeFSSpec(vRef, dirID, fname, &spec);
	if (err != noErr) return 0;
	if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return 0;

	count = buf_cap - 1;
	if (FSRead(ref, &count, out_buf) != noErr && count == 0) {
		FSClose(ref);
		return 0;
	}
	FSClose(ref);
	if (count < 0) count = 0;
	if (count >= buf_cap) count = buf_cap - 1;
	out_buf[count] = '\0';
	macsurf_debug_log_writef("bookmarks LOAD count=%ld bytes", count);
	return count;
#else
	(void)out_buf; (void)buf_cap;
	return 0;
#endif
}

void macos9_bookmarks_save(const char *buf, long len)
{
#ifdef __MACOS9__
	OSErr err;
	short vRef;
	long dirID;
	FSSpec spec;
	unsigned char fname[32];
	short ref = 0;
	long count;
	const char *name = "MacSurf Bookmarks";
	size_t nlen;

	if (buf == NULL || len < 0) return;

	err = macsurfdata_dir_get(NULL, &vRef, &dirID);
	if (err != noErr) return;

	nlen = strlen(name);
	if (nlen > 31) nlen = 31;
	fname[0] = (unsigned char)nlen;
	memcpy(fname + 1, name, nlen);

	err = FSMakeFSSpec(vRef, dirID, fname, &spec);
	if (err == fnfErr) {
		err = FSpCreate(&spec, 'MPLS', 'TEXT', smSystemScript);
		if (err != noErr) return;
		err = FSMakeFSSpec(vRef, dirID, fname, &spec);
		if (err != noErr) return;
	} else if (err != noErr) {
		return;
	}

	if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) return;
	(void)SetEOF(ref, 0);
	if (len > 0) {
		count = len;
		(void)FSWrite(ref, &count, buf);
	}
	SetEOF(ref, len);
	FSClose(ref);
	(void)FlushVol(NULL, vRef);
	macsurf_debug_log_writef("bookmarks SAVE len=%ld", len);
#else
	(void)buf; (void)len;
#endif
}

/* ------------------------------------------------------------------ */
/* fixes698 (#47) - persistent visit HISTORY store. A "MacSurf History" */
/* text file next to bookmarks in the MacSurfData root, moved by the   */
/* same FSSpec binary I/O (NOT MSL fopen). chrome_extras.c owns the    */
/* "ts<TAB>url<TAB>title\n" (de)serialization; these move the bytes.   */
/* NetSurf's own urldb is session-only and exposes no clear API, so    */
/* MacSurf keeps its own store so history survives relaunch and can be */
/* cleared. Every failure is a silent no-op.                           */
/* ------------------------------------------------------------------ */

long macos9_history_load(char *out_buf, long buf_cap)
{
#ifdef __MACOS9__
	OSErr err;
	short vRef;
	long dirID;
	FSSpec spec;
	unsigned char fname[32];
	short ref = 0;
	long count;
	const char *name = "MacSurf History";
	size_t nlen;

	if (out_buf == NULL || buf_cap <= 0) return 0;
	out_buf[0] = '\0';

	err = macsurfdata_dir_get(NULL, &vRef, &dirID);
	if (err != noErr) return 0;

	nlen = strlen(name);
	if (nlen > 31) nlen = 31;
	fname[0] = (unsigned char)nlen;
	memcpy(fname + 1, name, nlen);

	err = FSMakeFSSpec(vRef, dirID, fname, &spec);
	if (err != noErr) return 0;
	if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return 0;

	count = buf_cap - 1;
	if (FSRead(ref, &count, out_buf) != noErr && count == 0) {
		FSClose(ref);
		return 0;
	}
	FSClose(ref);
	if (count < 0) count = 0;
	if (count >= buf_cap) count = buf_cap - 1;
	out_buf[count] = '\0';
	macsurf_debug_log_writef("history LOAD count=%ld bytes", count);
	return count;
#else
	(void)out_buf; (void)buf_cap;
	return 0;
#endif
}

void macos9_history_save(const char *buf, long len)
{
#ifdef __MACOS9__
	OSErr err;
	short vRef;
	long dirID;
	FSSpec spec;
	unsigned char fname[32];
	short ref = 0;
	long count;
	const char *name = "MacSurf History";
	size_t nlen;

	if (buf == NULL || len < 0) return;

	err = macsurfdata_dir_get(NULL, &vRef, &dirID);
	if (err != noErr) return;

	nlen = strlen(name);
	if (nlen > 31) nlen = 31;
	fname[0] = (unsigned char)nlen;
	memcpy(fname + 1, name, nlen);

	err = FSMakeFSSpec(vRef, dirID, fname, &spec);
	if (err == fnfErr) {
		err = FSpCreate(&spec, 'MPLS', 'TEXT', smSystemScript);
		if (err != noErr) return;
		err = FSMakeFSSpec(vRef, dirID, fname, &spec);
		if (err != noErr) return;
	} else if (err != noErr) {
		return;
	}

	if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) return;
	(void)SetEOF(ref, 0);
	if (len > 0) {
		count = len;
		(void)FSWrite(ref, &count, buf);
	}
	SetEOF(ref, len);
	FSClose(ref);
	(void)FlushVol(NULL, vRef);
	macsurf_debug_log_writef("history SAVE len=%ld", len);
#else
	(void)buf; (void)len;
#endif
}

/* fixes647 - downloads land in MacSurfData/Downloads (was the standalone
 * "MacSurf Downloads" folder). Same shared MacSurfData root as cache and
 * bookmarks, so there's one folder next to the app, not several. */
OSErr macos9_downloads_dir_get(short *vRef, long *dirID)
{
#ifdef __MACOS9__
	return macsurfdata_dir_get("Downloads", vRef, dirID);
#else
	(void)vRef; (void)dirID;
	return -1;
#endif
}

/* ------------------------------------------------------------------ */
/* fixes368 (#167) - cookie-jar persistence across launches.          */
/*                                                                    */
/* A Facebook (or any) login lives entirely in the urldb cookie jar,  */
/* which is in-memory only - so it evaporates on quit. These two      */
/* helpers persist it so the session survives a relaunch.             */
/*                                                                    */
/* We reuse NetSurf's mature cookie serializer (urldb_save_cookies /  */
/* urldb_load_cookies, content/urldb.c), which reads/writes a tab-    */
/* separated text file through stdio. It only exposes a path-based    */
/* API, so unlike the rest of this file (FSSpec binary I/O) we hand   */
/* it a leaf filename and let MSL resolve it against the app's        */
/* default directory - the same place every launch, so the file       */
/* round-trips. Return type is nserror (an int enum); we only log it, */
/* so an `int` extern decl avoids dragging urldb.h + nsurl into this  */
/* Toolbox-heavy TU.                                                  */
/*                                                                    */
/* Every failure is a SILENT no-op: a missing file (first run) or a   */
/* refused fopen just leaves the jar in-memory-only - exactly the     */
/* pre-fixes368 behaviour, never a crash. If the hardware bring-up    */
/* shows MSL fopen won't honour this path, the fallback is the FSSpec */
/* route (serialize urldb to a buffer + FSWrite like                  */
/* macos9_deadhost_save) - see facebook-mbasic-scope.md Step 2.       */
extern int urldb_load_cookies(const char *filename);
extern int urldb_save_cookies(const char *filename);

#define MACSURF_COOKIE_FILE ":MacSurfData:MacSurf Cookies"
#define MACSURF_COOKIE_LEAF  "MacSurf Cookies"

static void macos9_cookies_ensure_dir(void)
{
#ifdef __MACOS9__
	short vRef;
	long dirID;
	(void)macsurfdata_dir_get(NULL, &vRef, &dirID);
#endif
}

#ifdef __MACOS9__
extern int macos9_fsspec_to_path(const FSSpec *spec, char *out, long cap);

static int macos9_cookie_fullpath(char *out, long cap)
{
	short vRef;
	long dirID;
	FSSpec spec;
	OSErr err;
	unsigned char fname[32];
	size_t nlen;
	if (macsurfdata_dir_get(NULL, &vRef, &dirID) != noErr) return -1;
	nlen = strlen(MACSURF_COOKIE_LEAF);
	if (nlen > 31) nlen = 31;
	fname[0] = (unsigned char)nlen;
	memcpy(fname + 1, MACSURF_COOKIE_LEAF, nlen);
	err = FSMakeFSSpec(vRef, dirID, fname, &spec);
	if (err != noErr && err != fnfErr) return -1;
	return macos9_fsspec_to_path(&spec, out, cap);
}

static long macos9_cookie_file_bytes(void)
{
	short vRef;
	long dirID;
	FSSpec spec;
	short ref = 0;
	long eof = -1;
	unsigned char fname[32];
	size_t nlen;
	if (macsurfdata_dir_get(NULL, &vRef, &dirID) != noErr) return -1;
	nlen = strlen(MACSURF_COOKIE_LEAF);
	if (nlen > 31) nlen = 31;
	fname[0] = (unsigned char)nlen;
	memcpy(fname + 1, MACSURF_COOKIE_LEAF, nlen);
	if (FSMakeFSSpec(vRef, dirID, fname, &spec) != noErr) return -1;
	if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return -1;
	if (GetEOF(ref, &eof) != noErr) eof = -1;
	FSClose(ref);
	return eof;
}
#endif /* __MACOS9__ */

void macos9_cookies_load(void)
{
	int r;
	macos9_cookies_ensure_dir();
#ifdef __MACOS9__
	{
		char path[512];
		long pre = macos9_cookie_file_bytes();
		if (macos9_cookie_fullpath(path, (long)sizeof path) == 0) {
			r = urldb_load_cookies(path);
			macsurf_debug_log_writef(
				"WORK ckload rc=%d filebytes=%ld path=%s",
				r, pre, path);
			return;
		}
		macsurf_debug_log_writef("WORK ckload PATHFAIL filebytes=%ld", pre);
	}
#endif
	r = urldb_load_cookies(MACSURF_COOKIE_FILE);
	macsurf_debug_log_writef("WORK ckload rc=%d (colon-fallback %s)", r,
		MACSURF_COOKIE_FILE);
}

void macos9_cookies_save(void)
{
	int r;
#ifdef __MACOS9__
	/* Clean shutdown already passes through cookie persistence. Flush the
	 * small cache-size checkpoint here too so short sessions do not lose the
	 * last (<16) exact store deltas and gradually undercount the disk cache. */
	if (g_cache_total_dirty > 0) cache_total_state_save();
#endif
	macos9_cookies_ensure_dir();
#ifdef __MACOS9__
	{
		char path[512];
		if (macos9_cookie_fullpath(path, (long)sizeof path) == 0) {
			r = urldb_save_cookies(path);
			macsurf_debug_log_writef(
				"WORK cksave rc=%d filebytes=%ld path=%s",
				r, macos9_cookie_file_bytes(), path);
			return;
		}
		macsurf_debug_log_writef("WORK cksave PATHFAIL");
	}
#endif
	r = urldb_save_cookies(MACSURF_COOKIE_FILE);
	macsurf_debug_log_writef("WORK cksave rc=%d (colon-fallback %s)", r,
		MACSURF_COOKIE_FILE);
}
