# MacSurf CSS Capability Audit

**Generated:** 2026-05-18 (post fixes129)
**Purpose:** Canonical reference for what MacSurf's CSS engine actually does and doesn't render. Updated as features land or get re-verified.

## Verdict legend

- **✓ Works**, parsed AND read by layout/redraw AND has visible effect
- **~ Inert**, parsed by libcss but no `css_computed_*` accessor in layout/redraw (or accessor exists but result discarded)
- **✗ Absent**, no parser entry or selector handling

---

## Selectors (CSS Selectors L3/L4)

| Feature | Verdict | Evidence |
|---|---|---|
| Type / class / ID | ✓ | css/select.c:35-52 |
| Attribute selectors (`[a=b]`, `~=`, `\|=`, `^=`, `$=`, `*=`) | ✓ | libcss/include/libcss/select.h:81-100 |
| Combinators (descendant / child `>` / adjacent `+` / general sibling `~`) | ✓ | css/select.c:39-44 |
| `:root`, `:empty`, `:link` | ✓ | css/select.c:73,76,77 |
| `:first-child`, `:last-child`, `:nth-child(n)` | ✓ | libcss/src/select/css_select.c:2700+ |
| `:visited` | ~ Inert | css/select.c:1520, handler always returns false |
| **`:hover`** | ~ Inert | css/select.c:1540, handler always returns false |
| **`:focus`** | ~ Inert | css/select.c:1578, handler always returns false |
| **`:active`** | ~ Inert | css/select.c:1559, handler always returns false |
| `:checked` | ~ Inert | css/select.c:1635, handler always returns false |
| `:disabled` / `:enabled` | ~ Inert | css/select.c:1597, 1616 |
| `:target`, `:lang()` | ~ Inert | css/select.c:1654, 85 |
| `:not()`, `:is()`, `:where()`, `:focus-visible`, `:focus-within`, `:placeholder-shown` | ✗ | Not in pseudo-class list |
| `::before`, `::after`, `::first-line`, `::first-letter` | ✓ (defined) | libcss/include/libcss/select.h:25-28, but content generation needs check |
| `::placeholder`, `::selection`, `::marker` | ✗ | Not in enum |

## Box model

| Feature | Verdict | Evidence |
|---|---|---|
| `width`, `height`, `max-width`, `max-height`, `min-width` | ✓ | layout.c:450, 697, 896, 1055, 1066 |
| `min-height` | ~ Inert | parser exists; not read in layout |
| `margin` / `padding` (all sides + auto) | ✓ | layout.c:79-90 |
| `border-width` / `style` / `color` / `radius` | ✓ | layout.c:95-114; redraw.c:2331 (radius) |
| `box-sizing` | ✓ | layout.c:670, 898 |

## Backgrounds & borders

| Feature | Verdict | Evidence |
|---|---|---|
| `background-color` | ✓ | redraw.c:382, 1059 |
| `background-image: linear-gradient(...)` (standard) | ✓ (post fixes129) | bridges to MACSURF_GRADIENT slot |
| `background-image: url(...)` | ~ Inert | parser exists but redraw doesn't render |
| `background-image: radial-gradient(...)` (standard) | ✓ (post fixes129) | same bridge as linear |
| `background-position` | ✓ | redraw.c:1211 |
| `background-repeat` | ✓ | redraw.c:1211 |
| `background-size`, `background-attachment`, `background-clip`, `background-origin` | ~ Inert | parsers exist; not read |
| `box-shadow` (outer, inset, multiple) | ✓ | redraw.c:1024, 1434 |
| `border-image` | ~ Inert | parser only |

## Display & positioning

| Feature | Verdict | Evidence |
|---|---|---|
| `display: block / inline / inline-block / flex / inline-flex / grid / inline-grid / table* / list-item / none` | ✓ | layout.c:883-888 |
| `display: contents` | ✗ | No BOX_CONTENTS |
| `position: static / relative / absolute / fixed` | ✓ | layout.c:1001, 1158, 1160, but **fixed degrades to absolute** (no viewport anchoring) |
| `position: sticky` | ✗ | No CSS_POSITION_STICKY |
| `top` / `right` / `bottom` / `left` | ✓ | accessors used |
| `z-index` | ~ Inert | parser exists; dump.c:1822 only; no stacking context |
| `float`, `clear` | ✓ | layout.c:1118-1119, 236 |
| `overflow`, `overflow-x`, `overflow-y` | ✓ | layout.c:1187, 1200; redraw.c:1871 |
| `visibility` | ✓ | redraw.c:2378 |

## Flexbox

All major features implemented (layout_flex.c is a dedicated engine):

| Feature | Verdict |
|---|---|
| `display: flex` / `inline-flex` | ✓ |
| `flex-direction`, `flex-wrap`, `flex-flow` | ✓ |
| `flex-grow`, `flex-shrink`, `flex-basis`, `flex` shorthand | ✓ |
| `justify-content`, `align-items`, `align-self`, `align-content` | ✓ |
| `order` | ✓ |
| `gap`, `row-gap`, `column-gap` | ✓ (single-value; two-value `gap: A B` collapses) |

## Grid

| Feature | Verdict | Evidence |
|---|---|---|
| `display: grid` / `inline-grid` | ✓ | layout_grid.c |
| `-macsurf-grid: N` (lite syntax) | ✓ | layout_grid.c:194 |
| `-macsurf-grid-tracks` | ✓ | layout_grid.c:235 |
| **`grid-template-columns` (standard, `repeat()`, `fr`, `minmax`)** | ~ Inert | parser exists; not consumed |
| `grid-template-rows` (standard) | ~ Inert | same |
| `grid-column`, `grid-row`, `grid-area` | ~ Inert | not consumed |
| `grid-auto-flow`, `grid-auto-columns`, `grid-auto-rows` | ✗ | no auto track handling |
| `place-items`, `place-content` | ✗ | no alignment |

## Typography

| Feature | Verdict | Evidence |
|---|---|---|
| `font-size`, `line-height` | ✓ | layout.c:5338, 2679 |
| **`font-family`** | ~ Inert | parser exists; layout doesn't read for glyph selection |
| **`font-weight`** | ~ Inert | parser exists; not read |
| **`font-style`** | ~ Inert | parser exists; not read |
| `font-variant` | ~ Inert | parser exists; not read |
| `text-align`, `text-indent` | ✓ | layout.c:1421, 241 |
| `text-decoration` | ✓ | redraw.c:1867 |
| **`text-transform`** | ~ Inert | parser exists; not applied |
| **`letter-spacing`, `word-spacing`** | ~ Inert | parsers exist; not used in text rendering |
| `white-space` | ✓ | layout.c:488 |
| `word-break`, `overflow-wrap`, `hyphens` | ~ Inert | parsers exist; not read |
| `-macsurf-text-shadow` | ~ Inert | parser exists; not read in redraw |
| `@font-face`, `font-display` | ~ Inert | parsed but no font loading |

## Colors & values

| Feature | Verdict | Evidence |
|---|---|---|
| Named colors, `#rgb`, `#rrggbb`, `#rgba`, `#rrggbbaa`, `rgb()`, `rgba()`, `hsl()`, `hsla()` | ✓ | libcss color parsing |
| `hwb()` | ~ Inert | parsing likely; not all paths verified |
| `lab()`, `lch()`, `color()` | ✗ | modern color spaces absent |
| `currentColor` | ~ Inert | recognized but not specially propagated |
| **`calc()`** | ✓ | css__parse_calc in dimension parsers |
| `min()`, `max()`, `clamp()` | ✗ | no comparison functions |
| **CSS custom properties (`--name`)** | ✓ | fixes133-139, verified in libcss/src/parse/custom_properties.h |
| **`var(--name, fallback)`** | ✓ | fixes139 lexer keystone + resolution in cascade |
| Units: px, em, rem, %, pt, in, cm, mm | ✓ | core units |
| `ch`, `ex` | ~ Inert | estimated as em equivalents |
| `vw`, `vh`, `vmin`, `vmax` | ✗ | no viewport-relative unit math |

## Transforms

| Feature | Verdict | Evidence |
|---|---|---|
| `-macsurf-transform: translate() / rotate() / scale()` | ✓ | redraw.c:1394; sin/cos LUT (fixes73) |
| `transform: ...` (**standard CSS syntax**) | ✗ | only vendor prefix recognized |
| `transform-origin` | ✗ | not parsed |
| 3D transforms | ✗ | 2D only |

## Transitions & animations

| Feature | Verdict | Evidence |
|---|---|---|
| `-macsurf-animation-opacity` | ✓ | custom one-shot |
| `-macsurf-animation-rotate` | ✓ | custom one-shot |
| **`transition-*` (standard)** | ✗ | not parsed |
| **`@keyframes`, `animation-*` (standard)** | ✗ | not parsed |

## Filters & effects

| Feature | Verdict | Evidence |
|---|---|---|
| `opacity` | ✓ | redraw.c:1395 |
| `filter: blur / brightness / contrast / drop-shadow / grayscale / hue-rotate / invert / opacity / saturate / sepia` | ✗ | no filter functions |
| `backdrop-filter` | ✗ | no backdrop |
| `mix-blend-mode`, `background-blend-mode` | ✗ | no blend modes |

## Media queries

| Feature | Verdict | Evidence |
|---|---|---|
| `@media screen / all` | ✓ | libcss parses, NetSurf applies |
| `@media (max-width: ...)` / `(min-width: ...)` / height variants | ~ Inert | parsed but **not evaluated against actual viewport** (post-fixes124 viewport IS honest, so eval just needs wiring) |
| `@media print` | ~ Inert | parsed; screen-only |
| `orientation`, `prefers-color-scheme`, `prefers-reduced-motion`, `hover`, `pointer` | ✗ | modern media features absent |

## Other

| Feature | Verdict | Evidence |
|---|---|---|
| `object-fit` | ✓ | redraw.c:272 (fixes116) |
| `object-position` | ~ Inert | parser exists; not read |
| `aspect-ratio` (native CSS3) | ~ Inert | parser likely; not read |
| `clip-path` | ✗ | not parsed |
| `clip` (legacy) | ✓ | redraw.c:2132 |
| `mask` | ✗ | not parsed |
| `will-change` | ~ Inert | optimization hint, OK to ignore |
| `scroll-behavior` | ~ Inert | parsed; no smooth scroll |
| `scroll-snap-*` | ~ Inert | parsed; not implemented |
| `caret-color`, `accent-color` | ~ Inert | parsed; form widgets don't honor |
| `pointer-events` | ~ Inert | parsed; event model doesn't check |
| `user-select` | ~ Inert | selection always allowed |
| **`cursor`** | ~ Inert | parser exists; frontend doesn't call SetCursor on hover |
| `outline`, `outline-offset` | ✓ partial | redraw.c:2384-2403 (outline) |
| `vertical-align`, `direction` | ✓ | layout-side |
| `unicode-bidi` | ~ Inert | always LTR |
| `writing-mode` | ✗ | no vertical/RTL |

---

## Headline numbers

- **✓ Fully works:** ~68 properties / features
- **~ Parsed but inert:** ~32 properties (these are the cheap wins, parser already done)
- **✗ Absent entirely:** ~45 features (most are CSS3 polish or modern compositor-dependent)

## The "inert" list, what's already half-built

These have parsers but no consumer. Implementing each is mostly "add the `css_computed_*` read at the right spot in layout or redraw":

1. **`:hover` / `:focus` / `:active` / `:checked` / `:disabled`**, all share the same fix (dynamic re-cascade infrastructure)
2. **`cursor`**, `SetCursor()` call on hover-update
3. **`font-family`, `font-weight`, `font-style`**, wire to QuickDraw font selection
4. **`text-transform`, `letter-spacing`, `word-spacing`, `word-break`**, text-pass adjustments
5. **`min-height`, `background-size`, `aspect-ratio`, `object-position`**, layout/paint reads
6. **`@media (max-width)` evaluation**, wire viewport into media-query matcher
7. **`z-index`**, paint order reorganization
8. **`-macsurf-text-shadow`**, wire to text redraw
9. **`background-image: url(...)`**, load + paint pipeline
10. **`pointer-events`**, hit-test guard

## What we can't reasonably build on OS 9

- `filter: blur`, framebuffer convolution too slow on G3
- 3D transforms, no perspective compositor
- GPU-class compositing (`will-change` actually working, `transform: translateZ()`)
- Container queries, heavy, not common
- `lab()`/`lch()`/`color()`, modern color spaces, niche usage
- Smooth animations of layout-affecting properties (cooperative scheduler can't sustain 60fps relayout)
