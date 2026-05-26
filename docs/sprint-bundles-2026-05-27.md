# Sprint bundles — 2026-05-27

Grouped from the 50+ open CSS issues filed yesterday. Each bundle shares a test page surface and a related code-change region, so one tar + one log capture validates the whole bundle.

Ordered roughly by effort × value (smallest-fastest first). Skip-able bundles for "tomorrow" tagged at the end.

---

## Bundle A — Text styling (one HTML page covers all)

**Test page:** `test-text.html` with one card per probe.
**Code region:** `html_font.c` resolution + `plotters.c` text paint + `macos9_font.c` measurement.

| # | Title | V1 scope |
|---|---|---|
| [#26](https://github.com/mplsllc/macsurf/issues/26) | Multi-tier font-weight | Threshold >= 600 → bold; <600 → regular |
| [#51](https://github.com/mplsllc/macsurf/issues/51) | word-spacing edge cases | Negative values, percent, `normal` |
| [#53](https://github.com/mplsllc/macsurf/issues/53) | text-transform | uppercase, lowercase, capitalize (ASCII) |
| [#54](https://github.com/mplsllc/macsurf/issues/54) | letter-spacing edge cases | Negative, percent, em, `normal` |
| [#57](https://github.com/mplsllc/macsurf/issues/57) | text-indent | First-line cursor advance |
| [#58](https://github.com/mplsllc/macsurf/issues/58) | tab-size | Advance to next `tab-size * char-width` multiple |
| [#44](https://github.com/mplsllc/macsurf/issues/44) | text-decoration extended | color, dashed/dotted style, thickness |

**Total: 7 issues, mostly small. Estimated 3-4 hours.**

---

## Bundle B — Backgrounds & gradients

**Test page:** `test-bg.html` with 6 cards.
**Code region:** `cssh_css.c` preprocessor + `plotters.c` gradient paint.

| # | Title | V1 scope |
|---|---|---|
| [#27](https://github.com/mplsllc/macsurf/issues/27) | Stacked `linear-gradient(a), linear-gradient(b)` | Extract first gradient; drop rest |
| [#41](https://github.com/mplsllc/macsurf/issues/41) | `background-attachment: fixed` for gradients | Reuse fixes137 viewport-anchored origin |
| [#78](https://github.com/mplsllc/macsurf/issues/78) | image-rendering: pixelated | Disable box-filter, use nearest-neighbor |

**Total: 3 issues, small. Estimated 1-2 hours.**

---

## Bundle C — Grid V2 follow-ups

**Test page:** `test-grid2.html` (mirrors test3.html style).
**Code region:** `layout_grid.c` + `cssh_css.c` shorthand expansion.

| # | Title | V1 scope |
|---|---|---|
| [#25](https://github.com/mplsllc/macsurf/issues/25) | justify-items / justify-self | cssh_css `text-align` bridge for cells |
| [#64](https://github.com/mplsllc/macsurf/issues/64) | place-items / place-content shorthands | cssh_css split into align-+justify- pair |
| [#65](https://github.com/mplsllc/macsurf/issues/65) | grid-auto-flow: dense | Backfill empty cells with later items |

**Total: 3 issues, medium. Estimated 2-3 hours.**

---

## Bundle D — Visual feedback (outline + cursor + image-rendering)

Already partially in Bundle B (image-rendering). Splitting for clarity if Bundle B is run separately.

**Test page:** `test-visual.html` with focus / hover / link probes.
**Code region:** `redraw.c` outline paint + `window.c` cursor dispatch.

| # | Title | V1 scope |
|---|---|---|
| [#60](https://github.com/mplsllc/macsurf/issues/60) | outline + outline-offset | Paint outline rect outside border |
| [#79](https://github.com/mplsllc/macsurf/issues/79) | cursor custom values | wait, help, crosshair, not-allowed, grab, etc. |

**Total: 2 issues, small. Estimated 1-2 hours.**

---

## Bundle E — Parser audit (no-op acceptance for hint properties)

**Test page:** `test-noop.html` — just confirms parser accepts the property names without `unknown property` warnings.
**Code region:** None functional; verify libcss parses, log silently.

| # | Title | V1 scope |
|---|---|---|
| [#85](https://github.com/mplsllc/macsurf/issues/85) | contain | Recognise but ignore (it's a perf hint) |
| [#86](https://github.com/mplsllc/macsurf/issues/86) | will-change | Recognise but ignore |
| [#87](https://github.com/mplsllc/macsurf/issues/87) | overscroll-behavior | Recognise but ignore |
| [#33](https://github.com/mplsllc/macsurf/issues/33) | break-* / page-break-* / orphans / widows | Recognise but ignore (no print path) |

**Total: 4 issues, trivial. Estimated 30 minutes.**

---

## Bundle F — Inline / line metrics

**Test page:** `test-lines.html` with mixed line-height + vertical-align probes.
**Code region:** Inline layout in `layout.c`.

| # | Title | V1 scope |
|---|---|---|
| [#55](https://github.com/mplsllc/macsurf/issues/55) | line-height numeric multiplier | Cascade-correct unitless * font-size |
| [#59](https://github.com/mplsllc/macsurf/issues/59) | vertical-align extended | text-top, text-bottom, super, sub |

**Total: 2 issues, medium. Estimated 2 hours.**

---

## Bundle G — Pseudo-element styling

**Test page:** `test-pseudo.html` with list + form input probes.
**Code region:** Marker paint + placeholder text paint.

| # | Title | V1 scope |
|---|---|---|
| [#70](https://github.com/mplsllc/macsurf/issues/70) | ::marker (list bullet styling) | Color + font on the marker glyph |
| [#71](https://github.com/mplsllc/macsurf/issues/71) | ::placeholder | Color on form input greyed text |

**Total: 2 issues, medium. Estimated 2 hours.**

---

## Bundle H — Form-state pseudo-classes + accent

**Test page:** `test-forms2.html` (extension of yesterday's test-forms.html).
**Code region:** Selector evaluator + form paint.

| # | Title | V1 scope |
|---|---|---|
| [#89](https://github.com/mplsllc/macsurf/issues/89) | :checked / :disabled / :valid / :invalid | Match against DOM form-state |
| [#73](https://github.com/mplsllc/macsurf/issues/73) | accent-color + caret-color | Color form controls + TextEdit caret |

**Total: 2 issues, medium. Estimated 2-3 hours.**

---

## Bundle I — @-rules

**Test page:** `test-atrules.html`.
**Code region:** `cssh_css.c` + media-query evaluator.

| # | Title | V1 scope |
|---|---|---|
| [#52](https://github.com/mplsllc/macsurf/issues/52) | @media prefers-color-scheme / orientation | Recognise + evaluate (light=true, dark=false default) |
| [#74](https://github.com/mplsllc/macsurf/issues/74) | @supports | Conditional inclusion based on property table |
| [#76](https://github.com/mplsllc/macsurf/issues/76) | @layer | Flatten or honour cascade priority |

**Total: 3 issues, medium-large. Estimated 3-4 hours.**

---

## Bundle J — Logical properties + sizing keywords

**Test page:** `test-logical.html`.
**Code region:** `cssh_css.c` alias pass + layout sizing resolution.

| # | Title | V1 scope |
|---|---|---|
| [#61](https://github.com/mplsllc/macsurf/issues/61) | inline-size, block-size, padding-inline-*, margin-inline-*, inset-inline-* | cssh_css alias to physical (LTR-only V1) |
| [#62](https://github.com/mplsllc/macsurf/issues/62) | min-content, max-content, fit-content | Intrinsic-size resolution |

**Total: 2 issues, but #62 is larger. Estimated 2-3 hours.**

---

## Bundle K — Text flow direction (carry-over)

**Test page:** `test-dir.html`.
**Code region:** Inline layout direction swap.

| # | Title | V1 scope |
|---|---|---|
| [#35](https://github.com/mplsllc/macsurf/issues/35) | writing-mode / unicode-bidi | V1: block-direction swap only (no glyph rotation) |
| [#49](https://github.com/mplsllc/macsurf/issues/49) | direction: rtl | Right-aligned blocks + reversed inline sequence |

**Total: 2 issues, structural. Estimated 3-4 hours.**

---

## Bundle L — Multicol V3 + paged

**Test page:** `test-multicol3.html`.

| # | Title | V1 scope |
|---|---|---|
| [#28](https://github.com/mplsllc/macsurf/issues/28) | column-span: all | Segment children at span-all child, full-width slot between |

**Total: 1 issue, medium-large. Estimated 2 hours.**

---

## Bundle M — Counters V2 (solo)

| # | Title | V1 scope |
|---|---|---|
| [#42](https://github.com/mplsllc/macsurf/issues/42) | counter-set + nested counter scopes | Per-element counter stack; `counters()` plural |

**Total: 1 issue, medium. Estimated 2-3 hours.**

---

## Deferred this sprint (structural / risky / needs hardware probe)

- [#34](https://github.com/mplsllc/macsurf/issues/34) transition / animation — big feature
- [#37](https://github.com/mplsllc/macsurf/issues/37) clip-path / mask / filter — QD limitation
- [#38](https://github.com/mplsllc/macsurf/issues/38) caption-side — structural
- [#39](https://github.com/mplsllc/macsurf/issues/39) table-layout: fixed — destabilisation risk
- [#40](https://github.com/mplsllc/macsurf/issues/40) row-gap independent — bit slot work
- [#43](https://github.com/mplsllc/macsurf/issues/43) JPEG re-decode perf — needs hardware probe
- [#63](https://github.com/mplsllc/macsurf/issues/63) sticky V2 — sticky has its own ground state to handle
- [#66](https://github.com/mplsllc/macsurf/issues/66) subgrid — structural
- [#67](https://github.com/mplsllc/macsurf/issues/67) :has() — selector parser surgery
- [#68](https://github.com/mplsllc/macsurf/issues/68) :is() / :where() — selector expansion
- [#69](https://github.com/mplsllc/macsurf/issues/69) :nth-child extended — formula parser
- [#72](https://github.com/mplsllc/macsurf/issues/72) :focus-visible / :focus-within — needs focus event tracking
- [#75](https://github.com/mplsllc/macsurf/issues/75) @container — multi-pass layout dependency
- [#77](https://github.com/mplsllc/macsurf/issues/77) @font-face full — Font Manager integration
- [#80](https://github.com/mplsllc/macsurf/issues/80) appearance / form control overhaul — multi-file
- [#81](https://github.com/mplsllc/macsurf/issues/81) border-image — 9-slice painter
- [#82](https://github.com/mplsllc/macsurf/issues/82) box-decoration-break — inline split semantics
- [#83](https://github.com/mplsllc/macsurf/issues/83) mix-blend-mode — no QD primitive
- [#88](https://github.com/mplsllc/macsurf/issues/88) @property — depends on animation
- [#90](https://github.com/mplsllc/macsurf/issues/90) form input styling overhaul — multi-file
- [#36](https://github.com/mplsllc/macsurf/issues/36) fill-opacity / stroke-opacity — SVG paint
- [#84](https://github.com/mplsllc/macsurf/issues/84) scroll-behavior + scroll-snap — animation framework dep
- [#48](https://github.com/mplsllc/macsurf/issues/48) bookmarks, [#45](https://github.com/mplsllc/macsurf/issues/45) find, [#50](https://github.com/mplsllc/macsurf/issues/50) tabs — UX features, not CSS

---

## Recommended start order today

1. **Bundle E** (parser audit no-ops) — 30 min, 4 quick closes, builds momentum
2. **Bundle A** (text styling) — biggest visual-impact bundle, 7 closes
3. **Bundle B** (backgrounds/gradients) — 3 closes
4. **Bundle D** (outline / cursor) — 2 closes
5. **Bundle C** (grid V2 follow-ups) — 3 closes

Total target: **~19 issues closed in one day** if bundles A-E ship cleanly.

Stretch goals: F (inline metrics), G (pseudo-element styling), H (form-state pseudo-classes).
