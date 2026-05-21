# fixes310, Empty box tree after fetch / convert / READY

Diagnosis-only round. No code changes ship in fixes310. Instrumentation
to disambiguate the remaining hypotheses ships as fixes310a.

## Symptom (from the log evidence cited in the round-307 brief)

Two redraws were captured after the fixes306 fetcher fix:

```
update: bw_redraw done visits=0 block=0 inlinec=0 inline=0 text=0 other=0 skips=0 plot_text=0 plot_rect=1
...
[after a second http_setup]
update: bw_redraw done visits=3 block=3 inlinec=0 inline=0 text=0 other=0 skips=0 plot_text=0 plot_rect=1
```

`macos9_hrb_visits` is incremented at the top of [redraw.c html_redraw_box (line 1327)](../../browser/netsurf/content/handlers/html/redraw.c#L1327) before any return, so `visits=N` means exactly N entries into `html_redraw_box`.

## Map of the relevant call paths

### 1. `update: bw_redraw done visits=0 plot_rect=1` is the no-content path, not a bug.

[browser_window.c:2606-2611](../../browser/netsurf/desktop/browser_window.c#L2606):

```c
if ((bw->current_content == NULL) && (bw->children == NULL)) {
    /* Browser window has no content, render blank fill */
    ctx->plot->clip(ctx, clip);
    return (ctx->plot->rectangle(ctx, plot_style_fill_white, clip) == NSERROR_OK);
}
```

That is exactly one `plot->rectangle` call (→ `plot_rect=1`) and zero entries into `html_redraw_box` (→ `visits=0`). Fits the first log line exactly.

The first redraw is dispatched from the `InvalWindowRect` at the bottom of [window.c macos9_window_create](../../browser/netsurf/frontends/macos9/window.c#L161), which fires before fetch / parse / convert have finished. Expected.

There is also a second silent path that produces this same signature: `content_redraw` at [content.c:573](../../browser/netsurf/content/content.c#L573) returns `true` without touching the plotter when `c->locked == true`. `c->locked` is set by [content_convert at content.c:85](../../browser/netsurf/content/content.c#L85) and cleared in `content_set_ready` ([content.c:294](../../browser/netsurf/content/content.c#L294)). So between the first byte of the body arriving and the READY broadcast firing, any redraw that finds `current_content` non-NULL still no-ops at the `c->locked` gate, plotting nothing extra and leaving the early `plot->rectangle` from [browser_window.c:2630-2632](../../browser/netsurf/desktop/browser_window.c#L2630) (the children-frames blank-fill), but only when `bw->children` is non-NULL, which the home URL navigation does not hit.

So in this build the first `visits=0 plot_rect=1` line is the pre-content blank-fill, not a stuck-locked redraw.

### 2. `update: bw_redraw done visits=3 block=3 plot_rect=1` is the actual bug.

`visits=3, block=3, all other counters 0` means `html_redraw_box` was entered exactly three times and every visited box was `BOX_BLOCK`. The walker recurses unconditionally over `box->children` ([redraw.c:1265-1276](../../browser/netsurf/content/handlers/html/redraw.c#L1265)), the only filter is float type, and the clip-skip path explicitly does NOT return early ([redraw.c:1512-1514](../../browser/netsurf/content/handlers/html/redraw.c#L1512), the `DIAG: do NOT return` patch). So three visits really is three boxes total in the walked subtree, not "we visited a thousand boxes and clipped most of them out."

`plot_rect=1` is the background fill at [redraw.c html_redraw line 2142](../../browser/netsurf/content/handlers/html/redraw.c#L2142), `ctx->plot->rectangle(ctx, &pstyle_fill_bg, clip)`, fired before the box walk. The walker itself doesn't plot any further rectangles because none of the three blocks have backgrounds, borders, or any of the other branches that hit `plot->rectangle`, and none of the children traversal reaches a `BOX_TEXT` (which would have hit `plot->text` and bumped `plot_text`).

A three-block tree with every block sterile shaped like:

```
BOX_BLOCK (root layout)
  └── BOX_BLOCK
        └── BOX_BLOCK
```

In NetSurf's normal flow this corresponds to a successfully-converted-but-empty page: `html` → `body` → one structural child (or `root.children → html → body` depending on whether the synthetic root in [box_construct.c convert_xml_to_box conversion-complete branch (line 1293-1310)](../../browser/netsurf/content/handlers/html/box_construct.c#L1293) is included). For any non-trivial page (e.g. `<html><body>Hello</body></html>`), we would expect at minimum `BOX_BLOCK ×2 + BOX_INLINE_CONTAINER + BOX_INLINE + BOX_TEXT = visits=5, block=2, inlinec=1, inline=1, text=1`.

So: **layout completed, `htmlc->layout` is non-NULL and walked; the box tree has zero text and zero inline content**.

## Vtable / registration audit (the parts that COULD have explained this and don't)

All three of these were the leading candidates from the brief; all three are wired correctly.

### html_content_handler vtable: fully populated

[html.c html_init line 2375-2411](../../browser/netsurf/content/handlers/html/html.c#L2375):

| Slot | Wiring |
|---|---|
| `create` | `html_create` |
| `process_data` | `html_process_data` |
| `data_complete` | `html_convert` |
| `reformat` | `html_reformat` |
| `redraw` | `html_redraw` |
| `destroy` | `html_destroy` |
| `stop` | `html_stop` |
| `mouse_track` / `mouse_action` / `keypress` / `open` / `close` | wired |
| `get_selection` / `clear_selection` / `get_contextual_content` | wired |
| `scroll_at_point` / `drop_file_at_point` | wired |
| `debug_dump` / `debug` | wired |
| `clone` / `get_encoding` / `type` / `exec` | wired |
| `saw_insecure_objects` | wired |
| `textsearch_*` / `textselection_*` | wired |
| `no_share` | `true` |

`html_content_handler` is `memset` to zero at [html.c:2380](../../browser/netsurf/content/handlers/html/html.c#L2380) before the slot-by-slot population, so there are no leftover NULL slots.

### content_factory registration

[html.c html_init line 2417-2422](../../browser/netsurf/content/handlers/html/html.c#L2417) iterates `html_types[]` and calls `content_factory_register_handler(html_types[i], &html_content_handler)` with success-checking. `html_init` is invoked from [desktop/netsurf.c netsurf_init line 200](../../browser/netsurf/desktop/netsurf.c#L200). `netsurf_init` runs from [main.c line 352](../../browser/netsurf/frontends/macos9/main.c#L352) before the home-URL `browser_window_create`.

### content_redraw dispatch

[content.c:564-584](../../browser/netsurf/content/content.c#L564), guards on `c->locked` and `c->handler->redraw == NULL`, otherwise dispatches via `c->handler->redraw(c, data, clip, ctx)`. The redraw slot is wired (above), so for any HTML content, `html_redraw` is what runs.

The fact that the second log line shows `visits=3` (not `visits=0`) confirms: by the time of the second redraw, content is NOT locked, the handler vtable IS reached, and `html_redraw_box` IS being entered three times.

## What the visits=3 tree actually means about box construction

The async box construction loop in [box_construct.c convert_xml_to_box (line 1240-1321)](../../browser/netsurf/content/handlers/html/box_construct.c#L1240) only assigns `ctx->content->layout` at the **conversion-complete branch** ([line 1306](../../browser/netsurf/content/handlers/html/box_construct.c#L1306)). On any earlier failure path the callback fires with `success=false` and `html_box_convert_done` ([html.c:222-244](../../browser/netsurf/content/handlers/html/html.c#L222)) calls `content_broadcast_error` + `content_set_error`, no READY, no current_content swap.

So if `visits=3` is happening at all, then:

1. `html_finish_conversion` → `dom_to_box` → `convert_xml_to_box` ran the loop to its conversion-complete branch.
2. `box_normalise_block(&root, ctx->root_box, ctx->content)` returned `true` ([box_construct.c:1302](../../browser/netsurf/content/handlers/html/box_construct.c#L1302)).
3. `ctx->content->layout = root.children` was assigned ([line 1306](../../browser/netsurf/content/handlers/html/box_construct.c#L1306)).
4. `html_box_convert_done(c, true)` ran ([line 1309](../../browser/netsurf/content/handlers/html/box_construct.c#L1309)), which then broadcast READY ([html.c:282](../../browser/netsurf/content/handlers/html/html.c#L282)).

But the resulting tree has only three `BOX_BLOCK` nodes and zero descendants of any other type.

## Candidate root causes (in order of probability for fixes310a probes)

The root cause cannot be pinned down from Linux source alone; this needs runtime evidence. The candidates are, ranked:

### A. The fetched body is truncated or empty.

If the HTTP fetcher delivers, say, a `200 OK` plus `<html><body></body></html>` and nothing else (e.g. the proxy returns an empty response, or the connection drops between headers and the real body), Hubbub parses it into a DOM with only `html` and `body`, `dom_to_box` runs to completion with no text/inline boxes, normalise yields a 3-block tree, READY fires. **This matches the observed shape exactly.**

The HTTP fetcher in [macos9_http_fetcher.c mfs_poll_one (line 100-124)](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L100) currently has **no `MS_LOG`** for body byte count, only for `mfs_open` and `http_setup`. There is no instrumentation that would distinguish "delivered 0 body bytes" from "delivered 50KB."

The proxy at `116.202.231.103:8765` is not on this machine and is unreachable from CI, so we cannot fetch `http://mac.mp.ls/simple.html` and inspect the response from Linux. Field evidence is the only path here.

### B. Hubbub parsed the body, but the resulting DOM has html/body and nothing else.

Possible if the body bytes deliver but a charset / encoding / preamble issue causes Hubbub to discard everything after the body tag. Less likely, Hubbub is the same codebase that's working in prior MacSurf builds, but cannot be ruled out without DOM-level instrumentation.

### C. dom_to_box descended into the DOM, but `box_normalise_block` stripped the inline content.

Possible if `box_normalise_block` is buggy under CW8 (e.g. designated-init conversion or for-scope fallout from the fixes260-304 sprint dropped a field initialiser). This is a real category, the sprint hit 38 fixes, but no specific symptom currently points here.

### D. Layout produced the boxes correctly, but something between layout and redraw is corrupting `box->children` / `box->next`.

The defensive sanity clamp at [redraw.c:1351-1395](../../browser/netsurf/content/handlers/html/redraw.c#L1351) already documents that "Upstream layout / CSS engine has been observed to leave box->x, box->y, box->margin/padding/border with huge garbage values when the computed style or unit_len_ctx is incompletely initialised." If the same class of corruption hits `box->children`, the walker would see an empty children list. No current sanity clamp covers `children` / `next` / `parent`.

### E. The CONTENT_HTML status path bypassed reformat, layout never ran.

Disqualified: a non-reformatted box tree from `convert_xml_to_box` would still have html/body/etc. with the children populated, the "3 visits all BOX_BLOCK" shape is consistent with normalise-success but minimal-DOM, not with skipped layout. Ruling this out is partial without confirming `c->width` / `c->height` are reasonable.

## Recommended fixes310a, narrowly-scoped instrumentation

Ship a fixes310a hotfix that adds the following `MS_LOG` calls only. No behavioural changes:

1. **HTTP fetcher body byte count**, at [macos9_http_fetcher.c mfs_poll_one body branch (line 119-122)](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L119), accumulate per-context body bytes into `c->body_bytes` and emit one summary `MS_LOG` at the FETCH_FINISHED transition in `macos9_http_poll` ([line 132](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L132)) of the form `http: done, body_bytes=%ld status=%d`.

2. **html_box_convert_done success log**, at [html.c html_box_convert_done line 244](../../browser/netsurf/content/handlers/html/html.c#L244) (just after the success/abort gate), walk `c->layout` once with a small inline counter (count nodes, count by type) and emit one `MS_LOG` of the form `box convert: layout=%p total=%d block=%d inlinec=%d inline=%d text=%d other=%d`.

3. **html_reformat post-layout dimensions**, at [html.c html_reformat line 1109](../../browser/netsurf/content/handlers/html/html.c#L1109), after the descendant-overflow expansion, emit `MS_LOG` of the form `reformat: w=%d h=%d cw=%d ch=%d desc_x1=%d desc_y1=%d` (where `cw/ch` are the input width/height, `desc_x1/y1` come off `layout`).

4. **html_process_data byte-in count**, at [html.c html_process_data line 742-770](../../browser/netsurf/content/handlers/html/html.c#L742), accumulate total bytes into the existing htmlc and emit one summary `MS_LOG` at end of the call. Lets us cross-check (1), if the fetcher delivers 50KB but the parser only sees 1KB, that's a different bug.

These four probes disambiguate (A) vs (B) vs (C) vs (D):

| body_bytes | parser_bytes | layout total | desc_y1 | Indicates |
|---|---|---|---|---|
| 0 | 0 | 3 | 0 | (A) Empty body delivered |
| > 0, small | matches | 3 | small | (A) Truncated body |
| > 0, normal | matches | 3 | small | (B) Hubbub discarded content OR (C) box_normalise stripped |
| > 0, normal | matches | many | normal | (D) Layout fine, walker sees empty children → corruption |
| > 0, normal | mismatch | varies | varies | Data delivery defect inside content.c |

That's enough to scope fixes311 specifically.

## Out of scope for fixes310a (do not add)

- Box tree dump (`box_dump` writes to FILE*, which under MacSurf MSL is not suitable for the file-backed log channel, would need a wrapper and that's fixes311+ scope if the four probes don't conclude).
- Any changes to redraw.c, layout.c, or the box walker.
- Any defensive fix to `box->children` / `box->next` until D is confirmed.

## Reconciliation: CLAUDE.md "Build State" claims are stale

`CLAUDE.md` Build State currently reads "MacSurf v0.3 renders real live web pages on G3 hardware with native CSS custom property support" and "First confirmed page: MacTrove (`http://mac.mp.ls/`), 2026-04-19, via the full NetSurf pipeline." This snapshot is from before the fixes260-304 build-environment recovery sprint. **After that sprint, the pipeline links and runs but produces an empty box tree and a grey window for any page; render is not currently working.**

The fixes306 page-load fix re-enabled the URL submit / navigate path (`g->bw == NULL` was killing every navigate). With that fix in place, fetch + parse + convert + READY all flow, but the rendered output is empty. fixes310 (this round) is the diagnosis pass; fixes311 is the actual render fix scoped from fixes310a evidence.

CLAUDE.md is updated in this same fix round to reflect "v0.3 render path under reconstruction post-fixes260-304 sprint; render currently produces empty box tree (fixes310 diagnosis)."

## Summary table

| Question from the round-307 brief | Answer |
|---|---|
| Is `content/handlers/html/box_construct.c` in the build? | Yes (confirmed in `MacSurf.mcp`). |
| Is `content/handlers/html/layout.c` in the build? | Yes (confirmed in `MacSurf.mcp`). |
| Is `content/handlers/html/redraw.c` in the build? | Yes, `macos9_hrb_visits` etc. live in this file and are being incremented (visits=3). |
| Is `html_content_handler` correctly initialized? | Yes, all 30+ slots wired in `html_init`, `memset` to zero before population. |
| Is `html_redraw` wired? | Yes (`html_content_handler.redraw = html_redraw` at html.c:2391). Confirmed reaching the walker (visits=3, not 0). |
| Is layout triggered? | Yes, `browser_window_content_ready` runs `content_reformat` after READY, dispatching to `html_reformat` → `layout_document`. |
| Does the handler get registered for text/html via `content_factory_register_handler`? | Yes, `html_init` iterates `html_types[]` calling `content_factory_register_handler` with error-checking. |
| Is `htmlc->layout` NULL post-conversion? | No, visits=3 proves it's non-NULL and walked. The bug is that the tree is degenerate, not absent. |
| Does the box tree have zero children, or are descendants invisible? | Zero children at the tree level, visits=3 with strict types means the walker reached three blocks and nothing else exists, NOT that descendants were clipped (`DIAG: do NOT return` keeps the walker descending past clip-skip). |
| Reconciliation of CLAUDE.md "v0.3 renders MacTrove" claim? | Stale; reconciled in this round. Render currently broken post fixes260-304 recovery. |
