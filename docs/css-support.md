# MacSurf CSS Support

**What CSS MacSurf renders on Mac OS 9, tracked against the CSS3 feature list at
[w3schools.com/cssref/css3_browsersupport.php](https://www.w3schools.com/cssref/css3_browsersupport.php).**

MacSurf is a NetSurf-based browser doing full native CSS on PowerPC Mac OS 9 —
libcss parse + cascade, our own layout, and QuickDraw painting. This page is the
user-facing summary; the deep engineering audit lives in
[docs/research/css-gap-inventory-2026-07-13.md](research/css-gap-inventory-2026-07-13.md).

*Last updated: MacSurf 2.0.5 (2026-07-15).*

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
| `background-clip` / `background-origin` | ✖ | Deferred (#255). |
| `background-attachment: fixed` | ✖ | |

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
| `text-overflow: ellipsis` | ✖ | Clips today; ellipsis deferred. |
| `text-decoration` longhands | ✅&ast; | color / style / thickness (2.0.5, #249). |
| `caret-color` | ✅&ast; | In-page text caret (2.0.5, #252). |
| `word-spacing` / `writing-mode` / `unicode-bidi` | ✖ | LTR only; vertical & RTL deferred (#248). |

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
| `transition` | ✖ | Degrades to the end state (no animation). |
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
| `gap` / `column-gap` | ✅ | Single-value; two-value `gap: A B` ◑ (row-gap shares storage). |
| Placement / span / `grid-auto-flow` | ◑ | Round 2 in progress (#279). |
| `subgrid` | ✖ | (#66) |

## Multiple Columns

| Feature | Support | Notes |
|---|:---:|---|
| `column-count` / `column-width` / `column-gap` | ✅ | Multicol V1. |
| `column-span: all` | ✖ | Parsed, not yet applied. |

## User Interface & Box Model

| Feature | Support | Notes |
|---|:---:|---|
| `box-sizing` | ✅ | |
| `resize` / `user-select` | ✖ | (#257) |
| `appearance` / native form-control styling | ✖ | Deferred (#80). |
| `outline` | ✖ | Focus rings deferred. |
| `image-rendering` | ▫&ast; | Parsed/stored; QuickDraw scaling is nearest-neighbor regardless (2.0.5, #256). |

## Modern building blocks

| Feature | Support | Notes |
|---|:---:|---|
| **CSS Custom Properties** (`var()`, `--foo`) | ✅&ast; | Native `var()` resolution at cascade time — the feature that unblocked modern themes (Drupal/XenForo). **First in the NetSurf family.** |
| **CSS Logical Properties** | ✅&ast; | `margin/padding/border-block\|inline`, `inset-*`, logical sizing (2.0.5, #247). |
| `:root`, attribute & structural selectors | ✅ | |
| `:is()` / `:where()` / `:has()` | ◑ | Partial matching (#163). |
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
