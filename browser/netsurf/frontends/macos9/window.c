#include <stdlib.h>
#include <string.h>
#include "utils/errors.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "netsurf/types.h"
#include "netsurf/window.h"
#include "netsurf/mouse.h"
#include "netsurf/browser_window.h"
#include "desktop/browser_history.h"
#include "macos9.h"
#include "macsurf_config.h"
#include "macsurf_debug.h"

#ifdef __MACOS9__
#include <MacWindows.h>
#include <Controls.h>
#include <Appearance.h>
#include <Quickdraw.h>
#include <TextEdit.h>
#endif

static struct gui_window *window_list = NULL;
static struct gui_window *macos9_window_create(struct browser_window *bw, struct gui_window *ex, gui_window_create_flags f);

struct gui_window *macos9_find_window(WindowRef w) { struct gui_window *g; for(g=window_list;g;g=g->next) if(g->window==w) return g; return NULL; }
struct gui_window *macos9_window_list_head(void) { return window_list; }
static void set_status_text(struct gui_window *g, const char *m) { if(!m) g->status[0]=0; else { strncpy(g->status,m,127); g->status[127]=0; } }
static void compute_url_te_rect(const Rect *u, Rect *o) { o->left=(short)(u->left+4); o->top=(short)(u->top+2); o->right=(short)(u->right-4); o->bottom=(short)(u->bottom-2); }

void macos9_window_layout(struct gui_window *g) {
	Rect c; short w, h, ux, ur, cb, ht; if(!g||!g->window) return;
	GetWindowBounds(g->window, 33, &c); w=(short)(c.right-c.left); h=(short)(c.bottom-c.top);
	ux=(short)(4+4*64); ur=(short)(w-4); SetRect(&g->url_rect, ux, 8, ur, 30);
	ht=(short)(h-15); cb=(short)(ht-16); SetRect(&g->content_rect, 0, 38, (short)(w-15), cb); SetRect(&g->status_rect, 0, cb, (short)(w-15), ht);
	if(g->vscroll) { MoveControl(g->vscroll, (short)(w-15), 37); SizeControl(g->vscroll, 16, (short)(cb-36)); }
	if(g->hscroll) { MoveControl(g->hscroll, -1, ht); SizeControl(g->hscroll, (short)(w-13), 16); }
}

void macos9_window_invalidate_all(struct gui_window *g) { Rect r; if(!g||!g->window)return; GetWindowBounds(g->window, 33, &r); r.right=(short)(r.right-r.left); r.bottom=(short)(r.bottom-r.top); r.left=0; r.top=0; InvalWindowRect(g->window, &r); }
void macos9_window_invalidate_content(struct gui_window *g) { if(!g||!g->window)return; InvalWindowRect(g->window, &g->content_rect); }

/* fixes76c -- invalidate a single rect, clipped to content_rect.
 * x, y are window coords (already include the content_rect.top /
 * scroll offset because redraw.c receives that offset from main.c
 * and walks the box tree adding it). Used by the animation tick to
 * avoid full-content redraws (whole-page flashing on OS 9 hardware).
 *
 * fixes76b shipped this with a page-coord->window-coord conversion
 * (`content_rect.top + (py - scroll_y)`) that double-applied the
 * chrome offset: the invalidated rect ended up 38 px below the
 * actual box. Visible symptom: top half of each animated badge
 * never repainted, showing one frozen opacity above the live one. */
void macos9_window_invalidate_rect(struct gui_window *g, int px, int py, int pw, int ph) {
#ifdef __MACOS9__
	Rect r;
	int wx0, wy0, wx1, wy1;
	if (!g || !g->window) return;
	wx0 = px;
	wy0 = py;
	wx1 = px + pw;
	wy1 = py + ph;
	if (wx0 < g->content_rect.left)  wx0 = g->content_rect.left;
	if (wy0 < g->content_rect.top)   wy0 = g->content_rect.top;
	if (wx1 > g->content_rect.right) wx1 = g->content_rect.right;
	if (wy1 > g->content_rect.bottom)wy1 = g->content_rect.bottom;
	if (wx1 <= wx0 || wy1 <= wy0) return;
	r.left = (short)wx0; r.top = (short)wy0;
	r.right = (short)wx1; r.bottom = (short)wy1;
	InvalWindowRect(g->window, &r);
#else
	(void)g; (void)px; (void)py; (void)pw; (void)ph;
#endif
}

void macos9_window_update_scrollbars(struct gui_window *g) {
	int vw, vh, mx, my; if(!g) return;
	vw=g->content_rect.right-g->content_rect.left; vh=g->content_rect.bottom-g->content_rect.top;
	mx=g->content_width-vw; my=g->content_height-vh; if(mx<0) mx=0; if(my<0) my=0;
#ifdef __MACOS9__
	SetPortWindowPort(g->window);
	if(g->vscroll) { SetControlMaximum(g->vscroll, (short)(my>32767?32767:my)); SetControlValue(g->vscroll, (short)g->scroll_y); HiliteControl(g->vscroll, (short)(my>0?0:255)); Draw1Control(g->vscroll); }
	if(g->hscroll) { SetControlMaximum(g->hscroll, (short)(mx>32767?32767:mx)); SetControlValue(g->hscroll, (short)g->scroll_x); HiliteControl(g->hscroll, (short)(mx>0?0:255)); Draw1Control(g->hscroll); }
#endif
}

void macos9_window_scroll_to(struct gui_window *g, int nx, int ny) { if(!g) return; g->scroll_x=nx; g->scroll_y=ny; macos9_window_update_scrollbars(g); macos9_window_invalidate_content(g); }
void macos9_window_scroll_by(struct gui_window *g, int dx, int dy) { if(g) macos9_window_scroll_to(g, g->scroll_x+dx, g->scroll_y+dy); }

void macos9_window_handle_scrollbar_click(struct gui_window *g, ControlRef c, short p, void *lp) {
#ifdef __MACOS9__
	Point pt;
	short step = 48, page = 200;
	int cur, mx;
	if(!g || !c || !lp) return;
	pt = *(Point*)lp;
	SetPortWindowPort(g->window);
	cur = (c == g->vscroll) ? g->scroll_y : g->scroll_x;
	mx  = (c == g->vscroll) ? GetControlMaximum(g->vscroll) : GetControlMaximum(g->hscroll);
	switch (p) {
	case 20: cur -= step; break;          /* up/left arrow */
	case 21: cur += step; break;          /* down/right arrow */
	case 22: cur -= page; break;          /* page up/left */
	case 23: cur += page; break;          /* page down/right */
	case 129:                              /* thumb drag */
		TrackControl(c, pt, NULL);
		cur = GetControlValue(c);
		break;
	default:
		TrackControl(c, pt, NULL);
		cur = GetControlValue(c);
		break;
	}
	if (cur < 0) cur = 0;
	if (cur > mx) cur = mx;
	if (c == g->vscroll) macos9_window_scroll_to(g, g->scroll_x, cur);
	else                 macos9_window_scroll_to(g, cur, g->scroll_y);
#endif
}

void macos9_window_te_activate_url(struct gui_window *g) { if(!g||!g->url_te||g->url_field_active) return; SetPortWindowPort(g->window); TEActivate(g->url_te); g->url_field_active=1; InvalWindowRect(g->window, &g->url_rect); }
void macos9_window_te_deactivate_url(struct gui_window *g) { if(!g||!g->url_te||!g->url_field_active) return; SetPortWindowPort(g->window); TEDeactivate(g->url_te); g->url_field_active=0; InvalWindowRect(g->window, &g->url_rect); }

static void set_url_te_text(struct gui_window *g, const char *u) {
	CharsHandle h;
	long new_len;
	long cur_len;
	if(!g||!g->url_te||!u) return;
	/* fixes109 — dedupe. NetSurf core calls gui_window->set_url repeatedly
	 * during navigation (initial, after redirect, on every history nav, on
	 * some progress events). The old unconditional InvalRect on url_rect
	 * triggered an updateEvt → browser_window_redraw → draw_url_bar cycle
	 * on every call, even when the URL string was byte-identical. On a
	 * loading page the URL bar would visibly pulse for many seconds with
	 * nothing changing — that was a big part of the "sticky" feeling. Now:
	 * compare against the current TE buffer and skip if equal. */
	new_len = (long)strlen(u);
	h = TEGetText(g->url_te);
	if (h != NULL) {
		cur_len = GetHandleSize((Handle)h);
		if (cur_len == new_len &&
		    (new_len == 0 || memcmp(*h, u, (size_t)new_len) == 0)) {
			return;
		}
	}
	SetPortWindowPort(g->window);
	TESetText(u, new_len, g->url_te);
	TECalText(g->url_te);
	InvalWindowRect(g->window, &g->url_rect);
}

void macos9_window_navigate(struct gui_window *g, const char *u) {
	struct nsurl *n;
	nserror nav_e;
	long uu_len;
	int i;
	MS_LOG("navigate:");
	MS_LOG(u ? u : "(null)");
	if(!g||!u||!u[0]) { MS_LOG("nav: no g or empty u"); return; }
	if(!g->bw) { MS_LOG("nav: no bw"); return; }
	uu_len = (long)strlen(u);
	macsurf_debug_log_writef("nav: url len=%ld bytes", uu_len);
	for(i = 0; u[i] != 0; i++) {
		unsigned char c = (unsigned char)u[i];
		if (c < 0x20 || c == 0x7F) {
			macsurf_debug_log_writef("nav: ctrl-char at %d = 0x%02x", i, (int)c);
		}
	}
	set_url_te_text(g,u);
	set_status_text(g,"Loading...");
	if(g->window) InvalWindowRect(g->window, &g->status_rect);
	if(nsurl_create(u,&n)!=NSERROR_OK) { MS_LOG("nav: nsurl_create FAIL"); return; }
	MS_LOG("nav: calling browser_window_navigate");
	{
		/* fixes161a — mark the next http_setup() as DOCUMENT so the
		 * resource governor gives it document-class priority, regardless
		 * of URL suffix. Single-shot: consumed by the first setup call. */
		extern void macos9_http_mark_next_as_document(void);
		macos9_http_mark_next_as_document();
	}
	nav_e = browser_window_navigate(g->bw, n, NULL, BW_NAVIGATE_HISTORY, NULL, NULL, NULL);
	macsurf_debug_log_writef("nav: bw_navigate returned %d", (int)nav_e);
	nsurl_unref(n);
	MS_LOG("nav: done");
}

void macos9_window_address_bar_submit(struct gui_window *g) {
	CharsHandle h; long l; char r[1024], f[1024];
	long i, j;
	MS_LOG("URL submit fired");
	if(!g||!g->url_te) { MS_LOG("submit: no g or url_te"); return; }
	h=TEGetText(g->url_te); if(!h) { MS_LOG("submit: TEGetText null"); return; }
	l=GetHandleSize((Handle)h); if(l<=0) { MS_LOG("submit: empty"); return; }
	if(l>1023) l=1023; HLock((Handle)h); memcpy(r,*h,(size_t)l); HUnlock((Handle)h); r[l]=0;
	/* Strip control chars (CR/LF/tab/embedded NUL) and leading whitespace. */
	j = 0;
	for (i = 0; i < l; i++) {
		unsigned char c = (unsigned char)r[i];
		if (c >= 0x20 && c != 0x7F) {
			r[j++] = (char)c;
		}
	}
	r[j] = 0;
	/* Trim trailing spaces. */
	while (j > 0 && r[j-1] == ' ') { j--; r[j] = 0; }
	/* Trim leading spaces. */
	{ long k = 0; while (r[k] == ' ') k++; if (k > 0) memmove(r, r+k, (size_t)(j - k + 1)); }
	macsurf_debug_log_writef("submit: cleaned url='%s'", r);
	if (r[0] == 0) { MS_LOG("submit: empty after clean"); return; }
	if(!strstr(r,"://")) sprintf(f,"http://%s",r); else strcpy(f,r);
	macos9_window_navigate(g,f);
}

void macos9_window_back(struct gui_window *g) { if(g&&g->bw&&browser_window_history_back_available(g->bw)) { browser_window_history_back(g->bw, false); macos9_window_update_button_states(g); } }
void macos9_window_forward(struct gui_window *g) { if(g&&g->bw&&browser_window_history_forward_available(g->bw)) { browser_window_history_forward(g->bw, false); macos9_window_update_button_states(g); } }
void macos9_window_reload(struct gui_window *g) { if(g&&g->bw) browser_window_reload(g->bw, true); }
void macos9_window_home(struct gui_window *g) { macos9_window_navigate(g, MACSURF_HOME_URL); }

void macos9_window_update_button_states(struct gui_window *g) {
#ifdef __MACOS9__
	if(!g) return; SetPortWindowPort(g->window);
	if(g->back_btn) { HiliteControl(g->back_btn, (short)(g->bw && browser_window_history_back_available(g->bw)?0:255)); Draw1Control(g->back_btn); }
	if(g->forward_btn) { HiliteControl(g->forward_btn, (short)(g->bw && browser_window_history_forward_available(g->bw)?0:255)); Draw1Control(g->forward_btn); }
	if(g->reload_btn) { HiliteControl(g->reload_btn, (short)(g->bw && browser_window_has_content(g->bw)?0:255)); Draw1Control(g->reload_btn); }
	if(g->home_btn) Draw1Control(g->home_btn);
#endif
}

void macos9_window_resize(struct gui_window *g) {
	Rect tr; TERec *ptr; if(!g) return; macos9_window_layout(g);
#ifdef __MACOS9__
	if(g->url_te) { compute_url_te_rect(&g->url_rect, &tr); ptr = *g->url_te; ptr->destRect = tr; ptr->viewRect = tr; TECalText(g->url_te); }
#endif
	macos9_window_update_scrollbars(g); g->needs_reformat=1; macos9_window_invalidate_all(g);
}

void macos9_windows_te_idle(void) {
#ifdef __MACOS9__
	struct gui_window *g; GrafPtr op; GetPort(&op);
	for(g=window_list; g; g=g->next) { if(g->url_field_active && g->url_te) { SetPortWindowPort(g->window); TEIdle(g->url_te); } }
	SetPort(op);
#endif
}

void macos9_windows_process_deferred(void) {
	struct gui_window *g; for(g=window_list;g;g=g->next) if(g->needs_reformat && g->bw && !g->reformat_in_progress) {
		g->reformat_in_progress=1; g->needs_reformat=0; browser_window_schedule_reformat(g->bw); g->reformat_in_progress=0;
	}
}

struct gui_window *initial_win = NULL;

struct gui_window *macos9_create_initial_window(void) {
	struct browser_window *bw = NULL;
	nserror e;
	MS_LOG("create_initial: calling browser_window_create");
	e = browser_window_create(BW_CREATE_HISTORY, NULL, NULL, NULL, &bw);
	if (e != NSERROR_OK) { MS_LOG("create_initial: browser_window_create FAIL"); return NULL; }
	MS_LOG("create_initial: bw attached");
	initial_win = window_list;
	return window_list;
}

static struct gui_window *macos9_window_create(struct browser_window *bw, struct gui_window *ex, gui_window_create_flags f) {
	struct gui_window *g=(struct gui_window *)calloc(1,sizeof(*g)); Rect b; short x; if(!g) return NULL;
	/* fixes124: open at desktop-class default size (1024x768)
	 * so real-window-width media queries naturally match the
	 * desktop branch on modern responsive sites. Clamped to
	 * the actual screen bounds with margin so a G3 iBook at
	 * 800x600 still gets a usable window without overflow.
	 * Uses Carbon-safe GetQDGlobalsScreenBits (qd globals
	 * struct is unavailable to Carbon apps). */
	{
		BitMap bm;
		Rect sb;
		short sw;
		short sh;
		short want_w;
		short want_h;
		short left;
		short top;
		short right;
		short bot;
		GetQDGlobalsScreenBits(&bm);
		sb = bm.bounds;
		sw = (short)(sb.right - sb.left);
		sh = (short)(sb.bottom - sb.top);
		want_w = 1024;
		want_h = 768;
		left = 40;
		top = 50;
		if ((short)(sw - left - 20) < want_w) {
			want_w = (short)(sw - left - 20);
		}
		if ((short)(sh - top - 30) < want_h) {
			want_h = (short)(sh - top - 30);
		}
		if (want_w < 480) want_w = 480;
		if (want_h < 360) want_h = 360;
		right = (short)(left + want_w);
		bot = (short)(top + want_h);
		SetRect(&b, left, top, right, bot);
	}
	g->bw=bw; if(CreateNewWindow(6, 0x1F, &b, &g->window)!=0) { free(g); return NULL; }
	SetWRefCon(g->window,(long)g); SetPortWindowPort(g->window); SetWTitle(g->window,(const unsigned char*)"\pMacSurf");
	g->next=window_list; window_list=g; 
	x=4; SetRect(&b,x,8,(short)(x+60),30); g->back_btn=NewControl(g->window,&b,(const unsigned char*)"\pBack",1,0,0,1,368,(long)g); x=(short)(x+64);
	SetRect(&b,x,8,(short)(x+60),30); g->forward_btn=NewControl(g->window,&b,(const unsigned char*)"\pFwd",1,0,0,1,368,(long)g); x=(short)(x+64);
	SetRect(&b,x,8,(short)(x+60),30); g->reload_btn=NewControl(g->window,&b,(const unsigned char*)"\pReload",1,0,0,1,368,(long)g); x=(short)(x+64);
	SetRect(&b,x,8,(short)(x+60),30); g->home_btn=NewControl(g->window,&b,(const unsigned char*)"\pHome",1,0,0,1,368,(long)g);
	SetRect(&b,0,0,16,16); g->vscroll=NewControl(g->window,&b,(const unsigned char*)"\p",1,0,0,0,384,(long)g);
	g->hscroll=NewControl(g->window,&b,(const unsigned char*)"\p",1,0,0,0,384,(long)g);
	macos9_window_layout(g);
#ifdef __MACOS9__
	/* Show + select the window FIRST so subsequent Toolbox calls
	 * run in a fully realized port/state. The previous order
	 * (create TE before show) is the suspected cause of the
	 * "URL field unresponsive on initial window" regression. */
	ShowWindow(g->window); SelectWindow(g->window);
	SetPortWindowPort(g->window);
	compute_url_te_rect(&g->url_rect,&b); g->url_te=TENew(&b,&b);
	if(g->url_te) {
		TESetText(MACSURF_HOME_URL,(long)strlen(MACSURF_HOME_URL),g->url_te);
		TECalText(g->url_te);
		TEActivate(g->url_te);
		TESetSelect(0, 32767, g->url_te);  /* select-all so first keystroke replaces */
	}
	GetWindowPortBounds(g->window,&b); b.right=(short)(b.right-b.left); b.bottom=(short)(b.bottom-b.top); b.left=0; b.top=0;
	InvalWindowRect(g->window,&b);
#endif
	g->url_field_active=1; macos9_window_update_scrollbars(g); macos9_window_update_button_states(g);
	return g;
}

void macos9_window_destroy(struct gui_window *g) { struct gui_window **p; for(p=&window_list;*p;p=&(*p)->next) if(*p==g) { *p=g->next; break; }
#ifdef __MACOS9__
	if(g->content_gworld) { DisposeGWorld(g->content_gworld); g->content_gworld = NULL; }
#endif
	if(g->window) DisposeWindow(g->window); free(g); }
/* fixes100 — honour NetSurf's per-update dirty rect.
 *
 * Pre-fixes100 this function logged the supplied rect and then
 * unconditionally InvalWindowRect'd the entire content area. That
 * meant every keystroke (caret blink, textarea text change), every
 * scroll-bar value update, every late image arrival, every layout
 * tweak triggered a full content viewport repaint — on the duck-duck
 * search results page that's ~170 DrawText calls and ~600 box-tree
 * visits per character typed, ~200-300ms per keystroke on G3/G4.
 *
 * NetSurf passes r in document/layout coords (pre-scroll). Convert
 * to window coords: window = layout + content_rect.{top,left} -
 * scroll_{y,x}. Then clip to content_rect so we never invalidate
 * outside the viewport (a stray invalidate over the URL bar would
 * trigger TextEdit overdraw bugs). Null r still means full content,
 * matching the prior contract for callers that ask for a full
 * repaint (NEW_CONTENT, reformat, etc). */
static nserror macos9_gw_invalidate(struct gui_window *g, const struct rect *r) {
	if(!g||!g->window) return 0;
	if (r != NULL) {
		Rect ir;
		int wx0, wy0, wx1, wy1;
		macsurf_debug_log_writef("gw_invalidate: r=(%d,%d,%d,%d)",
			r->x0, r->y0, r->x1, r->y1);
		wx0 = r->x0 + g->content_rect.left - g->scroll_x;
		wy0 = r->y0 + g->content_rect.top  - g->scroll_y;
		wx1 = r->x1 + g->content_rect.left - g->scroll_x;
		wy1 = r->y1 + g->content_rect.top  - g->scroll_y;
		if (wx0 < g->content_rect.left)   wx0 = g->content_rect.left;
		if (wy0 < g->content_rect.top)    wy0 = g->content_rect.top;
		if (wx1 > g->content_rect.right)  wx1 = g->content_rect.right;
		if (wy1 > g->content_rect.bottom) wy1 = g->content_rect.bottom;
		if (wx1 <= wx0 || wy1 <= wy0) return 0;
		ir.left   = (short)wx0;
		ir.top    = (short)wy0;
		ir.right  = (short)wx1;
		ir.bottom = (short)wy1;
		InvalWindowRect(g->window, &ir);
	} else {
		MS_LOG("gw_invalidate: r=NULL (full)");
		InvalWindowRect(g->window, &g->content_rect);
	}
	return 0;
}
static bool macos9_gw_get_scroll(struct gui_window *g, int *x, int *y) { if(x) *x=g->scroll_x; if(y) *y=g->scroll_y; return 1; }
static nserror macos9_gw_set_scroll(struct gui_window *g, const struct rect *r) { if(r) macos9_window_scroll_to(g,r->x0,r->y0); return 0; }
static nserror macos9_gw_get_dimensions(struct gui_window *g, int *w, int *h) { if(!g) return 0; if(w) *w=g->content_rect.right-g->content_rect.left; if(h) *h=g->content_rect.bottom-g->content_rect.top; return 0; }
static nserror macos9_gw_event(struct gui_window *g, enum gui_window_event e) {
	struct hlcache_handle *cur = (g && g->bw) ? browser_window_get_content(g->bw) : NULL;
	macsurf_debug_log_writef("gw_event: e=%d current_content=%p", (int)e, cur);
	if(e==GW_EVENT_UPDATE_EXTENT||e==GW_EVENT_NEW_CONTENT||e==GW_EVENT_STOP_THROBBER) {
		int w=0, h=0; if(g->bw && browser_window_get_extents(g->bw, false, &w, &h)==NSERROR_OK) { g->content_width=w; g->content_height=h; }
		if(e==GW_EVENT_NEW_CONTENT) {
			g->scroll_x=0; g->scroll_y=0;
			if(g->bw) {
				browser_window_schedule_reformat(g->bw);
				MS_LOG("gw_event: NEW_CONTENT -> schedule_reformat");
			}
		}
		macsurf_debug_log_writef("gw_event: invalidate w=%d h=%d scroll=(%d,%d)", g->content_width, g->content_height, g->scroll_x, g->scroll_y);
		macos9_window_update_scrollbars(g); macos9_window_update_button_states(g); macos9_window_invalidate_all(g);
	}
	return 0;
}
static void macos9_gw_set_title(struct gui_window *g, const char *t) { Str255 p; size_t l; if(!g||!g->window||!t) return; l=strlen(t); if(l>255) l=255; p[0]=(unsigned char)l; memcpy(p+1,t,l); SetWTitle(g->window,p); }
static nserror macos9_gw_set_url(struct gui_window *g, struct nsurl *u) { const char *s; if(g&&u&&g->url_te&&(s=nsurl_access(u))) set_url_te_text(g,s); return 0; }
static void macos9_gw_set_status(struct gui_window *g, const char *t) {
	/* fixes109 — dedupe. NetSurf core fires set_status on every fetch
	 * progress callback, every hover, every link-tracking transition.
	 * Many of those fire with the same string repeatedly (e.g. "Loading
	 * https://X..." while bytes accumulate). Unconditional InvalRect on
	 * status_rect was painting the status bar dozens of times per
	 * second on a loading page, costing CPU and showing as the pulsing
	 * status-bar redraw loop in the fixes107 log. Skip the InvalRect
	 * unless the visible text actually changed. */
	if (g == NULL || t == NULL) return;
	if (strcmp(g->status, t) == 0) return;
	strncpy(g->status, t, 127);
	g->status[127] = 0;
	if (g->window) InvalWindowRect(g->window, &g->status_rect);
}

static void macos9_gw_set_pointer(struct gui_window *g, enum gui_pointer_shape shape)
{
#ifdef __MACOS9__
	(void)g;
	switch (shape) {
	case GUI_POINTER_POINT:
		SetThemeCursor(kThemePointingHandCursor);
		break;
	case GUI_POINTER_CARET:
		SetThemeCursor(kThemeIBeamCursor);
		break;
	case GUI_POINTER_WAIT:
	case GUI_POINTER_PROGRESS:
		SetThemeCursor(kThemeWatchCursor);
		break;
	case GUI_POINTER_CROSS:
		SetThemeCursor(kThemeCrossCursor);
		break;
	case GUI_POINTER_MOVE:
		SetThemeCursor(kThemeOpenHandCursor);
		break;
	default:
		SetThemeCursor(kThemeArrowCursor);
		break;
	}
#else
	(void)g; (void)shape;
#endif
}

static struct gui_window_table wt = {
	macos9_window_create, macos9_window_destroy, macos9_gw_invalidate, macos9_gw_get_scroll,
	macos9_gw_set_scroll, macos9_gw_get_dimensions, macos9_gw_event, macos9_gw_set_title,
	macos9_gw_set_url, (void*)0, macos9_gw_set_status, macos9_gw_set_pointer, (void*)0, (void*)0, (void*)0, (void*)0, (void*)0, (void*)0, (void*)0, (void*)0
};
struct gui_window_table *macos9_window_table = &wt;
