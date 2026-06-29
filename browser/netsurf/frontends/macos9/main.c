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
#include "macsurf_timebase.h"

#ifdef __MACOS9__
#include <OpenTransport.h>
#include <OpenTptInternet.h>
#include <Movies.h>
OTClientContextPtr macos9_ot_context = NULL;
/* macTLS expects this symbol; aliased to our OT context after init. */
OTClientContextPtr g_ostls_ot_context = NULL;
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
extern const BitMap *GetPortBitMapForCopyBits(CGrafPtr port);
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
	/* fixes303 — razor-sharp 1px inset border. Top + left are technical
	 * charcoal #444444, bottom + right are crisp white #FFFFFF. White
	 * canvas inside. Text in Geneva 12 with the existing favicon at the
	 * left and the TE rect inset well past it. */
	RGBColor black     = {0, 0, 0};
	RGBColor white     = {0xFFFF, 0xFFFF, 0xFFFF};
	RGBColor charcoal  = {0x4444, 0x4444, 0x4444};   /* #444444 */
	Rect u = gw->url_rect;
	Rect r;
	short L = u.left, T = u.top, R = u.right, B = u.bottom;

	/* white input canvas inside the 1px frame */
	RGBForeColor(&white);
	SetRect(&r, (short)(L + 1), (short)(T + 1), (short)(R - 1), (short)(B - 1));
	PaintRect(&r);

	/* top + left = #444 */
	RGBForeColor(&charcoal);
	SetRect(&r, L, T, R, (short)(T + 1)); PaintRect(&r);
	SetRect(&r, L, T, (short)(L + 1), B); PaintRect(&r);
	/* bottom + right = #FFFFFF */
	RGBForeColor(&white);
	SetRect(&r, L, (short)(B - 1), R, B); PaintRect(&r);
	SetRect(&r, (short)(R - 1), T, R, B); PaintRect(&r);

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
	RGBColor black = {0,0,0}, white = {0xFFFF, 0xFFFF, 0xFFFF};
	Rect r = gw->status_rect;
	RGBForeColor(&black); RGBBackColor(&white);
	EraseRect(&r); FrameRect(&r);
	MoveTo((short)(r.left+4), (short)(r.bottom-4));
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
	AppendResMenu(apple_menu, 'DRVR');
	InsertMenu(apple_menu, 0);

	file_menu = NewMenu(MENU_FILE, "\pFile");
	AppendMenu(file_menu, "\pNew Window/N");
	AppendMenu(file_menu, "\pOpen Location.../L");
	AppendMenu(file_menu, "\pClose/W");
	AppendMenu(file_menu, "\p(-");
	AppendMenu(file_menu, "\pQuit/Q");
	InsertMenu(file_menu, 0);

	edit_menu = NewMenu(MENU_EDIT, "\pEdit");
	AppendMenu(edit_menu, "\pUndo/Z");
	AppendMenu(edit_menu, "\p(-");
	AppendMenu(edit_menu, "\pCut/X");
	AppendMenu(edit_menu, "\pCopy/C");
	AppendMenu(edit_menu, "\pPaste/V");
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
		InsertMenu(view_menu, 0);
	}

	/* fixes351 (#48) — Bookmarks menu. Items dispatch to the existing
	 * macos9_bookmark_add / macos9_bookmark_list_show in
	 * macos9_chrome_extras.c (which already store in a session-scope
	 * array; disk persistence deferred). */
	{
		MenuHandle bookmark_menu = NewMenu(MENU_BOOKMARK, "\pBookmarks");
		AppendMenu(bookmark_menu, "\pAdd Bookmark/D");
		AppendMenu(bookmark_menu, "\pShow Bookmarks/B");
		InsertMenu(bookmark_menu, 0);
	}

	DrawMenuBar();
#endif
}

static void macos9_handle_menu(short menu_id, short item) {
#ifdef __MACOS9__
	WindowRef front;
	struct gui_window *gw = NULL;	/* fixes369a — init to silence CW8
					 * "used before initialized" (assigned
					 * per-case below before use). */
	macsurf_debug_log_writef(
		"fixes352c handle_menu: menu_id=%d item=%d",
		(int)menu_id, (int)item);
	switch (menu_id) {
	case MENU_APPLE:
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
		default: break;
		}
		break;
	case MENU_BOOKMARK:
		/* fixes351 (#48) — Bookmarks menu dispatcher. The add/show
		 * functions live in macos9_chrome_extras.c and were landed in
		 * fixes331 with no menu wiring at the time; this hooks them up. */
		switch (item) {
		case ITEM_BMK_ADD: {
			extern void macos9_bookmark_add(struct gui_window *g);
			macos9_bookmark_add(gw);
		} break;
		case ITEM_BMK_SHOW: {
			extern void macos9_bookmark_list_show(struct gui_window *g);
			macos9_bookmark_list_show(gw);
		} break;
		default: break;
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

static void macos9_handle_update(const EventRecord *event) {
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
		EraseRect(&update_bounds);
	} else {
		/* Fallback path: paint directly into window. Flash returns. */
		Boolean fb_top_dirty = (Boolean)(update_bounds.top < gw->content_rect.top);
		Boolean fb_bot_dirty = (Boolean)(update_bounds.bottom > gw->content_rect.bottom);
		EraseRect(&gw->content_rect);
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
		browser_window_redraw(gw->bw,
			gw->content_rect.left - gw->scroll_x,
			gw->content_rect.top  - gw->scroll_y,
			&clip, &ctx);
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
			blk.red = 0; blk.green = 0; blk.blue = 0;
			wht.red = 0xFFFF; wht.green = 0xFFFF; wht.blue = 0xFFFF;
			SetGWorld(saved_port, saved_gdh);
			src_bm = GetPortBitMapForCopyBits(
				(CGrafPtr)gw->content_gworld);
			dst_bm = GetPortBitMapForCopyBits(GetWindowPort(win));
			RGBForeColor(&blk); RGBBackColor(&wht);
			/* Dst port visRgn is BeginUpdate's update region; QD
			 * hardware-clips the blit so only dirty pixels reach
			 * the screen even though src/dst rects span the full
			 * content area. */
			CopyBits(src_bm, dst_bm, &off_bounds, &off_bounds,
			         srcCopy, NULL);
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
	EndUpdate(win);
#endif
}

void macos9_handle_mouse_down(const EventRecord *event) {
#ifdef __MACOS9__
	WindowRef win;
	short part = FindWindow(event->where, &win);
	struct gui_window *gw;
	switch (part) {
		case inMenuBar: {
			long sel = MenuSelect(event->where);
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
			if (win && TrackGoAway(win, event->where)) {
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
						} else if (ctrl == NULL && PtInRect(p, &gw->url_rect)) {
							macos9_window_te_activate_url(gw);
							if (gw->url_te) TEClick(p, (event->modifiers & shiftKey) != 0, gw->url_te);
						} else if (ctrl == NULL && PtInRect(p, &gw->content_rect) && gw->bw) {
							/* Click in content area — dispatch to NetSurf so
							 * links navigate, forms submit, etc.  Coordinates
							 * are translated from window-local to NetSurf
							 * content space (= local - content origin + scroll). */
							int x_ns, y_ns, rx_ns, ry_ns;
							browser_mouse_state mods = 0;
							Point relp;
							macos9_window_te_deactivate_url(gw);
							if (event->modifiers & shiftKey)
								mods |= BROWSER_MOUSE_MOD_1;
							if (event->modifiers & controlKey)
								mods |= BROWSER_MOUSE_MOD_2;
							if (event->modifiers & optionKey)
								mods |= BROWSER_MOUSE_MOD_3;
							x_ns = (int)p.h - gw->content_rect.left + gw->scroll_x;
							y_ns = (int)p.v - gw->content_rect.top  + gw->scroll_y;
							MS_LOG("content: PRESS_1");
							browser_window_mouse_click(gw->bw,
								BROWSER_MOUSE_PRESS_1 | mods,
								x_ns, y_ns);
							/* Wait for the mouse-up that ends this click.
							 * StillDown blocks until the user releases the
							 * button; safe under cooperative MT because
							 * Toolbox yields ticks while polling. */
							while (StillDown()) { /* spin */ }
							GetMouse(&relp);
							rx_ns = (int)relp.h - gw->content_rect.left + gw->scroll_x;
							ry_ns = (int)relp.v - gw->content_rect.top  + gw->scroll_y;
							MS_LOG("content: CLICK_1");
							browser_window_mouse_click(gw->bw,
								BROWSER_MOUSE_CLICK_1 | mods,
								rx_ns, ry_ns);
							/* inline onclick handlers run natively in the JS engine */
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

void macos9_handle_key_down(const EventRecord *event) {
#ifdef __MACOS9__
	WindowRef win = FrontWindow();
	struct gui_window *gw = win ? macos9_find_window(win) : NULL;
	char ch = (char)(event->message & charCodeMask);
	if (event->modifiers & cmdKey) {
		long sel = MenuKey(ch);
		if (sel != 0) {
			macos9_handle_menu((short)((sel >> 16) & 0xFFFF),
				(short)(sel & 0xFFFF));
			return;
		}
	}
	if (!gw) return;
	if (gw->url_field_active && gw->url_te) {
		if (ch == 0x0D || ch == 0x03) {
			macos9_window_address_bar_submit(gw);
		} else if (ch == 0x1B) {
			macos9_window_te_deactivate_url(gw);
		} else {
			SetPortWindowPort(gw->window);
			TEKey(ch, gw->url_te);
			InvalWindowRect(gw->window, &gw->url_rect);
		}
	} else {
		unsigned char uc;
		int is_printable;
		uc = (unsigned char)ch;
		is_printable = (uc >= 0x20 && uc < 0x7F) || uc == 0x08 || uc == 0x7F || uc == 0x0D;
		if (is_printable && gw->bw) {
			extern bool browser_window_key_press(struct browser_window *, unsigned long);
			if (browser_window_key_press(gw->bw, (unsigned long)uc)) {
				macsurf_debug_log_writef("page key: 0x%02x consumed", (int)uc);
				/* fixes101 — do NOT invalidate the whole content area here.
				 * NetSurf calls gw_invalidate during browser_window_key_press
				 * with the exact dirty rects (textarea field bbox + caret).
				 * fixes100 made gw_invalidate honour those rects; this
				 * trailing whole-content InvalRect was overriding them
				 * with a 585x351 region, forcing ~170 DrawText calls per
				 * keystroke on the duck-duck search results page. If
				 * NetSurf returned true but didn't invalidate, nothing
				 * visually changed and there is nothing to repaint. */
				return;
			}
		}
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
		{ extern bool macos9_schedule_run(void); macos9_schedule_run(); }
		/* fixes90: unconditional fetcher pump. NetSurf core's
		 * scheduler-driven fetcher_poll only re-arms while a fetch
		 * is active; after the first nav's HTTP fetch finishes,
		 * the chain goes dormant, stranding queued stub fetches
		 * and synchronous cache-hit broadcasts. Pumping here
		 * keeps queued jobs dispatching and per-scheme polls
		 * running every loop iteration. */
		{ extern void fetch_pump(void); fetch_pump(); }
		macos9_windows_te_idle(); macos9_windows_process_deferred();
		macos9_poll_mouse_hover();
		{ extern void macos9_animation_tick(void);
		  macos9_animation_tick(); }
		/* fixes321 (#103) — drive setTimeout / setInterval. */
#ifdef WITH_QUICKJS
		{ extern void macsurf_qjs_pump_all(void);
		  macsurf_qjs_pump_all(); }
#endif
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

int main(void) {
	/* fixes477: calibrate PPC time base register (mftb) first so
	 * macsurf_monotonic_ms() and performance.now() have a valid
	 * baseline from the first JS eval.  Two TickCount boundaries
	 * (~33 ms) elapse here at startup; acceptable cost. */
	macsurf_tb_calibrate();
	macsurf_debug_log_init();
	/* fixes366a -- start the profile clock so initial-page-load timing
	 * has a t0. macsurf_profile_stamp(label) anywhere downstream will
	 * produce a meaningful delta. The nav-time reset
	 * (macos9_window_navigate) refreshes t0 on each URL submit. */
	macsurf_profile_reset();
	MS_LOG("== MacSurf start ==");
	macsurf_profile_stamp("main: log init done");
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
	/* macEntropy: fold the persisted seed in before any handshake, so the
	 * first HTTPS fetch after a cold boot isn't drawing on a thin pool. */
	OSTLS_LoadSeed();
	MS_LOG("macEntropy: seed loaded");
	/* fixes413 -- prove on-device whether the macTLS SHA-384 core (fixes411)
	 * is live and correct. If this logs FAIL, every Sectigo/SHA-384 cert
	 * chain will be rejected; if it never logs at all, the macTLS library
	 * in this binary is not the one carrying fixes411. */
	{
		extern int OSTLS_SHA384_KAT(void);
		int kr = OSTLS_SHA384_KAT();
		if (kr == 0) {
			MS_LOG("macTLS SHA-384 KAT: PASS (single+multiblock)");
		} else if (kr == 1) {
			MS_LOG("macTLS SHA-384 KAT: FAIL abc (single-block)");
		} else if (kr == 2) {
			MS_LOG("macTLS SHA-384 KAT: FAIL 200a (MULTI-BLOCK)");
		} else {
			MS_LOG("macTLS SHA-384 KAT: FAIL 112a (two-block pad)");
		}
	}
	RegisterAppearanceClient();
	MS_LOG("Appearance OK");

	/* fixes78: QuickTime startup. Required before any
	 * GraphicsImportComponent / Movies.h API call. Without this,
	 * GetGraphicsImporterForDataRef may still return a valid component
	 * for the format-identification phase but GraphicsImportDraw silently
	 * no-ops because the QT drawing subsystem isn't online. */
	{
		OSErr qt_err = EnterMovies();
		if (qt_err == noErr) {
			MS_LOG("EnterMovies OK");
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
		(void)SetAntiAliasedTextEnabled(true, 12);
		MS_LOG("font quality: outline on, AA floor=12pt, fract off");
	}

	macos9_init_menus();
	MS_LOG("menus installed");
	/* fixes294 — decode the baked-in default favicon PNG into a GWorld
	 * that lives for the life of the process.  Must happen AFTER
	 * EnterMovies (which initialises QT but we use lodepng for this) and
	 * AFTER menus install (purely conventional ordering, no real
	 * dependency).  Idempotent and best-effort: paint helper bails if
	 * load failed. */
	macos9_window_load_default_favicon();
	MS_LOG("default favicon loaded");
	/* fixes297 — toolbar button icons.  Best-effort; any failure
	 * leaves the corresponding button text-only. */
	macos9_window_load_toolbar_icons();
#endif
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
	MS_LOG("netsurf_register done");
	nsoption_init(NULL, NULL, NULL);
	MS_LOG("nsoption_init done");
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
	/* fixes430: drop from 32MB to 4MB.  The forum index is 22MB of
	 * subresource bytes; holding 32MB of stale llcache from the previous
	 * page on top of the new page's working set exhausted the heap.
	 * 4MB is still enough for shared CSS/images on intra-site navigation
	 * (XenForo bundles are ~252KB) while giving the new page's DOM+box
	 * tree and libcss cascade enough room. */
	nsoption_set_int(memory_cache_size, 0);
	MS_LOG("images enabled, author_css on, fetcher 128/16, mem cache 0");
	netsurf_init(NULL);
	MS_LOG("netsurf_init done");
	/* fixes368 (#167) — restore a prior session's cookie jar (Facebook
	 * login etc.) from disk now that urldb is up. Best-effort no-op on
	 * first run. */
	macos9_cookies_load();
	MS_LOG("cookies loaded");
#ifdef WITH_QUICKJS
	js_initialise();
	MS_LOG("js_initialise done");
#endif
	{
		extern nserror macos9_http_fetcher_register(void);
		macos9_http_fetcher_register();
		MS_LOG("http_fetcher registered");
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
			MS_LOG("launch: create empty window, defer home nav");
			browser_window_create(BW_CREATE_HISTORY | BW_CREATE_FOREGROUND,
				NULL, NULL, NULL, &bw);
			macsurf_debug_log_writef(
				"launch home: clock_ms=%ld (startup, pre-loop)",
				(long)macsurf_monotonic_ms());
			if (bw != NULL) {
				macos9_schedule(0, macos9_deferred_home_load, bw);
				MS_LOG("launch: home nav scheduled (deferred)");
			}
		}
		if (bw == NULL) {
			MS_LOG("launch: fallback create_initial_window");
			macos9_create_initial_window();
		}
	}
	MS_LOG("initial window created");
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
	MS_LOG("event loop exited");
	/* fixes368 (#167) — persist the cookie jar BEFORE netsurf_exit tears
	 * urldb down, so this session's Facebook login survives the relaunch. */
	macos9_cookies_save();
	MS_LOG("cookies saved");
	macos9_quitting = (bool)1; netsurf_exit();
#ifdef WITH_QUICKJS
	js_finalise();
#endif
	/* macEntropy: persist this session's accumulated entropy so the next
	 * cold boot starts warm. Must run before OT teardown. */
	OSTLS_SaveSeed();
	MS_LOG("macEntropy: seed saved");
	if (macos9_ot_context) CloseOpenTransportInContext(macos9_ot_context);
	return 0;
}
