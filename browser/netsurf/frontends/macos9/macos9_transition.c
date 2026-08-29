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
    /* v is Q22.10 seconds; ticks = v *60 /1024; v small so 32-bit safe */
    long tmp = (long)v * 60;
    return (int)(tmp / 1024);
}

css_fixed macsurf_transition_ease_linear(css_fixed t)
{
    return t;
}

/* simple linear ease for synthetic; cubic solver deferred to 2B-2 */
static css_fixed apply_timing(css_transition_timing_entry *timing, css_fixed progress)
{
    /* progress is Q22.10 0..1024 */
    if (timing == NULL) return progress;
    if (timing->type == CSS_TIMING_LINEAR) {
        return progress;
    }
    if (timing->type == CSS_TIMING_EASE ||
        timing->type == CSS_TIMING_EASE_IN ||
        timing->type == CSS_TIMING_EASE_OUT ||
        timing->type == CSS_TIMING_EASE_IN_OUT) {
        /* For 2B-1 synthetic, map keywords to linear to keep tests deterministic.
         * Real cubic mapping comes in 2B-2. */
        return progress;
    }
    if (timing->type == CSS_TIMING_CUBIC_BEZIER) {
        /* Simplified: linear for now; real solver in 2B-2 */
        return progress;
    }
    if (timing->type == CSS_TIMING_STEPS) {
        int steps = (int)timing->step_count;
        int pos = (int)timing->step_pos;
        long prog = (long)progress; /* 0..1024 */
        long step_size = 1024 / steps;
        long current_step;
        if (steps <= 0) return progress;
        if (pos == 0) {
            /* jump-start: first step at 0 */
            current_step = (prog * steps) / 1024;
            if (current_step >= steps) current_step = steps - 1;
            return (css_fixed)((current_step * 1024) / steps);
        } else if (pos == 1) {
            /* jump-end: first step after first interval */
            current_step = (prog * steps) / 1024;
            if (current_step >= steps) return 1024;
            return (css_fixed)(((current_step) * 1024) / steps);
        } else if (pos == 2) {
            /* jump-none: no step at 0 nor 1 */
            current_step = (prog * steps) / 1024;
            if (current_step == 0) return 0;
            if (current_step >= steps) return 1024;
            return (css_fixed)(((current_step - 1) * 1024) / (steps - 1));
        } else {
            /* jump-both */
            current_step = (prog * steps) / 1024;
            if (current_step >= steps) return 1024;
            return (css_fixed)(((current_step + 1) * 1024) / (steps + 1));
        }
    }
    return progress;
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
