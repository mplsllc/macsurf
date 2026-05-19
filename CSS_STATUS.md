# MacSurf CSS Status Report

Generated 2026-05-19. Last revised 2026-05-19 (fixes132 + fixes133 hardware-accepted).

This is a brutal, hedge-free audit of CSS support in MacSurf. The goal is to identify what works, what doesn't, and what to implement next.

## Hardware-verified status (post fixes134a)

```text
viewport units: ✓ Works
- fixes132 corrected swapped VH/VW conversion in unit.c.
- Affects height/min-height/width/etc. using vh/vw/vmin/vmax.

min-height: ✓ Works
- Source audit showed it was already consumed in layout_apply_minmax_height,
  flex, grid, tables, and replaced/replaced-like paths.
- Previous status was wrong; the visible failure was caused by swapped vh/vw
  unit conversion.

z-index: ✓ Partial
- fixes133 implements basic positioned explicit numeric z-index paint ordering.
- Positive z-index overlays/dropdowns now paint above normal content.
- Equal z-index preserves DOM order.
- Negative z-index and full CSS stacking-context paint order are deferred.

content for ::before / ::after: ✓ string + counter()
- fixes134a + fix1 materialize STRING items as BOX_TEXT under an
  INLINE_CONTAINER wrapper. CSS_CONTENT_SET guard prevents the fixes37
  uninitialised-c_item hang.
- fix1 reuses an existing trailing INLINE_CONTAINER for ::after so the
  generated content renders inline-adjacent to the element text instead
  of dropping to a new line.
- Still skipped without crashing: URI, ATTR, COUNTERS (plural),
  open/close-quote items.

CSS counters: ✓ flat decimal
- fixes134b adds counter-reset, counter-increment, content: counter(name).
- Flat document-scope table on box_construct_ctx. Element NORMAL
  counter-reset/increment fires before ::before; pseudo's own
  counter-reset/increment fires inside box_construct_generate before
  content materialises.
- Decimal output only. counters(name, ".") plural form, roman/alpha
  styles, nested CSS counter scopes deferred.
```

## fixes132 revision

The original audit claimed `min-height` was "NOT consumed in layout". A direct source audit refuted this: `layout_apply_minmax_height` (layout.c:2165) calls `ns_computed_min_height` and applies it via two sites in `layout_block_context` (layout.c:4031, 4089). Flex (layout_flex.c:1360-1362), grid (layout_grid.c:432-434), tables (layout.c:2086), and replaced elements (layout.c:175-178) also honor it. The user-visible "min-height collapses" symptom was actually a **VH/VW swap in `css_unit__px_per_unit`** (unit.c:271-275): `CSS_UNIT_VH` was returning `viewport_width/100` and `CSS_UNIT_VW` was returning `viewport_height/100`. The same file's `css_unit__absolute_len2pt` (lines 107-113) had them correct, so the bug was internal inconsistency, not a missing case. Swapped back in fixes132. This also fixes `height: 100vh`, `width: 100vw`, `vmin`, `vmax`, and any other viewport-unit property — they all routed through the broken `px_per_unit` path.

---

## The honest summary

MacSurf parses **167 CSS properties** via libcss. The layout/redraw pipeline only **reads 87 of them**. The gap between "parsed" and "consumed" is where modern pages fall apart visually — libcss correctly computes the cascade, but the layout engine never asks for the value, so the property does nothing.

Of the 87 consumed properties, **most work**, but a handful are partial (limits or accuracy gaps), and several depend on subsystems (font selection, color resolution) that have their own gaps.

---

## What actually works on real pages

These have been verified on hardware or in screenshots and produce the correct visual result:

### Box model
- `width`, `height`, `min-width`, `max-width`, `max-height`
- `margin` (all sides, `auto` centering)
- `padding` (all sides)
- `border-width`, `border-color`, `border-style` (all sides)
- `border-radius` (rounded corners via `PaintRoundRect`/`FrameRoundRect` — fixes172)
- `box-sizing`
- `box-shadow` (independent h/v offsets + custom colour — fixes175/178)

### Display & positioning
- `display: block | inline | inline-block | none | flex | inline-flex | grid | table | table-cell | table-row | list-item`
- `position: static | relative | absolute | fixed`
- `top`, `right`, `bottom`, `left`
- `float: left | right | none`
- `clear: left | right | both | none`
- `visibility: visible | hidden`
- `z-index: <integer>` (basic positioned stacking, two-pass paint — fixes133)

### Flexbox
- `flex-direction: row | row-reverse | column | column-reverse`
- `flex-wrap: nowrap | wrap | wrap-reverse`
- `flex-grow`, `flex-shrink`, `flex-basis`
- `justify-content: flex-start | flex-end | center | space-between | space-around | space-evenly`
- `align-content` (all values)
- `align-items`, `align-self`
- `order` (stable bubble-sort before layout)
- `gap: N` single-value (both axes get N)
- `column-gap: N`

### Grid (V1 — fixes75 + fixes118)
- `display: grid`
- `-macsurf-grid: N` (MacSurf shorthand for N equal columns)
- `grid-template-columns: <length-list>` (real track widths — fixes118)

### Typography
- `color`
- `font-family` (name match: Geneva, Monaco, Chicago, Charcoal)
- `font-size`
- `font-weight: normal | bold` (via QuickDraw `face=1` bold smear)
- `font-style: normal | italic` (via QuickDraw `face=2`)
- `text-align: left | right | center | justify`
- `text-decoration: underline | overline | line-through`
- `text-indent`
- `text-transform: uppercase | lowercase | capitalize | none`
- `line-height`
- `letter-spacing`
- `word-spacing` (length values; layout + paint in sync — fixes139b)
- `white-space: normal | nowrap | pre`
- `vertical-align`

### Backgrounds
- `background-color`
- `background-image: url(...)` (PNG/GIF/BMP/TIFF/JPEG via lodepng + QT — fixes78-79b)
- `background-position`
- `background-repeat` (repeat / repeat-x / repeat-y / no-repeat all honored by macos9 plot_bitmap tile loop — fixes138)
- `background-attachment: fixed` (viewport-anchored origin + tiles parallax-correct against fixes138 — fixes137 + fixes138)
- `-macsurf-gradient: linear-gradient(...)` (multi-stop — fixes49)
- `-macsurf-gradient: radial-gradient(...)` (24-ring oval stack — fixes74d)

### Lists & content
- `list-style-type` (disc, decimal, etc. — but bullet glyph renders as `;` on G3, see Known Issues)
- `list-style-image`
- `content` for `::before` / `::after` pseudo-elements

### Custom & visual
- CSS Custom Properties / `var()` — full native resolution (fixes133-139)
- `opacity` (QuickDraw stipple pattern at 1.0/0.80/0.50/0.25)
- `cursor` parsed; on hover, the cursor changes via `SetThemeCursor` (fixes131)
- `outline`, `outline-color`, `outline-style`, `outline-width` (parsed and consumed in redraw)
- `clip` (CSS 2 `clip: rect(...)`)
- `-macsurf-transform: rotate() translate() scale()` (fixes73)
- `-macsurf-text-shadow` (fixes50)
- `object-fit: fill | contain | cover | none | scale-down` (fixes116)
- `overflow: visible | hidden | scroll | auto` (clipping applies on block / inline-block / table-cell / flex / inline-flex / grid — fixes131 added flex/grid)
- `border-collapse`, `border-spacing` (tables)

### Pseudo-classes (fixes130 + fixes130e)
- `:hover` (re-cascade on mouse-track)
- `:active`
- `:focus`
- (Static pseudo-classes always worked: `:first-child`, `:nth-child(n)`, etc. via libcss selector engine)

---

## What is PARSED but NOT CONSUMED (the silent-fail category)

These accept author CSS without complaint but have zero effect on rendering. Every one of these is a probable visual bug on real pages.

| Property | Parsed | Layout reads? | Redraw reads? | Impact |
|---|---|---|---|---|
| `background-attachment` | yes | no | **yes (fixes137 + fixes138)** | viewport anchor + repeating tile parallax both shipped. Gradient-fixed deferred. |
| `caption-side` | yes | no | no | **Audited fixes139: deferred.** `<caption>` maps to `BOX_INLINE` in `box_construct.c:139`; no `BOX_TABLE_CAPTION` type exists. Proper support requires new box type + normalise rewrite + table-layout sibling placement. Multi-file structural change, not a minimal round. |
| `column-count` | yes | no | no | multi-column text layout broken. Deferred — text-balancing across columns is genuinely complex. |
| `column-fill` | yes | no | no | depends on column-count |
| `column-rule-*` | yes | no | no | depends on column-count |
| `column-span` | yes | no | no | depends on column-count |
| `column-width` | yes | no | no | depends on column-count |
| `quotes` | yes | no | no | `q { quotes: ... }` ignored |
| `empty-cells` | yes | no | **yes (fixes139a)** | show/hide both honored; hidden empty cells skip background + border paint while keeping their layout slot |
| `table-layout` | yes | no | no | **Audited fixes139: deferred.** 1064 lines of dedicated table.c + table integration in layout.c. Too risky for one round per sprint rule "do not destabilize all table layout". |
| `unicode-bidi` | yes | no | no | bidi text |
| `writing-mode` | yes | no | no | vertical-writing pages broken |
| `word-spacing` | yes | **yes (fixes139b)** | **yes (fixes139b)** | length values shift word gaps; layout and paint both updated so wrap point follows |
| `break-after`, `break-before`, `break-inside` | yes | no | no | print/column breaks |
| `page-break-*` | yes | no | no | print breaks |
| `orphans`, `widows` | yes | no | no | print typography |
| `fill-opacity`, `stroke-opacity` | yes | no | no | SVG-only, low priority |

**Highest-impact silent fails on real pages, ranked:**

1. ~~**`background-attachment: fixed`**~~ **Shipped at fixes137 + fixes138 (2026-05-19).** Viewport-anchored origin + repeating-tile parallax both work end-to-end. Gradient-fixed still deferred.
2. ~~**`empty-cells`**~~ **Shipped at fixes139a (2026-05-19).** Hidden empty cells skip background + border paint; cell still occupies its layout slot. Whitespace-only counts as empty per spec.
3. ~~**`word-spacing`**~~ **Shipped at fixes139b (2026-05-19).** Length values resolve through the same plot_font_style_t field as letter-spacing; macos9_font_measure counts ASCII spaces and updates the wrap point so layout and paint agree.
4. **`column-count`** — Magazine-style multi-column text. Deferred — text-balancing across columns is genuinely complex.
5. **`caption-side`** — Audited fixes139, **deferred**. Captions currently fall through as BOX_INLINE because no BOX_TABLE_CAPTION type exists in this fork. Real support needs box_construct + box_normalise + table-layout coordination.
6. **`table-layout`** — Audited fixes139, **deferred**. 1064 lines of dedicated table layout code; the sprint rule "do not destabilize all table layout" forbids a one-round attempt.

(`min-height` and viewport units were previously listed here. Both shipped in fixes132. `z-index` shipped in fixes133. `counter-increment` / `counter-reset` shipped in fixes134b — see "What actually works" section.)

---

## What is BROKEN or PARTIAL on consumed properties

### `gap: A B` two-value form (fixes148 limitation)
Single-value `gap: N` works (both axes get N). Two-value `gap: A B` loses A and stores only B as column-gap. Fix requires adding `CSS_PROP_ROW_GAP` as an independent property with its own bit slot in `css_computed_style_i.bits[]`. Bit budget audit shows word 15 has 27 free bits, word 14 bottom 5 bits are full. ~17 files to touch. Real-world impact: 97% of MacTrove pages use single-value form, deferred.

### `font-family` matching
Currently only matches `Geneva`, `Monaco`, `Chicago`, `Charcoal` by name. Any other family name falls through to the OS 9 default font for the resolved generic. Modern sites specifying `"Helvetica Neue", system-ui, sans-serif` get the system font, not their preferred. Not strictly broken — there are no other fonts installed by default — but the matching is narrow.

### `font-weight` granularity
Only `bold` (>= 600) vs `normal` (< 600). Numeric weights 100/200/300/400/500/600/700/800/900 all collapse to two values. Acceptable for QuickDraw which only has bold/non-bold.

### `text-overflow: ellipsis`
**fixes135a + fixes135c (2026-05-19): WORKS on hardware.** Parser/cascade/computed plumbing in 135a; visual paint-after rendering in 135c, accepted on G3. The 135c architecture paints the ellipsis as a separate overlay: the original text draws normally (overflow:hidden clips at the container edge), then a background-coloured rect followed by a `…` text call paints over the rightmost slice. Avoids the text-buffer-mutation bug that broke the earlier fixes135b attempt. The macos9 plot_text path folds U+2026 to MacRoman 0xC9 ([macos9_font.c:109](browser/netsurf/frontends/macos9/macos9_font.c#L109)) so the same three UTF-8 bytes render correctly. Deferred to V2: multi-line ellipsis, two-value form (`text-overflow: clip ellipsis`), custom string marker, RTL/start-side ellipsis, ellipsis across complex nested inline boxes.

### `word-break` / `overflow-wrap` / `word-wrap`
**fixes136a (2026-05-19): plumbing accepted. fixes136b deferred.**

Parsed/computed status:
- `word-break: normal | break-all | keep-all` — parses, computes correctly.
- `overflow-wrap: normal | break-word | anywhere` — parses, computes correctly.
- `word-wrap: break-word` — parses as a legacy alias for `overflow-wrap`.
- Invalid values fall back to `normal`. `css_computed_word_break()` and `css_computed_overflow_wrap()` return the right enum.
- `keep-all` is parsed for forward-compat but inert without CJK segmentation. `anywhere` aliases to `break-word` semantically.

**Current layout behaviour (intentional deviation from spec).** MacSurf's inline layout already performs emergency character-boundary wrapping for long unbreakable runs even when the author has not set `overflow-wrap: break-word`. [macos9_font_split](browser/netsurf/frontends/macos9/macos9_font.c#L310) line 352-364: if no space character fits in the available width, it hard-breaks at the next character boundary. This matches the practical effect of `overflow-wrap: break-word` / `word-break: break-all` for URLs and hashes, but does **not** yet distinguish strict `normal` vs `break-word` vs `break-all` semantics. Hardware-confirmed at fixes136a: long URLs and 64-char hash tokens wrap mid-character without needing any author CSS.

A strict-spec fixes136b that disables this hard-break for `overflow-wrap: normal` was considered and rejected: real pages do not explicitly opt into `break-word`, so making `normal` strict would horizontally overflow every long URL or hash that authors haven't pre-formatted. Practical readability wins over spec strictness. The libcss plumbing still pays off when a future caller (e.g. a CSS reset that wants strict non-breaking, or richer line-break logic for `break-all`'s aggressive mode) needs to read the computed value.

If a real page later breaks because the default is too aggressive, fixes136b can revisit a narrower change: make `overflow-wrap: normal` + `word-break: normal` together signal "do not character-break", while leaving every other combination on the friendly default.

### `clip-path` and `mask`
Not implemented. Decorative shape clipping silently ignored. Pages degrade to rectangular fallback (usually fine).

### `transition` and `animation`
Deferred to v0.4.5 (already noted in CLAUDE.md). MacSurf has a one-shot `-macsurf-animation-rotate` extension but no real CSS animation property support.

### Bitmap rendering
- PNG transparency: 1-bit `CopyMask` threshold (anti-aliased edges become binary)
- TIFF: opaque rendering only (`QTNewGWorld(k32ARGBPixelFormat)` returns `cDepthErr -157` on OS 9)
- Path A1.5 (`CopyDeepMask` + 8-bit mask) is queued

### List bullets render as `;`
Known issue from fixes33 era. `list-style-type: disc` resolves correctly in cascade but the glyph rendering shows `;` instead of `•` on G3 hardware. Documented in [docs/css-milestone-2026-05-13.md](docs/css-milestone-2026-05-13.md).

### Inline boxes occasionally duplicate
Known issue post-fixes33. Some inline-box runs render twice. Not blocking comprehension; cause unknown.

### URL bar input on initial window
Probably fixed by fixes77g, needs verification. Workaround: File → New Window.

---

## Implementation priorities

Ranked by visual impact on real-world pages. Each priority is a discrete shippable round.

### P0 — `z-index` basic positioned stacking — SHIPPED (fixes133)

`html_redraw_box_children` in `redraw.c` now paints in two passes: pass 1 walks non-z-indexed children in DOM order; pass 2 stable-sorts positioned children with explicit `z-index` by ascending z-index and paints them on top. Fixed-size buffer (64) per level — overflow falls back to DOM order paint. Negative z-index treated same as zero in this pass (paints after parent's content, not before). Full CSS 2.1 painting order (negative-z-before-parent, opacity/transform stacking contexts, full 7-level paint order) is deferred to v2.

### P1 — `min-height` — SHIPPED (fixes132)

`min-height` was already wired across block, flex, grid, table, and replaced-element layout paths. The user-visible symptom (`min-height: 100vh` collapsing to text height) traced to the VH/VW swap in `css_unit__px_per_unit`, which fixes132 corrected.

### P2 — `text-overflow: ellipsis` (MEDIUM-HIGH impact, MEDIUM effort)

Card components on every modern UI use this to truncate long titles. Combined with fixes131's overflow-clipping on flex containers, ellipsis would finally make cards look polished.

**Scope:**
- Parse `text-overflow` property (new parser + selector files)
- In `layout_inline.c` (or wherever line boxes are finalized), when overflow:hidden is set and a line would exceed its container, truncate at the last fitting character and append `…`
- Mac OS 9 `…` glyph = `0xC9` in MacRoman, which we already convert from UTF-8

**Files:** new `p_text_overflow.c`, `s_text_overflow.c`, modifications to `layout_inline.c`, `propstrings.h`, dispatch tables.
**Estimated rounds:** 1-2.

### P3 — Viewport units `vw` / `vh` / `vmin` / `vmax` — SHIPPED (fixes132)

The unit-handler in `css_unit__px_per_unit` (unit.c:271-275) had `CSS_UNIT_VH` and `CSS_UNIT_VW` swapped — VH returned `viewport_width/100` and VW returned `viewport_height/100`. The same file's `css_unit__absolute_len2pt` had them correct. Swapped back in fixes132. `vmin`/`vmax` route through `css_unit__map_viewport_units` to VH or VW, so they get fixed automatically.

### P4 — `counter-increment` / `counter-reset` (MEDIUM impact, MEDIUM effort)

Numbered headings, ordered lists with custom numbering, table-of-contents formats. NetSurf's `content_item` already has counter types in the cascade; we just don't drive them.

**Scope:**
- Walk DOM in document order, maintaining a counter table per scope
- Resolve `counter(name)` in `content: counter(chapter)` to the current value
- Reset on `counter-reset` declarations

**Files:** new `redraw_counters.c` helper invoked from `html_redraw_box` for boxes with `content` containing counter items.
**Estimated rounds:** 2-3.

### P5 — `word-break: break-all` / `overflow-wrap: break-word` (LOW-MEDIUM impact, LOW effort)

Long URLs in body text overflow containers. Word-break forces character-level wrapping when normal word breaks won't fit.

**Scope:**
- In `layout_inline.c`, when measuring a line and a single word exceeds the line width, allow mid-word breaks
- Property already parsed by NetSurf's libcss

**Files:** `layout_inline.c`.
**Estimated rounds:** 1.

### P6 — `transition` / `animation` (deferred to v0.4.5)

Animation framework would touch event loop, paint scheduling, time tracking. Substantial scope. Defer.

### P7 — `column-count` for multi-column text (LOW-MEDIUM impact, HIGH effort)

Magazine-style layout. Genuinely complex (text balancing across columns). Defer until proven needed.

### P8 — `clip-path`, `mask`, `filter` (LOW impact, HIGH effort)

Decorative. Pages degrade to rectangular fallback which is acceptable. Defer indefinitely.

---

## Test plan

Without dedicated regression tests, every CSS round needs a real-page verification step. Suggested gauntlet:

1. **`tests/css/z_index.html`** — three boxes with overlapping `position: absolute` and explicit z-index — paint order should match z-index, not DOM order.
2. **`tests/css/min_height.html`** — a `div` with `min-height: 300px` containing one short line of text — should be 300px tall.
3. **`tests/css/text_overflow.html`** — card with `width: 200px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap` and 500px of text — should show `Very long title…`
4. **`tests/css/viewport_units.html`** — full-screen hero with `height: 100vh` — should fill the viewport.
5. **`tests/css/counters.html`** — `<h2>` styled with `counter-increment` and `content: counter(...)` — should auto-number.
6. **Real-page regression: MacTrove home, MacTrove app page, DuckDuckGo Lite, Wikipedia article** — visual diff against previous shipped state.

---

## Out-of-CSS items affecting page rendering

These are not CSS properties but affect whether pages "load properly":

1. **HTTP fetcher reliability** — fixes98-105 closed the major leaks; current state is stable across many-page browsing.
2. **TLS** — handled by proxy (out of scope for the browser).
3. **JavaScript** — Duktape ES5 in base build; modern JS still needs proxy render-and-flatten.
4. **Forms** — `<input>` rendering works; form submission path is wired. Style cascade reaches form controls.
5. **Tables** — table layout works for simple tables; complex tables (colspan/rowspan with auto-layout) may have gaps.

---

## What I would ship next

P1 (min-height) and P3 (viewport units) shipped in fixes132 as a 2-line swap. Top of remaining stack: **P0 (z-index) — biggest visual fix left**, then P2 (text-overflow ellipsis), then P5 (word-break / overflow-wrap), then P4 (counters).
