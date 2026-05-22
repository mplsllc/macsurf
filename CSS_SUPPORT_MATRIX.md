# MacSurf CSS Support Matrix

**Generated 2026-05-22 (fixes200).** Regenerate inventories with
`tools/css_audit.sh`; outputs land under `tools/audit/`. This file
is the human-curated map; the audit script is the ground truth for
what libcss currently parses / exposes / consumes.

## Status legend

| Status | Meaning |
|---|---|
| FULL | Parsed, cascaded, consumed by layout/redraw/hit-test. Probe page passes on hardware. |
| PARTIAL | Implemented with documented limits. |
| PARSED_NOT_CONSUMED | libcss parses + cascades, but no layout/redraw call site reads the accessor. Silent drop. |
| MISSING_PARSER | libcss does not parse the property — value is discarded at parse time. |
| NEEDS_LAYOUT | Parsed + consumed by redraw, but layout doesn't honour it. |
| NEEDS_REDRAW | Parsed + layout knows about it, but redraw doesn't paint. |
| NEEDS_SELECTOR_ENGINE | Selector grammar present, dynamic match/invalidation incomplete. |
| NEEDS_AT_RULE | Parser tokenises the at-rule but cascade or matcher doesn't use it. |
| NEEDS_FRONTEND | NetSurf core supports it, MacSurf frontend (QuickDraw/Carbon) doesn't. |
| QUICKDRAW_FALLBACK | Implemented as the closest QuickDraw-feasible approximation. |
| INTENTIONALLY_UNSUPPORTED | Deliberately out of scope (aural, paged, web fonts, JS-driven). |

## Auto-inventory snapshot

- libcss parsers: **192**
- libcss public computed accessors: **131**
- accessors consumed somewhere in `browser/netsurf/`: **110**
- accessors with **no consumer**: **21** (the fast-lane target list)
- MacSurf vendor accessors: **11**
- at-rules recognised by parser: 6 (`@charset`, `@font-face`, `@import`, `@media`, `@namespace`, `@page`)
- pseudo classes/elements in parser: 30

## How to use this document

1. Find the feature module below.
2. Status column is the source of truth for "do we support X?"
3. The path column says how the next round closes the gap.
4. Each track listed in `CSS_IMPLEMENTATION_PLAN.md` references a row here.

---

## A. CSS 2.1 / 2.2

### A.1 Syntax, cascade, conditional

| Feature | Status | Path |
|---|---|---|
| At-rule `@charset` | FULL | parser strips, declared encoding honoured |
| At-rule `@import` | FULL | resolved through fetcher |
| At-rule `@media` (types: screen, all, print) | PARTIAL | screen/all evaluate; print queries don't gate layout because no print mode |
| At-rule `@namespace` | PARSED_NOT_CONSUMED | grammar accepts, selectors don't filter |
| At-rule `@font-face` | NEEDS_AT_RULE | parsed; no system-font registration. **Track E.** |
| At-rule `@page` | INTENTIONALLY_UNSUPPORTED | no paged output |
| `!important` | FULL | cascade respects |
| Custom properties (`var()`, `--foo`) | FULL | fixes133-139 native libcss |
| `inherit` / `initial` / `unset` | PARTIAL | inherit + initial honoured; `unset`/`revert` parsed, treated as initial |
| Cascade origin (UA / author) | FULL | resource:default.css is UA |
| Specificity | FULL | libcss native |

### A.2 Selectors (Level 3 baseline + practical L4 subset)

All pseudos below are **parsed**. Column indicates dynamic/match behaviour.

| Selector | Status | Notes |
|---|---|---|
| `*` universal | FULL | |
| Type / class / id | FULL | |
| Descendant / child / adjacent / general-sibling | FULL | |
| Attribute `[a]`, `[a=v]`, `[a~=v]`, `[a\|=v]` | FULL | |
| Attribute `[a^=v]`, `[a$=v]`, `[a*=v]` | FULL | |
| `:link`, `:visited` | PARTIAL | link styled; visited treated as link to dodge history-snoop |
| `:hover` | FULL | fixes130e fixed walk direction + node-data reuse |
| `:active` | PARTIAL | matches during mousedown; release invalidation can lag |
| `:focus` | PARTIAL | URL field only; no DOM focus model for `<a>`/`<input>` outside URL bar |
| `:first-child` / `:last-child` / `:only-child` | FULL | |
| `:first-of-type` / `:last-of-type` / `:only-of-type` | FULL | |
| `:nth-child(n)`, `:nth-of-type`, `:nth-last-*` | FULL | |
| `:not(...)` (single simple selector) | FULL | |
| `:not(...)` (selector list, L4) | NEEDS_SELECTOR_ENGINE | only single simple inside `not` works |
| `:empty` | FULL | |
| `:enabled` / `:disabled` / `:checked` | NEEDS_SELECTOR_ENGINE | parsed; matchers need form-state plumbing |
| `:root`, `:target`, `:lang(x)` | FULL | |
| `::before` / `::after` | FULL | fixes134a/d generated content + counters |
| `::first-line` | NEEDS_LAYOUT | parsed, not applied — needs inline-line snapshotting |
| `::first-letter` | NEEDS_LAYOUT | same |
| `:is()`, `:where()`, `:has()` | MISSING_PARSER | L4 only — not in this libcss vintage |
| `:focus-visible`, `:focus-within` | MISSING_PARSER | |
| `:placeholder-shown` | MISSING_PARSER | |

### A.3 Box model

| Property | Status |
|---|---|
| `width`, `height`, `min-width`, `min-height`, `max-width`, `max-height` | FULL |
| `padding-*` | FULL |
| `margin-*` (incl. auto, negative) | FULL |
| `border-*-width`, `border-*-style`, `border-*-color` | FULL |
| `border-radius` | FULL (fixes172 — QuickDraw `PaintRoundRect`/`FrameRoundRect`) |
| `box-sizing` | FULL |
| `outline` / `outline-color` / `outline-style` / `outline-width` | PARTIAL — drawn as a thin border outside box; offsets not honoured |
| `outline-offset` | MISSING_PARSER |

### A.4 Visual formatting / display

| Property | Status |
|---|---|
| `display: block / inline / inline-block / none / list-item` | FULL |
| `display: flex / inline-flex` | FULL |
| `display: grid / inline-grid` | PARTIAL — V2 columns/rows/spans/placement (fixes178); subgrid + `dense` deferred |
| `display: table / table-row / table-cell / table-row-group / table-caption` | PARTIAL |
| `display: contents` | NEEDS_LAYOUT |
| `display: flow-root` | NEEDS_LAYOUT — treated as block; doesn't establish BFC |
| `position: static / relative / absolute / fixed` | FULL |
| `position: sticky` | NEEDS_LAYOUT — falls back to relative |
| `top`/`right`/`bottom`/`left` | FULL |
| `float: left / right / none` | FULL |
| `clear` | FULL |
| `visibility: visible / hidden / collapse` | PARTIAL — collapse not honoured on table rows |
| `overflow-x` / `overflow-y` (`visible/hidden/scroll/auto/clip`) | PARTIAL — hidden clips; scroll has no per-element scrollbar UI |
| `z-index` + stacking contexts | FULL (fixes147 CSS 2.1 Appendix E painting order) |

### A.5 Floats, positioning

Covered above.

### A.6 Lists

| Property | Status |
|---|---|
| `list-style-type: disc / circle / square / decimal / none` | FULL |
| `list-style-type: lower-alpha / upper-alpha / lower-roman / upper-roman / decimal-leading-zero / lower-latin / upper-latin / lower-greek` | FULL (verified fixes202) — libcss `css_computed_format_list_style` already handles all of these; `layout__set_numerical_marker_text` invokes it for any non-disc/circle/square/none type. Non-ASCII glyph types (Greek, CJK, Armenian, Hebrew, etc.) round-trip to `?` through MacRoman; ASCII-result types render correctly. |
| `list-style-position: inside / outside` | PARTIAL — `inside` placement not honoured |
| `list-style-image` | PARTIAL — fetches but doesn't render image marker |

### A.7 Tables

| Property | Status |
|---|---|
| `border-collapse`, `border-spacing` | FULL |
| `empty-cells: show / hide` | FULL (fixes139a) |
| `caption-side` | PARSED_NOT_CONSUMED — no `BOX_TABLE_CAPTION` plumbing |
| `table-layout: auto` | FULL |
| `table-layout: fixed` | PARSED_NOT_CONSUMED — uses auto algorithm always. **Track F (tables).** |
| `colspan` / `rowspan` (HTML attrs) | FULL |
| Percent column widths | PARTIAL |

### A.8 Backgrounds & borders

| Property | Status |
|---|---|
| `background-color` | FULL |
| `background-image` (single bitmap) | FULL |
| `background-repeat` (`repeat/repeat-x/repeat-y/no-repeat`) | FULL (fixes138 tile loop) |
| `background-position` | FULL |
| `background-attachment: scroll / fixed` | FULL (fixes137) |
| `background-clip` / `background-origin` / `background-size` | MISSING_PARSER (`size` partially via shorthand) |
| Multiple backgrounds (`background: a, b`) | MISSING_PARSER — only first layer kept |
| Linear gradients (`linear-gradient`) | FULL via `-macsurf-gradient` and standard alias |
| Radial gradients | FULL (fixes74) — vendor-only `-macsurf-gradient` |
| Conic gradients | MISSING_PARSER |

### A.9 Colors

| Feature | Status |
|---|---|
| Named colours, `#rgb`, `#rrggbb` | FULL |
| `rgb()` / `rgba()` legacy | FULL |
| `hsl()` / `hsla()` legacy | FULL |
| Modern slash `rgb(r g b / a)` / `hsl(h s l / a)` | PARTIAL — accepted, alpha clamped to nearest QuickDraw stipple |
| `hwb()`, `lab()`, `lch()`, `oklab()`, `oklch()`, `color()` | MISSING_PARSER |
| `currentColor` | FULL |
| `transparent` | FULL |

### A.10 Fonts & text

| Property | Status |
|---|---|
| `font-family` (system fonts + generic aliases sans/serif/mono) | FULL (fixes157) |
| `font-size` (px, em, rem, %, pt, smaller/larger, keywords) | FULL |
| `font-style: italic / oblique` | FULL — both routed to italic face |
| `font-weight: 100..900 / bold / normal` | PARTIAL — collapsed to bold/regular |
| `font-variant: small-caps` | PARTIAL — small-caps fakes by uppercasing at smaller size |
| `font-stretch` | MISSING_PARSER |
| `line-height` | FULL |
| `letter-spacing` | FULL |
| `word-spacing` | FULL (fixes139b) |
| `text-align: left/right/center/justify` | FULL |
| `text-align: start/end/match-parent` | PARTIAL — start/end map to LTR equivalents |
| `text-indent` | FULL |
| `text-transform: uppercase/lowercase/capitalize/none` | FULL |
| `text-decoration: underline/overline/line-through` | FULL |
| `text-decoration-color` | MISSING_PARSER (re-classified fixes202) — libcss doesn't parse the longhand. Decoration uses `color`. Deferred behind a new-property round. |
| `text-decoration-style` (`solid/dotted/dashed/wavy/double`) | MISSING_PARSER — only solid |
| `text-decoration-thickness` | MISSING_PARSER |
| `text-shadow` (standard) | FULL (fixes175 preprocessor → `-macsurf-text-shadow`) |
| `text-shadow` with blur radius | PARTIAL — blur silently dropped to 0 |
| `text-overflow: ellipsis` | FULL (fixes135a/c) |
| `white-space: normal/pre/nowrap/pre-wrap/pre-line` | FULL |
| `word-break: normal/break-all/keep-all` | PARTIAL (fixes202) — `break-all` triggers char-level break when no whitespace break fits; `keep-all` not differentiated from `normal` (CJK detection unavailable on MacRoman) |
| `overflow-wrap` / `word-wrap: normal/break-word/anywhere` | FULL (fixes202) — `break-word` and `anywhere` both invoke char-level fallback when whitespace split returns 0 |
| `hyphens` | MISSING_PARSER |
| `direction: ltr / rtl` | PARTIAL — `rtl` text reads LTR (no bidi) |
| `unicode-bidi` | PARSED_NOT_CONSUMED |
| `writing-mode` | PARSED_NOT_CONSUMED |

### A.11 Visual effects

| Property | Status |
|---|---|
| `opacity` | PARTIAL — QuickDraw stipple at 1.0/0.80/0.50/0.25 |
| `box-shadow` | FULL (fixes175/178 — independent h/v + colour, no blur) |
| `box-shadow` blur radius | QUICKDRAW_FALLBACK — silently 0 |
| `box-shadow: inset` | NEEDS_REDRAW |
| `clip` (legacy) / `clip-path` | MISSING_PARSER (clip-path); legacy `clip:` PARTIAL |
| `filter`, `backdrop-filter`, `mix-blend-mode` | INTENTIONALLY_UNSUPPORTED (graceful drop) |
| `mask`, `mask-image` | INTENTIONALLY_UNSUPPORTED |

### A.12 UI / interaction

| Property | Status |
|---|---|
| `cursor` | FULL — set_pointer in window.c (fixes131) |
| `pointer-events: auto/none` | MISSING_PARSER (re-classified fixes202) — no libcss parser, no public accessor. Would need a new property, which is the trap zone; deferred until a Track-H sized round. |
| `user-select` | MISSING_PARSER — selection model is platform-defined |
| `caret-color` | MISSING_PARSER |
| `appearance` | MISSING_PARSER |
| `resize` | MISSING_PARSER |

### A.13 Generated content & counters

| Feature | Status |
|---|---|
| `content:` string | FULL |
| `content: counter(name [, style])` | FULL — decimal only |
| `content: counters(name, sep)` plural | NEEDS_LAYOUT |
| `content: open-quote / close-quote / no-open-quote / no-close-quote` | FULL (fixes140) |
| `content: attr(x)` | PARTIAL — attr() returns value, no fallback/type-coercion |
| `content: url(...)` | NEEDS_REDRAW |
| `counter-increment`, `counter-reset` | FULL (fixes134d) |
| `quotes` | FULL (fixes140) |

### A.14 Paged media (CSS 2.1 chapter 13)

| Property | Status |
|---|---|
| `page-break-*`, `break-*`, `orphans`, `widows` | QUICKDRAW_FALLBACK (fixes202) — parsed and silently dropped. Reason: no paged-output path on a screen-only browser; only relevant when print or multicol pagination ships. Track E will read `break-*` for multicol. |
| `@page` | INTENTIONALLY_UNSUPPORTED |

### A.15 Aural CSS

INTENTIONALLY_UNSUPPORTED — visual browser only.

---

## B. CSS3+ modules

### B.1 Flexible Box Layout (Level 1)

| Feature | Status |
|---|---|
| `display: flex / inline-flex` | FULL |
| `flex-direction`, `flex-wrap`, `flex-flow` | FULL |
| `justify-content` (all 6 values) | FULL (fixes41) |
| `align-content` (all 6 values) | FULL |
| `align-items`, `align-self` | FULL |
| `flex-basis`, `flex-grow`, `flex-shrink`, `flex` shorthand | FULL |
| Intrinsic main-size resolution (auto basis on indefinite containers) | FULL (fixes176) |
| Intrinsic main-size in column direction | PARTIAL — needs concrete height to resolve |
| `gap` / `row-gap` / `column-gap` on flex | PARTIAL (fixes148) — single value; two-value `gap: A B` drops A |
| `order` | FULL |

### B.2 Grid Layout (Level 1)

| Feature | Status |
|---|---|
| `display: grid` | FULL |
| `grid-template-columns` (px, %, fr, repeat, minmax, fit-content) | PARTIAL — V2 (fixes148, fixes148b3) |
| `grid-template-rows` | PARTIAL — V1 (fixes150) — PX honoured, FR falls back |
| `grid-template-areas` | PARTIAL (fixes178b) — document-scope name table, 32 names max, 8x8 grid max, named `grid-area: <ident>` lookup; cross-stylesheet references not resolved |
| `grid-column` span/start/end | FULL (fixes151) |
| `grid-row` span/start/end | FULL (fixes158, probe added fixes178a) |
| `grid-area` shorthand | PARTIAL (fixes178c) — 4-value numeric `rs/cs/re/ce` and 2-value `rs/cs` work; named-area form depends on fixes178b lookup |
| `grid-auto-rows` / `-columns` / `-flow` | MISSING_PARSER |
| `align-items` / `align-self` on grid | FULL (fixes178d) — stretch (default), flex-start, flex-end, center; baseline -> flex-start |
| `justify-items` / `justify-self` on grid | MISSING_PARSER — libcss vintage has no parser/accessor; new vendor property needed |
| `gap` two-value on grid | PARTIAL — same as flex |
| Subgrid | INTENTIONALLY_UNSUPPORTED (this libcss vintage) |

### B.3 Multi-column

| Feature | Status |
|---|---|
| `column-count`, `column-width`, `columns` shorthand | PARSED_NOT_CONSUMED. **Track D.** |
| `column-gap` | FULL (shared with flex/grid) |
| `column-rule-*` | PARSED_NOT_CONSUMED. **Track D.** |
| `column-span` | PARSED_NOT_CONSUMED |
| `column-fill` | PARSED_NOT_CONSUMED |
| `break-before/after/inside` (and legacy `page-break-*`) | PARSED_NOT_CONSUMED — relevant only in multicol/print |

### B.4 Box Alignment (Level 3)

Mostly aliased into flex/grid above. `place-content`, `place-items`,
`place-self` shorthands: MISSING_PARSER.

### B.5 Transforms

| Feature | Status |
|---|---|
| `transform: rotate / translate / scale (X/Y) / matrix` | PARTIAL — via vendor `-macsurf-transform` (fixes73), standard alias attempted in fixes141 hung the engine (reverted; documented in CLAUDE.md) |
| `transform-origin` | PARTIAL — box centre fixed |
| 3D transforms (`rotateX/Y/Z`, `perspective`, `matrix3d`) | INTENTIONALLY_UNSUPPORTED — QuickDraw 2D only |
| `backface-visibility` | INTENTIONALLY_UNSUPPORTED |
| `will-change` | PARSED_NOT_CONSUMED — safe no-op |

### B.6 Transitions / Animations

| Feature | Status |
|---|---|
| `transition`, `transition-*` | MISSING_PARSER. **Track H.** |
| `animation`, `animation-*`, `@keyframes` | PARTIAL — vendor `-macsurf-animation-*` opacity/rotate exists. **Track H.** |

### B.7 Backgrounds & Borders Level 3

| Feature | Status |
|---|---|
| `border-image-*` | MISSING_PARSER |
| `background-size` | PARTIAL — `cover`/`contain` not in layout |
| `box-shadow` (covered above) | FULL (no blur) |
| `border-radius` (covered above) | FULL |

### B.8 Values & Units 3/4

| Feature | Status |
|---|---|
| `px / em / rem / ex / ch / pt / in / cm / mm / pc` | FULL |
| `vw / vh / vmin / vmax` | FULL (fixes132 vh/vw swap fix; PROBE V1-V4) |
| `svw / lvw / dvw / svh / lvh / dvh` | MISSING_PARSER (L4) |
| `%` | FULL |
| `calc()` | FULL |
| `min()`, `max()`, `clamp()` | PARTIAL — accepted only inside calc-equivalent positions |
| `var(--x [, fallback])` | FULL |
| `env()` | MISSING_PARSER — safe fallback if added |
| `attr()` (typed L4) | PARTIAL — string-only |

### B.9 Color Level 3/4

Covered in A.9. Level 4 syntaxes are MISSING_PARSER.

### B.10 Fonts Level 3/4

| Feature | Status |
|---|---|
| `@font-face` local() | NEEDS_AT_RULE |
| `@font-face url()` (web fonts) | INTENTIONALLY_UNSUPPORTED — no TTF/OTF runtime install |
| `font-display`, `font-feature-settings`, `font-variation-settings` | MISSING_PARSER |
| Variable fonts | INTENTIONALLY_UNSUPPORTED |

### B.11 Images / replaced

| Feature | Status |
|---|---|
| PNG/GIF/JPEG/BMP/TIFF decode | FULL (fixes78/79b) — PNG has real per-pixel mask |
| WebP / AVIF / HEIC | INTENTIONALLY_UNSUPPORTED |
| `object-fit` (fill/contain/cover/none/scale-down) | FULL (fixes116) |
| `object-position` | MISSING_PARSER |
| `image-set()` | MISSING_PARSER |
| Lazy / responsive `srcset`, `sizes` | NEEDS_FRONTEND — fetcher picks first |

### B.12 UI Level 3/4

| `appearance`, `accent-color`, `caret-color` | MISSING_PARSER |
| `outline-offset` | MISSING_PARSER |
| `resize` | MISSING_PARSER |
| `scroll-behavior` | INTENTIONALLY_UNSUPPORTED (sync scroll only) |

### B.13 Overflow Level 3

| `overflow-x/y`, `overflow` | PARTIAL (covered above) |
| `overflow-clip-margin` | MISSING_PARSER |
| `text-overflow` | FULL |
| `scrollbar-*` | INTENTIONALLY_UNSUPPORTED (platform scrollbar only) |

### B.14 Containment

| `contain` | MISSING_PARSER — would be safe no-op |
| `content-visibility` | MISSING_PARSER |

### B.15 Media Queries / Conditional

| `@media` (`min/max-width`, `min/max-height`, orientation) | PARTIAL — width/height honoured; `prefers-*` not |
| `@supports (property: value)` | PARTIAL — answers by parse-success only, not by consumption |
| `@container` | MISSING_PARSER (queries) / NEEDS_LAYOUT (style queries) |
| `@layer` | MISSING_PARSER |

### B.16 Masking / Filters / Compositing

INTENTIONALLY_UNSUPPORTED across the module — declared above.

### B.17 SVG presentation properties

| `fill`, `stroke`, `fill-opacity`, `stroke-opacity` | INTENTIONALLY_UNSUPPORTED (fixes202) until SVG content handler exists — parsed by libcss for forward-compat but no SVG renderer to consume them |
| Full SVG rendering | NEEDS_FRONTEND — depends on SVG content handler |

---

## C. MacSurf vendor properties

All FULL by definition (we own them). Listed here so they show up
when greps look for parsed/consumed coverage.

| Property | Purpose | Replaces standard |
|---|---|---|
| `-macsurf-gradient` | linear+radial gradients | `linear-gradient`/`radial-gradient` in background-image |
| `-macsurf-text-shadow` | text-shadow with QuickDraw offsets | `text-shadow` (preprocessor bridge in cssh_css.c) |
| `-macsurf-transform` + `-macsurf-transform-b` | rotate/translate/scale | `transform` (standard alias attempt failed fixes141) |
| `-macsurf-grid` | N-column equal-width grid lite | `grid-template-columns: repeat(N, 1fr)` |
| `-macsurf-grid-rows` | px-row tracks | `grid-template-rows` (PX subset) |
| `-macsurf-grid-col-span` | grid-column placement | `grid-column` span/A/-1 forms |
| `-macsurf-animation-opacity` | opacity tween | partial `animation` |
| `-macsurf-animation-rotate` | rotate tween | partial `animation` |

---

## D. Quick-reference: parsed-not-consumed (the fast lane)

From `tools/audit/parsed_not_consumed.txt` (21 entries):

```
break-after / break-before / break-inside
column-count / column-fill / column-rule-* / column-span / column-width
fill-opacity / stroke-opacity
orphans / widows
overflow-wrap / word-break
page-break-after / page-break-before / page-break-inside
table-layout
unicode-bidi / writing-mode
macsurf-grid-col-span (already shipped; spurious — no public consumer needed)
```

The implementation plan groups these into **Track A (fast-lane pack)**
and **Track D (multi-column)**.

---

## E. Things NOT here yet that the implementation plan must add

- Print/paged: declared INTENTIONALLY_UNSUPPORTED at the at-rule layer.
- JS-driven CSSOM (`document.styleSheets[i].insertRule`, computed style
  from JS): out of scope for fixes200; Duktape integration is a
  separate track.
- Houdini / Paint API: INTENTIONALLY_UNSUPPORTED.
- View Transitions, scroll-driven animations, anchor positioning:
  MISSING_PARSER; all deferred behind Track H.

## F. Maintenance

When a feature ships:

1. Update the matching row above (status, link to fix number).
2. Run `tools/css_audit.sh` and commit the refreshed `tools/audit/*`.
3. Add a probe to `tests/css/<feature>.html`.
4. Cross-link from `CSS_IMPLEMENTATION_PLAN.md`.

If a row's status doesn't have a probe, the feature is not "shipped"
no matter what the commit message says.
