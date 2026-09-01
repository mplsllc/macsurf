# MacSurf development roadmap

This is the maintained roadmap for active development. Historical plans and
research snapshots are context, not scheduling authority.

## Operating rule

Choose work from the newest `.private/research/css-gap-inventory-*.md`, verify
the current code before starting, and land one hardware-testable slice at a
time. `docs/status.md` records accepted checkpoints; `docs/css-support.md` is
the user-facing capability summary.

## Current focus: CSS Transitions (#322)

Round 2A (native parsing, cascade, computed descriptors) and Round 2B-2
(`opacity` temporal presentation) are hardware-verified. #322 remains open:
only opacity is supported, using the QuickDraw stipple approximation.

The next bounded slice is Round 2B-3: `color` and `background-color` only.
It must reuse the bounded transition-effect table and scheduler, preserve
computed Style B as the target state, interpolate `css_color` endpoints in
`0xAARRGGBB` before renderer conversion, and be host- and hardware-verified.
Stop after those two properties; transforms, layout transitions, and CSS
Animations (#323) are separate work.

## CSS backlog after the current transition slice

Re-derive the inventory before reprioritising. The currently useful buckets
are: height-family/flex-basis intrinsic keywords; Grid V2 remainder
(`minmax()`/`fit-content()` composition, stretch, definite-height FR rows);
and small independent layout consumers (`caption-side`, `list-style-position`).
Do not requeue completed `justify-self`, `background-clip: text`,
`background-origin`, or opacity work.

## Deliberately deferred

Writing modes/bidi, pagination/fragmentation, SVG paint-only opacity,
`clip-path`/mask/filter, subgrid, and CSS Animations require subsystems not
present in this browser. They are not opportunistic CSS-property tasks.

## Delivery standard

Every slice needs focused host coverage, C89 gates, a CodeWarrior build, and
maintainer-confirmed hardware evidence before its tracker status is advanced.
