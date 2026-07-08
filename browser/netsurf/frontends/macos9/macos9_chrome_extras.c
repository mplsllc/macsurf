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

/* Rebuild the dynamic portion of the Bookmarks menu (everything after
 * the item-2 separator). Called after add/load. AppendMenu interprets
 * metacharacters ('/', ';', '(', '-', ...) which URLs are full of, so
 * we append a placeholder then SetMenuItemText the real (meta-safe)
 * label.
 *
 * NOTE: we do NOT call CountMenuItems — on this CW8 SDK it macro-maps to
 * the classic CountMItems, which is absent from the linked library (link
 * error). Instead we track how many dynamic items we appended last time
 * in `prev_dynamic` and walk item indices ourselves (fixed layout: item
 * 1 = Add, item 2 = separator, items 3.. = bookmarks). */
void macos9_bookmark_menu_rebuild(void)
{
#ifdef __MACOS9__
	static int prev_dynamic = 0;
	MenuHandle m = GetMenuHandle(MENU_BOOKMARK);
	int i;
	short item_index;
	if (m == NULL) return;
	/* Delete the previously-appended block. Deleting item 3 repeatedly
	 * collapses it (indices shift down after each delete). */
	while (prev_dynamic > 0) { DeleteMenuItem(m, 3); prev_dynamic--; }
	/* fixes693: folders are organizational (managed in the bookmark window),
	 * so the flat menu lists only actual bookmarks. macsurf_bmk_menu_map[k]
	 * records which array slot the k-th menu item came from, so navigate()
	 * maps a menu item back to the right bookmark even with folders present. */
	macsurf_bmk_menu_n = 0;
	item_index = 2;                 /* last fixed item (separator) */
	for (i = 0; i < macsurf_bookmark_count; i++) {
		Str255 pt;
		const char *s;
		size_t ln;
		if (macsurf_bookmarks[i].is_folder) continue;
		s = (macsurf_bookmarks[i].label[0] != '\0') ?
			macsurf_bookmarks[i].label : macsurf_bookmarks[i].url;
		ln = strlen(s);
		if (ln > 80) ln = 80;
		pt[0] = (unsigned char)ln;
		memcpy(pt + 1, s, ln);
		AppendMenu(m, "\px");
		item_index++;
		SetMenuItemText(m, item_index, pt);
		macsurf_bmk_menu_map[macsurf_bmk_menu_n++] = i;
	}
	if (macsurf_bmk_menu_n == 0) {
		/* Leading '(' renders the item disabled — a greyed hint. */
		AppendMenu(m, "\p(No bookmarks yet");
		prev_dynamic = 1;
		return;
	}
	prev_dynamic = macsurf_bmk_menu_n;
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
		"MacSurf 1.68.2 - a web browser for Classic Mac OS 9";
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
