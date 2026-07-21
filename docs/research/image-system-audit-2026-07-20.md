# Image system audit — how images load, why they squish/disappear, and how to stop chasing it

**Date:** 2026-07-20 · **Tree:** master @ fixes933 · **Scope:** the whole `<img>` path from box construction → fetch → decode → layout → plot, plus lazy loading and the JS reconvert interaction.

This is a *charting* document, not a fix. Every claim below is source- or log-verified with `file:line`. It exists so we stop patching one symptom at a time (929→930→931→932→933 is five rounds on one knot) and instead fix the structure that produces all of them.

---

## 0. TL;DR — the three things that actually matter

1. **The observability is off, so every recent round has been half-blind.** The `WORK ` development channel is gated behind `MACSURF_WORK_LOG`, which is **defined nowhere in the tree** (only referenced in [macsurf_debug_log.c:668](../../browser/netsurf/frontends/macos9/macsurf_debug_log.c#L668)). A `WORK ` line survives the release gate **only if it also contains a crash keyword** (`FAIL`/`ERROR`/…). So of fixes933's three "disappear" probes, `WORK objdone ZERO-DIM` and `WORK getint ZERO-NAT` are **silently dropped**; only `QT-REDECODE-FAIL` (contains "FAIL") reaches disk. The disambiguating probes never wrote. **Fixing logging means fixing the channel first.**

2. **The fixes929/931 size memo is provably 100% inert on hardware.** Across every recent log, every `LIFE imgdims` line reads `stored=0 hit=0`, with `miss` climbing to 282. Nothing has *ever* recorded an image's size, so nothing has *ever* read one back. The memo — the maintainer's intended squish fix — has never once fired. Reading the code cannot say why (the store at [object.c:164-168](../../browser/netsurf/content/handlers/html/object.c#L164) is unconditional when width>0); **only a live value can**, and the probe that would carry it is the one gated off in point 1.

3. **There is no single "image."** An on-screen image is the coincidence of *four* independent facts, held in *four* places, written and cleared by *different* code on *different* threads of control:
   - `box->object` (the live handle) — [box.h:471](../../browser/netsurf/content/handlers/html/box.h#L471)
   - `box->obj_w/obj_h` (the fixes929 size memo on the box) — [box.h:529](../../browser/netsurf/content/handlers/html/box.h#L529)
   - the global `g_imgdims[512]` URL→size table — [box_special.c:1482](../../browser/netsurf/content/handlers/html/box_special.c#L1482)
   - `c->width/c->height` on the decoded content — [macos9_image.c:1665](../../browser/netsurf/frontends/macos9/macos9_image.c#L1665)

   plus **two** fetch paths (eager vs lazy) and a reconvert that churns the whole set. Squish and disappear are what happens when these four fall out of agreement — which the current design has no single owner to prevent. **The overhaul is: one authoritative per-image record, one fetch path, and layout that trusts `IS_REPLACED` instead of re-deriving "is this an image?" from whether a size happens to be present.**

---

## 1. The pipeline as it exists (the chart)

MacOS9 does **not** use NetSurf's core `content/handlers/image/*` decoders. It ships its own QuickTime/lodepng content handler, `macos9_qt_image_*`, registered at [macos9_image.c:2250](../../browser/netsurf/frontends/macos9/macos9_image.c#L2250). So the entire decode/plot story lives in the frontend.

```
                      ┌─────────────────────────── BOX CONSTRUCTION ───────────────────────────┐
                      │ box_image()  box_special.c:1702                                          │
  dom <img> ─────────►│  • box->flags |= IS_REPLACED            (:1756)                          │
                      │  • imgdims_lookup(url) → seed box->obj_w/obj_h if HIT  (:1764)  ── memo R │
                      │  • if CSS w&h set & non-%  → box->flags |= REPLACE_DIM (:1792)            │
                      │  • FETCH DECISION (:1779):                                                │
                      │       img_eager_budget>0 ? html_fetch_object (EAGER)                      │
                      │                          : macsurf_lazyimg_defer (LAZY, budget=10)        │
                      └───────────┬───────────────────────────────────────┬─────────────────────┘
                                  │ eager                                 │ lazy (11th+ image)
                                  ▼                                       ▼
                      ┌───────── FETCH ──────────┐          ┌──── g_lazyimg_head (GLOBAL) ────┐
                      │ html_fetch_object         │          │ entry{content,node,url}         │
                      │  object.c:952             │          │ dedupe key = (content,node)     │
                      │  • object->box = box      │          │ drained on PAINT only:          │
                      │  • base.active++ (if box) │          │  main.c:899 viewport_changed    │
                      │  • hlcache retrieve,       │          │  • box_for_node re-resolve      │
                      │    cb=html_object_callback│          │  • in-viewport → html_fetch_obj │
                      └───────────┬───────────────┘          └───────────┬─────────────────────┘
                                  └──────────────┬───────────────────────┘
                                                 ▼
                      ┌──────────────────── DECODE (deferred, fixes162) ────────────────────┐
                      │ macos9_qt_image_convert  macos9_image.c:1366                         │
                      │  • zero-byte guard (:1380)  ◄── revisit squish gate                  │
                      │  • GetNaturalBounds (:1628) → c->width=bw; c->height=bh (:1665)       │
                      │  • content_set_ready/done (:1677)   ── NO PIXELS YET                  │
                      │  • if src_size>48KB: llcache_drop_source_data (:1682) ◄── revisit hole│
                      └───────────┬─────────────────────────────────────────────────────────┘
                                  ▼
                      ┌──────── COMPLETION ────────┐        ┌────── LAYOUT (per reformat) ──────┐
                      │ html_object_callback        │        │ lh__box_is_object(b) =            │
                      │  object.c:311 READY / 348 DONE       │   object || obj_w>0 ||           │
                      │  → html_object_done:                 │   (IFRAME|REPLACE_DIM)           │
                      │     box->object = object (:151)      │   ── NOTE: never tests IS_REPLACED│
                      │     ow=get_width; if>0:              │  true  → layout_get_object_dims  │
                      │       obj_w/h + imgdims_remember     │  false → INLINE TEXT branch       │
                      │       (:164) ── memo W (NEVER FIRES) │          layout.c:4067 → SQUISH   │
                      │     reflow GATES G1/G2/G3 (§3)       │                                   │
                      └────────────────────────────┘        └───────────────┬───────────────────┘
                                                                             ▼
                      ┌──────────────────────── PLOT (first paint decodes) ────────────────────┐
                      │ macos9_qt_image_redraw  macos9_image.c:1688                             │
                      │  • decode to DISPLAY size → qti->bitmap (LRU-budgeted)                  │
                      │  • qti->bitmap==NULL → return, paint nothing (:2035)                    │
                      │  • macos9_plot_bitmap  plotters.c:1925                                  │
                      │      freed/corrupt guard (:1987) ◄── disappear-via-UAF lands here       │
                      │      RGBForeColor(black)/BackColor(white) (:2306) then CopyBits/CopyMask │
                      └────────────────────────────────────────────────────────────────────────┘

  RECONVERT (JS DOM mutation, e.g. analytics beacon → 5×/page) cross-cuts ALL of the above:
     html_reconvert  html.c:2328
       H1 clear node→box backlinks (:1808)  → box_for_node returns NULL until dom_to_box re-registers
       dom_to_box   → box_image RE-RUNS (budget already 0 → defers EVERY image to g_lazyimg_head)
       relink pass  html.c:2081 → per object: CONTENT_IMAGE & !background & box_for_node!=NULL & DONE
                                   ? newbox->object = content   (re-link)
                                   : RETIRE (release handle)     ◄── eager image → box->object NULL
```

### The two size "authorities" that layout actually consults
[layout_internal.h:166-192](../../browser/netsurf/content/handlers/html/layout_internal.h#L166): the **only** two intrinsic-size sources in the engine are
```c
lh__box_intrinsic_w(b) = b->object ? content_get_width(b->object)  : b->obj_w;
lh__box_intrinsic_h(b) = b->object ? content_get_height(b->object) : b->obj_h;
```
and the replaced-vs-inline pivot is
```c
lh__box_is_object(b) = b->object || b->obj_w>0 || (b->flags & (IFRAME|REPLACE_DIM));
```

---

## 2. Why images squish, and why they disappear — the mechanisms, separated

These are **two different failures** and conflating them cost rounds. Keep them apart.

### 2A. Squish = size loss (box collapses to line-height + alt-string width)
Root cause ([layout_internal.h:177](../../browser/netsurf/content/handlers/html/layout_internal.h#L177), confirmed by agent, restated by fixes929): **`IS_REPLACED` is set on every `<img>` at [box_special.c:1756](../../browser/netsurf/content/handlers/html/box_special.c#L1756) but layout never consults it.** Layout decides "is this a replaced element?" by asking `lh__box_is_object()` = "does it currently have a size from somewhere?" The instant all size sources are empty (`object==NULL`, `obj_w==0`, no `REPLACE_DIM`/`IFRAME`), the `<img>` stops being replaced *at all* and falls to the inline-text branch at [layout.c:4067](../../browser/netsurf/content/handlers/html/layout.c#L4067) / minmax at [layout.c:805](../../browser/netsurf/content/handlers/html/layout.c#L805): `height = line_height`, `width = measured alt string`. **That is the squish, exactly.**

The three ordinary ways all size sources go empty at once:
- **First paint of a lazy image**: `box_image` never fetches at construct for image #11+; `object==NULL` until a paint drains the queue. The memo *was* meant to cover this window — but it never fills (§0.2).
- **Revisit of an attribute-less image >48KB**: `hlcache_clean` destroyed the content, `llcache_handle_drop_source_data` freed the bytes ([macos9_image.c:1682](../../browser/netsurf/frontends/macos9/macos9_image.c#L1682)), so convert rebuilds a **zero-byte** content → `c->width` stays 0 → squish. fixes931's 48KB gate covers *small* icons only; big attribute-less images still squish on revisit.
- **Reconvert retire**: the relink pass retires an image object it can't re-key ([html.c:2137](../../browser/netsurf/content/handlers/html/html.c#L2137)) and the synchronous reformat runs before anything re-fetches.

Images with CSS `width`+`height` (→ `REPLACE_DIM`) are immune to squish because `REPLACE_DIM` keeps `lh__box_is_object` true. This is why "only *this* element squishes."

### 2B. Disappear = link/paint loss (box stays correctly sized, paints empty)
fixes933 correctly separated this: the home-page images have explicit CSS w&h → `REPLACE_DIM` → **always sized right**, yet they go blank. That is `box->object` going NULL with the box still the right size — a *link* loss, not a *size* loss. Every mechanism that can blank an already-painted image (from agent ac15bf, all verified):

| # | Mechanism | Site |
|---|---|---|
| A | `html_replace_object` nulls `box->object` before re-fetch | [object.c:765](../../browser/netsurf/content/handlers/html/object.c#L765) |
| B | `CONTENT_MSG_ERROR` nulls `box->object` (late fetch error) | [object.c:381](../../browser/netsurf/content/handlers/html/object.c#L381) |
| C | `html_object_free_objects` / box-tree destroy nulls it | [object.c:938](../../browser/netsurf/content/handlers/html/object.c#L938) |
| **D** | **reconvert relink retires an eager image → new box has no object** | [html.c:2137](../../browser/netsurf/content/handlers/html/html.c#L2137) |
| E | bitmap freed mid-redraw (death-row defer exists to prevent) | [macos9_bitmap.c:382](../../browser/netsurf/frontends/macos9/macos9_bitmap.c#L382) |
| F | knockout replay of a freed content → plotters freed-guard blanks | [plotters.c:1987](../../browser/netsurf/frontends/macos9/plotters.c#L1987) |
| **G** | **`hlcache_clean` evicts an eager image at users==0** | [hlcache.c:211](../../browser/netsurf/content/hlcache.c#L211) |
| H | `c->width` reads 0 at a later paint (rebuilt/zero-byte content) | [macos9_image.c:2169](../../browser/netsurf/frontends/macos9/macos9_image.c#L2169) |
| I | LRU evicts the decoded bitmap; re-decode fails if source dropped | [macos9_image.c:1897](../../browser/netsurf/frontends/macos9/macos9_image.c#L1897) |

The **home-page symptom is D+G**, triggered by the analytics beacon: `stats.mp.ls/script.js` does a single class mutation (`LIFE mutcensus total=1 ... CLASS=1`, seen in the logs) → full reconvert ~5×/page. Before fixes932 the images were lazy and absent when it fired (nothing to lose); fixes932 eager-loaded them, so now they're present *and* get churned. "Correct / squished / none, varying across visits" = the D+G+lazy-drain race, not a degradation.

---

## 3. The reflow-gate fragility ("Half 2")

Even when an image links correctly, the reflow that resizes its box is gated four ways, and holes here have been patched repeatedly (fixes689/797/916):

| Gate | Site | Condition |
|---|---|---|
| G1 (READY reflow) | [object.c:322](../../browser/netsurf/content/handlers/html/object.c#L322) | `base.active==0` & status READY/DONE |
| G2 (guaranteed) | [object.c:634](../../browser/netsurf/content/handlers/html/object.c#L634) | `base.active==0` & status READY/DONE |
| G3 (incremental) | [object.c:653](../../browser/netsurf/content/handlers/html/object.c#L653) | `incremental_reflow` & `!REPLACE_DIM` & throttle window |
| skip | [html.c:2575](../../browser/netsurf/content/handlers/html/html.c#L2575) | `layout==NULL` during reconvert → reflow swallowed |

The recurring bug: an image completing *inside the 250ms `reformat_time` throttle window* after the page is already DONE (a cached/instant completion) matches neither the `active==0` batch nor the immediate incremental branch, and its reflow is dropped. fixes916 closed the DONE-vs-READY variant; the structure remains fragile because "did this completion get a reflow?" depends on `active`, a throttle clock, and `layout!=NULL` all lining up. **A resize "fixes" everything only because `browser_window_schedule_reformat` is unconditional** — it bypasses all four gates.

---

## 4. Structural holes, ranked

1. **Four unsynchronised size stores, no owner** (§0.3). The memo was bolted on to cover `object==NULL`, but it (a) never fills and (b) is a *third* store rather than a unification.
2. **`IS_REPLACED` is not authoritative for layout** (§2A). An `<img>` should be laid out as replaced *by virtue of being an `<img>`*, with a defined fallback size, never demoted to text. This single change kills the entire squish class.
3. **The memo never fills** (§0.2) — open root cause, needs a live value. Leading hypotheses: html_object_done reads width before the deferred convert commits it; or these images complete via a path that bypasses html_object_done; or `hlcache_handle_get_content` returns a stale content at that instant.
4. **Two fetch paths for one concept** (eager vs lazy) with independent dedupe. The lazy queue is a **process-wide global** ([box_special.c:1564](../../browser/netsurf/content/handlers/html/box_special.c#L1564)) never cleared on navigation; entries from a departed page linger until a later paint reaps them by liveness. Eager fetches are invisible to the queue's `(content,node)` dedupe, so a drain between "deferred entry queued" and "eager fetch completed" can double-fetch the same box.
5. **`hlcache_clean` is over-eager for images** ([hlcache.c:222](../../browser/netsurf/content/hlcache.c#L222)): destroys any content at `users==0`, and images aren't disk-cached ([macos9_disk_cache.c:270](../../browser/netsurf/frontends/macos9/macos9_disk_cache.c#L270)) and drop their source bytes — so eviction is unrecoverable without a network re-fetch.
6. **Reconvert churns images that never needed churning.** A geometry-neutral class flip rebuilds the whole box tree and retires image objects. The census (fixes925/926) was staged to justify a "skip reconvert for geometry-neutral mutations" path; that path is the real fix for the beacon storm.
7. **Reflow-gate fragility** (§3).
8. **Observability gated off** (§0.1) — the reason 1–7 have been hard to nail.

---

## 5. The logging overhaul (do this first — it is the maintainer's stated step 1)

**Principle:** coarse, aggregate, transition/anomaly-gated — never per-image-per-frame. fixes911's per-entry `FlushVol` was undone twice; do not repeat it. Every line must survive the release gate on its own, which means **tag it `LIFE ` (always kept) — not `WORK ` (gated off).**

### 5.0 Unblock the channel (one-line prerequisite)
Either define `MACSURF_WORK_LOG` in `macsurf_prefix.h`, **or** (recommended) don't — instead give the image trace its own survivable tag so we never depend on a compile flag again. Recommendation: a dedicated **`LIFE img …`** family. `LIFE ` is unconditionally kept ([macsurf_debug_log.c:651](../../browser/netsurf/frontends/macos9/macsurf_debug_log.c#L651)), coarse by convention, and already how imgdims/relink/mutcensus report.

### 5.1 The trace — one coherent per-navigation image ledger
Emit aggregates once per load (in `html_reformat`, beside the existing `LIFE imgdims`), plus **anomaly lines** only when something is wrong:

- **Construct census** — per reformat: `LIFE img construct total=N eager=E lazy=L replacedim=R memoseed=M`. Tells us the eager/lazy split and how many boxes had a size before fetch. (box_image, aggregate counters.)
- **Completion ledger** — the memo mystery killer. At `html_object_done` entry, bump aggregates and emit **one** anomaly line when `ow<=0`: `LIFE img objdone url=… ow=%d oh=%d rd=%d cw=%d` where `cw` = `content__get_width` read a second way. Retag the existing fixes933 `WORK objdone ZERO-DIM` → `LIFE`. This alone resolves hole #3.
- **Link/unlink transitions** — the disappear killer. Aggregate, per reformat: `LIFE img link set=%d nulled=%d` counting the three `box->object=NULL` sites (A/B/C) and the one `=object` site. When `nulled>0` on a page that had painted images, that is the disappear, timestamped. (object.c:151/381/765/938.)
- **Reconvert relink** — already `LIFE objects relinked/inflight/retired why:…`. Keep. Add `evicted=` from a `hlcache_clean` image-destroy counter (hole G).
- **Lazy queue** — retag `RECON LAZY viewport …` → `LIFE img lazy fetched=%d kept=%d dropped-has-obj=%d` (the `box->object!=NULL` drop at [box_special.c:1668](../../browser/netsurf/content/handlers/html/box_special.c#L1668) is the highest-signal dedupe-vs-reconvert probe).
- **Decode/paint anomalies** — retag fixes933's `getint ZERO-NAT` → `LIFE`; keep `QT-REDECODE-FAIL` and the plotters `FAIL plot_bitmap freed/corrupt` (already survive). Add one line at [macos9_image.c:2035](../../browser/netsurf/frontends/macos9/macos9_image.c#L2035) (`qti->bitmap==NULL → paints nothing`) so "decoded nothing this frame" is distinguishable from "content freed".

### 5.2 What each reading will tell us immediately
- `objdone ow<=0` fires ⇒ the memo mystery is "width 0 at DONE" (deferred-decode timing) → fix is to store on the convert side, or read width later.
- `objdone` **never** fires but `stored` still 0 ⇒ html_object_done is not the sink → follow the object's real completion path.
- `img link nulled>0` correlated with a `mutcensus` line ⇒ confirms the beacon-reconvert disappear (D) → fix #6 (skip geometry-neutral reconverts) or make relink never retire a DONE image.
- `hlcache evicted>0` on the vanished image ⇒ hole G → pin image contents referenced by the live tree across `hlcache_clean`.

This is ~6 aggregate lines + ~4 anomaly lines total per load. No firehose.

---

## 6. The system overhaul (after the logging round confirms the readings)

Staged so each step is independently shippable and revertable, in dependency order:

- **Step 1 — Make `IS_REPLACED` authoritative (kills the squish class outright).** In `lh__box_is_object`/`lh__box_is_replace`, treat an `IS_REPLACED` box as replaced *unconditionally*; when no size source is available, lay it out at a defined fallback (its `obj_w/obj_h`, else 0×0 collapsed-but-replaced, else a placeholder box) — never the inline-text branch. An image may render empty for a frame, but it can never *become text*. Low risk, high payoff; harness-testable without hardware.
- **Step 2 — One image record, one owner.** Fold the three size stores into a single per-node record keyed on the DOM node (the lazy queue already proves node-keying survives reconvert). It carries URL + intrinsic size + fetch state; `box->object` and `box->obj_w/h` are *derived* from it on (re)link, never independent truth. The memo stops being a third store and becomes this record's size field — and gets written from the **convert** side (where the size is known) rather than `html_object_done` (where §0.2 shows it isn't reliably known).
- **Step 3 — One fetch path.** Route eager and lazy through the same enqueue+drain, with the eager budget just meaning "drain these immediately." Kills the eager/lazy dedupe blind spot (hole #4) and makes the queue per-content, cleared on navigation.
- **Step 4 — Pin live-tree image contents across `hlcache_clean`** (hole #5/G), or make images disk-cacheable again with a tight budget, so a revisit/eviction is recoverable without the zero-byte rebuild.
- **Step 5 — Skip reconvert for geometry-neutral mutations** (hole #6). The census is already built; the beacon storm is the motivating case. This removes the *trigger* for most disappears rather than making the reconvert survivable.
- **Step 6 — Collapse the reflow gates** (§3) once size/link are owned in one place: a single "an image resolved → schedule one coalesced unconditional reflow" path, not four conditional ones.

### Verification
- **Harness first, under ASan.** The harness already links the real object/hlcache/decode machinery (Tests 31/32 touch imgdims). Add: a lazy image sized before completion; an eager image surviving a reconvert with `box->object` non-NULL on the *new* box; the canary — an image whose node JS removed is retired without UAF; the intersection — reconvert while a fetch is genuinely in flight *and* its node is removed. Negative-control each (revert the fix → test must fail).
- **Hardware** (the harness can't settle these): home.macsurf.org cold→away→back with the beacon firing (D+G); a page reconverted mid-fetch; an attribute-less image >48KB on revisit (§2A big-image hole).

---

## 7. Appendix — verified reference points

- Memo store (never fires): [object.c:164](../../browser/netsurf/content/handlers/html/object.c#L164) · lookup: [box_special.c:1538](../../browser/netsurf/content/handlers/html/box_special.c#L1538) · table: [box_special.c:1482](../../browser/netsurf/content/handlers/html/box_special.c#L1482)
- Squish pivot: [layout_internal.h:177](../../browser/netsurf/content/handlers/html/layout_internal.h#L177) · inline fallback: [layout.c:4067](../../browser/netsurf/content/handlers/html/layout.c#L4067), [layout.c:805](../../browser/netsurf/content/handlers/html/layout.c#L805)
- Eager/lazy split: [box_special.c:1779](../../browser/netsurf/content/handlers/html/box_special.c#L1779) · budget init: [html.c:831](../../browser/netsurf/content/handlers/html/html.c#L831)
- Lazy queue global + drain: [box_special.c:1564](../../browser/netsurf/content/handlers/html/box_special.c#L1564), [box_special.c:1625](../../browser/netsurf/content/handlers/html/box_special.c#L1625) · paint trigger: [main.c:899](../../browser/netsurf/frontends/macos9/main.c#L899)
- Reconvert + relink: [html.c:2328](../../browser/netsurf/content/handlers/html/html.c#L2328), [html.c:2081](../../browser/netsurf/content/handlers/html/html.c#L2081)
- Deferred decode + source drop: [macos9_image.c:1665](../../browser/netsurf/frontends/macos9/macos9_image.c#L1665), [macos9_image.c:1682](../../browser/netsurf/frontends/macos9/macos9_image.c#L1682)
- Plot freed-guard + fg/bg reset: [plotters.c:1987](../../browser/netsurf/frontends/macos9/plotters.c#L1987), [plotters.c:2306](../../browser/netsurf/frontends/macos9/plotters.c#L2306)
- `hlcache_clean` eviction: [hlcache.c:211](../../browser/netsurf/content/hlcache.c#L211) · images not disk-cached: [macos9_disk_cache.c:270](../../browser/netsurf/frontends/macos9/macos9_disk_cache.c#L270)
- Log gate + WORK never defined: [macsurf_debug_log.c:668](../../browser/netsurf/frontends/macos9/macsurf_debug_log.c#L668), [macsurf_debug_log.c:651](../../browser/netsurf/frontends/macos9/macsurf_debug_log.c#L651)
- Hardware evidence: `builds/logs/2026/07/2[01]/*` — every `LIFE imgdims` reads `stored=0 hit=0`.
