# MacSurf development roadmap

This is the maintained roadmap for active development. Historical plans and
research snapshots are context, not scheduling authority.

## Operating rule

Choose work from the newest `.private/research/css-gap-inventory-*.md`, verify
the current code before starting, and land one hardware-testable slice at a
time. `docs/status.md` records accepted checkpoints; `docs/css-support.md` is
the user-facing capability summary.

## Current focus: Modern Compatibility Foundation

The active campaign prioritises browser contracts that cause modern pages to
choose the wrong path or discard whole rule blocks. It replaces the former
transition-first queue.

CSS Transitions #322 remains open at its accepted checkpoint: native parsing,
cascade, and computed descriptors, plus opacity temporal presentation, are
hardware-verified. Further transition adapters, CSS Animations (#323), Canvas,
and WebGL are paused until the campaign's post-Grid compatibility census.

### Round 1 — browser foundations

1. `matchMedia` / `MediaQueryList` (#342)
2. `:is()` / `:where()` (#163)
3. MutationObserver (#105)

### Round 2 — selectors

1. `:has()` (#163)
2. DOM `querySelector` / `querySelectorAll` selector parity (#334)

### Round 3 — layout observers

1. IntersectionObserver (#341)
2. ResizeObserver (#340)

### Round 4 — module loading

ES module scripts and their static module graph (#343).

### Round 5 — browser APIs

1. Bounded Web Crypto (#337)
2. Stylesheet CSSOM (#339)

### Round 6 — layout

Grid V2 remainder (#246): stretch behaviour, definite-height FR rows, named
line/area lookup, dense flow, negative lines, and shorthand remainder.

After Round 6, rerun the real-page capability census and set the next roadmap
from observed gaps. Do not resume the deferred work opportunistically.

## Deliberately deferred

Writing modes/bidi, pagination/fragmentation, SVG paint-only opacity,
`clip-path`/mask/filter, and subgrid require subsystems not present in this
browser. They are not opportunistic CSS-property tasks.

## Delivery standard

Every slice needs focused host coverage, C89 gates, a CodeWarrior build, and
maintainer-confirmed hardware evidence before its tracker status is advanced.
