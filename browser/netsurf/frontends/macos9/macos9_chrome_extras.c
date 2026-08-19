/*
 * MacSurf - macos9_chrome_extras.c
 *
 * fixes330+ - View Source / Find-in-page / Bookmarks / History UI.
 *
 * fixes352 (#96 #45 #107) - replace stubs with real implementations:
 *
 *   - View Source: route through content_get_source_data + data: URL
 *     (the invented "view-source:" scheme had no fetcher behind it).
 *   - Find-in-page: programmatic Carbon dialog with TextEdit input +
 *     OK/Cancel buttons; routes to browser_window_search. Search term
 *     cached for Find Again.
 *   - Bookmarks: 128-entry array of bookmarks AND folders, PERSISTED to a
 *     tab-delimited "MacSurf Bookmarks" file (macos9_disk_cache.c: see
 *     _bookmarks_save / _bookmarks_load).
 *     fixes882: this used to read "still session-only array; follow-on round
 *     wires desktop/hotlist.c for disk persistence". The follow-on round
 *     happened (fixes645); the comment did not. Persistence is our own file
 *     format rather than desktop/hotlist.c, which is probably why the
 *     sentence was never revisited.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include "about_audio_data.h"

#include "utils/ns_errors.h"
#include "netsurf/browser_window.h"
#include "netsurf/content.h"
#include "macos9.h"
#include "macsurf_debug.h"
#include "about_logo_data.h"   /* fixes726 - crisp 64x64 puffin PNG */
#include "manager_icons_data.h" /* fixes745 - History/Bookmark banner icons */

#ifdef __MACOS9__
#include <Carbon.h>
#include <Movies.h>
#include <QuickTimeComponents.h>
#endif

extern struct browser_window *macos9_gw_bw(struct gui_window *g);
extern void macos9_window_navigate(struct gui_window *g, const char *url);
extern const char *nsurl_access(const struct nsurl *u);
extern struct nsurl *browser_window_access_url(
	const struct browser_window *bw);

/* ====================================================================
 * fixes352 (#96) - View Source via data: URL
 *
 * The pre-fix path navigated to "view-source:<url>" which no fetcher
 * recognises (NetSurf core's nsurl.h enumerates HTTP/HTTPS/FILE/FTP/
 * MAILTO/DATA/OTHER - no view-source scheme), so the navigation
 * silently failed. NetSurf's data: scheme is fully wired and the
 * original content's source bytes are already in memory via
 * content_get_source_data; build a data:text/plain URL from those
 * bytes and navigate to it. The page then renders as plain text
 * showing the HTML markup.
 *
 * Size cap at 32 KB for V1. Larger pages get the first 32 KB; a
 * future round can build a dedicated source-display content handler
 * that streams without the data: round-trip.
 * ==================================================================== */
/* fixes352b (#96) - emit one percent-encoded byte (or pass unreserved
 * through). Shared by the open/close HTML wrapper strings and the
 * per-source-byte HTML-escape expansion. */
static void enc_byte(char **out, unsigned char b)
{
	static const char hex[] = "0123456789ABCDEF";
	char *p = *out;
	if ((b >= 'A' && b <= 'Z') ||
	    (b >= 'a' && b <= 'z') ||
	    (b >= '0' && b <= '9') ||
	    b == '-' || b == '_' || b == '.' || b == '~') {
		*p++ = (char)b;
	} else {
		*p++ = '%';
		*p++ = hex[(b >> 4) & 0xF];
		*p++ = hex[b & 0xF];
	}
	*out = p;
}

/* HTML-escape one source byte, calling enc_byte for each output char.
 * < → &lt; / > → &gt; / & → &amp; / quote → &quot; / other → pass-through. */
static void html_esc(char **out, unsigned char b)
{
	const char *esc = NULL;
	if (b == '<') esc = "&lt;";
	else if (b == '>') esc = "&gt;";
	else if (b == '&') esc = "&amp;";
	else if (b == '"') esc = "&quot;";
	if (esc != NULL) {
		while (*esc != '\0') enc_byte(out, (unsigned char)*esc++);
	} else {
		enc_byte(out, b);
	}
}

void macos9_view_source_for_window(struct gui_window *g)
{
	struct browser_window *bw;
	struct hlcache_handle *h;
	const unsigned char *src;
	size_t src_size;
	char *enc;
	char *p;
	size_t i;
	size_t cap = 32 * 1024;
	static const char *prefix =
		"data:text/html;charset=utf-8,";
	static const char *doc_open =
		"<!DOCTYPE html><html><head><title>Source</title>"
		"<style>body{font-family:Geneva,sans-serif;background:#FFF4D0;"
		"color:#002030;padding:16px;}pre{font-family:Monaco,monospace;"
		"font-size:11px;white-space:pre-wrap;color:#002030;background:"
		"#fff;border:1px solid #002030;padding:10px;}</style>"
		"</head><body><pre>";
	static const char *doc_close = "</pre></body></html>";
	size_t pfx_len, open_len, close_len;
	const char *cp;

	if (g == NULL) return;
	bw = macos9_gw_bw(g);
	if (bw == NULL) return;
	h = browser_window_get_content(bw);
	if (h == NULL) return;

	src = content_get_source_data(h, &src_size);
	if (src == NULL || src_size == 0) return;
	if (src_size > cap) src_size = cap;

	pfx_len = strlen(prefix);
	open_len = strlen(doc_open);
	close_len = strlen(doc_close);

	/* Worst case sizing:
	 *  - prefix is literal ASCII, copied as-is.
	 *  - open / close strings: each byte may percent-encode to 3.
	 *  - source bytes: each may HTML-escape to up to 5 chars (&amp;
	 *    or &quot;), then each of those may percent-encode to 3. So
	 *    up to 15 output bytes per source byte. */
	enc = (char *)malloc(pfx_len + open_len * 3 +
		src_size * 15 + close_len * 3 + 1);
	if (enc == NULL) return;

	memcpy(enc, prefix, pfx_len);
	p = enc + pfx_len;

	for (cp = doc_open; *cp != '\0'; cp++)
		enc_byte(&p, (unsigned char)*cp);

	for (i = 0; i < src_size; i++)
		html_esc(&p, src[i]);

	for (cp = doc_close; *cp != '\0'; cp++)
		enc_byte(&p, (unsigned char)*cp);

	*p = '\0';

	macos9_window_navigate(g, enc);
	free(enc);
}

/* ====================================================================
 * fixes352 (#45) - Find-in-page via a Carbon dialog
 *
 * Builds a small modal window programmatically (no DLOG/DITL resource
 * needed). The dialog has a TextEdit input field plus Find and Cancel
 * button rectangles. The event loop reads keystrokes into the TextEdit
 * and watches for Return/Esc/button clicks.
 *
 * On accept, calls browser_window_search with SEARCH_FLAG_FORWARDS.
 * Search term cached for a future Find Again wiring.
 *
 * IMPORTANT: requires `browser/netsurf/desktop/search.c` to be in
 * MacSurf.mcp for the browser_window_search symbol. content/textsearch.c
 * is already in the project.
 * ==================================================================== */
#ifdef __MACOS9__
static char macsurf_last_find_term[256] = {0};

static void c_to_pstring(const char *src, unsigned char *dest)
{
	size_t n = strlen(src);
	if (n > 255) n = 255;
	dest[0] = (unsigned char)n;
	memcpy(dest + 1, src, n);
}

static void trim_trailing_ws(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' ||
	                 s[n-1] == '\r' || s[n-1] == '\n')) {
		s[--n] = '\0';
	}
}
#endif

void macos9_find_in_page(struct gui_window *g)
{
#ifdef __MACOS9__
	WindowRef win;
	Rect win_bounds;
	Rect te_rect;
	Rect find_rect;
	Rect cancel_rect;
	TEHandle te;
	EventRecord ev;
	GrafPtr saved_port;
	bool done = false;
	bool accepted = false;
	char term[256];
	Str255 title;
	OSStatus err;

	macsurf_debug_log_writef("fixes352c find: TOP g=%p", (void *)g);
	if (g == NULL) return;

	macsurf_debug_log_writef("fixes352b find: entered g=%p", (void *)g);

	SetRect(&win_bounds, 200, 140, 580, 220);

	/* fixes352b (#45) - kWindowStandardHandlerAttribute is FORBIDDEN
	 * per CLAUDE.md Known Gotchas.
	 *
	 * fixes352d/e (#45) - kMovableModalWindowClass is rejected by
	 * MacSurf's Carbon CFM build (CarbonLib 1.x returns
	 * errInvalidWindowAttributesForClass / -5601 for every attribute
	 * combo including kWindowNoAttributes). The class itself is the
	 * problem - likely not supported on this CarbonLib.
	 *
	 * Mirror the proven URL-bar window pattern in window.c:614:
	 * CreateNewWindow(6, 0x1F, ...) i.e. kDocumentWindowClass with
	 * all standard widgets. Not technically modal (user can switch
	 * windows), but our event loop handles its own dispatch so it
	 * behaves modally enough for V1. Close box gives user a second
	 * way out beyond Cancel button. */
	err = CreateNewWindow(kDocumentWindowClass,
		kWindowCloseBoxAttribute,
		&win_bounds, &win);
	macsurf_debug_log_writef(
		"fixes352b find: CreateNewWindow err=%ld win=%p",
		(long)err, (void *)win);
	if (err != noErr || win == NULL) return;

	c_to_pstring("Find in page", title);
	SetWTitle(win, title);

	GetPort(&saved_port);
	SetPortWindowPort(win);

	SetRect(&te_rect, 12, 12, 368, 36);
	te = TENew(&te_rect, &te_rect);
	if (te == NULL) {
		SetPort(saved_port);
		DisposeWindow(win);
		return;
	}

	if (macsurf_last_find_term[0] != '\0') {
		TESetText(macsurf_last_find_term,
			(long)strlen(macsurf_last_find_term), te);
		TESetSelect(0, 32767, te);
	}

	SetRect(&find_rect,   268, 44, 368, 68);
	SetRect(&cancel_rect, 152, 44, 252, 68);

	ShowWindow(win);
	SelectWindow(win);
	TEActivate(te);

	while (!done) {
		WaitNextEvent(everyEvent, &ev, 30, NULL);
		switch (ev.what) {
		case mouseDown: {
			WindowRef which;
			short part = FindWindow(ev.where, &which);
			if (which != win) continue;
			if (part == inDrag) {
				Rect drag_bounds;
				BitMap qd_screen;
				GetQDGlobalsScreenBits(&qd_screen);
				drag_bounds = qd_screen.bounds;
				DragWindow(win, ev.where, &drag_bounds);
			} else if (part == inContent) {
				Point local = ev.where;
				GlobalToLocal(&local);
				if (PtInRect(local, &find_rect)) {
					accepted = true; done = true;
				} else if (PtInRect(local, &cancel_rect)) {
					done = true;
				} else if (PtInRect(local, &te_rect)) {
					TEClick(local, false, te);
				}
			} else if (part == inGoAway) {
				if (TrackGoAway(win, ev.where)) done = true;
			}
			break;
		}
		case keyDown:
		case autoKey: {
			char ch = (char)(ev.message & charCodeMask);
			if (ch == '\r' || ch == 0x03) {
				accepted = true; done = true;
			} else if (ch == 0x1B) {
				done = true;
			} else if ((ev.modifiers & cmdKey) &&
			           (ch == '.' || ch == 'q' || ch == 'Q')) {
				done = true;
			} else {
				TEKey(ch, te);
			}
			break;
		}
		case updateEvt:
			if ((WindowRef)ev.message == win) {
				BeginUpdate(win);
				EraseRect(&te_rect);
				FrameRect(&te_rect);
				TEUpdate(&te_rect, te);
				EraseRect(&find_rect);
				FrameRect(&find_rect);
				MoveTo(find_rect.left +
				       (find_rect.right - find_rect.left)/2 - 12,
				       find_rect.top + 16);
				DrawString("\pFind");
				EraseRect(&cancel_rect);
				FrameRect(&cancel_rect);
				MoveTo(cancel_rect.left +
				       (cancel_rect.right - cancel_rect.left)/2 - 20,
				       cancel_rect.top + 16);
				DrawString("\pCancel");
				EndUpdate(win);
			}
			break;
		case nullEvent:
			TEIdle(te);
			break;
		}
	}

	if (accepted) {
		CharsHandle ch_handle = TEGetText(te);
		long len = (*te)->teLength;
		if (len > (long)sizeof term - 1) len = (long)sizeof term - 1;
		if (len > 0) {
			HLock((Handle)ch_handle);
			memcpy(term, *(char **)ch_handle, (size_t)len);
			HUnlock((Handle)ch_handle);
		}
		term[len] = '\0';
		trim_trailing_ws(term);
	} else {
		term[0] = '\0';
	}

	TEDispose(te);
	SetPort(saved_port);
	DisposeWindow(win);

	if (accepted && term[0] != '\0') {
		struct browser_window *bw = macos9_gw_bw(g);
		strncpy(macsurf_last_find_term, term,
			sizeof macsurf_last_find_term - 1);
		macsurf_last_find_term[sizeof macsurf_last_find_term - 1] =
			'\0';
		macsurf_debug_log_writef(
			"fixes352 find: searching for '%s'", term);
		if (bw != NULL) {
			extern void browser_window_search(
				struct browser_window *bw,
				void *context,
				int flags,
				const char *string);
			extern void macos9_window_scroll_to(
				struct gui_window *g, int nx, int ny);
			/* SEARCH_FLAG_FORWARDS = 1<<1 per
			 * desktop/search.h. */
			browser_window_search(bw, NULL, 1 << 1, term);
			/* fixes352f (#45) - NetSurf textsearch auto-scrolls
			 * to the first match, but its scroll-to-position
			 * math lands at coordinates outside our content
			 * bounds (scroll=(571,335) for a content-width=949
			 * page) and the user gets stranded in empty space
			 * with no easy way back. The textsearch highlight
			 * (rendered via content_textsearch_ishighlighted)
			 * stays attached to the matched text, so resetting
			 * scroll to (0, 0) gives the user "highlighted match
			 * visible somewhere on the page, scroll normally to
			 * find it." Standard scroll arrows / page-up keys
			 * still work as expected from the top. */
			macos9_window_scroll_to(g, 0, 0);
		}
	}
#else
	(void)g;
#endif
}

/* ====================================================================
 * #48 Bookmarks - fixes645: clickable Bookmarks MENU + disk persistence
 *
 * Old behaviour (fixes351/352): a session-only array of URL strings and
 * a "Show Bookmarks" StandardAlert dump - which the user (rightly)
 * called useless: you couldn't click a bookmark to go there. This
 * replaces it with real menu integration.
 *
 * Storage is now {url, label} pairs. `label` is the page title captured
 * from the window title bar (GetWTitle) at add time, falling back to the
 * URL. The Bookmarks menu lists each entry below "Add Bookmark" and a
 * separator; selecting one navigates the front window to its URL. The
 * set round-trips to a "MacSurf Bookmarks" text file (one "url\tlabel\n"
 * record per line) via macos9_disk_cache's FSSpec I/O, so bookmarks now
 * survive relaunch.
 *
 * Menu layout (MENU_BOOKMARK):
 *   item 1  : "Add Bookmark"
 *   item 2  : separator
 *   item 3+ : one per bookmark (ITEM_BMK_FIRST = 3)
 * ==================================================================== */
#define MACSURF_BOOKMARKS_MAX 128   /* folders + bookmarks share the array */
#define MACSURF_BMK_URL_MAX   512
#define MACSURF_BMK_LBL_MAX   96

/* fixes693 (#50-adjacent bookmark UI): each record is a bookmark OR a folder.
 * `id` is a stable per-record identifier (>0); `parent_id` gives folder
 * membership (0 = root). `is_folder` records with no url are organizational
 * containers. The flat parent-id model serializes trivially and stays C89. */
struct macsurf_bookmark {
	int  id;
	int  parent_id;
	int  is_folder;
	char url[MACSURF_BMK_URL_MAX];
	char label[MACSURF_BMK_LBL_MAX];  /* bookmark label or folder name */
};
static struct macsurf_bookmark macsurf_bookmarks[MACSURF_BOOKMARKS_MAX];
static int macsurf_bookmark_count = 0;
static int macsurf_bookmark_next_id = 1;

/* Menu-position -> bookmark array index map. Folders are not shown in the
 * flat menu, so the Nth menu item is NOT the Nth array slot; navigate() maps
 * through this. Rebuilt by macos9_bookmark_menu_rebuild. */
static int macsurf_bmk_menu_map[MACSURF_BOOKMARKS_MAX];
static int macsurf_bmk_menu_n = 0;

extern long macos9_bookmarks_load(char *out_buf, long buf_cap);
extern void macos9_bookmarks_save(const char *buf, long len);

/* Serialize the in-memory set to a heap buffer (caller frees; NULL + *out_len
 * 0 when empty). Grammar (fixes693):
 *   F<TAB>id<TAB>parent<TAB>name        - a folder
 *   B<TAB>id<TAB>parent<TAB>url<TAB>label - a bookmark
 * A legacy line (no sigil, "url<TAB>label") is still READ as a root bookmark
 * by _restore, so old "MacSurf Bookmarks" files load unchanged; the writer
 * always emits the new grammar. Buffer heap-allocated - never on the stack.
 * Shared by _persist (disk save) and the manager's Export button. */
static char *macsurf_bookmarks_serialize(long *out_len)
{
	char *buf;
	size_t cap, pos = 0;
	int i;
	*out_len = 0;
	if (macsurf_bookmark_count <= 0) return NULL;
	cap = (size_t)macsurf_bookmark_count *
		(MACSURF_BMK_URL_MAX + MACSURF_BMK_LBL_MAX + 48) + 8;
	buf = (char *)malloc(cap);
	if (buf == NULL) return NULL;
	for (i = 0; i < macsurf_bookmark_count; i++) {
		struct macsurf_bookmark *b = &macsurf_bookmarks[i];
		size_t ll = strlen(b->label);
		size_t ul = b->is_folder ? 0 : strlen(b->url);
		char hdr[48];
		int hn;
		if (b->is_folder)
			hn = sprintf(hdr, "F\t%d\t%d\t", b->id, b->parent_id);
		else
			hn = sprintf(hdr, "B\t%d\t%d\t", b->id, b->parent_id);
		if (hn < 0) continue;
		if (pos + (size_t)hn + ul + ll + 3 >= cap) break;
		memcpy(buf + pos, hdr, (size_t)hn); pos += (size_t)hn;
		if (!b->is_folder) {
			memcpy(buf + pos, b->url, ul); pos += ul;
			buf[pos++] = '\t';
		}
		memcpy(buf + pos, b->label, ll); pos += ll;
		buf[pos++] = '\n';
	}
	*out_len = (long)pos;
	return buf;
}

static void macsurf_bookmarks_persist(void)
{
	char *buf;
	long len;
	buf = macsurf_bookmarks_serialize(&len);
	if (buf == NULL) {
		macos9_bookmarks_save("", 0);
		return;
	}
	macos9_bookmarks_save(buf, len);
	free(buf);
}

/* Split a NUL-terminated line into up to `maxf` fields on TAB, in place.
 * Returns the field count; fields[] point into the line. The final field
 * keeps any embedded tabs (label/name may not, but this is defensive). */
static int macsurf_bmk_split(char *line, char **fields, int maxf)
{
	int n = 0;
	char *p = line;
	while (n < maxf) {
		fields[n++] = p;
		if (n == maxf) break;   /* last field = remainder incl. tabs */
		p = strchr(p, '\t');
		if (p == NULL) break;
		*p++ = '\0';
	}
	return n;
}

/* Append ONE parsed record to the store. line is NUL-terminated and
 * TAB-delimited; grammar as in _serialize, plus the legacy "url<TAB>label"
 * root-bookmark form. Shared by _restore (fresh array) and the Import path
 * (appends to whatever is already in the array). Returns 1 if appended. */
static int macsurf_bmk_append_line(char *line)
{
	struct macsurf_bookmark *b;
	const char *url = "";
	const char *label = "";
	int rec_id = 0, rec_parent = 0, is_folder = 0, legacy = 0;
	size_t ul, ll;
	if (line[0] == '\0') return 0;
	if (macsurf_bookmark_count >= MACSURF_BOOKMARKS_MAX) return 0;

	if (line[0] == 'F' && line[1] == '\t') {
		char *f[4];
		if (macsurf_bmk_split(line, f, 4) >= 4) {
			is_folder = 1;
			rec_id = atoi(f[1]); rec_parent = atoi(f[2]);
			label = f[3];
		} else return 0;
	} else if (line[0] == 'B' && line[1] == '\t') {
		char *f[5];
		int nf = macsurf_bmk_split(line, f, 5);
		if (nf >= 4) {
			rec_id = atoi(f[1]); rec_parent = atoi(f[2]);
			url = f[3];
			label = (nf >= 5) ? f[4] : "";
		} else return 0;
	} else {
		/* legacy "url<TAB>label" - root bookmark, id assigned below */
		char *tab = strchr(line, '\t');
		legacy = 1;
		if (tab != NULL) { *tab = '\0'; url = line; label = tab + 1; }
		else { url = line; label = ""; }
	}

	if (!is_folder) {
		int dupi;
		ul = strlen(url);
		if (ul == 0 || ul >= MACSURF_BMK_URL_MAX) return 0;
		/* matches macos9_bookmark_add + macsurf_bmk_import_record: the
		 * store can never hold two records with the same URL, so a
		 * re-import of an export never doubles entries. */
		for (dupi = 0; dupi < macsurf_bookmark_count; dupi++)
			if (!macsurf_bookmarks[dupi].is_folder &&
			    strcmp(macsurf_bookmarks[dupi].url, url) == 0)
				return 0;
	}
	ll = strlen(label);
	if (ll >= MACSURF_BMK_LBL_MAX) ll = MACSURF_BMK_LBL_MAX - 1;

	b = &macsurf_bookmarks[macsurf_bookmark_count];
	b->is_folder = is_folder;
	b->parent_id = rec_parent;
	if (legacy || rec_id <= 0) b->id = macsurf_bookmark_next_id++;
	else {
		b->id = rec_id;
		if (rec_id >= macsurf_bookmark_next_id)
			macsurf_bookmark_next_id = rec_id + 1;
	}
	if (is_folder) b->url[0] = '\0';
	else { memcpy(b->url, url, strlen(url)); b->url[strlen(url)] = '\0'; }
	memcpy(b->label, label, ll); b->label[ll] = '\0';
	macsurf_bookmark_count++;
	return 1;
}

/* Parse the on-disk file back into the array. Silent no-op on failure. */
static void macsurf_bookmarks_restore(void)
{
	long n;
	char *buf;
	char *p;
	size_t cap = MACSURF_BOOKMARKS_MAX *
		(MACSURF_BMK_URL_MAX + MACSURF_BMK_LBL_MAX + 48) + 16;
	buf = (char *)malloc(cap);
	if (buf == NULL) return;
	n = macos9_bookmarks_load(buf, (long)cap);
	if (n <= 0) { free(buf); return; }
	macsurf_bookmark_count = 0;
	macsurf_bookmark_next_id = 1;
	p = buf;
	while (*p != '\0' && macsurf_bookmark_count < MACSURF_BOOKMARKS_MAX) {
		char *line = p;
		char *nl = strchr(p, '\n');
		if (nl != NULL) { *nl = '\0'; p = nl + 1; }
		else { p = line + strlen(line); }
		(void)macsurf_bmk_append_line(line);
	}
	free(buf);
}

/* ---- fixes693 mutators (used by the management window; also safe standalone) */

static int macsurf_bmk_find(int id)
{
	int i;
	for (i = 0; i < macsurf_bookmark_count; i++)
		if (macsurf_bookmarks[i].id == id) return i;
	return -1;
}

/* Rename a bookmark or folder by id. Returns 1 on success. */
int macos9_bookmark_rename(int id, const char *new_label)
{
	int i = macsurf_bmk_find(id);
	size_t ln;
	if (i < 0 || new_label == NULL) return 0;
	ln = strlen(new_label);
	if (ln >= MACSURF_BMK_LBL_MAX) ln = MACSURF_BMK_LBL_MAX - 1;
	memcpy(macsurf_bookmarks[i].label, new_label, ln);
	macsurf_bookmarks[i].label[ln] = '\0';
	macsurf_bookmarks_persist();
	macos9_bookmark_menu_rebuild();
	return 1;
}

/* Set a bookmark's URL (folders have none). Rejects empty/oversized values.
 * Returns 1 on success. */
int macos9_bookmark_set_url(int id, const char *new_url)
{
	int i = macsurf_bmk_find(id);
	size_t ul;
	if (i < 0 || new_url == NULL) return 0;
	if (macsurf_bookmarks[i].is_folder) return 0;
	ul = strlen(new_url);
	if (ul == 0 || ul >= MACSURF_BMK_URL_MAX) return 0;
	memcpy(macsurf_bookmarks[i].url, new_url, ul);
	macsurf_bookmarks[i].url[ul] = '\0';
	macsurf_bookmarks_persist();
	macos9_bookmark_menu_rebuild();
	return 1;
}

/* Delete a bookmark/folder by id. Deleting a folder re-parents its children
 * to root (parent_id 0) rather than orphaning or cascade-deleting them. */
int macos9_bookmark_delete(int id)
{
	int i = macsurf_bmk_find(id);
	int was_folder;
	int j;
	if (i < 0) return 0;
	was_folder = macsurf_bookmarks[i].is_folder;
	if (was_folder) {
		for (j = 0; j < macsurf_bookmark_count; j++)
			if (macsurf_bookmarks[j].parent_id == id)
				macsurf_bookmarks[j].parent_id = 0;
	}
	for (j = i; j < macsurf_bookmark_count - 1; j++)
		macsurf_bookmarks[j] = macsurf_bookmarks[j + 1];
	macsurf_bookmark_count--;
	macsurf_bookmarks_persist();
	macos9_bookmark_menu_rebuild();
	return 1;
}

/* Move a bookmark/folder under a new parent (0 = root). Returns 1 on
 * success. Refuses to parent an item to itself. */
int macos9_bookmark_set_parent(int id, int parent_id)
{
	int i = macsurf_bmk_find(id);
	if (i < 0 || id == parent_id) return 0;
	macsurf_bookmarks[i].parent_id = parent_id;
	macsurf_bookmarks_persist();
	macos9_bookmark_menu_rebuild();
	return 1;
}

/* Create a folder under `parent_id` (0 = root). Returns new folder id, or 0. */
int macos9_bookmark_new_folder(const char *name, int parent_id)
{
	struct macsurf_bookmark *b;
	size_t ln;
	if (name == NULL || macsurf_bookmark_count >= MACSURF_BOOKMARKS_MAX)
		return 0;
	b = &macsurf_bookmarks[macsurf_bookmark_count];
	b->id = macsurf_bookmark_next_id++;
	b->parent_id = parent_id;
	b->is_folder = 1;
	b->url[0] = '\0';
	ln = strlen(name);
	if (ln >= MACSURF_BMK_LBL_MAX) ln = MACSURF_BMK_LBL_MAX - 1;
	memcpy(b->label, name, ln); b->label[ln] = '\0';
	macsurf_bookmark_count++;
	macsurf_bookmarks_persist();
	macos9_bookmark_menu_rebuild();
	return b->id;
}

/* ====================================================================
 * IMPORT (manager-window "Import Bookmarks...").
 *
 * Accepts either our own TAB grammar or a Netscape-format HTML export
 * (the format every browser's "Export bookmarks" produces). All parsing
 * is plain C over an explicit [buf, buf+len) range - no NUL needed, no
 * case-insensitive libc (ANSI C89 only).
 * ==================================================================== */

/* Case-insensitive substring search (ANSI C89). */
static const char *macsurf_stristr(const char *hay, const char *needle)
{
	size_t hl = strlen(hay);
	size_t nl = strlen(needle);
	size_t i, j;
	if (nl == 0) return hay;
	if (nl > hl) return NULL;
	for (i = 0; i + nl <= hl; i++) {
		for (j = 0; j < nl; j++) {
			char a = hay[i + j], b = needle[j];
			if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
			if (a != b) break;
		}
		if (j == nl) return hay + i;
	}
	return NULL;
}

/* Does the tag name at p (the char after '<') match `tag` case-insensitively,
 * bounded by '>' / whitespace / '/'? */
static int bmk_tag_is(const char *p, const char *tag)
{
	size_t tl = strlen(tag);
	size_t i;
	for (i = 0; i < tl; i++) {
		char a = p[i], b = tag[i];
		if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
		if (a != b) return 0;
	}
	if (p[tl] == '\0' || p[tl] == '>' || p[tl] == ' ' || p[tl] == '\t' ||
	    p[tl] == '\n' || p[tl] == '\r' || p[tl] == '/') return 1;
	return 0;
}

/* Extract attribute `attr` from an opening-tag string (p = after '<').
 * Case-insensitive attribute NAME; the value is verbatim (URLs are
 * case-sensitive). Handles "x=y" quoted or bare. Returns 1 on success. */
static int bmk_attr_get(const char *p, const char *attr, char *out, int cap)
{
	size_t al = strlen(attr);
	size_t n;
	if (cap <= 1) return 0;
	for (;;) {
		const char *nm;
		size_t nml = 0;
		size_t i;
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
		nm = p;
		while (p[nml] != '\0' && p[nml] != '=' && p[nml] != '>' &&
		       p[nml] != ' ' && p[nml] != '\t' && p[nml] != '\n' &&
		       p[nml] != '\r' && p[nml] != '/') nml++;
		if (nml == 0) break;
		p += nml;
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
		if (*p != '=') continue;      /* bare name, no value - skip */
		p++;
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
		if (nml == al) {
			int eq = 1;
			for (i = 0; i < nml; i++) {
				char ca = nm[i], cb = attr[i];
				if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
				if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
				if (ca != cb) { eq = 0; break; }
			}
			if (eq) {
				n = 0;
				if (*p == '"' || *p == '\'') {
					char qc = *p++;
					while (*p != '\0' && *p != qc && n + 1 < (size_t)cap)
						out[n++] = *p++;
					out[n] = '\0';
				} else {
					while (*p != '\0' && *p != '>' && *p != ' ' &&
					       *p != '\t' && *p != '\n' && *p != '\r' &&
					       n + 1 < (size_t)cap) out[n++] = *p++;
					out[n] = '\0';
				}
				return 1;
			}
		}
		/* not our attribute - skip this value, keep scanning */
		if (*p == '"' || *p == '\'') {
			char qc = *p++;
			while (*p != '\0' && *p != qc) p++;
			if (*p == qc) p++;
		} else {
			while (*p != '\0' && *p != '>' && *p != ' ' && *p != '\t' &&
			       *p != '\n' && *p != '\r') p++;
		}
	}
	return 0;
}

/* Copy a text run [s, end) into out, decoding the HTML entities Netscape
 * exports use (&amp; &lt; &gt; &quot; &apos; &#NN; &#xHH;) and collapsing
 * whitespace. Writes at most cap-1 chars, NUL-terminates. */
static void bmk_text_run(const char *s, const char *end, char *out, int cap)
{
	size_t n = 0;
	int pending_space = 0;
	while (s < end && n + 1 < (size_t)cap) {
		if (*s == '&') {
			const char *t = s + 1;
			if (t + 4 <= end && t[0] == 'a' && t[1] == 'm' &&
			    t[2] == 'p' && t[3] == ';') {
				if (pending_space && n > 0 && n + 1 < (size_t)cap)
					out[n++] = ' ';
				pending_space = 0;
				out[n++] = '&'; s = t + 4; continue;
			}
			if (t + 3 <= end && t[0] == 'l' && t[1] == 't' &&
			    t[2] == ';') {
				if (pending_space && n > 0 && n + 1 < (size_t)cap)
					out[n++] = ' ';
				pending_space = 0;
				out[n++] = '<'; s = t + 3; continue;
			}
			if (t + 3 <= end && t[0] == 'g' && t[1] == 't' &&
			    t[2] == ';') {
				if (pending_space && n > 0 && n + 1 < (size_t)cap)
					out[n++] = ' ';
				pending_space = 0;
				out[n++] = '>'; s = t + 3; continue;
			}
			if (t + 5 <= end && t[0] == 'q' && t[1] == 'u' &&
			    t[2] == 'o' && t[3] == 't' && t[4] == ';') {
				if (pending_space && n > 0 && n + 1 < (size_t)cap)
					out[n++] = ' ';
				pending_space = 0;
				out[n++] = '"'; s = t + 5; continue;
			}
			if (t + 5 <= end && t[0] == 'a' && t[1] == 'p' &&
			    t[2] == 'o' && t[3] == 's' && t[4] == ';') {
				if (pending_space && n > 0 && n + 1 < (size_t)cap)
					out[n++] = ' ';
				pending_space = 0;
				out[n++] = '\''; s = t + 5; continue;
			}
			if (t + 2 <= end && t[0] == '#') {
				char *ep = NULL;
				long v;
				if (t[1] == 'x' || t[1] == 'X') v = strtol(t + 2, &ep, 16);
				else v = strtol(t + 1, &ep, 10);
				if (ep != NULL && *ep == ';' && v > 0 && v < 256) {
					if (pending_space && n > 0 &&
					    n + 1 < (size_t)cap) out[n++] = ' ';
					pending_space = 0;
					out[n++] = (char)v;
					s = ep + 1;
					continue;
				}
			}
			if (pending_space && n > 0 && n + 1 < (size_t)cap)
				out[n++] = ' ';
			pending_space = 0;
			out[n++] = '&';
			s++;
			continue;
		}
		if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
			pending_space = 1;
			s++;
			continue;
		}
		if (pending_space && n > 0 && n + 1 < (size_t)cap) out[n++] = ' ';
		pending_space = 0;
		out[n++] = *s;
		s++;
	}
	if (pending_space && n > 0 && n + 1 < (size_t)cap) out[n++] = ' ';
	out[n] = '\0';
}

/* Add an imported bookmark under `parent` (0 = root). Dedupes by URL across
 * the whole store so re-importing a file never doubles entries. Returns the
 * new id, or 0 when skipped/full. */
static int macsurf_bmk_import_record(const char *url, const char *label,
		int parent)
{
	struct macsurf_bookmark *b;
	size_t ul, ll;
	int i;
	if (url == NULL || url[0] == '\0') return 0;
	ul = strlen(url);
	if (ul >= MACSURF_BMK_URL_MAX) return 0;
	if (macsurf_bookmark_count >= MACSURF_BOOKMARKS_MAX) return 0;
	for (i = 0; i < macsurf_bookmark_count; i++)
		if (!macsurf_bookmarks[i].is_folder &&
		    strcmp(macsurf_bookmarks[i].url, url) == 0) return 0;
	b = &macsurf_bookmarks[macsurf_bookmark_count];
	b->id = macsurf_bookmark_next_id++;
	b->parent_id = parent;
	b->is_folder = 0;
	memcpy(b->url, url, ul); b->url[ul] = '\0';
	ll = (label != NULL) ? strlen(label) : 0;
	if (ll >= MACSURF_BMK_LBL_MAX) ll = MACSURF_BMK_LBL_MAX - 1;
	if (ll > 0) memcpy(b->label, label, ll);
	b->label[ll] = '\0';
	macsurf_bookmark_count++;
	return b->id;
}

/* Create an imported folder under `parent`. Returns the new folder id, or 0
 * when the store is full. */
static int macsurf_bmk_import_folder(const char *name, int parent)
{
	struct macsurf_bookmark *b;
	size_t ln;
	if (macsurf_bookmark_count >= MACSURF_BOOKMARKS_MAX) return 0;
	b = &macsurf_bookmarks[macsurf_bookmark_count];
	b->id = macsurf_bookmark_next_id++;
	b->parent_id = parent;
	b->is_folder = 1;
	b->url[0] = '\0';
	ln = (name != NULL) ? strlen(name) : 0;
	if (ln >= MACSURF_BMK_LBL_MAX) ln = MACSURF_BMK_LBL_MAX - 1;
	if (ln > 0) memcpy(b->label, name, ln);
	b->label[ln] = '\0';
	macsurf_bookmark_count++;
	return b->id;
}

/* Import a Netscape-format HTML bookmark file over the explicit range
 * [in, in+len). <DT><A HREF="u">label</A> → bookmark; <DT><H3>name</H3>
 * followed by <DL>…</DL> → folder with children; </DL> pops. Returns the
 * number of records added. */
static int macsurf_bmk_import_html(const char *in, long len)
{
	const char *end = in + len;
	const char *p = in;
	int fstack[32];
	int fsp = 0;
	int added = 0;
	char pend_name[MACSURF_BMK_LBL_MAX];
	int have_pend = 0;
	pend_name[0] = '\0';

	while (p < end) {
		const char *lt = (const char *)memchr(p, '<', (size_t)(end - p));
		const char *gt;
		const char *txt_end;
		if (lt == NULL) break;
		p = lt + 1;
		if (p >= end) break;
		if (*p == '!') {
			/* comment / doctype - skip to '>' */
			gt = (const char *)memchr(p, '>', (size_t)(end - p));
			if (gt == NULL) break;
			p = gt + 1;
			continue;
		}
		if (*p == '/') {
			/* closing tag */
			if (bmk_tag_is(p + 1, "dl") && fsp > 0) {
				int pid = fstack[--fsp];
				if (have_pend) {   /* folder with no <DL>: create */
					if (macsurf_bmk_import_folder(pend_name,
							pid) != 0) added++;
					have_pend = 0;
				}
			}
			gt = (const char *)memchr(p, '>', (size_t)(end - p));
			if (gt == NULL) break;
			p = gt + 1;
			continue;
		}
		if (bmk_tag_is(p, "a")) {
			char href[MACSURF_BMK_URL_MAX];
			char label[MACSURF_BMK_LBL_MAX];
			int parent = (fsp > 0) ? fstack[fsp - 1] : 0;
			gt = (const char *)memchr(p, '>', (size_t)(end - p));
			if (gt == NULL) break;
			txt_end = gt + 1;
			{
				const char *lt2 = (const char *)memchr(txt_end, '<',
					(size_t)(end - txt_end));
				if (lt2 != NULL) txt_end = lt2;
			}
			if (bmk_attr_get(p, "href", href, (int)sizeof href) &&
			    href[0] != '\0') {
				bmk_text_run(gt + 1, txt_end, label,
					(int)sizeof label);
				if (macsurf_bmk_import_record(href, label,
						parent) != 0) added++;
			}
			p = txt_end;
			continue;
		}
		if (bmk_tag_is(p, "h3")) {
			gt = (const char *)memchr(p, '>', (size_t)(end - p));
			if (gt == NULL) break;
			txt_end = gt + 1;
			{
				const char *lt2 = (const char *)memchr(txt_end, '<',
					(size_t)(end - txt_end));
				if (lt2 != NULL) txt_end = lt2;
			}
			bmk_text_run(gt + 1, txt_end, pend_name,
				(int)sizeof pend_name);
			have_pend = 1;
			p = txt_end;
			continue;
		}
		if (bmk_tag_is(p, "dl")) {
			int parent = (fsp > 0) ? fstack[fsp - 1] : 0;
			if (have_pend) {
				int fid = macsurf_bmk_import_folder(pend_name,
					parent);
				if (fid != 0) {
					if (fsp < 32) fstack[fsp++] = fid;
					added++;
				}
				have_pend = 0;
			} else if (fsp < 32) {
				fstack[fsp++] = parent;
			}
			gt = (const char *)memchr(p, '>', (size_t)(end - p));
			if (gt == NULL) break;
			p = gt + 1;
			continue;
		}
		/* any other tag (dt, p, hr, meta, h1, ...) - skip it */
		gt = (const char *)memchr(p, '>', (size_t)(end - p));
		if (gt == NULL) break;
		p = gt + 1;
	}
	if (have_pend) {   /* folder with no <DL> - create at stack top */
		int parent = (fsp > 0) ? fstack[fsp - 1] : 0;
		if (macsurf_bmk_import_folder(pend_name, parent) != 0) added++;
	}
	return added;
}

/* Import from a buffer: our own TAB grammar, or a Netscape HTML export.
 * Appends to the store (no reset), persists, rebuilds the menu. Returns the
 * number of records added. Grammar sniff: the first non-blank line containing
 * a TAB means our format (HTML exports have none). */
int macos9_bookmarks_import_buffer(const char *buf, long len)
{
	const char *end;
	const char *p;
	int tab_grammar = 0;
	int added = 0;
	if (buf == NULL || len <= 0) return 0;
	end = buf + len;
	p = buf;
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	if (p < end) {
		const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
		const char *line_end = (nl != NULL) ? nl : end;
		if (memchr(p, '\t', (size_t)(line_end - p)) != NULL)
			tab_grammar = 1;
	}
	if (tab_grammar) {
		char *copy = (char *)malloc((size_t)(end - buf) + 1);
		char *q, *line;
		if (copy == NULL) return 0;
		memcpy(copy, buf, (size_t)(end - buf));
		copy[end - buf] = '\0';
		q = copy;
		while (*q != '\0' && macsurf_bookmark_count < MACSURF_BOOKMARKS_MAX) {
			line = q;
			{
				char *nl = strchr(q, '\n');
				if (nl != NULL) { *nl = '\0'; q = nl + 1; }
				else q = line + strlen(line);
			}
			{
				size_t ll2 = strlen(line);   /* strip CRLF \r */
				if (ll2 > 0 && line[ll2 - 1] == '\r')
					line[ll2 - 1] = '\0';
			}
			if (macsurf_bmk_append_line(line)) added++;
		}
		free(copy);
	} else {
		added = macsurf_bmk_import_html(buf, len);
	}
	if (added > 0) {
		macsurf_bookmarks_persist();
		macos9_bookmark_menu_rebuild();
	}
	return added;
}

/* fixes707 - hierarchical Bookmarks menu. Folders were previously skipped
 * entirely (they "vanished"); now each folder is a real submenu holding its
 * bookmarks. Root-level bookmarks stay as direct items in the parent menu.
 *
 * Carbon hierarchical menus: create a submenu with a unique ID, InsertMenu
 * it with hierMenu(-1), then mark the parent item with hMenuCmd + the
 * submenu ID so the Menu Manager pops it. A submenu selection comes back
 * from MenuSelect as (submenu-ID, item), dispatched via
 * macos9_bookmark_submenu_navigate. We never call CountMItems (absent on
 * this SDK); item indices are tracked by hand. */
#ifndef hMenuCmd
#define hMenuCmd 0x1B
#endif
#ifndef hierMenu
#define hierMenu (-1)
#endif

#ifdef __MACOS9__
static MenuHandle bmk_submenus[MENU_BMK_SUB_MAX];
static int        bmk_submenu_folderid[MENU_BMK_SUB_MAX];
static int        bmk_submenu_count = 0;
#endif

void macos9_bookmark_menu_rebuild(void)
{
#ifdef __MACOS9__
	static int prev_dynamic = 0;
	MenuHandle m = GetMenuHandle(MENU_BOOKMARK);
	int i;
	short item_index;
	if (m == NULL) return;
	/* Delete the previously-appended parent items (from the first dynamic
	 * slot; deleting it repeatedly collapses the block). */
	while (prev_dynamic > 0) { DeleteMenuItem(m, ITEM_BMK_FIRST); prev_dynamic--; }
	/* Tear down last round's submenus. */
	for (i = 0; i < bmk_submenu_count; i++) {
		if (bmk_submenus[i] != NULL) {
			DeleteMenu((short)(MENU_BMK_SUB_BASE + i));
			DisposeMenu(bmk_submenus[i]);
			bmk_submenus[i] = NULL;
		}
	}
	bmk_submenu_count = 0;

	macsurf_bmk_menu_n = 0;
	item_index = (short)(ITEM_BMK_FIRST - 1);   /* last fixed item (separator) */

	/* Root-level bookmarks (parent_id == 0) as direct items. */
	for (i = 0; i < macsurf_bookmark_count; i++) {
		struct macsurf_bookmark *b = &macsurf_bookmarks[i];
		Str255 pt;
		const char *s;
		size_t ln;
		if (b->is_folder || b->parent_id != 0) continue;
		s = (b->label[0] != '\0') ? b->label : b->url;
		ln = strlen(s); if (ln > 80) ln = 80;
		pt[0] = (unsigned char)ln;
		memcpy(pt + 1, s, ln);
		AppendMenu(m, "\px");
		item_index++;
		SetMenuItemText(m, item_index, pt);
		macsurf_bmk_menu_map[macsurf_bmk_menu_n++] = i;
	}

	/* Each folder as a hierarchical submenu of its bookmarks. */
	for (i = 0; i < macsurf_bookmark_count &&
			bmk_submenu_count < MENU_BMK_SUB_MAX; i++) {
		struct macsurf_bookmark *f = &macsurf_bookmarks[i];
		Str255 pt;
		const char *nm;
		size_t ln;
		MenuHandle sub;
		short subID;
		short sub_item;
		int j;
		if (!f->is_folder) continue;
		nm = (f->label[0] != '\0') ? f->label : "(folder)";
		ln = strlen(nm); if (ln > 80) ln = 80;
		pt[0] = (unsigned char)ln;
		memcpy(pt + 1, nm, ln);
		AppendMenu(m, "\px");
		item_index++;
		SetMenuItemText(m, item_index, pt);
		macsurf_bmk_menu_map[macsurf_bmk_menu_n++] = -1;   /* folder marker */

		subID = (short)(MENU_BMK_SUB_BASE + bmk_submenu_count);
		sub = NewMenu(subID, "\px");
		if (sub == NULL) continue;    /* fall back: folder shows, no submenu */
		sub_item = 0;
		for (j = 0; j < macsurf_bookmark_count; j++) {
			struct macsurf_bookmark *bb = &macsurf_bookmarks[j];
			Str255 sp;
			const char *ss;
			size_t sl;
			if (bb->is_folder || bb->parent_id != f->id) continue;
			ss = (bb->label[0] != '\0') ? bb->label : bb->url;
			sl = strlen(ss); if (sl > 80) sl = 80;
			sp[0] = (unsigned char)sl;
			memcpy(sp + 1, ss, sl);
			AppendMenu(sub, "\px");
			sub_item++;
			SetMenuItemText(sub, sub_item, sp);
		}
		if (sub_item == 0) AppendMenu(sub, "\p(empty");
		InsertMenu(sub, hierMenu);
		SetItemCmd(m, item_index, hMenuCmd);
		SetItemMark(m, item_index, (short)subID);
		bmk_submenus[bmk_submenu_count] = sub;
		bmk_submenu_folderid[bmk_submenu_count] = f->id;
		bmk_submenu_count++;
	}

	if (macsurf_bmk_menu_n == 0) {
		AppendMenu(m, "\p(No bookmarks yet");
		prev_dynamic = 1;
		return;
	}
	prev_dynamic = macsurf_bmk_menu_n;
#endif
}

/* Navigate to the bookmark chosen from a folder SUBMENU. MenuSelect returns
 * the submenu's ID + the 1-based item within it. */
void macos9_bookmark_submenu_navigate(struct gui_window *g, int menu_id, int item)
{
#ifdef __MACOS9__
	int seq = menu_id - MENU_BMK_SUB_BASE;
	int folder_id, j, k = 0;
	if (g == NULL || item < 1) return;
	if (seq < 0 || seq >= bmk_submenu_count) return;
	folder_id = bmk_submenu_folderid[seq];
	for (j = 0; j < macsurf_bookmark_count; j++) {
		struct macsurf_bookmark *bb = &macsurf_bookmarks[j];
		if (bb->is_folder || bb->parent_id != folder_id) continue;
		k++;
		if (k == item) { macos9_window_navigate(g, bb->url); return; }
	}
#else
	(void)g; (void)menu_id; (void)item;
#endif
}

/* Navigate the front window to the bookmark backing a given menu item
 * (item numbers ITEM_BMK_FIRST.. map to bookmark index 0..). */
void macos9_bookmark_navigate(struct gui_window *g, int menu_item)
{
	int menu_pos = menu_item - ITEM_BMK_FIRST;
	int idx;
	if (g == NULL) return;
	if (menu_pos < 0 || menu_pos >= macsurf_bmk_menu_n) return;
	idx = macsurf_bmk_menu_map[menu_pos];   /* fixes693: map past folders */
	if (idx < 0 || idx >= macsurf_bookmark_count) return;
	macos9_window_navigate(g, macsurf_bookmarks[idx].url);
}

void macos9_bookmark_add(struct gui_window *g)
{
	struct browser_window *bw;
	struct nsurl *u;
	const char *href;
	int i;
	if (g == NULL) return;
	bw = macos9_gw_bw(g);
	if (bw == NULL) return;
	u = browser_window_access_url(bw);
	if (u == NULL) return;
	href = nsurl_access(u);
	if (href == NULL || strlen(href) >= MACSURF_BMK_URL_MAX) return;
	for (i = 0; i < macsurf_bookmark_count; i++) {
		if (strcmp(macsurf_bookmarks[i].url, href) == 0) return;
	}
	if (macsurf_bookmark_count >= MACSURF_BOOKMARKS_MAX) return;
	macsurf_bookmarks[macsurf_bookmark_count].id = macsurf_bookmark_next_id++;
	macsurf_bookmarks[macsurf_bookmark_count].parent_id = 0;
	macsurf_bookmarks[macsurf_bookmark_count].is_folder = 0;
	strcpy(macsurf_bookmarks[macsurf_bookmark_count].url, href);
	macsurf_bookmarks[macsurf_bookmark_count].label[0] = '\0';
#ifdef __MACOS9__
	/* Label = current page title from the window title bar. */
	if (g->window != NULL) {
		Str255 wt;
		size_t ln;
		GetWTitle(g->window, wt);
		ln = wt[0];
		if (ln > MACSURF_BMK_LBL_MAX - 1) ln = MACSURF_BMK_LBL_MAX - 1;
		memcpy(macsurf_bookmarks[macsurf_bookmark_count].label,
			wt + 1, ln);
		macsurf_bookmarks[macsurf_bookmark_count].label[ln] = '\0';
	}
#endif
	macsurf_bookmark_count++;
	macsurf_bookmarks_persist();
	macos9_bookmark_menu_rebuild();
}

/* Startup hook: load persisted bookmarks and populate the menu. Called
 * from main.c after the menu bar is built. */
void macos9_bookmarks_init(void)
{
	macsurf_bookmarks_restore();
	macos9_bookmark_menu_rebuild();
}

/* ====================================================================
 * HISTORY (fixes694 → fixes698, #47).
 *
 * fixes694 read NetSurf's urldb, but urldb is session-only (its disk
 * (de)serializer is never wired - only the cookie jar is) and exposes no
 * clear API, so it can neither persist across launches nor be cleared.
 * fixes698 replaces it with MacSurf's own persistent store: an array of
 * {timestamp, url, title}, most-recent-first, deduped by URL, saved to a
 * "MacSurf History" file in the MacSurfData root (macos9_disk_cache.c).
 *
 * Recorded from window.c's title-set path (macos9_history_record), where
 * the committed URL and the human page title are both known. The menu
 * shows the most recent MACSURF_HIST_MENU_SHOW entries; the manager
 * window (fixes699) shows all of them, day-grouped, and Clear empties it.
 * ==================================================================== */
#define MACSURF_HISTORY_MAX    400   /* persisted cap */
#define MACSURF_HIST_MENU_SHOW  30   /* entries listed in the menu */
#define MACSURF_HIST_URL_MAX   512
#define MACSURF_HIST_TTL_MAX   100

struct macsurf_hist_ent {
	long ts;                         /* Mac seconds (GetDateTime) */
	char url[MACSURF_HIST_URL_MAX];
	char title[MACSURF_HIST_TTL_MAX];
};
static struct macsurf_hist_ent macsurf_hist[MACSURF_HISTORY_MAX];
static int macsurf_hist_n = 0;

extern long macos9_history_load(char *out_buf, long buf_cap);
extern void macos9_history_save(const char *buf, long len);

/* Only real navigable pages belong in history. */
static int macsurf_hist_url_ok(const char *u)
{
	if (u == NULL || u[0] == '\0') return 0;
	if (strncmp(u, "http://", 7) == 0)  return 1;
	if (strncmp(u, "https://", 8) == 0) return 1;
	return 0;
}

/* Serialize the store to the on-disk file, one "ts<TAB>url<TAB>title\n"
 * record per line. Heap buffer - never on the stack. */
static void macsurf_history_persist(void)
{
	char *buf;
	size_t cap, pos = 0;
	int i;
	if (macsurf_hist_n <= 0) { macos9_history_save("", 0); return; }
	cap = (size_t)macsurf_hist_n *
		(MACSURF_HIST_URL_MAX + MACSURF_HIST_TTL_MAX + 24) + 8;
	buf = (char *)malloc(cap);
	if (buf == NULL) return;
	for (i = 0; i < macsurf_hist_n; i++) {
		struct macsurf_hist_ent *e = &macsurf_hist[i];
		char hdr[24];
		int hn = sprintf(hdr, "%ld\t", e->ts);
		size_t ul = strlen(e->url);
		size_t tl = strlen(e->title);
		if (hn < 0) continue;
		if (pos + (size_t)hn + ul + tl + 3 >= cap) break;
		memcpy(buf + pos, hdr, (size_t)hn); pos += (size_t)hn;
		memcpy(buf + pos, e->url, ul); pos += ul;
		buf[pos++] = '\t';
		memcpy(buf + pos, e->title, tl); pos += tl;
		buf[pos++] = '\n';
	}
	macos9_history_save(buf, (long)pos);
	free(buf);
}

/* Parse the on-disk file back into the store. Silent no-op on failure. */
static void macsurf_history_restore(void)
{
	long n;
	char *buf;
	char *p;
	size_t cap = MACSURF_HISTORY_MAX *
		(MACSURF_HIST_URL_MAX + MACSURF_HIST_TTL_MAX + 24) + 16;
	buf = (char *)malloc(cap);
	if (buf == NULL) return;
	n = macos9_history_load(buf, (long)cap);
	if (n <= 0) { free(buf); return; }
	macsurf_hist_n = 0;
	p = buf;
	while (*p != '\0' && macsurf_hist_n < MACSURF_HISTORY_MAX) {
		char *line = p;
		char *nl = strchr(p, '\n');
		char *t1, *t2;
		struct macsurf_hist_ent *e;
		size_t ul, tl;
		if (nl != NULL) { *nl = '\0'; p = nl + 1; }
		else { p = line + strlen(line); }
		if (line[0] == '\0') continue;
		t1 = strchr(line, '\t');
		if (t1 == NULL) continue;
		*t1++ = '\0';
		t2 = strchr(t1, '\t');
		if (t2 != NULL) *t2++ = '\0'; else t2 = (char *)"";
		if (!macsurf_hist_url_ok(t1)) continue;
		ul = strlen(t1);
		if (ul >= MACSURF_HIST_URL_MAX) continue;
		tl = strlen(t2);
		if (tl >= MACSURF_HIST_TTL_MAX) tl = MACSURF_HIST_TTL_MAX - 1;
		e = &macsurf_hist[macsurf_hist_n];
		e->ts = atol(line);
		memcpy(e->url, t1, ul); e->url[ul] = '\0';
		memcpy(e->title, t2, tl); e->title[tl] = '\0';
		macsurf_hist_n++;
	}
	free(buf);
}

/* Record a visit. Called from window.c's title-set path. Deduped by URL
 * (an existing entry moves to the front and refreshes its timestamp +
 * title), newest at index 0, oldest dropped when the store is full. */
void macos9_history_record(struct gui_window *g, const char *title)
{
	struct browser_window *bw;
	struct nsurl *u;
	const char *href;
	const char *ttl = (title != NULL) ? title : "";
	long now = 0;
	int i, found = -1;
	size_t ul, tl;
	struct macsurf_hist_ent tmp;

	if (g == NULL) return;
	bw = macos9_gw_bw(g);
	if (bw == NULL) return;
	u = browser_window_access_url(bw);
	if (u == NULL) return;
	href = nsurl_access(u);
	if (!macsurf_hist_url_ok(href)) return;
	ul = strlen(href);
	if (ul >= MACSURF_HIST_URL_MAX) return;

#ifdef __MACOS9__
	{
		unsigned long secs = 0;
		GetDateTime(&secs);
		now = (long)secs;
	}
#endif

	for (i = 0; i < macsurf_hist_n; i++) {
		if (strcmp(macsurf_hist[i].url, href) == 0) { found = i; break; }
	}

	if (found < 0) {
		/* new entry - make room at the front */
		if (macsurf_hist_n < MACSURF_HISTORY_MAX) macsurf_hist_n++;
		for (i = macsurf_hist_n - 1; i > 0; i--)
			macsurf_hist[i] = macsurf_hist[i - 1];
		found = 0;
		memcpy(macsurf_hist[0].url, href, ul);
		macsurf_hist[0].url[ul] = '\0';
	} else if (found > 0) {
		/* existing - pull it to the front, preserving its url */
		tmp = macsurf_hist[found];
		for (i = found; i > 0; i--)
			macsurf_hist[i] = macsurf_hist[i - 1];
		macsurf_hist[0] = tmp;
		found = 0;
	}

	tl = strlen(ttl);
	if (tl >= MACSURF_HIST_TTL_MAX) tl = MACSURF_HIST_TTL_MAX - 1;
	memcpy(macsurf_hist[0].title, ttl, tl);
	macsurf_hist[0].title[tl] = '\0';
	macsurf_hist[0].ts = now;

	macsurf_history_persist();
	macos9_history_menu_rebuild();
}

/* Empty the entire history store (menu item + manager-window button). */
void macos9_history_clear(void)
{
	macsurf_hist_n = 0;
	macsurf_history_persist();
	macos9_history_menu_rebuild();
}

/* Delete ONE history entry by store index (manager-window Delete button).
 * Shifts the tail down, persists, rebuilds the menu. */
void macos9_history_delete_entry(int i)
{
	int j;
	if (i < 0 || i >= macsurf_hist_n) return;
	for (j = i; j < macsurf_hist_n - 1; j++)
		macsurf_hist[j] = macsurf_hist[j + 1];
	macsurf_hist_n--;
	macsurf_history_persist();
	macos9_history_menu_rebuild();
}

/* fixes706 - Clear Cache menu handler: wipe the disk cache (cached bodies +
 * the old deadhosts.txt) and the in-memory dead-host state, then report the
 * count. Bookmarks / history / cookies (MacSurfData root) are untouched. */
void macos9_cache_clear_ui(void)
{
#ifdef __MACOS9__
	extern long macos9_cache_clear(void);
	extern void macos9_https_forget_all(void);
	long n;
	Str255 msg;
	char buf[80];
	short item;
	n = macos9_cache_clear();
	macos9_https_forget_all();
	sprintf(buf, "Cleared %ld cached file%s from disk.",
		n, (n == 1) ? "" : "s");
	c_to_pstring(buf, msg);
	StandardAlert(kAlertNoteAlert, msg, "\p", NULL, &item);
#else
	extern long macos9_cache_clear(void);
	(void)macos9_cache_clear();
#endif
}

/* Read-only accessors for the manager window (fixes699), same TU. */
int  macos9_history_count(void) { return macsurf_hist_n; }
long macos9_history_entry_ts(int i)
{ return (i >= 0 && i < macsurf_hist_n) ? macsurf_hist[i].ts : 0; }
const char *macos9_history_entry_url(int i)
{ return (i >= 0 && i < macsurf_hist_n) ? macsurf_hist[i].url : ""; }
const char *macos9_history_entry_title(int i)
{ return (i >= 0 && i < macsurf_hist_n) ? macsurf_hist[i].title : ""; }

/* Rebuild the dynamic portion of the History menu (items >= ITEM_HIST_FIRST;
 * item 1 = Clear History, item 2 = separator are fixed, appended in main.c).
 * Same dynamic-item discipline as the bookmark menu (track prev_dynamic;
 * never CountMenuItems; AppendMenu placeholder + SetMenuItemText). */
void macos9_history_menu_rebuild(void)
{
#ifdef __MACOS9__
	static int prev_dynamic = 0;
	MenuHandle m = GetMenuHandle(MENU_HISTORY);
	int i, shown;
	short item_index;
	if (m == NULL) return;
	while (prev_dynamic > 0) { DeleteMenuItem(m, ITEM_HIST_FIRST); prev_dynamic--; }

	if (macsurf_hist_n == 0) {
		AppendMenu(m, "\p(No history yet");
		prev_dynamic = 1;
		return;
	}
	item_index = (short)(ITEM_HIST_FIRST - 1);
	shown = (macsurf_hist_n < MACSURF_HIST_MENU_SHOW)
		? macsurf_hist_n : MACSURF_HIST_MENU_SHOW;
	for (i = 0; i < shown; i++) {
		Str255 pt;
		const char *s = (macsurf_hist[i].title[0] != '\0') ?
			macsurf_hist[i].title : macsurf_hist[i].url;
		size_t ln = strlen(s);
		if (ln > 80) ln = 80;
		pt[0] = (unsigned char)ln;
		memcpy(pt + 1, s, ln);
		AppendMenu(m, "\px");
		item_index++;
		SetMenuItemText(m, item_index, pt);
	}
	prev_dynamic = shown;
#endif
}

/* Navigate the front window to the history entry backing a menu item
 * (items ITEM_HIST_FIRST.. map to store index 0..). */
void macos9_history_navigate(struct gui_window *g, int menu_item)
{
	int idx = menu_item - ITEM_HIST_FIRST;
	if (g == NULL) return;
	if (idx < 0 || idx >= macsurf_hist_n) return;
	macos9_window_navigate(g, macsurf_hist[idx].url);
}

/* Startup hook: load the persisted history and populate the menu. */
void macos9_history_init(void)
{
	macsurf_history_restore();
	macos9_history_menu_rebuild();
}

/* ====================================================================
 * fixes699 (#47) - History MANAGER WINDOW.
 *
 * A self-contained modal window (built programmatically, no DLOG/DITL,
 * mirroring macos9_find_in_page's proven pattern). Lists every stored
 * visit, grouped under day headers (Today / Yesterday / "Wed, Jul 8,
 * 2026"), newest first. Select a row and press Go / double-click to
 * navigate; Clear History empties the store; Done / close box dismiss.
 *
 * Scrolling is keyboard (arrows / PageUp-Down / Home-End) plus two
 * on-window scroll arrows - deliberately NOT a Carbon scrollbar CDEF,
 * which crashes on real G3/G4 hardware (see CLAUDE.md Known Gotchas).
 * ==================================================================== */
#ifdef __MACOS9__

/* chrome_confirm_delete lives in the bookmark-manager section below; the
 * History window (defined first) needs it for its Delete button. */
static int chrome_confirm_delete(const char *msg);

/* Read the TE's text into out (NUL-terminated). */
static void chrome_te_get_text(TEHandle te, char *out, int cap)
{
	CharsHandle h = TEGetText(te);
	long len = (*te)->teLength;
	if (len > (long)cap - 1) len = (long)cap - 1;
	if (len > 0) {
		HLock((Handle)h);
		memcpy(out, *(char **)h, (size_t)len);
		HUnlock((Handle)h);
	}
	out[len] = '\0';
}

/* Draw the "Find:" label + a bordered search field. chrome_mgr_header leaves
 * TextSize(15) in effect for the row/button text, so the 12pt label restores
 * it. labx is the label's left edge in window-local coords. */
static void chrome_draw_search_field(const Rect *r, short labx)
{
	RGBColor blk;
	RGBColor saved_fg;
	blk.red = blk.green = blk.blue = 0;
	GetForeColor(&saved_fg);
	EraseRect(r);
	FrameRect(r);
	RGBForeColor(&blk);
	TextSize(12);
	MoveTo(labx, (short)(r->top + 14));
	DrawString("\pFind:");
	TextSize(15);
	RGBForeColor(&saved_fg);
}

/* One rendered line: a day header, or a visit (hidx into macsurf_hist[]). */
struct hw_row {
	short is_header;
	short hidx;
	char  text[160];
};

/* Centered rounded push button with a Pascal label. fixes710 - rounded
 * corners read as a real Mac push button rather than a bare box. Shared by
 * both manager windows. */
/* fixes742 - vertical gradient fill (8-bit endpoint colours) for the manager
 * windows' shine. */
static void chrome_vgrad(const Rect *r, int r0, int g0, int b0,
		int r1, int g1, int b1)
{
	short y;
	short h = (short)(r->bottom - r->top);
	RGBColor c;
	Rect ln;
	if (h <= 0) return;
	ln.left = r->left;
	ln.right = r->right;
	for (y = 0; y < h; y++) {
		int rv = r0 + (r1 - r0) * y / h;
		int gv = g0 + (g1 - g0) * y / h;
		int bv = b0 + (b1 - b0) * y / h;
		c.red   = (unsigned short)((rv << 8) | rv);
		c.green = (unsigned short)((gv << 8) | gv);
		c.blue  = (unsigned short)((bv << 8) | bv);
		RGBForeColor(&c);
		ln.top = (short)(r->top + y);
		ln.bottom = (short)(ln.top + 1);
		PaintRect(&ln);
	}
}

/* fixes745 - decode a PNG into a 32-bit colour GWorld + a 1-bit mask BitMap
 * (from alpha) for CopyMask compositing over the gradient banner. Returns 1 on
 * success. Mirrors about_logo_ensure. */
static int mgr_icon_build(const unsigned char *png, unsigned long len,
		GWorldPtr *out_gw, BitMap *out_mask, Rect *out_src)
{
	extern unsigned lodepng_decode32(unsigned char **out, unsigned *w,
			unsigned *h, const unsigned char *in, unsigned long insize);
	unsigned char *rgba = NULL;
	unsigned w = 0, h = 0, err;
	OSErr oerr;
	GWorldPtr saved_port;
	GDHandle saved_gdh;
	PixMapHandle pm;
	long dst_rb, mask_rb, row, col;
	unsigned char *src_row, *dst_row, *mrow;

	*out_gw = NULL;
	out_mask->baseAddr = NULL;
	err = lodepng_decode32(&rgba, &w, &h, png, len);
	if (err != 0 || rgba == NULL || w == 0 || h == 0) {
		if (rgba != NULL) free(rgba);
		return 0;
	}
	SetRect(out_src, 0, 0, (short)w, (short)h);
	GetGWorld(&saved_port, &saved_gdh);
	oerr = NewGWorld(out_gw, 32, out_src, NULL, NULL, 0);
	if (oerr != noErr || *out_gw == NULL) {
		free(rgba); SetGWorld(saved_port, saved_gdh); return 0;
	}
	pm = GetGWorldPixMap(*out_gw);
	if (pm == NULL || !LockPixels(pm)) {
		DisposeGWorld(*out_gw); *out_gw = NULL;
		free(rgba); SetGWorld(saved_port, saved_gdh); return 0;
	}
	dst_rb = (long)((*pm)->rowBytes & 0x3FFF);
	for (row = 0; row < (long)h; row++) {
		src_row = rgba + row * (long)w * 4L;
		dst_row = (unsigned char *)GetPixBaseAddr(pm) + row * dst_rb;
		for (col = 0; col < (long)w; col++) {
			dst_row[col*4+0] = 0xFF;
			dst_row[col*4+1] = src_row[col*4+0];
			dst_row[col*4+2] = src_row[col*4+1];
			dst_row[col*4+3] = src_row[col*4+2];
		}
	}
	UnlockPixels(pm);
	SetGWorld(saved_port, saved_gdh);
	mask_rb = (((long)w + 15) / 16) * 2;
	out_mask->baseAddr = NewPtrClear(mask_rb * (long)h);
	if (out_mask->baseAddr == NULL) {
		DisposeGWorld(*out_gw); *out_gw = NULL; free(rgba); return 0;
	}
	out_mask->rowBytes = (short)mask_rb;
	out_mask->bounds = *out_src;
	for (row = 0; row < (long)h; row++) {
		src_row = rgba + row * (long)w * 4L;
		mrow = (unsigned char *)out_mask->baseAddr + row * mask_rb;
		for (col = 0; col < (long)w; col++) {
			if (src_row[col*4+3] >= 128)
				mrow[col >> 3] |= (unsigned char)(0x80 >> (col & 7));
		}
	}
	free(rgba);
	return 1;
}

/* Lazy-decoded banner icons (id 1 = History, 2 = Bookmarks). */
static GWorldPtr s_mgr_ic_gw[3];
static BitMap    s_mgr_ic_mask[3];
static Rect      s_mgr_ic_src[3];
static int       s_mgr_ic_tried[3];

static void mgr_icon_ensure(int id)
{
	if (id < 1 || id > 2 || s_mgr_ic_tried[id]) return;
	s_mgr_ic_tried[id] = 1;
	if (id == 1)
		(void)mgr_icon_build(macos9_mgr_hist_png, macos9_mgr_hist_png_len,
			&s_mgr_ic_gw[id], &s_mgr_ic_mask[id], &s_mgr_ic_src[id]);
	else
		(void)mgr_icon_build(macos9_mgr_bm_png, macos9_mgr_bm_png_len,
			&s_mgr_ic_gw[id], &s_mgr_ic_mask[id], &s_mgr_ic_src[id]);
}

/* fixes742/745 - a shiny gold gradient title banner across the top of a manager
 * window, with an icon (id 1=History, 2=Bookmarks, 0=none) + bold white title.
 * content is the window's local rect. */
static void chrome_mgr_header(const Rect *content, const char *title, int icon)
{
	Rect band;
	Rect ln;
	RGBColor saved_fg;
	RGBColor white;
	RGBColor accent;
	white.red = white.green = white.blue = 0xFFFF;
	accent.red = 0x8C8C; accent.green = 0x5A5A; accent.blue = 0x1010;  /* dark amber */
	band.left = content->left;
	band.right = content->right;
	band.top = content->top;
	band.bottom = (short)(content->top + 34);   /* fixes744 - dialed back */
	GetForeColor(&saved_fg);
	/* fixes744 - MacSurf gold scheme (softened orange) instead of blue */
	chrome_vgrad(&band, 0xF0, 0xA8, 0x40, 0xD2, 0x82, 0x1E);
	ln.left = band.left; ln.right = band.right;
	ln.top = (short)(band.bottom - 1); ln.bottom = band.bottom;
	RGBForeColor(&accent); PaintRect(&ln);
	{
		short tx = (short)(band.left + 14);
		if (icon >= 1 && icon <= 2) {
			mgr_icon_ensure(icon);
			if (s_mgr_ic_gw[icon] != NULL) {
				GrafPtr gp;
				Rect ir;
				short isz = MACOS9_MGR_ICON_SIZE;
				ir.left = (short)(band.left + 12);
				ir.top = (short)(band.top + (34 - isz) / 2);
				ir.right = (short)(ir.left + isz);
				ir.bottom = (short)(ir.top + isz);
				GetPort(&gp);
				{
					const BitMap *src = GetPortBitMapForCopyBits(
						(CGrafPtr)s_mgr_ic_gw[icon]);
					const BitMap *dst = GetPortBitMapForCopyBits(
						(CGrafPtr)gp);
					RGBColor blk, wht2;
					blk.red = blk.green = blk.blue = 0;
					wht2.red = wht2.green = wht2.blue = 0xFFFF;
					RGBForeColor(&blk); RGBBackColor(&wht2);
					if (src != NULL && dst != NULL)
						CopyMask(src, &s_mgr_ic_mask[icon], dst,
							&s_mgr_ic_src[icon],
							&s_mgr_ic_src[icon], &ir);
				}
				tx = (short)(ir.right + 8);
			}
		}
		RGBForeColor(&white);
		TextFont(1); TextFace(bold); TextSize(15);
		MoveTo(tx, (short)(band.top + 23));
		DrawText(title, 0, (short)strlen(title));
		TextFace(normal);
	}
	RGBForeColor(&saved_fg);
}

/* fixes742 - shiny rounded button: a light top-lit gradient clipped to the
 * round-rect, a crisp frame, centred label. */
static void chrome_draw_button(const Rect *r, ConstStr255Param label)
{
	short bw = (short)(r->right - r->left);
	short tw = StringWidth(label);
	RGBColor blk;
	RGBColor saved_fg;
	RgnHandle btn = NewRgn();
	RgnHandle sav = NewRgn();
	blk.red = blk.green = blk.blue = 0;
	GetForeColor(&saved_fg);
	EraseRect(r);
	if (btn != NULL && sav != NULL) {
		GetClip(sav);
		OpenRgn();
		FrameRoundRect(r, 10, 10);
		CloseRgn(btn);
		SetClip(btn);
		chrome_vgrad(r, 0xFD, 0xF6, 0xE6, 0xF0, 0xD8, 0xAC);  /* fixes745 - gold */
		SetClip(sav);
	}
	RGBForeColor(&blk);
	PenSize(1, 1);
	FrameRoundRect(r, 10, 10);
	MoveTo((short)(r->left + (bw - tw) / 2),
	       (short)(r->top + (r->bottom - r->top) / 2 + 4));
	DrawString(label);
	RGBForeColor(&saved_fg);
	if (btn != NULL) DisposeRgn(btn);
	if (sav != NULL) DisposeRgn(sav);
}

/* Format a day header from a Mac-seconds timestamp relative to today. */
static void chrome_day_header(long ts, long today_day, char *out)
{
	static const char *wd[7] =
		{ "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	static const char *mo[12] =
		{ "Jan", "Feb", "Mar", "Apr", "May", "Jun",
		  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	long day = ts / 86400L;
	DateTimeRec dtr;
	const char *wds;
	const char *mos;
	if (day == today_day)     { strcpy(out, "Today");     return; }
	if (day == today_day - 1) { strcpy(out, "Yesterday"); return; }
	SecondsToDate((unsigned long)ts, &dtr);
	wds = (dtr.dayOfWeek >= 1 && dtr.dayOfWeek <= 7) ? wd[dtr.dayOfWeek - 1] : "";
	mos = (dtr.month >= 1 && dtr.month <= 12) ? mo[dtr.month - 1] : "";
	sprintf(out, "%s, %s %d, %d", wds, mos,
		(int)dtr.day, (int)dtr.year);
}

/* (Re)build the flat display-row list from the store. `filter` (may be NULL
 * or empty) keeps only entries whose title OR url matches (case-insensitive
 * substring). Returns row count. */
static int hw_build_rows(struct hw_row *rows, int cap, long today_day,
		const char *filter)
{
	int i, n = 0;
	long prev_day = 0x7FFFFFFFL;
	int have_filter = (filter != NULL && filter[0] != '\0');
	for (i = 0; i < macsurf_hist_n && n < cap - 1; i++) {
		long day = macsurf_hist[i].ts / 86400L;
		const char *ttl = macsurf_hist[i].title;
		const char *url = macsurf_hist[i].url;
		if (have_filter && macsurf_stristr(ttl, filter) == NULL &&
		    macsurf_stristr(url, filter) == NULL) continue;
		if (n == 0 || day != prev_day) {
			rows[n].is_header = 1;
			rows[n].hidx = -1;
			chrome_day_header(macsurf_hist[i].ts, today_day, rows[n].text);
			n++;
			prev_day = day;
			if (n >= cap - 1) break;
		}
		rows[n].is_header = 0;
		rows[n].hidx = (short)i;
		if (ttl[0] != '\0') {
			size_t tl = strlen(ttl);
			if (tl > 150) tl = 150;
			memcpy(rows[n].text, ttl, tl);
			rows[n].text[tl] = '\0';
		} else {
			size_t ul = strlen(url);
			if (ul > 150) ul = 150;
			memcpy(rows[n].text, url, ul);
			rows[n].text[ul] = '\0';
		}
		n++;
	}
	return n;
}

/* First entry row at or after `from` (skips headers); -1 if none. */
static int hw_next_entry(struct hw_row *rows, int nrows, int from)
{
	int r;
	for (r = from; r < nrows; r++)
		if (!rows[r].is_header) return r;
	return -1;
}
static int hw_prev_entry(struct hw_row *rows, int from)
{
	int r;
	for (r = from; r >= 0; r--)
		if (!rows[r].is_header) return r;
	return -1;
}

void macos9_history_window_show(struct gui_window *g)
{
	WindowRef win;
	Rect wb, list, up, dn, clr, del, go, done, search_rect;
	GrafPtr saved_port;
	EventRecord ev;
	struct hw_row *rows;
	int cap, nrows;
	int scroll_top = 0, sel = -1;
	int row_h = 20, vis;              /* fixes742 - taller rows, more padding */
	long today_day = 0;
	int done_flag = 0;
	char go_url[MACSURF_HIST_URL_MAX];
	int last_click_row = -1;
	unsigned long last_click_time = 0;
	int dirty = 1;
	TEHandle te_search = NULL;
	int search_focus = 0;
	char filter[128];

	if (g == NULL) return;
	go_url[0] = '\0';
	filter[0] = '\0';

	{
		unsigned long secs = 0;
		GetDateTime(&secs);
		today_day = (long)secs / 86400L;
	}

	cap = macsurf_hist_n * 2 + 4;
	rows = (struct hw_row *)malloc((size_t)cap * sizeof(struct hw_row));
	if (rows == NULL) return;

	SetRect(&wb, 120, 90, 640, 490);
	if (CreateNewWindow(kDocumentWindowClass, kWindowCloseBoxAttribute,
			&wb, &win) != noErr || win == NULL) {
		free(rows);
		return;
	}
	{ Str255 t; c_to_pstring("History", t); SetWTitle(win, t); }

	GetPort(&saved_port);
	SetPortWindowPort(win);
	TextFont(1);   /* application font (Geneva) */
	TextSize(12);  /* fixes742 - larger row text */

	/* content is 520 x 400 local. Search field + list below the 34px
	 * title banner; the filter narrows rows to title/URL matches. */
	SetRect(&search_rect, 48, 40, 512, 60);
	SetRect(&list, 8, 64, 512, 344);
	SetRect(&up,   494, 64,  512, 86);
	SetRect(&dn,   494, 322, 512, 344);
	SetRect(&clr,  8,   352, 128, 376);
	SetRect(&del,  132, 352, 192, 376);
	SetRect(&go,   300, 352, 380, 376);
	SetRect(&done, 420, 352, 512, 376);
	vis = (list.bottom - list.top - 4) / row_h;

	te_search = TENew(&search_rect, &search_rect);
	if (te_search == NULL) {
		SetPort(saved_port);
		DisposeWindow(win);
		free(rows);
		return;
	}

	nrows = hw_build_rows(rows, cap, today_day, filter);
	sel = hw_next_entry(rows, nrows, 0);

	ShowWindow(win);
	SelectWindow(win);

	while (!done_flag) {
		WaitNextEvent(everyEvent, &ev, 30, NULL);
		switch (ev.what) {
		case updateEvt:
			if ((WindowRef)ev.message == win) {
				BeginUpdate(win);
				EndUpdate(win);   /* validate; shared draw repaints */
				dirty = 1;
			} else {
				/* fixes709 - repaint an uncovered background browser
				 * window (dragging left it white). Restore our port. */
				extern void macos9_handle_update(const EventRecord *event);
				macos9_handle_update(&ev);
				SetPortWindowPort(win);
			}
			break;
		case mouseDown: {
			WindowRef which;
			short part = FindWindow(ev.where, &which);
			Point lp;
			if (which != win) break;
			if (part == inDrag) {
				Rect db; BitMap sb;
				GetQDGlobalsScreenBits(&sb);
				db = sb.bounds;
				DragWindow(win, ev.where, &db);
				break;
			}
			if (part == inGoAway) {
				if (TrackGoAway(win, ev.where)) done_flag = 1;
				break;
			}
			if (part != inContent) break;
			lp = ev.where;
			GlobalToLocal(&lp);
			if (PtInRect(lp, &done)) { done_flag = 1; break; }
			if (PtInRect(lp, &clr)) {
				macos9_history_clear();
				nrows = hw_build_rows(rows, cap, today_day, filter);
				sel = hw_next_entry(rows, nrows, 0);
				scroll_top = 0;
				last_click_row = -1;
				break;
			}
			if (PtInRect(lp, &del)) {
				if (sel >= 0 && sel < nrows && !rows[sel].is_header) {
					int hi = rows[sel].hidx;
					if (hi >= 0 && hi < macsurf_hist_n &&
					    chrome_confirm_delete(
						"Delete this history entry?")) {
						macos9_history_delete_entry(hi);
						nrows = hw_build_rows(rows, cap,
							today_day, filter);
						if (sel >= nrows) sel = nrows - 1;
						if (nrows == 0) sel = -1;
						if (scroll_top > nrows - vis)
							scroll_top = nrows - vis;
						if (scroll_top < 0) scroll_top = 0;
					}
				}
				break;
			}
			if (PtInRect(lp, &go)) {
				if (sel >= 0 && sel < nrows && !rows[sel].is_header) {
					int hi = rows[sel].hidx;
					if (hi >= 0 && hi < macsurf_hist_n) {
						strcpy(go_url, macsurf_hist[hi].url);
						done_flag = 1;
					}
				}
				break;
			}
			if (PtInRect(lp, &up)) {
				scroll_top -= 3;
				if (scroll_top < 0) scroll_top = 0;
				break;
			}
			if (PtInRect(lp, &dn)) {
				int maxtop = nrows - vis;
				if (maxtop < 0) maxtop = 0;
				scroll_top += 3;
				if (scroll_top > maxtop) scroll_top = maxtop;
				break;
			}
			if (PtInRect(lp, &search_rect)) {
				if (!search_focus) {
					search_focus = 1;
					TEActivate(te_search);
				}
				TEClick(lp, false, te_search);
				break;
			}
			if (PtInRect(lp, &list)) {
				int idx = scroll_top + (lp.v - (list.top + 2)) / row_h;
				if (search_focus) {
					search_focus = 0;
					TEDeactivate(te_search);
				}
				if (idx >= 0 && idx < nrows && !rows[idx].is_header) {
					if (idx == last_click_row &&
					    (ev.when - last_click_time) <= GetDblTime()) {
						int hi = rows[idx].hidx;
						if (hi >= 0 && hi < macsurf_hist_n) {
							strcpy(go_url, macsurf_hist[hi].url);
							done_flag = 1;
						}
					}
					sel = idx;
					last_click_row = idx;
					last_click_time = ev.when;
				}
			}
			break;
		}
		case keyDown:
		case autoKey: {
			char ch = (char)(ev.message & charCodeMask);
			if ((ev.modifiers & cmdKey) &&
			    (ch == '.' || ch == 'w' || ch == 'W')) {
				done_flag = 1;
			} else if (search_focus && ch == 0x09) {
				/* Tab leaves the search field */
				search_focus = 0;
				TEDeactivate(te_search);
			} else if (search_focus && ch == 0x1B) {
				/* Esc clears the filter first, then closes */
				if (filter[0] != '\0') {
					filter[0] = '\0';
					TESetSelect(0, 32767, te_search);
					TESetText("", 0, te_search);
					nrows = hw_build_rows(rows, cap,
						today_day, filter);
					scroll_top = 0;
					sel = hw_next_entry(rows, nrows, 0);
				} else {
					done_flag = 1;
				}
			} else if (search_focus && (ch == 0x0D || ch == 0x03)) {
				/* Return in the field = Go */
				if (sel >= 0 && sel < nrows && !rows[sel].is_header) {
					int hi = rows[sel].hidx;
					if (hi >= 0 && hi < macsurf_hist_n) {
						strcpy(go_url, macsurf_hist[hi].url);
						done_flag = 1;
					}
				}
			} else if (search_focus) {
				int was_empty = (filter[0] == '\0');
				if (ch == 0x1F || ch == 0x1E || ch == 0x0C ||
				    ch == 0x0B || ch == 0x01 || ch == 0x04) {
					/* list navigation still works while typing */
					if (ch == 0x1F) {
						int ns = hw_next_entry(rows, nrows,
							(sel < 0) ? 0 : sel + 1);
						if (ns >= 0) sel = ns;
					} else if (ch == 0x1E) {
						int ps = hw_prev_entry(rows,
							(sel <= 0) ? 0 : sel - 1);
						if (ps >= 0) sel = ps;
					} else if (ch == 0x0C) {
						scroll_top += vis - 1;
					} else if (ch == 0x0B) {
						scroll_top -= vis - 1;
					} else if (ch == 0x01) {
						scroll_top = 0;
						sel = hw_next_entry(rows, nrows, 0);
					} else {
						sel = hw_prev_entry(rows, nrows - 1);
						scroll_top = nrows - vis;
					}
				} else if ((ch >= 0x20 && ch < 0x7F) || ch == 0x08) {
					/* printable / backspace - edit, re-filter */
					TEKey(ch, te_search);
					chrome_te_get_text(te_search, filter,
						(int)sizeof filter);
					nrows = hw_build_rows(rows, cap,
						today_day, filter);
					if (was_empty || nrows == 0) scroll_top = 0;
					sel = hw_next_entry(rows, nrows, 0);
				} else {
					TEKey(ch, te_search);
				}
			} else if (ch == 0x09) {
				/* Tab enters the search field */
				search_focus = 1;
				TEActivate(te_search);
			} else if (ch == 0x1B) {          /* Esc */
				done_flag = 1;
			} else if (ch == 0x0D || ch == 0x03) { /* Return / Enter */
				if (sel >= 0 && sel < nrows && !rows[sel].is_header) {
					int hi = rows[sel].hidx;
					if (hi >= 0 && hi < macsurf_hist_n) {
						strcpy(go_url, macsurf_hist[hi].url);
						done_flag = 1;
					}
				}
			} else if (ch == 0x1F) {          /* Down arrow */
				int ns = hw_next_entry(rows, nrows,
					(sel < 0) ? 0 : sel + 1);
				if (ns >= 0) sel = ns;
			} else if (ch == 0x1E) {          /* Up arrow */
				int ps = hw_prev_entry(rows, (sel <= 0) ? 0 : sel - 1);
				if (ps >= 0) sel = ps;
			} else if (ch == 0x0C) {          /* Page Down */
				scroll_top += vis - 1;
			} else if (ch == 0x0B) {          /* Page Up */
				scroll_top -= vis - 1;
			} else if (ch == 0x01) {          /* Home */
				scroll_top = 0; sel = hw_next_entry(rows, nrows, 0);
			} else if (ch == 0x04) {          /* End */
				sel = hw_prev_entry(rows, nrows - 1);
				scroll_top = nrows - vis;
			}
			if (scroll_top < 0) scroll_top = 0;
			{
				int maxtop = nrows - vis;
				if (maxtop < 0) maxtop = 0;
				if (scroll_top > maxtop) scroll_top = maxtop;
			}
			/* keep selection visible */
			if (sel >= 0) {
				if (sel < scroll_top) scroll_top = sel;
				else if (sel >= scroll_top + vis)
					scroll_top = sel - vis + 1;
			}
			break;
		}
		default:
			break;
		}

		/* Any user event may have changed scroll/selection/contents. */
		if (ev.what == mouseDown || ev.what == keyDown ||
		    ev.what == autoKey)
			dirty = 1;

		/* Repaint only when something changed (no idle flicker). */
		if (!done_flag && dirty) {
			RgnHandle saveclip = NewRgn();
			Rect tr;
			int r, y;
			GetClip(saveclip);
			{ Rect content; SetRect(&content, 0, 0, 520, 400);
			  chrome_mgr_header(&content, "History", 1); }
			/* search field (label + frame, then the TE's text) */
			chrome_draw_search_field(&search_rect, 6);
			TEUpdate(&search_rect, te_search);
			EraseRect(&list);
			FrameRect(&list);
			ClipRect(&list);
			y = list.top + 2;
			for (r = scroll_top;
			     r < nrows && (r - scroll_top) < vis; r++) {
				short len = (short)strlen(rows[r].text);
				RGBColor blk, wht;
				blk.red = blk.green = blk.blue = 0;
				wht.red = wht.green = wht.blue = 0xFFFF;
				tr.left = (short)(list.left + 1);
				tr.right = (short)(list.right - 20);
				tr.top = (short)y;
				tr.bottom = (short)(y + row_h);
				if (rows[r].is_header) {
					RGBColor hc;
					hc.red = 0x7A7A; hc.green = 0x4E4E; hc.blue = 0x1414;
					chrome_vgrad(&tr, 0xFB, 0xF0, 0xDC, 0xF4, 0xE2, 0xC0);
					RGBForeColor(&hc);
					TextFace(bold);
					MoveTo((short)(list.left + 6), (short)(y + 14));
					DrawText(rows[r].text, 0, len);
					TextFace(normal);
					RGBForeColor(&blk);
				} else {
					if (r == sel) {
						RGBColor selc;
						selc.red = 0xE8E8; selc.green = 0x9E9E; selc.blue = 0x3838;
						RGBForeColor(&selc); PaintRect(&tr);
					} else if (((r - scroll_top) & 1) != 0) {
						RGBColor st;
						st.red = 0xFDFD; st.green = 0xF8F8; st.blue = 0xEFEF;
						RGBForeColor(&st); PaintRect(&tr);
					}
					/* per-site favicon dot - green = visited. V1 has no
					 * per-host icon cache, so the dot is a fixed colour
					 * (window.c's per-window favicon GWorlds are private). */
					{
						Rect dot;
						RGBColor grn;
						dot.left = (short)(list.left + 7);
						dot.top = (short)(y + 7);
						dot.right = (short)(dot.left + 7);
						dot.bottom = (short)(dot.top + 7);
						grn.red = 0x3030; grn.green = 0xE0E0;
						grn.blue = 0x3030;
						RGBForeColor(&grn);
						PaintOval(&dot);
					}
					if (r == sel) RGBForeColor(&wht);
					else RGBForeColor(&blk);
					MoveTo((short)(list.left + 22), (short)(y + 14));
					DrawText(rows[r].text, 0, len);
					RGBForeColor(&blk);
				}
				y += row_h;
			}
			SetClip(saveclip);
			DisposeRgn(saveclip);
			/* scroll arrows */
			EraseRect(&up); FrameRect(&up);
			MoveTo(up.left + 6, up.top + 15); DrawString("\p^");
			EraseRect(&dn); FrameRect(&dn);
			MoveTo(dn.left + 6, dn.top + 15); DrawString("\pv");
			/* buttons */
			chrome_draw_button(&clr, "\pClear History");
			chrome_draw_button(&del, "\pDelete");
			chrome_draw_button(&go, "\pGo");
			chrome_draw_button(&done, "\pDone");
			if (nrows == 0) {
				MoveTo(list.left + 12, list.top + 24);
				DrawString((filter[0] != '\0') ? "\p(No matches)"
					: "\p(No history yet)");
			}
			dirty = 0;
		}
	}

	SetPort(saved_port);
	TEDispose(te_search);
	DisposeWindow(win);
	free(rows);

	if (go_url[0] != '\0')
		macos9_window_navigate(g, go_url);
}

#else  /* !__MACOS9__ */
void macos9_history_window_show(struct gui_window *g) { (void)g; }
#endif

/* ====================================================================
 * fixes700 (#50) - Bookmark MANAGER WINDOW.
 *
 * Modal window over the same list/scroll/draw pattern as the History
 * window, presenting the two-level folder tree (root bookmarks, then
 * each folder and its children indented). Buttons: New Folder, Rename,
 * Delete (with confirm), Move (cycles a bookmark's parent folder), Go,
 * Done. Rename / New Folder collect text via chrome_prompt_text, a
 * reusable TextEdit modal. The folder model + mutators are fixes693.
 * ==================================================================== */
#ifdef __MACOS9__

/* Reusable single-line text prompt (mirrors macos9_find_in_page's TE
 * dialog). Returns 1 with the entered text in out[] (accepted, non-empty),
 * else 0. */
static int chrome_prompt_text(const char *title, const char *initial,
	char *out, int outcap)
{
	WindowRef win;
	Rect wb, te_rect, ok_rect, cancel_rect;
	TEHandle te;
	EventRecord ev;
	GrafPtr saved;
	Str255 pt;
	int done = 0, accepted = 0;

	SetRect(&wb, 200, 170, 560, 262);
	if (CreateNewWindow(kDocumentWindowClass, kWindowCloseBoxAttribute,
			&wb, &win) != noErr || win == NULL)
		return 0;
	c_to_pstring(title, pt);
	SetWTitle(win, pt);
	GetPort(&saved);
	SetPortWindowPort(win);
	TextFont(1);
	TextSize(10);

	SetRect(&te_rect, 12, 30, 348, 50);
	te = TENew(&te_rect, &te_rect);
	if (te == NULL) { SetPort(saved); DisposeWindow(win); return 0; }
	if (initial != NULL && initial[0] != '\0') {
		TESetText(initial, (long)strlen(initial), te);
		TESetSelect(0, 32767, te);
	}
	SetRect(&ok_rect, 260, 58, 348, 82);
	SetRect(&cancel_rect, 150, 58, 250, 82);

	ShowWindow(win);
	SelectWindow(win);
	TEActivate(te);

	while (!done) {
		WaitNextEvent(everyEvent, &ev, 20, NULL);
		switch (ev.what) {
		case mouseDown: {
			WindowRef which;
			short part = FindWindow(ev.where, &which);
			if (which != win) break;
			if (part == inDrag) {
				Rect db; BitMap sb;
				GetQDGlobalsScreenBits(&sb);
				db = sb.bounds;
				DragWindow(win, ev.where, &db);
			} else if (part == inGoAway) {
				if (TrackGoAway(win, ev.where)) done = 1;
			} else if (part == inContent) {
				Point lp = ev.where;
				GlobalToLocal(&lp);
				if (PtInRect(lp, &ok_rect)) { accepted = 1; done = 1; }
				else if (PtInRect(lp, &cancel_rect)) done = 1;
				else if (PtInRect(lp, &te_rect)) TEClick(lp, false, te);
			}
			break;
		}
		case keyDown:
		case autoKey: {
			char ch = (char)(ev.message & charCodeMask);
			if (ch == '\r' || ch == 0x03) { accepted = 1; done = 1; }
			else if (ch == 0x1B) done = 1;
			else if ((ev.modifiers & cmdKey) && ch == '.') done = 1;
			else TEKey(ch, te);
			break;
		}
		case updateEvt:
			if ((WindowRef)ev.message == win) {
				BeginUpdate(win);
				EraseRect(&te_rect); FrameRect(&te_rect);
				TEUpdate(&te_rect, te);
				EraseRect(&ok_rect); FrameRect(&ok_rect);
				MoveTo(ok_rect.left + 34, ok_rect.top + 16);
				DrawString("\pOK");
				EraseRect(&cancel_rect); FrameRect(&cancel_rect);
				MoveTo(cancel_rect.left + 26, cancel_rect.top + 16);
				DrawString("\pCancel");
				EndUpdate(win);
			}
			break;
		case nullEvent:
			TEIdle(te);
			break;
		default:
			break;
		}
	}

	if (accepted) {
		CharsHandle h = TEGetText(te);
		long len = (*te)->teLength;
		if (len > (long)outcap - 1) len = (long)outcap - 1;
		if (len > 0) {
			HLock((Handle)h);
			memcpy(out, *(char **)h, (size_t)len);
			HUnlock((Handle)h);
		}
		out[len] = '\0';
	} else {
		out[0] = '\0';
	}

	TEDispose(te);
	SetPort(saved);
	DisposeWindow(win);
	return accepted && out[0] != '\0';
}

/* Two-field text prompt (Name / URL) - the Edit Bookmark dialog. Tab moves
 * between the fields; Return accepts. Returns 1 with both fields filled in
 * (out_name non-empty; out_url may be empty - macos9_bookmark_set_url does
 * the real validation), else 0. */
static int chrome_prompt_text2(const char *title, const char *init_name,
		const char *init_url, char *out_name, int namecap,
		char *out_url, int urlcap)
{
	WindowRef win;
	Rect wb, name_rect, url_rect, ok_rect, cancel_rect;
	TEHandle te_name, te_url;
	EventRecord ev;
	GrafPtr saved;
	Str255 pt;
	int done = 0, accepted = 0;
	int active_field = 0;   /* 0 = name, 1 = url */

	SetRect(&wb, 200, 170, 560, 310);
	if (CreateNewWindow(kDocumentWindowClass, kWindowCloseBoxAttribute,
			&wb, &win) != noErr || win == NULL)
		return 0;
	c_to_pstring(title, pt);
	SetWTitle(win, pt);
	GetPort(&saved);
	SetPortWindowPort(win);
	TextFont(1);
	TextSize(10);

	SetRect(&name_rect, 60, 30, 348, 50);
	SetRect(&url_rect,  60, 62, 348, 82);
	te_name = TENew(&name_rect, &name_rect);
	if (te_name == NULL) { SetPort(saved); DisposeWindow(win); return 0; }
	te_url = TENew(&url_rect, &url_rect);
	if (te_url == NULL) {
		TEDispose(te_name);
		SetPort(saved);
		DisposeWindow(win);
		return 0;
	}
	if (init_name != NULL && init_name[0] != '\0') {
		TESetText(init_name, (long)strlen(init_name), te_name);
		TESetSelect(0, 32767, te_name);
	}
	if (init_url != NULL && init_url[0] != '\0') {
		TESetText(init_url, (long)strlen(init_url), te_url);
		TESetSelect(0, 32767, te_url);
	}
	SetRect(&ok_rect, 260, 100, 348, 124);
	SetRect(&cancel_rect, 150, 100, 250, 124);

	ShowWindow(win);
	SelectWindow(win);
	TEActivate(te_name);

	while (!done) {
		WaitNextEvent(everyEvent, &ev, 20, NULL);
		switch (ev.what) {
		case mouseDown: {
			WindowRef which;
			short part = FindWindow(ev.where, &which);
			if (which != win) break;
			if (part == inDrag) {
				Rect db; BitMap sb;
				GetQDGlobalsScreenBits(&sb);
				db = sb.bounds;
				DragWindow(win, ev.where, &db);
			} else if (part == inGoAway) {
				if (TrackGoAway(win, ev.where)) done = 1;
			} else if (part == inContent) {
				Point lp = ev.where;
				GlobalToLocal(&lp);
				if (PtInRect(lp, &ok_rect)) { accepted = 1; done = 1; }
				else if (PtInRect(lp, &cancel_rect)) done = 1;
				else if (PtInRect(lp, &name_rect)) {
					active_field = 0;
					TEDeactivate(te_url);
					TEActivate(te_name);
					TEClick(lp, false, te_name);
				} else if (PtInRect(lp, &url_rect)) {
					active_field = 1;
					TEDeactivate(te_name);
					TEActivate(te_url);
					TEClick(lp, false, te_url);
				}
			}
			break;
		}
		case keyDown:
		case autoKey: {
			char ch = (char)(ev.message & charCodeMask);
			if (ch == '\r' || ch == 0x03) { accepted = 1; done = 1; }
			else if (ch == 0x1B) done = 1;
			else if ((ev.modifiers & cmdKey) && ch == '.') done = 1;
			else if (ch == 0x09) {   /* Tab switches fields */
				if (active_field == 0) {
					active_field = 1;
					TEDeactivate(te_name);
					TEActivate(te_url);
					TESetSelect(0, 32767, te_url);
				} else {
					active_field = 0;
					TEDeactivate(te_url);
					TEActivate(te_name);
					TESetSelect(0, 32767, te_name);
				}
			} else if (active_field == 0) TEKey(ch, te_name);
			else TEKey(ch, te_url);
			break;
		}
		case updateEvt:
			if ((WindowRef)ev.message == win) {
				RGBColor blk;
				BeginUpdate(win);
				blk.red = blk.green = blk.blue = 0;
				RGBForeColor(&blk);
				TextSize(10);
				MoveTo(10, 42); DrawString("\pName:");
				MoveTo(10, 74); DrawString("\pURL:");
				EraseRect(&name_rect); FrameRect(&name_rect);
				TEUpdate(&name_rect, te_name);
				EraseRect(&url_rect); FrameRect(&url_rect);
				TEUpdate(&url_rect, te_url);
				EraseRect(&ok_rect); FrameRect(&ok_rect);
				MoveTo(ok_rect.left + 34, ok_rect.top + 16);
				DrawString("\pOK");
				EraseRect(&cancel_rect); FrameRect(&cancel_rect);
				MoveTo(cancel_rect.left + 26, cancel_rect.top + 16);
				DrawString("\pCancel");
				EndUpdate(win);
			}
			break;
		case nullEvent:
			TEIdle(te_name);
			TEIdle(te_url);
			break;
		default:
			break;
		}
	}

	if (accepted) {
		CharsHandle h = TEGetText(te_name);
		long len = (*te_name)->teLength;
		if (len > (long)namecap - 1) len = (long)namecap - 1;
		if (len > 0) {
			HLock((Handle)h);
			memcpy(out_name, *(char **)h, (size_t)len);
			HUnlock((Handle)h);
		}
		out_name[len] = '\0';
		h = TEGetText(te_url);
		len = (*te_url)->teLength;
		if (len > (long)urlcap - 1) len = (long)urlcap - 1;
		if (len > 0) {
			HLock((Handle)h);
			memcpy(out_url, *(char **)h, (size_t)len);
			HUnlock((Handle)h);
		}
		out_url[len] = '\0';
	} else {
		out_name[0] = '\0';
		out_url[0] = '\0';
	}

	TEDispose(te_name);
	TEDispose(te_url);
	SetPort(saved);
	DisposeWindow(win);
	return accepted && out_name[0] != '\0';
}

/* Caution alert with Delete / Cancel. Returns 1 if the user confirms. */
static int chrome_confirm_delete(const char *msg)
{
	Str255 p;
	SInt16 item = 0;
	AlertStdAlertParamRec par;
	c_to_pstring(msg, p);
	par.movable = false;
	par.helpButton = false;
	par.filterProc = NULL;
	par.defaultText = (StringPtr)"\pDelete";
	par.cancelText = (StringPtr)"\pCancel";
	par.otherText = NULL;
	par.defaultButton = kAlertStdAlertOKButton;
	par.cancelButton = kAlertStdAlertCancelButton;
	par.position = kWindowDefaultPosition;
	StandardAlert(kAlertCautionAlert, p, "\p", &par, &item);
	return item == kAlertStdAlertOKButton;
}

/* One rendered line in the bookmark tree. */
struct bw_row {
	short bidx;       /* index into macsurf_bookmarks[] */
	short is_folder;
	short depth;      /* 0 = root/folder, 1 = bookmark inside a folder */
	char  text[160];
};

/* Build the two-level display list: root bookmarks, then each folder and
 * the bookmarks parented to it. `filter` (may be NULL or empty) keeps only
 * labels/URLs matching the case-insensitive substring. Returns row count. */
static int bw_build_rows(struct bw_row *rows, int cap, const char *filter)
{
	int i, j, n = 0;
	int have_filter = (filter != NULL && filter[0] != '\0');
	for (i = 0; i < macsurf_bookmark_count && n < cap; i++) {
		struct macsurf_bookmark *b = &macsurf_bookmarks[i];
		const char *s;
		size_t ln;
		if (b->is_folder || b->parent_id != 0) continue;
		if (have_filter && macsurf_stristr(b->label, filter) == NULL &&
		    macsurf_stristr(b->url, filter) == NULL) continue;
		rows[n].bidx = (short)i; rows[n].is_folder = 0; rows[n].depth = 0;
		s = (b->label[0] != '\0') ? b->label : b->url;
		ln = strlen(s); if (ln > 150) ln = 150;
		memcpy(rows[n].text, s, ln); rows[n].text[ln] = '\0';
		n++;
	}
	for (i = 0; i < macsurf_bookmark_count && n < cap; i++) {
		struct macsurf_bookmark *f = &macsurf_bookmarks[i];
		const char *nm;
		size_t ln;
		if (!f->is_folder) continue;
		if (have_filter && macsurf_stristr(f->label, filter) == NULL)
			continue;
		nm = (f->label[0] != '\0') ? f->label : "(folder)";
		strcpy(rows[n].text, "> ");
		ln = strlen(nm); if (ln > 150) ln = 150;
		memcpy(rows[n].text + 2, nm, ln); rows[n].text[2 + ln] = '\0';
		rows[n].bidx = (short)i; rows[n].is_folder = 1; rows[n].depth = 0;
		n++;
		for (j = 0; j < macsurf_bookmark_count && n < cap; j++) {
			struct macsurf_bookmark *b = &macsurf_bookmarks[j];
			const char *s;
			if (b->is_folder || b->parent_id != f->id) continue;
			if (have_filter && macsurf_stristr(b->label, filter) == NULL &&
			    macsurf_stristr(b->url, filter) == NULL) continue;
			rows[n].bidx = (short)j; rows[n].is_folder = 0; rows[n].depth = 1;
			s = (b->label[0] != '\0') ? b->label : b->url;
			ln = strlen(s); if (ln > 150) ln = 150;
			memcpy(rows[n].text, s, ln); rows[n].text[ln] = '\0';
			n++;
		}
	}
	return n;
}

/* Swap a record with the nearest array slot sharing its (parent, folder)
 * status, in the given direction (+1 = toward the end of the array). The
 * visible order of siblings within a folder/root level therefore follows
 * the buttons. Returns 1 on a swap. */
static int bw_move_sibling(int bidx, int dir)
{
	struct macsurf_bookmark tmp;
	int j;
	if (bidx < 0 || bidx >= macsurf_bookmark_count) return 0;
	for (j = bidx + dir; j >= 0 && j < macsurf_bookmark_count; j += dir) {
		if (macsurf_bookmarks[j].parent_id ==
				macsurf_bookmarks[bidx].parent_id &&
		    macsurf_bookmarks[j].is_folder ==
				macsurf_bookmarks[bidx].is_folder) {
			tmp = macsurf_bookmarks[bidx];
			macsurf_bookmarks[bidx] = macsurf_bookmarks[j];
			macsurf_bookmarks[j] = tmp;
			return 1;
		}
	}
	return 0;
}

/* fixes708 - move a bookmark into a folder via a POPUP picker (replaces the
 * confusing "cycle to next folder" Move). Pops a menu of "Top Level" + every
 * folder at the given global point; the choice becomes the bookmark's parent.
 * No-op for a folder row. */
#define BW_MOVE_POPUP_ID 250
static void bw_move_via_picker(int bidx, short glob_top, short glob_left)
{
	MenuHandle pm;
	int folder_ids[MACSURF_BOOKMARKS_MAX + 1];
	int nf = 0, i, cur_item = 1;
	long chosen;
	if (bidx < 0 || bidx >= macsurf_bookmark_count) return;
	if (macsurf_bookmarks[bidx].is_folder) return;
	pm = NewMenu(BW_MOVE_POPUP_ID, "\pMove");
	if (pm == NULL) return;
	AppendMenu(pm, "\pTop Level");
	folder_ids[nf++] = 0;
	for (i = 0; i < macsurf_bookmark_count && nf <= MACSURF_BOOKMARKS_MAX; i++) {
		Str255 pt;
		const char *nm;
		size_t ln;
		if (!macsurf_bookmarks[i].is_folder) continue;
		nm = (macsurf_bookmarks[i].label[0] != '\0')
			? macsurf_bookmarks[i].label : "(folder)";
		ln = strlen(nm); if (ln > 80) ln = 80;
		pt[0] = (unsigned char)ln;
		memcpy(pt + 1, nm, ln);
		AppendMenu(pm, "\px");
		SetMenuItemText(pm, (short)(nf + 1), pt);
		if (macsurf_bookmarks[i].id == macsurf_bookmarks[bidx].parent_id)
			cur_item = nf + 1;   /* pre-highlight current folder */
		folder_ids[nf++] = macsurf_bookmarks[i].id;
	}
	InsertMenu(pm, hierMenu);
	chosen = PopUpMenuSelect(pm, glob_top, glob_left, (short)cur_item);
	DeleteMenu(BW_MOVE_POPUP_ID);
	DisposeMenu(pm);
	if (chosen != 0) {
		int item = (int)(chosen & 0xFFFF);
		if (item >= 1 && item <= nf)
			macos9_bookmark_set_parent(
				macsurf_bookmarks[bidx].id, folder_ids[item - 1]);
	}
}

/* fixes710 - invert a visible list row (self-reversing drop-target hilite). */
static void bw_invert_row(const Rect *list, int row, int scroll_top, int row_h)
{
	Rect r;
	int vi = row - scroll_top;
	if (vi < 0) return;
	r.left = (short)(list->left + 1);
	r.right = (short)(list->right - 20);
	r.top = (short)(list->top + 2 + vi * row_h);
	r.bottom = (short)(r.top + row_h);
	InvertRect(&r);
}

/* fixes710 - drag-and-drop move. Called on mouseDown on a bookmark row.
 * Tracks the mouse (no Carbon Drag Manager needed): while held, the folder
 * row under the cursor is framed as the drop target; on release over a
 * folder the bookmark is reparented there. Returns 1 if a move happened, so
 * the caller rebuilds. A plain click (no drag onto a folder) returns 0. */
static int bw_try_drag(struct bw_row *rows, int nrows, int src_row,
		const Rect *list, int scroll_top, int row_h)
{
	Point mp;
	int src_bidx = rows[src_row].bidx;
	int target = -1, last_target = -1;
	int did_move = 0;
	if (src_bidx < 0 || src_bidx >= macsurf_bookmark_count) return 0;
	if (macsurf_bookmarks[src_bidx].is_folder) return 0;
	while (StillDown()) {
		int t;
		GetMouse(&mp);   /* local to the manager window's port */
		if (PtInRect(mp, list)) {
			t = scroll_top + (mp.v - (list->top + 2)) / row_h;
			if (t < 0 || t >= nrows || t == src_row || !rows[t].is_folder)
				t = -1;
		} else {
			t = -1;
		}
		if (t != last_target) {
			if (last_target >= 0)
				bw_invert_row(list, last_target, scroll_top, row_h);
			if (t >= 0)
				bw_invert_row(list, t, scroll_top, row_h);
			last_target = t;
		}
		target = t;
	}
	if (last_target >= 0)
		bw_invert_row(list, last_target, scroll_top, row_h);   /* restore */
	if (target >= 0 && target < nrows && rows[target].is_folder) {
		int fbidx = rows[target].bidx;
		if (fbidx >= 0 && fbidx < macsurf_bookmark_count) {
			macos9_bookmark_set_parent(macsurf_bookmarks[src_bidx].id,
				macsurf_bookmarks[fbidx].id);
			did_move = 1;
		}
	}
	return did_move;
}

/* ---- Import / Export file plumbing (FSSpec I/O, mirroring
 * macos9_disk_cache.c; Navigation Services picker mirrors the proven
 * fixes721 file-gadget pattern in window.c) ---- */

extern int macos9_fsspec_to_path(const FSSpec *spec, char *out, long cap);

/* Read an entire file into a heap buffer (caller frees via DisposePtr).
 * Returns the byte count, or -1 on any failure. Capped at 1 MB. */
static long chrome_fsspec_read_all(const FSSpec *spec, char **out)
{
	short ref;
	long len;
	long got;
	char *buf;
	OSErr err;
	*out = NULL;
	if (FSpOpenDF(spec, fsRdPerm, &ref) != noErr) return -1;
	if (GetEOF(ref, &len) != noErr || len <= 0) { FSClose(ref); return -1; }
	if (len > 1024L * 1024L) len = 1024L * 1024L;
	buf = (char *)NewPtr(len);
	if (buf == NULL) { FSClose(ref); return -1; }
	got = len;
	err = FSRead(ref, &got, buf);
	FSClose(ref);
	if (err != noErr) { DisposePtr(buf); return -1; }
	if (got <= 0) { DisposePtr(buf); return -1; }
	*out = buf;
	return got;
}

/* Write bytes to a file (creating it if missing) at vRefNum/dirID/name.
 * Returns 0 on success. */
static OSErr chrome_fsspec_write_all(short vRef, long dirID,
		ConstStr255Param name, const char *data, long len)
{
	FSSpec spec;
	short ref;
	OSErr err;
	err = FSMakeFSSpec(vRef, dirID, name, &spec);
	if (err == fnfErr) {
		err = FSpCreate(&spec, 'MPLS', 'TEXT', smSystemScript);
		if (err == noErr)
			err = FSMakeFSSpec(vRef, dirID, name, &spec);
	}
	if (err != noErr) return err;
	err = FSpOpenDF(&spec, fsRdWrPerm, &ref);
	if (err != noErr) return err;
	err = SetEOF(ref, 0);
	if (err == noErr && len > 0) {
		long wrote = len;
		err = FSWrite(ref, &wrote, data);
	}
	if (err == noErr) err = SetEOF(ref, len);
	FSClose(ref);
	if (err == noErr) FlushVol(NULL, vRef);
	return err;
}

/* Import Bookmarks... - Navigation Services open dialog, then feed the
 * chosen file to the plain-C import parser. Reports the count. */
static void bw_import_bookmarks(void)
{
	NavDialogOptions opts;
	NavReplyRecord reply;
	OSErr err, aeerr;
	FSSpec spec;
	AEKeyword kw;
	DescType dt;
	Size sz;
	char *data;
	long len;
	int added;
	Str255 pmsg;
	char msg[240];

	if (NavGetDefaultDialogOptions(&opts) != noErr) return;
	err = NavGetFile(NULL, &reply, &opts, NULL, NULL, NULL, NULL, NULL);
	if (err != noErr) return;
	if (!reply.validRecord) { NavDisposeReply(&reply); return; }
	/* CarbonLib replies may carry typeFSS or typeFSRef; try both. */
	aeerr = AEGetNthPtr(&reply.selection, 1, typeFSS, &kw, &dt,
			&spec, sizeof spec, &sz);
	if (aeerr != noErr) {
		FSRef ref;
		OSErr e2 = AEGetNthPtr(&reply.selection, 1, typeFSRef, &kw, &dt,
				&ref, sizeof ref, &sz);
		if (e2 == noErr)
			e2 = FSGetCatalogInfo(&ref, kFSCatInfoNone, NULL, NULL,
					&spec, NULL);
		aeerr = e2;
	}
	NavDisposeReply(&reply);
	if (aeerr != noErr) return;
	len = chrome_fsspec_read_all(&spec, &data);
	if (len <= 0) return;
	added = macos9_bookmarks_import_buffer(data, len);
	DisposePtr(data);
	sprintf(msg, "Imported %d bookmark%s.", added,
		(added == 1) ? "" : "s");
	c_to_pstring(msg, pmsg);
	{
		SInt16 item;
		StandardAlert(kAlertNoteAlert, pmsg, "\p", NULL, &item);
	}
}

/* Export Bookmarks... - write the store in the on-disk TAB grammar to
 * MacSurfData/"MacSurf Bookmarks Export.txt" and report the full path
 * (Navigation Services Put is avoided: NavPutFile fails with -5699
 * under CarbonLib - see macos9_download.c). */
static void bw_export_bookmarks(void)
{
	char *data;
	long len;
	short vRef;
	long dirID;
	Str255 name;
	Str255 pmsg;
	char msg[240];
	SInt16 item;
	OSErr err;

	data = macsurf_bookmarks_serialize(&len);
	if (data == NULL) {
		c_to_pstring("No bookmarks to export.", pmsg);
		StandardAlert(kAlertNoteAlert, pmsg, "\p", NULL, &item);
		return;
	}
	if (macos9_data_dir_get(NULL, &vRef, &dirID) != noErr) {
		free(data);
		return;
	}
	c_to_pstring("MacSurf Bookmarks Export.txt", name);
	{
		long nrec = 0;
		long k;
		for (k = 0; k < len; k++)
			if (data[k] == '\n') nrec++;
		err = chrome_fsspec_write_all(vRef, dirID, name, data, len);
		free(data);
		if (err != noErr) {
			c_to_pstring("Could not write the export file.", pmsg);
			StandardAlert(kAlertNoteAlert, pmsg, "\p", NULL, &item);
			return;
		}
		{
			FSSpec spec;
			char path[1024];
			size_t pl;
			if (FSMakeFSSpec(vRef, dirID, name, &spec) != noErr ||
			    macos9_fsspec_to_path(&spec, path,
				(long)sizeof path) != 0)
				strcpy(path, "MacSurfData");
			/* msg is 240 bytes - never let the path blow it */
			pl = strlen(path);
			if (pl > 180) {
				strcpy(path + 177, "...");
				pl = 180;
			}
			sprintf(msg, "Exported %ld bookmark%s to:\n%s", nrec,
				(nrec == 1) ? "" : "s", path);
			c_to_pstring(msg, pmsg);
			StandardAlert(kAlertNoteAlert, pmsg, "\p", NULL, &item);
		}
	}
}

void macos9_bookmark_window_show(struct gui_window *g)
{
	WindowRef win;
	Rect wb, list, up, dn, nf, rn, del, mv, upbtn, dnbtn;
	Rect imp, exp, go, done, search_rect;
	GrafPtr saved_port;
	EventRecord ev;
	struct bw_row *rows;
	int cap, nrows;
	int scroll_top = 0, sel = -1;
	int row_h = 20, vis;              /* fixes742 - taller rows, more padding */
	int done_flag = 0, dirty = 1;
	char go_url[MACSURF_BMK_URL_MAX];
	int moved_id = 0;
	TEHandle te_search = NULL;
	int search_focus = 0;
	char filter[128];

	if (g == NULL) return;
	go_url[0] = '\0';
	filter[0] = '\0';

	cap = macsurf_bookmark_count * 2 + 4;
	rows = (struct bw_row *)malloc((size_t)cap * sizeof(struct bw_row));
	if (rows == NULL) return;

	SetRect(&wb, 110, 90, 750, 490);
	if (CreateNewWindow(kDocumentWindowClass, kWindowCloseBoxAttribute,
			&wb, &win) != noErr || win == NULL) {
		free(rows);
		return;
	}
	{ Str255 t; c_to_pstring("Bookmarks", t); SetWTitle(win, t); }

	GetPort(&saved_port);
	SetPortWindowPort(win);
	TextFont(1);
	TextSize(12);  /* fixes742 - larger row text */

	/* content is 640 x 400 local. Search field + tree list below the 34px
	 * title banner; the filter narrows rows to label/URL matches. */
	SetRect(&search_rect, 46, 40, 552, 60);
	SetRect(&list, 8, 64, 552, 344);
	SetRect(&up,   534, 64,  552, 86);
	SetRect(&dn,   534, 322, 552, 344);
	SetRect(&nf,   8,   352, 88,  376);
	SetRect(&rn,   92,  352, 148, 376);
	SetRect(&del,  152, 352, 208, 376);
	SetRect(&mv,   212, 352, 260, 376);
	SetRect(&upbtn, 264, 352, 330, 376);
	SetRect(&dnbtn, 334, 352, 414, 376);
	SetRect(&imp,  418, 352, 474, 376);
	SetRect(&exp,  478, 352, 534, 376);
	SetRect(&go,   538, 352, 580, 376);
	SetRect(&done, 584, 352, 632, 376);
	vis = (list.bottom - list.top - 4) / row_h;

	te_search = TENew(&search_rect, &search_rect);
	if (te_search == NULL) {
		SetPort(saved_port);
		DisposeWindow(win);
		free(rows);
		return;
	}

	nrows = bw_build_rows(rows, cap, filter);
	sel = (nrows > 0) ? 0 : -1;

	ShowWindow(win);
	SelectWindow(win);

	while (!done_flag) {
		int rebuilt = 0;
		WaitNextEvent(everyEvent, &ev, 30, NULL);
		switch (ev.what) {
		case updateEvt:
			if ((WindowRef)ev.message == win) {
				BeginUpdate(win);
				EndUpdate(win);
				dirty = 1;
			} else {
				/* fixes709 - repaint a background browser window we
				 * uncovered while being dragged, so it doesn't stay
				 * white. Restore our port afterwards. */
				extern void macos9_handle_update(const EventRecord *event);
				macos9_handle_update(&ev);
				SetPortWindowPort(win);
			}
			break;
		case mouseDown: {
			WindowRef which;
			short part = FindWindow(ev.where, &which);
			Point lp;
			if (which != win) break;
			if (part == inDrag) {
				Rect db; BitMap sb;
				GetQDGlobalsScreenBits(&sb);
				db = sb.bounds;
				DragWindow(win, ev.where, &db);
				break;
			}
			if (part == inGoAway) {
				if (TrackGoAway(win, ev.where)) done_flag = 1;
				break;
			}
			if (part != inContent) break;
			lp = ev.where;
			GlobalToLocal(&lp);
			if (PtInRect(lp, &done)) { done_flag = 1; break; }
			if (PtInRect(lp, &up)) {
				scroll_top -= 3; if (scroll_top < 0) scroll_top = 0; break;
			}
			if (PtInRect(lp, &dn)) {
				int maxtop = nrows - vis; if (maxtop < 0) maxtop = 0;
				scroll_top += 3; if (scroll_top > maxtop) scroll_top = maxtop;
				break;
			}
			if (PtInRect(lp, &nf)) {
				char name[MACSURF_BMK_LBL_MAX];
				if (chrome_prompt_text("New Folder", "", name, sizeof name))
					macos9_bookmark_new_folder(name, 0);
				rebuilt = 1; break;
			}
			if (PtInRect(lp, &rn)) {
				if (sel >= 0 && sel < nrows) {
					int bi = rows[sel].bidx;
					if (bi >= 0 && bi < macsurf_bookmark_count) {
						if (macsurf_bookmarks[bi].is_folder) {
							char name[MACSURF_BMK_LBL_MAX];
							if (chrome_prompt_text("Rename Folder",
								macsurf_bookmarks[bi].label,
								name, sizeof name))
								macos9_bookmark_rename(
									macsurf_bookmarks[bi].id,
									name);
						} else {
							/* Edit Bookmark: label AND url in one
							 * two-field dialog. */
							char name[MACSURF_BMK_LBL_MAX];
							char url[MACSURF_BMK_URL_MAX];
							if (chrome_prompt_text2("Edit Bookmark",
								macsurf_bookmarks[bi].label,
								macsurf_bookmarks[bi].url,
								name, (int)sizeof name,
								url, (int)sizeof url)) {
								macos9_bookmark_rename(
									macsurf_bookmarks[bi].id,
									name);
								macos9_bookmark_set_url(
									macsurf_bookmarks[bi].id,
									url);
							}
						}
					}
				}
				rebuilt = 1; break;
			}
			if (PtInRect(lp, &del)) {
				if (sel >= 0 && sel < nrows) {
					int bi = rows[sel].bidx;
					if (bi >= 0 && bi < macsurf_bookmark_count &&
					    chrome_confirm_delete(
						macsurf_bookmarks[bi].is_folder ?
						"Delete this folder? Its bookmarks move to the top level."
						: "Delete this bookmark?"))
						macos9_bookmark_delete(macsurf_bookmarks[bi].id);
				}
				rebuilt = 1; break;
			}
			if (PtInRect(lp, &mv)) {
				Point gp;
				if (sel >= 0 && sel < nrows && !rows[sel].is_folder) {
					gp.h = mv.left; gp.v = mv.top;
					LocalToGlobal(&gp);
					bw_move_via_picker(rows[sel].bidx, gp.v, gp.h);
				}
				rebuilt = 1; break;
			}
			if (PtInRect(lp, &upbtn)) {
				if (sel >= 0 && sel < nrows) {
					int bi = rows[sel].bidx;
					if (bi >= 0 && bi < macsurf_bookmark_count) {
						int mid = macsurf_bookmarks[bi].id;
						if (bw_move_sibling(bi, -1)) moved_id = mid;
					}
				}
				rebuilt = 1; break;
			}
			if (PtInRect(lp, &dnbtn)) {
				if (sel >= 0 && sel < nrows) {
					int bi = rows[sel].bidx;
					if (bi >= 0 && bi < macsurf_bookmark_count) {
						int mid = macsurf_bookmarks[bi].id;
						if (bw_move_sibling(bi, 1)) moved_id = mid;
					}
				}
				rebuilt = 1; break;
			}
			if (PtInRect(lp, &imp)) {
				bw_import_bookmarks();
				rebuilt = 1; break;
			}
			if (PtInRect(lp, &exp)) {
				bw_export_bookmarks();
				rebuilt = 1; break;
			}
			if (PtInRect(lp, &go)) {
				if (sel >= 0 && sel < nrows && !rows[sel].is_folder) {
					int bi = rows[sel].bidx;
					if (bi >= 0 && bi < macsurf_bookmark_count) {
						strcpy(go_url, macsurf_bookmarks[bi].url);
						done_flag = 1;
					}
				}
				break;
			}
			if (PtInRect(lp, &search_rect)) {
				if (!search_focus) {
					search_focus = 1;
					TEActivate(te_search);
				}
				TEClick(lp, false, te_search);
				break;
			}
			if (PtInRect(lp, &list)) {
				int idx = scroll_top + (lp.v - (list.top + 2)) / row_h;
				if (search_focus) {
					search_focus = 0;
					TEDeactivate(te_search);
				}
				if (idx >= 0 && idx < nrows) {
					sel = idx;
					/* fixes710 - drag a bookmark onto a folder to
					 * move it (folders/headers aren't draggable). */
					if (!rows[idx].is_folder &&
					    bw_try_drag(rows, nrows, idx, &list,
							scroll_top, row_h))
						rebuilt = 1;
				}
			}
			break;
		}
		case keyDown:
		case autoKey: {
			char ch = (char)(ev.message & charCodeMask);
			if ((ev.modifiers & cmdKey) &&
			    (ch == '.' || ch == 'w' || ch == 'W')) {
				done_flag = 1;
			} else if (search_focus && ch == 0x09) {
				/* Tab leaves the search field */
				search_focus = 0;
				TEDeactivate(te_search);
			} else if (search_focus && ch == 0x1B) {
				/* Esc clears the filter first, then closes */
				if (filter[0] != '\0') {
					filter[0] = '\0';
					TESetSelect(0, 32767, te_search);
					TESetText("", 0, te_search);
					nrows = bw_build_rows(rows, cap, filter);
					scroll_top = 0;
					sel = (nrows > 0) ? 0 : -1;
				} else {
					done_flag = 1;
				}
			} else if (search_focus && (ch == 0x0D || ch == 0x03)) {
				/* Return in the field = Go */
				if (sel >= 0 && sel < nrows && !rows[sel].is_folder) {
					int bi = rows[sel].bidx;
					if (bi >= 0 && bi < macsurf_bookmark_count) {
						strcpy(go_url, macsurf_bookmarks[bi].url);
						done_flag = 1;
					}
				}
			} else if (search_focus) {
				int was_empty = (filter[0] == '\0');
				if (ch == 0x1F || ch == 0x1E || ch == 0x0C ||
				    ch == 0x0B || ch == 0x01 || ch == 0x04) {
					/* list navigation still works while typing */
					if (ch == 0x1F) { if (sel < nrows - 1) sel++; }
					else if (ch == 0x1E) { if (sel > 0) sel--; }
					else if (ch == 0x0C) { scroll_top += vis - 1; }
					else if (ch == 0x0B) { scroll_top -= vis - 1; }
					else if (ch == 0x01) {
						scroll_top = 0; sel = (nrows > 0) ? 0 : -1;
					} else {
						sel = nrows - 1; scroll_top = nrows - vis;
					}
				} else if ((ch >= 0x20 && ch < 0x7F) || ch == 0x08) {
					/* printable / backspace - edit, re-filter */
					TEKey(ch, te_search);
					chrome_te_get_text(te_search, filter,
						(int)sizeof filter);
					nrows = bw_build_rows(rows, cap, filter);
					if (was_empty || nrows == 0) scroll_top = 0;
					sel = (nrows > 0) ? 0 : -1;
				} else {
					TEKey(ch, te_search);
				}
			} else if (ch == 0x09) {
				/* Tab enters the search field */
				search_focus = 1;
				TEActivate(te_search);
			} else if (ch == 0x1B) {
				done_flag = 1;
			} else if (ch == 0x0D || ch == 0x03) {
				if (sel >= 0 && sel < nrows && !rows[sel].is_folder) {
					int bi = rows[sel].bidx;
					if (bi >= 0 && bi < macsurf_bookmark_count) {
						strcpy(go_url, macsurf_bookmarks[bi].url);
						done_flag = 1;
					}
				}
			} else if (ch == 0x1F) {
				if (sel < nrows - 1) sel++;
			} else if (ch == 0x1E) {
				if (sel > 0) sel--;
			} else if (ch == 0x0C) {
				scroll_top += vis - 1;
			} else if (ch == 0x0B) {
				scroll_top -= vis - 1;
			} else if (ch == 0x01) {
				scroll_top = 0; sel = (nrows > 0) ? 0 : -1;
			} else if (ch == 0x04) {
				sel = nrows - 1; scroll_top = nrows - vis;
			}
			if (scroll_top < 0) scroll_top = 0;
			{
				int maxtop = nrows - vis; if (maxtop < 0) maxtop = 0;
				if (scroll_top > maxtop) scroll_top = maxtop;
			}
			if (sel >= 0) {
				if (sel < scroll_top) scroll_top = sel;
				else if (sel >= scroll_top + vis)
					scroll_top = sel - vis + 1;
			}
			break;
		}
		default:
			break;
		}

		if (rebuilt) {
			int r;
			nrows = bw_build_rows(rows, cap, filter);
			if (moved_id != 0) {
				/* Move Up/Down swapped structs - keep the moved
				 * bookmark selected by its stable id. */
				sel = -1;
				for (r = 0; r < nrows; r++) {
					int bi = rows[r].bidx;
					if (bi >= 0 && bi < macsurf_bookmark_count &&
					    macsurf_bookmarks[bi].id == moved_id) {
						sel = r;
						break;
					}
				}
				moved_id = 0;
			}
			if (sel >= nrows) sel = nrows - 1;
			if (nrows == 0) sel = -1;
			if (scroll_top > nrows - vis) {
				scroll_top = nrows - vis;
				if (scroll_top < 0) scroll_top = 0;
			}
		}
		if (ev.what == mouseDown || ev.what == keyDown ||
		    ev.what == autoKey)
			dirty = 1;

		if (!done_flag && dirty) {
			RgnHandle saveclip = NewRgn();
			Rect tr;
			int r, y;
			GetClip(saveclip);
			{ Rect content; SetRect(&content, 0, 0, 640, 400);
			  chrome_mgr_header(&content, "Bookmarks", 2); }
			chrome_draw_search_field(&search_rect, 6);
			TEUpdate(&search_rect, te_search);
			EraseRect(&list);
			FrameRect(&list);
			ClipRect(&list);
			y = list.top + 2;
			for (r = scroll_top;
			     r < nrows && (r - scroll_top) < vis; r++) {
				short len = (short)strlen(rows[r].text);
				short x = (short)(list.left + 6 + rows[r].depth * 18);
				RGBColor blk, wht;
				blk.red = blk.green = blk.blue = 0;
				wht.red = wht.green = wht.blue = 0xFFFF;
				tr.left = (short)(list.left + 1);
				tr.right = (short)(list.right - 20);
				tr.top = (short)y;
				tr.bottom = (short)(y + row_h);
				if (r == sel) {
					RGBColor selc;
					selc.red = 0xE8E8; selc.green = 0x9E9E; selc.blue = 0x3838;
					RGBForeColor(&selc); PaintRect(&tr);
				} else if (((r - scroll_top) & 1) != 0) {
					RGBColor st;
					st.red = 0xFDFD; st.green = 0xF8F8; st.blue = 0xEFEF;
					RGBForeColor(&st); PaintRect(&tr);
				}
				if (rows[r].is_folder) TextFace(bold);
				if (r == sel) {
					RGBForeColor(&wht);
				} else if (rows[r].is_folder) {
					RGBColor fc;
					fc.red = 0x7A7A; fc.green = 0x4E4E; fc.blue = 0x1414;
					RGBForeColor(&fc);
				} else {
					RGBForeColor(&blk);
				}
				MoveTo(x, (short)(y + 14));
				DrawText(rows[r].text, 0, len);
				if (rows[r].is_folder) TextFace(normal);
				RGBForeColor(&blk);
				y += row_h;
			}
			SetClip(saveclip);
			DisposeRgn(saveclip);
			EraseRect(&up); FrameRect(&up);
			MoveTo(up.left + 6, up.top + 15); DrawString("\p^");
			EraseRect(&dn); FrameRect(&dn);
			MoveTo(dn.left + 6, dn.top + 15); DrawString("\pv");
			chrome_draw_button(&nf, "\pNew Folder");
			chrome_draw_button(&rn, "\pRename");
			chrome_draw_button(&del, "\pDelete");
			chrome_draw_button(&mv, "\pMove");
			chrome_draw_button(&upbtn, "\pMove Up");
			chrome_draw_button(&dnbtn, "\pMove Down");
			chrome_draw_button(&imp, "\pImport...");
			chrome_draw_button(&exp, "\pExport...");
			chrome_draw_button(&go, "\pGo");
			chrome_draw_button(&done, "\pDone");
			if (nrows == 0) {
				MoveTo(list.left + 12, list.top + 24);
				if (filter[0] != '\0')
					DrawString("\p(No matches)");
				else
					DrawString("\p(No bookmarks yet)");
			}
			dirty = 0;
		}
	}

	TEDispose(te_search);
	SetPort(saved_port);
	DisposeWindow(win);
	free(rows);

	if (go_url[0] != '\0')
		macos9_window_navigate(g, go_url);
}

#else  /* !__MACOS9__ */
void macos9_bookmark_window_show(struct gui_window *g) { (void)g; }
#endif

/* Legacy "Show Bookmarks" alert - retained for ABI but no longer menu-
 * wired (the live menu supersedes it). */
void macos9_bookmark_list_show(struct gui_window *g)
{
	(void)g;
}

/* ====================================================================
 * About box - a small animated "space glass" credits window.
 *
 * Shown from the Apple menu's "About MacSurf..." item. Double-buffered
 * through a 32-bit offscreen GWorld (flicker-free) and animated off the
 * event loop's null-event frame tick: a vertical deep-blue gradient, a
 * drifting starfield, a metallic shine that sweeps the title, and a
 * scrolling credits roll of the supporter list. Everything fades up
 * from black on open (all draw colours are scaled by a brightness ramp,
 * since classic GWorlds have no alpha).
 *
 * The supporter roll is the single source of truth for the in-app
 * credits - add a new supporter to about_roll[] below (and to
 * README.md + .private/supporters.md). Because the list scrolls, it has
 * no length ceiling (unlike the old Str255 StandardAlert body, which is
 * still built from the same names as a resource-free fallback).
 * ==================================================================== */
#ifdef __MACOS9__

#define ABOUT_W      380
#define ABOUT_H      300
#define ABOUT_STARS  48

/* kind: 0 = supporter name, 1 = section header, 2 = blank spacer. */
struct about_line { const char *text; short kind; };
static const struct about_line about_roll[] = {
	{ "Patreon supporters", 1 },
	{ "Mack Chamberlain",   0 },
	{ "Shlooom",            0 },
	{ "Kestral",            0 },
	{ "Mothra",             0 },
	{ "",                   2 },
	{ "Ko-Fi supporters",   1 },
	{ "kilgeist",           0 },
	{ "Turuun",             0 },
	{ "Rogue",              0 },
	{ "",                   2 },
	{ "Thank you for keeping",  0 },
	{ "vintage Macs on the web.",0 }
};
#define ABOUT_ROLL_N ((short)(sizeof(about_roll) / sizeof(about_roll[0])))

static short         about_star_x[ABOUT_STARS];
static short         about_star_y0[ABOUT_STARS];
static short         about_star_v[ABOUT_STARS];   /* 1/16 px per tick */
static short         about_star_b[ABOUT_STARS];   /* 0..255 base brightness */
static int           about_stars_ready = 0;
static unsigned long about_rng = 0;

static unsigned long about_rand(void)
{
	about_rng = about_rng * 1103515245UL + 12345UL;
	return (about_rng >> 16) & 0x7fffUL;
}

static void about_stars_init(void)
{
	short i;
	about_rng = (unsigned long)TickCount() ^ 0x9E3779B9UL;
	for (i = 0; i < ABOUT_STARS; i++) {
		about_star_x[i]  = (short)(about_rand() % ABOUT_W);
		about_star_y0[i] = (short)(about_rand() % ABOUT_H);
		about_star_v[i]  = (short)(4 + (about_rand() % 20));
		about_star_b[i]  = (short)(90 + (about_rand() % 165));
	}
	about_stars_ready = 1;
}

/* Set the QuickDraw foreground to (r,g,b) [0..255] scaled by brightness
 * bri [0..255] - the whole frame fades up from black by ramping bri. */
static void about_fore(short r, short g, short b, short bri)
{
	RGBColor c;
	if (bri < 0) bri = 0;
	if (bri > 255) bri = 255;
	c.red   = (unsigned short)(((long)r * bri / 255) * 257);
	c.green = (unsigned short)(((long)g * bri / 255) * 257);
	c.blue  = (unsigned short)(((long)b * bri / 255) * 257);
	RGBForeColor(&c);
}

static void about_center(const char *s, short cx, short y)
{
	short n = (short)strlen(s), w;
	if (n <= 0) return;
	w = TextWidth(s, 0, n);
	MoveTo(cx - w / 2, y);
	DrawText(s, 0, n);
}

/* fixes726 - crisp About-box puffin. The old PlotIconID upscaled the 32x32
 * icon-family member to 52x52 (visibly blocky). We decode a 64x64 PNG once
 * into a 32-bit colour GWorld plus a hand-built 1-bit mask (from alpha) and
 * CopyMask it over the animated gradient, so it composites cleanly (stars show
 * through the transparent margins) and downscales crisply. */
static GWorldPtr s_about_logo_gw   = NULL;
static BitMap    s_about_logo_mask;   /* real 1-bit BitMap (rowBytes<0x2000) */
static Rect      s_about_logo_src;
static int       s_about_logo_tried  = 0;
static int       s_about_logo_ok     = 0;

static void about_logo_ensure(void)
{
	extern unsigned lodepng_decode32(unsigned char **out, unsigned *w,
			unsigned *h, const unsigned char *in,
			unsigned long insize);
	unsigned char *rgba = NULL;
	unsigned w = 0, h = 0, err;
	OSErr oerr;
	GWorldPtr saved_port;
	GDHandle saved_gdh;
	PixMapHandle pm;
	long dst_rb, mask_rb, row, col;
	unsigned char *src_row, *dst_row, *mrow;

	if (s_about_logo_tried) return;
	s_about_logo_tried = 1;

	err = lodepng_decode32(&rgba, &w, &h, macos9_about_logo_png,
		macos9_about_logo_png_len);
	if (err != 0 || rgba == NULL || w == 0 || h == 0) {
		if (rgba != NULL) free(rgba);
		return;
	}
	SetRect(&s_about_logo_src, 0, 0, (short)w, (short)h);

	GetGWorld(&saved_port, &saved_gdh);
	oerr = NewGWorld(&s_about_logo_gw, 32, &s_about_logo_src, NULL, NULL, 0);
	if (oerr != noErr || s_about_logo_gw == NULL) {
		free(rgba); SetGWorld(saved_port, saved_gdh); return;
	}
	pm = GetGWorldPixMap(s_about_logo_gw);
	if (pm == NULL || !LockPixels(pm)) {
		DisposeGWorld(s_about_logo_gw); s_about_logo_gw = NULL;
		free(rgba); SetGWorld(saved_port, saved_gdh); return;
	}
	/* colour plane: straight ARGB copy (mask handles transparency) */
	dst_rb = (long)((*pm)->rowBytes & 0x3FFF);
	for (row = 0; row < (long)h; row++) {
		src_row = rgba + row * (long)w * 4L;
		dst_row = (unsigned char *)GetPixBaseAddr(pm) + row * dst_rb;
		for (col = 0; col < (long)w; col++) {
			dst_row[col*4+0] = 0xFF;
			dst_row[col*4+1] = src_row[col*4+0];
			dst_row[col*4+2] = src_row[col*4+1];
			dst_row[col*4+3] = src_row[col*4+2];
		}
	}
	UnlockPixels(pm);
	SetGWorld(saved_port, saved_gdh);

	/* 1-bit mask BitMap: bit set (black) where the pixel is opaque, so
	 * CopyMask copies the source there and leaves the gradient elsewhere. */
	mask_rb = (((long)w + 15) / 16) * 2;   /* even, well under 0x2000 */
	s_about_logo_mask.baseAddr = NewPtrClear(mask_rb * (long)h);
	if (s_about_logo_mask.baseAddr == NULL) {
		DisposeGWorld(s_about_logo_gw); s_about_logo_gw = NULL;
		free(rgba); return;
	}
	s_about_logo_mask.rowBytes = (short)mask_rb;
	s_about_logo_mask.bounds   = s_about_logo_src;
	for (row = 0; row < (long)h; row++) {
		src_row = rgba + row * (long)w * 4L;
		mrow = (unsigned char *)s_about_logo_mask.baseAddr + row * mask_rb;
		for (col = 0; col < (long)w; col++) {
			if (src_row[col*4+3] >= 128)
				mrow[col >> 3] |= (unsigned char)(0x80 >> (col & 7));
		}
	}
	free(rgba);
	s_about_logo_ok = 1;
}

/* Render one frame into the offscreen GWorld, then blit it to the
 * window. elapsed is ticks since the window opened. */
static void about_draw(GWorldPtr off, WindowRef win, const Rect *content,
	const Rect *okr, long elapsed, short titleFnum, short bodyFnum,
	const RGBColor *black, const RGBColor *white)
{
	CGrafPtr     saveGW;
	GDHandle     saveGD;
	PixMapHandle offpm = GetGWorldPixMap(off);
	short        bri = (short)(elapsed * 255 / 30);
	Rect         full;

	if (bri > 255) bri = 255;
	if (bri < 0)   bri = 0;
	SetRect(&full, 0, 0, ABOUT_W, ABOUT_H);

	GetGWorld(&saveGW, &saveGD);
	SetGWorld(off, NULL);
	ClipRect(&full);

	/* 1. vertical gradient backdrop (deep blue -> near black) */
	{
		short yy;
		for (yy = 0; yy < ABOUT_H; yy += 2) {
			Rect ln;
			short rr = (short)(20 + (2  - 20) * yy / ABOUT_H);
			short gg = (short)(28 + (4  - 28) * yy / ABOUT_H);
			short bb = (short)(70 + (16 - 70) * yy / ABOUT_H);
			about_fore(rr, gg, bb, bri);
			SetRect(&ln, 0, yy, ABOUT_W, yy + 2);
			PaintRect(&ln);
		}
	}

	/* 2. drifting starfield */
	{
		short i;
		for (i = 0; i < ABOUT_STARS; i++) {
			short yy = (short)((about_star_y0[i] +
				((elapsed * about_star_v[i]) >> 4)) % ABOUT_H);
			short sb = about_star_b[i];
			Rect pr;
			about_fore(sb, sb, (short)(sb > 235 ? 255 : sb + 20), bri);
			if (sb > 200)
				SetRect(&pr, about_star_x[i], yy,
					about_star_x[i] + 2, yy + 2);
			else
				SetRect(&pr, about_star_x[i], yy,
					about_star_x[i] + 1, yy + 1);
			PaintRect(&pr);
		}
	}

	/* 3. app icon (puffin) - crisp 64x64 CopyMask blit (fixes726), with a
	 * PlotIconID fallback if the PNG decode failed. */
	{
		Rect ir;
		SetRect(&ir, 164, 14, 216, 66);
		about_logo_ensure();
		if (s_about_logo_ok && s_about_logo_gw != NULL) {
			PixMapHandle spm = GetGWorldPixMap(s_about_logo_gw);
			RGBColor sfg, sbg;
			GetForeColor(&sfg); GetBackColor(&sbg);
			RGBForeColor(black); RGBBackColor(white);
			if (spm != NULL && LockPixels(spm)) {
				CopyMask((BitMap *)*spm, &s_about_logo_mask,
					(BitMap *)*offpm,
					&s_about_logo_src, &s_about_logo_src, &ir);
				UnlockPixels(spm);
			}
			RGBForeColor(&sfg); RGBBackColor(&sbg);
		} else {
			PlotIconID(&ir, kAlignNone, kTransformNone, 128);
		}
	}

	/* 4. title with a sweeping metallic shine */
	{
		const char *tt = "MacSurf 2.0.5";
		short n = (short)strlen(tt), tw, tx, ty = 100;
		TextFont(titleFnum);
		TextFace(bold);
		TextSize(20);
		tw = TextWidth(tt, 0, n);
		tx = (ABOUT_W - tw) / 2;
		about_fore(198, 216, 244, bri);
		MoveTo(tx, ty);
		DrawText(tt, 0, n);
		{
			long period = 150;
			long phase  = elapsed % period;
			long span   = tw + 80;
			short bc = (short)(tx - 40 + phase * span / period);
			Rect band;
			SetRect(&band, (short)(bc - 10), ty - 20,
				(short)(bc + 10), ty + 6);
			ClipRect(&band);
			about_fore(255, 255, 255, bri);
			MoveTo(tx, ty);
			DrawText(tt, 0, n);
			ClipRect(&full);
		}
		TextFace(normal);
	}

	/* 5. accent underline + subtitle + author */
	{
		Rect a;
		about_fore(70, 140, 255, bri);
		SetRect(&a, 96, 110, 284, 112);
		PaintRect(&a);
		TextFont(bodyFnum);
		TextSize(9);
		TextFace(normal);
		about_fore(120, 145, 190, bri);
		about_center("Native TLS 1.3   -   by mplsllc", 190, 128);
		about_fore(150, 165, 200, bri);
		about_center("For Gary & Kaija", 190, 143);
	}

	/* 6. scrolling supporter credits roll (clipped to a viewport) */
	{
		Rect  vp;
		short lineH = 16, i, cx = 190;
		long  vph, cycle, offs;
		SetRect(&vp, 24, 152, ABOUT_W - 24, 250);
		vph   = vp.bottom - vp.top;
		cycle = (long)ABOUT_ROLL_N * lineH + vph;
		offs  = (elapsed / 3) % cycle;
		ClipRect(&vp);
		for (i = 0; i < ABOUT_ROLL_N; i++) {
			short y = (short)(vp.bottom - offs + i * lineH);
			if (about_roll[i].kind == 2) continue;
			if (y < vp.top - lineH || y > vp.bottom + lineH) continue;
			TextFont(bodyFnum);
			TextSize(10);
			if (about_roll[i].kind == 1) {
				TextFace(bold);
				about_fore(120, 185, 255, bri);
			} else {
				TextFace(normal);
				about_fore(232, 240, 252, bri);
			}
			about_center(about_roll[i].text, cx, y);
		}
		TextFace(normal);
		ClipRect(&full);
	}

	/* 7. beveled OK button */
	{
		about_fore(46, 68, 120, bri);
		PaintRoundRect(okr, 12, 12);
		PenSize(1, 1);
		about_fore(120, 160, 230, bri);
		FrameRoundRect(okr, 12, 12);
		TextFont(bodyFnum);
		TextFace(bold);
		TextSize(10);
		about_fore(240, 246, 255, bri);
		about_center("OK", (short)((okr->left + okr->right) / 2),
			(short)(okr->top + 16));
		TextFace(normal);
	}

	/* blit offscreen -> window. Reset fg/bg first: classic QuickDraw
	 * colorizes CopyBits toward the port foreground (fixes301j). */
	SetGWorld(saveGW, saveGD);
	RGBForeColor(black);
	RGBBackColor(white);
	CopyBits((BitMap *)*offpm,
		GetPortBitMapForCopyBits(GetWindowPort(win)),
		content, content, srcCopy, NULL);
}

/* The About sounds stay inside the application.  QuickTime imports the MP3
 * from this handle; the caller keeps it alive until the movie is disposed. */
static Movie about_audio_from_memory(const unsigned char *bytes, long len,
		Handle *data_out)
{
	Handle data;
	Movie movie;
	MovieImportComponent importer;
	Track used_track;
	TimeValue duration;
	long import_flags;
	ComponentResult err;

	*data_out = NULL;
	if (bytes == NULL || len <= 0) return NULL;
	data = NewHandle(len);
	if (data == NULL || MemError() != noErr) {
		MS_LOG("LIFE about-audio FAIL alloc");
		return NULL;
	}
	BlockMoveData(bytes, *data, len);

	movie = NewMovie(0);
	if (movie == NULL) {
		DisposeHandle(data);
		MS_LOG("LIFE about-audio FAIL NewMovie");
		return NULL;
	}
	importer = OpenDefaultComponent(MovieImportType, 'MPG3');
	if (importer == NULL) {
		DisposeMovie(movie);
		DisposeHandle(data);
		MS_LOG("LIFE about-audio FAIL no-mp3-importer");
		return NULL;
	}
	used_track = NULL;
	duration = 0;
	import_flags = 0;
	err = MovieImportHandle(importer, data, movie, NULL, &used_track, 0,
			&duration, movieImportCreateTrack, &import_flags);
	CloseComponent(importer);
	if (err != noErr || used_track == NULL) {
		DisposeMovie(movie);
		DisposeHandle(data);
		macsurf_debug_log_writef("LIFE about-audio FAIL import=%d", (int)err);
		return NULL;
	}
	SetMovieActive(movie, true);
	GoToBeginningOfMovie(movie);
	StartMovie(movie);
	*data_out = data;
	macsurf_debug_log_writef("LIFE about-audio START bytes=%ld duration=%ld",
		len, (long)duration);
	return movie;
}
#endif /* __MACOS9__ */

void macos9_about_show(void)
{
#ifdef __MACOS9__
	WindowRef    win = NULL;
	Rect         wb, content, okr;
	GWorldPtr    off = NULL;
	PixMapHandle offpm = NULL;
	GrafPtr      savedPort;
	EventRecord  ev;
	long         startTick;
	short        titleFnum = 0, bodyFnum = 0;
	int          done = 0;
	RGBColor     black, white;
	Movie        audio_movie = NULL;
	Handle       audio_data = NULL;

	{
		BitMap sb;
		short  sw, sh, l, t;
		GetQDGlobalsScreenBits(&sb);
		sw = sb.bounds.right - sb.bounds.left;
		sh = sb.bounds.bottom - sb.bounds.top;
		l = (short)((sw - ABOUT_W) / 2);
		t = (short)((sh - ABOUT_H) / 3);
		if (l < 4) l = 4;
		if (t < 40) t = 40;
		SetRect(&wb, l, t, (short)(l + ABOUT_W), (short)(t + ABOUT_H));
	}

	if (CreateNewWindow(kDocumentWindowClass, kWindowCloseBoxAttribute,
			&wb, &win) != noErr || win == NULL)
		goto fallback;

	SetWTitle(win, "\pAbout MacSurf");
	GetPort(&savedPort);
	SetPortWindowPort(win);

	SetRect(&content, 0, 0, ABOUT_W, ABOUT_H);
	NewGWorld(&off, 32, &content, NULL, NULL, 0);
	if (off == NULL) {
		SetPort(savedPort);
		DisposeWindow(win);
		goto fallback;
	}
	offpm = GetGWorldPixMap(off);
	LockPixels(offpm);

	GetFNum("\pGeneva", &titleFnum);
	bodyFnum = titleFnum;
	if (!about_stars_ready) about_stars_init();

	SetRect(&okr, 155, 262, 225, 286);
	black.red = black.green = black.blue = 0;
	white.red = white.green = white.blue = 0xFFFF;

	ShowWindow(win);
	SelectWindow(win);
	startTick = (long)TickCount();

	while (!done) {
		if (audio_movie != NULL) {
			MoviesTask(audio_movie, 0);
			if (IsMovieDone(audio_movie)) {
				DisposeMovie(audio_movie);
				DisposeHandle(audio_data);
				audio_movie = NULL;
				audio_data = NULL;
				MS_LOG("LIFE about-audio DONE");
			}
		}
		WaitNextEvent(everyEvent, &ev, 2, NULL);
		switch (ev.what) {
		case mouseDown: {
			WindowRef which;
			short part = FindWindow(ev.where, &which);
			if (which != win) break;
			if (part == inDrag) {
				Rect db; BitMap sb2;
				GetQDGlobalsScreenBits(&sb2);
				db = sb2.bounds;
				DragWindow(win, ev.where, &db);
			} else if (part == inGoAway) {
				if (TrackGoAway(win, ev.where)) done = 1;
			} else if (part == inContent) {
				Point lp = ev.where;
				Rect gary_r, kaija_r;
				GlobalToLocal(&lp);
				if (PtInRect(lp, &okr)) done = 1;
				SetRect(&gary_r, 155, 140, 195, 153);
				SetRect(&kaija_r, 210, 140, 248, 153);
				if (PtInRect(lp, &gary_r)) {
					if (audio_movie != NULL) {
						StopMovie(audio_movie);
						DisposeMovie(audio_movie);
						DisposeHandle(audio_data);
						audio_movie = NULL;
						audio_data = NULL;
					}
					audio_movie = about_audio_from_memory(realbutter_mp3,
						realbutter_mp3_len, &audio_data);
				}
				if (PtInRect(lp, &kaija_r)) {
					if (audio_movie != NULL) {
						StopMovie(audio_movie);
						DisposeMovie(audio_movie);
						DisposeHandle(audio_data);
						audio_movie = NULL;
						audio_data = NULL;
					}
					audio_movie = about_audio_from_memory(hiitsme_mp3,
						hiitsme_mp3_len, &audio_data);
				}
			}
			break;
		}
		case keyDown:
		case autoKey: {
			char ch = (char)(ev.message & charCodeMask);
			if (ch == '\r' || ch == 0x03 || ch == 0x1B || ch == ' ')
				done = 1;
			break;
		}
		case updateEvt:
			if ((WindowRef)ev.message == win) {
				BeginUpdate(win);
				EndUpdate(win);
			}
			break;
		default:
			break;
		}

		about_draw(off, win, &content, &okr,
			(long)TickCount() - startTick,
			titleFnum, bodyFnum, &black, &white);
	}

	if (audio_movie != NULL) {
		StopMovie(audio_movie);
		DisposeMovie(audio_movie);
		DisposeHandle(audio_data);
	}
	UnlockPixels(offpm);
	DisposeGWorld(off);
	SetPort(savedPort);
	DisposeWindow(win);
	return;

fallback:
	/* Resource-free StandardAlert, built from the same supporter roll.
	 * Reached only if the window or offscreen buffer can't be created. */
	{
		char   body[512];
		char  *p = body;
		short  i, item;
		Str255 ptitle, pbody;
		size_t blen;
		const char *title =
			"MacSurf 2.0.5 - a web browser for Classic Mac OS 9";
		size_t tlen = strlen(title);

		strcpy(p, "Native TLS 1.3 via macTLS. Built on NetSurf.\r"
			"by mplsllc\r\r");
		p += strlen(p);
		for (i = 0; i < ABOUT_ROLL_N; i++) {
			const char *t = about_roll[i].text;
			if (about_roll[i].kind == 2) { *p++ = '\r'; continue; }
			if (about_roll[i].kind == 0) { *p++ = ' '; *p++ = ' '; }
			strcpy(p, t);
			p += strlen(p);
			*p++ = '\r';
		}
		*p = '\0';

		if (tlen > 255) tlen = 255;
		ptitle[0] = (unsigned char) tlen;
		memcpy(ptitle + 1, title, tlen);
		blen = strlen(body);
		if (blen > 255) blen = 255;
		pbody[0] = (unsigned char) blen;
		memcpy(pbody + 1, body, blen);
		StandardAlert(kAlertNoteAlert, ptitle, pbody, NULL, &item);
	}
#endif
}
