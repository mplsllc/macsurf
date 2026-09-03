/*
 * MacSurf Transition 2B-1 synthetic engine tests
 * Linux harness only
 */
#include <stdio.h>
#include <string.h>
#include <dom/dom.h>
#include <libcss/computed.h>
#include <libcss/fpmath.h>
#include "frontends/macos9/macos9_transition.h"
#include "frontends/macos9/macos9_opacity.h"

static bool test_macos9_opacity_renderer_contract(void)
{
    if (macos9_opacity_resolve(0, 0) != MACOS9_OPACITY_SCALE) return false;
    if (macos9_opacity_resolve(0, 1) != 0) return false;
    if (macos9_opacity_bucket_for(0) != MACOS9_OPACITY_SKIP) return false;
    if (macos9_opacity_bucket_for(100) != MACOS9_OPACITY_SPARSE) return false;
    if (macos9_opacity_bucket_for(512) != MACOS9_OPACITY_HALF) return false;
    if (macos9_opacity_bucket_for(700) != MACOS9_OPACITY_DENSE) return false;
    if (macos9_opacity_bucket_for(900) != MACOS9_OPACITY_SOLID) return false;
    if (macos9_opacity_bucket_for(1024) != MACOS9_OPACITY_SOLID) return false;
    fprintf(stderr, "  [2B-2] opacity renderer contract PASS\n");
    return true;
}

/* helper: create a fresh DOM element to serve as stable node identity */
static dom_node *create_test_node(void)
{
    dom_document *doc = NULL;
    dom_element *elem = NULL;
    dom_string *name = NULL;
    dom_exception err;
    err = dom_implementation_create_document(DOM_IMPLEMENTATION_HTML,
            NULL, NULL, NULL, NULL, NULL, &doc);
    if (err != DOM_NO_ERR || doc == NULL) return NULL;
    err = dom_string_create((const uint8_t *)"div", 3, &name);
    if (err != DOM_NO_ERR) {
        dom_node_unref((dom_node *)doc);
        return NULL;
    }
    err = dom_document_create_element(doc, name, &elem);
    dom_string_unref(name);
    dom_node_unref((dom_node *)doc);
    if (err != DOM_NO_ERR || elem == NULL) return NULL;
    return (dom_node *)elem;
}

static void destroy_test_node(dom_node *n)
{
    if (n != NULL) dom_node_unref(n);
}

bool test_macos9_transition_2b1(void)
{
    dom_node *n1 = NULL;
    dom_node *n2 = NULL;
    dom_node *n3 = NULL;
    css_fixed out = 0;
    css_transition_timing_entry timing;
    bool ok = true;
    uint32_t now = 0;
    int i;

    macsurf_transition_init();
    macsurf_transition_test_now = 0;

    /* 0. time conversion: Q22.10 seconds -> ticks (60Hz) */
    {
        if (macsurf_transition_fixed_to_ticks(0) != 0) { fprintf(stderr, "FAIL fixed 0s\n"); ok=false; goto done; }
        if (macsurf_transition_fixed_to_ticks(1024) != 60) { fprintf(stderr, "FAIL fixed 1s %d\n", macsurf_transition_fixed_to_ticks(1024)); ok=false; goto done; }
        if (macsurf_transition_fixed_to_ticks(512) != 30) { fprintf(stderr, "FAIL 0.5s\n"); ok=false; goto done; }
        if (macsurf_transition_fixed_to_ticks(256) != 15) { fprintf(stderr, "FAIL 250ms %d\n", macsurf_transition_fixed_to_ticks(256)); ok=false; goto done; }
        if (macsurf_transition_fixed_to_ticks(102) != 6) { fprintf(stderr, "FAIL 100ms %d expected 6\n", macsurf_transition_fixed_to_ticks(102)); ok=false; goto done; }
        if (macsurf_transition_fixed_to_ticks(1) != 0) { fprintf(stderr, "FAIL 1ms %d expected 0\n", macsurf_transition_fixed_to_ticks(1)); ok=false; goto done; }
        /* 0.5ms also 0 or 1, allow either (same as 1ms) */
        if (macsurf_transition_fixed_to_ticks(1) < 0 || macsurf_transition_fixed_to_ticks(1) > 1) { fprintf(stderr, "FAIL 0.5ms\n"); ok=false; goto done; }
        if (macsurf_transition_fixed_to_ticks(1024000) != 60000) { fprintf(stderr, "FAIL 1000s %d\n", macsurf_transition_fixed_to_ticks(1024000)); ok=false; goto done; }
        /* large not wrap negative */
        if (macsurf_transition_fixed_to_ticks(1024000) < 0) { fprintf(stderr, "FAIL large negative\n"); ok=false; goto done; }
        /* signed delay: -0.25s = -256 => -15 */
        if (macsurf_transition_fixed_to_ticks(-256) != -15) { fprintf(stderr, "FAIL -250ms %d\n", macsurf_transition_fixed_to_ticks(-256)); ok=false; goto done; }
        fprintf(stderr, "  [2B-1] time conversion PASS\n");
    }

    /* timing: linear */
    timing.type = CSS_TIMING_LINEAR;
    timing.x1 = 0; timing.y1 = 0; timing.x2 = 0; timing.y2 = 0;
    timing.step_count = 0; timing.step_pos = 0;

    /* 1. initial style must not transition */
    n1 = create_test_node();
    if (n1 == NULL) { fprintf(stderr, "FAIL 2B-1 create n1\n"); ok = false; goto done; }
    if (macsurf_transition_create(n1, 1, 0, 1024, 0, 1024, timing, 0, NULL) != 0) {
        fprintf(stderr, "FAIL 2B-1 initial style should not transition\n"); ok = false; goto done;
    }
    if (macsurf_transition_has_active()) {
        fprintf(stderr, "FAIL 2B-1 initial style left active\n"); ok = false; goto done;
    }
    fprintf(stderr, "  [2B-1] 1 initial style PASS\n");

    /* 2. same target A == B: no transition */
    if (macsurf_transition_create(n1, 1, 512, 512, 0, 1024, timing, 0, n1) != 0) {
        fprintf(stderr, "FAIL 2B-1 same target should not transition\n"); ok = false; goto done;
    }
    fprintf(stderr, "  [2B-1] 2 same target PASS\n");

    /* 3. duration zero: no active effect */
    if (macsurf_transition_create(n1, 1, 0, 1024, 0, 0, timing, 0, n1) != 0) {
        fprintf(stderr, "FAIL 2B-1 duration zero should not create effect\n"); ok = false; goto done;
    }
    if (macsurf_transition_has_active()) {
        fprintf(stderr, "FAIL 2B-1 duration zero left active\n"); ok = false; goto done;
    }
    fprintf(stderr, "  [2B-1] 3 duration zero PASS\n");

    /* 4. positive delay: remains at start until delay expires */
    macsurf_transition_reset();
    now = 100;
    macsurf_transition_test_now = now;
    if (macsurf_transition_create(n1, 1, 0, 1024, 512, 1024, timing, now, n1) == 0) {
        fprintf(stderr, "FAIL 2B-1 positive delay create\n"); ok = false; goto done;
    }
    /* at now (elapsed 0, delay 512 -> effective -512 <0 => DELAY, out 0) */
    if (!macsurf_transition_get_presented(n1, 1, &out, now) || out != 0) {
        fprintf(stderr, "FAIL 2B-1 positive delay at start out=%d expected 0\n", (int)out); ok = false; goto done;
    }
    /* at now+10 ticks (still before delay 30 ticks? 512 fixed = 0.5s =30 ticks) */
    /* 512 fixed = 0.5s, ticks = 0.5*60=30 */
    now += 10;
    macsurf_transition_test_now = now;
    if (!macsurf_transition_get_presented(n1, 1, &out, now) || out != 0) {
        fprintf(stderr, "FAIL 2B-1 positive delay mid-delay out=%d\n", (int)out); ok = false; goto done;
    }
    /* at now+30 ticks (delay expired, progress 0) */
    now += 20;
    macsurf_transition_test_now = now;
    if (!macsurf_transition_get_presented(n1, 1, &out, now) || out != 0) {
        /* at effective 0, progress 0 => start */
        fprintf(stderr, "FAIL 2B-1 positive delay at delay end out=%d\n", (int)out); ok = false; goto done;
    }
    /* at now+15 more (mid) */
    now += 15;
    macsurf_transition_test_now = now;
    if (!macsurf_transition_get_presented(n1, 1, &out, now) || out <= 0 || out >= 1024) {
        fprintf(stderr, "FAIL 2B-1 positive delay mid out=%d\n", (int)out); ok = false; goto done;
    }
    fprintf(stderr, "  [2B-1] 4 positive delay PASS\n");
    macsurf_transition_reset();

    /* 5. negative delay: begins partially progressed */
    now = 200;
    macsurf_transition_test_now = now;
    /* delay -0.25s = -256, duration 1s =1024, ticks -15 and 60 */
    if (macsurf_transition_create(n1, 1, 0, 1024, -256, 1024, timing, now, n1) == 0) {
        fprintf(stderr, "FAIL 2B-1 negative delay create\n"); ok = false; goto done;
    }
    if (!macsurf_transition_get_presented(n1, 1, &out, now) || out <= 0 || out >= 1024) {
        fprintf(stderr, "FAIL 2B-1 negative delay at start out=%d expected ~256\n", (int)out); ok = false; goto done;
    }
    /* check approximately 25% */
    if (out < 200 || out > 300) {
        fprintf(stderr, "FAIL 2B-1 negative delay progress out=%d not ~256\n", (int)out); ok = false; goto done;
    }
    fprintf(stderr, "  [2B-1] 5 negative delay PASS\n");
    macsurf_transition_reset();

    /* 6. active midpoint: progress between 0 and 1 */
    now = 300;
    macsurf_transition_test_now = now;
    if (macsurf_transition_create(n1, 1, 0, 1024, 0, 1024, timing, now, n1) == 0) {
        fprintf(stderr, "FAIL 2B-1 midpoint create\n"); ok = false; goto done;
    }
    now += 30; /* half duration 60 ticks, so 30 is midpoint */
    macsurf_transition_test_now = now;
    if (!macsurf_transition_get_presented(n1, 1, &out, now) || out <= 200 || out >= 800) {
        fprintf(stderr, "FAIL 2B-1 midpoint out=%d expected ~512\n", (int)out); ok = false; goto done;
    }
    fprintf(stderr, "  [2B-1] 6 active midpoint PASS\n");
    macsurf_transition_reset();

    /* 7. completion: exact target, effect retired */
    now = 400;
    macsurf_transition_test_now = now;
    if (macsurf_transition_create(n1, 1, 0, 1024, 0, 1024, timing, now, n1) == 0) {
        fprintf(stderr, "FAIL 2B-1 completion create\n"); ok = false; goto done;
    }
    now += 70; /* beyond duration 60 */
    macsurf_transition_test_now = now;
    if (!macsurf_transition_get_presented(n1, 1, &out, now) || out != 1024) {
        fprintf(stderr, "FAIL 2B-1 completion out=%d expected 1024\n", (int)out); ok = false; goto done;
    }
    /* tick should retire */
    macsurf_transition_tick(NULL);
    if (macsurf_transition_has_active()) {
        fprintf(stderr, "FAIL 2B-1 completion not retired\n"); ok = false; goto done;
    }
    if (macsurf_transition_get_presented(n1, 1, &out, now)) {
        /* after retired, should return false (use target) */
        fprintf(stderr, "FAIL 2B-1 completion should be gone\n"); ok = false; goto done;
    }
    fprintf(stderr, "  [2B-1] 7 completion PASS\n");
    macsurf_transition_reset();

    /* 8. no active effects: scheduler inactive */
    if (macsurf_transition_scheduler_active()) {
        fprintf(stderr, "FAIL 2B-1 scheduler should be inactive with no effects\n"); ok = false; goto done;
    }
    now = 500;
    macsurf_transition_test_now = now;
    if (macsurf_transition_create(n1, 1, 0, 1024, 0, 1024, timing, now, n1) == 0) {
        fprintf(stderr, "FAIL 2B-1 scheduler create\n"); ok = false; goto done;
    }
    if (!macsurf_transition_scheduler_active()) {
        fprintf(stderr, "FAIL 2B-1 scheduler should be active\n"); ok = false; goto done;
    }
    /* complete it */
    now += 70;
    macsurf_transition_test_now = now;
    macsurf_transition_tick(NULL);
    if (macsurf_transition_scheduler_active()) {
        fprintf(stderr, "FAIL 2B-1 scheduler should be inactive after completion\n"); ok = false; goto done;
    }
    fprintf(stderr, "  [2B-1] 8 scheduler inactive PASS\n");
    macsurf_transition_reset();

    /* 9. node teardown: effect removed safely */
    n2 = create_test_node();
    n3 = create_test_node();
    if (n2 == NULL || n3 == NULL) { fprintf(stderr, "FAIL create n2/n3\n"); ok = false; goto done; }
    now = 600;
    macsurf_transition_test_now = now;
    if (macsurf_transition_create(n2, 1, 0, 1024, 0, 1024, timing, now, n2) == 0) { fprintf(stderr, "FAIL teardown create n2\n"); ok=false; goto done; }
    if (macsurf_transition_create(n3, 1, 0, 1024, 512, 1024, timing, now, n3) == 0) { fprintf(stderr, "FAIL teardown create n3\n"); ok=false; goto done; }
    if (macsurf_transition_active_count() != 2) { fprintf(stderr, "FAIL teardown count 2\n"); ok=false; goto done; }
    macsurf_transition_node_destroy(n2);
    if (macsurf_transition_active_count() != 1) { fprintf(stderr, "FAIL teardown n2 removed count %d\n", macsurf_transition_active_count()); ok=false; goto done; }
    if (macsurf_transition_get_presented(n2, 1, &out, now)) { fprintf(stderr, "FAIL teardown n2 still has effect\n"); ok=false; goto done; }
    /* n3 should still be there */
    if (!macsurf_transition_get_presented(n3, 1, &out, now) || out != 0) { fprintf(stderr, "FAIL teardown n3 out=%d\n", (int)out); ok=false; goto done; }
    macsurf_transition_node_destroy(n3);
    if (macsurf_transition_has_active()) { fprintf(stderr, "FAIL teardown all removed\n"); ok=false; goto done; }
    fprintf(stderr, "  [2B-1] 9 node teardown PASS\n");
    destroy_test_node(n2); n2 = NULL;
    destroy_test_node(n3); n3 = NULL;
    macsurf_transition_reset();

    /* 10. capacity exhaustion: safe target-state fallback */
    {
        dom_node *nodes[80];
        int created = 0;
        for (i = 0; i < 80; i++) {
            nodes[i] = create_test_node();
            if (nodes[i] == NULL) { fprintf(stderr, "FAIL capacity create node %d\n", i); ok=false; goto cap_done; }
        }
        now = 700;
        macsurf_transition_test_now = now;
        for (i = 0; i < 80; i++) {
            int r = macsurf_transition_create(nodes[i], 1, 0, 1024, 0, 1024, timing, now, nodes[i]);
            if (r) created++;
        }
        if (created != MACSURF_TRANSITION_MAX_ACTIVE) {
            fprintf(stderr, "FAIL capacity created %d expected %d\n", created, MACSURF_TRANSITION_MAX_ACTIVE); ok=false; goto cap_done;
        }
        if (macsurf_transition_active_count() != MACSURF_TRANSITION_MAX_ACTIVE) {
            fprintf(stderr, "FAIL capacity count %d\n", macsurf_transition_active_count()); ok=false; goto cap_done;
        }
        /* next create should fail safely, not crash, and not increase count */
        {
            dom_node *extra = create_test_node();
            int r = macsurf_transition_create(extra, 1, 0, 1024, 0, 1024, timing, now, extra);
            if (r != 0) { fprintf(stderr, "FAIL capacity extra should fail\n"); ok=false; destroy_test_node(extra); goto cap_done; }
            if (macsurf_transition_active_count() != MACSURF_TRANSITION_MAX_ACTIVE) { fprintf(stderr, "FAIL capacity extra changed count\n"); ok=false; destroy_test_node(extra); goto cap_done; }
            destroy_test_node(extra);
        }
        fprintf(stderr, "  [2B-1] 10 capacity exhaustion PASS (%d)\n", MACSURF_TRANSITION_MAX_ACTIVE);
cap_done:
        for (i = 0; i < 80; i++) {
            if (nodes[i] != NULL) {
                macsurf_transition_node_destroy(nodes[i]);
                destroy_test_node(nodes[i]);
            }
        }
        if (!ok) goto done;
        macsurf_transition_reset();
    }

    /* 11. TickCount wrap-safe elapsed */
    {
        uint32_t start = 0xFFFFFFF0UL;
        uint32_t now2 = 0x00000010UL;
        uint32_t elapsed = macsurf_transition_elapsed(now2, start);
        if (elapsed != 0x20UL) {
            fprintf(stderr, "FAIL wrap elapsed %lu expected 32\n", elapsed); ok=false; goto done;
        }
        /* through engine: create at wrap point, check progress */
        macsurf_transition_reset();
        n1 = create_test_node(); /* reuse n1? need fresh */
        /* n1 already exists, but we reset; create new */
        destroy_test_node(n1); n1 = create_test_node();
        if (n1 == NULL) { fprintf(stderr, "FAIL wrap create\n"); ok=false; goto done; }
        start = 0xFFFFFFF0UL;
        now = start;
        macsurf_transition_test_now = now;
        if (macsurf_transition_create(n1, 1, 0, 1024, 0, 1024, timing, start, n1) == 0) { fprintf(stderr, "FAIL wrap create2\n"); ok=false; goto done; }
        now2 = 0x00000010UL;
        macsurf_transition_test_now = now2;
        if (!macsurf_transition_get_presented(n1, 1, &out, now2)) { fprintf(stderr, "FAIL wrap get\n"); ok=false; goto done; }
        /* elapsed 32 ticks, duration 60 ticks, progress ~546 (32/60*1024) */
        if (out <= 400 || out >= 700) {
            fprintf(stderr, "FAIL wrap progress out=%d expected ~546\n", (int)out); ok=false; goto done;
        }
        fprintf(stderr, "  [2B-1] 11 TickCount wrap PASS\n");
    }

    fprintf(stderr, "  [2B-1] all synthetic tests PASS\n");

done:
    if (n1) destroy_test_node(n1);
    if (n2) destroy_test_node(n2);
    if (n3) destroy_test_node(n3);
    macsurf_transition_reset();
    return ok;
}

bool test_macos9_transition_opacity(void)
{
    dom_node *n = NULL;
    css_fixed out = 0;
    css_transition_timing_entry timing;
    bool ok = true;
    uint32_t now = 0;

    if (!test_macos9_opacity_renderer_contract()) return false;
    n = create_test_node();
    if (n == NULL) { fprintf(stderr, "FAIL opacity create node\n"); return false; }
    macsurf_transition_init();
    timing.type = CSS_TIMING_LINEAR; timing.x1=0; timing.y1=0; timing.x2=0; timing.y2=0; timing.step_count=0; timing.step_pos=0;

    /* 6. initial style -> no effect handled via handle_style_change, but synthetic: */
    /* For direct create, initial anchor NULL should not create */
    if (macsurf_transition_create(n, CSS_PROP_OPACITY, 0, 1024, 0, 1024, timing, 0, NULL) != 0) {
        fprintf(stderr, "FAIL opacity initial\n"); ok=false; goto done_op;
    }
    /* 7. same opacity -> no effect */
    if (macsurf_transition_create(n, CSS_PROP_OPACITY, 512, 512, 0, 1024, timing, 0, n) != 0) {
        fprintf(stderr, "FAIL opacity same\n"); ok=false; goto done_op;
    }
    /* 9. explicit opacity descriptor -> effect */
    now = 100;
    macsurf_transition_test_now = now;
    if (macsurf_transition_create(n, CSS_PROP_OPACITY, 0, 1024, 0, 1024, timing, now, n) == 0) {
        fprintf(stderr, "FAIL opacity explicit create\n"); ok=false; goto done_op;
    }
    if (!macsurf_transition_get_presented(n, CSS_PROP_OPACITY, &out, now) || out != 0) {
        fprintf(stderr, "FAIL opacity start %d\n", (int)out); ok=false; goto done_op;
    }
    /* 14. midpoint */
    now += 30;
    macsurf_transition_test_now = now;
    if (!macsurf_transition_get_presented(n, CSS_PROP_OPACITY, &out, now) || out < 400 || out > 600) {
        fprintf(stderr, "FAIL opacity midpoint %d\n", (int)out); ok=false; goto done_op;
    }
    /* 15. completion */
    now += 40;
    macsurf_transition_test_now = now;
    if (!macsurf_transition_get_presented(n, CSS_PROP_OPACITY, &out, now) || out != 1024) {
        fprintf(stderr, "FAIL opacity end %d\n", (int)out); ok=false; goto done_op;
    }
    macsurf_transition_tick(NULL);
    if (macsurf_transition_has_active()) { fprintf(stderr, "FAIL opacity not retired\n"); ok=false; goto done_op; }
    fprintf(stderr, "  [2B-2] opacity 0->1 linear PASS\n");
    macsurf_transition_reset();
    destroy_test_node(n); n = create_test_node();
    if (n == NULL) { ok=false; goto done_op; }

    /* 16. 1->0 */
    now = 200;
    macsurf_transition_test_now = now;
    if (macsurf_transition_create(n, CSS_PROP_OPACITY, 1024, 0, 0, 1024, timing, now, n) == 0) { fprintf(stderr, "FAIL 1->0 create\n"); ok=false; goto done_op; }
    now += 30;
    macsurf_transition_test_now = now;
    if (!macsurf_transition_get_presented(n, CSS_PROP_OPACITY, &out, now) || out < 400 || out > 600) { fprintf(stderr, "FAIL 1->0 midpoint %d\n", (int)out); ok=false; goto done_op; }
    fprintf(stderr, "  [2B-2] opacity 1->0 PASS\n");
    macsurf_transition_reset();

    /* 11. zero duration -> no effect */
    now = 300;
    macsurf_transition_test_now = now;
    if (macsurf_transition_create(n, CSS_PROP_OPACITY, 0, 1024, 0, 0, timing, now, n) != 0) { fprintf(stderr, "FAIL zero duration\n"); ok=false; goto done_op; }
    fprintf(stderr, "  [2B-2] zero duration PASS\n");

    /* 17. +delay holds start */
    now = 400;
    macsurf_transition_test_now = now;
    if (macsurf_transition_create(n, CSS_PROP_OPACITY, 0, 1024, 512, 1024, timing, now, n) == 0) { fprintf(stderr, "FAIL +delay create\n"); ok=false; goto done_op; }
    if (!macsurf_transition_get_presented(n, CSS_PROP_OPACITY, &out, now) || out != 0) { fprintf(stderr, "FAIL +delay start %d\n", (int)out); ok=false; goto done_op; }
    now += 10;
    macsurf_transition_test_now = now;
    if (!macsurf_transition_get_presented(n, CSS_PROP_OPACITY, &out, now) || out != 0) { fprintf(stderr, "FAIL +delay mid %d\n", (int)out); ok=false; goto done_op; }
    fprintf(stderr, "  [2B-2] +delay PASS\n");
    macsurf_transition_reset();

    /* 18. -delay starts progressed */
    now = 500;
    macsurf_transition_test_now = now;
    if (macsurf_transition_create(n, CSS_PROP_OPACITY, 0, 1024, -256, 1024, timing, now, n) == 0) { fprintf(stderr, "FAIL -delay create\n"); ok=false; goto done_op; }
    if (!macsurf_transition_get_presented(n, CSS_PROP_OPACITY, &out, now) || out < 200 || out > 300) { fprintf(stderr, "FAIL -delay %d\n", (int)out); ok=false; goto done_op; }
    fprintf(stderr, "  [2B-2] -delay PASS\n");
    macsurf_transition_reset();

    /* 19. -delay >= duration completes immediately */
    now = 600;
    macsurf_transition_test_now = now;
    if (macsurf_transition_create(n, CSS_PROP_OPACITY, 0, 1024, -1024, 1024, timing, now, n) != 0) { fprintf(stderr, "FAIL -delay beyond should not create\n"); ok=false; goto done_op; }
    if (macsurf_transition_has_active()) { fprintf(stderr, "FAIL -delay beyond active\n"); ok=false; goto done_op; }
    fprintf(stderr, "  [2B-2] -delay beyond PASS\n");

    /* 20-25 timing */
    {
        css_transition_timing_entry t2;
        t2.type = CSS_TIMING_EASE; t2.x1=0; t2.y1=0; t2.x2=0; t2.y2=0; t2.step_count=0; t2.step_pos=0;
        now = 700; macsurf_transition_test_now = now;
        if (macsurf_transition_create(n, CSS_PROP_OPACITY, 0, 1024, 0, 1024, t2, now, n)==0) { fprintf(stderr, "FAIL ease create\n"); ok=false; goto done_op; }
        macsurf_transition_reset();
        t2.type = CSS_TIMING_CUBIC_BEZIER; t2.x1=16384; t2.y1=6553; t2.x2=16384; t2.y2=65536;
        now = 710; macsurf_transition_test_now = now;
        if (macsurf_transition_create(n, CSS_PROP_OPACITY, 0, 1024, 0, 1024, t2, now, n)==0) { fprintf(stderr, "FAIL cubic create\n"); ok=false; goto done_op; }
        macsurf_transition_reset();
        t2.type = CSS_TIMING_STEPS; t2.step_count=4; t2.step_pos=1;
        now = 720; macsurf_transition_test_now = now;
        if (macsurf_transition_create(n, CSS_PROP_OPACITY, 0, 1024, 0, 1024, t2, now, n)==0) { fprintf(stderr, "FAIL steps create\n"); ok=false; goto done_op; }
        macsurf_transition_reset();
        fprintf(stderr, "  [2B-2] timing variations PASS\n");
    }

    /* 26. interruption */
    now = 800;
    macsurf_transition_test_now = now;
    if (macsurf_transition_create(n, CSS_PROP_OPACITY, 0, 1024, 0, 1024, timing, now, n)==0) { fprintf(stderr, "FAIL interrupt create\n"); ok=false; goto done_op; }
    now += 24; /* 40% of 60 ticks =24 */
    macsurf_transition_test_now = now;
    if (!macsurf_transition_get_presented(n, CSS_PROP_OPACITY, &out, now) || out < 300 || out > 500) { fprintf(stderr, "FAIL interrupt mid %d\n", (int)out); ok=false; goto done_op; }
    {
        css_fixed mid = out;
        /* retarget to 0.2 = 204 */
        if (macsurf_transition_create(n, CSS_PROP_OPACITY, mid, 204, 0, 1024, timing, now, n)==0) {
            /* create will sample current presented as start, so pass dummy start */
            fprintf(stderr, "FAIL interrupt retarget\n"); ok=false; goto done_op;
        }
        /* new start should be ~mid, not 0 or 1024 */
        if (!macsurf_transition_get_presented(n, CSS_PROP_OPACITY, &out, now) || out < 300 || out > 500) { fprintf(stderr, "FAIL interrupt new start %d vs mid %d\n", (int)out, (int)mid); ok=false; goto done_op; }
        /* check only one effect */
        if (macsurf_transition_active_count() != 1) { fprintf(stderr, "FAIL interrupt count %d\n", macsurf_transition_active_count()); ok=false; goto done_op; }
    }
    fprintf(stderr, "  [2B-2] interruption PASS\n");
    macsurf_transition_reset();

    /* 27. teardown during delay/active */
    now = 900;
    macsurf_transition_test_now = now;
    if (macsurf_transition_create(n, CSS_PROP_OPACITY, 0, 1024, 512, 1024, timing, now, n)==0) { fprintf(stderr, "FAIL teardown delay create\n"); ok=false; goto done_op; }
    macsurf_transition_node_destroy(n);
    if (macsurf_transition_has_active()) { fprintf(stderr, "FAIL teardown delay still active\n"); ok=false; goto done_op; }
    /* recreate for active */
    destroy_test_node(n); n = create_test_node();
    now = 910;
    macsurf_transition_test_now = now;
    if (macsurf_transition_create(n, CSS_PROP_OPACITY, 0, 1024, 0, 1024, timing, now, n)==0) { fprintf(stderr, "FAIL teardown active create\n"); ok=false; goto done_op; }
    now += 10;
    macsurf_transition_test_now = now;
    macsurf_transition_get_presented(n, CSS_PROP_OPACITY, &out, now);
    macsurf_transition_node_destroy(n);
    if (macsurf_transition_has_active()) { fprintf(stderr, "FAIL teardown active still\n"); ok=false; goto done_op; }
    fprintf(stderr, "  [2B-2] teardown PASS\n");
    macsurf_transition_reset();

    /* 29. scheduler inactive */
    if (macsurf_transition_scheduler_active()) { fprintf(stderr, "FAIL scheduler inactive\n"); ok=false; goto done_op; }
    now = 1000;
    macsurf_transition_test_now = now;
    if (macsurf_transition_create(n, CSS_PROP_OPACITY, 0, 1024, 0, 1024, timing, now, n)==0) { fprintf(stderr, "FAIL scheduler create\n"); ok=false; goto done_op; }
    if (!macsurf_transition_scheduler_active()) { fprintf(stderr, "FAIL scheduler active\n"); ok=false; goto done_op; }
    now += 70;
    macsurf_transition_test_now = now;
    macsurf_transition_tick(NULL);
    if (macsurf_transition_scheduler_active()) { fprintf(stderr, "FAIL scheduler after\n"); ok=false; goto done_op; }
    fprintf(stderr, "  [2B-2] scheduler PASS\n");

    fprintf(stderr, "  [2B-2] opacity tests PASS\n");
done_op:
    if (n) destroy_test_node(n);
    macsurf_transition_reset();
    return ok;
}
