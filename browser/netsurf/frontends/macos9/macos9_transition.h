/*
 * MacSurf CSS Transitions - Round 2B generic bounded effect engine
 * Not yet wired to real computed-style; synthetic numeric tests only for 2B-1
 */

#ifndef macos9_transition_h_
#define macos9_transition_h_

#include <stdbool.h>
#include <stdint.h>
#include <dom/dom.h>
#include <libcss/computed.h>

#define MACSURF_TRANSITION_MAX_ACTIVE 64
#define MACSURF_TRANSITION_TICK_MS 16

enum macsurf_transition_state {
    MACSURF_TRANSITION_DELAY = 0,
    MACSURF_TRANSITION_ACTIVE = 1,
    MACSURF_TRANSITION_COMPLETE = 2
};

struct macsurf_transition_effect {
    dom_node *node;
    uint32_t prop;
    uint32_t start_tick;
    css_fixed delay;
    css_fixed duration;
    css_transition_timing_entry timing;
    css_fixed start_value;
    css_fixed target_value;
    enum macsurf_transition_state state;
    bool in_use;
    uint8_t diag_milestones;
};

/* wrap-safe elapsed: unsigned 32-bit subtraction handles TickCount wrap */
static uint32_t macsurf_transition_elapsed(uint32_t now, uint32_t start)
{
    return now - start;
}

/* API */
void macsurf_transition_init(void);
void macsurf_transition_reset(void);
int macsurf_transition_create(dom_node *node, uint32_t prop,
        css_fixed start_value, css_fixed target_value,
        css_fixed delay, css_fixed duration,
        css_transition_timing_entry timing,
        uint32_t now_tick,
        dom_node *initial_style_anchor);
bool macsurf_transition_get_presented(dom_node *node, uint32_t prop,
        css_fixed *out_value, uint32_t now_tick);
bool macsurf_transition_has_active(void);
void macsurf_transition_tick(void *p);
void macsurf_transition_node_destroy(dom_node *node);
int macsurf_transition_active_count(void);
bool macsurf_transition_scheduler_active(void);

/* 2B-2 opacity: shared style-change entry point */
struct html_content;
bool macsurf_transition_handle_style_change(struct html_content *c, dom_node *node,
        const css_computed_style *old_style, const css_computed_style *new_style,
        uint32_t now);
bool macsurf_transition_get_opacity(dom_node *node, css_fixed target,
        uint32_t now, css_fixed *out);

/* for tests: expose conversion */
int macsurf_transition_fixed_to_ticks(css_fixed v);
css_fixed macsurf_transition_ease_linear(css_fixed t);
extern uint32_t macsurf_transition_test_now;

#endif
