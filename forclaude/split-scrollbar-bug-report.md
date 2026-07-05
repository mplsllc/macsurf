# The "Split Scrollbar" INT_MAX Descendant-Extent Bug — Engineering Research Report

> Handoff dossier for a higher-capability agent tasked with designing the fix.
> Target: MacSurf (NetSurf fork, Mac OS 9 / PowerPC / CodeWarrior 8, strict C89).
> Page: `tinkerdifferent.com` (XenForo forum, ThemeHouse "Abyss" dark theme).
> Date: 2026-07-04. Working tree ≈ fixes624 (uncommitted).
> All line citations below were re-verified against the working-tree source before publication.

---

## 1. Executive Summary

On `tinkerdifferent.com` at a 949 px content viewport, MacSurf lays out the page so that the **root layout box is correctly 949 px wide** (`lyt_wh=949,11062`), but the browser then believes the page's *content width* (`c->width`, "c_w") is exactly **2147483647 = INT_MAX**. The visible content is packed into the leftmost ~949 px while a giant empty dark canvas extends to the right, plus a spurious horizontal scrollbar whose thumb represents a ~2.1-billion-pixel document — the visual "split."

The corruption is **X-axis only**: the vertical axis is completely clean (`c_h=11062`, `desc_y1=11062`). The mechanism is well-characterized:

- `c->width` is computed in `html_reformat` (`html.c:1798-1799`) as `layout->x + layout->descendant_x1`. With `layout->x == 0` (`lyt_xy=0,0`), **`c_w == descendant_x1` exactly**. The root's `descendant_x1` is `INT_MAX`, so `c_w` becomes `INT_MAX`.
- `descendant_x1` is a running-max border-edge bounding box accumulated up the tree. Exactly **one descendant box** contributes an `INT_MAX` X-extent, and it rides to the root unclamped.
- The value **2147483647 is bit-exact `INT_MAX`, which is the definition of `UNKNOWN_WIDTH` / `UNKNOWN_MAX_WIDTH`** (`box.h:45-46`). This is **not** `AUTO` (which is `INT_MIN = -2147483648`, `layout_internal.h:27` / `layout_safe.h:38`), and **not** additive overflow of a coordinate. It is the box-width sentinel every box is born with (`box_manipulate.c:119,127`), surviving into geometry because some box's width was either **never resolved off the sentinel** or was **computed to a near-INT_MAX value** by the fork's flex length-resolution, and the descendant-bbox walk passes it through instead of aborting/clamping.

**Two co-leading origin theories** (both verified holed in source, both reproduce the *exact* value):
- **H2** — the fixes623 flex place-site's `max_width` fallback (`layout_flex.c:1336`) accepts the `UNKNOWN_MAX_WIDTH (INT_MAX)` sentinel with no guard, yielding `b->width = INT_MAX - delta`.
- **H3** — `layout_flex__resolve_line` substitutes `available_main = INT_MAX` for an `AUTO` main size (`layout_flex.c:1140-1141`), and an un-clamped `flex-grow` item absorbs it to a ~INT_MAX `target_main_size`.

A third theory (**H1**, un-laid subtree left by the fixes613f flex tolerance) remains plausible but is **demoted from "leading"** — the "exact +INT_MAX" observation does **not** discriminate it from H2/H3 (see §5). The single highest-leverage catch-all is the descendant-bbox propagator (`layout_get_box_bbox`, `layout.c:6742-6751`), whose two "clamps" are both no-ops precisely when the *width itself* is the garbage.

**The pending `fixes624` `GARBAGEBOX` diagnostic would name the exact leaf box, but as shipped it cannot reach it** (children-only DFS, 256-deep truncating stack, and it skips `float_children` / `list_marker` / `inline_end` chains). In the captured log it produced **zero** output despite `descendant_x1 > 1e6` firing twice — which, if that binary contains fixes624, is positive evidence the leak box is a float / list-marker / inline-chain box or lies beyond the truncated stack. **Harden the diagnostic before trusting any negative.**

---

## 2. Reproduction & Environment

- **Browser:** MacSurf — a fork of the NetSurf engine ported to Classic Mac OS 9 PowerPC, cross-compiled with CodeWarrior 8 in strict C89 mode. Native macTLS HTTPS, QuickJS JS engine (working tree). Repo root `/home/patrick/Webs/macsurf`.
- **Layout code under scrutiny:** `browser/netsurf/content/handlers/html/` — `layout.c`, `layout_flex.c`, `layout_grid.c`, `html.c`, `box.h`, `box_manipulate.c`, `layout_internal.h`, `layout_safe.h`. Frontend prefix: `browser/netsurf/frontends/macos9/macsurf_prefix.h`.
- **Page:** `https://tinkerdifferent.com` — XenForo forum software, ThemeHouse "Abyss" dark theme. Heavy flex/grid layout (`p-body`, `p-body-inner`, `p-body-main--withSidebar`, thread-list rows, last-post cells with `overflow-x:hidden; white-space:nowrap`).
- **Viewport:** 949 px content width, 613 px content height (`in_w=949 in_h=613`).
- **Two-pass layout:** (1) `layout_minmax_*` computes each box's `min_width`/`max_width` (min-content / max-content); (2) `layout_document` → `layout_block_context` / `layout_flex` place and size boxes. `AUTO == INT_MIN`; `UNKNOWN_WIDTH == UNKNOWN_MAX_WIDTH == INT_MAX`.

**Exact log lines** (`scratchpad/macsurf_debug.log`, two identical reformat passes at timestamps `[2977]` and `[3127]`, lines 4307/4308 and 6712/6713):

```
reformat: in_w=949 in_h=613 c_w=2147483647 c_h=11062 desc_x1=2147483647 desc_y1=11062 lyt_xy=0,0 lyt_wh=949,11062
SITE ... boxes=4668 blk=853 inlinec=486 inline=840 text=873 other=1616 in_w=949 in_h=613 c_w=2147483647 c_h=11062 img_ok=43 img_fail=0 css_ok=9 css_skip=0 css_total=657216/2097152
```

Key reads:
- `lyt_wh=949,11062` — root layout box is **correctly 949 wide**.
- `lyt_xy=0,0` — `layout->x == 0`, so `c_w == descendant_x1` bit-for-bit.
- `desc_x1=2147483647` — one descendant's X-extent is `INT_MAX`.
- `desc_y1=11062`, `c_h=11062` — **Y axis clean**.
- `boxes=4668` — a large tree; the leak is a single leaf/subtree.

**Practical mitigation already found:** `fixes621` page zoom to 0.75 makes the effective viewport `949/0.75 ≈ 1265`, which can exceed the real min-content so the page fits. A workaround, not a fix.

---

## 3. Precise Symptom Characterization

- **Visual:** content collapses into a narrow left column (~949 px of real content) with a large empty dark band to its right; a spurious horizontal scrollbar whose extent corresponds to a ~2.1-billion-pixel-wide document.
- **Numeric:** `c_w == desc_x1 == 2147483647` (exact `INT_MAX`), while `c_h == desc_y1 == 11062` (finite, correct). **X-axis-only corruption.**
- **Asymmetry explained:** every box is born `width = UNKNOWN_WIDTH (INT_MAX)`, `height = 0` (`box_manipulate.c:119-120`); note `descendant_x1`/`descendant_y1` are born **0**, not a sentinel (`box_manipulate.c:122`). A box that layout never resolves keeps `(x=0, w=INT_MAX, h=0)`. On the Y axis a height-0 box contributes nothing; on the X axis its `INT_MAX` width blows up `desc_x1`. The failure-tolerant paths that leave such boxes generally **zero the height but never the width** (e.g. `layout_flex.c:1367` sets `b->height = 0` only), which is exactly why X breaks and Y stays clean.

### 3.1 The "exact +INT_MAX" value is NOT a hypothesis discriminator (important)

An earlier draft treated the exact `+INT_MAX` value as evidence favoring a *raw sentinel* (H1) over a *computed* width (H2/H3). **That is a false discriminator.** Given the geometry field types (all `int`, §4.5) and the delta composition (§4.6), all three theories reproduce the exact value:

- **H1 (raw sentinel):** an un-laid box keeps `box->width = INT_MAX` and (never laid out) `padding = border = 0`. In `layout_get_box_bbox`, `desc_x1_own = padL + INT_MAX + padR + borderR = INT_MAX` exactly.
- **H2/H3 (computed near-INT_MAX):** the place-site writes `b->width = tgt - delta` where `delta = lh__delta_outer_width(b) = padL + padR + borderL + borderR + mL + mR` (non-auto margins only). Then in `layout_get_box_bbox`:
  ```
  desc_x1_own = padL + (tgt - delta) + padR + borderR
              = INT_MAX - (borderL + mL + mR)          (for tgt = INT_MAX)
  ```
  For a flex item with **zero left border and zero horizontal margins** (the common case), `desc_x1_own == INT_MAX` **exactly**.

So the exact value only argues **against H4** (an `INT_MIN` auto-margin leak, which would need the `INT_MIN - 1 == INT_MAX` wrap). It does **not** rank H1 above H2/H3. H2 is verified holed in source (§5); nothing in the evidence favors the birth sentinel over the computed value.

---

## 4. The Chain of Computation

### 4.1 The sink: `html_reformat` copies `descendant_x1` into `c->width`

`browser/netsurf/content/handlers/html/html.c:1789-1801`:

```c
c->width = layout->x + layout->padding[LEFT] + layout->width +
    layout->padding[RIGHT] + layout->border[RIGHT].width +
    layout->margin[RIGHT];
...
/* if boxes overflow right or bottom edge, expand to contain it */
if (c->width < layout->x + layout->descendant_x1)
    c->width = layout->x + layout->descendant_x1;   /* html.c:1798-1799 — INT_MAX enters here */
```

`layout->descendant_x1 == INT_MAX` and `layout->x == 0` (`lyt_xy=0,0`), so `c->width` becomes exactly `INT_MAX`. Confirmed surfacing site. The `fixes311` reformat probe that produced the log is immediately below at `html.c:1804-1809`.

### 4.2 The accumulator: `layout_get_box_bbox` — TWO sentinel re-injection sites

`layout_get_box_bbox` — `browser/netsurf/content/handlers/html/layout.c:6733-6770` (verbatim):

```c
*desc_x0 = -box->border[LEFT].width;
*desc_y0 = -box->border[TOP].width;
*desc_x1 = box->padding[LEFT] + box->width + box->padding[RIGHT] +
        box->border[RIGHT].width;                                   /* :6742 */
*desc_y1 = box->padding[TOP] + box->height + box->padding[BOTTOM] +
        box->border[BOTTOM].width;
if (*desc_x0 < -10000) *desc_x0 = 0;
if (*desc_y0 < -10000) *desc_y0 = 0;
if (*desc_x1 > 10000) *desc_x1 = (box->width > 0) ? box->width : 10000;               /* :6748 */
if (*desc_y1 > 10000) *desc_y1 = (box->height > 0) ? box->height : 10000;
if (*desc_x1 <= *desc_x0) *desc_x1 = *desc_x0 + ((box->width > 0) ? box->width : 1);  /* :6750 */
if (*desc_y1 <= *desc_y0) *desc_y1 = *desc_y0 + ((box->height > 0) ? box->height : 1);/* :6751 */
```

There are **two** re-injection paths for a garbage width, gated by whether horizontal padding/border is zero. **All of `box->x`, `box->width`, `box->padding[]`, `box->border[].width`, `box->descendant_x1` are 32-bit `int`** (§4.5), so `INT_MAX + positive` wraps.

- **Zero horizontal padding/border (bare block/text — the common leaf case):** `:6742` gives `*desc_x1 = INT_MAX` exactly. `:6748` fires (`> 10000` true) and reassigns `*desc_x1 = box->width` (still `INT_MAX`, since `INT_MAX > 0`). **The clamp restores the sentinel.** This is why the value is bit-exact.
- **Non-zero horizontal padding/border:** `padding[LEFT] + INT_MAX + …` at `:6742` **signed-overflows to a large negative**. Then `:6748` is *false* (negative), so it does **not** fire — but `:6750` now fires (`*desc_x1 <= *desc_x0`) and re-injects `*desc_x1 = *desc_x0 + box->width`, driving it back to ~`INT_MAX`. **A fix that patches only `:6742-6748` still leaks through `:6750-6751`.**

Both clamps only ever defend against garbage *padding/border*, never a garbage *width*. The Y branch (`:6749`, `:6751`) stays sane because the tolerated-failure paths zero `height`.

`layout_update_descendant_bbox` — `layout.c:6782-6839` — accumulates up the tree with **no ceiling / no saturation**:

```c
/* for overflow_x == VISIBLE and non-HTML-object child, read child's own descendant extent raw: */
child_desc_x1 = child->descendant_x1;    /* :6816 */
...
child_desc_x1 += child_x;                 /* :6827 */
if (box->descendant_x1 < child_desc_x1)
    box->descendant_x1 = child_desc_x1;  /* :6835-6836 */
```

Note: when the child's own `overflow-x` is **not** visible, the raw-read branch at `:6812-6817` is skipped and `child_desc_x1` stays the border-edge value from `layout_get_box_bbox` (`:6808`) — which for an `INT_MAX`-own-width child is *also* `INT_MAX`. Either way an `INT_MAX`-own-width child pushes `INT_MAX` into its parent.

### 4.3 The fixes574 per-axis overflow clip — the propagation gate (previously omitted)

`layout_calculate_descendant_bboxes` applies a **per-axis overflow:hidden clip** when recursing into normal-flow children — `layout.c:6914-6951`:

```c
bool ox_hidden = box->style != NULL &&
        css_computed_overflow_x(box->style) == CSS_OVERFLOW_HIDDEN;
bool oy_hidden = ...overflow_y == CSS_OVERFLOW_HIDDEN;
int save_x1 = box->descendant_x1;  ...
if (ox_hidden && oy_hidden) continue;               /* skip child entirely */
layout_update_descendant_bbox(unit_len_ctx, box, child, 0, 0);
if (ox_hidden) { box->descendant_x0 = save_x0; box->descendant_x1 = save_x1; }  /* restore X */
if (oy_hidden) { ... restore Y ... }
```

This was added (`fixes574`) specifically for the XenForo `.hScroller-scroll` nav bar (`white-space:nowrap; overflow-x:hidden`) so its ~1380 px of non-wrapping content did not expand the document width. **This constrains the leak's propagation path and is a diagnostic lever:**

- A child's contribution to an `overflow-x:hidden` parent is **discarded** (restore). So the `INT_MAX` chain from a leaf up to the root must **not** pass through any `overflow-x:hidden` ancestor via the *child-contribution* path — the leak either (a) rides a chain with **no** `overflow-x:hidden` ancestor between it and the root, **or**
- (b) the leak originates in an `overflow-x:hidden` box's **own** width. The box's `descendant_x1` is first seeded from its own border edge at `layout_get_box_bbox` (`:6860`), *then* children are applied and hidden axes restored to that own-width-derived value. So an `overflow-x:hidden` box with `own width == INT_MAX` still exports `descendant_x1 == INT_MAX` to its parent. **Given XenForo's last-post/nav cells are `overflow-x:hidden` and were exactly fixes622's target, an `overflow-x:hidden` container whose own width computed to INT_MAX is a prime suspect.**

### 4.4 The recursion covers floats, list-markers, and inline-end chains (not just `children`)

`layout_calculate_descendant_bboxes` (`layout.c:6849-6969`) accumulates from **four** sources, all of which can carry an `INT_MAX`-width box:

- Normal-flow `box->children` — `:6907-6952` (with the fixes574 clip).
- `BOX_INLINE_END` sibling walk — `:6886-6900` (walks `box->inline_end->next …`).
- `box->float_children` — `:6954-6960` (floats **are** accumulated into `descendant_x1` at `:6960`).
- `box->list_marker` — `:6963-6967`.

This matters for the diagnostic: fixes624 (§6.5) walks **only** `bx->children`, so a leak in a float, a list-marker, or an inline-end chain is invisible to it — yet all three feed the real accumulator.

### 4.5 The geometry field types — WHY the sentinel wraps (the task's explicit ask)

From `box.h` (all plain `int`, i.e. **32-bit** on CW8 PPC):

| Field | Decl | Line |
|---|---|---|
| `x` | `int x;` | `box.h:301` |
| `width` | `int width;` | `box.h:310` |
| `height` | `int height;` | `box.h:314` |
| `descendant_x0/y0/x1/y1` | `int …;` | `box.h:330-333` |
| `margin[4]` | `int margin[4];` | `box.h:338` |
| `padding[4]` | `int padding[4];` | `box.h:343` |
| `min_width` | `int min_width;` | `box.h:364` |
| `max_width` | `int max_width;` | `box.h:370` |

`struct box_border` (`border[4]`) carries an `int width`. Flex-side `target_main_size`, `base_size`, `available_main` are likewise plain `int` (`layout_flex.c` flex-item struct). **This is load-bearing:** it is *why* `INT_MAX + padding` overflows at `layout.c:6742` (§4.2), *why* `INT_MIN - 1 == INT_MAX` (the H4 coincidence, §5), and *why* the value observed is bit-exact rather than saturated.

### 4.6 `lh__delta_outer_width` composition (needed for the arithmetic in §3.1 and §5)

`layout_internal.h:257-265`:

```c
static inline int lh__delta_outer_width(const struct box *b) {
    return b->padding[LEFT] + b->padding[RIGHT] +
           b->border[LEFT].width + b->border[RIGHT].width +
           lh__non_auto_margin(b, LEFT) + lh__non_auto_margin(b, RIGHT);
}
static inline int lh__non_auto_margin(const struct box *b, enum box_side side) {
    return (b->margin[side] == AUTO) ? 0 : b->margin[side];   /* :242-244 */
}
```

So `delta` = border+padding+**non-auto**-margin, both sides; AUTO margins contribute 0. This is *why* the flex margin-delta path cannot leak an `INT_MIN` (it is masked to 0) and *why* substituting a near-INT_MAX `b->width` reproduces the exact extent (§3.1).

### 4.7 The DEAD assert channel (major correction — the prior draft's mechanism was wrong)

An earlier draft claimed the sentinel "passes silently" because MacSurf redefines `assert()` to a *non-aborting breadcrumb logger*. **That is wrong for `layout.c`.** The real mechanism is that the assert is **compiled out to a no-op**:

1. `macsurf_prefix.h:35-46` sets `#define NDEBUG 1` (unless `MACSURF_REAL_ASSERTS`), then defines a "belt-and-braces" `assert(expr)` macro that calls `macsurf_assert_failed_` (a real breadcrumb writer, `macsurf_debug_log.c`, active under `MACSURF_DEBUG`).
2. **BUT `layout.c:37` does `#include <assert.h>` *after* the prefix takes effect.** Per C §7.2, MSL's `<assert.h>` `#undef`s and re-defines `assert` according to the *current* `NDEBUG`; with `NDEBUG == 1`, `assert(expr)` becomes `((void)0)`.
3. Therefore `assert(box->width != UNKNOWN_WIDTH)` at **`layout.c:6855`** (and `assert(box->height != AUTO)` at `:6856`) compile to **pure no-ops**. They can **never** emit a breadcrumb. The prefix's breadcrumb macro is overridden in exactly the NetSurf-core TUs that include `<assert.h>` (layout.c among them).

Consequences the fix designer must absorb:
- The guard is silent because it is a **compiled-out MSL no-op**, not because a tolerant logger swallowed it. Either way, an `UNKNOWN_WIDTH` box sails into `layout_get_box_bbox`.
- The captured log's **zero `ASSERT`/`LAYOUTPHASE` lines** are explained by this (both are MSL/release no-ops in this TU), **not** by the build being stale. Do not use their absence as evidence about the tree.
- Any diagnostic step that expects a `macsurf_assert_failed_` / `layout.c:6855` breadcrumb is **futile unless the assert is first re-armed** — e.g. build with `-DMACSURF_REAL_ASSERTS`, re-`#define assert` the breadcrumb *after* `layout.c:37`, or add an explicit `macsurf_debug_log_writef` at `:6855`.

### 4.8 The sentinel & AUTO definitions

```c
/* box.h:45-46 */
#define UNKNOWN_WIDTH     INT_MAX   /* == 2147483647 */
#define UNKNOWN_MAX_WIDTH INT_MAX

/* layout_internal.h:27 and layout_safe.h:38 */
#define AUTO INT_MIN                /* == -2147483648 */

/* layout_flex.c:79   FLEX_SAFE_MAX  == 1000000
   layout_safe.h:46   LAYOUT_SAFE_MAX == 1000000  */

/* box_manipulate.c:118-127 — every box born with width/max_width sentinels; extents born 0 */
box->x = box->y = 0;
box->width  = UNKNOWN_WIDTH;        /* INT_MAX */
box->height = 0;
box->descendant_x0 = box->descendant_y0 = 0;
box->descendant_x1 = box->descendant_y1 = 0;   /* NOT a sentinel */
box->min_width = 0;
box->max_width = UNKNOWN_MAX_WIDTH;             /* INT_MAX */
```

---

## 5. Root-Cause Analysis

All theories share one **mechanism** — a box carrying `width == INT_MAX` (raw or computed) propagates through the defeated bbox clamp — and differ only on the **origin**. Ranking below reflects the source-verified state: **H2 and H3 co-lead** (both verified holed in source), H1 is a strong third, H4/H5/H6 trail.

### H2 — fixes623 place-site `max_width` fallback accepts `UNKNOWN_MAX_WIDTH` *(co-leading, verified holed)*

`layout_flex.c:1316-1354` (`fixes623`, verbatim core):

```c
int tgt = item->target_main_size;
int delta = lh__delta_outer_width(b);
...
if (tgt == AUTO || tgt < 0) {
    if (b->max_width != AUTO && b->max_width > 0) {   /* :1336 — NO UNKNOWN_MAX_WIDTH guard */
        tgt = b->max_width;
    } else if (b->min_width != AUTO && b->min_width > 0) {
        tgt = b->min_width;
    } else {
        tgt = 0;
    }
}
b->width = tgt - delta;                                /* :1345 */
```

`UNKNOWN_MAX_WIDTH (INT_MAX)` satisfies `!= AUTO && > 0`, so when an item enters the fallback with `max_width` still at the sentinel, `tgt = INT_MAX` and `b->width = INT_MAX - delta ≈ INT_MAX`. **Contrast** `flex_item_intrinsic_main_size` (`layout_flex.c:134-135`), which *correctly* excludes it: `if (mw != UNKNOWN_MAX_WIDTH && mw != AUTO && mw > 0)`. The place-site simply omits that clause.

- **Why the sentinel is present:** the fork's `box_minmax_invalidate_tree` writes `UNKNOWN_MAX_WIDTH` into every `box->max_width` to force a minmax recompute; any flex item whose recompute didn't repopulate `max_width` carries `INT_MAX` into the place-site.
- **Note on §8's claim:** fixes623 building "and c_w still INT_MAX" does **not** exonerate the place-site. The guard at `:1335` only fires for `tgt == AUTO || tgt < 0`; its fallback then leaks `INT_MAX` *through* the guard (H2), and a huge-positive `tgt` **bypasses the guard entirely** (H3, `tgt > 0`). fixes623 ruled out only the *AUTO/negative* case.
- **Confirm:** instrument `target_main_size` and `b->max_width` at `:1317`/`:1335` for items entering the fallback; a hardened GARBAGEBOX naming a `BOX_FLEX`/flex-item box.
- **Minimal fix:** add `&& b->max_width != UNKNOWN_MAX_WIDTH` at `:1336` (and `&& b->min_width != UNKNOWN_WIDTH` at `:1338`), plus a hard upper clamp on `b->width` after `:1354`.

### H3 — `available_main == AUTO → INT_MAX` free-space seed inflates a grow item *(co-leading, mechanism verified)*

`layout_flex__resolve_line` (`layout_flex.c:1130-1146`, verbatim):

```c
int available_main = ctx->available_main;
...
if (available_main == AUTO) {
    available_main = INT_MAX;      /* :1140-1141 */
}
grow = (line->main_size < available_main);
initial_free_main = available_main; /* :1145 = INT_MAX */
```

`layout_flex__distribute_free_main` (`:1045-1073`) hands that `INT_MAX` free space to a `flex-grow>0` item with no max-width: `result = FMUL(INTTOFIX(remaining_free_main), ratio); item->target_main_size = base_size + FIXTOINT(result)` → `target_main_size ≈ INT_MAX`. `layout_flex__get_min_max_violations` (`:938-1032`) only clamps down when `item->max_main > 0` (`:974`); logs show `max_main=-1`, so no clamp. The negative-only rescue at `:1019` doesn't catch a huge positive. The fixes623 guard at `:1335` only catches `AUTO`/negative, not huge-positive, so `b->width ≈ INT_MAX` at `:1345`.

- **Enabling condition:** `layout_flex_inner` deliberately leaves `available_main == AUTO` unsanitized (`fixes167a`, `layout_flex.c:1946` sanitizes only `!= AUTO`); the containing-block fallback (`~:1884-1919`) can yield `paired = INT_MIN` so `available_main == AUTO` reaches `resolve_line`.
- **Why the fixes614 probe is blind to it:** the `fixes614 clamp` diagnostic gates on `ctx->available_main > 400` (`layout_flex.c:963`). A container with `available_main == AUTO (INT_MIN)` is `< 400`, so it emits **no** clamp line — consistent with every logged container showing sane finite targets while the offender is invisible.
- **Confirm:** targeted probe **inside** `resolve_line` logging `flex ptr + (available_main==AUTO) + each item grow/base_size/target`; or lower/remove the `>400` gate.
- **Correct fix (spec-aligned, §9):** when `available_main == AUTO`, grow does **not** apply (indefinite main size ⇒ no free space) — keep items at `base_size`/max-content instead of seeding `INT_MAX`. Belt-and-braces: clamp `remaining_free_main` to `FLEX_SAFE_MAX` in `distribute_free_main` and add a positive ceiling in `get_min_max_violations` mirroring the `:1019` negative rescue.

### H1 — Un-laid-out subtree left behind by fixes613f flex-item failure tolerance *(strong third)*

`layout_flex.c:1356-1368` (`fixes613f`): `layout_flex_item()` calls `layout_block_context` on a `BOX_BLOCK` cell (`layout_flex.c:377`). If that returns false (e.g. a nested flex/grid inside the cell fails at `layout.c:5232-5240`), the tolerance sets **only** `b->height = 0` (`:1367`) and continues. The failed cell's **descendants are never laid out**, so they keep `width = UNKNOWN_WIDTH (INT_MAX)`; `layout_get_box_bbox` passes that up.

- **Supporting evidence:** the `(w=INT_MAX, h=0)` signature matches "never laid out" boxes; the now-stripped `FLEXITEM FAIL: type=14 width=0` / `type=0 width=31` probe showed items *were* failing layout. fixes623 does not fix this because it guards the item's own width, not its un-laid *children*.
- **Why demoted from "leading":** the exact-value argument that once elevated H1 does **not** discriminate it from H2/H3 (§3.1), and its would-be tiebreaker (an `UNKNOWN_WIDTH` box tripping the `:6855` assert) is **dead** (§4.7). H1 requires a box that *finished* layout still carrying the *birth* sentinel; H2/H3 write a *computed* near-INT_MAX. Nothing in the current evidence favors one over the other, and H2 is verified holed.
- **Confirm:** hardened GARBAGEBOX naming a `type=0` (BOX_BLOCK) or inline/text child under a zero-height flex item, `w=2147483647`, with the item's own padding 0.
- **Fix:** in the fixes613f tolerance, normalize **width** across the degraded subtree (walk descendants, set any `width==UNKNOWN_WIDTH` to 0 or the item's resolved main size), not just height.

### H4 — Auto-margin `INT_MIN` leaking directly into an x/width read *(alternative; exact-value coincidence only)*

Mostly refuted: at the place-site, `extra`/`extra_remainder` are only computed when `available_main != AUTO` (`layout_flex.c:1251-1258`), and `lh__non_auto_margin` (`layout_internal.h:242-244`) returns 0 for `AUTO` everywhere it feeds x/width (`lh__delta_outer_width`, `layout_internal.h:257-265`). The 24 `INT_MIN` margins in the stock box tree are pure `box_dump` display artifacts on finite-width boxes.

- **Kept alive only by:** `INT_MIN - 1` wraps to exactly `INT_MAX = 2147483647` (§4.5). If **any** site reads `box->margin[LEFT|RIGHT]` directly (unguarded by `lh__non_auto_margin`) and does a `margin - 1` / decrement / direct add into a coordinate or used width on the over-wide `margin:0 auto` wrapper, it would produce exactly the observed value.
- **Confirm/refute:** grep the flex/grid/block width & x-position paths for any direct `box->margin[...]` read not routed through `lh__non_auto_margin`. If none exist, H4 is dead and the value match is coincidence.

### H5 — Watchdog degrade paths *(latent, currently inert — ruled out as active)*

`fixes171` degrade sites (`layout.c:4799-4802`, `1086`, `2624`, `4707`; `layout_flex.c:1808`; `layout_grid.c:356`) all do `->height = 0; return true;` **without setting width** — identical shape to H1. But `layout_watchdog_enter` currently **always returns 0** (`layout_safe.h:196-206`, caps removed), so these paths do not fire on this page. Kept on record because re-enabling the caps re-opens the class.

### H6 — Grid-in-flex interaction *(secondary)*

The fork lets flex accept grid children (`fixes174/176`). A grid track/item auto width resolved against an indefinite (`AUTO/INT_MIN`) container inline size could produce `INT_MAX`; the tinkerdifferent node/last-post rows are grid-in-flex. Check `layout_grid.c` track sizing against an `INT_MIN` available inline size.

### Ruled out — normal block width-solve as *originator*

With `available_width = 949` (finite): `layout_solve_width` (`layout.c:1742-1861`) can only shrink (AUTO → `available - margins/borders/padding`, floored to 0; max-width only reduces; min-width only raises to `min_width`). The auto-margin center (`:1829-1842`) clamps `margin[LEFT] >= 0` and folds negatives into `margin[RIGHT]`. `layout_block_find_dimensions` (`:1881-1956`) always assigns `box->width = layout_solve_width(...)` with `max_width == -1` ("none"), not `INT_MAX`. The root `layout_document` (`:7010-7027`) sets `doc->width = 949`. So the block path cannot **originate** `INT_MAX`; `c_w=INT_MAX` is a *propagated* value.

---

## 6. Empirical Evidence Appendix

### 6.1 The reformat/site log (fork, on hardware)

```
[2977] reformat: in_w=949 in_h=613 c_w=2147483647 c_h=11062 desc_x1=2147483647 desc_y1=11062 lyt_xy=0,0 lyt_wh=949,11062   (line 4307)
[2977] SITE boxes=4668 blk=853 inlinec=486 inline=840 text=873 other=1616 in_w=949 in_h=613 c_w=2147483647 c_h=11062 img_ok=43 img_fail=0 css_ok=9 css_skip=0 css_total=657216/2097152   (line 4308)
[3127] (identical second pass at lines 6712/6713)
```

- Root width correct (`lyt_wh=949`); `layout->x=0` ⇒ `c_w == desc_x1`; Y clean.
- Two reformat passes (reflow storm) — possibly self-reinforcing across passes (open question §12).

### 6.2 fixes614 flex clamp table — ALL bounded (flex place-site's *own emitted values* are finite)

4694 `fixes614 clamp` lines, every one bounded. Examples:

```
fixes614 clamp: target=151 base=266 min_main=0 max_main=-1 boxminw=16 horiz=1
fixes614 clamp: target=413 base=598 min_main=0 max_main=-1
```

Distribution: `min_main ∈ {0 (×3222), 200 (×1472)}`; `max_main ∈ {-1 (×4670), 909 (×24)}`; `target` range **145..891** (top: 891×2, 670×4, 659×16, 630×8, 613×160, 418×1472, 413×1472, 196×1472). **No negative, no INT-scale value.** — **Caveat:** this probe gates on `available_main > 400` (`layout_flex.c:963`), so an `available_main == AUTO` container (H3) or the specific item that leaks is **invisible** to it. Bounded clamp lines do **not** exonerate the flex path.

### 6.3 Container probes (`hdr:`) — all FINITE, all tiling correctly

```
uix_pageWrapper--fixed / p-pageWrapper  t=14 x=0   w=949 h=11047  dy=(-17..11072)
p-body                                  t=14(FLEX) x=0   w=949 h=9724
p-body-inner                            t=14(FLEX) x=20  w=909 h=9684
p-body-main p-body-main--withSidebar    t=14(FLEX) x=20  w=909
p-body-content                          x=20  w=659
p-body-pageContent                      x=20  w=659
p-body-sidebar                          x=679 w=250
p-body-header                           t=0   x=0   w=949 h=121
p-navSticky ... uix_stickyBar           x=0   w=949 h=56
p-sectionLinks                          x=0   w=949 h=36
```

`content(659) + sidebar(250) = 909` tiles perfectly inside `main(x=20 w=909)`. `t=14 = BOX_FLEX`, `t=0 = BOX_BLOCK`. **These probes print descendant-Y (`dy`) only, not descendant-X**, so they cannot reveal the X leak; the offender is a deeper leaf whose X-extent bubbles into `descendant_x1` without changing any named container's own width.

### 6.4 What did NOT appear in the captured log

- **No `GARBAGEBOX` line** — see §6.5 for what that implies.
- **No `FLEXITEM FAIL` line** (probe stripped).
- The only other `2147483647` hits (lines 6744/6779/6815, `FAIL diag ot_err=2147483647`) are an **unrelated TLS error sentinel**.
- **No `ASSERT`/`LAYOUTPHASE` lines** — explained by §4.7 (compiled-out MSL no-op in `layout.c`), **not** by a stale build. Do **not** infer "tree is sentinel-free."

### 6.5 fixes624 diagnostic — ran-and-found-nothing is *positive evidence*, and the walk is holed

`html.c:1811-1851` (verbatim structure): fires when `layout->descendant_x1 > 1000000 || < -1000000`; one-shot (`static td_garbage_dumped`); iterative DFS over a fixed `struct box *stack[256]`, stops after `found < 14`, and pushes children only while `sp < 256`:

```c
stack[sp++] = layout;
while (sp > 0 && found < 14) {
    struct box *bx = stack[--sp];
    if (bx->width > 1000000 || bx->width < -1000000 ||
        bx->x > 1000000 || bx->x < -1000000) {
        macsurf_debug_log_writef("GARBAGEBOX type=%d x=%d w=%d dx1=%d mL=%d mR=%d", ...);
        found++;
    }
    for (ch = bx->children; ch != NULL && sp < 256; ch = ch->next)
        stack[sp++] = ch;   /* children ONLY; floats/list_marker/inline_end NOT walked */
}
```

**Reconciliation (previously "AWAITING"):** the trigger condition (`descendant_x1 > 1e6`) is *true* in this log (lines 4307, 6712), and the `fixes311` reformat probe two lines above **is present** in the log — strong evidence the binary was built from source containing fixes624 (same function, same TU). Under that reading, **the one-shot walk ran and emitted nothing**, which is *positive* evidence the leak box is **unreachable by a children-only, stack-256, `found<14` DFS** — i.e. it is a **float / list_marker / inline_end** box (§4.4) or lies **beyond the truncated 256-stack** over the 4668-box tree. (If instead the binary predates fixes624, the marker must be confirmed before trusting the negative — reconcile which by grepping the exact binary's source.)

**Holes to fix before trusting any negative:** children-only (skips `float_children`, `list_marker`, `BOX_INLINE_END` chain — all live accumulators, §4.4); 256-slot stack silently drops subtrees; `found<14` cap. It *does* check both `>1e6` and `<-1e6` on `bx->x` and `bx->width`, so it would flag the box **if reached**.

### 6.6 Stock netsurf-gtk box-tree evidence (`netsurf_gtk_boxtree.txt`, 4547 lines)

Root (line 1):

```
<HTML> x0 y0 w984 h14787 min1297 max8381 desc(-2 -7 1346 14787) m(0 0 0 0) BLOCK ID:XF
```

- `desc_x1 = 1346` **FINITE**; window `w984`; **min-content 1297** (> 984 → normal finite horizontal overflow + ordinary scrollbar).
- min-content chain: lines 1, 2 (BODY), 4/5 (FLEX ID:top), 150 (FLEX w1297), 151 (FLEX w1200 x49, `m(0 -IM 0 -IM)` = `max-width:1200; margin:0 auto`), 167/168 (FLEX w1297), 171 (BLOCK w1297), … 2495.
- min-content-defining flex row (line 2495): `x0 y0 w1297 h164 min1297 max1353 FLEX <DIV>` (a thread-list item row).
- The separate `overflow:hidden; nowrap` last-post cell (the `min999` second bug fixes622 targets):

```
line 2542: FLEX x66 w999 min999 max1009            (display:flex, overflow-x:visible, white-space:normal)
line 2543:   BLOCK <DIV> x0 w999 min999 max1009     (overflow-x:hidden, overflow-y:hidden, white-space:nowrap)
line 2544:     INLINE_CONTAINER w999
line 2546:       TEXT 5275 'Page Buffer Capture from Radius FPD/SE VRAM ...' w999
```

Stock computes the clipping cell's min-content as the **full unclipped text width** (`min999`), which per CSS §4.5 should be 0 for `overflow != visible` — this `999` feeds the flex-row min up to `1297`. **Independent of the INT_MAX bug** (fixes622 addresses it).

### 6.7 The 24 INT_MIN margins (all display artifacts on FINITE boxes)

All are auto-margins printed by `box_dump` as `INT_MIN`, every one on a finite-width box: lines 10 (FLEX w1200 `m(0 -IM 0 -IM)` = `margin:0 auto`), 17 (BLOCK w716), 58 (FLEX w284), 98/99 (FLEX w1200), 121 (FLEX w1160), 140 (FLEX w1200), 144 (BLOCK w202), 147 (FLEX w18), 151 (FLEX w1200 — the content container), 168 (FLEX w1297), the 11 `x1258/1259 w24 h18 min0 max0 FLEX m(0 -2147483648 0 0)` `margin-left:auto` icon buttons at lines 183/467/1044/1304/1498/1695/1821/2130/2325/2490/2822, plus 4429/4495 (FLEX w1200). **Every one has a finite `w`.** This is the key contrast: **the fork lets an `INT_MAX` *width* sentinel enter arithmetic; stock never does.**

---

## 7. Stock-NetSurf vs MacSurf-Fork Divergence

| | Stock netsurf-gtk (Debian 3.11) | MacSurf fork |
|---|---|---|
| Root `desc_x1` | **1346** (finite) | **2147483647** (INT_MAX) |
| min-content | 1297 | (overflows to INT_MAX before this matters) |
| Behavior | `w1297 > w984` window → ordinary finite horizontal scrollbar | INT_MAX content → giant empty canvas + spurious 2.1-billion-px scrollbar |
| `INT_MIN` margins | display artifact only, finite widths computed | also mostly display artifact — but a **width sentinel** leaks into arithmetic |
| `assert(width != UNKNOWN_WIDTH)` | **aborts** (debug) / never reached (release lays out every box) | **compiled out to `((void)0)`** in `layout.c` (`<assert.h>` after `NDEBUG=1`, §4.7) → sentinel sails through |

**Fork modifications that plausibly cause the divergence:**

1. **Failure-tolerant layout** — `fixes613f` (flex holds when a sub-item fails), `fixes167` (flex survival), `fixes171` (watchdog degrade), `fixes173` (engine survives whatever given). Stock aborts/propagates a failure; the fork keeps the tree and continues, leaving un-laid boxes at `UNKNOWN_WIDTH`.
2. **Compiled-out assert** — `<assert.h>` re-included after `NDEBUG=1` (§4.7). Removes the exact guard that would catch this.
3. **Sentinel-defeating bbox "clamps"** — `layout_get_box_bbox:6748` and `:6750` both restore `box->width` when it is huge-positive/overflowed; well-intentioned clamps that are no-ops for the one input that matters.
4. **fixes623 place-site fallback** — accepts `UNKNOWN_MAX_WIDTH` as a real target width (`:1336`).
5. **`available_main==AUTO → INT_MAX`** seed (`:1140-1141`) with no down-clamp for grow items lacking a max.
6. **fixes174/176 flex-accepts-grid + intrinsic main-size** — a grid-in-flex surface not present in the same form upstream.

**Fork flex fix history (git):** `d09869b8` fixes613f, `66f7b994` fixes176 (flex intrinsic main-size), `22ac2678` fixes174 (flex accepts grid children; CSS cap → 1MB), `b4530c3e` fixes171 (watchdog caps), `5c844995` fixes170, `5171c891` fixes167 (flex survival), `be17ccdc` fixes166 (FLEXPHASE probes), `8278ba6e` fixes148 (gap parsing). layout.c: `7e7e8c78` fixes194, `cc426386` fixes173, `b821877f` fixes128, `f6aa1839` fixes202, `be2e375d` fixes179, and the fixes574 per-axis overflow clip. Working-tree fixes622/623/624 uncommitted.

---

## 8. What Has Been Tried (fixes620-624)

| Fix | Change | Rationale | Observed effect |
|---|---|---|---|
| **fixes620** | base-bg (`box_html` canvas bg) + rgba alpha compositing + TLS-resumed observability | make the page base dark like Abyss | base-bg **works**; **unrelated** to split |
| **fixes621** | Cmd `-`/`+`/`0` page zoom (`browser_window_set_scale`) | usability | zoom-out 0.75 → viewport `949/0.75≈1265` can exceed min-content so page fits — **mitigation, not a fix** |
| **fixes622** | `layout_minmax_block` — reset content-derived min to 0 for `overflow-x != visible` (CSS §4.5 auto-min-0; `layout.c` minmax path) | fixes the SEPARATE `min999` overflow:hidden bug | correct per spec; **did NOT change `c_w=INT_MAX`** |
| **fixes623** | `layout_flex.c` place-site (`:1316-1354`): guard `b->width = target - delta` for `target==AUTO`/`<0`; resolve to max/min-content, floor to min-content, clamp `>=0` | catch AUTO/negative targets at place-site | built & live (old FLEXITEM FAIL probe disappeared); **`c_w` STILL = INT_MAX** |
| **fixes624** | one-shot `GARBAGEBOX` DFS in `html_reformat` (`html.c:1811-1851`) logging any box whose own `x`/`width` > 1e6 | name the exact leak box | **ran, emitted nothing** (§6.5); walk is children-only + stack-256-truncated |

**Corrected takeaway (was self-contradictory):** fixes622 and fixes623 both left `c_w=INT_MAX` untouched. fixes623 ruled out **only the AUTO/negative case at the place-site** — it did **not** exonerate the place-site itself: the `max_width` fallback (H2) leaks `INT_MAX` *through* the guard, and a huge-positive `tgt` (H3) *bypasses* the guard (`tgt > 0`). So "fixes623 ran and c_w still INT_MAX" is fully consistent with the leak being **at `layout_flex.c:1345`**. The leak is a **near-INT_MAX width reaching the bbox walk**, most consistent with **H2 and H3 (co-leading)**, with **H1** a strong third.

---

## 9. Relevant CSS Spec Rules

1. **Flexbox §4.5 — Automatic Minimum Size of Flex Items** (`w3.org/TR/css-flexbox-1/#min-size-auto`): automatic min = smaller of content-size (min-content) and specified/transferred size, **BUT "if the item's overflow property is not visible, the automatic minimum size is 0."** Validates `fixes622`. Note: for `white-space:nowrap` the min-content genuinely IS the full unbroken text width — so the fork's `min999` is a *correct min-content*; the bug is only that `overflow-x:hidden` should drop the flex auto-min to 0. **Separate from the INT_MAX leak.**
2. **Flexbox §8.1 — Aligning with `auto` margins** (`w3.org/TR/css-flexbox-1/#auto-margins`), verbatim: *"auto margins are treated as 0 [during calculations of flex bases and flexible lengths]. Prior to alignment via justify-content and align-self, any positive free space is distributed to auto margins in that dimension. Overflowing elements ignore their auto margins and overflow in the end/foot direction."* → `margin:auto` on an over-wide box → treat as **0**, never negative, never a sentinel. **And: an indefinite (auto) main size means there is no positive free space to distribute — so grow must not apply (directly relevant to H3's `INT_MAX` seed).**
3. **CSS 2.1 §10.3.3 — Block, non-replaced, normal flow** (the `html > body > wrapper` chain): an over-constrained `auto` margin resolves to **0** (in LTR the over-constraint is absorbed by forcing `margin-right`). Real browsers resolve `margin:auto` on an over-wide box toward 0, never toward negative infinity.

**Spec bottom line:** every canonical rule collapses an unresolvable/over-constrained auto margin *or* an indefinite main size to **0** for arithmetic. The fork's bug is letting a **sentinel** (`INT_MAX` width, or the `INT_MAX` `available_main` seed) enter arithmetic instead of collapsing to 0.

---

## 10. Online Research Findings

- **AUTO = INT_MIN is the UPSTREAM NetSurf convention**, not a MacSurf invention (upstream `layout_internal.h`; NetSurf doxygen `ci.netsurf-browser.org/jenkins/.../layout_8c.html`). Stock's `box_dump` `m(0 -2147483648 0 -2147483648)` is a display artifact of an **unresolved auto-margin sentinel**, not a value fed into arithmetic.
- **NetSurf flexbox is new and immature:** `display:flex` first shipped in **NetSurf 3.11 (2023)** via **libcss 0.8.0** (`netsurf-browser.org/about/news.html`; the `netsurf-users` "LibCSS: Flexbox property support review" thread). A young, lightly-hardened code path — consistent with the fork hitting an over-constrained edge upstream doesn't exercise the same way (upstream lays out every box; the fork's survival layers leave boxes un-laid).
- **No upstream NetSurf bug matches the INT_MAX descendant-bbox leak.** Mantis surfaced only different-flavor overflow bugs: **#2689** "Unwanted vertical scroll bars on content" (overflow-x, fixed 3.10) and **#1535** "Broken html layout table." Conclusion: the `2147483647` leak is **MacSurf-fork-specific arithmetic**.
- **Upstream `layout_flex` auto-margin distribution** computes `extra = available_main - used_main_size` and distributes even when negative, but survives because unresolved auto margins are read back through `lh__non_auto_margin()` which returns 0. The fork mirrors this helper (`layout_internal.h:242-244`), so the **margin-delta path is not the leak** — which is why H4 is refuted except for the `INT_MIN-1` direct-read coincidence.

URLs: `w3.org/TR/css-flexbox-1/#min-size-auto` (§4.5) · `w3.org/TR/css-flexbox-1/#auto-margins` (§8.1) / `drafts.csswg.org/css-flexbox-1/` · `w3.org/TR/CSS21/visudet.html#blockwidth` (2.1 §10.3.3) · `netsurf-browser.org/about/news.html` · `ci.netsurf-browser.org/jenkins/` · `raw.githubusercontent.com/netsurf-browser/netsurf/master/content/handlers/html/layout_flex.c` · NetSurf Mantis #2689, #1535.

---

## 11. Recommended Next Diagnostic Steps & Likely Fix Shape

### 11.1 Diagnostics (do these first — one log line settles the origin)

1. **Harden `fixes624` and re-capture (top priority).** Before trusting a negative:
   - Raise/remove the `stack[256]` cap (`html.c:1845`) — a 4668-box tree overruns it.
   - Also walk `float_children`, `list_marker`, and the `BOX_INLINE_END`/`inline_end` chain, not just `bx->children` (§4.4).
   - Then capture a run where `descendant_x1 > 1e6` AND the diag fires. The `GARBAGEBOX type= x= w= dx1= mL= mR=` line **discriminates all hypotheses**:
     - flex-item box, `w ≈ INT_MAX - delta`, item's `max_width==UNKNOWN_MAX_WIDTH` → **H2**.
     - grow item in an `available_main==AUTO` container → **H3**.
     - `type=0` (BOX_BLOCK)/inline/text child under a zero-height flex item, `w=2147483647`, padding 0 → **H1**.
     - garbage `x` rather than `w` → x-leak variant.
     - box is a float / list-marker / inline-end box → confirms the §6.5 inference and the propagation path.
2. **Probe inside `layout_flex__resolve_line`** (`layout_flex.c:1130`): log `flex ptr`, `(available_main==AUTO)`, and each item's `grow/base_size/target`. Lower or remove the `>400` gate on the `fixes614` clamp probe (`layout_flex.c:963`) so an `available_main==AUTO` container becomes visible. **Also log `b->max_width` at `layout_flex.c:1335`** for items entering the fallback — this settles **H2 vs H3** directly (sentinel `max_width` ⇒ H2; huge positive `target` with finite `max_width` ⇒ H3).
3. **~~Grep for a `layout.c:6855` assert breadcrumb~~ — DROP unless the assert is re-armed.** Per §4.7 that site is a compiled-out MSL no-op and can never emit. To use it, build with `-DMACSURF_REAL_ASSERTS`, or add an explicit `macsurf_debug_log_writef` at `:6855` logging `box->type`/`box->x`. A hit would confirm a raw `UNKNOWN_WIDTH` box (H1) over a computed width (H2/H3).
4. **Grep the flex/grid/block width & x paths** for any direct `box->margin[LEFT|RIGHT]` read not routed through `lh__non_auto_margin`, especially `margin - 1` / decrement (settles H4 via `INT_MIN-1 == INT_MAX`).
5. **Check the `overflow-x:hidden` own-width path (§4.3):** log any box with `overflow-x:hidden` whose own `width > 1e6` — a prime H2/H3 suspect given XenForo's last-post/nav cells.

### 11.2 The likely fix (layered — all compatible, probably all wanted)

**A. Catch-all at the propagator (highest leverage, single choke point).** In `layout_get_box_bbox`, treat an unusable width as 0 *before* computing `desc_x1`, mirroring how a height-0 box already keeps Y clean — **and remove BOTH defeated clamps (`:6748` AND `:6750`)**, which each re-inject the sentinel (§4.2):

```c
{
    int bw = box->width;
    if (bw == UNKNOWN_WIDTH || bw < 0 || bw > LAYOUT_SAFE_MAX)
        bw = 0;                       /* never let the sentinel/overflow become an extent */
    *desc_x1 = box->padding[LEFT] + bw + box->padding[RIGHT] +
               box->border[RIGHT].width;
}
/* replace the two `if (*desc_x1 ...) *desc_x1 = box->width;` re-injections at :6748 and :6750
   with a finite saturating clamp that never restores box->width when the width was garbage. */
```

Optionally add a hard saturating ceiling in `layout_update_descendant_bbox` (`:6835`) so no single child pushes a parent past `LAYOUT_SAFE_MAX`. Safest of all: **skip** a child whose `width == UNKNOWN_WIDTH` entirely (it was never laid out and has no valid geometry).

**B. Source fix at the flex place-site (H2).** `layout_flex.c:1336-1338`:

```c
if (tgt == AUTO || tgt < 0) {
    if (b->max_width != AUTO && b->max_width > 0 &&
        b->max_width != UNKNOWN_MAX_WIDTH)          /* <-- add (mirror :135) */
        tgt = b->max_width;
    else if (b->min_width != AUTO && b->min_width > 0 &&
             b->min_width != UNKNOWN_WIDTH)         /* <-- add for the min fallback */
        tgt = b->min_width;
    else
        tgt = 0;
}
...
if (b->width > FLEX_SAFE_MAX)                        /* hard upper clamp after :1354 */
    b->width = (ctx->available_main > 0) ? ctx->available_main : FLEX_SAFE_MAX;
```

**C. Source fix for the AUTO free-space seed (H3) and the un-laid subtree (H1).**
- **H3:** in `layout_flex__resolve_line` (`:1140-1141`), do **not** substitute `available_main = INT_MAX`. Per §8.1, indefinite main size ⇒ grow does not apply — keep items at `base_size`/max-content (free space = 0). Belt-and-braces: clamp `remaining_free_main` to `FLEX_SAFE_MAX` in `distribute_free_main` (`:1067-1071`) and add a positive ceiling in `get_min_max_violations` (`:974`/`:1019`).
- **H1:** in the `fixes613f` tolerance (`:1367`), when zeroing a failed item's height, **also normalize width** across the degraded subtree (walk descendants, set any `width==UNKNOWN_WIDTH` to 0 or the item's resolved main size). Same for the latent watchdog degrade sites if ever re-enabled.

**D. Belt-and-suspenders at the sink.** At `html.c:1798`, reject `descendant_x1 > ~200000` (mirroring the documented `redraw.c ±200000` clamp gotcha) so a future regression can never surface a 2.1-billion-px canvas. A guard, not a substitute for A-C.

**CW8/C89 constraints:** no `//`, no `for (int …)`, all declarations at top of block, no `long long` fixed-point (miscompiles on CW8 PPC), `AUTO=INT_MIN`, `UNKNOWN_WIDTH=INT_MAX`. Use existing `FLEX_SAFE_MAX` / `LAYOUT_SAFE_MAX` (both `1000000`) as ceiling constants. Remember all geometry fields are 32-bit `int` (§4.5): saturate *before* any `INT_MAX + padding` can overflow.

---

## 12. Open Questions for the Higher Agent

1. **Which exact box does the hardened `fixes624` walk name** (type / x / width / dx1 / mL / mR), and is it reached only after adding float/list_marker/inline_end traversal? Is `w` the raw sentinel `2147483647` (→ H1 or an H2 fallback with delta 0) or slightly under (`INT_MAX - delta` → computed, H2/H3 with non-zero border/margin)? Recall (§3.1) that zero-left-border + zero-margin makes even a *computed* width bit-exact `INT_MAX`, so the value alone will not fully separate H1 from H2/H3 — the box *type* and its `max_width` will.
2. **Fix ordering / scope:** ship the **catch-all propagator guard (A)** first (single choke point, stops the whole class), the **source fixes (B/C)** second (correctness), or both in one round? A alone stops the visible bug but *masks* silently-wrong geometry to 0; B/C fix the origin.
3. **Should the descendant walk skip `UNKNOWN_WIDTH` children entirely** rather than substituting 0?
4. **Is the `INT_MAX` self-reinforcing across the two reformat passes?** Is `available_width`/`content_max_width` ever seeded from a parent's `c->width`/`box->width` that was itself `INT_MAX` on a prior partial pass (reflow storm)?
5. **Does the fork read `box->margin[]` directly anywhere** in the flex/grid/block width/x path, unguarded by `lh__non_auto_margin` (the `INT_MIN-1 == INT_MAX` coincidence, H4)?
6. **Is the leak box `overflow-x:hidden` with an INT_MAX own width** (§4.3)? If so, the fixes574 restore preserves it — B/C at the source is required, A is the safety net.
7. **fixes622 verification:** is the `overflow:hidden` min-content-0 reset actually *reached* for the offending last-post cell, or does a `!lh__box_is_flex_item(block)` fixed-width branch gate flex items differently? Confirm it does not feed a grow item once the INT_MAX path is fixed.
8. **Grid-in-flex (H6):** do the tinkerdifferent node/last-post rows resolve a grid track/item auto width against an `INT_MIN` container inline size (`layout_grid.c`)?

---

*End of report.*
