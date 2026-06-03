# JS→DOM→render: the re-convert-after-mutation plan (build blueprint)

How MacSurf paints JS-built SPA content (Facebook's feed, and any modern SPA). Output of the
spa-render-research workflow (3 research agents + design + 2 adversarial reviews). The reviews
caught **three guaranteed-crash bugs** in the first design — they are integrated below as
**mandatory guards**. Build to *this* doc, not the raw design.

## The load-bearing insight (confirmed)
**DOM nodes persist across a box-tree rebuild. We recompute only the disposable BOX tree from the
unchanged DOM; JS keeps mutating the same DOM.**
- JS element wrappers hold `__el = dom_element*` (macsurf_js_dom.c:196), refcount-pinned. These
  point into the **libdom document** (owned by `htmlc->document`), which `talloc_free(htmlc->bctx)`
  does **NOT** touch (bctx owns only `struct box`). So **JS wrappers never go stale** across
  re-converts. The box tree is a projection we discard and recompute; the DOM is the source of
  truth JS owns.
- Box construction and JS execution are **separate** — `dom_to_box` never calls `js_exec`
  (script exec is the parser path, html.c:1108). **Scripts do NOT re-run on re-convert** → no
  re-convert storm.

## End-to-end flow
```
JS mutates real DOM (appendChild/setAttribute/textContent/insertBefore on real __el)
  → macos9_js_mark_dom_dirty()  → macos9_schedule(DEBOUNCE, reconvert_cb, gw)   [schedule dedups burst→1]
  → (top of next poll pass, BEFORE updateEvt → redraw-safe) reconvert_cb fires
  → html_reconvert(htmlc):  teardown guards → talloc_free(bctx) → dom_to_box(html_reconvert_done)
  → html_reconvert_done:  imagemap_extract → browser_window_schedule_reformat
  → html_reformat: layout_document over FRESH layout → GW_EVENT → repaint.  PAINT.
```
Redraw-safety is structural: `macos9_schedule_run` (main.c:848) runs before `updateEvt` dispatch
in the same `WaitNextEvent` return, so a re-convert can never fire mid-redraw.

## Milestones

### M1 — wire document.body / documentElement / head to REAL wrapped elements
- **macsurf_dom_dispatch.c**: add out-of-line wrappers `macsurf_dom_html_document_get_body`
  (`dom_html_document_get_body`) and `macsurf_dom_document_get_document_element`. `#include <dom/html/html_document.h>`.
- **macsurf_js_dom.c**: externs + getter C fns `macsurf_document_get_body/_documentElement/_head`
  → `macsurf_push_element(duk, el)` (NULL→js null). Register as **getter** accessors on `document`
  (DUK_DEFPROP_HAVE_GETTER) before the `"document"` put (:919).
- **macsurf_js.c**: delete the `document.body=...||null` lines (:473-475) AND the fixes379 `mkEl()`
  body/documentElement/head fakes (:962-987) — **keep** the Promise.all/etc. polyfills in that block.
- **clientWidth regression guard:** the real body wrapper must still answer `.clientWidth/.clientHeight`
  (FB viewport code). Add those as getters on the element wrapper (return viewport w/h), OR keep a
  thin JS shim that copies clientWidth onto document.body after wiring. **Do not regress fixes379.**
- **Verify:** `document.body.appendChild(document.createElement('div'))` → body child count +1
  (walk via macsurf_dom_node_get_first_child/get_next_sibling). Real `el=%p` differs from the fake.

### M2 — the safe re-convert routine (manual trigger first)
`html_reconvert(htmlc)` + dedicated `html_reconvert_done` (do NOT reuse `html_box_convert_done` —
it destroys the parser + re-fires set_ready). **Teardown order is mandatory (review fixes):**

1. **Guard:** if `htmlc->reflowing` → return NSERROR_NEED_DATA (caller re-arms). Never free mid-layout.
2. **One-in-flight:** if `htmlc->box_conversion_context != NULL` → re-arm, don't restart (NOT the
   useless `g_reconvert_in_progress` flag — the worker is async). Cancel only a truly stale ctx.
3. **HAZARD-1 guard (mandatory):** before any free, **walk the existing box tree; for each
   `box->node != NULL`, clear its box user_data** (`dom_node_set_user_data(node, box-node key, NULL...)`).
   Else nodes that skip box-gen (display:none, script/style/head/meta) keep dangling box* → UAF in
   `box_for_node(parent)`.
4. **HAZARD-2 guard (mandatory):** neutralize `htmlc->object_list` — null each `object->box` and
   stop/relink in-flight image fetches, else async `html_object_done` writes `box->object` into a
   freed box → heap corruption. (Re-convert spawns fresh fetches; old entries must not dangle.)
5. **HAZARD-3 guard:** forms (`fc->box=NULL` for all controls), `imagemap_destroy`, recreate
   selection. (The design's "biggest hazard" — real but secondary to 1 & 2.)
6. **B3 null-deref guard (mandatory):** do **NOT** null `htmlc->layout` while a same-pass deferred
   reformat can deref `layout->style` (html.c:1290). **Double-buffer:** keep the old tree until
   `html_reconvert_done` swaps in the new one; OR null-guard `html_reformat` (bail if layout==NULL).
7. `talloc_free(bctx); bctx=NULL;` (NULL immediately — prevents double-free vs html_free_layout/destroy).
8. `dom_document_get_document_element` + `html_get_dimensions` + `dom_to_box(html, htmlc,
   html_reconvert_done, &htmlc->box_conversion_context)`.

`html_reconvert_done(c, ok)`: `box_conversion_context=NULL`; if ok → `imagemap_extract` →
`browser_window_schedule_reformat`. Clear the `reconverting` flag here (set it in html_reconvert).
- **Verify:** a JS-appended `<div>` with text PAINTS after one manual trigger.

### M3 — dom-dirty flag + debounced auto re-convert (+ the M4 floor, pulled forward)
- **NEW file `macos9_reconvert.c`/.h** (→ add to MacSurf.mcp). `macos9_js_mark_dom_dirty()` +
  `macos9_reconvert_cb`. `RECONVERT_DEBOUNCE_MS=400` (G3-tunable up to 800-1000).
- Call `macos9_js_mark_dom_dirty()` after each successful mutation in macsurf_js_dom.c
  (appendChild :700, setAttribute :668, setTextContent :567 — and **delete the fixes320j inline
  reformat block :571-598**, superseded by the debounce). insertBefore when added.
- **Pull M4 into M3 (perf review):** FB's feed mutates continuously, so a full re-convert per
  debounce can livelock a 233 MHz G3. Ship M3 WITH: one-in-flight (box_conversion_context guard) +
  a **min-interval FLOOR** ("never start a re-convert if now - last_reconvert_end < FLOOR"), FLOOR
  set from a measured `macsurf_profile_stamp("reconvert-done")` in perf/history.csv. Instrument first.
- **Verify:** ONE `reconvert: done` per mutation burst (not N); content paints ~debounce after the
  burst, no manual trigger; perf stamp captures cost.

### M4 — hardening
innerHTML real (libhubbub fragment parse → splice → mark dirty) only if M3 shows FB assigns it;
createTextNode real wrapper; insertBefore; per-window `reconvert_in_progress`; bounded re-convert
count; debounce/floor tuning from perf data.

## Files
Edit: macsurf_dom_dispatch.c, macsurf_js_dom.c, macsurf_js.c, html.c (core), + NEW macos9_reconvert.c/.h.
**MacSurf.mcp:** add `macos9_reconvert.c` (M3). No other .mcp change.

## Verdict from review
No infinite loop (scripts don't re-run; bursts dedup; one-in-flight serializes). The crash hazards
(1,2,6) are the real risks — all have mandatory guards above. Melt risk on G3 = mitigated by the
floor + one-in-flight pulled into M3. **Safe to build to this doc.**
