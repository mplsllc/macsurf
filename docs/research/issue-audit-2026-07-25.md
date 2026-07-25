# Open-issue audit — 2026-07-25 (branch `3.0`, ~fixes1039)

**92 open issues, every one re-verified against the tree.** Issue bodies were treated as
untrusted: 43 of the 92 were filed in 2026-05, when the tree was at ~fixes400. It is now at
fixes1039. Every `ALREADY-DONE` verdict below cites the code that closes it.

Baseline at time of audit: harness **46/46 PASS**, `make -C harness check-macdefault` **OK**,
`git status` clean, origin + github both 0-ahead/0-behind.

---

## 1. Headline

| Bucket | Count |
|---|---:|
| **ALREADY-DONE** — fixed in code, never closed | **15** |
| **MISFRAMED** — title/body no longer describes reality | **6** |
| **PARTIAL** — half shipped, remainder is real | 12 |
| **OPEN** — genuinely outstanding | 54 |
| **WONTFIX** — 3 upheld, 2 split | 5 |

**~21 issues can leave the board with zero code**, on the maintainer's confirmation.
Three of the six `priority: high` issues (#222, #262, #264) are in that set.

---

## 2. ALREADY-DONE — propose close, evidence attached

> Per DIRECTIVE, none of these are closed by the agent. This is a confirm-list.

| # | Claim in issue | Closing evidence |
|---|---|---|
| 29 | querySelector/All missing | `macsurf_qjs.c:6567-6570`, `:5249-5253`; compound matcher `:2864`/`:2930` (fixes864/871/880). Harness Tests 10, 15, 23 |
| 104 | fetch() API absent | Real Promise `fetch` `:7819` over `macos9_js_fetch.c` (fixes846/865/961). Test 3 |
| 126 | ARIA attribute selectors | Full CSS3 attr matcher set: `css/select.c:857/909/974/1045/1109/1177`, vtable `:119-124` |
| 133 | Hacker News compat | CLAUDE.md:131 "works: login + near-Chrome render, verified"; README 2.0.5 HW screenshot |
| 182 | hiddenscroll removeChild of null | `qjs_get_body` + parentNode-tracking `mkfb` fallback `:6651-6700` |
| 222 | XMLHttpRequest undefined | Native XHR `:7732-7860` over the slot arena; async delivery `macos9_js_fetch.c:300/380-404`. Test 3 |
| 261 | getElementsBy* are `[]` stubs | doc `:7315-7327` (fixes873), element-scoped `:3298-3304` (fixes1009). Test 41 |
| 263 | node traversal missing | `qjs_wrap_text_node:5415-5520`, installers `:3440-3466` (fixes878). Tests 21, 42, 46 |
| 264 | flat event model, no UI bridge | **Both halves.** Bridge `:3481-3520` (fixes989); `addEventListener`/`dispatchEvent` `:5196-5210`; `macsurf_qjs_dispatch_dom_click` **deleted** — survives only in a comment at `:3487`. Tests 38, 40 |
| 280 | external SVG never paints | `macos9_image.c:2426-2446` `macos9_svg_src_redraw` → `macos9_svg_paint_standalone` (fixes823); handler `:2471-2473` |
| 303 | reconvert fenced to facebook.com | fixes874 `de52fd60`; generation-token pending set `macos9_reconvert.c:72-110` |
| 307 | fetchers never set tainted_tls | fixes958 `e0fa8b9d` + fixes962; caller `macos9_tls_fetcher.c:1395`, read `llcache.c:2702-2706` |
| 108 | Mac-side duplicate source trees | Obsoleted by `drop-to-imac.sh` → one canonical `/Projects/MacSurfSource/` |
| 137 | Remove Object Code automation | `forclaude/drop-to-imac.sh:11-13` stamps strictly-increasing future mtimes — this *is* the automation |
| 220 | Access Paths absolute | `forclaude/Access Paths.xml` (the delivered copy) is already 55 `PathRoot=Project` / 1 Absolute. Only the stale repo-root copy disagrees — one file sync |
| 176 | flex `width:auto` items take full container width → wrong wrapping | **Fixed by `fixes176` `66f7b994`** ("flex intrinsic main-size resolution"). Re-tested this audit against the issue's own XenForo `.p-title` repro, both cases, MacSurf vs Chrome — see §6.1. `engine-bug` label can come off |

## 3. MISFRAMED — retitle or close as duplicate

| # | Reality |
|---|---|
| **50** | Title says "re-enable tabs (currently disabled)". **There is no tab code at all** — zero hits for `BW_CREATE_TAB\|tab_bar\|new_tab` in `frontends/macos9/`; `window.c:1307` ignores its `ex`/`f` args. Retitle *"Implement tabs (none exist; Cmd-N multi-window is the model)"*. XL, not a toggle |
| **100** | "Iframe support (real, not just stub)" — it is **~85% real**: `box_special.c:1090`, `frames.c:201`, `browser_window.c:1009/1696`, paint `redraw.c:4712-4714`, per-iframe JSRuntime. Two hardware iframe-lifetime crashes were fixed (fixes905/915) — which only happens to code that runs. Reframe to the true residue: depth clamp, youtube/vimeo skip at `box_special.c:1131-1151`, no iframe scroll/focus routing |
| **135** | GitHub — not a site issue. Symptom of #264 + #265. Close as duplicate (parent #196 is already closed) |
| **136** | MDN — same as #135 |
| **90** | Duplicate scope of #80. Merge |
| **267** | META tracker numbers are stale in both directions. Stated 155/115/40; **measured today 161 parsed / 136 consumed / ~25 dropped**, 62 rewrite sites. It lists twelve now-closed issues as open and omits everything from fixes846→1039. This is the right home for this audit |

## 4. WON'T-FIX reassessment

Re-examined on today's capabilities rather than assumed. **Two of five change.**

| # | Verdict | Reason |
|---|---|---|
| **#127 EventSource** | **RECONSIDER — M** | The stated blocker is obsolete. Both fetchers already de-chunk *progressively* (`macos9_http_fetcher.c:853`, `macos9_tls_fetcher.c:2229`) and the JS slot arena already takes per-`FETCH_DATA` callbacks (`macos9_js_fetch.c:380-386`), deferred via `macos9_schedule`. It merely accumulates and fires at FINISHED. Work = stream flag + `\n\n` block parser + dispatch-per-block + `Last-Event-ID` reconnect. **One real blocker: the 4 s no-progress timeout (`:83`) would kill an idle SSE stream** — needs a long-lived per-fetch flag |
| **#127 WebSocket** | **RELABEL** "feasible, not scheduled" | Issue's reason ("sync fetcher can't sustain it without a rewrite to OT async notifiers") is **factually false today**: macTLS already exposes `OSTLS_Pump`/`Read`/`Write`/`WantWrite` (`ostls_async.h:231-274`), SHA-1 and base64 are in-tree. RFC6455 framing ≈400 lines. Real cost is structural (doesn't fit `struct fetch`/llcache). Payoff thin — WS sites are SPAs we can't render anyway |
| **#128 Workers** | **KEEP** (better reason) | Issue's stated reason is stale twice: cites *Duktape*, and "contexts aren't lightweight" is wrong — we already run multiple isolated JSRuntimes concurrently (Tests 8, 30). The real blocker is **semantic**: cooperative scheduling means `postMessage` + a main-thread compute loop **deadlocks**, which is the exact workload workers exist for. **Add to the issue: do not ship a `Worker` stub** — a `Worker` whose `onmessage` never fires is the same trap as the `MutationObserver` no-op |
| **#129 HTTP/2+3** | **KEEP** | ALPN already advertises `http/1.1` only (`ostls_async.c:750-753`) — so "is HTTP/1.1 still universally served?" is answered empirically every day on hardware: yes. HPACK + framing + multiplexing ≈3-5k lines under a one-request-per-endpoint fetcher. **The claimed win is mostly obtainable by fixing #183's pool behaviour: M vs XL.** HTTP/3 additionally needs QUIC over UDP with no UDP fetcher |
| **#130 getRandomValues** | **RECONSIDER — S, do it now** | Ships today backed by **clock + stack-address xorshift** (`macsurf_qjs.c:6760-6790`) — values predictable from a timestamp, and pages use them for CSRF nonces. macEntropy's never-reset SHA-256 pool is linked and hardware-verified (`ostls_entropy.c:103/140`), reachable only via `OSTLS_InjectEntropy:414`. Work = add `OSTLS_RandomBytes()`, call it from `:6791`. ~40 lines; fixes `randomUUID` free. **This is a security fix, not a feature** |
| **#130 crypto.subtle** | **RECONSIDER narrowly** | Absent today (correct for feature detection). BearSSL supplies every primitive; the gap is API, not math. Only OAuth PKCE (`subtle.digest('SHA-256')`) is plausibly on our path. File a narrow *digest + HMAC subset* issue; rest of #130 stays wontfix |
| **#130 WebGL/WebRTC/Geo/Camera** | **KEEP** | No GPU, no UDP fetcher, no sensors. Confirm we expose *absence* or an immediate error — never a permanently-pending stub |
| **#260 aural/speech** | **KEEP (strengthened)** | Parsers exist but have **zero footprint in `css_computed_style_i`** (verified: no azimuth/elevation/voice_family/pitch/richness/volume in propset or computed). So there is no reclaim upside — the only argument for reopening. Implementing = an OS 9 Speech Manager subsystem, XL, zero demand |

---

## 5. The `bits[16]` scare — corrected

CLAUDE.md treats the full bit array as a structural blocker on new CSS properties. **It is full, but it is not a blocker.**

**Verified full:** OR-ing every `*_MASK` per word across `autogenerated_propset.h` gives `0xffffffff`
for all of `bits[0]`…`bits[15]` — **total free bits: 0**. Last slot is `JUSTIFY_ITEMS` at
`bits[15]` shift 30.

**Three escape routes, one already shipping:**

1. **The scalar-tail hatch is in production for 17 properties.** `css_computed_style_i` already
   carries plain `int32_t` keyword fields appended after `z_index` — `box_decoration_break:404`,
   `tab_size:409`, `image_rendering:415`, plus background_size, tab/text-decoration longhands,
   the `macsurf_*` family. All keyword-valued ⇒ byte-deterministic ⇒ satisfy the `arena.c`
   memcmp condition exactly. **End-append shifts no existing offsets, so it is strictly safer
   for CW8 than extending `bits[]`.** CLAUDE.md's "never scalar `_i` fields" over-generalises the
   real rule, which forbids only *non-byte-deterministic* scalars (pointers, arrays, per-cascade-divergent data).

2. **`bits[17]` is a 1-line change.** Nothing hardcodes 16 (`autogenerated_computed_v2.h` is a
   dead file with zero includers). Arena needs zero changes — both paths are `sizeof`-driven
   (`arena.c:23`, `:141`). `bits[]` is the **first** member, so +4 bytes keeps 4-alignment with
   no new padding holes. The real risk is CW8 stale `.o` (every subsequent field shifts) —
   build hygiene, mitigated by Remove Object Code.

3. **Reclaimable slots.** `CLIP` is the fattest slot in the array — `bits[2]` shift 6, **width 26**,
   for a deprecated CSS2 property; demoting the rect reclaims 24 bits = 12 two-bit properties.

**Verdict: zero issues in the CSS cluster are truly blocked on the bit array.** Use the
scalar-tail pattern for the next 5-10 keyword properties.

---

## 6. The real keystone: intrinsic sizing

`min-content` / `max-content` / `fit-content` is the structural prerequisite CLAUDE.md already
flags — and it turns out to need **zero new bits**.

`enum css_width_e` (`include/libcss/properties.h:1139-1143`) defines only `INHERIT=0`, `SET=1`,
`AUTO=2`. The type field is 2 bits (`set_width` masks `type & 0x3`, `autogenerated_propset.h:2671-2686`),
so **code point 3 is free**, and when type==3 the 5 unit bits (32 values) are semantically dead
and available to carry the keyword. Same shape on height, min-*, max-*, flex-basis, top/right/bottom/left, margin-*.

**The solver already exists — this is plumbing, not a from-scratch build.** `box->min_width` is
*by definition* min-content ("width of box taking all line breaks", `box.h:360-371`) and
`box->max_width` is max-content ("width that would be taken with no line breaks"). They are
populated by `layout_minmax_block` / `_table` / `_inline_container` / `_line` (`layout.c:317/463/1131/768`).
Exposing the CSS keywords is therefore: parse into code point 3, then resolve to the field that
already holds the answer. **This drops Batch A from XL to M/L.**

### 6.1 #176 re-tested — does not reproduce

The issue's own repro (XenForo `.p-title{display:flex;flex-wrap:wrap}` + `h1{margin-right:auto}`
+ sibling) was run through `reconvert_harness --layout` at width 993 and compared against
headless Chrome at the same width:

| Case | MacSurf | Chrome | Match |
|---|---|---|---|
| Short title | H1 w=204 y=28, DIV y=8 — **one line** (`lines=1`) | H1 w=172 y=29, DIV w=41 y=8 — **one line** | ✅ structure |
| Long title | H1 w=977 y=28, DIV wraps (`lines=2`) | H1 w=977 y=29, DIV w=109 y=125 — **wraps** | ✅ |

Wrapping in the long case is *correct* per Flexbox §9.3 — the h1's hypothetical main size
(max-content) exceeds the container, so it takes its own line. Chrome does the same. Residual
width deltas (204 vs 172, 40 vs 41) are font-metric differences, not layout-model differences.
`fixes176` at `layout_flex.c:559-587` (intrinsic main-size fallback + the conditional
`available_width` clamp) is what closed it.

### 6.2 …and the dependency chain is weaker than it looks

Checked rather than assumed. **Grid already reaches the solver**: `layout_grid.c:662-663` reads
`cc->min_width` / `cc->max_width` for column sizing (the fixes817-820 auto-track work), and
`:281-283` clamps items to `max_width`. So:

- **#246's remainder is _row-height_ distribution** (`layout_grid.c:884-891`, the
  `row_track_h[i] = 0` FR/PERCENT placeholder) — a height problem. **Not gated on intrinsic width at all.**
- **#279** needs placement-aware track derivation, which consumes the solver that is *already
  reachable*. Not gated either.
- **#226** needs diagnosis before design — a `MACSURF_PAGEMAP` + `--layout` probe, not a solver.
- `table-layout: auto` is the one genuine dependent. *(`table-layout` itself is already consumed —
  `table.c:837`, `layout.c:497,3063`; CLAUDE.md's "silently dropped" line is stale.)*

**Net: there is no XL keystone gating the CSS work.** The author-facing
`width: min-content|max-content|fit-content` keyword is worth adding on its own merits (~6-8 files:
propstrings, `properties.h`, the `autogenerated_*` parsers, propset/propget type-3 handling,
`s_*.c` cascade, layout consumer — using free code point 3 with the keyword riding the 5 dead
unit bits), but it should be scheduled as a normal M, **not as a prerequisite round**. The batches
below are far more parallelisable than a first read of the tracker suggests.

---

## 7. Corrections to propagate into CLAUDE.md

1. **The `bits[16]` block is over-stated** — document the scalar-tail hatch as the shipped answer (§5).
2. **`table-layout` is consumed, not dropped** (`table.c:837`, `layout.c:497`, `:3063`).
   `fill-opacity`/`stroke-opacity` are *computed-but-unconsumed*, not "dropped at parse".
3. **`document.title` is dead code, not a bad getter** — `qjs_document_title_get/_set` are defined
   at `macsurf_qjs.c:1239/1246` and referenced **exactly once each** (their own definition). Nothing
   wires them onto `doc`, so `document.title` is `undefined`, not `''`. `document.title.indexOf(…)`
   **throws**. Strictly worse than #266 claims; one-line fix beside the cookie pair at `:6595`.
4. **`tagName` is already inconsistent in-tree** (#299): real elements report lowercase `:1776`,
   fallback elements report **uppercase** (`:6667`, `:8319`), and MacSurf's own XF.Editor shim at
   `:9190` compares `el.tagName==='TEXTAREA'` — **a comparison that never matches today**.

---

## 8. Batches

Ordered hardest-first per the maintainer's direction. Phase 0 is zero-code and runs in parallel
because it needs the maintainer's confirmation, not engineering time.

**PHASE 0 — Tracker hygiene (no code).** Confirm-close §2 (15), reframe §3 (6), apply the §4
wontfix splits, refresh #267 with the measured 161/136/~25 and the fixes1039 WANT list.
Makes every other estimate trustworthy. **~21 issues off the board.**

**BATCH A — Form controls (#80 + #90 merged, + #113). Re-scoped from XL to M/L — see §9.**

**BATCH B — Intrinsic-sizing keyword (M, no longer a prerequisite).** `width/height/min-*/max-*:
min-content|max-content|fit-content` via free code point 3. Independent; schedule on its merits.

**BATCH C — "consumer already has the value" (S each, one round).** #256 `box_decoration_break`
(field exists, consumer missing) · #258 fill/stroke-opacity (computed, unconsumed) · #274
(preprocessor alias → `overflow-wrap:break-word`, already consumed — take option (b), ~10 lines)
· #305 nav-time reset · #114 remainder (`<a download>`, `<a target>`) · #306 (PAGEMAP probe first).
**Highest value/effort ratio in the tree; nothing here touches `css_computed_style_i`.**

**BATCH D — JS truth-telling (S each).** #299 tagName uppercase + `localName` · #266
`document.title` wiring · #130 `getRandomValues` → macEntropy · #265 geometry gate.
**Ordering trap: #299 must precede #262's serializer** — `outerHTML` at `:3052` builds from
`el.tagName`, and real browsers serialize *lowercase*. Flip first, then serialize from `localName`.

**BATCH E — URL-bar cluster (S each, one round).** #236 + #237 + #238 + #243. All inside
`draw_url_bar` (`main.c:133-160`), the URL key path (`main.c:1286-1310`), and `window.c:191/591/2031-2046`.
Touched on every single navigation.

**BATCH F — Preferences file (M, then unblocks).** No persistent prefs file exists anywhere —
zero hits for `kPreferencesFolderType`; `nsoption_*` is only ever set programmatically
(`main.c:1898-1950`). Build `macos9_prefs.c` once against the `macos9_disk_cache.c:226` pattern;
then #241, #154, and any future tabs pref are incremental. **Do not start #241/#154 without it.**

**BATCH G — Menus and actions (M).** #102 + #242 + #97. Shared `PopUpMenuSelect`
(`macos9_chrome_extras.c:1947`) + Carbon control track + NavPutFile. `save_link` is currently
NULL at `window.c:2701`.

**PARK (record-only):** #34, #37, #66, #75, #81, #83, #88, #163, #259, #269, #98, #156, #245,
#248. All "never parsed" or platform-hostile, all XL, none load-bearing on a real page today.

---

## 9. #80 `appearance` — re-scoped from XL to M/L

CLAUDE.md's premise for this issue is **wrong**, and it is the reason #80 has been sized as
"its own dedicated round" for months:

> *"form controls render via Carbon Control Manager (`Draw1Control`); `appearance:none` means
> synthetic CSS-painted controls + new click/key handling"*

**Form controls do not go through the Control Manager at all.** Verified:

- `Draw1Control` appears in `frontends/macos9/window.c` **only for browser chrome** — scrollbars
  (`:464-465`) and the back/forward/stop/reload/home buttons (`:1202-1206`). Zero form-control uses.
- Gadgets are painted by NetSurf's **own plotters** in `content/handlers/html/redraw.c`:
  `html_redraw_checkbox` (`:4718`), `html_redraw_radio` (`:4726`), `html_redraw_file` (`:4734`).
  `html_redraw_checkbox` is pure `ctx->plot->rectangle` + plot styles — and fixes829/#252 already
  threads a **CSS-derived** `accent-color` into its fill/stroke.

**So the synthetic painter already exists.** What is actually missing is narrower:

1. **`appearance` is not parsed anywhere** — zero hits in `libcss/src/parse/properties/`, zero in
   `libcss/include/libcss/properties.h`, zero in the `cssh_css.c` preprocessor. It needs a real
   property add. **But it is keyword-valued**, so it takes the scalar-tail hatch (§5) — an
   `int32_t appearance` appended after `z_index`, **no bit pressure, no `bits[17]`**.
2. **CSS background/border already apply to text-ish gadgets but not to the rest.** Both the
   background gate (`redraw.c:4239-4242`) and the border gate (`:4290-4293`) whitelist exactly
   `GADGET_TEXTAREA` / `GADGET_TEXTBOX` / `GADGET_PASSWORD`. `GADGET_CHECKBOX` and `GADGET_RADIO`
   appear **only** at their paint sites (`:4718`, `:4726`) and in neither whitelist — which is
   precisely why you cannot style a checkbox today.
3. **`appearance:none` then reduces to two edits**: gate the `html_redraw_checkbox`/`_radio` calls
   on `appearance != none`, and extend those two whitelists so the normal box painter (background,
   border, border-radius — all already implemented) takes over.

**Revised plan:** one property-add round (mechanical, no bit pressure) + one redraw round. Hit/key
handling is *not* in scope for `appearance:none` on checkbox/radio — they are already hit-tested as
boxes. #113's modern input types then ride the same painter, as originally planned.

**Net effect of §6.1, §6.2 and §9 together: the three batches that were sized XL are all M/L.
There is no XL work left in the CSS/layout tracker.**
