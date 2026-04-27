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
