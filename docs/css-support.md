# MacSurf CSS Support

**What CSS MacSurf renders on Mac OS 9, tracked against the CSS3 feature list at
[w3schools.com/cssref/css3_browsersupport.php](https://www.w3schools.com/cssref/css3_browsersupport.php).**

MacSurf is a NetSurf-based browser doing full native CSS on PowerPC Mac OS 9 —
libcss parse + cascade, our own layout, and QuickDraw painting. This page is the
user-facing summary; the deep engineering audit lives in
[.private/research/css-gap-inventory-2026-08-14.md](../.private/research/css-gap-inventory-2026-08-14.md).

*Last updated: development snapshot after fixes1216 (2026-08-19).*

### Legend

| Mark | Meaning |
|:---:|---|
| ✅ | **Full** — rendered correctly on hardware |
| ◑ | **Partial** — works with documented limits (see notes) |
| ▫ | **Parsed, not yet painted** — accepted but no visual effect yet |
| ✖ | **Not yet** — deferred or out of scope |
| **&ast;** | **New in MacSurf** — implemented by us, **not present in the upstream NetSurf engine** we forked. In several cases this is a *first for any classic Mac OS browser.* |

> The **&ast;** marks are the ones to show off: they're original engine work, not
> inherited. Everything else is MacSurf carrying NetSurf's CSS forward onto OS 9.

---

## Borders

| Property | Support | Notes |
|---|:---:|---|
| `border-radius` | ✅ | Rounded corners. |
| `box-shadow` | ✅ | |
| `border-image` | ✖ | Deferred (#81). |

## Backgrounds

| Property | Support | Notes |
|---|:---:|---|
| `background-color` (incl. **rgba**) | ✅&ast; | rgba now composites against the real backdrop, not white (2.0.5, #227). |
| `background-image` | ✅ | Raster + gradients; external SVG backgrounds ◑ (#280). |
| `background-size` | ◑ | Honored for raster images; not yet for background SVG. |
| `background-position` / `-repeat` | ✅ | |
| `background: none` / `transparent` reset | ✅ | Shorthand reset fixed 2.0.5 (#268). |
| `background-clip` | ✅&ast; | `border-box`/`padding-box`/`content-box` honored (2.0.5, #255); `text` clips gradient and bitmap backgrounds to glyphs, hardware-confirmed on G3 iMac (fixes1218–1223). |
| `background-origin` | ◑&ast; | Longhand `border-box` / `padding-box` / `content-box` positions raster backgrounds independently of clipping; hardware-confirmed on G3 iMac (fixes1215/1216). Shorthand box keywords are deferred. |
| `background-blend-mode` | ✅&ast; | All 16 standard keywords parse and cascade. The supported background layer blends with `background-color` for gradients and raster images; `multiply` hardware-confirmed for both on G3 iMac (fixes1224/1225, #255). |
| `background-attachment: fixed` | ✅&ast; | Raster images and gradients stay viewport-anchored while their box scrolls. |

## Gradients

| Feature | Support | Notes |
|---|:---:|---|
| `linear-gradient` | ✅ | |
| `radial-gradient` | ✅ | |
| `conic-gradient` | ✖ | |

## Text effects & typography

| Property | Support | Notes |
|---|:---:|---|
| `text-align: justify` | ✅&ast; | Real word-spreading to both margins (2.0.5, #271). **First in the NetSurf family.** |
| `text-align-last` | ✅&ast; | (2.0.5, #251) |
| `text-justify` | ✅&ast; | inter-word (2.0.5, #251) |
| `hyphens: manual` (`&shy;`) | ✅&ast; | Soft-hyphen line breaking with a trailing "-" (2.0.5, #251/#272/#275). |
| `hyphens: auto` | ✖ | Needs a hyphenation dictionary; deferred. |
| `tab-size` | ✅&ast; | Tabs in `<pre>` (2.0.5, #251). |
| `word-break` / `overflow-wrap` | ✅&ast; | break-all / break-word / anywhere (#273/#234). |
| `text-shadow` | ✅ | |
| `text-overflow: ellipsis` | ✅&ast; | Paints an ellipsis over clipped single-line overflow. |
| `text-decoration` longhands | ✅&ast; | color / style / thickness (2.0.5, #249). |
| `caret-color` | ✅&ast; | In-page text caret (2.0.5, #252). |
| `word-spacing` | ✅&ast; | Applies to inline text, including justified lines. |
| `writing-mode` / `unicode-bidi` | ✖ | Horizontal LTR layout only; vertical writing and full bidi are deferred (#248). |

## Fonts

| Feature | Support | Notes |
|---|:---:|---|
| `font-family` matching | ◑ | Maps to available OS 9 faces (Geneva/Monaco/Chicago/Charcoal + fallbacks). |
| `font-weight` | ◑ | QuickDraw is bold/regular — numeric weights collapse to those two. |
| `font-style`, `font-size`, `line-height` | ✅ | |
| Web fonts (`@font-face`) | ◑&ast; | Downloadable icon-glyph fonts render (sfnt cmap + QuickDraw fill); full Google-Fonts text faces still fast-failed (#77). |

## 2D / 3D Transforms

| Feature | Support | Notes |
|---|:---:|---|
| 2D `transform` (translate/scale/rotate/matrix) | ✅ | |
| 3D transforms | ✖ | |
| Individual `translate`/`rotate`/`scale` props | ✖ | Deferred (#254). |

## Transitions & Animations

| Feature | Support | Notes |
|---|:---:|---|
| `transition` | ◑ | Temporal presentation is hardware-verified for `opacity` only (QuickDraw stipple approximation); other properties currently degrade to the end state. |
| `animation` / `@keyframes` | ✖ | Same — final state, no motion. |

## Layout — Flexbox

| Feature | Support | Notes |
|---|:---:|---|
| `display: flex`, direction, wrap | ✅ | |
| `justify-content` (main axis) | ✅ | |
| `align-items` / `align-self` (cross axis) | ✅&ast; | center / flex-start / flex-end / stretch (2.0.5, #270). |
| `flex-grow` / `-shrink` / `-basis` | ✅ | Item content-sizing + no-overflow past the container (#278). |
| `place-content` / `place-items` / `place-self` | ✅&ast; | Box-alignment shorthands (2.0.5, #253). |

## Layout — Grid

| Feature | Support | Notes |
|---|:---:|---|
| `display: grid`, `grid-template-columns/rows` | ✅ | Fixed, %, and fr tracks. |
| `auto` tracks sized to content | ✅&ast; | Spec-order content sizing (2.0.5, #62). |
| `gap` / `row-gap` / `column-gap` | ✅&ast; | Independent row and column gaps; two-value `gap: A B` is honored. |
| Placement / span / `grid-auto-flow` | ◑&ast; | Numeric placement, spans, areas, and row/column/dense flow work; named lines and negative lines other than `-1` are deferred. |
| `justify-self` | ✅&ast; | start / center / stretch per grid item (2.0.5 follow-up, #279). |
| `subgrid` | ✖ | (#66) |

## Multiple Columns

| Feature | Support | Notes |
|---|:---:|---|
| `column-count` / `column-width` / `column-gap` | ✅ | Multicol V1. |
| `column-span: all` | ✅&ast; | Spanning children break out across all columns. |

## User Interface & Box Model

| Feature | Support | Notes |
|---|:---:|---|
| `box-sizing` | ✅ | |
| `resize` / `user-select` | ✖ | (#257) |
| `appearance: none` | ✅&ast; | Suppresses the native widget so CSS can paint the control (#80). |
| `outline` | ✅&ast; | CSS outline color, width, and style paint outside the border box. Native focus-ring policy remains separate. |
| `image-rendering` | ▫&ast; | Parsed/stored; QuickDraw scaling is nearest-neighbor regardless (2.0.5, #256). |

## Modern building blocks

| Feature | Support | Notes |
|---|:---:|---|
| **CSS Custom Properties** (`var()`, `--foo`) | ✅&ast; | Native `var()` resolution at cascade time — the feature that unblocked modern themes (Drupal/XenForo). **First in the NetSurf family.** |
| **CSS Logical Properties** | ✅&ast; | `margin/padding/border-block\|inline`, `inset-*`, logical sizing (2.0.5, #247). |
| `:root`, attribute & structural selectors | ✅ | |
| `:is()` / `:where()` / `:has()` | ◑ | Recognised but not semantically matched yet (#163). |
| `@media` queries | ✅ | |
| `@container` queries | ✖ | (#75) |
| `clip-path` / `mask` / `filter` | ✖ | Degrade to flat rendering. |
| `mix-blend-mode` / `isolation` | ✖ | (#83) |

---

## What the &ast; adds up to

Everything marked **&ast;** is engine work original to MacSurf — it is not in the
upstream NetSurf codebase we forked, so no other NetSurf frontend renders it.
The typography cluster (justification, `text-align-last`, `text-justify`,
`hyphens`, `tab-size`), the logical-property translation, `caret-color`,
`text-decoration` longhands, grid auto-track sizing, and native `var()` are all
new this way. Justified text and `var()` in particular are, as far as we can
find, firsts for **any** browser running on Classic Mac OS.

Gaps above aren't dead ends — most are tracked issues on the road-to-100%
meta ([#267](https://github.com/mplsllc/macsurf/issues/267)). Animations,
filters, and RTL/vertical text are the honest current frontier.
