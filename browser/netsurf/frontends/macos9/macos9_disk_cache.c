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

#define MACSURF_CACHE_MAGIC0 'M'
#define MACSURF_CACHE_MAGIC1 'S'
#define MACSURF_CACHE_MAGIC2 'C'
#define MACSURF_CACHE_MAGIC3 'A'
#define MACSURF_CACHE_MAGIC4 'C'
#define MACSURF_CACHE_MAGIC5 'H'
#define MACSURF_CACHE_MAGIC6 'E'
#define MACSURF_CACHE_MAGIC7 0x01

/* fixes181 — Reload sets this to 1 so the next lookup short-circuits
 * to miss. Cleared after a successful store so sub-resources resume
 * normal cache behaviour. */
int macsurf_http_skip_next_cache = 0;

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

/* Build/resolve "MacSurf Cache" under a given base directory. Creates it if
 * missing. Returns noErr + the folder's (vRefNum, dirID). */
static OSErr cache_dir_under(short base_vref, long base_dir,
		short *vRef, long *dirID)
{
	OSErr err;
	FSSpec spec;
	unsigned char fname[32];
	const char *name = "MacSurf Cache";
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

/* Resolve the cache folder. fixes641 (#197): create "MacSurf Cache" next to the
 * running application; fall back to the boot Desktop only if the app dir can't
 * be resolved or isn't writable. */
static OSErr cache_dir_get(short *vRef, long *dirID)
{
	OSErr err;
	short base_vref;
	long base_dir;
	short desk_vref;
	long desk_dir;

	err = macos9_app_dir_get(&base_vref, &base_dir);
	if (err == noErr) {
		err = cache_dir_under(base_vref, base_dir, vRef, dirID);
		if (err == noErr) return noErr;
	}
	/* Fallback: boot-volume Desktop (original behaviour). */
	err = FindFolder(kOnSystemDisk, kDesktopFolderType,
			kDontCreateFolder, &desk_vref, &desk_dir);
	if (err != noErr) return err;
	return cache_dir_under(desk_vref, desk_dir, vRef, dirID);
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
	return 0;
}

void macos9_cache_store(const char *url, int status, const char *mime,
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
	if (!macos9_cache_mime_eligible(status, mime)) return;

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
	/* fixes248 — FlushVol REMOVED. Same rationale as fixes96 on the
	 * log writer: synchronous volume flush costs 10-50 ms per call on
	 * real hardware, and with ~20 cache stores per cold mactrove load
	 * that was 200-1000 ms of pure disk-sync wait between sub-resource
	 * deliveries. HFS's normal write cache flushes on its own cadence,
	 * and the macsurf_debug_log's session-close FlushVol catches any
	 * remaining buffered writes at app quit. Worst case if the app
	 * crashes: a few cache files may be partially written and fail the
	 * magic check at next lookup — which is exactly the "miss" path
	 * (refetch from network). No data corruption risk. */

	macsurf_debug_log_writef(
		"CACHE store url=%s mime=%s len=%ld",
		url, mime, body_len);
	macsurf_http_skip_next_cache = 0;
#else
	(void)url; (void)status; (void)mime; (void)body_ptr; (void)body_len;
#endif
}

int macos9_cache_lookup(const char *url, char **body_out,
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
	if (macsurf_http_skip_next_cache) return 0;

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

/* fixes238 — dead-host file persistence. File "deadhosts.txt" lives in
 * the same MacSurf Cache folder as the body cache. Plain text, one
 * "host:port" entry per line. */

long macos9_deadhost_load(char *out_buf, long buf_cap)
{
#ifdef __MACOS9__
	OSErr err;
	short vRef;
	long dirID;
	FSSpec spec;
	unsigned char fname[16];
	short ref = 0;
	long count;
	const char *name = "deadhosts.txt";
	size_t nlen;

	if (out_buf == NULL || buf_cap <= 0) return 0;
	out_buf[0] = '\0';

	err = cache_dir_get(&vRef, &dirID);
	if (err != noErr) return 0;

	nlen = strlen(name);
	if (nlen > 15) nlen = 15;
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
	unsigned char fname[16];
	short ref = 0;
	long count;
	const char *name = "deadhosts.txt";
	size_t nlen;

	if (buf == NULL || len < 0) return;

	err = cache_dir_get(&vRef, &dirID);
	if (err != noErr) return;

	nlen = strlen(name);
	if (nlen > 15) nlen = 15;
	fname[0] = (unsigned char)nlen;
	memcpy(fname + 1, name, nlen);

	err = FSMakeFSSpec(vRef, dirID, fname, &spec);
	if (err == fnfErr) {
		err = FSpCreate(&spec, '????', '????', smSystemScript);
		if (err != noErr) return;
		err = FSMakeFSSpec(vRef, dirID, fname, &spec);
		if (err != noErr) return;
	} else if (err != noErr) {
		return;
	}

	if (FSpOpenDF(&spec, fsWrPerm, &ref) != noErr) return;
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
	OSErr err;
	short vRef;
	long dirID;
	FSSpec spec;
	unsigned char fname[16];
	const char *name = "deadhosts.txt";
	size_t nlen;

	err = cache_dir_get(&vRef, &dirID);
	if (err != noErr) return;
	nlen = strlen(name);
	if (nlen > 15) nlen = 15;
	fname[0] = (unsigned char)nlen;
	memcpy(fname + 1, name, nlen);
	if (FSMakeFSSpec(vRef, dirID, fname, &spec) == noErr) {
		(void)FSpDelete(&spec);
	}
#endif
}

/* ------------------------------------------------------------------ */
/* fixes645 (#48) — bookmark persistence across launches.             */
/*                                                                    */
/* Raw-buffer read/write of a "MacSurf Bookmarks" text file next to   */
/* the app (or Desktop fallback), using the same FSSpec binary I/O as */
/* the dead-host list — NOT the flaky MSL fopen path. chrome_extras.c */
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

	err = cache_dir_get(&vRef, &dirID);
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

	err = cache_dir_get(&vRef, &dirID);
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

/* fixes645 — resolve the "MacSurf Downloads" folder (app-relative,    */
/* Desktop fallback) and hand back vRef/dirID so the download path can */
/* auto-save without a modal Nav dialog. Reuses cache_dir_get's app-   */
/* dir-first logic but targets a sibling "MacSurf Downloads" folder.   */
OSErr macos9_downloads_dir_get(short *vRef, long *dirID)
{
#ifdef __MACOS9__
	OSErr err;
	short base_vref;
	long base_dir;
	short desk_vref;
	long desk_dir;
	FSSpec spec;
	long new_dir;
	unsigned char fname[24];
	const char *fn = "MacSurf Downloads";
	size_t n = strlen(fn);
	if (n > 23) n = 23;
	fname[0] = (unsigned char)n;
	memcpy(fname + 1, fn, n);

	err = macos9_app_dir_get(&base_vref, &base_dir);
	if (err != noErr) {
		err = FindFolder(kOnSystemDisk, kDesktopFolderType,
			kDontCreateFolder, &desk_vref, &desk_dir);
		if (err != noErr) return err;
		base_vref = desk_vref;
		base_dir = desk_dir;
	}

	err = FSMakeFSSpec(base_vref, base_dir, fname, &spec);
	if (err == fnfErr) {
		err = FSpDirCreate(&spec, smSystemScript, &new_dir);
		if (err != noErr) return err;
		*vRef = base_vref;
		*dirID = new_dir;
		return noErr;
	} else if (err != noErr) {
		return err;
	}
	/* Folder (or a same-named file) exists — resolve its dirID. */
	{
		CInfoPBRec pb;
		Str63 nm;
		memcpy(nm, fname, fname[0] + 1);
		memset(&pb, 0, sizeof(pb));
		pb.dirInfo.ioNamePtr = nm;
		pb.dirInfo.ioVRefNum = base_vref;
		pb.dirInfo.ioDrDirID = base_dir;
		pb.dirInfo.ioFDirIndex = 0;
		if (PBGetCatInfoSync(&pb) != noErr ||
		    !(pb.dirInfo.ioFlAttrib & ioDirMask)) {
			return dirNFErr;
		}
		*vRef = base_vref;
		*dirID = pb.dirInfo.ioDrDirID;
		return noErr;
	}
#else
	(void)vRef; (void)dirID;
	return -1;
#endif
}

/* ------------------------------------------------------------------ */
/* fixes368 (#167) — cookie-jar persistence across launches.          */
/*                                                                    */
/* A Facebook (or any) login lives entirely in the urldb cookie jar,  */
/* which is in-memory only — so it evaporates on quit. These two      */
/* helpers persist it so the session survives a relaunch.             */
/*                                                                    */
/* We reuse NetSurf's mature cookie serializer (urldb_save_cookies /  */
/* urldb_load_cookies, content/urldb.c), which reads/writes a tab-    */
/* separated text file through stdio. It only exposes a path-based    */
/* API, so unlike the rest of this file (FSSpec binary I/O) we hand   */
/* it a leaf filename and let MSL resolve it against the app's        */
/* default directory — the same place every launch, so the file       */
/* round-trips. Return type is nserror (an int enum); we only log it, */
/* so an `int` extern decl avoids dragging urldb.h + nsurl into this  */
/* Toolbox-heavy TU.                                                  */
/*                                                                    */
/* Every failure is a SILENT no-op: a missing file (first run) or a   */
/* refused fopen just leaves the jar in-memory-only — exactly the     */
/* pre-fixes368 behaviour, never a crash. If the hardware bring-up    */
/* shows MSL fopen won't honour this path, the fallback is the FSSpec */
/* route (serialize urldb to a buffer + FSWrite like                  */
/* macos9_deadhost_save) — see facebook-mbasic-scope.md Step 2.       */
extern int urldb_load_cookies(const char *filename);
extern int urldb_save_cookies(const char *filename);

#define MACSURF_COOKIE_FILE "MacSurf Cookies"

void macos9_cookies_load(void)
{
	int r;
	r = urldb_load_cookies(MACSURF_COOKIE_FILE);
	macsurf_debug_log_writef("cookies: load rc=%d (%s)", r,
		MACSURF_COOKIE_FILE);
}

void macos9_cookies_save(void)
{
	int r;
	r = urldb_save_cookies(MACSURF_COOKIE_FILE);
	macsurf_debug_log_writef("cookies: save rc=%d (%s)", r,
		MACSURF_COOKIE_FILE);
}
