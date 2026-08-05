#include "macos9.h"
#include <stdio.h>
#include <string.h>
#include "utils/log.h"
#include "utils/nsurl.h"
#include "netsurf/netsurf.h"
#include "netsurf/browser_window.h"
#include "netsurf/plotters.h"
#include "desktop/gui_table.h"
#include "utils/nsoption.h"
#include "macsurf_config.h"
#include "macsurf_debug.h"
#include "macsurf_memory.h"    /* macsurf_recon_mem() */
#include "macsurf_timebase.h"
#include "macsurf_osver.h"     /* fixes936 -- macsurf_os_is_osx() */

#ifdef __MACOS9__
#include <OpenTransport.h>
#include <OpenTptInternet.h>
#include <Movies.h>
#include <Processes.h>   /* fixes983 -- our own FSSpec, for the icon claim */
#include <Files.h>       /* fixes983 -- FSpGetFInfo / FSpSetFInfo */
OTClientContextPtr macos9_ot_context = NULL;
/* macTLS expects this symbol; aliased to our OT context after init. */
OTClientContextPtr g_ostls_ot_context = NULL;
/* fixes656 — last content scroll offset, published for the core sticky-overlay
 * hit-test (interaction.c compute_sticky_shift). Set just before every
 * browser_window_mouse_click/_track so the hit-test can recover the PAINTED
 * position of position:sticky boxes (which are pinned to a viewport edge at
 * paint time but hit-tested at their un-pinned layout position). */
int macos9_hittest_scroll_x = 0;
int macos9_hittest_scroll_y = 0;
/* macEntropy host hooks (macTLS os9/ostls_entropy.h). Declared locally,
 * matching this file's extern idiom, to avoid pulling BearSSL headers
 * into main.c. LoadSeed warms cold-start; StirEntropy feeds event jitter
 * from the poll loop; SaveSeed persists the session's entropy at quit. */
extern void OSTLS_LoadSeed(void);
extern void OSTLS_SaveSeed(void);
extern void OSTLS_StirEntropy(const void *data, unsigned long len);
/* fixes368 (#167) — cookie-jar persistence (impl in macos9_disk_cache.c).
 * Load after netsurf_init so a prior Facebook login is restored; save at
 * quit so this session's login survives the relaunch. Declared extern here
 * to avoid pulling macos9_disk_cache.h's includes into main.c. */
extern void macos9_cookies_load(void);
extern void macos9_cookies_save(void);
/* Event-loop callbacks — declared here to avoid pulling internal headers
 * into main.c. Each is called from macos9_poll() once per loop pass. */
extern bool   macos9_schedule_run(void);
extern void   fetch_pump(void);
extern int    macos9_op_depth;
extern void   macos9_animation_tick(void);
extern void   macsurf_qjs_pump_all(void);
extern void   macos9_deathrow_drain(void);
/* JS init: js_initialise/js_finalise are declared in the shared content js.h
 * and provided by the QuickJS engine glue (macsurf_qjs.c). */
#ifdef WITH_QUICKJS
#include "content/handlers/javascript/js.h"
#endif
#ifndef kInitOTForApplicationMask
#define kInitOTForApplicationMask 0x00000002
#endif
#ifndef activeFlag
#define activeFlag 0x0001
#endif
/* fixes141 — defensive event-class whitelist.
 * mUpMask: required for TrackControl on push buttons and scroll bars.
 * activMask: needed for activateEvt (URL field + button hilite on focus change).
 * Wheel events are NOT in the mask — CarbonLib on OS 9 crashes when
 * it attempts to dispatch kEventMouseWheelMoved (CarbonLib: not available).
 *
 * fixes507 — osMask and highLevelEventMask REMOVED. MacSurf's
 * WaitNextEvent dispatch (switch (ev.what) in main.c) only handles
 * updateEvt / mouseDown / keyDown / autoKey / activateEvt; osEvt (what=15
 * suspend/resume) and kHighLevelEvent (AppleEvents) fall to default:break,
 * so we processed neither. But requesting them forced the Toolbox through
 * its TSM / high-level delivery machinery inside WaitNextEvent. With no
 * InitTSMAwareApplication / NewTSMDocument call, the Text Services Manager
 * has no document state and _TSMEvent crashed writing a near-zero pointer
 * (00010DE0, low-memory) during event delivery. We never consumed these
 * events; not requesting them keeps the Toolbox off that code path. */
#define MACOS9_EVENT_MASK (mDownMask | mUpMask | keyDownMask | autoKeyMask | \
	updateMask | activMask)
#else
#define MACOS9_EVENT_MASK everyEvent
#endif

bool macos9_done = (bool)0;
bool macos9_quitting = (bool)0;
struct netsurf_table macos9_table;
extern const struct plotter_table macos9_plotters;

#ifdef __MACOS9__
/* fixes77c -- CW8's Quickdraw.h omits the Carbon accessor prototype. The
 * symbol is in CarbonLib 1.0+, so an explicit declaration is enough. */
#endif

/* fixes366j -- heap-state probes for per-reformat leak / fragmentation
 * profiling. FreeMem() is total free bytes in the app heap; MaxBlock()
 * is the largest contiguous free block. If free shrinks monotonically
 * across reformats -> leak; if MaxBlock collapses faster than FreeMem
 * -> fragmentation, and the Memory Manager's compaction pass on each
 * NewPtr is what makes successive layout passes explode (5s -> 239s on
 * a stable 1993-box tree). Both are CarbonLib-safe classic Memory
 * Manager calls. Return long (KB on huge heaps stays < 2^31). */
long macos9_heap_free_bytes(void)
{
#ifdef __MACOS9__
	return (long)FreeMem();
#else
	return 0;
#endif
}

long macos9_heap_max_block(void)
{
#ifdef __MACOS9__
	return (long)MaxBlock();
#else
	return 0;
#endif
}

/* fixes366l -- absolute Microseconds() as a double, for measuring a
 * single layout_document pass directly (the layout-done STAMP delta is
 * misleading: it spans all the inter-reformat network/fetch work, which
 * on mactrove is dominated by TLS handshakes). Double avoids the CW8
 * PPC 64-bit-multiply miscompile; IEEE-754's 52-bit mantissa covers the
 * full 2^32*hi+lo range exactly. Caller subtracts two readings and
 * casts the small delta to long microseconds. */
double macos9_micros(void)
{
#ifdef __MACOS9__
	UnsignedWide w;
	Microseconds(&w);
	return (double)w.hi * 4294967296.0 + (double)w.lo;
#else
	return 0.0;
#endif
}

static void draw_url_bar(struct gui_window *gw) {
#ifdef __MACOS9__
	/* fixes727 — rounded "pill" URL field. White rounded canvas, a soft grey
	 * border at rest and a 2px orange focus ring while the field is active for
	 * typing. Geneva 12 text, favicon at the left. The rounded corners sit on
	 * the toolbar gradient, which draw_toolbar_bg repaints just before this. */
	RGBColor black  = {0, 0, 0};
	RGBColor white  = {0xFFFF, 0xFFFF, 0xFFFF};
	RGBColor border = {0x9999, 0x9999, 0x9999};    /* #999 soft edge */
	RGBColor orange = {0xF4F4, 0x8484, 0x1616};    /* focus accent */
	Rect u = gw->url_rect;

	/* white rounded canvas */
	RGBForeColor(&white);
	PaintRoundRect(&u, 12, 12);
	/* border — orange focus ring when active, soft grey otherwise */
	if (gw->url_field_active) {
		RGBForeColor(&orange); PenSize(2, 2);
	} else {
		RGBForeColor(&border); PenSize(1, 1);
	}
	FrameRoundRect(&u, 12, 12);
	PenSize(1, 1);

	/* text */
	RGBForeColor(&black); RGBBackColor(&white);
	TextFont(kFontIDGeneva); TextSize(12); TextFace(0);
	if (gw->url_te != NULL) TEUpdate(&gw->url_rect, gw->url_te);
	/* fixes294 — favicon paints on top of the URL field's white
	 * background.  No-op if the default favicon failed to load. */
	macos9_window_draw_favicon(gw);
#endif
}

static void draw_status_bar(struct gui_window *gw) {
#ifdef __MACOS9__
	/* fixes727 — refined status bar: a soft light-grey fill with a 1px top
	 * highlight/separator instead of the old boxy black FrameRect, and lighter
	 * #555 text. Reads as a subtle chrome shelf rather than a stark box. */
	RGBColor fill  = {0xEDED, 0xEDED, 0xEDED};   /* #EDEDED shelf */
	RGBColor top   = {0xB4B4, 0xB4B4, 0xB4B4};   /* #B4 separator */
	RGBColor text  = {0x5555, 0x5555, 0x5555};   /* #555 text */
	RGBColor bg    = {0xEDED, 0xEDED, 0xEDED};
	Rect r = gw->status_rect;
	Rect line;
	RGBForeColor(&fill); RGBBackColor(&bg);
	PaintRect(&r);
	line.left = r.left; line.right = r.right;
	line.top = r.top; line.bottom = (short)(r.top + 1);
	RGBForeColor(&top); PaintRect(&line);
	/* fixes627: pin a small chrome font (Geneva 9) before drawing status /
	 * hover-URL text so it doesn't inherit a huge content-plot size. */
	TextFont(kFontIDGeneva); TextSize(9); TextFace(0);
	RGBForeColor(&text);
	MoveTo((short)(r.left+6), (short)(r.bottom-4));
	if (gw->status[0]) {
		unsigned char p[128]; size_t l = strlen(gw->status);
		if(l>127) l=127; p[0]=(unsigned char)l; memcpy(p+1, gw->status, l);
		DrawString(p);
	}
#endif
}

static void macos9_init_menus(void) {
#ifdef __MACOS9__
	MenuHandle apple_menu, file_menu, edit_menu, go_menu;
	apple_menu = NewMenu(MENU_APPLE, "\p\024");
	AppendMenu(apple_menu, "\pAbout MacSurf...");
	AppendMenu(apple_menu, "\p(-");
	/* fixes753 (#228) — do NOT AppendResMenu('DRVR') here. Under Carbon /
	 * CarbonLib the Menu Manager auto-populates the Apple menu with the
	 * Apple Menu Items folder AND auto-handles their selection. Adding them
	 * ourselves produced a SECOND copy of every item (the "duplicated Apple
	 * menu when MacSurf is frontmost" bug) — and the app never dispatched
	 * them anyway (the MENU_APPLE handler only acts on item 1 = About).
	 * The system's single auto-added copy is the correct one. */
	InsertMenu(apple_menu, 0);

	file_menu = NewMenu(MENU_FILE, "\pFile");
	AppendMenu(file_menu, "\pNew Window/N");
	AppendMenu(file_menu, "\pOpen Location.../L");
	AppendMenu(file_menu, "\pClose/W");
	AppendMenu(file_menu, "\pSend Debug Log");   /* fixes720 (item 4) */
	AppendMenu(file_menu, "\p(-");
	AppendMenu(file_menu, "\pQuit/Q");
	InsertMenu(file_menu, 0);

	edit_menu = NewMenu(MENU_EDIT, "\pEdit");
	AppendMenu(edit_menu, "\pUndo/Z");
	AppendMenu(edit_menu, "\p(-");
	AppendMenu(edit_menu, "\pCut/X");
	AppendMenu(edit_menu, "\pCopy/C");
	AppendMenu(edit_menu, "\pPaste/V");
	AppendMenu(edit_menu, "\p(-");                 /* fixes743 (#214) */
	AppendMenu(edit_menu, "\pSelect All/A");
	InsertMenu(edit_menu, 0);

	go_menu = NewMenu(MENU_GO, "\pGo");
	AppendMenu(go_menu, "\pBack/[");
	AppendMenu(go_menu, "\pForward/]");
	AppendMenu(go_menu, "\pStop/.");
	AppendMenu(go_menu, "\pReload/R");
	AppendMenu(go_menu, "\p(-");
	AppendMenu(go_menu, "\pHome/H");
	InsertMenu(go_menu, 0);

	/* fixes330 (#96 #45) — View menu with View Source + Find. */
	{
		MenuHandle view_menu = NewMenu(MENU_VIEW, "\pView");
		AppendMenu(view_menu, "\pView Source/U");
		AppendMenu(view_menu, "\p(-");
		AppendMenu(view_menu, "\pFind.../F");
		/* fixes883 — Zoom and Downloads: both already worked, neither was
		 * discoverable. Page zoom was reachable only through Cmd -/+/0
		 * keystrokes that appeared in no menu, and the download-manager
		 * window only ever opened by starting a download. Keep the item
		 * numbering in sync with the ITEM_VIEW_* defines in macos9.h. */
		AppendMenu(view_menu, "\p(-");                    /* 4 */
		AppendMenu(view_menu, "\pZoom In/+");             /* 5 */
		AppendMenu(view_menu, "\pZoom Out/-");            /* 6 */
		AppendMenu(view_menu, "\pActual Size/0");         /* 7 */
		AppendMenu(view_menu, "\p(-");                    /* 8 */
		AppendMenu(view_menu, "\pDownloads");             /* 9 */
		InsertMenu(view_menu, 0);
	}

	/* fixes645 (#48) — Bookmarks menu. Item 1 = Add, item 2 = separator,
	 * items 3+ are the saved bookmarks themselves (filled in by
	 * macos9_bookmarks_init below and rebuilt on each add). Selecting a
	 * bookmark navigates the front window to it. */
	{
		MenuHandle bookmark_menu = NewMenu(MENU_BOOKMARK, "\pBookmarks");
		AppendMenu(bookmark_menu, "\pAdd Bookmark/D");
		AppendMenu(bookmark_menu, "\pManage Bookmarks\311/B");
		AppendMenu(bookmark_menu, "\p(-");
		InsertMenu(bookmark_menu, 0);
	}

	/* fixes694/698 (#47) — History menu. Item 1 = Clear History, item 2 =
	 * separator, items 3+ = recent visits (most-recent first) filled in by
	 * macos9_history_init below and rebuilt on each menu-bar click. */
	{
		MenuHandle history_menu = NewMenu(MENU_HISTORY, "\pHistory");
		AppendMenu(history_menu, "\pShow All History/H");
		AppendMenu(history_menu, "\pClear History");
		AppendMenu(history_menu, "\pClear Cache");
		AppendMenu(history_menu, "\p(-");
		InsertMenu(history_menu, 0);
	}

	DrawMenuBar();

	/* fixes645 (#48) — load persisted bookmarks and populate the menu.
	 * Must run after the menu is inserted so GetMenuHandle finds it. */
	macos9_bookmarks_init();
	macos9_history_init();   /* fixes694 (#47) */
#endif
}

/* fixes720 (#207 tooling) — File > Send Debug Log. MacSurf has no file picker,
 * so instead of the web upload form we read our OWN Desktop log and POST it
 * straight to the server (urlencoded, reusing the fixes312 POST path). One
 * click, no SimpleText/copy/paste, no picker. The server (upload.php) accepts
 * the `logtext` field and saves it. Plain http:// so the classic-UA vhost
 * serves it without an https redirect. */
static void macos9_send_debug_log(struct gui_window *gw)
{
#ifdef __MACOS9__
	char *raw;
	char *body;
	long  rawlen, i, bp;
	long  cap = 1024L * 1024L;   /* 1 MB — a non-nuclear log is far smaller */
	nsurl *url = NULL;
	static const char hexd[] = "0123456789ABCDEF";
	static const char pfx[] = "who=MacSurf&logtext=";

	if (gw == NULL || gw->bw == NULL) return;
	raw = (char *)malloc((size_t)cap);
	if (raw == NULL) return;
	rawlen = macsurf_debug_log_read(raw, cap);
	if (rawlen <= 0) { free(raw); return; }

	/* url-encode: worst case 3 bytes per input byte + prefix + NUL. */
	body = (char *)malloc((size_t)(rawlen * 3 + (long)sizeof pfx + 4));
	if (body == NULL) { free(raw); return; }
	bp = 0;
	for (i = 0; pfx[i] != '\0'; i++) body[bp++] = pfx[i];
	for (i = 0; i < rawlen; i++) {
		unsigned char ch = (unsigned char)raw[i];
		if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
		    (ch >= '0' && ch <= '9') ||
		    ch == '-' || ch == '_' || ch == '.' || ch == '~') {
			body[bp++] = (char)ch;
		} else {
			body[bp++] = '%';
			body[bp++] = hexd[(ch >> 4) & 0x0F];
			body[bp++] = hexd[ch & 0x0F];
		}
	}
	body[bp] = '\0';
	free(raw);

	if (nsurl_create("http://macsurf.org/upload.php", &url) == NSERROR_OK) {
		macsurf_profile_reset();
		/* post_urlenc is arg 5; core strdup's it, so freeing body after is
		 * safe. Navigates the front window to the "thank you" response. */
		browser_window_navigate(gw->bw, url, NULL, BW_NAVIGATE_HISTORY,
			body, NULL, NULL);
		nsurl_unref(url);
	}
	free(body);
#else
	(void)gw;
#endif
}

/* fixes886 — forward declaration, and it is REQUIRED here.
 *
 * fixes883 defined macos9_zoom_apply just above macos9_handle_key_down (~1213),
 * but macos9_handle_menu below calls it at ~474 -- EARLIER in the file. C89 then
 * implicitly declares it `int()` at that first call, and the real
 * `static void (struct gui_window *, float, int)` definition conflicts:
 *     Error: identifier 'macos9_zoom_apply(...)' redeclared
 *            was declared as: 'int (...)'
 * followed by a cascade of "undefined identifier 'gw'" as CW8 discards the
 * parameter list it could not reconcile.
 *
 * The Linux pre-flight missed this twice over: gcc in C89 mode ALLOWS an
 * implicit declaration, and more importantly the check never reached this code
 * at all -- main.c dies at a missing OpenTransport.h, which the check was
 * reporting as an unchanged error count rather than as "did not run". Both are
 * addressed in tools/retro68_check.sh. */
static void macos9_zoom_apply(struct gui_window *gw, float amount, int absolute);

static void macos9_handle_menu(short menu_id, short item) {
#ifdef __MACOS9__
	WindowRef front;
	struct gui_window *gw = NULL;	/* fixes369a — init to silence CW8
					 * "used before initialized" (assigned
					 * per-case below before use). */
	macsurf_debug_log_writef(
		"fixes352c handle_menu: menu_id=%d item=%d",
		(int)menu_id, (int)item);
	/* fixes707 — a bookmark folder submenu selection carries the submenu's
	 * own ID as menu_id; route it to the folder's Nth bookmark. */
	if (menu_id >= MENU_BMK_SUB_BASE &&
	    menu_id < MENU_BMK_SUB_BASE + MENU_BMK_SUB_MAX) {
		front = FrontWindow();
		gw = front ? macos9_find_window(front) : NULL;
		if (gw != NULL)
			macos9_bookmark_submenu_navigate(gw, menu_id, item);
		HiliteMenu(0);
		return;
	}
	switch (menu_id) {
	case MENU_APPLE:
		/* Item 1 = "About MacSurf..."; items 3+ are desk accessories. */
		if (item == 1) {
			extern void macos9_about_show(void);
			macos9_about_show();
		}
		break;
	case MENU_FILE:
		switch (item) {
		case ITEM_FILE_NEW: {
			struct browser_window *bw = NULL;
			nsurl *home = NULL;
			if (nsurl_create(MACSURF_HOME_URL, &home) == NSERROR_OK) {
				/* fixes161a — mark next setup as DOCUMENT class. */
				extern void macos9_http_mark_next_as_document(void);
				macos9_http_mark_next_as_document();
				/* fixes366a — fresh phase clock for this navigation. */
				macsurf_profile_reset();
				macsurf_profile_stamp("nav: File>New home");
				browser_window_create(BW_CREATE_HISTORY | BW_CREATE_FOREGROUND,
					home, NULL, NULL, &bw);
				nsurl_unref(home);
			}
		} break;
		case ITEM_FILE_LOCATION: {
			/* fixes109 — Cmd+L focuses the URL bar and selects all so
			 * the next keystroke replaces the existing URL. Was a
			 * dead menu entry before this fix (menu accepted Cmd+L
			 * via the "/L" suffix but no handler ran, so the user
			 * got no visible response). */
			WindowRef wfront = FrontWindow();
			struct gui_window *gwl = wfront ? macos9_find_window(wfront) : NULL;
			if (gwl != NULL) {
				extern void macos9_window_te_activate_url(struct gui_window *);
				macos9_window_te_activate_url(gwl);
				if (gwl->url_te != NULL) {
					SetPortWindowPort(gwl->window);
					TESetSelect(0, 32767, gwl->url_te);
				}
			}
		} break;
		case ITEM_FILE_CLOSE:
			/* fixes641 (#189): Cmd-W / File>Close closes ONLY the front
			 * window (was a dead menu item — no case existed). Same
			 * per-window teardown as the go-away box. */
			front = FrontWindow();
			gw = front ? macos9_find_window(front) : NULL;
			if (gw != NULL) {
				if (gw->bw != NULL)
					browser_window_destroy(gw->bw);
				else
					macos9_window_destroy(gw);
				if (macos9_window_list_head() == NULL)
					macos9_done = (bool)1;
			}
			break;
		case ITEM_FILE_SENDLOG:
			/* fixes720: POST the Desktop MacSurf Debug.log to the server. */
			front = FrontWindow();
			gw = front ? macos9_find_window(front) : NULL;
			if (gw != NULL)
				macos9_send_debug_log(gw);
			break;
		case ITEM_FILE_QUIT:
			macos9_done = (bool)1;
			break;
		default: break;
		}
		break;
	case MENU_GO:
		front = FrontWindow();
		gw = front ? macos9_find_window(front) : NULL;
		if (!gw) break;
		switch (item) {
		case ITEM_GO_BACK:    macos9_window_back(gw); break;
		case ITEM_GO_FORWARD: macos9_window_forward(gw); break;
		case ITEM_GO_RELOAD:  macos9_window_reload(gw); break;
		case ITEM_GO_HOME:    macos9_window_home(gw); break;
		default: break;
		}
		break;
	case MENU_VIEW:
		front = FrontWindow();
		gw = front ? macos9_find_window(front) : NULL;
		if (!gw) break;
		switch (item) {
		case ITEM_VIEW_SOURCE: {
			/* fixes330 (#96) — open view-source: URL in a new
			 * window. The source fetcher path is wired separately
			 * in the resource stub fetchers; for V1 we open a new
			 * tab with view-source: prefix and a future resource
			 * handler can format the bytes. */
			extern void macos9_view_source_for_window(
				struct gui_window *g);
			macos9_view_source_for_window(gw);
		} break;
		case ITEM_VIEW_FIND: {
			/* fixes330 (#45) — find-in-page via prompt for now.
			 * Future: real Carbon dialog with search controls. */
			extern void macos9_find_in_page(struct gui_window *g);
			macsurf_debug_log_writef(
				"fixes352c ITEM_VIEW_FIND: gw=%p", (void *)gw);
			macos9_find_in_page(gw);
		} break;
		/* fixes883 — same helper as the Cmd -/+/0 keystrokes. */
		case ITEM_VIEW_ZOOM_IN:
			macos9_zoom_apply(gw, 0.1, 0);
			break;
		case ITEM_VIEW_ZOOM_OUT:
			macos9_zoom_apply(gw, -0.1, 0);
			break;
		case ITEM_VIEW_ZOOM_100:
			macos9_zoom_apply(gw, 1.0, 1);
			break;
		case ITEM_VIEW_DOWNLOADS:
			/* fixes883 — the manager window was previously
			 * reachable only by starting a download. */
			macos9_download_mgr_show();
			break;
		default: break;
		}
		break;
	case MENU_BOOKMARK:
		/* fixes641 (#200): derive the front window FIRST (this case used
		 * to run with an uninitialized `gw`). */
		front = FrontWindow();
		gw = front ? macos9_find_window(front) : NULL;
		if (gw == NULL) break;
		/* fixes645 (#48) — item 1 adds the current page; items >= 3 are
		 * saved bookmarks (item 2 is the separator, never selectable) and
		 * navigate the front window to their URL. */
		if (item == ITEM_BMK_ADD) {
			extern void macos9_bookmark_add(struct gui_window *g);
			macos9_bookmark_add(gw);
		} else if (item == ITEM_BMK_MANAGE) {
			macos9_bookmark_window_show(gw);
		} else if (item >= ITEM_BMK_FIRST) {
			macos9_bookmark_navigate(gw, item);
		}
		break;
	case MENU_HISTORY:
		/* fixes694/698 (#47) — item 1 clears history; items >= 3 are
		 * recent-visit entries (item 2 is the separator, never selectable)
		 * and navigate the front window. Menu refreshed on menu-bar click. */
		front = FrontWindow();
		gw = front ? macos9_find_window(front) : NULL;
		if (gw == NULL) break;
		if (item == ITEM_HIST_SHOW_ALL) {
			macos9_history_window_show(gw);
		} else if (item == ITEM_HIST_CLEAR) {
			macos9_history_clear();
		} else if (item == ITEM_HIST_CLEAR_CACHE) {
			macos9_cache_clear_ui();
		} else if (item >= ITEM_HIST_FIRST) {
			macos9_history_navigate(gw, item);
		}
		break;
	case MENU_EDIT:
		/* fixes376 — the Edit menu was dead (items appended, no
		 * dispatcher). Cmd-X/C/V reach here via MenuKey too. If the URL
		 * field has focus, operate on its TextEdit; otherwise deliver
		 * the NetSurf NS_KEY_* edit code to the page so in-page form
		 * fields (textarea/input) copy/cut/paste via desktop/textarea.c
		 * + the clipboard.c Scrap callbacks. */
		front = FrontWindow();
		gw = front ? macos9_find_window(front) : NULL;
		if (gw == NULL)
			break;
		if (gw->url_field_active && gw->url_te != NULL) {
			switch (item) {
			case ITEM_EDIT_CUT:
			case ITEM_EDIT_COPY:
			case ITEM_EDIT_PASTE:
				macos9_url_te_edit(gw, item);
				break;
			case ITEM_EDIT_SELECT_ALL:   /* fixes743 (#214) */
				TESetSelect(0, 32767, gw->url_te);
				InvalWindowRect(gw->window, &gw->url_rect);
				break;
			default: break;
			}
		} else if (gw->bw != NULL) {
			unsigned long code = 0;
			extern bool browser_window_key_press(
				struct browser_window *, unsigned long);
			switch (item) {
			case ITEM_EDIT_CUT:   code = 24; break; /* NS_KEY_CUT_SELECTION  */
			case ITEM_EDIT_COPY:  code = 3;  break; /* NS_KEY_COPY_SELECTION */
			case ITEM_EDIT_PASTE: code = 22; break; /* NS_KEY_PASTE          */
			case ITEM_EDIT_SELECT_ALL: code = 1; break; /* NS_KEY_SELECT_ALL — fixes743 */
			default:              code = 0;  break; /* Undo: no core key     */
			}
			if (code != 0) {
				(void)browser_window_key_press(gw->bw, code);
				macos9_window_invalidate_content(gw);
			}
		}
		break;
	default: break;
	}
	HiliteMenu(0);
#else
	(void)menu_id; (void)item;
#endif
}

/* fixes659: erase the page base to WHITE, not the port's default grey.
 * fixes629 removed the UA body{background:#fff} (it painted white OVER dark
 * sites); the side effect is that a LIGHT site whose <html> has no background
 * (XenForo/68kmla set their page colour on wrapper classes, not <html>) no
 * longer covers the grey EraseRect base, so bare regions (e.g. the account
 * nav strip) show grey. White is the correct browser-default canvas. Dark
 * sites are unaffected: their <html> background paints over the full viewport,
 * covering this erase exactly as it covered the old grey one. bkColor is
 * saved/restored so anti-aliased text blending elsewhere is untouched. */
static void macos9_erase_content_base(const Rect *r)
{
	RGBColor sv_bk, wht;
	GetBackColor(&sv_bk);
	wht.red = wht.green = wht.blue = 0xFFFF;
	RGBBackColor(&wht);
	EraseRect(r);
	RGBBackColor(&sv_bk);
}

/* fixes709 — non-static so the modal manager windows (macos9_chrome_extras.c)
 * can repaint background browser windows exposed while they're dragged. */
void macos9_handle_update(const EventRecord *event) {
#ifdef __MACOS9__
	WindowRef win = (WindowRef)(unsigned long)event->message;
	struct gui_window *gw = macos9_find_window(win);
	CGrafPtr  saved_port = NULL;
	GDHandle  saved_gdh  = NULL;
	PixMapHandle gwpm    = NULL;
	RgnHandle vr         = NULL;
	Boolean   gworld_active = (Boolean)0;
	Rect      off_bounds;
	Rect      update_bounds;
	int       off_w, off_h;
	/* fixes645 (#199): the modeless download-manager window is not a
	 * gui_window, so macos9_find_window returns NULL and the update would
	 * be dropped. Draw it here and return before the gui_window path. */
	if (macos9_download_mgr_is(win)) { macos9_download_mgr_draw(); return; }
	if (!gw || macos9_quitting) return;
	SetPortWindowPort(win); BeginUpdate(win);
	/* fixes77f -- offscreen GWorld V2.
	 *
	 * Architecture (correcting fixes77c's failure mode):
	 *   1. BeginUpdate has already set the window port's visRgn to the
	 *      update region. Capture its bounding box -- this is the dirty
	 *      area in window coords.
	 *   2. Allocate / reuse a GWorld matching content_rect's size.
	 *   3. SetGWorld + SetOrigin(content_rect.left, content_rect.top) so
	 *      the GWorld's local coords are window coords. Pixel (0,0) of
	 *      the GWorld pixmap is addressed as port coord
	 *      (content_rect.left, content_rect.top).
	 *   4. NO SetClip. plotters.c manages QD clip exclusively via its
	 *      plot_clip callback. Calling SetClip here corrupts plotters.c's
	 *      clip-tracking state machine -- that was fixes77c's bug.
	 *   5. EraseRect the dirty bbox in window coords. After SetOrigin,
	 *      QD subtracts the origin to find pixel coords, so the erased
	 *      pixels line up 1:1 with the pixels plotters will overwrite.
	 *   6. Pass update_bounds (cast to struct rect) as the clip arg to
	 *      browser_window_redraw. NetSurf's box-tree walker uses this to
	 *      prune branches: only boxes whose bbox intersects update_bounds
	 *      visit plotters. For a per-rect animation tick this collapses
	 *      the walk from ~60 boxes to ~5.
	 *   7. SetGWorld back to window. Window's visRgn is still the update
	 *      region (BeginUpdate set it). CopyBits content_rect to
	 *      content_rect: QD intrinsically hardware-clips the blit to
	 *      visRgn, so only dirty pixels reach the screen.
	 *
	 * The badge animation's old frame stays on screen until the new
	 * frame is fully composed in the GWorld; the swap is atomic from
	 * the user's perspective. No EraseRect-then-paint flash. */
	vr = NewRgn();
	if (vr != NULL) {
		GetPortVisibleRegion(GetWindowPort(win), vr);
		GetRegionBounds(vr, &update_bounds);
		DisposeRgn(vr); vr = NULL;
	} else {
		/* Last-resort fallback: assume whole content area is dirty. */
		update_bounds = gw->content_rect;
	}
	off_bounds = gw->content_rect;
	off_w = off_bounds.right - off_bounds.left;
	off_h = off_bounds.bottom - off_bounds.top;
	if (gw->content_gworld != NULL &&
	    (gw->content_gworld_rect.right  - gw->content_gworld_rect.left  != off_w ||
	     gw->content_gworld_rect.bottom - gw->content_gworld_rect.top  != off_h)) {
		DisposeGWorld(gw->content_gworld);
		gw->content_gworld = NULL;
	}
	if (gw->content_gworld == NULL && off_w > 0 && off_h > 0) {
		Rect r; r.left = 0; r.top = 0;
		r.right = (short)off_w; r.bottom = (short)off_h;
		if (NewGWorld(&gw->content_gworld, 0, &r, NULL, NULL, 0) != noErr) {
			gw->content_gworld = NULL;
		} else {
			gw->content_gworld_rect = off_bounds;
		}
	}
	if (gw->content_gworld != NULL) {
		gwpm = GetGWorldPixMap(gw->content_gworld);
		if (gwpm != NULL && LockPixels(gwpm)) {
			GetGWorld(&saved_port, &saved_gdh);
			SetGWorld(gw->content_gworld, NULL);
			SetOrigin(off_bounds.left, off_bounds.top);
			gworld_active = (Boolean)1;
		} else {
			gwpm = NULL;
		}
	}
	if (gworld_active) {
		/* Erase only the dirty bbox; window-coord rect lines up with
		 * pixel rows correctly because SetOrigin already ran. */
		macos9_erase_content_base(&update_bounds);
	} else {
		/* Fallback path: paint directly into window. Flash returns. */
		Boolean fb_top_dirty = (Boolean)(update_bounds.top < gw->content_rect.top);
		Boolean fb_bot_dirty = (Boolean)(update_bounds.bottom > gw->content_rect.bottom);
		macos9_erase_content_base(&gw->content_rect);
		if (fb_top_dirty) {
			macos9_window_draw_toolbar_bg(gw);
			draw_url_bar(gw);
			DrawControls(win);
			macos9_window_draw_toolbar_icons(gw);
		}
		if (fb_bot_dirty) {
			draw_status_bar(gw);
		}
	}
	{ extern struct hlcache_handle *browser_window_get_content(struct browser_window *);
	  struct hlcache_handle *cur = gw->bw ? browser_window_get_content(gw->bw) : NULL;
	  macsurf_debug_log_writef("update: bw=%p current_content=%p ready=%d",
	    gw->bw, cur,
	    (gw->bw && browser_window_redraw_ready(gw->bw)) ? 1 : 0); }
	if (gw->bw && browser_window_redraw_ready(gw->bw)) {
		struct rect clip; struct redraw_context ctx;
		macsurf_debug_log_writef(
			"update: redraw_ready, bw=%p scroll=(%d,%d) crect=(%d,%d,%d,%d) ub=(%d,%d,%d,%d) gw=%d",
			gw->bw, gw->scroll_x, gw->scroll_y,
			(int)gw->content_rect.left, (int)gw->content_rect.top,
			(int)gw->content_rect.right, (int)gw->content_rect.bottom,
			(int)update_bounds.left, (int)update_bounds.top,
			(int)update_bounds.right, (int)update_bounds.bottom,
			(int)gworld_active);
		/* Clip = update_bounds (the dirty bbox), in window coords.
		 * NetSurf's box-tree walker prunes branches outside this. */
		clip.x0 = update_bounds.left; clip.y0 = update_bounds.top;
		clip.x1 = update_bounds.right; clip.y1 = update_bounds.bottom;
		memset(&ctx, 0, sizeof(ctx));
		ctx.interactive = (bool)1;
		ctx.background_images = (bool)1;
		ctx.plot = &macos9_plotters;
		{ extern struct gui_window *macos9_paint_gw;
		  macos9_paint_gw = gw; }
		/* Stage 1: the redraw box-tree walk dereferences content/box
		 * objects; keep op_depth>0 so the death-row drain cannot fire
		 * and free one mid-walk. */
		{ extern int macos9_op_depth; macos9_op_depth++; }
		{
			/* fixes640 — accumulate paint CPU (full box-tree redraw). */
			/* fixes759 (#212) DIAGNOSTIC — time this paint and count boxes
			 * walked. RECON KEY only measured key_press; the real per-keystroke
			 * cost on a heavy page is THIS full-tree redraw fired on the
			 * update event. clip = dirty rect (small when only a textarea line
			 * changed); boxes = how many the walk visited regardless of clip
			 * (if ~all page boxes, the tree walk isn't pruning to the clip). */
			double t_paint = macos9_micros();
			extern long macos9_hrb_visits;
			long _v0 = macos9_hrb_visits, _pdt;
			browser_window_redraw(gw->bw,
				gw->content_rect.left - gw->scroll_x,
				gw->content_rect.top  - gw->scroll_y,
				&clip, &ctx);
			_pdt = (long)(macos9_micros() - t_paint);
			macsurf_profile_accum_paint(_pdt);
			macsurf_debug_log_writef("RECON PAINT dt=%ldus clip=%dx%d boxes=%ld",
				_pdt, (int)(clip.x1 - clip.x0), (int)(clip.y1 - clip.y0),
				(long)(macos9_hrb_visits - _v0));
		}
		{ extern int macos9_op_depth; macos9_op_depth--; }
		{ extern struct gui_window *macos9_paint_gw;
		  macos9_paint_gw = NULL; }
		/* fixes451: emit profile stamp + PROFILE line only once per page;
		 * profile_emitted is reset by macos9_gw_set_url on navigation. */
		if (!gw->profile_emitted) {
			struct nsurl *pu;
			gw->profile_emitted = 1;
			macsurf_profile_stamp("first-paint-done");
			pu = (gw && gw->bw) ?
				browser_window_access_url(gw->bw) : NULL;
			macsurf_profile_emit(pu ? nsurl_access(pu) : "(unknown)");
		}
		if (gworld_active) {
			const BitMap *src_bm;
			const BitMap *dst_bm;
			RGBColor blk; RGBColor wht;
			Rect blit_r;   /* fixes825: dirty-rect blit, see below */
			blk.red = 0; blk.green = 0; blk.blue = 0;
			wht.red = 0xFFFF; wht.green = 0xFFFF; wht.blue = 0xFFFF;
			SetGWorld(saved_port, saved_gdh);
			src_bm = GetPortBitMapForCopyBits(
				(CGrafPtr)gw->content_gworld);
			dst_bm = GetPortBitMapForCopyBits(GetWindowPort(win));
			RGBForeColor(&blk); RGBBackColor(&wht);
			/* fixes825 (#212/#239): blit only the DIRTY rect, not the
			 * whole viewport. The old code passed off_bounds (the entire
			 * content_rect) as src+dst and relied on the dst visRgn to
			 * clip -- but classic-QD CopyBits sets up its blit loop over
			 * the RECT it's handed, so a full-viewport rect costs a
			 * full-viewport blit (~20ms measured per keystroke via WORK
			 * keylat) even when a single form field changed. The GWorld's
			 * non-dirty pixels already match the screen (only update_bounds
			 * was erased+redrawn this pass), so blitting just the dirty
			 * area keeps them in sync at a fraction of the cost. Clamp to
			 * content_rect first: update_bounds can include chrome rows
			 * that lie outside the GWorld (content-sized), and reading
			 * those would blit garbage. */
			blit_r = update_bounds;
			if (blit_r.left   < gw->content_rect.left)
				blit_r.left   = gw->content_rect.left;
			if (blit_r.top    < gw->content_rect.top)
				blit_r.top    = gw->content_rect.top;
			if (blit_r.right  > gw->content_rect.right)
				blit_r.right  = gw->content_rect.right;
			if (blit_r.bottom > gw->content_rect.bottom)
				blit_r.bottom = gw->content_rect.bottom;
			if (blit_r.right > blit_r.left &&
			    blit_r.bottom > blit_r.top) {
				CopyBits(src_bm, dst_bm, &blit_r, &blit_r,
				         srcCopy, NULL);
			}
			if (gwpm != NULL) UnlockPixels(gwpm);
			gworld_active = (Boolean)0;
		}
		/* fixes297h / fixes298 — only paint chrome regions that are
		 * actually dirty.  Top chrome = toolbar+URL bar+buttons.
		 * Bottom chrome = status bar.  Gradient bg paints first so
		 * the URL bar and button icons sit on top of it. */
		{
			Boolean top_dirty = (Boolean)(update_bounds.top < gw->content_rect.top);
			Boolean bot_dirty = (Boolean)(update_bounds.bottom > gw->content_rect.bottom);
			if (top_dirty) {
				macos9_window_draw_toolbar_bg(gw);
				draw_url_bar(gw);
				DrawControls(win);
				macos9_window_draw_toolbar_icons(gw);
			}
			if (bot_dirty) {
				draw_status_bar(gw);
			}
		}
		if (gw->url_field_active && gw->url_te) TEActivate(gw->url_te);
	} else if (gw->bw) {
		MS_LOG("update: bw not ready, skip");
	}
	if (gworld_active) {
		/* Defensive: bw branch didn't run; tear down GWorld port. */
		SetGWorld(saved_port, saved_gdh);
		if (gwpm != NULL) UnlockPixels(gwpm);
		gworld_active = (Boolean)0;
	}
	/* fixes663 (#191): draw the in-page text caret on top of the just-
	 * composited content, in window coords, clipped to the content area.
	 * caret_x/y are document coords from macos9_gw_place_caret; convert the
	 * same way macos9_gw_invalidate does. Only the blink-ON phase draws;
	 * QuickDraw hardware-clips to BeginUpdate's dirty region so a content
	 * repaint elsewhere never disturbs a steady caret. */
	if (gw->caret_active && gw->caret_on) {
		int cwx = gw->content_rect.left + gw->caret_x - gw->scroll_x;
		int cy0 = gw->content_rect.top  + gw->caret_y - gw->scroll_y;
		int cy1 = cy0 + gw->caret_h;
		if (cwx >= gw->content_rect.left && cwx < gw->content_rect.right) {
			RGBColor blk;
			RgnHandle savedClip = NewRgn();
			/* #252: honour the focused field's CSS caret-color. The ns
			 * colour primitive is 0x00BBGGRR (see nscss_color_to_ns and
			 * plotters.c macos9_colour_to_rgb). Falls back to black for
			 * caret-color:auto or when no editable box is focused, so the
			 * default caret is unchanged. */
			colour caretcol = 0;
			struct hlcache_handle *carh = (gw->bw != NULL) ?
					browser_window_get_content(gw->bw) : NULL;
			extern bool html_get_caret_colour(struct hlcache_handle *h,
					colour *colour_out);
			if (cy0 < gw->content_rect.top)    cy0 = gw->content_rect.top;
			if (cy1 > gw->content_rect.bottom) cy1 = gw->content_rect.bottom;
			SetPortWindowPort(win);
			if (savedClip != NULL) GetClip(savedClip);
			ClipRect(&gw->content_rect);
			if (carh != NULL && html_get_caret_colour(carh, &caretcol)) {
				blk.red   = (unsigned short)(((caretcol >>  0) & 0xff) * 257);
				blk.green = (unsigned short)(((caretcol >>  8) & 0xff) * 257);
				blk.blue  = (unsigned short)(((caretcol >> 16) & 0xff) * 257);
			} else {
				blk.red = blk.green = blk.blue = 0;
			}
			RGBForeColor(&blk);
			PenNormal();
			MoveTo((short)cwx, (short)cy0);
			LineTo((short)cwx, (short)cy1);
			if (savedClip != NULL) { SetClip(savedClip); DisposeRgn(savedClip); }
		}
	}
	EndUpdate(win);
	macos9_urlsug_draw(gw);   /* fixes763 — redraw dropdown atop fresh content */
	/* fixes738 — viewport-gated image loading. After the content is
	 * composited and the box tree is stable, fetch any deferred images now
	 * within (viewport + margin) and drop dead queue entries; off-screen
	 * images stay queued until a later scroll/paint. Runs only when
	 * redraw_ready (content laid out) so the box walk is safe. */
	if (gw != NULL && gw->bw != NULL &&
	    browser_window_redraw_ready(gw->bw)) {
		extern void macsurf_lazyimg_viewport_changed(int scroll_y,
				int viewport_h);
		macsurf_lazyimg_viewport_changed(gw->scroll_y,
			gw->content_rect.bottom - gw->content_rect.top);
	}
#endif
}

void macos9_handle_mouse_down(const EventRecord *event) {
#ifdef __MACOS9__
	WindowRef win;
	short part = FindWindow(event->where, &win);
	struct gui_window *gw;
	/* fixes645 (#199): route clicks on the modeless download-manager
	 * window (drag / close / select) — it is not a gui_window. */
	if (macos9_download_mgr_is(win)) {
		macos9_download_mgr_click(part, event->where);
		return;
	}
	switch (part) {
		case inMenuBar: {
			long sel;
			/* fixes694 (#47) — refresh the History menu from urldb just
			 * before the user picks from it, so it reflects the latest
			 * visits. Cheap: a bounded top-N snapshot walk. */
			macos9_history_menu_rebuild();
			sel = MenuSelect(event->where);
			if (sel != 0) macos9_handle_menu((short)((sel >> 16) & 0xFFFF),
				(short)(sel & 0xFFFF));
			HiliteMenu(0);
		} break;
		case inDrag:
			if (win) {
				Rect bounds;
				bounds.left = -32000; bounds.top = -32000;
				bounds.right = 32000; bounds.bottom = 32000;
				DragWindow(win, event->where, &bounds);
			}
			break;
		case inGoAway:
			/* fixes641 (#189): close ONLY the clicked window, not the
			 * whole app. The old code set the global macos9_done quit
			 * flag, so closing a 2nd window (or either window) exited
			 * the run loop and netsurf_exit tore down BOTH OS windows.
			 * browser_window_destroy cascades through the gui destroy
			 * vtable into macos9_window_destroy, which unlinks just this
			 * one gui_window and cancels its scheduled callbacks. Quit
			 * only when the LAST window is gone (Mac convention). */
			if (win && TrackGoAway(win, event->where)) {
				struct gui_window *cgw = macos9_find_window(win);
				if (cgw != NULL && cgw->bw != NULL) {
					browser_window_destroy(cgw->bw);
				} else if (cgw != NULL) {
					macos9_window_destroy(cgw);
				}
				if (macos9_window_list_head() == NULL)
					macos9_done = (bool)1;
			}
			break;
		case inGrow:
			if (win) {
				Rect lim;
				long sz;
				lim.left = 200; lim.top = 100;
				lim.right = 32000; lim.bottom = 32000;
				sz = GrowWindow(win, event->where, &lim);
				if (sz != 0) {
					SizeWindow(win, (short)(sz & 0xFFFF), (short)((sz >> 16) & 0xFFFF), (Boolean)1);
					gw = macos9_find_window(win);
					if (gw) macos9_window_resize(gw);
				}
			}
			break;
		case inZoomIn:
		case inZoomOut:
			/* fixes645 (#188): the zoom/maximize box (top-right of the
			 * title bar) had no useful handler. We DON'T rely on
			 * ZoomWindow/ZoomWindowIdeal: CreateNewWindow never populates
			 * the classic WStateData standard state, so the Window
			 * Manager's own zoom would jump to garbage, and its inZoomIn
			 * vs inZoomOut part code isn't reliable without that state.
			 * Instead we track zoom ourselves: first click saves the
			 * current content bounds and fills the screen (below the menu
			 * bar); next click restores the saved bounds. */
			if (win && TrackBox(win, event->where, part)) {
				gw = macos9_find_window(win);
				if (gw != NULL) {
					SetPortWindowPort(win);
					if (!gw->zoomed) {
						/* fixes646 (#188): use Set/GetWindowBounds on the
						 * SAME region (content, 33) so the save/restore is
						 * an exact round-trip. The fixes645 MoveWindow +
						 * SizeWindow drifted because MoveWindow repositions
						 * a different origin than the content bounds we
						 * saved, so each cycle shifted the window and the
						 * fill math looked wrong. SetWindowBounds sets the
						 * content region to an exact global Rect in one
						 * call — no move/size ambiguity. */
						Rect content;
						BitMap qd;
						short mbar;
						GetWindowBounds(win, 33,
							&gw->zoom_saved_bounds);
						GetQDGlobalsScreenBits(&qd);
						mbar = GetMBarHeight();
						content.left = (short)(qd.bounds.left + 4);
						content.top = (short)(qd.bounds.top + mbar + 22);
						content.right = (short)(qd.bounds.right - 4);
						content.bottom = (short)(qd.bounds.bottom - 6);
						SetWindowBounds(win, 33, &content);
						gw->zoomed = 1;
						macsurf_debug_log_writef(
							"zoom max: scr=%d,%d,%d,%d mbar=%d -> content=%d,%d,%d,%d",
							(int)qd.bounds.top, (int)qd.bounds.left,
							(int)qd.bounds.bottom, (int)qd.bounds.right,
							(int)mbar, (int)content.top, (int)content.left,
							(int)content.bottom, (int)content.right);
					} else {
						SetWindowBounds(win, 33,
							&gw->zoom_saved_bounds);
						gw->zoomed = 0;
						macsurf_debug_log_writef(
							"zoom restore: content=%d,%d,%d,%d",
							(int)gw->zoom_saved_bounds.top,
							(int)gw->zoom_saved_bounds.left,
							(int)gw->zoom_saved_bounds.bottom,
							(int)gw->zoom_saved_bounds.right);
					}
					macos9_window_resize(gw);
				}
			}
			break;
		case inContent:
			if (win) {
				if (win != FrontWindow()) {
					SelectWindow(win);
				} else {
					Point p = event->where;
					ControlRef ctrl;
					short cpart;
					gw = macos9_find_window(win);
					if (gw) {
						SetPortWindowPort(win);
						GlobalToLocal(&p);
						/* fixes298b — user-pane buttons aren't visible to
						 * FindControl (the default user-pane hit-test
						 * returns kControlNoPart, and Carbon interprets
						 * that as "click not in any control" → returns
						 * ctrl=NULL).  Hit-test our four toolbar buttons
						 * manually via PtInRect BEFORE FindControl runs.
						 * tbtn != NULL after this block means we handled
						 * the click; skip FindControl + the URL/content
						 * dispatch. */
						{
							ControlRef tbtn = NULL;
							void (*tact)(struct gui_window *) = NULL;
							Rect bb;
							if (gw->back_btn) { GetControlBounds(gw->back_btn, &bb);
								if (PtInRect(p, &bb)) { tbtn = gw->back_btn; tact = macos9_window_back; } }
							if (tbtn == NULL && gw->forward_btn) { GetControlBounds(gw->forward_btn, &bb);
								if (PtInRect(p, &bb)) { tbtn = gw->forward_btn; tact = macos9_window_forward; } }
							if (tbtn == NULL && gw->stop_btn) { GetControlBounds(gw->stop_btn, &bb);
								if (PtInRect(p, &bb)) { tbtn = gw->stop_btn; tact = macos9_window_stop; } }   /* fixes724 */
							if (tbtn == NULL && gw->reload_btn) { GetControlBounds(gw->reload_btn, &bb);
								if (PtInRect(p, &bb)) { tbtn = gw->reload_btn; tact = macos9_window_reload; } }
							if (tbtn == NULL && gw->home_btn) { GetControlBounds(gw->home_btn, &bb);
								if (PtInRect(p, &bb)) { tbtn = gw->home_btn; tact = macos9_window_home; } }
							if (tbtn != NULL) {
								Point up;
								GetControlBounds(tbtn, &bb);
								while (StillDown()) { /* wait for release */ }
								GetMouse(&up);
								if (PtInRect(up, &bb)) tact(gw);
								cpart = 0; ctrl = (ControlRef)(long)1;
								/* sentinel: non-NULL ctrl with cpart=0
								 * so the if-chain below skips all
								 * branches (each requires either cpart
								 * non-zero or PtInRect-not-handled). */
							} else {
								cpart = FindControl(p, win, &ctrl);
							}
						}
						if (cpart != 0 && ctrl != NULL) {
							if (ctrl == gw->vscroll || ctrl == gw->hscroll) {
								macos9_window_handle_scrollbar_click(gw, ctrl, cpart, &p);
							}
						} else if (ctrl == NULL && macos9_urlsug_click(gw, p)) {
							/* fixes763 — a suggestion row was clicked + navigated */
						} else if (ctrl == NULL && PtInRect(p, &gw->url_rect)) {
							macos9_urlsug_hide(gw);   /* fixes763 dismiss stale suggestions */
							macos9_window_te_activate_url(gw);
							if (gw->url_te) { TEClick(p, (event->modifiers & shiftKey) != 0, gw->url_te); TESelView(gw->url_te); } /* fixes756 (#229) */
						} else if (ctrl == NULL && PtInRect(p, &gw->content_rect) && gw->bw) {
							/* fixes763: dropdown is hidden by the
							 * macos9_window_te_deactivate_url() call below. */
							/* Click in content area — dispatch to NetSurf so
							 * links navigate, forms submit, etc.  Coordinates
							 * are translated from window-local to NetSurf
							 * content space (= local - content origin + scroll). */
							int x_ns, y_ns, rx_ns, ry_ns;
							browser_mouse_state mods = 0;
							Point relp;
							int px_ns, py_ns, cx_ns, cy_ns, last_cx, last_cy;
							int dragging = 0;
							Point curp;
							unsigned long last_draw_tick = 0; /* fixes747 */
							unsigned long last_scroll_tick = 0; /* fixes749 #216 */
							macos9_window_te_deactivate_url(gw);
							if (event->modifiers & shiftKey)
								mods |= BROWSER_MOUSE_MOD_1;
							if (event->modifiers & controlKey)
								mods |= BROWSER_MOUSE_MOD_2;
							if (event->modifiers & optionKey)
								mods |= BROWSER_MOUSE_MOD_3;
							x_ns = (int)p.h - gw->content_rect.left + gw->scroll_x;
							y_ns = (int)p.v - gw->content_rect.top  + gw->scroll_y;
							macos9_hittest_scroll_x = gw->scroll_x;
							macos9_hittest_scroll_y = gw->scroll_y;
							MS_LOG("content: PRESS_1");
							browser_window_mouse_click(gw->bw,
								BROWSER_MOUSE_PRESS_1 | mods,
								x_ns, y_ns);
							/* fixes662 (#192): drive a real drag so in-page text SELECTION works.
							 * Old code busy-spun through StillDown and only sent PRESS then CLICK,
							 * so NetSurf's textarea never saw the DRAG_1/HOLDING_1 it needs to build
							 * a selection. Now: poll the mouse while held; once it moves past a small
							 * threshold, start the drag (mouse_click DRAG_1 at the press point) and
							 * feed mouse_track(DRAG_ON|HOLDING_1) as it moves; on release end the drag
							 * with mouse_track(0). A pure click (no movement) still fires CLICK_1 so
							 * links/buttons/caret placement are unchanged. Mirrors the framebuffer
							 * frontend's fb_browser_window_move drag model. */
							px_ns = x_ns; py_ns = y_ns;
							last_cx = x_ns; last_cy = y_ns;
							while (StillDown()) {
								int dx, dy;
								GetMouse(&curp);
								/* fixes749 (#216) - auto-scroll while selecting: if the cursor is
								 * dragged above/below the content area, scroll the page (throttled)
								 * so the selection extends past the visible region. scroll_y updates
								 * before cx/cy so mouse_track below extends to the new position. */
									if (dragging)
										macos9_drag_autoscroll(gw, curp, &last_scroll_tick);
								cx_ns = (int)curp.h - gw->content_rect.left + gw->scroll_x;
								cy_ns = (int)curp.v - gw->content_rect.top  + gw->scroll_y;
								if (!dragging) {
									dx = cx_ns - px_ns; if (dx < 0) dx = -dx;
									dy = cy_ns - py_ns; if (dy < 0) dy = -dy;
									if (dx > 4 || dy > 4) {
										dragging = 1;
										browser_window_mouse_click(gw->bw,
											BROWSER_MOUSE_DRAG_1 | mods, px_ns, py_ns);
									}
								}
								if (dragging && (cx_ns != last_cx || cy_ns != last_cy)) {
									last_cx = cx_ns; last_cy = cy_ns;
									browser_window_mouse_track(gw->bw,
										BROWSER_MOUSE_DRAG_ON | BROWSER_MOUSE_HOLDING_1 | mods,
										cx_ns, cy_ns);
									/* fixes747 — LIVE selection feedback. mouse_track
									 * invalidates the changed selection rows, but the
									 * StillDown loop blocks the event loop so nothing
									 * repaints until release (selection felt frozen /
									 * "not smooth"). Repaint the pending delta here,
									 * throttled to ~20fps so a big page's box-walk
									 * doesn't stall the drag. handle_update only reads
									 * event->message, so a synthetic updateEvt is safe. */
									macos9_throttled_repaint(gw, &last_draw_tick);
								}
							GetMouse(&relp);
							rx_ns = (int)relp.h - gw->content_rect.left + gw->scroll_x;
							ry_ns = (int)relp.v - gw->content_rect.top  + gw->scroll_y;
							if (dragging) {
								MS_LOG("content: DRAG end");
								browser_window_mouse_track(gw->bw, 0, rx_ns, ry_ns);
							} else {
								MS_LOG("content: CLICK_1");
								browser_window_mouse_click(gw->bw,
									BROWSER_MOUSE_CLICK_1 | mods, rx_ns, ry_ns);
							}
							}
							/* fixes882: this said "inline onclick handlers run
						 * natively in the JS engine". They do not. Nothing
						 * compiles an on* HTML ATTRIBUTE into a handler --
						 * the _H map is only ever populated by a JS
						 * assignment (el.onclick = fn) through the prototype
						 * accessors, so <button onclick="f()"> never runs f.
						 * And even a JS-assigned handler is unreachable from
						 * a real click, because the bridge at
						 * interaction.c's fire-dom-click site is a `return 0`
						 * stub (#300). See the note there. */
						} else {
							macos9_window_te_deactivate_url(gw);
						}
					}
				}
			}
			break;
	}
#endif
}

/* fixes886 — definition; the forward declaration is up with the other
 * prototypes, because macos9_handle_menu (far above) calls this.
 *
 * fixes883 — ONE zoom implementation, shared by the Cmd -/+/0 keystrokes and
 * the new View menu items, so the two paths cannot drift apart.
 *
 * NetSurf's scale sets the layout viewport to (window / scale), so zooming OUT
 * lays the page out at a WIDER effective width -- the desktop layout -- then
 * renders it scaled down to fit the physical window. On a 1024x768 Mac that
 * both shrinks-to-fit and takes the wide layout path instead of the cramped
 * narrow-column one (fixes621). */
static void macos9_zoom_apply(struct gui_window *gw, float amount, int absolute)
{
	extern nserror browser_window_set_scale(
		struct browser_window *bw, float scale, bool absolute);
	if (gw == NULL || gw->bw == NULL) return;
	browser_window_set_scale(gw->bw, amount, absolute ? true : false);
	gw->needs_reformat = 1;
	macos9_window_invalidate_content(gw);
}

void macos9_handle_key_down(const EventRecord *event) {
#ifdef __MACOS9__
	WindowRef win = FrontWindow();
	struct gui_window *gw = win ? macos9_find_window(win) : NULL;
	char ch = (char)(event->message & charCodeMask);
	if (event->modifiers & cmdKey) {
		long sel;
		/* fixes621: Cmd -/+/0 page zoom. NetSurf's scale sets the
		 * layout viewport to (window / scale), so zooming OUT lays the
		 * page out at a WIDER effective width -- the desktop layout --
		 * then renders it scaled down to fit the physical window. On a
		 * 1024x768 Mac this both shrinks-to-fit and takes the wide
		 * layout path instead of the cramped narrow-column one.
		 * Cmd-minus = out, Cmd-plus/equals = in, Cmd-0 = reset 100%. */
		if (gw != NULL && gw->bw != NULL &&
		    (ch == '-' || ch == '_' || ch == '=' || ch == '+' ||
		     ch == '0')) {
			/* fixes883 — same helper the View > Zoom items call.
			 * Note this intercepts BEFORE MenuKey below, so the
			 * menu items' Cmd shortcuts land here rather than
			 * dispatching through the menu; identical result, and
			 * the menu entries exist so the keystrokes are
			 * discoverable at all. */
			if (ch == '0') {
				macos9_zoom_apply(gw, 1.0, 1);
			} else if (ch == '-' || ch == '_') {
				macos9_zoom_apply(gw, -0.1, 0);
			} else {
				macos9_zoom_apply(gw, 0.1, 0);
			}
			return;
		}
		sel = MenuKey(ch);
		if (sel != 0) {
			macos9_handle_menu((short)((sel >> 16) & 0xFFFF),
				(short)(sel & 0xFFFF));
			return;
		}
	}
	if (!gw) return;
	if (gw->url_field_active && gw->url_te) {
		extern int  macos9_urlsug_active(struct gui_window *g);
		extern int  macos9_urlsug_move(struct gui_window *g, int dir);
		extern int  macos9_url_typeahead(struct gui_window *g);
		extern void macos9_urlsug_refresh(struct gui_window *g);
		if (ch == 0x0D || ch == 0x03) {
			macos9_window_address_bar_submit(gw);
		} else if (ch == 0x1B) {
			macos9_window_te_deactivate_url(gw);
		} else if ((ch == 0x1F || ch == 0x1E) && macos9_urlsug_active(gw)) {
			macos9_urlsug_move(gw, (ch == 0x1F) ? 1 : -1);  /* fixes763 arrow nav */
		} else {
			int did_ac = 0;
			SetPortWindowPort(gw->window);
			TEKey(ch, gw->url_te);
			/* fixes762 — inline history autocomplete on FORWARD typing only
			 * (skip backspace/delete/control so editing isn't fought). When a
			 * completion is inserted it shows the URL start with the tail
			 * selected, so don't scroll the caret (end) into view. */
			if ((unsigned char)ch >= 0x20 && (unsigned char)ch != 0x7F) {
				did_ac = macos9_url_typeahead(gw);  /* fixes763 dropdown + inline */
			} else {
				macos9_urlsug_refresh(gw);          /* fixes763 rebuild on delete */
			}
			if (!did_ac)
				TESelView(gw->url_te);  /* fixes756 (#229) caret into view */
			InvalWindowRect(gw->window, &gw->url_rect);
		}
	} else {
		unsigned char uc;
		unsigned long ns_key;
		int have_key;
		Boolean shift_down;
		uc = (unsigned char)ch;
		shift_down = (event->modifiers & shiftKey) != 0;
		/* fixes660 (#191/#192/#194): translate the Mac key code to an
		 * NS_KEY_* value and offer it to the focused in-page field FIRST
		 * (caret move, Tab focus traversal, shift-arrow selection, edit).
		 * Most Mac control-char codes already EQUAL their NS_KEY_* value:
		 * arrows 0x1C-0x1F = NS_KEY_LEFT..DOWN 28-31, Tab 0x09 = 9,
		 * backspace 0x08 = 8, forward-delete 0x7F = 127, CR 0x0D = 13;
		 * only shift-Tab, Home, End and Page keys need remapping. If a
		 * field consumes it we return; otherwise (nothing focused ->
		 * content_keypress returns false) we fall through to window
		 * scrolling for the navigation keys, exactly as before. */
		ns_key = 0; have_key = 0;
		if (uc >= 0x20 && uc < 0x7F) { ns_key = uc; have_key = 1; }
		else switch (uc) {
			case 0x08: ns_key = 8;   have_key = 1; break; /* backspace   */
			case 0x7F: ns_key = 127; have_key = 1; break; /* fwd delete  */
			case 0x0D: ns_key = 13;  have_key = 1; break; /* return      */
			case 0x09: ns_key = shift_down ? 11 : 9; have_key = 1; break; /* (shift-)tab */
			case 0x19: ns_key = 11;  have_key = 1; break; /* back-tab    */
			case 0x1C: ns_key = 28;  have_key = 1; break; /* left        */
			case 0x1D: ns_key = 29;  have_key = 1; break; /* right       */
			case 0x1E: ns_key = 30;  have_key = 1; break; /* up          */
			case 0x1F: ns_key = 31;  have_key = 1; break; /* down        */
			case 0x01: ns_key = 128; have_key = 1; break; /* home = LINE_START */
			case 0x04: ns_key = 129; have_key = 1; break; /* end  = LINE_END   */
			case 0x0B: ns_key = 136; have_key = 1; break; /* pgup = PAGE_UP    */
			case 0x0C: ns_key = 137; have_key = 1; break; /* pgdn = PAGE_DOWN  */
			default: break;
		}
		if (have_key && gw->bw) {
				extern bool browser_window_key_press(struct browser_window *, unsigned long);
				int _consumed;
				_consumed = browser_window_key_press(gw->bw, ns_key) ? 1 : 0;
				if (_consumed) {
					/* fixes760 (#212): paint the edit NOW instead of waiting for
					 * the next event-loop updateEvt. While a page is still loading
					 * the loop spins the fetcher (idle=0ms) and delays that
					 * updateEvt, so each typed letter appeared with a visible lag.
					 * A synthetic updateEvt repaints the just-invalidated region
					 * immediately (same technique as drag-select fixes747;
					 * handle_update only reads event->message). The fixes825
					 * dirty-rect blit keeps this cheap (~2ms/keystroke). */
					if (gw->window != NULL) {
						EventRecord _uev;
						_uev.what = updateEvt;
						_uev.message = (long)(unsigned long)gw->window;
						macos9_handle_update(&_uev);
					}
					return;
				}
		}
		/* not consumed by a field -> window navigation scrolling */
		switch (ch) {
			case 0x1E: macos9_window_scroll_by(gw, 0, -48); break; /* up */
			case 0x1F: macos9_window_scroll_by(gw, 0,  48); break; /* down */
			case 0x1C: macos9_window_scroll_by(gw, -48, 0); break; /* left */
			case 0x1D: macos9_window_scroll_by(gw,  48, 0); break; /* right */
			case 0x0B: macos9_window_scroll_by(gw, 0, -gw->content_rect.bottom + gw->content_rect.top); break; /* page up */
			case 0x0C: macos9_window_scroll_by(gw, 0,  gw->content_rect.bottom - gw->content_rect.top); break; /* page down */
			case 0x01: macos9_window_scroll_to(gw, 0, 0); break; /* home */
			case 0x04: macos9_window_scroll_to(gw, 0, 0x7FFFFFFF); break; /* end */
			default: break;
		}
	}
#endif
}

static void macos9_handle_activate(const EventRecord *event) {
#ifdef __MACOS9__
	WindowRef win = (WindowRef)(unsigned long)event->message;
	struct gui_window *gw = win ? macos9_find_window(win) : NULL;
	bool becoming_active = (event->modifiers & activeFlag) != 0;
	if (!gw) return;
	SetPortWindowPort(win);
	if (becoming_active) {
		if (gw->url_field_active && gw->url_te) TEActivate(gw->url_te);
	} else {
		if (gw->url_te) TEDeactivate(gw->url_te);
	}
	macos9_window_update_button_states(gw);
	macos9_window_invalidate_all(gw);
#else
	(void)event;
#endif
}

void macos9_poll(void) {
	EventRecord ev;
	/* fixes937 (OS X tier 1b): one-shot proof that the event loop was
	 * actually REACHED. Everything between InitOT and here is otherwise
	 * invisible in a shipped build -- the per-milestone MS_LOGs are now
	 * "BOOT " prefixed and whitelisted, but this is the one that separates
	 * "died during startup" from "started, then died in the loop". */
	{
		static int loop_marked = 0;
		if (!loop_marked) {
			loop_marked = 1;
			MS_LOG("BOOT event loop entered");
			macsurf_debug_log_flush();
		}
	}
	/* fixes583 DIAG: main-event-loop heartbeat (throttled ~2s). If the
	 * browser freezes and THIS stops while 'qjs: interrupt hb' keeps pulsing,
	 * the loop is blocked inside js_exec (runaway JS). If this keeps pulsing
	 * with no other progress, it's a stall, not a freeze. If BOTH stop, the
	 * event loop is wedged in a non-JS tight loop. */
	{
		static unsigned long hb_last = 0;
		unsigned long hb_now = (unsigned long)TickCount();
		if (hb_now - hb_last > 120) {
			hb_last = hb_now;
			macsurf_debug_log_writef("evloop: hb tick=%ld", (long)hb_now);
			/* fixes953 — the live OT snapshot that used to run here is retired.
			 * It walked all 64 fetch slots every ~2s to catch an OS X hang that
			 * is now fixed, and its output no longer passes the crash-only log
			 * gate anyway. macos9_https_recon_ot_tick() is left in the fetcher
			 * for the next investigation; nothing calls it. */
		}
	}
	/* fixes234a — revert sleep=0 (fixes234). On Carbon CFM, sleep=0 in
	 * WaitNextEvent yields effectively zero quantum to the OS scheduler.
	 * OT's notifier callbacks (which set nf_data_pending) starve, so
	 * even though packets arrive in the network buffer, our pump can't
	 * see them. Captured diag: pumps=570737 ot_recv_calls=553581
	 * nodata=553495 (99.98% no-data) — pure busy-spin. Going back to
	 * sleep=1 gives 60 Hz polling, plenty for cooperative-MT delivery
	 * cadence. The real wins from fixes234 (READ_CHUNK=8192,
	 * PUMP_STEPS=32) stay. */
	if (WaitNextEvent(MACOS9_EVENT_MASK, &ev, 1, NULL)) {
		/* macEntropy host seam: the whole EventRecord is jittery -- ev.where
		 * (mouse location), ev.when (event tick, i.e. key-press latency),
		 * ev.what, ev.message. Fold it into the pool every real event. */
		OSTLS_StirEntropy(&ev, sizeof ev);
		switch (ev.what) {
			case updateEvt:   macos9_handle_update(&ev); break;
			case mouseDown:   macos9_handle_mouse_down(&ev); break;
			case keyDown: case autoKey: macos9_handle_key_down(&ev); break;
			case activateEvt: macos9_handle_activate(&ev); break;
			default: break;
		}
	}
	if (!macos9_quitting) {
		{ macos9_schedule_run(); }
		/* fixes90: unconditional fetcher pump. NetSurf core's
		 * scheduler-driven fetcher_poll only re-arms while a fetch
		 * is active; after the first nav's HTTP fetch finishes,
		 * the chain goes dormant, stranding queued stub fetches
		 * and synchronous cache-hit broadcasts. Pumping here
		 * keeps queued jobs dispatching and per-scheme polls
		 * running every loop iteration. */
		{ macos9_op_depth++; fetch_pump(); macos9_op_depth--; }
		macos9_windows_te_idle(); macos9_windows_process_deferred();
		/* fixes725 — nav-button hover highlight: track the pointer over the
		 * front window's toolbar (cheap; only repaints on a hover change). */
		{ WindowRef hfw = FrontWindow();
		  struct gui_window *hfg = hfw ? macos9_find_window(hfw) : NULL;
		  if (hfg != NULL) macos9_window_update_hover(hfg); }
		/* fixes640 — emit the PERFACC phase summary ONCE, at the real
		 * load-complete edge (browser_window_stop_available true->false),
		 * so the post-first-paint reflow/settle passes are included (they
		 * are the whole point of measuring). first-paint would emit too
		 * early and miss the reflow storm. */
		{
			WindowRef pf_win = FrontWindow();
			struct gui_window *pf_gw = pf_win ?
				macos9_find_window(pf_win) : NULL;
			if (pf_gw != NULL && pf_gw->bw != NULL) {
				if (browser_window_stop_available(pf_gw->bw)) {
					pf_gw->perf_load_active = 1;
				} else if (pf_gw->perf_load_active &&
						!pf_gw->perf_summary_emitted) {
					struct nsurl *pf_u =
						browser_window_access_url(pf_gw->bw);
					pf_gw->perf_summary_emitted = 1;
					pf_gw->perf_load_active = 0;
					macsurf_profile_emit_phases(pf_u ?
						nsurl_access(pf_u) : "(unknown)");
				}
			}
		}
		macos9_poll_mouse_hover();
		{ macos9_animation_tick(); }
		/* fixes321 (#103) — drive setTimeout / setInterval. */
#ifdef WITH_QUICKJS
		{ macsurf_qjs_pump_all(); }
#endif
		/* Stage 1 (fixes565): the one quiescent drain point. schedule_run /
		 * fetch_pump / event dispatch have all returned, so nothing
		 * walk/convert/redraw is on the stack. macos9_deathrow_drain()
		 * itself no-ops unless macos9_op_depth==0, so a nested poll
		 * (should one ever exist) cannot free mid-operation. */
		{ macos9_deathrow_drain(); }
	}
}

/*
 * macos9_poll_mouse_hover -- poll mouse position once per event-loop
 * pass and dispatch HOVER to NetSurf if the cursor sits inside the
 * front window's content rect. Lets NetSurf's status-bar / link-
 * highlight code update without us having to install a Carbon
 * mouse-moved handler (which CarbonLib doesn't reliably support on
 * OS 9 anyway).
 */
void macos9_poll_mouse_hover(void) {
#ifdef __MACOS9__
	static Point last_pt = {-1, -1};
	static unsigned long last_dispatch_tick = 0;
	unsigned long now;
	WindowRef win;
	struct gui_window *gw;
	Point p;
	int x_ns, y_ns;
	if (macos9_quitting) return;
	win = FrontWindow();
	gw = win ? macos9_find_window(win) : NULL;
	if (!gw || !gw->bw) return;
	/* fixes430: skip hover dispatch while a new page is loading.
	 * browser_window_stop_available is true when loading_content exists.
	 * During that window the old box tree may be transitioning to the new
	 * one; mouse_track walking it risks a stale-box UAF.  Reset last_pt
	 * so hover fires fresh when the new content is fully ready. */
	if (browser_window_stop_available(gw->bw)) {
		last_pt.h = -1;
		last_pt.v = -1;
		return;
	}
	SetPortWindowPort(win);
	GetMouse(&p);
	if (p.h == last_pt.h && p.v == last_pt.v) return;
	/* fixes239 — debounce hover dispatch to ~10 Hz (6 ticks at 60 Hz).
	 * browser_window_mouse_track walks the box tree to find the node
	 * under the cursor on every call; firing per-pixel during mouse
	 * movement burns 1000-box walks at 60 Hz. Node-boundary crossings
	 * are the only events that trigger html_recascade_tree (the real
	 * expensive work), so 10 Hz is plenty for responsive :hover styling
	 * and status-bar link preview. */
	now = (unsigned long)TickCount();
	if (last_dispatch_tick != 0 && (now - last_dispatch_tick) < 6) return;
	last_dispatch_tick = now;
	last_pt = p;
	if (!PtInRect(p, &gw->content_rect)) return;
	x_ns = (int)p.h - gw->content_rect.left + gw->scroll_x;
	y_ns = (int)p.v - gw->content_rect.top  + gw->scroll_y;
	macos9_hittest_scroll_x = gw->scroll_x;
	macos9_hittest_scroll_y = gw->scroll_y;
	browser_window_mouse_track(gw->bw, BROWSER_MOUSE_HOVER, x_ns, y_ns);
#endif
}

/* fixes531: deferred initial navigation.
 *
 * Firing the home-page fetch synchronously from main() BEFORE the
 * WaitNextEvent loop starts meant the first live HTTPS connect ran with
 * the cooperative OT pump not yet cycling and the monotonic-clock
 * baseline not yet established.  The connect deadline was therefore
 * already expired on the very first pump, so the connection was abandoned
 * after a single pass and reported a bogus timeout.  Log signature on the
 * failing run: `pumps=1`, `ot_send calls=0 bytes=0`, `ot_err=2147483647`
 * (INT_MAX sentinel, i.e. no real OT error), with `os_err=0 br_err=0(OK)`.
 * The page then fell back to about:query/fetcherror.  Manual URL-bar
 * navigation always worked because by then the event loop was live.
 *
 * Fix: create the window empty (no url -> no synchronous fetch) and
 * schedule the navigation as a zero-delay callback, so it runs on the
 * first macos9_schedule_run pass with the loop running, the OT pump
 * cycling, and the clock baseline valid.  This routes the initial load
 * through the exact browser_window_navigate path manual navigation uses,
 * rather than special-casing the first fetch.  It fixes both candidate
 * causes (clock-baseline-not-ready and pump-not-cycling) at once by
 * removing the precondition that makes either bite.
 */
static void macos9_deferred_home_load(void *pw)
{
	struct browser_window *bw = (struct browser_window *)pw;
	nsurl *home = NULL;
	extern void macos9_http_mark_next_as_document(void);

	/* WATCH (callback-safety audit, 2026-06-29): this is the ONE live
	 * content/window-referencing scheduled callback with no out-of-band
	 * liveness guard and no bw-keyed teardown cancel.  It is SAFE today ONLY
	 * because it is scheduled with delay 0 at startup (see the macos9_schedule
	 * call in main) and therefore fires on the first scheduler pass, before
	 * any user event can destroy the root browser_window.  If you ever give it
	 * a non-zero delay, re-arm it after startup, or schedule it from anywhere
	 * a window teardown could intervene, it MUST first validate bw against the
	 * live window set (walk window_list / browser_window liveness) or be
	 * cancelled on browser_window destroy — otherwise it becomes a UAF on a
	 * freed browser_window. */

	/* One free log line to confirm cause 1 in passing: compare this
	 * first-tick clock value against the "launch home: clock_ms=" line
	 * logged at startup.  If the startup value is 0/garbage and this one
	 * is sane, the monotonic baseline wasn't valid pre-loop. */
	macsurf_debug_log_writef("deferred home: clock_ms=%ld (first tick)",
		(long)macsurf_monotonic_ms());

	if (bw == NULL) {
		MS_LOG("deferred home: bw NULL, skip");
		return;
	}
	if (nsurl_create(MACSURF_HOME_URL, &home) != NSERROR_OK) {
		MS_LOG("deferred home: nsurl_create failed");
		return;
	}
	macos9_http_mark_next_as_document();
	/* reset the profile clock at the real nav start, as the old
	 * synchronous path did (fixes366a). */
	macsurf_profile_reset();
	macsurf_profile_stamp("nav: launch home (deferred)");
	browser_window_navigate(bw, home, NULL, BW_NAVIGATE_HISTORY,
		NULL, NULL, NULL);
	nsurl_unref(home);
}


/* fixes983 -- claim the custom-icon bit on our own application file.
 *
 * MacSurf ships one artifact with two icons: the classic ICN#/icl8 family for
 * Mac OS 9 and an 'icns' for Mac OS X (fixes982). Shipping the 'icns' was not
 * enough and the reason is this flag. Mac OS X only consults a file's
 * custom-icon resource (-16455) when kHasCustomIcon is set in its Finder
 * flags; with the flag clear it falls back to the BNDL icon family, which is
 * the OS 9 artwork -- exactly what hardware showed ("it was reading the os9
 * icon fine ... it's the same old one right now"). Measured before changing
 * anything: the built app read flags=0x0100, and setting 0x0400 by hand on
 * that same binary produced the OS X icon immediately.
 *
 * Nothing in the toolchain sets the flag. CodeWarrior writes the type and
 * creator but not the Finder flags, and doing it by hand after every build is
 * the kind of step that is forgotten once and then debugged for an hour. So
 * the application asserts it about itself, which is the same spirit as the
 * 'vers' and 'plst' resources: identity the app carries rather than relies on
 * someone to attach.
 *
 * Write-once: returns immediately if the bit is already set, so this touches
 * the file on the first launch after an install and never again. Silent on
 * every failure -- a read-only volume or a locked file simply means the icon
 * stays as it was, which is cosmetic and must never be a launch failure.
 *
 * Safe on Mac OS 9 ONLY because fixes983 also emits the CLASSIC icon family
 * at -16455 alongside the 'icns'. With the flag set and no classic family
 * there, OS 9's Finder would look for a custom icon, find nothing it
 * understands, and draw a generic one -- trading the OS X icon for a
 * regression on the platform that was already right.
 */
#ifndef kHasCustomIcon
#define kHasCustomIcon 0x0400
#endif

static void macsurf_claim_custom_icon(void)
{
#ifdef __MACOS9__
	ProcessSerialNumber psn;
	ProcessInfoRec      info;
	FSSpec              appSpec;
	FInfo               fi;
	OSErr               err;

	psn.highLongOfPSN = 0;
	psn.lowLongOfPSN  = kCurrentProcess;
	memset(&info, 0, sizeof(info));
	info.processInfoLength = sizeof(ProcessInfoRec);
	info.processName = NULL;
	info.processAppSpec = &appSpec;
	if (GetProcessInformation(&psn, &info) != noErr) {
		return;
	}
	if (FSpGetFInfo(&appSpec, &fi) != noErr) {
		return;
	}
	if ((fi.fdFlags & kHasCustomIcon) != 0) {
		return;   /* already claimed -- nothing to write */
	}
	fi.fdFlags |= kHasCustomIcon;
	err = FSpSetFInfo(&appSpec, &fi);
	/* %d only: macsurf_debug_log_writef's hand-rolled formatter supports
	 * %d/%ld/%p/%s/%% and nothing else -- a %X here would print garbage. */
	macsurf_debug_log_writef(
		"LIFE icon custom-icon claimed err=%d flags=%d",
		(int)err, (int)fi.fdFlags);
#endif
}


int main(void) {
	/* fixes477: calibrate PPC time base register (mftb) first so
	 * macsurf_monotonic_ms() and performance.now() have a valid
	 * baseline from the first JS eval.  Two TickCount boundaries
	 * (~33 ms) elapse here at startup; acceptable cost. */
	macsurf_tb_calibrate();
	macsurf_debug_log_init();
	/* fixes936 (OS X tier 1): settle OS 9 vs Mac OS X BEFORE anything consumes
	 * the answer. macsurf_heap_bounds_init() below is the FIRST consumer -- on
	 * OS X the Process Manager partition window it reads is fiction and must
	 * not be allowed to narrow the pointer guards (that would re-create the
	 * #207 blank page on purpose). Emits the one-shot "RECON OS" line, so it
	 * has to run AFTER the log is open. This call site sits OUTSIDE the
	 * #ifdef __MACOS9__ block that opens further down; the Mac-only guard
	 * lives inside the module, so the Linux syntax check sees a no-op. */
	macsurf_osver_init();
	/* fixes719 (#207): capture the REAL application-partition pointer window
	 * from the Process Manager NOW -- after the log is up (so RECON HEAP
	 * BOUNDS is recorded) and before ANY string interning / URL parse /
	 * content fetch (netsurf_init and later). Every hardcoded
	 * 0x01000000/0x20000000/0x28000000 pointer-range guard tests against
	 * this window instead, so a partition mapped high (>0x28000000 on a
	 * higher-RAM Mac) no longer makes those guards reject valid heap
	 * pointers -> the blank-screen bug. Must stay before netsurf_init. */
	macsurf_heap_bounds_init();
	/* fixes366a -- start the profile clock so initial-page-load timing
	 * has a t0. macsurf_profile_stamp(label) anywhere downstream will
	 * produce a meaningful delta. The nav-time reset
	 * (macos9_window_navigate) refreshes t0 on each URL submit. */
	macsurf_profile_reset();
	MS_LOG("== MacSurf start ==");
	/* fixes711 (#207): earliest possible RECON snapshot -- VM on/off +
	 * heap/temp/purge -- flushed immediately so even an early blank leaves
	 * the baseline (and the VM state that labels this whole run) on disk. */
	macsurf_recon_mem("boot");
	macsurf_profile_stamp("main: log init done");
#ifdef __MACOS9__
	/* fixes680 (#207): DIAG memory/lifecycle trace. FreeMem = total free
	 * bytes in the app partition, MaxBlock = largest contiguous block. Logged
	 * at each startup milestone so a launch-time failure shows how far init
	 * got and whether the heap starved/fragmented. 'DIAG' survives the
	 * crash-only gate (fixes675). Remove for release. */
	macsurf_debug_log_writef("DIAG boot: free=%ld maxblk=%ld",
		(long)FreeMem(), (long)MaxBlock());
#endif
#ifdef __MACOS9__
#ifndef kInitOTForApplicationMask
#define kInitOTForApplicationMask 0x00000002
#endif
	InitCursor();
	if (InitOpenTransportInContext(kInitOTForApplicationMask, &macos9_ot_context) != noErr) {
		MS_LOG("InitOT FAIL");
		macos9_ot_context = NULL;
	} else {
		MS_LOG("InitOT OK");
	}
	g_ostls_ot_context = macos9_ot_context;
	/* fixes936 (OS X tier 1): the InitOT OK/FAIL MS_LOG above is not enough.
	 * "InitOT FAIL" survives the crash-only gate but "InitOT OK" does NOT, so
	 * a successful OT init is invisible and indistinguishable from a crash
	 * before this point. Open Transport IS present on Mac OS X 10.0-10.4
	 * (deprecated at 10.4, not removed), so this line is the first thing that
	 * tells us whether the networking stack even came up on the 10.3 test
	 * machine. NOTE: this prints on OS 9 too (it is not conditioned on
	 * osx=) -- that is expected on both platforms, not an anomaly. Shares the
	 * "RECON OT" whitelist entry with the fetcher emitters. */
	macsurf_debug_log_writef("RECON OT init ok=%d ctx=%p osx=%d",
		(macos9_ot_context != NULL) ? 1 : 0,
		(void *)macos9_ot_context,
		macsurf_os_is_osx());
	/* macEntropy: fold the persisted seed in before any handshake, so the
	 * first HTTPS fetch after a cold boot isn't drawing on a thin pool. */
	OSTLS_LoadSeed();
	MS_LOG("BOOT macEntropy: seed loaded");
	/* fixes413 -- prove on-device whether the macTLS SHA-384 core (fixes411)
	 * is live and correct. If this logs FAIL, every Sectigo/SHA-384 cert
	 * chain will be rejected; if it never logs at all, the macTLS library
	 * in this binary is not the one carrying fixes411. */
	{
		extern int OSTLS_SHA384_KAT(void);
		int kr = OSTLS_SHA384_KAT();
		if (kr == 0) {
			MS_LOG("BOOT macTLS SHA-384 KAT: PASS (single+multiblock)");
		} else if (kr == 1) {
			MS_LOG("macTLS SHA-384 KAT: FAIL abc (single-block)");
		} else if (kr == 2) {
			MS_LOG("macTLS SHA-384 KAT: FAIL 200a (MULTI-BLOCK)");
		} else {
			MS_LOG("macTLS SHA-384 KAT: FAIL 112a (two-block pad)");
		}
	}
	RegisterAppearanceClient();
	MS_LOG("BOOT Appearance OK");

	macsurf_claim_custom_icon();   /* fixes983 */

	/* fixes78: QuickTime startup. Required before any
	 * GraphicsImportComponent / Movies.h API call. Without this,
	 * GetGraphicsImporterForDataRef may still return a valid component
	 * for the format-identification phase but GraphicsImportDraw silently
	 * no-ops because the QT drawing subsystem isn't online. */
	{
		OSErr qt_err = EnterMovies();
		if (qt_err == noErr) {
			MS_LOG("BOOT EnterMovies OK");
		} else {
			MS_LOG("EnterMovies FAIL");
		}
	}

	/* fixes51 -- font quality upgrades, system-wide.
	 *
	 * SetOutlinePreferred(true) tells QuickDraw to render text from
	 *   TrueType outlines instead of scaling a bitmap when the
	 *   exact pt-size bitmap is missing.
	 *
	 * SetAntiAliasedTextEnabled(true, 8) turns on AA above 8 pt
	 *   on color displays. Mac OS 8.5+ feature; works through
	 *   CarbonLib. Below 8 pt the smoothing makes small UI text
	 *   blurry, hence the floor.
	 *
	 * fixes51a -- SetFractEnable removed. Fractional advances make
	 *   DrawText consume sub-pixel widths while TextWidth (used by
	 *   the layout-side font_width) still returns integer pixels.
	 *   The mismatch under-allocates horizontal space per line,
	 *   forcing NetSurf to wrap mid-line and produce overlapping
	 *   text-box positions. Integer-only advance widths match what
	 *   TextWidth reports. */
	{
		extern pascal void SetOutlinePreferred(Boolean);
		extern OSStatus SetAntiAliasedTextEnabled(Boolean, SInt16);
		SetOutlinePreferred(true);
		/* fixes68: AA floor raised from 8 to 12. AA at body sizes (8-10pt)
		 * produces sub-pixel fuzz because there aren't enough pixels per
		 * glyph for the antialiasing to look clean — net effect is blurry
		 * body text. Floor at 12 keeps body bitmap-crisp; larger sizes
		 * (headings, page titles) still get smooth AA edges. Dial up to
		 * 14 or 16 if body still looks fuzzy on the target hardware. */
		/* fixes952 — antialiasing is enabled on BOTH platforms again.
		 * fixes939 skipped this on OS X on my hypothesis that classic QD
		 * antialiasing was causing the TESetText crash. That was wrong: the
		 * real cause was TARGET_API_MAC_CARBON never being set (fixes951),
		 * and the AA skip made no difference to the crash. Keeping a
		 * platform split built on a disproven theory would just mean OS X
		 * renders text worse than OS 9 for no reason. */
		(void)SetAntiAliasedTextEnabled(true, 12);
		MS_LOG("BOOT font quality: outline on, AA floor=12pt, fract off");
	}

	macos9_init_menus();
	MS_LOG("BOOT menus installed");
	/* fixes294 — decode the baked-in default favicon PNG into a GWorld
	 * that lives for the life of the process.  Must happen AFTER
	 * EnterMovies (which initialises QT but we use lodepng for this) and
	 * AFTER menus install (purely conventional ordering, no real
	 * dependency).  Idempotent and best-effort: paint helper bails if
	 * load failed. */
	macos9_window_load_default_favicon();
	MS_LOG("BOOT default favicon loaded");
	/* fixes297 — toolbar button icons.  Best-effort; any failure
	 * leaves the corresponding button text-only. */
	macos9_window_load_toolbar_icons();
	MS_LOG("BOOT toolbar icons loaded");
#endif
	MS_LOG("BOOT mac-only init block done");
	memset(&macos9_table, 0, sizeof(macos9_table));
	macos9_table.window = macos9_window_table;
	macos9_table.utf8 = macos9_utf8_table;
	macos9_table.bitmap = macos9_bitmap_table;
	macos9_table.layout = macos9_layout_table;
	macos9_table.misc = &macos9_misc_table;
	macos9_table.download = macos9_download_table;
	macos9_table.clipboard = macos9_clipboard_table;
	{
		extern struct gui_llcache_table *null_llcache_table;
		extern struct gui_fetch_table macos9_fetch_table;
		macos9_table.llcache = null_llcache_table;
		macos9_table.fetch = &macos9_fetch_table;
	}
	netsurf_register(&macos9_table);
	MS_LOG("BOOT netsurf_register done");
	nsoption_init(NULL, NULL, NULL);
	MS_LOG("BOOT nsoption_init done");
	/* fixes78: image content handler (QuickTime Graphics Importers) is
	 * now registered in macos9_image.c. Enable image fetches so <img>
	 * elements actually trigger network fetches and decode through the
	 * QT importer pipeline. */
	nsoption_set_bool(foreground_images, true);
	nsoption_set_bool(background_images, true);
	/* Enable author CSS so inline <style>/<link> rules apply. */
	nsoption_set_bool(author_level_css, true);
	/* fixes319 (#115-#121) — turn on inline <script> execution. Defaults
	 * to false in NetSurf core; without this, the JS bridge that
	 * fixes316 wired up is dead-code from NetSurf's perspective because
	 * html_script_exec returns early without ever calling js_exec. */
	nsoption_set_bool(enable_javascript, true);
	/* fixes1115b (#265) -- enable <select> dropdown menus. The core
	 * form-control <select> handler (textarea.c / forms.c form_select_*
	 * callbacks) is gated on this option; without it, <select> elements
	 * render as empty rectangles and dropdowns never open. The Amiga and
	 * framebuffer frontends set this to true (gui.c:1056, gui.c:2238
	 * respectively); the macos9 frontend never did. One line. */
	nsoption_set_bool(core_select_menu, true);
	/* fixes91: raise concurrent-fetch caps. NetSurf defaults are
	 * max_fetchers=24 / max_fetchers_per_host=5. With our HTTP fetcher's
	 * MFS_INIT-at-setup state-machine, slots stay non-IDLE past the
	 * point NetSurf's fetch_ring drains, so by the third user navigation
	 * fetch_ring saturates and `fetch_dispatch_jobs` refuses to call
	 * ops.start on new fetches. The HTTP fetcher fires anyway (poll
	 * doesn't gate on start) but the stub fetcher needs ctx->started
	 * and so hangs. Raise the caps so the gate never bites; the proper
	 * fix (start-gated mfs_open) lives in macos9_http_fetcher.c. */
	nsoption_set_int(max_fetchers, 128);
	/* fixes232 — drop per-host cap from 16 to 4 so the HTTPS keep-alive
	 * pool (fixes231) actually catches reuses. Previously 16 parallel
	 * fetches per host meant every cold-page-load issued 16+ cold
	 * handshakes before any could finish and seed the pool; subsequent
	 * fetches in the same load missed the pool because everything was
	 * already in flight. With 4 parallel max, only the first 4 are cold;
	 * fetches 5-30 dequeue as 1-4 complete and pull warm connections
	 * out of the pool. Net win: ~25 saved TLS handshakes per cold page
	 * load (~18s of BearSSL ECDHE on a 233 MHz G3). */
	nsoption_set_int(max_fetchers_per_host, 4);
	/* fixes106 — capped memory_cache_size at 2 MB on a 16 MB partition
	 * to keep the live-page working set out of cache contention.
	 *
	 * fixes160d — partition is now 194 MB by CW8 setting. Bump cache
	 * to 32 MB. Modern retro setups (SSDs, Ethernet, real bandwidth)
	 * benefit hugely from a cache that holds the current page's full
	 * sub-resource set plus several recent pages' worth, so back-button
	 * is instant and intra-site navigation skips re-fetch of shared
	 * CSS / images. Apple alone burns ~2 MB on stylesheets per page —
	 * at 32 MB the cache holds Apple + 5-6 typical pages of history
	 * with room left over for libcss/libdom working set. */
	/* fixes430: 32MB -> 4MB (a 22MB forum index + 32MB stale llcache on top
	 * of the live working set exhausted the heap).
	 * fixes460-463: dropped to 0 while chasing an llcache-reentrancy crash and
	 * the blank-page bug.
	 * fixes731: RESTORE to 8MB. The blank page was root-caused to the
	 * pointer-ceiling guards (#207 / fixes719), NOT the cache, and the
	 * llcache reentrancy guards (fixes459/460, op_depth) are still in place —
	 * so the reason it was zeroed no longer applies. With no cache, every
	 * navigation (and every back-button) re-fetched AND re-did the ~1s TLS
	 * handshake for shared CSS/JS/images; an 8MB cache lets intra-site nav and
	 * back-button skip both. Kept conservative (not 32MB) to avoid the fixes430
	 * heap-exhaustion on heavy forum indexes and to stay safe on 64MB Macs. */
	nsoption_set_int(memory_cache_size, 32 * 1024 * 1024);
	MS_LOG("BOOT images enabled, author_css on, fetcher 128/16, mem cache 32MB");
#ifdef __MACOS9__
	macsurf_debug_log_writef("DIAG pre-netsurf_init: free=%ld maxblk=%ld",
		(long)FreeMem(), (long)MaxBlock());
#endif
	netsurf_init(NULL);
	MS_LOG("BOOT netsurf_init done");
	/* fixes721b — MacSurf loads no Messages file, so core message tokens
	 * render as the raw token (e.g. an <input type=file> showed a white box
	 * reading "Form_Drop"). Register the handful of form-gadget labels core
	 * actually paints/statuses so they read as English. */
	{
		extern nserror messages_add_key_value(const char *k, const char *v);
		messages_add_key_value("Form_Drop", "Choose File\311");
		messages_add_key_value("FormFile", "File upload");
		messages_add_key_value("FormSubmit", "Submit form");
		messages_add_key_value("FormReset", "Reset form");
		messages_add_key_value("FormButton", "Button");
		messages_add_key_value("FormTextarea", "Text area");
		messages_add_key_value("FormSelect", "Select");
		messages_add_key_value("FormCheckbox", "Checkbox");
		messages_add_key_value("FormRadio", "Radio button");
		messages_add_key_value("FormTextbox", "Text field");
	}
#ifdef __MACOS9__
	macsurf_debug_log_writef("DIAG post-netsurf_init: free=%ld maxblk=%ld",
		(long)FreeMem(), (long)MaxBlock());
#endif
	/* fixes711 (#207): snapshot the heap just after core init, before the
	 * first page. Compared against RECON MEM boot this shows how much
	 * contiguity netsurf_init consumed -- the pool libcss must draw from. */
	macsurf_recon_mem("post-init");
	/* fixes368 (#167) — restore a prior session's cookie jar (Facebook
	 * login etc.) from disk now that urldb is up. Best-effort no-op on
	 * first run. */
	macos9_cookies_load();
	MS_LOG("BOOT cookies loaded");
#ifdef WITH_QUICKJS
	js_initialise();
	MS_LOG("BOOT js_initialise done");
#endif
	{
		extern nserror macos9_http_fetcher_register(void);
		macos9_http_fetcher_register();
		MS_LOG("BOOT http_fetcher registered");
	}
	{
		struct browser_window *bw = NULL;
		if (macos9_ot_context != NULL) {
			/* fixes531: create the window EMPTY (NULL url -> no
			 * synchronous fetch), then defer the home navigation to
			 * the first event-loop tick via the scheduler.  Running
			 * the first live fetch before the WaitNextEvent loop was
			 * cycling produced an instant bogus-timeout (pumps=1,
			 * ot_err=INT_MAX) -> about:query/fetcherror.  See
			 * macos9_deferred_home_load above. */
			extern nserror macos9_schedule(int t,
				void (*callback)(void *p), void *p);
			MS_LOG("BOOT launch: create empty window, defer home nav");
			browser_window_create(BW_CREATE_HISTORY | BW_CREATE_FOREGROUND,
				NULL, NULL, NULL, &bw);
			macsurf_debug_log_writef(
				"launch home: clock_ms=%ld (startup, pre-loop)",
				(long)macsurf_monotonic_ms());
			if (bw != NULL) {
				macos9_schedule(0, macos9_deferred_home_load, bw);
				MS_LOG("BOOT launch: home nav scheduled (deferred)");
			}
		}
		if (bw == NULL) {
			MS_LOG("BOOT launch: fallback create_initial_window");
			macos9_create_initial_window();
		}
	}
	MS_LOG("BOOT initial window created");
#ifdef __MACOS9__
	macsurf_debug_log_writef("DIAG post-window: free=%ld maxblk=%ld",
		(long)FreeMem(), (long)MaxBlock());
#endif
	/* fixes247 — font probes (fixes144a / fixes153) gated behind a
	 * startup flag, default off. They were extremely useful when
	 * diagnosing fixes144b sub-AA glyph spacing and fixes153 gui_layout
	 * vmetric work, but now run on every launch writing ~420 lines of
	 * diagnostic data (~84 ms of disk I/O) before the first fetch.
	 * Flip MACSURF_FONT_PROBE_ON_STARTUP to 1 (or pass via preprocessor)
	 * to re-enable when investigating font issues. */
#ifndef MACSURF_FONT_PROBE_ON_STARTUP
#define MACSURF_FONT_PROBE_ON_STARTUP 0
#endif
#if MACSURF_FONT_PROBE_ON_STARTUP
	macos9_font_metric_probe_run();
	macos9_font_vmetric_probe_run();
#endif
	while (!macos9_done) macos9_poll();
	MS_LOG("BOOT event loop exited");
	/* fixes368 (#167) — persist the cookie jar BEFORE netsurf_exit tears
	 * urldb down, so this session's Facebook login survives the relaunch. */
	macos9_cookies_save();
	MS_LOG("BOOT cookies saved");
	macos9_quitting = (bool)1;
#ifdef WITH_QUICKJS
	/* fixes717 (#207 log) — finalise the JS heap BEFORE netsurf_exit.
	 * netsurf_exit releases the hlcache handles; running js_finalise AFTER
	 * it made QuickJS teardown call hlcache_get_url on already-freed
	 * handles (the "wild handle" spam at shutdown). Destroy the consumer
	 * (JS) before the producer (browser core). */
	js_finalise();
#endif
	netsurf_exit();
	/* macEntropy: persist this session's accumulated entropy so the next
	 * cold boot starts warm. Must run before OT teardown. */
	OSTLS_SaveSeed();
	MS_LOG("BOOT macEntropy: seed saved");
	if (macos9_ot_context) CloseOpenTransportInContext(macos9_ot_context);
	return 0;
}
