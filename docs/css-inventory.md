# MacSurf CSS Inventory

Post-revert snapshot at the 0.1a1 baseline (fixes158a + fixes159 revert). Three buckets, derived from grepping `css_computed_*(` callsites across `browser/netsurf` and `browser/libdom` against the full libcss `property_handlers[]` table.

Counts are property-name count, not feature count, a single feature like "border" expands to 20+ property names.

## Working, 101 properties

libcss parses → cascade computes → layout/redraw consumes. The bulk of CSS 2.1 plus CSS3 backgrounds, transforms, flex, grid V1, gradients, opacity, shadows, custom properties, viewport units, aspect-ratio.

- **Box model.** width, height, min-/max-width/height, margin-{top,right,bottom,left}, padding-{top,right,bottom,left}, box-sizing.
- **Border.** border-{top,right,bottom,left}-{color,style,width}, border-collapse, border-spacing, border-radius.
- **Background.** background-color, background-image, background-position, background-repeat, background-attachment; gradients (linear, radial, stops).
- **Typography.** color, font-family, font-size, font-weight, font-style, font-variant, line-height, letter-spacing (partial, see below), text-align, text-indent, text-transform, text-decoration, vertical-align, white-space, direction, unicode-bidi.
- **Display & flow.** display (block, inline, inline-block, none, flex, grid, table family), position, top/right/bottom/left, float, clear, clip, visibility, opacity, z-index.
- **Flex (read-axis V1).** flex-basis, flex-direction, flex-grow, flex-shrink, flex-wrap, justify-content, align-content, align-items, align-self, order.
- **Grid V1.** macsurf-grid pipeline (fr units, repeat(), minmax(), grid-template-columns + rows, gaps), explicit placement via macsurf_grid_placement (grid-column/row + longhands, positive int lines, fixes158).
- **Effects.** macsurf-transform (rotate / scale / translate), macsurf-text-shadow, box-shadow, opacity, macsurf-gradient.
- **Lists.** list-style-type (decimal + disc + circle + square; roman/alpha variants partial, see below), list-style-image, list-style-position.
- **Tables.** table-layout, caption-side, empty-cells.
- **Content.** content (counters incl. counter-reset / counter-increment), quotes.
- **Cursor & UI.** cursor (consumed by hit-test, but only the standard set; needs verification of `pointer` actually changing pointer).
- **Outline.** outline-color, outline-style, outline-width, paints a rect outside the border box (redraw.c fixes40 vintage). Stroke widths > 1px may render thin until QuickDraw PenSize path lands.
- **Overflow.** overflow-x, overflow-y.
- **Other.** column-gap (row-gap not consumed), object-fit, text-overflow, aspect-ratio.

## Parsed by libcss, NOT consumed by layout/redraw

The fast lane. libcss already parses and stores these; honoring them is purely a layout or redraw change. **No new libcss properties, no struct surgery, no fixes159-style risk.**

### High-value (real impact on real-world sites)

| Property family | Cost | Why it's worth doing |
|---|---|---|
| **CSS multi-column** (`column-count`, `column-width`, `column-rule-*`, `column-span`, `column-fill`, `columns` shorthand) | Moderate. Layout needs a multi-column flow pass. | Visible win on news sites, blogs, long-form layouts. |
| **word-break, overflow-wrap** | Small. Text-flow tweak in layout. | CJK wrap, long URL/identifier wrap, fixes the "page goes off the right edge" crash on data tables. |
| **list-style-type extensions** (lower-roman, upper-roman, lower-alpha, upper-alpha, decimal-leading-zero, lower-greek, etc.) | Small. List-marker formatter switch. | Document and reference site rendering. |
| **text-decoration-color** | Tiny. Separate color from text when drawing underline. | Visual polish; some sites use grey underlines. |
| **pointer-events: none** | Tiny. Hit-test skip flag. | Lets some sticky-overlay sites be clickable underneath. |

### Print-only (low priority, MacSurf can't print)

`page-break-{after,before,inside}`, `break-{after,before,inside}`, `orphans`, `widows`.

### Shorthands that "work" via their longhands

`background`, `border` (and sides), `font`, `flex`, `flex-flow`, `list-style`, `margin`, `padding`, `outline` (shorthand), `overflow` (shorthand), `gap`, `columns`, `column-rule`. The shorthand getter shows 0 consumers but the feature is already covered by the longhands. No work needed.

### Aural CSS, intentionally skipped forever

azimuth, cue (3 variants), elevation, pause (3 variants), pitch (2 variants), play-during, richness, speak (4 variants), speech-rate, stress, voice-family, volume. 20 properties from CSS 2.1's aural module; a visual browser doesn't implement them.

### SVG presentation

fill-opacity, stroke-opacity. Useful only when SVG rendering lands as a separate project.

## Not parsed by libcss, adding any of these is the trap zone

These properties don't have libcss parsers. Adding one means new struct fields, propstrings entries, dispatch table rows, the surface area that ate fixes159. **The default move is to look for a preprocessor pattern (rewrite to an existing libcss property in cssh_css.c) before reaching for new vendor wiring.**

### Layout, implementable via preprocessor or layout-side carry

- **grid-template-areas**, named grid cells. Preprocessor resolves names → col/row, emits existing macsurf-grid-placement packed int. No new property.
- **justify-items, justify-self** (Grid V2 alignment). Deferred. Next attempt: bit-pack into existing macsurf_grid_col_span int32 (8 spare high bits), not a new property. See [[project_grid_v2_alignment_deferred]].
- **place-items, place-self, place-content**, wait for justify-* to land first.
- **position: sticky**, scroll-position aware layout, non-trivial.
- **subgrid**, dense auto-flow, named grid lines, negative grid line numbers, defer further.

### Visual, out of QuickDraw's lane

- `clip-path`, `mask`, `mask-image`, `mix-blend-mode`, `filter` (blur, drop-shadow), `backdrop-filter`, `border-image`. All require compositing primitives QuickDraw doesn't have. Skip.

### Motion

- `transition`, `animation`, `@keyframes`, needs a time loop tied to the event pump. Possible, but it's a project on its own.
- `transform-origin`, `transform-style`, `perspective`, extensions to working transform.

### Modern selectors / queries

- `@container` queries, needs container metric tracking.
- `:has()` selector, selector engine extension.
- `@supports`, partial; parser probably handles it, logic untested.

### Color Module 4

- `color-mix()`, `oklch()`, `lab()`, `lch()`. Not parsed. Cheap to add `oklch` because the conversion math is straightforward, but rarely needed in practice, sites usually fall back to rgb/hex.

## Partial / known-quirky

These work for some inputs but not others; documenting so we don't think they're broken.

- **letter-spacing.** Honored at the run level but exact glyph-by-glyph spacing depends on font metrics; some fonts show drift.
- **list-style-type.** Decimal, disc, circle, square confirmed. Roman/alpha/greek not verified.
- **outline > 1px.** Forward-references a QuickDraw PenSize path that may not be fully wired; thicker outlines may render at 1px on hardware.
- **align-items / justify-content keywords.** libcss accepts `flex-start` / `flex-end` / `center` / `stretch` / `baseline`. Bare `start` / `end` (CSS Grid syntax) are silently dropped, they fall back to `stretch`.
- **font-family aliases.** Sans-serif / serif / monospace dispatch landed in fixes157. Custom font lookups via lookup table; missing fonts fall back to Helvetica.

## Known limitations carried past 0.1a1

See [[project_known_issues_0_1a1]] for the live list. Highlights:
- 16 MB Carbon partition can't hold image-heavy modern sites.
- `%u` in macos9_image.c logger (cosmetic, one-line cleanup).
- Grid V2 alignment deferred.

## Strategic next moves

Looking at the gaps + the user's pace concern, the cheap-wins-first order is:

1. **Polish pack (fixes160).** word-break, overflow-wrap, list-style-type extensions, text-decoration-color, pointer-events: none, %u logger cleanup. All redraw-only or text-flow-only, all already parsed. One tar, 5-6 properties.
2. **CSS multi-column (fixes161+).** Real layout work but a real visual win. column-count + column-rule + column-fill.
3. **grid-template-areas (fixes162).** Preprocessor pattern; no libcss surgery.
4. **Grid V2 alignment retry (fixes163).** Bit-packed into existing field, learning from fixes159.
5. **transitions / animations.** Big project, defer to a dedicated sprint.
