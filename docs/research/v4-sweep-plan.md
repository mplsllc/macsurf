# v4 sweep, close out "impossible CSS"

Target: rip through every remaining high-impact CSS3 feature MacSurf
doesn't yet support, end with a v4 tag, then move on to v5 (whatever
that turns out to be, content adaptation, JS-ier engine, etc.).

The sweep is structured as ~12 focused fix rounds. Each round is
shippable on its own and adds visible behaviour. Ordering puts the
cheap-but-high-impact features first so the test page fills out
quickly, then escalates to the big-lift items (animation, Grid).

## Rounds

### fixes73, transform V3: scale + skew + glyph rotation 90°/270°
- Add second int32 storage slot for `scale_x_q88` + `scale_y_q88`
- Parse `scale()`, `scaleX()`, `scaleY()`, `skewX()`, `skewY()`, `matrix()`
- Plotter applies scale to box corners pre-rotation
- Plot_text recognises 90°/270° → per-character vertical placement
- Closes out the transform chapter started by fixes71/72

### fixes74, `::before` / `::after` + `content:` strings
- libcss has selector support already; need pseudo-element box
  generation in box_construct.c
- Inject a synthetic BOX_INLINE between the element box and its
  children, holding the `content:` string
- Common usage: tooltips, decorative quotes, list markers

### fixes75, radial-gradient + conic-gradient
- Extend gradient infrastructure already used by linear-gradient
  (fixes47/49 multi-stop framework)
- Radial: per-pixel distance-from-centre interpolation
- Conic: per-pixel angle-from-centre interpolation
- QuickDraw plotter walks the rectangle pixel-by-pixel for the
  gradient fill, slow but acceptable for small boxes

### fixes76, clip-path
- `clip-path: polygon(...)`, `circle()`, `ellipse()`, `inset()`
- QuickDraw `RgnHandle` is built-in for clipping, direct fit
- Construct the shape's path as a region, push it on the clip stack
  before drawing the element's content

### fixes77, filter (blur, grayscale, brightness, contrast, sepia)
- Per-pixel image processing on an offscreen GWorld
- Box blur: separable two-pass, kernel size 1..16 px
- Grayscale: luminance = 0.299R + 0.587G + 0.114B (fixed-point)
- Brightness / contrast: simple linear pixel ops
- Sepia: matrix multiply for tinting
- Composite filtered GWorld back over the box bounds

### fixes78, transition
- Single-property interpolation between two values over time
- Hook the WaitNextEvent loop's nullEvent for animation frames
- Linear easing first; cubic-bezier later
- Most useful for: hover state, attribute change, class swap
- Smaller-scoped cousin of animation; lays the timing groundwork

### fixes79, animation + @keyframes
- Parse `@keyframes name { 0% {...} 100% {...} }` rules
- Property `animation: name 2s linear infinite`
- Re-uses fixes78 interpolation engine
- Each animated element gets a scheduler entry that advances on
  each nullEvent pass, mutates its computed style, triggers
  invalidate
- This is the visceral "OS 9 doing 2025 web" demo

### fixes80, 3D transforms (perspective, rotateX, rotateY, rotateZ)
- Extends V2's matrix to 4×4 with perspective divide
- `perspective: 800px` on parent sets the z-foreshortening factor
- `rotateY(45deg)` rotates around the vertical axis
- Each corner gets (x, y, z) → perspective-divide → (x', y')
- Renders rotated parallelograms via QuickDraw poly fill
- Cards flipping etc.

### fixes81, backdrop-filter
- `backdrop-filter: blur(8px)` reads pixels BEHIND the element
  from screen, blurs them via fixes77 infrastructure, paints the
  blurred copy under the element's background
- Frosted-glass overlays, the iOS aesthetic
- Expensive but doable for small areas; document as opt-in

### fixes82, CSS Grid (explicit tracks subset)
- `display: grid`
- `grid-template-columns: 1fr 1fr 200px` (track sizing, px, fr, %)
- `grid-template-rows`
- `gap` (already done for flex, extend semantics to grid)
- `grid-column` / `grid-row` for explicit placement (line numbers)
- SKIP for v4: named lines, grid-template-areas, auto-placement
  algorithms, span keywords. Those are v5.

### fixes83, utilities sweep
- `text-overflow: ellipsis` + `overflow: hidden` + `white-space: nowrap`
- `aspect-ratio: 16/9`
- CSS counters: `counter-increment`, `counter-reset`, `content: counter(...)`
- `writing-mode: vertical-rl` (now we have per-glyph plot support)
- `position: sticky`
- `@media (prefers-color-scheme: dark)`, wire to a system pref
- Multiple backgrounds (`background: url(...), linear-gradient(...)`)

### fixes84, v4 milestone
- New CLAUDE.md state section documenting everything that lands
- Tag `v0.4` (or `v0.5`, current tag is `v0.4.1`)
- Test page sweep: every feature shown on a single demo page
- Screenshot reel for the project README

## Per-round shape

Each round follows the now-established workflow:
1. Add the libcss property + cascade + storage (when needed)
2. Add the renderer / plotter / box-construct hook
3. Update advanced.html with a new PROBE card demonstrating the feature
4. `make` on G4, screenshot, ship the tar

Some rounds (fixes79 animation, fixes82 Grid) may overflow a single
shippable unit and split into a/b/c sub-rounds. Won't pre-commit to
the granularity; let the work decide.

## Risk register

| Risk | Round | Mitigation |
|---|---|---|
| Per-pixel filters too slow on G3 | 77, 81 | Cache filter output; opt-in via `@media (min-resolution)` |
| Grid track sizing complexity | 82 | Stick to explicit-track v4; full auto-placement is v5 |
| Animation eats nullEvent budget | 78, 79 | Frame skip when redraws can't keep up; cap to 30 fps |
| 16-bit transform storage runs out | 73, 80 | Already adding macsurf_transform_b; if 3D needs more, allocate from libcss arena |
| @keyframes parsing edge cases | 79 | Start with 0% / 100% only; add intermediate stops in a follow-up |

## Out of scope for v4

These remain on the V5+ backlog:
- `mask` (alpha mask compositing, too much pixel work)
- `mix-blend-mode` (per-pixel blend ops)
- `font-display: swap` (no web fonts on OS 9)
- `scroll-snap`
- `@container` queries
- Subgrid
- View transitions API (browser-side, JS-driven)
