#include "macos9/macos9.h"
#include <OpenTransport.h>
extern OTClientContextPtr macos9_ot_context;
#include <stdio.h>
#include <string.h>
#include "utils/log.h"
#include "utils/nsurl.h"
#include "netsurf/netsurf.h"
#include "netsurf/browser_window.h"
#include "netsurf/plotters.h"
#include "desktop/gui_table.h"
#include "macsurf_config.h"
#include "macsurf_debug.h"

#ifdef __MACOS__
#include <OpenTransport.h>
#include <OpenTptInternet.h>
OTClientContextPtr macos9_ot_context = NULL;
#endif

bool macos9_done = (bool)0;
bool macos9_quitting = (bool)0;
struct netsurf_table macos9_table;
extern const struct plotter_table macos9_plotters;

static void draw_url_bar(struct gui_window *gw) {
#ifdef __MACOS__
	RGBColor black = {0,0,0}, white = {0xFFFF, 0xFFFF, 0xFFFF};
	RGBForeColor(&black); RGBBackColor(&white);
	TextFont(kFontIDMonaco); TextSize(10); TextFace(0);
	EraseRect(&gw->url_rect); FrameRect(&gw->url_rect);
	if (gw->url_te != NULL) TEUpdate(&gw->url_rect, gw->url_te);
#endif
}

static void draw_status_bar(struct gui_window *gw) {
#ifdef __MACOS__
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
#ifdef __MACOS__
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
#ifdef __MACOS__
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
#ifdef __MACOS__
	WindowRef win = (WindowRef)(unsigned long)event->message;
	struct gui_window *gw = macos9_find_window(win);
	if (!gw || macos9_quitting) return;
	SetPortWindowPort(win); BeginUpdate(win);
	EraseRect(&gw->content_rect);
	draw_url_bar(gw); DrawControls(win); draw_status_bar(gw);
	if (gw->bw && browser_window_redraw_ready(gw->bw)) {
		struct rect clip; struct redraw_context ctx;
		extern long macos9_plot_text_count, macos9_plot_rect_count;
		extern long macos9_hrb_visits, macos9_hrb_block, macos9_hrb_inlinec,
			    macos9_hrb_inline, macos9_hrb_text, macos9_hrb_other,
			    macos9_hrb_clip_skips;
		macos9_plot_text_count = 0;
		macos9_plot_rect_count = 0;
		macos9_hrb_visits = 0;
		macos9_hrb_block = 0;
		macos9_hrb_inlinec = 0;
		macos9_hrb_inline = 0;
		macos9_hrb_text = 0;
		macos9_hrb_other = 0;
		macos9_hrb_clip_skips = 0;
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
		browser_window_redraw(gw->bw,
			gw->content_rect.left - gw->scroll_x,
			gw->content_rect.top  - gw->scroll_y,
			&clip, &ctx);
		macsurf_debug_log_writef(
			"update: bw_redraw done visits=%ld block=%ld inlinec=%ld inline=%ld text=%ld other=%ld skips=%ld plot_text=%ld plot_rect=%ld",
			macos9_hrb_visits, macos9_hrb_block, macos9_hrb_inlinec,
			macos9_hrb_inline, macos9_hrb_text, macos9_hrb_other,
			macos9_hrb_clip_skips,
			macos9_plot_text_count, macos9_plot_rect_count);
	} else if (gw->bw) {
		MS_LOG("update: bw not ready, skip");
	}
	EndUpdate(win);
#endif
}

void macos9_handle_mouse_down(const EventRecord *event) {
#ifdef __MACOS__
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
								macos9_window_handle_scrollbar_click(gw, ctrl, cpart, NULL);
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
#ifdef __MACOS__
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
			TEKey(ch, gw->url_te);
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

void macos9_poll(void) {
	EventRecord ev;
	if (WaitNextEvent(everyEvent, &ev, 1, NULL)) {
		switch (ev.what) {
			case updateEvt: macos9_handle_update(&ev); break;
			case mouseDown: macos9_handle_mouse_down(&ev); break;
			case keyDown: case autoKey: macos9_handle_key_down(&ev); break;
		}
	}
	if (!macos9_quitting) {
		{ extern bool macos9_schedule_run(void); macos9_schedule_run(); }
		macos9_windows_te_idle(); macos9_windows_process_deferred();
	}
}

int main(void) {
	macsurf_debug_log_init();
	MS_LOG("== MacSurf start ==");
#ifdef __MACOS__
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
	netsurf_init(NULL);
	MS_LOG("netsurf_init done");
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
	if (macos9_ot_context) CloseOpenTransportInContext(macos9_ot_context);
	return 0;
}
