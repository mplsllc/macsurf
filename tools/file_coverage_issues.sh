#!/bin/bash
# One-shot: file the CSS/JS/rendering coverage-gap issues identified in the
# 2026-07-12 full-coverage audit. Grouped by feature cluster (per maintainer).
# Deduped against existing open issues (#29 #34 #37 #38 #40 #42 #62 #66 #72 #75
# #77 #80 #81 #83 #84 #88 #90 #91 #100 #104 #106 #109 #111 #113 #114 #126 #163
# #164 #176 #182 #186 #211 #222 #226 #227 #230 #234 #235). Prints each URL.
set -e
mk() { gh issue create "$@" | tail -1; }

echo "=== CSS property clusters ==="

mk --title "CSS: Grid V2 follow-ups (stretch default, FR-row distribution, named lines/areas, dense flow, negative lines)" \
  --label "enhancement" --label "area: grid" --label "priority: medium" \
  --body "Grid V1 (\`-macsurf-grid*\` + layout_grid.c) is solid, but several V2 behaviours are missing/partial:
- \`align-items: stretch\` default not applied — cells leave empty space when the row track exceeds cell content height.
- FR **row** distribution against a definite container \`height\` unimplemented (only column FR distribution shipped, fixes148).
- Named grid lines and full \`grid-template-areas\` name lookup.
- Dense \`grid-auto-flow: dense\`.
- Negative line numbers beyond the \`-1\` fill sentinel; \`grid-area\` shorthand across separate rules.
Audit ref: .private/docs/css-status.md 'Grid V2 follow-ups'."

mk --title "CSS: logical properties (margin/padding/border-block|inline, inset-*, block-size/inline-size, min/max-*-size)" \
  --label "enhancement" --label "area: css" \
  --body "cssh_css.c has \`macsurf__rewrite_logical_properties\` doing partial lowering, but full flow-relative support is incomplete: \`margin-block/-inline-*\`, \`padding-block/-inline-*\`, \`border-block/-inline-*\`, \`inset\`/\`inset-block/-inline-*\`, \`block-size\`/\`inline-size\`, \`min/max-block/inline-size\`. Map to physical longhands via writing-mode/direction. Low urgency for LTR-horizontal but increasingly common in modern resets/frameworks."

mk --title "CSS: writing-mode + unicode-bidi (vertical & bidirectional text)" \
  --label "enhancement" --label "area: css" --label "priority: low" \
  --body "Both parsed, neither consumed (dump.c-only). \`writing-mode: vertical-rl/vertical-lr\` and \`unicode-bidi\` (isolate/embed/bidi-override). \`direction\` IS consumed. Needs vertical line-layout + BiDi reordering — large scope; track for completeness."

mk --title "CSS3: text-decoration longhands (color/style/thickness, underline-offset/position)" \
  --label "enhancement" --label "area: css" \
  --body "Only the legacy \`text-decoration\` shorthand is consumed (redraw.c). Dropped: \`text-decoration-color\`, \`text-decoration-style\` (solid/double/dotted/dashed/wavy), \`text-decoration-thickness\`, \`text-underline-offset\`, \`text-underline-position\`, \`text-decoration-line\` as an independent longhand. QuickDraw can do color + a few styles; wavy/double approximate."

mk --title "CSS3: font-selection longhands (font-kerning/stretch/feature-settings/variant-caps/size-adjust)" \
  --label "enhancement" --label "area: css" --label "area: fonts" \
  --body "Modern font-selection descriptors are unparsed/unconsumed: \`font-kerning\`, \`font-stretch\`, \`font-feature-settings\`, \`font-variant-caps\`, \`font-size-adjust\`, \`@font-palette-values\`. Most degrade acceptably on QuickDraw (no variable fonts), but \`font-variant-caps: small-caps\` and \`font-kerning\` are worth honoring. Related: #91 per-font metrics, #77 @font-face."

mk --title "CSS: text/typography properties (tab-size, hyphens, text-align-last, text-justify, hanging-punctuation)" \
  --label "enhancement" --label "area: css" \
  --body "\`tab-size\` (parsed, not consumed — affects \`white-space: pre\` code blocks), \`hyphens\`/\`hyphenate-character\`, \`text-align-last\`, \`text-justify\`, \`hanging-punctuation\`. tab-size + hyphens are the highest-value here for readable code/prose."

mk --title "CSS: UI color properties (accent-color, caret-color, color-scheme, scrollbar-color)" \
  --label "enhancement" --label "area: css" \
  --body "\`accent-color\` + \`caret-color\` are parsed but dropped; \`color-scheme\` and \`scrollbar-color\` unparsed. accent-color (checkbox/radio/range tint) and caret-color (text caret) are small wins that visibly modernize form pages. Related: #80 form control styling."

mk --title "CSS: box-alignment shorthands (place-content/items/self, justify-items, justify-self)" \
  --label "enhancement" --label "area: css" \
  --body "Flex/Grid alignment longhands are consumed, but the shorthands \`place-content\`/\`place-items\`/\`place-self\` and grid's \`justify-items\`/\`justify-self\` aren't lowered. Expand to the existing align-*/justify-* longhands (mostly a preprocessor rewrite pass like the grid-alignment one)."

mk --title "CSS: individual transform properties + 3D (translate/rotate/scale, transform-origin, perspective, transform-style, backface-visibility)" \
  --label "enhancement" --label "area: css" \
  --body "\`transform\` is bridged to \`-macsurf-transform\` (2D affine: rotate/translate/scale). Missing: the individual \`translate\`/\`rotate\`/\`scale\` properties, \`transform-origin\` (origin currently fixed), and the 3D set \`perspective\`/\`transform-style\`/\`backface-visibility\`/\`perspective-origin\`. 3D can flatten to 2D; transform-origin + individual props are the tractable wins."

mk --title "CSS: background painting properties (background-clip, background-origin, background-blend-mode, background-attachment:fixed honoring)" \
  --label "enhancement" --label "area: css" --label "area: rendering" \
  --body "\`background-clip\` (border-box/padding-box/content-box, and \`text\` for gradient text), \`background-origin\`, \`background-blend-mode\` all unhonored. Also: \`background-attachment: fixed\` is *read* at redraw.c:1655/1787 but only detected, not truly viewport-pinned. background-clip:text unlocks the popular gradient-text effect."

mk --title "CSS: image-rendering + box-decoration-break" \
  --label "enhancement" --label "area: css" --label "area: rendering" \
  --body "\`image-rendering\` (parsed, dropped) — \`pixelated\`/\`crisp-edges\` should pick nearest-neighbor, \`auto\`/\`smooth\` the box-filter downscale (fixes203). Relevant to sharp scaling of pixel art / retro assets. \`box-decoration-break\` (parsed, dropped) — clone vs slice across line/column breaks."

mk --title "CSS: user-select + resize" \
  --label "enhancement" --label "area: css" \
  --body "\`user-select: none/text/all\` (suppress selection on chrome-y UI) and \`resize\` (textarea resize handle) are unparsed/unconsumed. Low priority but cheap; user-select improves text-selection UX on button/label-heavy pages."

mk --title "CSS: SVG presentation properties (fill-opacity, stroke-opacity, and inline-SVG paint gaps)" \
  --label "enhancement" --label "area: css" --label "area: rendering" \
  --body "\`fill-opacity\` / \`stroke-opacity\` parsed but dropped. Inline SVG (macos9_svg_inline.c) renders paths/rects/gradients/text; opacity on fills/strokes and broader SVG presentation attrs (stroke-dasharray, stroke-linecap/join) are gaps. Track SVG presentation completeness here."

echo "=== CSS low-priority grab-bag + wontfix ==="

mk --title "CSS: misc modern properties (low priority) — zoom, all, initial-letter, paint-order, overscroll-behavior, overflow-anchor, shape-outside, offset motion path" \
  --label "enhancement" --label "area: css" --label "priority: low" \
  --body "Umbrella for low-impact modern properties currently unparsed: \`zoom\`, \`all\`, \`initial-letter\`, \`paint-order\`, \`overscroll-behavior*\`, \`overflow-anchor\`, \`shape-outside\`, and the \`offset-*\` motion-path family. Individually rare on real pages; grouped so they're on the record without cluttering the active queue."

mk --title "CSS: aural/speech media properties (wontfix — documented)" \
  --label "wontfix" --label "area: css" \
  --body "The 18 CSS2 aural/speech properties are parsed (legacy libcss enum) but will never render on a visual browser: azimuth, elevation, cue-after, cue-before, pause-after, pause-before, pitch, pitch-range, play-during, richness, speak, speak-header, speak-numeral, speak-punctuation, speech-rate, stress, voice-family, volume. **Won't-fix**, filed for a complete coverage record. Paged-media/fragmentation (orphans/widows/break-*/page-break-*) is tracked under Print (#98), not here."

echo "=== JS/DOM clusters ==="

mk --title "DOM: getElementsByClassName / getElementsByTagName / getElementsByName return empty-array stubs" \
  --label "enhancement" --label "area: js-engine" \
  --body "macsurf_qjs.c:2427-2431 stub these to \`[]\` even though \`qjs_collect_by_tag\` already exists and could back getElementsByTagName natively. Wire them to libdom collections. Common on older/simpler sites that predate querySelector."

mk --title "DOM: innerHTML/outerHTML get is lossy and set doesn't parse (no HTML fragment parser)" \
  --label "enhancement" --label "area: js-engine" --label "priority: high" \
  --body "macos9_qjs.c: \`innerHTML\` read returns textContent (strips markup, :1483); write strips tags via regex and sets textContent (:1485) — no fragment parsing. \`outerHTML\` synthesizes a shallow string (:1490). Real sites that build DOM via \`el.innerHTML = '<...>'\` get flattened/empty output. Needs a hubbub fragment-parse → libdom insert path. High impact for JS-driven pages."

mk --title "DOM: non-element node traversal missing (childNodes/firstChild/lastChild/nextSibling/previousSibling, text nodes)" \
  --label "enhancement" --label "area: js-engine" \
  --body "Only ELEMENT_NODE traversal is live (children, nextElementSibling, etc.). \`childNodes\`/\`firstChild\`/\`lastChild\`/\`nextSibling\`/\`previousSibling\` are static empty/null stubs (:1617-1619) and text nodes are never wrapped. Scripts that walk mixed text+element children, or read \`node.nodeValue\`, break."

mk --title "JS: event bubbling/capture + real UI dispatch bridge (click/submit/input/change → JS)" \
  --label "enhancement" --label "area: js-engine" --label "priority: high" \
  --body "Event model is flat: dispatchEvent only invokes listeners on the exact target — no bubbling, no capture, no delegation (the dominant pattern in jQuery/modern apps). And there is no C→JS bridge synthesizing DOM events from real UI: \`macsurf_qjs_dispatch_dom_click\` (:4172) is a stub returning 0, so a real click never fires page \`click\`/\`submit\`/\`input\`/\`change\` handlers (only \`DOMContentLoaded\`/\`load\` flow from C). Two-part: (a) tree-walking bubble/capture dispatch; (b) wire frontend mouse/key/form events into it."

mk --title "JS: getComputedStyle + getBoundingClientRect are stubs (layout is invisible to JS)" \
  --label "enhancement" --label "area: js-engine" \
  --body "\`getComputedStyle\` (:2494) returns only inline \`style\` values; \`getBoundingClientRect\`/\`getClientRects\` (:1607) return zeros. Any script that measures layout or reads cascaded/computed values (sticky headers, lightboxes, responsive JS, #186 JS-gated backgrounds) fails. Wire to libcss computed style + the box tree geometry."

mk --title "JS: document.cookie / document.title getter / readyState not wired to real state" \
  --label "enhancement" --label "area: js-engine" \
  --body "\`document.cookie\` is a dead \`''\` (:2417) — not wired to the fixes367/368 libdom cookie jar (breaks JS session/consent logic). \`document.title\` getter always returns \`''\` (:713, setter works). \`readyState\` hardcoded \`'complete'\`. Small wiring wins that unblock common site scripts."

echo "ALL COVERAGE ISSUES FILED"
