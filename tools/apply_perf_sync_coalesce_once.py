#!/usr/bin/env python3
from pathlib import Path

qjs_path = Path('browser/netsurf/frontends/macos9/javascript/macsurf_qjs.c')
reconv_path = Path('browser/netsurf/frontends/macos9/macos9_reconvert.c')

qjs = qjs_path.read_text()
old = ''' * Content-keyed (iframes have their own content): a settle in one
 * runtime must not silence flushes for another. A DECLINED flush leaves
 * the flag 0 so the next read retries -- today's retry semantics are
 * preserved. qjs_geometry_settled() still independently gates every
 * read on tree stability/liveness. */
static int g_geom_settled = 0;
static void *g_geom_settled_c = NULL;

static void qjs_geom_settle_begin(void)
{
\tg_geom_settled = 0;
\tg_geom_settled_c = NULL;
}
'''
new = ''' * Content-keyed (iframes have their own content): a settle in one
 * runtime must not silence flushes for another.
 *
 * performance: a DECLINED forced flush is now also latched for the rest of
 * this JS execution. MacSurf is cooperatively scheduled, so every transient
 * reason a forced flush can decline (paint on stack, reconvert in progress,
 * active fetch hazard, conversion/layout busy) cannot clear until JS yields.
 * Retrying the same barrier on the next geometry getter in the SAME callback
 * can therefore never make progress; Hackaday measured 725 such declined
 * attempts. The first decline already schedules the existing 80ms retry when
 * appropriate. At the next JS execution boundary qjs_geom_settle_begin()
 * clears this latch and a fresh geometry read may try again.
 *
 * qjs_geometry_settled() still independently gates every read on tree
 * stability/liveness, so a latched decline returns the same safe undefined
 * answer rather than exposing stale boxes. */
static int g_geom_settled = 0;
static void *g_geom_settled_c = NULL;
static int g_geom_attempted = 0;
static void *g_geom_attempted_c = NULL;

static void qjs_geom_settle_begin(void)
{
\tg_geom_settled = 0;
\tg_geom_settled_c = NULL;
\tg_geom_attempted = 0;
\tg_geom_attempted_c = NULL;
}
'''
if old not in qjs:
    raise SystemExit('qjs settle block anchor not found')
qjs = qjs.replace(old, new, 1)

old = '''\tif (g_geom_settled && g_geom_settled_c == (void *) content)
\t\treturn;

\t/* Nothing pending: the box tree already answers for the current DOM.
\t * Settle without even paying for the flush call. */
\tif (!macos9_reconvert_pending_for(content)) {
\t\tg_geom_settled = 1;
\t\tg_geom_settled_c = (void *) content;
\t\treturn;
\t}

\t/* Pending marks: flush synchronously (the fixes1073 forced reflow).
\t * Return 1 means a flush ran and consumed this content's slots --
\t * settled. Return 0 is a DECLINED flush (budget, in-progress, ...):
\t * leave the flag 0 so the next read retries, preserving today's
\t * retry semantics. */
\tt0 = macos9_micros();
\tif (macos9_reconvert_flush_now((void *) content)) {
\t\tg_geom_settled = 1;
\t\tg_geom_settled_c = (void *) content;
\t}
\tg_geom_us += (long)(macos9_micros() - t0);
'''
new = '''\tif (g_geom_settled && g_geom_settled_c == (void *) content)
\t\treturn;

\t/* A flush attempt already declined in this SAME JS execution. None of
\t * macos9_reconvert_flush_now's transient blockers can clear until the
\t * cooperative event loop gets control back, so do not hammer the same
\t * guard/scheduler path on every geometry getter. qjs_geometry_settled()
\t * will still see the pending mutation and return the safe undefined. */
\tif (g_geom_attempted && g_geom_attempted_c == (void *) content)
\t\treturn;

\t/* Nothing pending: the box tree already answers for the current DOM.
\t * Settle without even paying for the flush call. */
\tif (!macos9_reconvert_pending_for(content)) {
\t\tg_geom_settled = 1;
\t\tg_geom_settled_c = (void *) content;
\t\treturn;
\t}

\t/* Pending marks: flush synchronously (the fixes1073 forced reflow).
\t * One attempt per content per JS execution is sufficient: if it declines,
\t * the existing retry is queued for after JS yields; if it succeeds, the
\t * settled flag below serves every remaining read in this execution. */
\tg_geom_attempted = 1;
\tg_geom_attempted_c = (void *) content;
\tt0 = macos9_micros();
\tif (macos9_reconvert_flush_now((void *) content)) {
\t\tg_geom_settled = 1;
\t\tg_geom_settled_c = (void *) content;
\t}
\tg_geom_us += (long)(macos9_micros() - t0);
'''
if old not in qjs:
    raise SystemExit('qjs geometry flush anchor not found')
qjs = qjs.replace(old, new, 1)
qjs_path.write_text(qjs)

reconv = reconv_path.read_text()
old = '''/* core re-convert trigger: 0 = NSERROR_OK (queued), non-zero = busy/skip. */
extern int html_reconvert_content(struct content *c);
/* fixes1094 (#265 Round B) - see html.c. */
'''
new = '''/* core re-convert trigger: 0 = NSERROR_OK (queued), non-zero = busy/skip. */
extern int html_reconvert_content(struct content *c);
/* The asynchronous callback already uses these proven transactions for a
 * precise inline-style mutation. The synchronous geometry path must use the
 * same cheap proof before paying for a whole-document rebuild. */
extern int html_reconvert_fast_style(struct content *c, void *node);
extern int html_reconvert_fast_inherited_color(struct content *c, void *node);
/* fixes1094 (#265 Round B) - see html.c. */
'''
if old not in reconv:
    raise SystemExit('reconvert extern anchor not found')
reconv = reconv.replace(old, new, 1)

old = '''\t/* MacSurf Trace 1c: this synchronous flush IS a render transaction.
\t * Build the frozen descriptor from c's pending slot (R3). */
\tfor (i = 0; i < RECONVERT_MAX_PENDING; i++) {
\t\tif (g_pending[i].c == c) {
\t\t\tstruct ms_diag_provenance ms_prov;
\t\t\tms_prov.nav = g_pending[i].nav;
\t\t\tms_prov.frame = g_pending[i].frame;
\t\t\tms_prov.doc = g_pending[i].doc;
\t\t\tms_prov.script = g_pending[i].script;
\t\t\tms_prov.task = g_pending[i].task;
\t\t\tms_prov.batch = g_pending[i].batch_id;
\t\t\tms_prov.pass = 0;
\t\t\tms_diag_batch_freeze(g_pending[i].batch_id);
\t\t\t(void) ms_diag_render_enter(&ms_rs, MS_RENDER_RECONVERT,
\t\t\t\t&ms_prov);
\t\t\tms_rs_open = 1;
\t\t\tbreak;
\t\t}
\t}

\tin_flush = 1;
\tt0 = macos9_micros();
\trc = html_reconvert_content(c);\t/* SYNCHRONOUS -- see fixes903 */
\tin_flush = 0;

\tif (rc != 0) {
'''
new = '''\t/* MacSurf Trace 1c: this synchronous flush IS a render transaction.
\t * Build the frozen descriptor from c's pending slot (R3). */
\tfor (i = 0; i < RECONVERT_MAX_PENDING; i++) {
\t\tif (g_pending[i].c == c) {
\t\t\tstruct ms_diag_provenance ms_prov;
\t\t\tms_prov.nav = g_pending[i].nav;
\t\t\tms_prov.frame = g_pending[i].frame;
\t\t\tms_prov.doc = g_pending[i].doc;
\t\t\tms_prov.script = g_pending[i].script;
\t\t\tms_prov.task = g_pending[i].task;
\t\t\tms_prov.batch = g_pending[i].batch_id;
\t\t\tms_prov.pass = 0;
\t\t\tms_diag_batch_freeze(g_pending[i].batch_id);
\t\t\t(void) ms_diag_render_enter(&ms_rs, MS_RENDER_RECONVERT,
\t\t\t\t&ms_prov);
\t\t\tms_rs_open = 1;
\t\t\tbreak;
\t\t}
\t}

\t/* performance: forced geometry used to bypass the exact style fast paths
\t * used by macos9_reconvert_cb and always buy a full dom_to_box rebuild.
\t * For one precise inline-style mutation, first run the same transactions:
\t * html_reconvert_fast_style commits only paint-only changes, and the
\t * inherited-colour transaction commits only when its whole affected
\t * subtree proves geometry/topology stable. A commit therefore satisfies
\t * the geometry barrier without rebuilding boxes. Any uncertainty falls
\t * straight through to the proven full synchronous reconvert below. */
\tif (i < RECONVERT_MAX_PENDING &&
\t    g_pending[i].multi == 0 &&
\t    g_pending[i].kind == MACOS9_DOMMUT_SETATTR_STYLE &&
\t    g_pending[i].node != NULL &&
\t    (c->status == CONTENT_STATUS_READY ||
\t     c->status == CONTENT_STATUS_DONE)) {
\t\tt0 = macos9_micros();
\t\tin_flush = 1;
\t\tg_style_fast_attempt++;
\t\trc = html_reconvert_fast_style(c, g_pending[i].node);
\t\tms_diag_render_stage(MS_STAGE_STYLEFAST,
\t\t\trc == 0 ? MS_SRES_COMMIT : MS_SRES_FALLBACK,
\t\t\tMS_SREASON_NONE, 0, 0, 0, (const char *) 0, 0);
\t\tif (rc == 0) {
\t\t\tg_style_fast_commit++;
\t\t} else {
\t\t\tg_style_fast_fallback++;
\t\t\tg_inherited_color_attempt++;
\t\t\trc = html_reconvert_fast_inherited_color(c,
\t\t\t\tg_pending[i].node);
\t\t\tif (rc == 0)
\t\t\t\tg_inherited_color_commit++;
\t\t\telse
\t\t\t\tg_inherited_color_fallback++;
\t\t}
\t\tin_flush = 0;
\t\tif (rc == 0) {
\t\t\tg_sync_us += (long)(macos9_micros() - t0);
\t\t\tg_sync_flushes++;
\t\t\tif (ms_rs_open) {
\t\t\t\tms_diag_render_leave(&ms_rs, MS_RRES_DONE,
\t\t\t\t\tMS_SREASON_NONE);
\t\t\t\tms_rs_open = 0;
\t\t\t}
\t\t\tfor (i = 0; i < RECONVERT_MAX_PENDING; i++) {
\t\t\t\tif (g_pending[i].c == c)
\t\t\t\t\tmacos9_reconvert_slot_clear(i);
\t\t\t}
\t\t\tif (g_pending_overflow == 0 &&
\t\t\t    macos9_reconvert_pending_count() == 0) {
\t\t\t\tg_first_mark_tick = 0;
\t\t\t\tg_defer_reported = 0;
\t\t\t}
\t\t\tg_last_reconvert_tick = (unsigned long) TickCount();
\t\t\treturn 1;
\t\t}
\t}

\tin_flush = 1;
\tt0 = macos9_micros();
\trc = html_reconvert_content(c);\t/* SYNCHRONOUS -- see fixes903 */
\tin_flush = 0;

\tif (rc != 0) {
'''
if old not in reconv:
    raise SystemExit('sync render anchor not found')
reconv = reconv.replace(old, new, 1)
reconv_path.write_text(reconv)
print('patched qjs geometry decline coalescing + sync style fast path')
