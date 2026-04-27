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
		clip.x0 = gw->content_rect.left; clip.y0 = gw->content_rect.top;
		clip.x1 = gw->content_rect.right; clip.y1 = gw->content_rect.bottom;
		memset(&ctx, 0, sizeof(ctx)); ctx.interactive = (bool)1; ctx.plot = &macos9_plotters;
		browser_window_redraw(gw->bw, gw->scroll_x, gw->scroll_y, &clip, &ctx);
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
		case inMenuBar:
			MenuSelect(event->where);
			HiliteMenu(0);
			break;
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
		netsurf_poll(); fetch_poll(NULL);
		macos9_windows_te_idle(); macos9_windows_process_deferred();
	}
}

int main(void) {
#ifdef MACSURF_DEBUG
	macsurf_debug_log_init(); MS_LOG("MacSurf Start");
#endif
#ifdef __MACOS__
	InitCursor();
	if (InitOpenTransportInContext(kInitOTForApplicationMask, &macos9_ot_context) != noErr) macos9_ot_context = NULL;
	RegisterAppearanceClient();
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
		macos9_table.llcache = null_llcache_table;
	}
	netsurf_register(&macos9_table);
	nsoption_init(NULL, NULL, NULL);
	netsurf_init(NULL);
	{ extern nserror macos9_http_fetcher_register(void); macos9_http_fetcher_register(); }
	macos9_create_initial_window();
	while (!macos9_done) macos9_poll();
	macos9_quitting = (bool)1; netsurf_exit();
	if (macos9_ot_context) CloseOpenTransportInContext(macos9_ot_context);
	return 0;
}
