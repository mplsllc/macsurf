/* Exercise the production batch state machine with deterministic render and
 * registry edges. No Carbon, DOM allocator or wall-clock scheduling needed. */
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#define MACSURF_RENDER_BATCH_TEST
#include "build/macos9_reconvert.c"

struct fake_node { int refs; int paint; int inherited; };
static unsigned long generation = 1, document = 1, batch_id;
static int live = 1, scheduled, full_calls, paint_calls, inherited_calls;
static int inject, refuse;
static struct content content;
static struct fake_node nodes[40];
int macsurf_reconvert_in_progress;
struct gui_window *macos9_paint_gw;
static void (*callback)(void *);

int macos9_content_is_live(struct content *c) { (void)c; return live; }
unsigned long macos9_content_token(struct content *c) { (void)c; return generation; }
int macos9_content_token_valid(struct content *c, unsigned long t)
{ (void)c; return live && generation == t; }
unsigned long html_content_get_doc_id(struct content *c) { (void)c; return document; }
unsigned long html_content_get_frame_id(struct content *c) { (void)c; return 1; }
unsigned long content_get_nav_id(struct content *c) { (void)c; return 1; }
unsigned long ms_diag_cur_script(void) { return 1; }
unsigned long ms_diag_cur_task(void) { return 1; }
unsigned long ms_diag_batch_open(const struct ms_diag_provenance *p)
{ (void)p; return ++batch_id; }
void ms_diag_batch_add(unsigned long b, int k, unsigned long t)
{ (void)b; (void)k; (void)t; }
void ms_diag_batch_freeze(unsigned long b) { (void)b; }
unsigned long ms_diag_render_enter(struct ms_diag_render_scope *s, int k,
        const struct ms_diag_provenance *p)
{ (void)s; (void)k; (void)p; return 1; }
void ms_diag_render_leave(struct ms_diag_render_scope *s, int r, int why)
{ (void)s; (void)r; (void)why; }
void macsurf_debug_log_writef(const char *fmt, ...) { (void)fmt; }
void *macsurf_reconvert_node_ref(void *n)
{ ((struct fake_node *)n)->refs++; return n; }
void macsurf_reconvert_node_unref(void *n)
{ assert(((struct fake_node *)n)->refs > 0); ((struct fake_node *)n)->refs--; }
nserror macos9_schedule(int ms, void (*cb)(void *), void *p)
{ (void)p; assert(ms >= 0); scheduled++; callback = cb; return NSERROR_OK; }
int macsurf_html_has_droppable_inflight(struct content *c) { (void)c; return 0; }
double macos9_micros(void) { return 0; }
int html_reconvert_fast_style(struct content *c, void *n)
{
    (void)c;
    paint_calls++;
    if (inject) {
        inject = 0;
        macos9_js_mark_dom_dirty_node(&content, &nodes[2], MACOS9_DOMMUT_SETATTR_STYLE);
        macos9_js_mark_dom_dirty_node(&content, &nodes[3], MACOS9_DOMMUT_SETATTR_STYLE);
        assert(scheduled == 0);
        assert(macos9_reconvert_flush_now(&content) == 0);
    }
    return ((struct fake_node *)n)->paint ? 0 : -1;
}
int html_reconvert_fast_inherited_color(struct content *c, void *n)
{ (void)c; inherited_calls++; return ((struct fake_node *)n)->inherited ? 0 : -1; }
int html_reconvert_fast_layout(struct content *c, void *n)
{ (void)c; (void)n; return -1; }
int html_reconvert_fast_class(struct content *c, void *n)
{ return html_reconvert_fast_inherited_color(c, n); }
int html_reconvert_content(struct content *c)
{
    (void)c;
    full_calls++;
    if (inject) {
        inject = 0;
        macos9_js_mark_dom_dirty_node(&content, &nodes[2], MACOS9_DOMMUT_SETATTR_STYLE);
        macos9_js_mark_dom_dirty_node(&content, &nodes[3], MACOS9_DOMMUT_SETATTR_STYLE);
        assert(scheduled == 0);
    }
    return refuse;
}
static void mark(int n, int kind)
{ macos9_js_mark_dom_dirty_node(&content, &nodes[n], kind); }
static void fire(void)
{ assert(scheduled == 1); scheduled = 0; callback(NULL); }
static void reset(void)
{
    int i;
    for (i = 0; i < 40; i++) assert(nodes[i].refs == 0);
    for (i = 0; i < RECONVERT_MAX_PENDING; i++) assert(g_pending[i].c == NULL);
    assert(scheduled == 0);
    memset(&g_render_stats, 0, sizeof(g_render_stats));
    full_calls = paint_calls = inherited_calls = 0;
    for (i = 0; i < 40; i++) nodes[i].paint = 1;
    content.status = CONTENT_STATUS_DONE;
}
int main(void)
{
    int i;
    reset();
    mark(0, MACOS9_DOMMUT_SETATTR_STYLE);
    mark(0, MACOS9_DOMMUT_SETATTR_STYLE);
    mark(0, MACOS9_DOMMUT_SETATTR_STYLE);
    assert(nodes[0].refs == 1 && scheduled == 1);
    fire();
    assert(paint_calls == 1 && full_calls == 0);
    assert(g_render_stats.invalidations_deduped == 2);
    puts("A: same-node dedupe PASS");
    reset();
    for (i = 0; i < 10; i++) mark(i, MACOS9_DOMMUT_SETATTR_STYLE);
    assert(g_pending[0].count == 10 && scheduled == 1);
    fire();
    assert(paint_calls == 10 && full_calls == 0);
    puts("B: ten precise nodes PASS");
    reset();
    mark(0, MACOS9_DOMMUT_APPENDCHILD);
    mark(1, MACOS9_DOMMUT_SETATTR_STYLE);
    fire();
    assert(paint_calls == 1 && full_calls == 1);
    puts("C: structural plus paint, one fallback PASS");
    reset();
    inject = 1;
    mark(0, MACOS9_DOMMUT_SETATTR_STYLE);
    fire();
    assert(scheduled == 1 && g_pending[0].count == 2);
    fire();
    assert(paint_calls == 3 && g_render_stats.rerun_batches == 1);
    puts("D: processing writes, one follow-up PASS");
    reset();
    inject = 1;
    mark(0, MACOS9_DOMMUT_APPENDCHILD);
    fire(); fire();
    assert(full_calls == 1 && paint_calls == 2);
    puts("D2: fallback writes survive PASS");
    reset();
    mark(0, MACOS9_DOMMUT_SETATTR_STYLE);
    generation++;
    fire();
    assert(paint_calls == 0 && full_calls == 0);
    mark(0, MACOS9_DOMMUT_SETATTR_STYLE);
    document++;
    fire();
    assert(paint_calls == 0 && nodes[0].refs == 0);
    puts("E: registry and document generations retire refs PASS");
    reset();
    for (i = 0; i < 40; i++) mark(i, MACOS9_DOMMUT_SETATTR_STYLE);
    assert(g_pending[0].count == 32 && nodes[32].refs == 0);
    fire();
    assert(full_calls == 1 && g_render_stats.batch_overflow == 1);
    puts("Overflow: bounded refs, one fallback PASS");
    reset();
    mark(0, MACOS9_DOMMUT_SETATTR_STYLE);
    mark(0, MACOS9_DOMMUT_APPENDCHILD);
    assert(nodes[0].refs == 2);
    assert(macos9_reconvert_flush_now(&content) == 1);
    assert(full_calls == 1 && paint_calls == 1);
    fire();
    assert(full_calls == 1);
    puts("Geometry: shared processor, no duplicate callback work PASS");
    reset();
    refuse = 1;
    mark(0, MACOS9_DOMMUT_APPENDCHILD);
    fire();
    assert(scheduled == 1 && nodes[0].refs == 0);
    refuse = 0;
    fire();
    reset();
    puts("Busy: one retained fallback retry PASS");
    return 0;
}
