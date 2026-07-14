# CSS Property Reference for MacSurf Gaps

> **Provenance:** Spec- and MDN-cited reference material gathered **2026-07-13** for the
> road-to-100% CSS work (**#267**). This is external reference material (W3C spec + MDN
> behaviour, values, and implementation notes per property), not a code audit of the
> current tree. For the *code-verified* gap inventory (what MacSurf actually parses,
> consumes, drops, or shims today) see the companion doc
> [css-gap-inventory-2026-07-13.md](css-gap-inventory-2026-07-13.md).

Reference material for implementing MacSurf's missing/partial CSS properties on a
NetSurf/libcss + QuickDraw (Mac OS 9) layout engine. Each entry: spec behavior + URL,
values, layout/paint implementation notes, and difficulty for a QuickDraw engine.

Difficulty legend (QuickDraw/OS 9 classic): **easy** = property value plumbed into
existing paint or a trivial layout tweak; **medium** = touches the line-breaker or
box-model but no new subsystem; **hard** = needs a new layout/paint subsystem or
Unicode/ICU-class machinery classic Mac lacks.

---

## GROUP 1 — Text / Typography (highest priority)

### text-align-last
- **Spec**: Sets alignment of the *last* line of a block (or the line right before a
  forced `<br>`). With `auto`, follows `text-align` — except when `text-align: justify`,
  where it falls back to `start`. https://developer.mozilla.org/en-US/docs/Web/CSS/text-align-last
  Spec: https://www.w3.org/TR/css-text-3/#text-align-last-property
- **Values**: `auto` (default) | `start` | `end` | `left` | `right` | `center` |
  `justify`. `start`/`end` resolve against `direction` (LTR: start=left).
- **Impl**: Consumed by the line-alignment step *after* line-breaking, only for the
  block's final line box (and lines terminated by forced break). Purely a per-line-box
  horizontal offset (or, for `justify`, extra inter-word distribution) applied at layout
  time; no box-model change. Requires the engine to *tag* which line box is "last".
- **Difficulty**: **easy**. Only affects x-offset of one already-computed line box.

### hyphens
- **Spec**: Controls hyphenation break opportunities inside words. `none` = never break
  in-word (not even at soft hyphens); `manual` = break only at U+00AD (`&shy;`) / U+2010;
  `auto` = engine may insert breaks per language dictionary (uses `lang`/`xml:lang`).
  A break inserted at a soft hyphen renders a visible hyphen glyph.
  https://developer.mozilla.org/en-US/docs/Web/CSS/hyphens
  Spec: https://www.w3.org/TR/css-text-4/#hyphens-property
- **Values**: `none` | `manual` (default) | `auto`.
- **Impl**: Line-breaker input. `manual` is cheap: treat U+00AD as a candidate break;
  if the break is taken, append the hyphenation char to the line before wrapping.
  `auto` needs per-language hyphenation dictionaries (Liang/TeX patterns) — a whole
  subsystem. Interacts with `word-break: break-all` (suppresses hyphens) and
  `overflow-wrap`. Also see `hyphenate-character`.
- **Difficulty**: `manual` = **easy** (soft-hyphen handling in breaker). `auto` = **hard**
  (needs dictionaries; heavy on 68k/PPC RAM/ROM budget — ship `manual` only, treat
  `auto` as `manual`).

### text-justify
- **Spec**: Selects the justification method applied when `text-align: justify` (or
  `text-align-last: justify`) is in effect. No effect otherwise.
  https://developer.mozilla.org/en-US/docs/Web/CSS/text-justify
  Spec: https://www.w3.org/TR/css-text-4/#text-justify-property
- **Values**: `auto` (default; UA picks) | `none` (disable justification) | `inter-word`
  (distribute extra space at word separators, i.e. vary spacing between words) |
  `inter-character` (distribute between every typographic char cluster; for CJK) |
  `distribute` (legacy alias of `inter-character`).
- **Impl**: Consumed by the justification pass. `inter-word` = divide the line's slack
  by the count of expansion opportunities (spaces) and add to each space advance.
  `inter-character` = add slack between grapheme clusters (needs cluster segmentation).
  Pure advance-width adjustment at paint/positioning; no reflow.
- **Difficulty**: `inter-word` = **easy** (count spaces, spread slack). `inter-character`
  = **medium/hard** (grapheme segmentation; for Latin-only build, map to `inter-word`).

### hanging-punctuation
- **Spec**: Lets certain punctuation hang outside the line box's start/end edge so text
  edges align optically. `first` = opening quotes/brackets (Unicode Ps/Pf/Pi + `'"`) hang
  at line start; `last` = closing (Pe/Pf/Pi + `'"`) hang at line end; `allow-end` = stops/
  commas hang at end only if they don't fit; `force-end` = always hang end stops/commas.
  https://developer.mozilla.org/en-US/docs/Web/CSS/hanging-punctuation
  Spec: https://www.w3.org/TR/css-text-3/#hanging-punctuation-property
- **Values**: `none` (default) | one or more of `first` | `last` | `force-end` |
  `allow-end` (`force-end`/`allow-end` mutually exclusive).
- **Impl**: Line-breaker + positioning. The hanging glyph's advance is excluded from the
  line's available-width accounting (start char shifts left of content edge; end char
  extends past it). Affects break decisions (`allow-end`) and x-position of first/last
  glyph. Requires Unicode category lookup for the affected chars.
- **Difficulty**: **medium**. Small char set, but touches width accounting + break logic;
  low real-world payoff — reasonable to defer.

### tab-size
- **Spec**: Width of the tab char U+0009 when tabs are preserved (`white-space: pre`/
  `pre-wrap`/`pre-line` with tabs). Integer = multiple of the space (U+0020) advance;
  `<length>` = absolute. Tab advances to the next tab stop (min one space).
  https://developer.mozilla.org/en-US/docs/Web/CSS/tab-size
  Spec: https://www.w3.org/TR/css-text-3/#tab-size-property
- **Values**: `<integer>` (default `8`) | `<length>`. Inherited.
- **Impl**: Text-measurement/positioning. When laying out a preserved tab, compute the
  x of the next tab stop relative to the line's start edge and set the tab's advance to
  reach it (not a fixed width). Only matters in `pre`-family white-space.
- **Difficulty**: **easy**. One measurement rule in the run-positioning loop.

### word-spacing
- **Spec**: Extra spacing added to each word-separator char (primarily U+0020). Adds to
  (does not replace) the space's normal advance; can be negative.
  https://developer.mozilla.org/en-US/docs/Web/CSS/word-spacing
  Spec: https://www.w3.org/TR/css-text-3/#word-spacing-property
- **Values**: `normal` (0) | `<length>` | (`<percentage>` in Text L4, of space advance).
  Inherited.
- **Impl**: Add the resolved length to the advance of every space glyph during run
  positioning. Interacts with justification (justification adds *on top*). Affects line
  width so it feeds back into line-breaking.
- **Difficulty**: **easy**. Advance bump on spaces (MacSurf likely handles the basic
  case; edge cases = negative values, non-U+0020 separators, % resolution).

### letter-spacing (edge cases)
- **Spec**: Extra spacing between characters (tracking), added after each typographic
  char cluster. `normal` computes to `0`. Applies at line-box edges too (trailing
  letter-spacing at line end is trimmed per Text L3/L4 nuance). Can be negative.
  https://developer.mozilla.org/en-US/docs/Web/CSS/letter-spacing
  Spec: https://www.w3.org/TR/css-text-3/#letter-spacing-property
- **Values**: `normal` | `<length>` (`<percentage>` in L4). Inherited.
- **Edge cases MacSurf likely mishandles**: (1) trailing spacing not trimmed at forced/
  soft line ends; (2) spacing applied *inside* ligatures/grapheme clusters instead of
  only between clusters; (3) negative values causing overlap and width underflow; (4)
  interaction with justification and with `text-align` centering (extra width must be in
  the used line width); (5) not applied at the last char before a break.
- **Impl**: Advance bump between clusters in run positioning; feeds line-breaking width.
- **Difficulty**: **easy/medium**. Base case easy; correct cluster + edge-trim = medium.

### writing-mode
- **Spec**: Sets block-flow direction and whether lines are horizontal or vertical.
  `horizontal-tb` = lines horizontal, blocks stack down. `vertical-rl`/`vertical-lr` =
  lines vertical, blocks stack left / right. `sideways-rl`/`sideways-lr` = vertical block
  flow with glyphs rotated 90°. Remaps which physical axis is inline vs block.
  https://developer.mozilla.org/en-US/docs/Web/CSS/writing-mode
  Spec: https://www.w3.org/TR/css-writing-modes-3/#block-flow
- **Values**: `horizontal-tb` (default) | `vertical-rl` | `vertical-lr` | `sideways-rl` |
  `sideways-lr`. Inherited.
- **Impl**: This is *foundational*, not a paint add: the entire layout must be expressed
  in logical (inline/block) coords and mapped to physical at the end. Line-breaker fills
  the inline axis (now vertical); logical margins/padding/sizes (`inline-size`, etc.)
  remap; glyphs need rotation/upright orientation (`text-orientation`).
- **Difficulty**: **hard**. Requires a logical-axis layout model + glyph rotation
  (QuickDraw text is horizontal-only; vertical needs offscreen rotate or per-glyph
  placement). Very low payoff for a classic-Mac Western browser — recommend supporting
  only `horizontal-tb`, gracefully ignore the rest.

### unicode-bidi (+ direction)
- **Spec**: Together with `direction`, controls bidirectional reordering. `normal` =
  implicit UBA applies across boundaries; `embed`/`isolate` open an embedding/isolation
  level using `direction`; `bidi-override` forces visual order per `direction`;
  `isolate-override` = isolate + override; `plaintext` = compute direction from content
  (UBA P2/P3) ignoring parent.
  https://developer.mozilla.org/en-US/docs/Web/CSS/unicode-bidi
  Spec: https://www.w3.org/TR/css-writing-modes-3/#unicode-bidi
- **Values**: `normal` (default) | `embed` | `isolate` | `bidi-override` |
  `isolate-override` | `plaintext`. Not inherited.
- **Impl**: Runs in the line-breaker/reordering stage: split each line into
  directional runs (Unicode Bidirectional Algorithm), reorder runs for visual display,
  then position. `direction: rtl` alone flips block inline base direction. Needs a UBA
  implementation and bidi mirroring of paired glyphs.
- **Difficulty**: **hard** (full UBA + mirroring). `direction: rtl` for pure-RTL content
  without mixing is **medium**. Low payoff for Western-target build — implement
  `direction`, stub bidi to LTR/RTL base only.

### word-break
- **Spec**: Whether/where lines may break *within* words. `normal` = default script
  rules. `break-all` = allow a break between any two chars to avoid overflow (excludes
  CJK from mid-char break the way it does non-CJK). `keep-all` = forbid breaks in CJK
  runs (non-CJK behaves as normal). `break-word` (legacy) = `overflow-wrap: anywhere`
  with `word-break: normal`.
  https://developer.mozilla.org/en-US/docs/Web/CSS/word-break
  Spec: https://www.w3.org/TR/css-text-3/#word-break-property
- **Values**: `normal` (default) | `break-all` | `keep-all` | `break-word` (legacy).
- **Impl**: Line-breaker: expands/restricts the set of break opportunities.
  `break-all` = every inter-char boundary becomes a soft break candidate. `keep-all`
  needs CJK detection to suppress the implicit CJK per-char breaks. Affects min-content.
- **Difficulty**: **easy/medium**. `break-all` easy for Latin. `keep-all` needs CJK
  script classification (medium; skip if CJK unsupported).

### overflow-wrap / word-wrap
- **Spec**: Emergency in-word breaking only when a word can't fit its line without
  overflow. `normal` = break only at normal opportunities. `break-word` = allow breaking
  an otherwise-unbreakable word, but these emergency breaks do *not* count toward
  `min-content` intrinsic size. `anywhere` = same breaking, but they *do* count toward
  min-content (so the box can shrink narrower). `word-wrap` is a legacy alias.
  https://developer.mozilla.org/en-US/docs/Web/CSS/overflow-wrap
  Spec: https://www.w3.org/TR/css-text-3/#overflow-wrap-property
- **Values**: `normal` (default) | `anywhere` | `break-word`. Inherited.
- **Impl**: Line-breaker fallback: if the next unbreakable run overflows the line and
  the line is empty (or after all normal opportunities fail), insert a forced break at
  the last char that fits. The `anywhere` vs `break-word` distinction only matters for
  intrinsic-size (min-content) computation used by shrink-to-fit/tables/flex.
- **Difficulty**: **easy/medium**. Emergency break easy; correct min-content difference
  = medium (matters for `table-layout` and shrink-to-fit sizing).

### text-overflow
- **Spec**: How clipped inline overflow is *signaled* — it does NOT itself cause
  overflow. Requires `overflow` != visible and a non-wrapping condition (`white-space:
  nowrap` for the classic single-line case). `clip` truncates at the content edge;
  `ellipsis` renders `…` (U+2026) at the affected edge, shortening visible text; a
  `<string>` renders a custom marker. First value = line-start edge, second = line-end.
  https://developer.mozilla.org/en-US/docs/Web/CSS/text-overflow
  Spec: https://www.w3.org/TR/css-overflow-3/#text-overflow
- **Values**: `clip` (default) | `ellipsis` | `<string>` | (two-value start/end form).
- **Impl**: Paint-time on the clipped line box: measure from the overflow edge inward,
  find the last glyph that leaves room for the ellipsis/string, paint text up to there
  then the marker. Applies in the inline (usually horizontal) direction only. No layout
  reflow; needs the box's clip rect (which QuickDraw clip regions provide natively).
- **Difficulty**: **easy**. Backward width measurement + draw `…`; QuickDraw ClipRect
  makes the clipping trivial.

---

## GROUP 2 — Tables (high priority)

### table-layout
- **Spec**: `auto` = column widths derived from content (measure all cells, distribute).
  `fixed` = widths depend only on table width, explicit `<col>`/first-row cell widths,
  borders and cell spacing — content is NOT measured; the table can be laid out from the
  first row. Under `fixed`, columns get width from (1) `<col>`/cell explicit width, else
  (2) share of remaining table width; overflow is clipped/handled per `overflow`. If
  table `width` is `auto`, `fixed` degrades to `auto`.
  https://developer.mozilla.org/en-US/docs/Web/CSS/table-layout
  Spec (auto algo): https://www.w3.org/TR/CSS22/tables.html#auto-table-layout ;
  (fixed): https://www.w3.org/TR/CSS22/tables.html#fixed-table-layout
- **Values**: `auto` (default) | `fixed`.
- **Impl**: Two distinct table-width algorithms in the table layout module. `fixed` is
  much simpler and cheaper: single pass off row 1. `auto` requires computing min/max
  content widths per column (uses `overflow-wrap`/`word-break` min-content!) then a
  constraint-solving distribution. This is the core table sizing decision.
- **Difficulty**: `fixed` = **medium**; `auto` = **hard** (min/max content solver over
  all cells, spanning cells, percentage columns). Implement `fixed` first — it also
  enables reliable `text-overflow` in cells.

### empty-cells
- **Spec**: In the *separated-borders* model (`border-collapse: separate`) only, controls
  whether borders and background are painted around cells with no in-flow content.
  `show` = paint them; `hide` = don't paint border/background for empty cells (and if a
  row is entirely empty hidden cells, the row may collapse). Ignored when
  `border-collapse: collapse`.
  https://developer.mozilla.org/en-US/docs/Web/CSS/empty-cells
  Spec: https://www.w3.org/TR/CSS22/tables.html#empty-cells
- **Values**: `show` (default) | `hide`. Inherited.
- **Impl**: Pure paint-time gate: when a table cell has no content and value is `hide`
  and model is separate, skip drawing that cell's border + background. "No content"
  includes whitespace-only per the collapsing rules. No layout effect (except the
  all-empty-row collapse edge case).
- **Difficulty**: **easy**. A boolean check in the cell paint routine.

### border-collapse (edge cases)
- **Spec**: `separate` = each cell has its own borders separated by `border-spacing`
  (and honors `empty-cells`). `collapse` = adjacent cell borders merge into one via the
  collapsing-border model; `border-spacing`/`empty-cells` no longer apply; `inset`→
  `ridge`, `outset`→`groove`. When two cells share an edge, a *conflict-resolution*
  algorithm picks the winning border: (1) `border-style: hidden` beats all (suppresses
  edge); (2) wider `border-width` wins; (3) tie → style priority `double > solid >
  dashed > dotted > ridge > outset > groove > inset`; (4) still tied → the border of the
  element earliest in this order wins: cell > row > row-group > column > col-group >
  table.
  https://developer.mozilla.org/en-US/docs/Web/CSS/border-collapse
  Conflict algo: https://www.w3.org/TR/CSS22/tables.html#border-conflict-resolution
- **Values**: `separate` (default) | `collapse`. Inherited.
- **Impl**: `collapse` changes both layout (border box widths are half-attributed to
  each cell; table/cell box edges shift) and paint (draw the single winning border
  centered on the grid line). Must run conflict resolution per shared edge before layout
  so cell positions account for merged border widths. This is the tricky table case.
- **Difficulty**: **medium/hard**. Conflict resolution is fiddly but bounded; the
  half-border geometry and centered painting on grid lines is the error-prone part.

---

## GROUP 3 — Fragmentation (lower priority; screen-first browser)

### break-before / break-after / break-inside
- **Spec**: Control forced/avoided breaks at box boundaries within a fragmentation
  context (paged media, multicol, regions). Forced values (`page`, `column`, `always`,
  `left`, `right`, `recto`, `verso`, `region`) force a break; `avoid*` values suppress
  breaks. Precedence when resolving a boundary: `break-before` > `break-after` >
  `break-inside`; forced beats avoid. Legacy `page-break-before/after/inside` map onto
  these (`always`↔`page`, `avoid`↔`avoid`, etc.).
  https://developer.mozilla.org/en-US/docs/Web/CSS/break-inside
  Spec: https://www.w3.org/TR/css-break-3/#break-between and #break-within
- **Values**: before/after: `auto`|`avoid`|`always`|`all`|`avoid-page`|`page`|`left`|
  `right`|`recto`|`verso`|`avoid-column`|`column`|`avoid-region`|`region`. inside:
  `auto`|`avoid`|`avoid-page`|`avoid-column`|`avoid-region`.
- **Impl**: Consumed only by a fragmentation engine (pagination for print, or
  multi-column layout). On a purely-scrolling screen browser they are no-ops. Relevant
  to MacSurf mainly for **printing** — the paginator, when slicing the block flow into
  page boxes, honors forced/avoid at box boundaries.
- **Difficulty**: **medium** *if* a paginator exists (it's just break-point selection);
  the paginator/multicol subsystem itself is the hard prerequisite. Safe to defer to
  print support; treat as no-op on screen.

### page-break-before / -after / -inside (legacy aliases)
- **Spec**: Legacy shorthands aliased to the `break-*` properties above; kept for compat.
  Same behavior, reduced value set (`auto`|`always`|`avoid`|`left`|`right` etc.).
  https://developer.mozilla.org/en-US/docs/Web/CSS/page-break-before
  Spec: https://www.w3.org/TR/css-break-3/#page-break-properties
- **Impl**: Parse-time alias to the modern property; no separate layout code.
- **Difficulty**: **easy** (aliasing) — behavior difficulty is that of `break-*`.

### orphans / widows
- **Spec**: `orphans` = minimum line boxes of a paragraph left at the *bottom* of a
  fragment before a break; `widows` = minimum lines carried to the *top* of the next
  fragment. The fragmenter must move a break earlier/later to satisfy both. Integer ≥ 1;
  default 2.
  https://developer.mozilla.org/en-US/docs/Web/CSS/orphans (and /widows)
  Spec: https://www.w3.org/TR/css-break-3/#widows-orphans
- **Values**: `<integer>` (default `2`). Inherited.
- **Impl**: Consumed by the paginator/multicol break-placement pass: when choosing a
  break inside a block's line stack, ensure ≥orphans lines precede and ≥widows lines
  follow; otherwise push the whole block (or more lines) to the next fragment. No-op
  without pagination.
- **Difficulty**: **medium** (only inside a paginator; else no-op). Defer with break-*.

---

## GROUP 4 — Backgrounds / Misc

### background-attachment: fixed
- **Spec**: `scroll` = background fixed to the element's own box (scrolls with the box in
  the page, not with the element's own overflow). `local` = background scrolls with the
  element's *contents* (its scroll position). `fixed` = background positioned relative to
  the *viewport*, so it stays put while the page scrolls (parallax). With `fixed`,
  `background-origin` is ignored and the positioning area is the viewport.
  https://developer.mozilla.org/en-US/docs/Web/CSS/background-attachment
  Spec: https://www.w3.org/TR/css-backgrounds-3/#the-background-attachment
- **Values**: `scroll` (default) | `fixed` | `local`. Per background layer (comma list).
- **Impl**: Paint-time: the background image's origin is computed against the viewport
  rect rather than the element's border box, and it must be re-painted on every scroll
  (invalidate on scroll). Forces the element's background painting to not be cached with
  the scrolled content. Performance cost = full repaint of the covered area per scroll.
- **Difficulty**: **medium**. Not conceptually hard, but on QuickDraw + slow classic Mac
  a re-blit-per-scroll is a real perf problem; requires viewport-relative origin math and
  scroll invalidation. Consider mapping `fixed`→`scroll` on low-end hardware.

### quotes
- **Spec**: Defines the strings used by `content: open-quote / close-quote` (and the
  no-op forms). Value is a pairwise list: level-1 open, level-1 close, level-2 open,
  level-2 close, … Nesting depth selects the pair; `no-open-quote`/`no-close-quote` just
  adjust depth without emitting. `auto` = UA picks quotes by content language.
  https://developer.mozilla.org/en-US/docs/Web/CSS/quotes
  Spec: https://www.w3.org/TR/css-content-3/#quotes
- **Values**: `none` | `auto` | `[<string> <string>]+`. Inherited.
- **Impl**: Consumed by the generated-content/`::before`/`::after` machinery: maintain a
  per-element quotation nesting counter; `open-quote` emits quotes[2*depth], increments;
  `close-quote` decrements, emits quotes[2*depth+1]. The emitted glyphs then flow as
  normal inline text. Needs `content` + counters support to be useful.
- **Difficulty**: **easy** (given generated-content support): a small nesting counter and
  string table. `auto` needs a language→quotes table.

### gap / row-gap / column-gap (two-value gap)
- **Spec**: `gap` is shorthand for `row-gap column-gap`. One value sets both; two values
  set row then column. Defines gutters between grid tracks / flex items / multicol
  columns (`column-gap` also applies to multicol). `normal` computes to `1em` for
  multicol column-gap, `0` for flex/grid. Percentages resolve against the container's
  corresponding dimension.
  https://developer.mozilla.org/en-US/docs/Web/CSS/gap
  Spec: https://www.w3.org/TR/css-align-3/#gaps
- **Values**: `row-gap`/`column-gap`: `normal` | `<length-percentage>`. `gap`:
  `<'row-gap'> <'column-gap'>?`.
- **Impl**: Consumed by whatever layout formatter owns the container (flex line packing,
  grid track placement, multicol). Adds gutter space *between* items only (not before
  first / after last). Straightforward additive spacing in the item-positioning loop —
  the "gap" is the two-value parsing + threading row vs column into the right axis.
- **Difficulty**: **easy** for multicol/flex (add gutter in the packing loop). Depends on
  having flex/grid at all; the two-value parse and axis mapping is the actual fix here.

---

## GROUP 5 — Effects (deferred; documented for completeness)

### transition
- **Spec**: Interpolates animatable property values over time on change. Shorthand for
  `transition-property/-duration/-timing-function/-delay`. Requires a per-frame
  animation tick and interpolation of computed values; discrete props snap.
  https://developer.mozilla.org/en-US/docs/Web/CSS/transition
  Spec: https://www.w3.org/TR/css-transitions-1/
- **Impl**: Needs an animation timeline/scheduler, per-property interpolators, and style
  recomputation + repaint each frame. **Difficulty: hard** (no timer/compositor culture
  on classic Mac; would peg the CPU). Defer.

### animation / @keyframes
- **Spec**: Time-based keyframed animation of properties, independent of state change.
  https://developer.mozilla.org/en-US/docs/Web/CSS/animation
  Spec: https://www.w3.org/TR/css-animations-1/
- **Impl**: As transitions plus a keyframe timeline and iteration/direction/fill logic.
  **Difficulty: hard**. Defer.

### clip-path
- **Spec**: Clips an element to a basic shape / geometry-box / SVG path; only the clipped
  region paints and receives events.
  https://developer.mozilla.org/en-US/docs/Web/CSS/clip-path
  Spec: https://www.w3.org/TR/css-masking-1/#the-clip-path
- **Impl**: Paint-time clip. Rect/inset/rounded map onto QuickDraw regions (feasible);
  polygon/ellipse/path need arbitrary region construction or a rasterized mask.
  **Difficulty: medium** for rect/inset (QuickDraw regions), **hard** for polygon/path.
  Defer; if ever done, support `inset()`/`circle()` via regions only.

### mask
- **Spec**: Uses an image/gradient's luminance or alpha as a per-pixel mask on the
  element's rendering.
  https://developer.mozilla.org/en-US/docs/Web/CSS/mask
  Spec: https://www.w3.org/TR/css-masking-1/#masking
- **Impl**: Per-pixel alpha compositing — QuickDraw has no alpha channel; needs an
  offscreen 32-bit buffer and manual blend. **Difficulty: hard**. Defer.

### filter
- **Spec**: Applies graphical filter functions (`blur`, `brightness`, `contrast`,
  `grayscale`, `drop-shadow`, etc.) to an element's rendered output.
  https://developer.mozilla.org/en-US/docs/Web/CSS/filter
  Spec: https://www.w3.org/TR/filter-effects-1/
- **Impl**: Render element to offscreen buffer, run per-pixel convolution/color math,
  composite back. Blur/shadow are expensive convolutions; no GPU. **Difficulty: hard**.
  Defer; a cheap `grayscale`/`brightness` LUT pass would be the only tractable subset.

---

## Recommended implementation order (impact × difficulty)
1. **Easy visual wins**: text-overflow (ellipsis), tab-size, word-spacing/letter-spacing
   edge cases, text-align-last, empty-cells, quotes, gap two-value.
2. **Line-breaker work**: overflow-wrap/word-wrap, word-break (break-all), hyphens
   (manual only), text-justify (inter-word).
3. **Tables**: table-layout: fixed, then border-collapse conflict resolution, then
   table-layout: auto (min/max solver — reuses the overflow-wrap min-content work).
4. **Defer / stub**: writing-mode (horizontal-tb only), unicode-bidi/direction (LTR/RTL
   base only), hyphens: auto, hanging-punctuation, fragmentation (until print), all of
   Group 5 effects, background-attachment: fixed (map to scroll on slow hardware).
