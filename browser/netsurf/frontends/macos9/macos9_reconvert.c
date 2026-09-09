/* MacSurf: precise, bounded render batches. C89 / CW8.
 * Queued nodes own references; content pointers are registry-token checked.
 * The active snapshot is never merged with incoming mutations.
 */
#include <string.h>
#include "macos9.h"
#include "macsurf_debug.h"
#include "macsurf_diag.h"
#include "macos9_reconvert.h"
#include "content/content_protected.h"

/* MACSURF_RECONVERT_DISABLED remains an externally selectable A/B control. */
struct gui_window;
#define RECONVERT_DEBOUNCE_MS 400
/* One slot for every possible live registry content, including frames.
 * Dead generations are reclaimed before admitting a new document. */
#define RECONVERT_MAX_PENDING 256
#define RECONVERT_MAX_INVALIDATIONS 32

extern nserror macos9_schedule(int ms, void (*cb)(void *), void *p);
extern int html_reconvert_content(struct content *c);
extern int html_reconvert_fast_class(struct content *c, void *node);
extern int html_reconvert_fast_style(struct content *c, void *node);
extern int html_reconvert_fast_inherited_color(struct content *c, void *node);
extern int macsurf_html_has_droppable_inflight(struct content *c);
extern void *macsurf_reconvert_node_ref(void *node);
extern void macsurf_reconvert_node_unref(void *node);
#if defined(__MACOS9__) || defined(MACSURF_RENDER_BATCH_TEST) || \
    defined(MACSURF_RECONVERT_TEST_HOOK)
extern int macos9_content_is_live(struct content *c);
extern unsigned long macos9_content_token(struct content *c);
extern int macos9_content_token_valid(struct content *c, unsigned long token);
#else
#define macos9_content_is_live(c) (1)
#define macos9_content_token(c) (1UL)
#define macos9_content_token_valid(c, t) (1)
#endif
extern unsigned long html_content_get_doc_id(struct content *c);
extern unsigned long html_content_get_frame_id(struct content *c);
extern unsigned long content_get_nav_id(struct content *c);

struct macos9_render_invalidation {
    void *node;
    int kind;
};
struct macos9_reconvert_pending {
    struct content *c;
    unsigned long token;
    struct ms_diag_provenance prov;
    unsigned count;
    struct macos9_render_invalidation invalidation[RECONVERT_MAX_INVALIDATIONS];
    int overflow;
    int processing;
    int rerun_needed;
};
static struct macos9_reconvert_pending g_pending[RECONVERT_MAX_PENDING];
static int g_callback_queued;
static int g_processing;
static int g_reconvert_enabled = 1;
static long g_inherited_color_attempt;
static long g_inherited_color_commit;
static long g_inherited_color_fallback;
static struct macos9_render_stats g_render_stats;
static void macos9_reconvert_cb(void *p);

void macos9_reconvert_render_stats(struct macos9_render_stats *stats)
{
    *stats = g_render_stats;
}

static int macos9_reconvert_valid(struct macos9_reconvert_pending *b)
{
    return b->c != NULL && macos9_content_is_live(b->c) &&
        macos9_content_token_valid(b->c, b->token) &&
        html_content_get_doc_id(b->c) == b->prov.doc;
}

static void macos9_reconvert_release(struct macos9_reconvert_pending *b)
{
    unsigned j;
    for (j = 0; j < b->count; j++)
        macsurf_reconvert_node_unref(b->invalidation[j].node);
    memset(b, 0, sizeof(*b));
}

static void macos9_reconvert_queue(void)
{
    int i;
    if (g_callback_queued || g_processing) return;
    for (i = 0; i < RECONVERT_MAX_PENDING; i++) {
        if (g_pending[i].c != NULL) {
            if (macos9_schedule(RECONVERT_DEBOUNCE_MS,
                    macos9_reconvert_cb, NULL) == 0)
                g_callback_queued = 1;
            return;
        }
    }
}

int macos9_reconvert_pending_for(void *cv)
{
    int i;
    for (i = 0; i < RECONVERT_MAX_PENDING; i++) {
        if (g_pending[i].c == cv && cv != NULL) {
            if (macos9_reconvert_valid(&g_pending[i])) return 1;
            if (!g_pending[i].processing)
                macos9_reconvert_release(&g_pending[i]);
        }
    }
    return 0;
}

void macsurf_js_set_reconvert_enabled(int enabled)
{
    g_reconvert_enabled = enabled != 0;
}

void macos9_js_mark_dom_dirty(struct content *c)
{
    macos9_js_mark_dom_dirty_node(c, NULL, MACOS9_DOMMUT_UNKNOWN);
}

void macos9_js_mark_dom_dirty_node(struct content *c, void *node, int kind)
{
    struct macos9_reconvert_pending *b;
    int i, free_slot;
    unsigned j;
    g_render_stats.mutations_received++;
#ifdef MACSURF_RECONVERT_DISABLED
    return;
#endif
    if (!g_reconvert_enabled || c == NULL || !macos9_content_is_live(c))
        return;
    free_slot = -1;
    b = NULL;
    for (i = 0; i < RECONVERT_MAX_PENDING; i++) {
        if (g_pending[i].c != NULL && !g_pending[i].processing &&
                !macos9_reconvert_valid(&g_pending[i]))
            macos9_reconvert_release(&g_pending[i]);
        if (g_pending[i].c == c && macos9_reconvert_valid(&g_pending[i])) {
            b = &g_pending[i];
            break;
        }
        if (g_pending[i].c == NULL && free_slot < 0) free_slot = i;
    }
    /* The table has the registry's capacity: no registered live content
     * can exhaust it after stale slots are retired. Never substitute the
     * front window for an untracked document. */
    if (b == NULL) {
        if (free_slot < 0) return;
        b = &g_pending[free_slot];
        b->c = c;
        b->token = macos9_content_token(c);
        b->prov.nav = content_get_nav_id(c);
        b->prov.frame = html_content_get_frame_id(c);
        b->prov.doc = html_content_get_doc_id(c);
    }
    if (g_processing) {
        g_render_stats.mutations_during_processing++;
        if (!b->rerun_needed) {
            b->rerun_needed = 1;
            g_render_stats.rerun_batches++;
        }
    }
    if (b->prov.batch == 0) {
        b->prov.script = ms_diag_cur_script();
        b->prov.task = ms_diag_cur_task();
        b->prov.batch = ms_diag_batch_open(&b->prov);
        g_render_stats.batches_queued++;
    }
    ms_diag_batch_add(b->prov.batch, kind, ms_diag_cur_task());
    if (!b->overflow) {
        for (j = 0; j < b->count; j++) {
            if (b->invalidation[j].node == node &&
                    b->invalidation[j].kind == kind) {
                g_render_stats.invalidations_deduped++;
                break;
            }
        }
        if (j == b->count) {
            if (node == NULL || b->count == RECONVERT_MAX_INVALIDATIONS) {
                b->overflow = 1;
                if (node != NULL) g_render_stats.batch_overflow++;
                /* Existing exact records remain available for classification.
                 * No references are acquired beyond the bound. */
            } else {
                b->invalidation[j].node = macsurf_reconvert_node_ref(node);
                b->invalidation[j].kind = kind;
                b->count++;
            }
        }
    }
    macos9_reconvert_queue();
}

/* Detach ownership into a frozen stack snapshot. Reentrant marks see only
 * the empty pending half; never release or overwrite the active nodes. */
static int macos9_reconvert_process(int i)
{
    struct macos9_reconvert_pending active;
    struct macos9_reconvert_pending *pending = &g_pending[i];
    struct ms_diag_render_scope scope;
    unsigned j;
    int full, rc, class_result;
    if (!macos9_reconvert_valid(pending)) {
        macos9_reconvert_release(pending);
        return 0;
    }
#ifdef __MACOS9__
    /* Use the existing quiescent lifetime fence for synchronous callers too. */
    { extern int macos9_op_depth; macos9_op_depth++; }
#endif
    active = *pending;
    pending->count = 0;
    pending->overflow = 0;
    pending->prov.batch = 0;
    pending->processing = 1;
    pending->rerun_needed = 0;
    g_processing = 1;
    g_render_stats.batches_processed++;
    ms_diag_batch_freeze(active.prov.batch);
    (void)ms_diag_render_enter(&scope, MS_RENDER_RECONVERT, &active.prov);
    full = active.overflow;
    class_result = -2;
    for (j = 0; j < active.count; j++) {
        rc = -1;
        if (active.invalidation[j].kind == MACOS9_DOMMUT_SETATTR_STYLE) {
            rc = html_reconvert_fast_style(active.c, active.invalidation[j].node);
            if (rc == 0) g_render_stats.targeted_paint++;
            else {
                g_inherited_color_attempt++;
                rc = html_reconvert_fast_inherited_color(active.c,
                    active.invalidation[j].node);
                if (rc == 0) {
                    g_render_stats.targeted_inherited++;
                    g_inherited_color_commit++;
                } else g_inherited_color_fallback++;
            }
        }
        if (active.invalidation[j].kind == MACOS9_DOMMUT_SETATTR_CLASS) {
            if (class_result == -2)
                class_result = html_reconvert_fast_class(active.c,
                    active.invalidation[j].node);
            rc = class_result;
            if (rc == 0) g_render_stats.targeted_inherited++;
        }
        if (rc != 0) full = 1;
    }
    rc = 0;
    if (full) {
        g_render_stats.full_fallback++;
        rc = html_reconvert_content(active.c);
    }
    ms_diag_render_leave(&scope, rc == 0 ? MS_RRES_DONE : MS_RRES_QUEUED,
        MS_SREASON_NONE);
    pending->processing = 0;
    if (rc != 0) {
        /* A refused fallback did not consume its work. Keep a single
         * imprecise retry alongside any mutations received during the pass. */
        pending->overflow = 1;
        if (pending->prov.batch == 0) pending->prov = active.prov;
    }
    macos9_reconvert_release(&active);
    if (pending->count == 0 && !pending->overflow)
        macos9_reconvert_release(pending);
    g_processing = 0;
#ifdef __MACOS9__
    { extern int macos9_op_depth; macos9_op_depth--; }
#endif
    return rc;
}

static void macos9_reconvert_cb(void *p)
{
    int i;
    unsigned long batches[RECONVERT_MAX_PENDING];
    extern int macsurf_reconvert_in_progress;
    extern struct gui_window *macos9_paint_gw;
    (void)p;
    g_callback_queued = 0;
    if (g_processing) return;
    if (macsurf_reconvert_in_progress || macos9_paint_gw != NULL) {
        macos9_reconvert_queue();
        return;
    }
    /* Freeze the callback's membership too: new documents or batches
     * opened during a pass must wait for the one follow-up callback. */
    for (i = 0; i < RECONVERT_MAX_PENDING; i++)
        batches[i] = g_pending[i].prov.batch;
    for (i = 0; i < RECONVERT_MAX_PENDING; i++) {
        if (g_pending[i].c != NULL && batches[i] != 0 &&
                batches[i] == g_pending[i].prov.batch)
            (void)macos9_reconvert_process(i);
    }
    macos9_reconvert_queue();
}

static long g_sync_flushes      = 0;	/* flushes actually run, this nav  */
static long g_sync_declined     = 0;	/* asked, refused (guard or budget) */
static long g_sync_us           = 0;	/* cumulative cost of the flushes   */

/* fixes1075 - WHY a flush was refused, because the first hardware log of
 * fixes1073 reported declined=660 flush=0 on hackaday and the instrument could
 * not say which guard was firing. A refusal count with no reason attached is
 * the same shape of unhelpful as `js=25s` with no compile/run split. */
static long g_sync_r_notdone  = 0;	/* content not CONTENT_STATUS_DONE   */
static long g_sync_r_active   = 0;	/* sub-resource fetches still in air */
static long g_sync_r_paint    = 0;	/* a redraw is walking the box tree  */
static long g_sync_r_inprog   = 0;	/* reconvert already in flight       */
static long g_sync_r_budget   = 0;	/* time budget for this nav spent    */
static long g_sync_r_busy     = 0;	/* html_reconvert refused, other     */

/* fixes1075 - budget by TIME, not by count.
 *
 * fixes1073 allowed 24 forced layouts per navigation on an estimate of ~100ms
 * each. Hardware measured 1.08s each (the Jetpack comment iframe spent 4.34s on
 * four of them), which makes that ceiling a 26-second worst case -- exactly the
 * regression this whole effort is trying not to cause.
 *
 * A count cannot bound a cost whose per-unit price is unknown and varies with
 * page size. Cumulative microseconds can, and it degrades where it should: a
 * page gets as many reflows as fit in the budget, cheap ones get more of them,
 * and once spent geometry falls back to `undefined` and JSSYNC says so.
 *
 * fixes1126 (#265) - 2s was an order of magnitude too tight. Hardware on
 * hackaday: the first TWO flushes cost 3.27s, so the budget was spent before
 * slick finished its opening measure burst and every later measurement
 * declined (budget=703 of 1240, flush=2). The per-flush price on that page is
 * ~1.6s (the reconvert's O(document) rebuild) -- that is the number to budget
 * against: 30s buys ~18 real reflows per navigation, enough for a widget's
 * whole init measure/mutate loop, while a genuinely pathological layout-thrash
 * page is still bounded and stays user-abortable: the JS interrupt handler
 * polls WaitNextEvent for Cmd-. every ~200ms between bytecodes, i.e. between
 * flushes (qjs_interrupt_handler), and the per-navigation reset bounds it to
 * one navigation's worth of cost. */
/* fixes1133 - raised from 30s to 120s. Hardware measured 19 priority flushes
 * consuming 31s on hackaday (1.6s each); the budget was spent before slick's
 * init ran, producing 1134 declines and stranding the measure/mutate cycle.
 * 120s buys ~75 flushes - enough headroom for any real page without removing
 * the safety valve entirely. Settle-once still limits to one flush per burst. */
#define MACOS9_SYNC_BUDGET_US 120000000L

void
macos9_reconvert_sync_stats(long *flushes, long *declined, long *us)
{
	if (flushes != NULL)  *flushes  = g_sync_flushes;
	if (declined != NULL) *declined = g_sync_declined;
	if (us != NULL)       *us       = g_sync_us;
}

/* fixes1075 - the per-reason breakdown behind `declined`. See the counters'
 * declarations for what each one means and which of them would change the
 * next round's target. */
void
macos9_reconvert_sync_reasons(long *notdone, long *active, long *paint,
		long *inprog, long *budget, long *busy)
{
	if (notdone != NULL) *notdone = g_sync_r_notdone;
	if (active != NULL)  *active  = g_sync_r_active;
	if (paint != NULL)   *paint   = g_sync_r_paint;
	if (inprog != NULL)  *inprog  = g_sync_r_inprog;
	if (budget != NULL)  *budget  = g_sync_r_budget;
	if (busy != NULL)    *busy    = g_sync_r_busy;
}

void
macos9_reconvert_sync_reset(void)
{
	/* Navigation summary: unlike the mutation census (per batch), these
	 * counters are intentionally accumulated until the JS navigation reset. */
	if (g_inherited_color_attempt != 0) {
		macsurf_debug_log_writef(
			"LIFE INHERITEDCOLOR attempt=%ld commit=%ld fallback=%ld",
			g_inherited_color_attempt, g_inherited_color_commit,
			g_inherited_color_fallback);
	}
	g_inherited_color_attempt = 0;
	g_inherited_color_commit = 0;
	g_inherited_color_fallback = 0;
	g_sync_flushes  = 0;
	g_sync_declined = 0;
	g_sync_us       = 0;
	g_sync_r_notdone = 0; g_sync_r_active = 0; g_sync_r_paint = 0;
	g_sync_r_inprog = 0;  g_sync_r_budget = 0; g_sync_r_busy = 0;

}


int
macos9_reconvert_flush_now(void *cv)
{
	static int in_flush = 0;
	extern int macsurf_reconvert_in_progress;
	extern struct gui_window *macos9_paint_gw;
	extern double macos9_micros(void);
	struct content *c = (struct content *) cv;
	double t0;
	int i;
	int rc;

	if (c == NULL)
		return 0;
	/* Nothing dirty: the box tree already answers for the current DOM. */
	if (!macos9_reconvert_pending_for(cv))
		return 0;

	/* fixes1075 - attribute the refusal. Ordered most-specific first so the
	 * counter names the ACTUAL blocker rather than whichever guard happens
	 * to be listed earliest. */
	if (in_flush || g_processing || macsurf_reconvert_in_progress) {
		/* A reconvert is on the stack or mid-flight. No tree is safe to
		 * read -- the old one is being torn down -- so the answer is
		 * undefined, and there is no way to "wait" for it here: the
		 * flush is synchronous and JS cannot yield mid-read. The work
		 * is not stranded though: the in-flight reconvert will clear it,
		 * and the retry catches the case where it cannot. */
		g_sync_r_inprog++; g_sync_declined++;
		macos9_reconvert_queue();
		return 0;
	}
	if (macos9_paint_gw != NULL) {
		/* A redraw is walking the box tree; a flush will be safe on the
		 * next event-loop pass, so retry then. */
		g_sync_r_paint++; g_sync_declined++;
		macos9_reconvert_queue();
		return 0;
	}
	if (!g_reconvert_enabled || !macos9_content_is_live(c)) {
		g_sync_r_busy++; g_sync_declined++; return 0;
	}
	/* These two are html_reconvert's own preconditions, checked here only so
	 * the refusal can be NAMED. html_reconvert enforces them regardless.
	 *   notdone: the document has not finished loading. Script init -- which
	 *     is when widgets measure -- runs inside this window, so if this is
	 *     the dominant reason then geometry is dead exactly when it matters
	 *     and the gate itself is the next problem, not the budget.
	 *   active: sub-resource fetches still in flight. Rebuilding now would
	 *     free an object list that html_object_callback still points into
	 *     (the fixes421 use-after-free). */
	/* fixes1094 (#265 Round B) - mirror html_reconvert's relaxed
	 * preconditions. These are screened here ONLY so a refusal can be named;
	 * html_reconvert enforces them regardless, so if the two drift this side
	 * silently declines work the other would have accepted. That is exactly
	 * what hid the problem before: hardware read notdone=630 of 630 declines
	 * with flush=0, because this DONE test ran first and the guard below was
	 * never even evaluated.
	 *
	 * READY (not DONE) is the real precondition -- READY is when the first
	 * box tree exists -- and the active-fetch hazard is now the narrow
	 * "in-flight entry the reconvert would FREE", not "any fetch at all".
	 * See the fixes1094 comments in html_reconvert. */
	/* fixes1096 (#265 Round C3) - LOADING too. Mirrors html_reconvert; see
	 * the safety argument there. This is the window `notdone` was counting
	 * (565 of 1247 declines on hardware) and the one the featured slider
	 * measures in. */
	if (c->status != CONTENT_STATUS_LOADING &&
	    c->status != CONTENT_STATUS_READY &&
	    c->status != CONTENT_STATUS_DONE) {
		g_sync_r_notdone++; g_sync_declined++; return 0;
	}
	if (c->active > 0 && macsurf_html_has_droppable_inflight(c)) {
		/* Transient: the in-flight entry a rebuild would free completes
		 * shortly. Retry on the next pass. */
		g_sync_r_active++; g_sync_declined++;
		macos9_reconvert_queue();
		return 0;
	}
	if (g_sync_us >= MACOS9_SYNC_BUDGET_US) {
		g_sync_r_budget++; g_sync_declined++; return 0;
	}


    in_flush = 1;
    t0 = macos9_micros();
    rc = 0;
    for (i = 0; i < RECONVERT_MAX_PENDING; i++) {
        if (g_pending[i].c == c) {
            rc = macos9_reconvert_process(i);
            break;
        }
    }
    in_flush = 0;
    macos9_reconvert_queue();
    if (rc != 0) {
        g_sync_r_busy++;
        g_sync_declined++;
        return 0;
    }
    g_sync_us += (long)(macos9_micros() - t0);
    g_sync_flushes++;
    /* A reentrant mutation leaves geometry unsettled until its batch runs. */
    return !macos9_reconvert_pending_for(cv);
}
