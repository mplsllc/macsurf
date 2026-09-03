/*
 * MacSurf  -  macsurf_trace.h
 *
 * MacSurf Trace: the compact universal event ring. Dev-mode only -- nothing is
 * recorded unless the ring is armed via `MSdg GET tracestart`. Every entry is
 * pointer-free integers: a timestamp, the live causal ids (pulled from
 * macsurf_diag's ambient scope), a category/event/state/reason tuple, and a
 * two-slot u32 payload (`a`/`b`) for category-specific values (old/new bits,
 * mutation count, job count, ...). No strings, no formatting, no log write on
 * the hot path.
 *
 * The structured durable models (documents / mutations / layout / paint) live
 * in macsurf_diag.c; this file is only the escape-hatch ring for anything not
 * yet given a dedicated MSdg verb.
 */

#ifndef MACSURF_TRACE_H
#define MACSURF_TRACE_H

enum ms_trace_cat {
	MS_TC_NAV = 0,
	MS_TC_DOC,
	MS_TC_SCRIPT,
	MS_TC_TASK,
	MS_TC_MUTATION,
	MS_TC_STYLE,
	MS_TC_LAYOUT,
	MS_TC_PAINT,
	MS_TC_ERROR,
	MS_TC__N
};

enum ms_trace_ev {
	MS_TE_NONE = 0,
	MS_TE_DOC_CREATE,
	MS_TE_DOC_DESTROY,
	MS_TE_MUTATION_BEGIN,
	MS_TE_MUTATION_MERGE,
	MS_TE_MUTATION_FREEZE,
	MS_TE_STYLEFAST_BEGIN,
	MS_TE_STYLEFAST_COMMIT,
	MS_TE_STYLEFAST_DECLINE,
	MS_TE_INHERITED_BEGIN,
	MS_TE_INHERITED_CANDIDATE,
	MS_TE_INHERITED_DECLINE,
	MS_TE_INHERITED_COMMIT,
	MS_TE_LAYOUT_BEGIN,
	MS_TE_LAYOUT_ASYNC,
	MS_TE_LAYOUT_DONE,
	MS_TE_LAYOUT_FAIL,
	MS_TE_PAINT_INVALIDATE,
	MS_TE_PAINT_BEGIN,
	MS_TE_PAINT_DONE
};

/* Arm/disarm. cat_mask is a bitmask of (1u << enum ms_trace_cat); pass 0 to
 * mean "all categories". level 0..4 is advisory verbosity (emitters may skip
 * fine-grained events below the armed level; v1 records everything >= 1). */
void macsurf_trace_arm(unsigned long cat_mask, int level);
void macsurf_trace_disarm(void);
int  macsurf_trace_armed(void);

/* Record one event. No-op (a single test + return) unless armed and the
 * category is in the mask. The causal id columns are read from macsurf_diag's
 * ambient scope at call time -- callers pass only the category-specific bits. */
void macsurf_trace_emit(int cat, int event, int state, int reason,
	unsigned long a, unsigned long b);

/* `MSdg GET trace`: newest-first dump of the ring (or a "disarmed" note). */
long macsurf_trace_serialize(char *buf, long cap);

#endif /* MACSURF_TRACE_H */
