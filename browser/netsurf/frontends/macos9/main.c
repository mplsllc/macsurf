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

#ifdef __MACOS9__
#include <OpenTransport.h>
#include <OpenTptInternet.h>
OTClientContextPtr macos9_ot_context = NULL;
#ifdef WITH_DUKTAPE
#include "javascript/macsurf_js.h"
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
 * it attempts to dispatch kEventMouseWheelMoved (CarbonLib: not available). */
#define MACOS9_EVENT_MASK (mDownMask | mUpMask | keyDownMask | autoKeyMask | \
	updateMask | activMask | osMask | highLevelEventMask)
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

static void draw_url_bar(struct gui_window *gw) {
#ifdef __MACOS9__
	RGBColor black = {0,0,0}, white = {0xFFFF, 0xFFFF, 0xFFFF};
	RGBForeColor(&black); RGBBackColor(&white);
	TextFont(kFontIDMonaco); TextSize(10); TextFace(0);
	EraseRect(&gw->url_rect); FrameRect(&gw->url_rect);
	if (gw->url_te != NULL) TEUpdate(&gw->url_rect, gw->url_te);
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

	DrawMenuBar();
#endif
}

static void macos9_handle_menu(short menu_id, short item) {
#ifdef __MACOS9__
	WindowRef front;
	struct gui_window *gw;
	switch (menu_id) {
	case MENU_APPLE:
		break;
	case MENU_FILE:
		switch (item) {
		case ITEM_FILE_NEW: {
			struct browser_window *bw = NULL;
			nsurl *home = NULL;
			if (nsurl_create(MACSURF_HOME_URL, &home) == NSERROR_OK) {
				browser_window_create(BW_CREATE_HISTORY | BW_CREATE_FOREGROUND,
					home, NULL, NULL, &bw);
				nsurl_unref(home);
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
	if (!gw || macos9_quitting) return;
	SetPortWindowPort(win); BeginUpdate(win);
	/* fixes77e -- revert fixes77c's offscreen-GWorld back buffer. The
	 * GWorld-+-SetClip+CopyBits path interacted badly with plotters.c's
	 * plot_clip bookkeeping (effective clip resolved to (0,0,0,0) on
	 * real hardware, so PaintRect/DrawText calls were issued but never
	 * landed pixels on screen -- the page "rendered" but stayed blank).
	 * Restore direct-paint to the window port; flash returns but the
	 * page displays. content_gworld stays NULL and the dispose stub in
	 * window_destroy is a no-op. */
	EraseRect(&gw->content_rect);
	draw_url_bar(gw); DrawControls(win); draw_status_bar(gw);
	if (gw->bw && browser_window_redraw_ready(gw->bw)) {
		struct rect clip; struct redraw_context ctx;
		extern long macos9_plot_text_count, macos9_plot_rect_count;
		extern long macos9_hrb_visits, macos9_hrb_block, macos9_hrb_inlinec,
			    macos9_hrb_inline, macos9_hrb_text, macos9_hrb_other,
			    macos9_hrb_clip_skips;
		extern long macos9_grad_set_count,
			    macos9_grad_radial_unpack_count,
			    macos9_grad_linear_unpack_count;
		macos9_plot_text_count = 0;
		macos9_plot_rect_count = 0;
		macos9_hrb_visits = 0;
		macos9_hrb_block = 0;
		macos9_hrb_inlinec = 0;
		macos9_hrb_inline = 0;
		macos9_hrb_text = 0;
		macos9_hrb_other = 0;
		macos9_hrb_clip_skips = 0;
		macos9_grad_set_count = 0;
		macos9_grad_radial_unpack_count = 0;
		macos9_grad_linear_unpack_count = 0;
		macsurf_debug_log_writef(
			"update: redraw_ready, bw=%p scroll=(%d,%d) crect=(%d,%d,%d,%d)",
			gw->bw, gw->scroll_x, gw->scroll_y,
			(int)gw->content_rect.left, (int)gw->content_rect.top,
			(int)gw->content_rect.right, (int)gw->content_rect.bottom);
		clip.x0 = gw->content_rect.left; clip.y0 = gw->content_rect.top;
		clip.x1 = gw->content_rect.right; clip.y1 = gw->content_rect.bottom;
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
		macsurf_debug_log_writef(
			"update: bw_redraw done visits=%ld block=%ld inlinec=%ld inline=%ld text=%ld other=%ld skips=%ld plot_text=%ld plot_rect=%ld",
			macos9_hrb_visits, macos9_hrb_block, macos9_hrb_inlinec,
			macos9_hrb_inline, macos9_hrb_text, macos9_hrb_other,
			macos9_hrb_clip_skips,
			macos9_plot_text_count, macos9_plot_rect_count);
		macsurf_debug_log_writef(
			"GRADIENT DIAG: set=%ld radial_unpacks=%ld linear_unpacks=%ld",
			macos9_grad_set_count,
			macos9_grad_radial_unpack_count,
			macos9_grad_linear_unpack_count);
		/* Redraw chrome AFTER content so any stray plotter that
		 * leaked outside content_rect can't leave the URL bar /
		 * controls / status bar visually torn. Addresses the
		 * 2026-04-18 survey hypothesis about URL field visual
		 * blanking on the initial window. */
		draw_url_bar(gw);
		DrawControls(win);
		draw_status_bar(gw);
		if (gw->url_field_active && gw->url_te) TEActivate(gw->url_te);
	} else if (gw->bw) {
		MS_LOG("update: bw not ready, skip");
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
						cpart = FindControl(p, win, &ctrl);
						if (cpart != 0 && ctrl != NULL) {
							if (ctrl == gw->vscroll || ctrl == gw->hscroll) {
								macos9_window_handle_scrollbar_click(gw, ctrl, cpart, &p);
							} else if (ctrl == gw->back_btn && TrackControl(ctrl, p, NULL)) {
								macos9_window_back(gw);
							} else if (ctrl == gw->forward_btn && TrackControl(ctrl, p, NULL)) {
								macos9_window_forward(gw);
							} else if (ctrl == gw->reload_btn && TrackControl(ctrl, p, NULL)) {
								macos9_window_reload(gw);
							} else if (ctrl == gw->home_btn && TrackControl(ctrl, p, NULL)) {
								macos9_window_home(gw);
							}
						} else if (PtInRect(p, &gw->url_rect)) {
							macos9_window_te_activate_url(gw);
							if (gw->url_te) TEClick(p, (event->modifiers & shiftKey) != 0, gw->url_te);
						} else if (PtInRect(p, &gw->content_rect) && gw->bw) {
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
	if (WaitNextEvent(MACOS9_EVENT_MASK, &ev, 1, NULL)) {
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
		macos9_windows_te_idle(); macos9_windows_process_deferred();
		macos9_poll_mouse_hover();
		{ extern void macos9_animation_tick(void);
		  macos9_animation_tick(); }
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
	WindowRef win;
	struct gui_window *gw;
	Point p;
	int x_ns, y_ns;
	if (macos9_quitting) return;
	win = FrontWindow();
	gw = win ? macos9_find_window(win) : NULL;
	if (!gw || !gw->bw) return;
	SetPortWindowPort(win);
	GetMouse(&p);
	if (p.h == last_pt.h && p.v == last_pt.v) return;
	last_pt = p;
	if (!PtInRect(p, &gw->content_rect)) return;
	x_ns = (int)p.h - gw->content_rect.left + gw->scroll_x;
	y_ns = (int)p.v - gw->content_rect.top  + gw->scroll_y;
	browser_window_mouse_track(gw->bw, BROWSER_MOUSE_HOVER, x_ns, y_ns);
#endif
}

int main(void) {
	macsurf_debug_log_init();
	MS_LOG("== MacSurf start ==");
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
	RegisterAppearanceClient();
	MS_LOG("Appearance OK");

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
	/* No image content handlers registered yet (libnsgif/libnsbmp not in
	 * project file list). Disable foreground image fetches so <img> boxes
	 * render as their alt-text fallback rather than spawning hung fetches
	 * that block html_can_begin_conversion. Sites still parse and render. */
	nsoption_set_bool(foreground_images, false);
	nsoption_set_bool(background_images, false);
	/* Enable author CSS so inline <style>/<link> rules apply. */
	nsoption_set_bool(author_level_css, true);
	MS_LOG("images disabled, author_css on");
	netsurf_init(NULL);
	MS_LOG("netsurf_init done");
#ifdef WITH_DUKTAPE
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
		nsurl *home = NULL;
		if (macos9_ot_context != NULL &&
		    nsurl_create(MACSURF_HOME_URL, &home) == NSERROR_OK) {
			MS_LOG("launch: browser_window_create with home url");
			browser_window_create(BW_CREATE_HISTORY | BW_CREATE_FOREGROUND,
				home, NULL, NULL, &bw);
			nsurl_unref(home);
		}
		if (bw == NULL) {
			MS_LOG("launch: fallback create_initial_window");
			macos9_create_initial_window();
		}
	}
	MS_LOG("initial window created");
	while (!macos9_done) macos9_poll();
	MS_LOG("event loop exited");
	macos9_quitting = (bool)1; netsurf_exit();
#ifdef WITH_DUKTAPE
	js_finalise();
#endif
	if (macos9_ot_context) CloseOpenTransportInContext(macos9_ot_context);
	return 0;
}
