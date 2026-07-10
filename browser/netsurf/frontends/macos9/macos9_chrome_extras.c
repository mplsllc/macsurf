/*
 * MacSurf — macos9_chrome_extras.c
 *
 * fixes330+ — View Source / Find-in-page / Bookmarks / History UI.
 *
 * fixes352 (#96 #45 #107) — replace stubs with real implementations:
 *
 *   - View Source: route through content_get_source_data + data: URL
 *     (the invented "view-source:" scheme had no fetcher behind it).
 *   - Find-in-page: programmatic Carbon dialog with TextEdit input +
 *     OK/Cancel buttons; routes to browser_window_search. Search term
 *     cached for Find Again.
 *   - Bookmarks: unchanged in this round — still session-only array;
 *     follow-on round wires desktop/hotlist.c for disk persistence.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "utils/ns_errors.h"
#include "netsurf/browser_window.h"
#include "netsurf/content.h"
#include "macos9.h"
#include "macsurf_debug.h"

#ifdef __MACOS9__
#include <Carbon.h>
#endif

extern struct browser_window *macos9_gw_bw(struct gui_window *g);
extern void macos9_window_navigate(struct gui_window *g, const char *url);
extern const char *nsurl_access(const struct nsurl *u);
extern struct nsurl *browser_window_access_url(
	const struct browser_window *bw);

/* ====================================================================
 * fixes352 (#96) — View Source via data: URL
 *
 * The pre-fix path navigated to "view-source:<url>" which no fetcher
 * recognises (NetSurf core's nsurl.h enumerates HTTP/HTTPS/FILE/FTP/
 * MAILTO/DATA/OTHER — no view-source scheme), so the navigation
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
/* fixes352b (#96) — emit one percent-encoded byte (or pass unreserved
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
 * fixes352 (#45) — Find-in-page via a Carbon dialog
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

	/* fixes352b (#45) — kWindowStandardHandlerAttribute is FORBIDDEN
	 * per CLAUDE.md Known Gotchas.
	 *
	 * fixes352d/e (#45) — kMovableModalWindowClass is rejected by
	 * MacSurf's Carbon CFM build (CarbonLib 1.x returns
	 * errInvalidWindowAttributesForClass / -5601 for every attribute
	 * combo including kWindowNoAttributes). The class itself is the
	 * problem — likely not supported on this CarbonLib.
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
			/* fixes352f (#45) — NetSurf textsearch auto-scrolls
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
 * #48 Bookmarks — fixes645: clickable Bookmarks MENU + disk persistence
 *
 * Old behaviour (fixes351/352): a session-only array of URL strings and
 * a "Show Bookmarks" StandardAlert dump — which the user (rightly)
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

/* Serialize the in-memory set to the on-disk file. Grammar (fixes693):
 *   F<TAB>id<TAB>parent<TAB>name        — a folder
 *   B<TAB>id<TAB>parent<TAB>url<TAB>label — a bookmark
 * A legacy line (no sigil, "url<TAB>label") is still READ as a root bookmark
 * by _restore, so old "MacSurf Bookmarks" files load unchanged; the writer
 * always emits the new grammar. Buffer heap-allocated — never on the stack. */
static void macsurf_bookmarks_persist(void)
{
	char *buf;
	size_t cap, pos = 0;
	int i;
	if (macsurf_bookmark_count <= 0) {
		macos9_bookmarks_save("", 0);
		return;
	}
	cap = (size_t)macsurf_bookmark_count *
		(MACSURF_BMK_URL_MAX + MACSURF_BMK_LBL_MAX + 48) + 8;
	buf = (char *)malloc(cap);
	if (buf == NULL) return;
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
	macos9_bookmarks_save(buf, (long)pos);
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
		struct macsurf_bookmark *b;
		const char *url = "";
		const char *label = "";
		int rec_id = 0, rec_parent = 0, is_folder = 0, legacy = 0;
		size_t ul, ll;
		if (nl != NULL) { *nl = '\0'; p = nl + 1; }
		else { p = line + strlen(line); }
		if (line[0] == '\0') continue;

		if (line[0] == 'F' && line[1] == '\t') {
			char *f[4];
			if (macsurf_bmk_split(line, f, 4) >= 4) {
				is_folder = 1;
				rec_id = atoi(f[1]); rec_parent = atoi(f[2]);
				label = f[3];
			} else continue;
		} else if (line[0] == 'B' && line[1] == '\t') {
			char *f[5];
			int nf = macsurf_bmk_split(line, f, 5);
			if (nf >= 4) {
				rec_id = atoi(f[1]); rec_parent = atoi(f[2]);
				url = f[3];
				label = (nf >= 5) ? f[4] : "";
			} else continue;
		} else {
			/* legacy "url<TAB>label" — root bookmark, id assigned below */
			char *tab = strchr(line, '\t');
			legacy = 1;
			if (tab != NULL) { *tab = '\0'; url = line; label = tab + 1; }
			else { url = line; label = ""; }
		}

		if (!is_folder) {
			ul = strlen(url);
			if (ul == 0 || ul >= MACSURF_BMK_URL_MAX) continue;
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

/* fixes707 — hierarchical Bookmarks menu. Folders were previously skipped
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
 * (de)serializer is never wired — only the cookie jar is) and exposes no
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
 * record per line. Heap buffer — never on the stack. */
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
		/* new entry — make room at the front */
		if (macsurf_hist_n < MACSURF_HISTORY_MAX) macsurf_hist_n++;
		for (i = macsurf_hist_n - 1; i > 0; i--)
			macsurf_hist[i] = macsurf_hist[i - 1];
		found = 0;
		memcpy(macsurf_hist[0].url, href, ul);
		macsurf_hist[0].url[ul] = '\0';
	} else if (found > 0) {
		/* existing — pull it to the front, preserving its url */
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

/* fixes706 — Clear Cache menu handler: wipe the disk cache (cached bodies +
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
 * fixes699 (#47) — History MANAGER WINDOW.
 *
 * A self-contained modal window (built programmatically, no DLOG/DITL,
 * mirroring macos9_find_in_page's proven pattern). Lists every stored
 * visit, grouped under day headers (Today / Yesterday / "Wed, Jul 8,
 * 2026"), newest first. Select a row and press Go / double-click to
 * navigate; Clear History empties the store; Done / close box dismiss.
 *
 * Scrolling is keyboard (arrows / PageUp-Down / Home-End) plus two
 * on-window scroll arrows — deliberately NOT a Carbon scrollbar CDEF,
 * which crashes on real G3/G4 hardware (see CLAUDE.md Known Gotchas).
 * ==================================================================== */
#ifdef __MACOS9__

/* One rendered line: a day header, or a visit (hidx into macsurf_hist[]). */
struct hw_row {
	short is_header;
	short hidx;
	char  text[160];
};

/* Centered rounded push button with a Pascal label. fixes710 — rounded
 * corners read as a real Mac push button rather than a bare box. Shared by
 * both manager windows. */
static void chrome_draw_button(const Rect *r, ConstStr255Param label)
{
	short bw = (short)(r->right - r->left);
	short tw = StringWidth(label);
	EraseRect(r);
	FrameRoundRect(r, 10, 10);
	MoveTo((short)(r->left + (bw - tw) / 2),
	       (short)(r->top + (r->bottom - r->top) / 2 + 4));
	DrawString(label);
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

/* (Re)build the flat display-row list from the store. Returns row count. */
static int hw_build_rows(struct hw_row *rows, int cap, long today_day)
{
	int i, n = 0;
	long prev_day = 0x7FFFFFFFL;
	for (i = 0; i < macsurf_hist_n && n < cap - 1; i++) {
		long day = macsurf_hist[i].ts / 86400L;
		const char *ttl = macsurf_hist[i].title;
		const char *url = macsurf_hist[i].url;
		if (i == 0 || day != prev_day) {
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
	Rect wb, list, up, dn, clr, go, done;
	GrafPtr saved_port;
	EventRecord ev;
	struct hw_row *rows;
	int cap, nrows;
	int scroll_top = 0, sel = -1;
	int row_h = 14, vis;
	long today_day = 0;
	int done_flag = 0;
	char go_url[MACSURF_HIST_URL_MAX];
	int last_click_row = -1;
	unsigned long last_click_time = 0;
	int dirty = 1;

	if (g == NULL) return;
	go_url[0] = '\0';

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
	TextSize(10);

	/* content is 520 x 400 local */
	SetRect(&list, 8, 8, 512, 356);
	SetRect(&up,   494, 8,   512, 30);
	SetRect(&dn,   494, 334, 512, 356);
	SetRect(&clr,  8,   364, 128, 388);
	SetRect(&go,   300, 364, 380, 388);
	SetRect(&done, 420, 364, 512, 388);
	vis = (list.bottom - list.top - 4) / row_h;

	nrows = hw_build_rows(rows, cap, today_day);
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
				/* fixes709 — repaint an uncovered background browser
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
				nrows = hw_build_rows(rows, cap, today_day);
				sel = hw_next_entry(rows, nrows, 0);
				scroll_top = 0;
				last_click_row = -1;
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
			if (PtInRect(lp, &list)) {
				int idx = scroll_top + (lp.v - (list.top + 2)) / row_h;
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
			EraseRect(&list);
			FrameRect(&list);
			ClipRect(&list);
			y = list.top + 2;
			for (r = scroll_top;
			     r < nrows && (r - scroll_top) < vis; r++) {
				short len = (short)strlen(rows[r].text);
				tr.left = (short)(list.left + 1);
				tr.right = (short)(list.right - 20);
				tr.top = (short)y;
				tr.bottom = (short)(y + row_h);
				if (rows[r].is_header) {
					TextFace(bold);
					MoveTo((short)(list.left + 4), (short)(y + 11));
					DrawText(rows[r].text, 0, len);
					TextFace(normal);
				} else {
					MoveTo((short)(list.left + 16), (short)(y + 11));
					DrawText(rows[r].text, 0, len);
					if (r == sel) InvertRect(&tr);
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
			chrome_draw_button(&go, "\pGo");
			chrome_draw_button(&done, "\pDone");
			if (nrows == 0) {
				MoveTo(list.left + 12, list.top + 24);
				DrawString("\p(No history yet)");
			}
			dirty = 0;
		}
	}

	SetPort(saved_port);
	DisposeWindow(win);
	free(rows);

	if (go_url[0] != '\0')
		macos9_window_navigate(g, go_url);
}

#else  /* !__MACOS9__ */
void macos9_history_window_show(struct gui_window *g) { (void)g; }
#endif

/* ====================================================================
 * fixes700 (#50) — Bookmark MANAGER WINDOW.
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
 * the bookmarks parented to it. Returns row count. */
static int bw_build_rows(struct bw_row *rows, int cap)
{
	int i, j, n = 0;
	for (i = 0; i < macsurf_bookmark_count && n < cap; i++) {
		struct macsurf_bookmark *b = &macsurf_bookmarks[i];
		const char *s;
		size_t ln;
		if (b->is_folder || b->parent_id != 0) continue;
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
			rows[n].bidx = (short)j; rows[n].is_folder = 0; rows[n].depth = 1;
			s = (b->label[0] != '\0') ? b->label : b->url;
			ln = strlen(s); if (ln > 150) ln = 150;
			memcpy(rows[n].text, s, ln); rows[n].text[ln] = '\0';
			n++;
		}
	}
	return n;
}

/* fixes708 — move a bookmark into a folder via a POPUP picker (replaces the
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

/* fixes710 — invert a visible list row (self-reversing drop-target hilite). */
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

/* fixes710 — drag-and-drop move. Called on mouseDown on a bookmark row.
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

void macos9_bookmark_window_show(struct gui_window *g)
{
	WindowRef win;
	Rect wb, list, up, dn, nf, rn, del, mv, go, done;
	GrafPtr saved_port;
	EventRecord ev;
	struct bw_row *rows;
	int cap, nrows;
	int scroll_top = 0, sel = -1;
	int row_h = 14, vis;
	int done_flag = 0, dirty = 1;
	char go_url[MACSURF_BMK_URL_MAX];

	if (g == NULL) return;
	go_url[0] = '\0';

	cap = macsurf_bookmark_count * 2 + 4;
	rows = (struct bw_row *)malloc((size_t)cap * sizeof(struct bw_row));
	if (rows == NULL) return;

	SetRect(&wb, 110, 90, 670, 490);
	if (CreateNewWindow(kDocumentWindowClass, kWindowCloseBoxAttribute,
			&wb, &win) != noErr || win == NULL) {
		free(rows);
		return;
	}
	{ Str255 t; c_to_pstring("Bookmarks", t); SetWTitle(win, t); }

	GetPort(&saved_port);
	SetPortWindowPort(win);
	TextFont(1);
	TextSize(10);

	/* content is 560 x 400 local */
	SetRect(&list, 8, 8, 552, 356);
	SetRect(&up,   534, 8,   552, 30);
	SetRect(&dn,   534, 334, 552, 356);
	SetRect(&nf,   8,   364, 92,  388);
	SetRect(&rn,   98,  364, 170, 388);
	SetRect(&del,  176, 364, 240, 388);
	SetRect(&mv,   246, 364, 302, 388);
	SetRect(&go,   430, 364, 486, 388);
	SetRect(&done, 492, 364, 552, 388);
	vis = (list.bottom - list.top - 4) / row_h;

	nrows = bw_build_rows(rows, cap);
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
				/* fixes709 — repaint a background browser window we
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
					char name[MACSURF_BMK_LBL_MAX];
					if (bi >= 0 && bi < macsurf_bookmark_count &&
					    chrome_prompt_text("Rename",
						macsurf_bookmarks[bi].label, name, sizeof name))
						macos9_bookmark_rename(macsurf_bookmarks[bi].id, name);
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
			if (PtInRect(lp, &list)) {
				int idx = scroll_top + (lp.v - (list.top + 2)) / row_h;
				if (idx >= 0 && idx < nrows) {
					sel = idx;
					/* fixes710 — drag a bookmark onto a folder to
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
			nrows = bw_build_rows(rows, cap);
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
			EraseRect(&list);
			FrameRect(&list);
			ClipRect(&list);
			y = list.top + 2;
			for (r = scroll_top;
			     r < nrows && (r - scroll_top) < vis; r++) {
				short len = (short)strlen(rows[r].text);
				short x = (short)(list.left + 4 + rows[r].depth * 16);
				tr.left = (short)(list.left + 1);
				tr.right = (short)(list.right - 20);
				tr.top = (short)y;
				tr.bottom = (short)(y + row_h);
				if (rows[r].is_folder) TextFace(bold);
				MoveTo(x, (short)(y + 11));
				DrawText(rows[r].text, 0, len);
				if (rows[r].is_folder) TextFace(normal);
				if (r == sel) InvertRect(&tr);
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
			chrome_draw_button(&go, "\pGo");
			chrome_draw_button(&done, "\pDone");
			if (nrows == 0) {
				MoveTo(list.left + 12, list.top + 24);
				DrawString("\p(No bookmarks yet)");
			}
			dirty = 0;
		}
	}

	SetPort(saved_port);
	DisposeWindow(win);
	free(rows);

	if (go_url[0] != '\0')
		macos9_window_navigate(g, go_url);
}

#else  /* !__MACOS9__ */
void macos9_bookmark_window_show(struct gui_window *g) { (void)g; }
#endif

/* Legacy "Show Bookmarks" alert — retained for ABI but no longer menu-
 * wired (the live menu supersedes it). */
void macos9_bookmark_list_show(struct gui_window *g)
{
	(void)g;
}

/* About box — shown from the Apple menu's "About MacSurf..." item. Carries the
 * project credit and the Patreon supporter roll. When a new supporter joins,
 * add their name to the SUPPORTERS line below (and to README.md's Supporters
 * section). Kept as a plain StandardAlert so it needs no DITL/'ALRT' resource. */
void macos9_about_show(void)
{
#ifdef __MACOS9__
	/* Build the Pascal strings at RUNTIME (length byte + bytes). A literal
	 * "\p..." only length-prefixes the first token, so splitting a Pascal
	 * literal across concatenated lines yields a wrong length; and the text
	 * renders as MacRoman, so keep it ASCII (no em dash). */
	static const char *title =
		"MacSurf 1.68.3 - a web browser for Classic Mac OS 9";
	static const char *body =
		"Native TLS 1.3 via macTLS. Built on the NetSurf engine.\r\r"
		"by mplsllc\r\r"
		"Patreon supporters:\r"
		"    Shlooom\r\r"
		"Ko-Fi supporters:\r"
		"    kilgeist\r"
		"    Turuun\r\r"
		"Thank you for keeping vintage Macs on the modern web.";
	Str255 ptitle;
	Str255 pbody;
	size_t tlen;
	size_t blen;
	short item;

	tlen = strlen(title);
	if (tlen > 255) tlen = 255;
	ptitle[0] = (unsigned char) tlen;
	memcpy(ptitle + 1, title, tlen);

	blen = strlen(body);
	if (blen > 255) blen = 255;
	pbody[0] = (unsigned char) blen;
	memcpy(pbody + 1, body, blen);

	StandardAlert(kAlertNoteAlert, ptitle, pbody, NULL, &item);
#endif
}
