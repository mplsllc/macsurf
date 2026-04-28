#include <stdlib.h>
#include <string.h>
#include "utils/errors.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "netsurf/types.h"
#include "netsurf/window.h"
#include "netsurf/browser_window.h"
#include "desktop/browser_history.h"
#include "macos9/macos9.h"
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
	Point pt; if(!g || !c) return; pt=*(Point*)lp; SetPortWindowPort(g->window); TrackControl(c, pt, NULL);
	if(c==g->vscroll) macos9_window_scroll_to(g, g->scroll_x, GetControlValue(c)); else macos9_window_scroll_to(g, GetControlValue(c), g->scroll_y);
#endif
}

void macos9_window_te_activate_url(struct gui_window *g) { if(!g||!g->url_te||g->url_field_active) return; SetPortWindowPort(g->window); TEActivate(g->url_te); g->url_field_active=1; InvalWindowRect(g->window, &g->url_rect); }
void macos9_window_te_deactivate_url(struct gui_window *g) { if(!g||!g->url_te||!g->url_field_active) return; SetPortWindowPort(g->window); TEDeactivate(g->url_te); g->url_field_active=0; InvalWindowRect(g->window, &g->url_rect); }

static void set_url_te_text(struct gui_window *g, const char *u) {
	if(!g||!g->url_te||!u) return; SetPortWindowPort(g->window);
	TESetText(u, (long)strlen(u), g->url_te); TECalText(g->url_te); InvalWindowRect(g->window, &g->url_rect);
}

void macos9_window_navigate(struct gui_window *g, const char *u) {
	struct nsurl *n;
	MS_LOG("navigate:");
	MS_LOG(u ? u : "(null)");
	if(!g||!u||!u[0]) { MS_LOG("nav: no g or empty u"); return; }
	if(!g->bw) { MS_LOG("nav: no bw"); return; }
	set_url_te_text(g,u);
	set_status_text(g,"Loading...");
	if(g->window) InvalWindowRect(g->window, &g->status_rect);
	if(nsurl_create(u,&n)!=NSERROR_OK) { MS_LOG("nav: nsurl_create FAIL"); return; }
	MS_LOG("nav: calling browser_window_navigate");
	browser_window_navigate(g->bw, n, NULL, BW_NAVIGATE_HISTORY, NULL, NULL, NULL);
	nsurl_unref(n);
	MS_LOG("nav: done");
}

void macos9_window_address_bar_submit(struct gui_window *g) {
	CharsHandle h; long l; char r[1024], f[1024];
	MS_LOG("URL submit fired");
	if(!g||!g->url_te) { MS_LOG("submit: no g or url_te"); return; }
	h=TEGetText(g->url_te); if(!h) { MS_LOG("submit: TEGetText null"); return; }
	l=GetHandleSize((Handle)h); if(l<=0) { MS_LOG("submit: empty"); return; }
	if(l>1023) l=1023; HLock((Handle)h); memcpy(r,*h,(size_t)l); HUnlock((Handle)h); r[l]=0;
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

struct gui_window *macos9_create_initial_window(void) { return macos9_window_create(NULL, NULL, 0); }

static struct gui_window *macos9_window_create(struct browser_window *bw, struct gui_window *ex, gui_window_create_flags f) {
	struct gui_window *g=(struct gui_window *)calloc(1,sizeof(*g)); Rect b; short x; if(!g) return NULL;
	g->bw=bw; SetRect(&b, 40, 50, 640, 470); if(CreateNewWindow(6, 0x1F, &b, &g->window)!=0) { free(g); return NULL; }
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
	compute_url_te_rect(&g->url_rect,&b); g->url_te=TENew(&b,&b);
	if(g->url_te) { TESetText(MACSURF_HOME_URL,(long)strlen(MACSURF_HOME_URL),g->url_te); TECalText(g->url_te); TEActivate(g->url_te); }
	ShowWindow(g->window); SelectWindow(g->window);
	GetWindowPortBounds(g->window,&b); b.right=(short)(b.right-b.left); b.bottom=(short)(b.bottom-b.top); b.left=0; b.top=0;
	InvalWindowRect(g->window,&b);
#endif
	g->url_field_active=1; macos9_window_update_scrollbars(g); macos9_window_update_button_states(g);
	return g;
}

void macos9_window_destroy(struct gui_window *g) { struct gui_window **p; for(p=&window_list;*p;p=&(*p)->next) if(*p==g) { *p=g->next; break; } if(g->window) DisposeWindow(g->window); free(g); }
static nserror macos9_gw_invalidate(struct gui_window *g, const struct rect *r) { if(!g||!g->window)return 0; InvalWindowRect(g->window, &g->content_rect); return 0; }
static bool macos9_gw_get_scroll(struct gui_window *g, int *x, int *y) { if(x) *x=g->scroll_x; if(y) *y=g->scroll_y; return 1; }
static nserror macos9_gw_set_scroll(struct gui_window *g, const struct rect *r) { if(r) macos9_window_scroll_to(g,r->x0,r->y0); return 0; }
static nserror macos9_gw_get_dimensions(struct gui_window *g, int *w, int *h) { if(!g) return 0; if(w) *w=g->content_rect.right-g->content_rect.left; if(h) *h=g->content_rect.bottom-g->content_rect.top; return 0; }
static nserror macos9_gw_event(struct gui_window *g, enum gui_window_event e) {
	if(e==GW_EVENT_UPDATE_EXTENT||e==GW_EVENT_NEW_CONTENT||e==GW_EVENT_STOP_THROBBER) {
		int w=0, h=0; if(g->bw && browser_window_get_extents(g->bw, false, &w, &h)==NSERROR_OK) { g->content_width=w; g->content_height=h; }
		macos9_window_update_scrollbars(g); macos9_window_update_button_states(g); macos9_window_invalidate_all(g);
	}
	return 0;
}
static void macos9_gw_set_title(struct gui_window *g, const char *t) { Str255 p; size_t l; if(!g||!g->window||!t) return; l=strlen(t); if(l>255) l=255; p[0]=(unsigned char)l; memcpy(p+1,t,l); SetWTitle(g->window,p); }
static nserror macos9_gw_set_url(struct gui_window *g, struct nsurl *u) { const char *s; if(g&&u&&g->url_te&&(s=nsurl_access(u))) set_url_te_text(g,s); return 0; }
static void macos9_gw_set_status(struct gui_window *g, const char *t) { if(g&&t) { strncpy(g->status,t,127); g->status[127]=0; if(g->window) InvalWindowRect(g->window,&g->status_rect); } }

static struct gui_window_table wt = {
	macos9_window_create, macos9_window_destroy, macos9_gw_invalidate, macos9_gw_get_scroll,
	macos9_gw_set_scroll, macos9_gw_get_dimensions, macos9_gw_event, macos9_gw_set_title,
	macos9_gw_set_url, (void*)0, macos9_gw_set_status, (void*)0, (void*)0, (void*)0, (void*)0, (void*)0, (void*)0, (void*)0, (void*)0, (void*)0
};
struct gui_window_table *macos9_window_table = &wt;
