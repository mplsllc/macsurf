/*
 * MacSurf CSS Transitions - Round 2B-1 generic bounded effect engine
 * Synthetic numeric tests only; no real opacity rendering yet
 */

#include <stdlib.h>
#include <string.h>
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
        /* determine initial state: check delay */
        {
            int delay_ticks = macsurf_transition_fixed_to_ticks(delay);
            int duration_ticks = macsurf_transition_fixed_to_ticks(duration);
            if (duration != 0 && duration_ticks == 0) duration_ticks = 1;
            long elapsed = 0;
            long effective = 0 - delay_ticks;
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
    {
        int delay_ticks = macsurf_transition_fixed_to_ticks(delay);
        int duration_ticks = macsurf_transition_fixed_to_ticks(duration);
            if (duration != 0 && duration_ticks == 0) duration_ticks = 1;
        long effective = 0 - delay_ticks;
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
    (void)p;
#ifdef __MACOS9__
    now = (uint32_t)TickCount();
#else
    /* harness: use fake tick incremented by caller */
    extern uint32_t macsurf_transition_test_now;
    now = macsurf_transition_test_now;
#endif
    for (i = 0; i < MACSURF_TRANSITION_MAX_ACTIVE; i++) {
        if (!g_effects[i].in_use) continue;
        /* check if complete */
        {
            uint32_t elapsed = macsurf_transition_elapsed(now, g_effects[i].start_tick);
            int delay_ticks = macsurf_transition_fixed_to_ticks(g_effects[i].delay);
            int duration_ticks = macsurf_transition_fixed_to_ticks(g_effects[i].duration);
            if (g_effects[i].duration != 0 && duration_ticks == 0) duration_ticks = 1;
            long effective = (long)elapsed - delay_ticks;
            if (effective >= duration_ticks) {
                /* complete: retire */
                dom_node_unref(g_effects[i].node);
                g_effects[i].node = NULL;
                g_effects[i].in_use = false;
                g_effect_count--;
                continue;
            }
            still_active = 1;
        }
        /* otherwise need invalidation: for synthetic, no paint; for real, caller will handle */
    }
    if (still_active) {
        macos9_schedule(MACSURF_TRANSITION_TICK_MS, macsurf_transition_tick, NULL);
        g_sched_armed = true;
        /* for paint, we would invalidate rects here */
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

/* test hook for harness */
uint32_t macsurf_transition_test_now = 0;
