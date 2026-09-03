/*
 * MacSurf CSS Transitions - Round 2B-1 generic bounded effect engine
 * Synthetic numeric tests only; no real opacity rendering yet
 */

#ifdef __MACOS9__
#include "macos9.h"
#else
#include <stdlib.h>
#include <string.h>
#endif
#include "macos9_transition.h"

#ifdef __MACOS9__
#include <Timer.h>
#else
extern uint32_t macsurf_transition_test_now;
#endif
#include <libcss/fpmath.h>
#include "utils/ns_errors.h"
extern nserror macos9_schedule(int t, void (*callback)(void *p), void *p);

static struct macsurf_transition_effect g_effects[MACSURF_TRANSITION_MAX_ACTIVE];
static int g_effect_count = 0;
static bool g_sched_armed = false;
#ifdef __MACOS9__
extern void macsurf_debug_log_writef(const char *fmt, ...);
static int g_transition_diag_budget = 96;
#define TRANSITION_DIAG(args) \
    do { \
        if (g_transition_diag_budget > 0) { \
            g_transition_diag_budget--; \
            macsurf_debug_log_writef args; \
        } \
    } while (0)
#endif

int macsurf_transition_fixed_to_ticks(css_fixed v)
{
    /* v is Q22.10 seconds; 1 tick = 1/60s.
     * ticks = v *60 /1024  (seconds_fixed * ticks_per_sec)
     * Deterministic round-to-nearest: ((frac*60 +512)>>10)
     * Overflow-safe without long long: divide-first.
     * No clamping here; caller clamps for duration if needed. */
    int is_neg = 0;
    css_fixed av;
    int whole;
    int frac;
    int ticks;
    if (v == 0) return 0;
    is_neg = (v < 0);
    av = is_neg ? -v : v;
    whole = av >> 10;
    frac = av & 1023;
    ticks = whole * 60 + ((frac * 60 + 512) >> 10);
    if (is_neg) ticks = -ticks;
    return ticks;
}

css_fixed macsurf_transition_ease_linear(css_fixed t)
{
    return t;
}

/* Scheduler semantics: 16ms is NOT transition time.
 * Transition progress is derived solely from TickCount() elapsed
 * (now - start) vs delay/duration. The scheduler merely decides
 * when to wake and recompute presentation; paint cadence is
 * whatever the machine sustains. */
static css_fixed apply_timing(css_transition_timing_entry *timing, css_fixed progress)
{
    /* progress is Q22.10 0..1024 */
    double x;
    double x1 = 0;
    double y1 = 0;
    double x2 = 0;
    double y2 = 0;
    double lo;
    double hi;
    double mid;
    double xm;
    double ym;
    int iter;
    if (timing == NULL) return progress;
    if (timing->type == CSS_TIMING_LINEAR) {
        return progress;
    }
    if (timing->type == CSS_TIMING_EASE) {
        x1 = 0.25; y1 = 0.1; x2 = 0.25; y2 = 1.0;
    } else if (timing->type == CSS_TIMING_EASE_IN) {
        x1 = 0.42; y1 = 0.0; x2 = 1.0; y2 = 1.0;
    } else if (timing->type == CSS_TIMING_EASE_OUT) {
        x1 = 0.0; y1 = 0.0; x2 = 0.58; y2 = 1.0;
    } else if (timing->type == CSS_TIMING_EASE_IN_OUT) {
        x1 = 0.42; y1 = 0.0; x2 = 0.58; y2 = 1.0;
    } else if (timing->type == CSS_TIMING_CUBIC_BEZIER) {
        x1 = (double)timing->x1 / 65536.0;
        y1 = (double)timing->y1 / 65536.0;
        x2 = (double)timing->x2 / 65536.0;
        y2 = (double)timing->y2 / 65536.0;
    } else if (timing->type == CSS_TIMING_STEPS) {
        int steps = (int)timing->step_count;
        int pos = (int)timing->step_pos;
        long prog = (long)progress;
        if (steps <= 0) return progress;
        if (pos == 0) {
            long current_step = (prog * steps) / 1024;
            if (current_step >= steps) current_step = steps - 1;
            return (css_fixed)((current_step * 1024) / steps);
        } else if (pos == 1) {
            long current_step = (prog * steps) / 1024;
            if (current_step >= steps) return 1024;
            return (css_fixed)(((current_step) * 1024) / steps);
        } else if (pos == 2) {
            long current_step = (prog * steps) / 1024;
            if (current_step == 0) return 0;
            if (current_step >= steps) return 1024;
            return (css_fixed)(((current_step - 1) * 1024) / (steps - 1));
        } else {
            long current_step = (prog * steps) / 1024;
            if (current_step >= steps) return 1024;
            return (css_fixed)(((current_step + 1) * 1024) / (steps + 1));
        }
    } else {
        return progress;
    }
    /* cubic-bezier solver: binary search 12 iterations, bounded */
    x = (double)progress / 1024.0;
    if (x <= 0.0) return 0;
    if (x >= 1.0) return 1024;
    lo = 0.0; hi = 1.0;
    for (iter = 0; iter < 12; iter++) {
        mid = (lo + hi) * 0.5;
        /* x(u) = 3*(1-u)^2*u*x1 + 3*(1-u)*u^2*x2 + u^3 */
        {
            double u = mid;
            double om = 1.0 - u;
            xm = 3.0 * om * om * u * x1 + 3.0 * om * u * u * x2 + u * u * u;
        }
        if (xm < x) lo = mid; else hi = mid;
    }
    /* y(u) at solved u */
    {
        double u = (lo + hi) * 0.5;
        double om = 1.0 - u;
        ym = 3.0 * om * om * u * y1 + 3.0 * om * u * u * y2 + u * u * u;
    }
    if (ym < 0.0) ym = 0.0;
    if (ym > 1.0) {
        /* allow overshoot for y outside 0..1 per spec? Clamp? For now allow
         * but spec says y may be -0.5..1.5, so don't clamp overshoot. */
    }
    return (css_fixed)(ym * 1024.0 + 0.5);
}

void macsurf_transition_init(void)
{
    int i;
    for (i = 0; i < MACSURF_TRANSITION_MAX_ACTIVE; i++) {
        g_effects[i].in_use = false;
        g_effects[i].node = NULL;
        g_effects[i].diag_milestones = 0;
    }
    g_effect_count = 0;
    g_sched_armed = false;
}

void macsurf_transition_reset(void)
{
    int i;
    for (i = 0; i < MACSURF_TRANSITION_MAX_ACTIVE; i++) {
        if (g_effects[i].in_use && g_effects[i].node != NULL) {
            dom_node_unref(g_effects[i].node);
            g_effects[i].node = NULL;
        }
        g_effects[i].in_use = false;
        g_effects[i].diag_milestones = 0;
    }
    g_effect_count = 0;
    g_sched_armed = false;
    /* cancel any scheduled tick */
    macos9_schedule(-1, macsurf_transition_tick, NULL);
}

int macsurf_transition_active_count(void)
{
    return g_effect_count;
}

bool macsurf_transition_has_active(void)
{
    return g_effect_count > 0;
}

bool macsurf_transition_scheduler_active(void)
{
    return g_sched_armed;
}

static int find_free_slot(void)
{
    int i;
    for (i = 0; i < MACSURF_TRANSITION_MAX_ACTIVE; i++) {
        if (!g_effects[i].in_use) return i;
    }
    return -1;
}

static int find_effect(dom_node *node, uint32_t prop)
{
    int i;
    for (i = 0; i < MACSURF_TRANSITION_MAX_ACTIVE; i++) {
        if (g_effects[i].in_use && g_effects[i].node == node && g_effects[i].prop == prop)
            return i;
    }
    return -1;
}

static void schedule_tick_if_needed(void)
{
    if (g_effect_count > 0 && !g_sched_armed) {
        macos9_schedule(MACSURF_TRANSITION_TICK_MS, macsurf_transition_tick, NULL);
        g_sched_armed = true;
    } else if (g_effect_count == 0 && g_sched_armed) {
        macos9_schedule(-1, macsurf_transition_tick, NULL);
        g_sched_armed = false;
    }
}

int macsurf_transition_create(dom_node *node, uint32_t prop,
        css_fixed start_value, css_fixed target_value,
        css_fixed delay, css_fixed duration,
        css_transition_timing_entry timing,
        uint32_t now_tick,
        dom_node *initial_style_anchor)
{
    int slot;
    struct macsurf_transition_effect *e;

    /* initial style must not transition */
    if (initial_style_anchor == NULL) {
        return 0;
    }
    /* target change required */
    if (start_value == target_value) {
        return 0;
    }
    /* duration zero: immediate, no effect */
    if (duration == 0) {
        return 0;
    }
    /* node must be valid */
    if (node == NULL) return 0;

    /* replace existing effect for same node+prop */
    slot = find_effect(node, prop);
    if (slot >= 0) {
        e = &g_effects[slot];
        /* sample current presentation as new start if active */
        if (e->state == MACSURF_TRANSITION_ACTIVE || e->state == MACSURF_TRANSITION_DELAY) {
            css_fixed cur;
            if (macsurf_transition_get_presented(node, prop, &cur, now_tick)) {
                start_value = cur;
            }
        }
        /* reuse slot: unref old node, keep slot */
        /* node is same, so no ref change */
        e->start_tick = now_tick;
        e->delay = delay;
        e->duration = duration;
        e->timing = timing;
        e->start_value = start_value;
        e->target_value = target_value;
        e->diag_milestones = 0;
        /* determine initial state: check delay */
        {
            int delay_ticks = macsurf_transition_fixed_to_ticks(delay);
            int duration_ticks = macsurf_transition_fixed_to_ticks(duration);
            long elapsed;
            long effective;
            if (duration != 0 && duration_ticks == 0) duration_ticks = 1;
            elapsed = 0;
            effective = 0 - delay_ticks;
            if (effective < 0) e->state = MACSURF_TRANSITION_DELAY;
            else if (effective >= duration_ticks) e->state = MACSURF_TRANSITION_COMPLETE;
            else e->state = MACSURF_TRANSITION_ACTIVE;
            if (e->state == MACSURF_TRANSITION_COMPLETE) {
                /* already at end: retire immediately */
                dom_node_unref(e->node);
                e->in_use = false;
                e->node = NULL;
                g_effect_count--;
                schedule_tick_if_needed();
                return 0;
            }
        }
        schedule_tick_if_needed();
        return 1;
    }

    /* new effect */
    slot = find_free_slot();
    if (slot < 0) {
        /* capacity exhausted: fail safely, render target */
        return 0;
    }
    e = &g_effects[slot];
    e->in_use = true;
    e->node = node;
    dom_node_ref(node);
    e->prop = prop;
    e->start_tick = now_tick;
    e->delay = delay;
    e->duration = duration;
    e->timing = timing;
    e->start_value = start_value;
    e->target_value = target_value;
    e->diag_milestones = 0;
    {
        int delay_ticks = macsurf_transition_fixed_to_ticks(delay);
        int duration_ticks = macsurf_transition_fixed_to_ticks(duration);
        long effective;
            if (duration != 0 && duration_ticks == 0) duration_ticks = 1;
        effective = 0 - delay_ticks;
        if (effective < 0) e->state = MACSURF_TRANSITION_DELAY;
        else if (effective >= duration_ticks) e->state = MACSURF_TRANSITION_COMPLETE;
        else e->state = MACSURF_TRANSITION_ACTIVE;
        if (e->state == MACSURF_TRANSITION_COMPLETE) {
            dom_node_unref(e->node);
            e->in_use = false;
            e->node = NULL;
            return 0;
        }
    }
    g_effect_count++;
    schedule_tick_if_needed();
    return 1;
}

bool macsurf_transition_get_presented(dom_node *node, uint32_t prop,
        css_fixed *out_value, uint32_t now_tick)
{
    int idx;
    struct macsurf_transition_effect *e;
    uint32_t elapsed;
    int delay_ticks;
    int duration_ticks;
    long effective;
    css_fixed progress;
    css_fixed eased;
    css_fixed delta;

    if (node == NULL || out_value == NULL) return false;
    idx = find_effect(node, prop);
    if (idx < 0) return false;
    e = &g_effects[idx];
    if (!e->in_use) return false;

    elapsed = macsurf_transition_elapsed(now_tick, e->start_tick);
    delay_ticks = macsurf_transition_fixed_to_ticks(e->delay);
    duration_ticks = macsurf_transition_fixed_to_ticks(e->duration);
            if (e->duration != 0 && duration_ticks == 0) duration_ticks = 1;
    if (duration_ticks <= 0) {
        *out_value = e->target_value;
        return true;
    }
    effective = (long)elapsed - delay_ticks;
    if (effective < 0) {
        /* still in delay: at start */
        *out_value = e->start_value;
        e->state = MACSURF_TRANSITION_DELAY;
        return true;
    }
    if (effective >= duration_ticks) {
        *out_value = e->target_value;
        e->state = MACSURF_TRANSITION_COMPLETE;
        return true;
    }
    e->state = MACSURF_TRANSITION_ACTIVE;
    /* progress 0..1024 */
    progress = (css_fixed)((effective * 1024) / duration_ticks);
    eased = apply_timing(&e->timing, progress);
    /* interpolate: start + (target-start)*eased/1024 */
    delta = e->target_value - e->start_value;
    {
        long prod = (long)delta * (long)eased;
        *out_value = e->start_value + (css_fixed)(prod / 1024);
    }
    return true;
}

void macsurf_transition_tick(void *p)
{
    int i;
    int still_active = 0;
    uint32_t now;
#ifndef __MACOS9__
    extern uint32_t macsurf_transition_test_now;
#endif
    (void)p;
#ifdef __MACOS9__
    now = (uint32_t)TickCount();
#else
    now = macsurf_transition_test_now;
#endif
    for (i = 0; i < MACSURF_TRANSITION_MAX_ACTIVE; i++) {
        if (!g_effects[i].in_use) continue;
        /* check if complete */
        {
            uint32_t elapsed;
            int delay_ticks;
            int duration_ticks;
            long effective;
            elapsed = macsurf_transition_elapsed(now, g_effects[i].start_tick);
            delay_ticks = macsurf_transition_fixed_to_ticks(g_effects[i].delay);
            duration_ticks = macsurf_transition_fixed_to_ticks(g_effects[i].duration);
            if (g_effects[i].duration != 0 && duration_ticks == 0) duration_ticks = 1;
            effective = (long)elapsed - delay_ticks;
            if (effective >= duration_ticks) {
#ifdef __MACOS9__
                TRANSITION_DIAG(("LIFE 2B2 effect complete node=%p now=%ld start=%ld target=%ld active=%d",
                    (void *)g_effects[i].node, (long)now,
                    (long)g_effects[i].start_value,
                    (long)g_effects[i].target_value, g_effect_count));
#endif
                /* complete: retire */
                dom_node_unref(g_effects[i].node);
                g_effects[i].node = NULL;
                g_effects[i].in_use = false;
                g_effect_count--;
                continue;
            }
            still_active = 1;
#ifdef __MACOS9__
            if (g_effects[i].prop == CSS_PROP_OPACITY && duration_ticks > 0) {
                int milestone = (int)(((long)effective * 4) / duration_ticks);
                css_fixed presented = 0;
                if (milestone < 0) milestone = 0;
                if (milestone > 3) milestone = 3;
                if ((g_effects[i].diag_milestones & (1 << milestone)) == 0 &&
                    macsurf_transition_get_presented(g_effects[i].node,
                        g_effects[i].prop, &presented, now)) {
                    g_effects[i].diag_milestones |= (uint8_t)(1 << milestone);
                    TRANSITION_DIAG(("LIFE 2B2 tick node=%p now=%ld elapsed=%ld progress=%d presented=%d active=%d invalidated=1",
                        (void *)g_effects[i].node, (long)now,
                        (long)elapsed, milestone * 25,
                        (int)presented, g_effect_count));
                }
            }
#endif
        }
        /* otherwise need invalidation: for synthetic, no paint; for real, caller will handle */
    }
    if (still_active) {
#ifdef __MACOS9__
        /* 2B-2 opacity: paint invalidation, conservative full content */
        {
            struct gui_window *gw;
            extern struct gui_window *macos9_window_list_head(void);
            extern void macos9_window_invalidate_content(struct gui_window *g);
            for (gw = macos9_window_list_head(); gw != NULL; gw = gw->next) {
                macos9_window_invalidate_content(gw);
            }
            TRANSITION_DIAG(("LIFE 2B2 invalidate active=%d", g_effect_count));
        }
#endif
        macos9_schedule(MACSURF_TRANSITION_TICK_MS, macsurf_transition_tick, NULL);
        g_sched_armed = true;
    } else {
        g_sched_armed = false;
        macos9_schedule(-1, macsurf_transition_tick, NULL);
    }
}

void macsurf_transition_node_destroy(dom_node *node)
{
    int i;
    if (node == NULL) return;
    for (i = 0; i < MACSURF_TRANSITION_MAX_ACTIVE; i++) {
        if (g_effects[i].in_use && g_effects[i].node == node) {
            dom_node_unref(g_effects[i].node);
            g_effects[i].node = NULL;
            g_effects[i].in_use = false;
            g_effect_count--;
        }
    }
    schedule_tick_if_needed();
}

bool macsurf_transition_handle_style_change(struct html_content *c, dom_node *node,
        const css_computed_style *old_style, const css_computed_style *new_style,
        uint32_t now)
{
    css_fixed op_a = 0;
    css_fixed op_b = 0;
    uint8_t type_a = 0;
    uint8_t type_b = 0;
    css_fixed delay = 0;
    css_fixed duration = 0;
    css_transition_timing_entry timing;
    uint32_t count = 0;
    int match_idx = -1;
    css_effective_transition_descriptor desc;
    uint32_t i;
    (void)c;
    if (node == NULL || old_style == NULL || new_style == NULL) return false;
    type_a = css_computed_opacity(old_style, &op_a);
    type_b = css_computed_opacity(new_style, &op_b);
    if (type_a != CSS_OPACITY_SET) op_a = 1024;
    if (type_b != CSS_OPACITY_SET) op_b = 1024;
    if (op_a == op_b) return false;
    count = css_computed_transition_descriptor_count(new_style);
    if (count == 0) {
#ifdef __MACOS9__
        TRANSITION_DIAG(("LIFE 2B2 hook node=%p old=%d new=%d desc=0 match=-1",
            (void *)node, (int)op_a, (int)op_b));
#endif
        return false;
    }
    for (i = 0; i < count; i++) {
        if (!css_computed_transition_descriptor(new_style, i, &desc)) continue;
        if (desc.prop.kind == CSS_TRANS_PROP_ALL) {
            match_idx = (int)i;
        } else if (desc.prop.kind == CSS_TRANS_PROP_KNOWN && desc.prop.prop_id == CSS_PROP_OPACITY) {
            match_idx = (int)i;
        } else if (desc.prop.kind == CSS_TRANS_PROP_CUSTOM_IDENT) {
            /* not opacity */
        }
    }
    if (match_idx < 0) {
#ifdef __MACOS9__
        TRANSITION_DIAG(("LIFE 2B2 hook node=%p old=%d new=%d desc=%ld match=-1",
            (void *)node, (int)op_a, (int)op_b, (long)count));
#endif
        return false;
    }
    if (!css_computed_transition_descriptor(new_style, (uint32_t)match_idx, &desc)) return false;
    delay = desc.delay;
    duration = desc.duration;
    timing = desc.timing;
    if (duration == 0) return false;
#ifdef __MACOS9__
    /* Most style changes do not create transitions. Defer the toolbox clock
     * read until opacity changed, a matching descriptor exists, and duration
     * is non-zero. */
    if (now == MACSURF_TRANSITION_NOW_AUTO)
        now = (uint32_t)TickCount();
#endif
    /* create/replace via generic engine */
    {
        int created = macsurf_transition_create(node, CSS_PROP_OPACITY,
            op_a, op_b, delay, duration, timing, now, (dom_node *)old_style);
#ifdef __MACOS9__
        TRANSITION_DIAG(("LIFE 2B2 hook node=%p old=%d new=%d desc=%ld match=%d dur=%d delay=%d timing=%d create=%d active=%d",
            (void *)node, (int)op_a, (int)op_b, (long)count, match_idx,
            (int)duration, (int)delay, (int)timing.type, created,
            g_effect_count));
#endif
        return created ? true : false;
    }
}

bool macsurf_transition_get_opacity(dom_node *node, css_fixed target,
        uint32_t now, css_fixed *out)
{
    css_fixed presented = 0;
    if (out == NULL) return false;
    if (macsurf_transition_get_presented(node, CSS_PROP_OPACITY, &presented, now)) {
        *out = presented;
        return true;
    }
    *out = target;
    return false;
}

/* test hook for harness */
uint32_t macsurf_transition_test_now = 0;
