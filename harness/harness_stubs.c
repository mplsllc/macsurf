/* harness: real (extern) defs of the macos9 debug facade + misc frontend stubs */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
void macsurf_debug_log_init(void){}
void macsurf_debug_log_close(void){}
void macsurf_debug_log_write(const char *s){ if(s){fputs(s,stderr);fputc('\n',stderr);} }
void macsurf_debug_log_writef(const char *fmt, ...){ va_list a; va_start(a,fmt); vfprintf(stderr,fmt,a); va_end(a); fputc('\n',stderr); }
void macsurf_debug_set_title(const char *m){(void)m;}
void macsurf_debug_show_int(const char *l,long v){(void)l;(void)v;}
void macsurf_debug_log_int(const char *l,long v){(void)l;(void)v;}
void macsurf_debug_log_str(const char *l,const char *v){(void)l;(void)v;}
void macsurf_debug_set_title_force(const char *m){(void)m;}
void macsurf_debug_log_int_force(const char *l,long v){(void)l;(void)v;}
void macsurf_debug_probe_append(const char *m){(void)m;}
void macsurf_debug_probe_append_int(const char *l,long v){(void)l;(void)v;}
void macsurf_debug_probe_reset(void){}

/* fixes895 — reconvert-crash hunt durable-position channel (Mac-only on
 * hardware; here it just echoes so Test 29's reconvert path links + is visible
 * in the ASan run). */
static char g_reconv_pos_h[256] = "reconv-pos (never set)";
void macsurf_debug_log_reconv_flush(int on){(void)on;}
void macsurf_reconv_pos_set(const char *phase,long seq,long node_ix,const char *tag){
	snprintf(g_reconv_pos_h,sizeof(g_reconv_pos_h),
		"reconv-pos phase=%s seq=%ld node=%ld tag=%s",
		phase?phase:"(null)",seq,node_ix,tag?tag:"");
}
void macsurf_reconv_pos_flush(void){ fprintf(stderr,"RECONV-POS: %s\n",g_reconv_pos_h); }
long macsurf_free_mem(void){ return -1; }

/* --- harness deathrow: defer frees to an explicit drain (mimics the Mac
 * quiescent-point defer so the reconvert free-timing matches). --- */
#include <stdbool.h>
int macos9_op_depth = 0;
struct dr_ent { void *ptr; void (*teardown)(void*); struct dr_ent *next; };
static struct dr_ent *g_dr = NULL;
struct content;
void macos9_deathrow_add(void *ptr, void (*teardown)(void *ptr), struct content *pin_key){
  struct dr_ent *e; (void)pin_key; if(!teardown){return;}
  e=(struct dr_ent*)malloc(sizeof *e); if(!e){teardown(ptr);return;}
  e->ptr=ptr; e->teardown=teardown; e->next=g_dr; g_dr=e;
}
void macos9_deathrow_drain(void){
  while(g_dr){ struct dr_ent *e=g_dr; g_dr=e->next; e->teardown(e->ptr); free(e); }
}
bool macos9_sched_any(bool (*pred)(void (*cb)(void *), void *p, void *arg), void *arg){ (void)pred;(void)arg; return false; }

/* macos9_js_mark_dom_dirty (macos9_reconvert.c) calls this unconditionally
 * after every JS-driven DOM mutation. driver.c calls html_reconvert_content
 * directly rather than relying on this debounce, so a no-op is correct here
 * (the earlier auto-gen stub was a 0-arg/void mismatch against the real
 * 3-arg signature -- harmless in practice since the body ignores args, but
 * fragile; this is the real signature instead). */
void macos9_schedule(int ms, void (*cb)(void *p), void *p) { (void)ms; (void)cb; (void)p; }
/* fixes846 (#167 S3) — macos9_js_fetch.c's xhr_slot_release() calls this on
 * every abort/teardown. No-op here for the same reason macos9_schedule is:
 * driver.c drives what it needs directly rather than depending on the
 * cooperative scheduler firing. */
void macos9_schedule_cancel_owner(void *p) { (void)p; }

/* selection_create/destroy: the earlier auto-gen stubs were 0-arg/void,
 * meaning html_reconvert's `c->sel = selection_create(...)` would store
 * whatever garbage was left in the return register -- a landmine for any
 * later selection_destroy(c->sel) (double-free / wild free risk). Real
 * minimal versions: an opaque heap blob that's safe to malloc/free, since
 * nothing on the reconvert-UAF repro path dereferences its contents. */
struct content;
struct selection *selection_create(struct content *c) { (void)c; return (struct selection *)malloc(1); }
void selection_destroy(struct selection *s) { free(s); }

/* fixes914 -- macos9_deathrow.c is a Mac-only frontend file and is NOT linked
 * into the harness, but talloc.c (which IS) reads these to name the death-row
 * entry being torn down in its corruption abort. Define them here so the
 * harness links; they stay NULL, which is exactly the "not inside a death-row
 * teardown" reading the field is meant to convey. */
void *macos9_deathrow_cur_ptr = NULL;
void *macos9_deathrow_cur_fn = NULL;
