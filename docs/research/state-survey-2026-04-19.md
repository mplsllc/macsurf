# MacSurf State Survey, 2026-04-19

Second snapshot, one day after the first real-page render (MacTrove,
2026-04-19, commit `56b59da` and predecessors). Goal: map exactly what
libcss handles correctly against MacTrove's actual stylesheets, so the
next execution round is surgical, not speculative. No code changes in
this round.

All `.c` / `.h` files in the macos9 frontend use Mac CR line endings ,
`wc -l` reports 0 for them. Use Read / Grep, not `wc` / `cat`.

Current branch: `v0.3-rendering`. Most recent commit at time of
writing: `56b59da docs(CLAUDE): note next fix zip is fixes133
(user-advanced counter)`.

Uncommitted working-tree changes (carried over from 2026-04-18):
`macos9_fetcher_stubs.c` (probe MS_LOG), `macos9_ns_fetcher.c` (small
user-local hunk), `proxy/macsurf-proxy` (build output).

---

# Track A, CSS rendering gap analysis

## A1. MacTrove CSS acquisition

### Fetch path

MacTrove (`http://mac.mp.ls/`) fronts the MacTrove
(`mactrove.com`) Drupal 11 site and is Cloudflare-protected. A direct
GET to `mac.mp.ls` over plain HTTP returns the Cloudflare "Just a
moment…" challenge interstitial. Going through the MacSurf proxy
(`http://116.202.231.103:8765`) returns the real Drupal page for HTML
requests, presumably because Cloudflare whitelists the proxy's Hetzner
IP or the origin header chain permits it. CSS subresource fetches from
the same proxy IP also return the challenge unless a `Referer:
http://mac.mp.ls/` header is sent. With that header they return the
real CSS.

Command used (for reproducibility):

```sh
curl -sS -x http://116.202.231.103:8765 \
     -A "MacSurf/0.3" \
     -H "Referer: http://mac.mp.ls/" \
     -H "Accept: text/css,*/*;q=0.1" \
     -o delta0.css "http://mac.mp.ls/sites/default/files/css/<hash>.css?delta=0&…"
```

(The `-A "MacSurf/0.3"` header matters, see
[macos9_ns_fetcher.c:222](../../browser/netsurf/frontends/macos9/macos9_ns_fetcher.c);
this is the UA the browser sends in production.)

### Files saved to `docs/research/mactrove-css/`

| File | Bytes | Rules (`{` count) | Source |
|---|---:|---:|---|
| `mactrove-delta0.css` | 2,943 | 55 | Drupal aggregate bundle delta=0, normalize.css + small core styles |
| `mactrove-delta1.css` | 59,579 | 603 | Drupal aggregate bundle delta=1, MacTrove's custom `platinum` theme |
| `fonts-tiny5.css` | 193 | 1 | `fonts.googleapis.com/css2?family=Tiny5` (referenced from HTML) |
| `mactrove-via-proxy.html` | 43,115 |, | Full index page (proxy route) |
| `mactrove-index.html` | 4,626 |, | Cloudflare challenge page (direct route, kept as evidence) |
| **Total CSS** | **62,715** | **659** |, |

### At-rule breakdown (across all three stylesheets)

| At-rule | delta0 | delta1 | tiny5 | Total |
|---|---:|---:|---:|---:|
| `@media` | 1 | 11 | 0 | **12** |
| `@import` | 0 | 0 | 0 | **0** |
| `@font-face` | 0 | 1 | 1 | **2** |
| `@keyframes` | 0 | 3 | 0 | **3** |
| `@supports` | 0 | 0 | 0 | **0** |

Distinct `@media` queries observed (width feature variants only ,
others listed inline):

- `@media (scripting:enabled)` (delta0)
- `@media (max-width:620px)` / `(max-width:720px)` / `(max-width:900px)`
- `@media (min-width:721px)` / `(min-width:721px) and (max-width:960px)`
- `@media (prefers-reduced-motion:reduce)`
- `@media print`

No `@supports` gating. No `@import` indirection. Font CDN
(`fonts.gstatic.com`) referenced by URL inside `@font-face { src: url(...) }`.

## A2. CSS feature inventory

Counts are absolute matches from the saved files. All sizable
features live in `delta1.css` (the Drupal theme bundle); `delta0.css`
is normalize.css plus a trivial `@media (scripting:enabled)` guard.

| Feature | delta0 | delta1 | tiny5 | Total |
|---|---:|---:|---:|---:|
| `display:flex` | 0 | 64 | 0 | 64 |
| `display:inline-flex` | 0 | 6 | 0 | 6 |
| `display:grid` | 0 | 7 | 0 | 7 |
| `display:inline-grid` | 0 | 0 | 0 | 0 |
| `var(--name)` uses | 0 | 219 | 0 | **219** |
| `--name:` definitions | 0 | 19 | 0 | 19 |
| `calc()` uses | 0 | 4 | 0 | 4 |
| `border-radius` | 0 | 30 | 0 | 30 |
| `box-shadow` | 0 | 40 | 0 | 40 |
| `transform:` | 1 | 9 | 0 | 10 |
| `transition:` | 0 | 11 | 0 | 11 |
| `animation:` | 0 | 3 | 0 | 3 |
| `position:sticky` | 0 | 2 | 0 | 2 |
| `object-fit` | 0 | 1 | 0 | 1 |
| `clip-path` | 0 | 0 | 0 | 0 |
| `mask` (mask:/mask-) | 0 | 0 | 0 | 0 |
| `margin-inline*` | 0 | 0 | 0 | 0 |
| `padding-inline*` | 0 | 0 | 0 | 0 |
| `margin-block*` / `padding-block*` | 0 | 0 | 0 | 0 |
| `inset-*` | 0 | 1 | 0 | 1 |

Flexbox properties (delta1):

| Property | Count |
|---|---:|
| `align-items` | 45 |
| `align-self` | 1 |
| `flex:` (shorthand) | 26 |
| `flex-direction` | 26 |
| `flex-shrink` | 4 |
| `flex-wrap` | 21 |
| `gap:` (shorthand gap) | 76 |
| `justify-content` | 17 |

Grid properties (delta1):

| Property | Count |
|---|---:|
| `grid-template-columns` | 19 |
| `column-gap` | 1 |

The 19 custom properties defined in `:root` are the theme tokens that
drive almost everything else (backgrounds, borders, text, link
colours, the full font stack):

```
--platinum-bg, --platinum-window-bg, --platinum-window-chrome,
--platinum-window-border, --platinum-bevel-light, --platinum-bevel-dark,
--platinum-shadow-dark, --platinum-text, --platinum-muted,
--platinum-dim, --platinum-link, --platinum-link-visited,
--platinum-accent, --platinum-highlight, --platinum-font-ui,
--platinum-font-body, --platinum-font-mono, --primary, --has-tile
```

Root definitions (verbatim):
```
--platinum-bg:#dddddd; --platinum-window-bg:#eeeeee;
--platinum-window-chrome:#cccccc; --platinum-window-border:#000000;
--platinum-bevel-light:#ffffff; --platinum-bevel-dark:#888888;
--platinum-shadow-dark:#666666; --platinum-text:#000000;
--platinum-muted:#555555; --platinum-dim:#888888;
--platinum-link:#0000cc; --platinum-link-visited:#551a8b;
--platinum-accent:#3366cc; --platinum-highlight:#ccccff;
--platinum-font-ui:"Chicago","ChicagoFLF","Charcoal",system-ui,sans-serif;
--platinum-font-body:"Chicago","Charcoal",system-ui,sans-serif;
--platinum-font-mono:Monaco,"Andale Mono","Lucida Console",monospace;
```

The 219 `var(--…)` uses outnumber hard-coded hex colours roughly 3:1
across delta1.

## A3. libcss feature support audit

All paths relative to `browser/libcss/`.

### Properties fully supported (parsed AND selection-handled AND in `CSS_PROP_*` enum)

Evidence: presence in `include/libcss/properties.h` CSS_PROP_ enum,
matching file under `src/parse/properties/`, and matching file under
`src/select/properties/`.

| Feature | Notes |
|---|---|
| `display` (block / inline / inline-block / table etc.) | `CSS_DISPLAY_*` enum includes 21 values, incl. `FLEX`, `INLINE_FLEX`, `GRID`, `INLINE_GRID` (parse-level) |
| Flexbox properties | `flex-basis`, `flex-direction`, `flex-grow`, `flex-shrink`, `flex-wrap`, all have parse + select handlers |
| Flex alignment | `align-items`, `align-content`, `align-self`, `justify-content`, `order`, parse + select handlers present |
| `column-gap` | Parse + select (`select/properties/column_gap.c`) |
| `box-sizing` | Parse + select |
| `position: sticky` | `CSS_POSITION_STICKY = 0x5` in `include/libcss/properties.h:774`; parsed in `autogenerated_position.c:107`; select handler `select/properties/position.c` |
| `overflow-x`, `overflow-y` | Parse + select (shorthand `overflow` splits) |
| `opacity`, `fill-opacity`, `stroke-opacity` | Parse + select |
| `font-family`, `font-size`, `font-style`, `font-weight`, `font-variant` | Parse + select |
| `line-height`, `letter-spacing`, `word-spacing`, `text-indent`, `text-align`, `text-decoration`, `text-transform`, `white-space` | Parse + select |
| `color`, `background-color`, `background-image` (URL-only), `background-position`, `background-repeat`, `background-attachment` | Parse + select |
| Border (top/right/bottom/left × color/style/width), `border-collapse`, `border-spacing` | Parse + select |
| Margin / padding (four sides), `width`, `height`, `min-width`, `max-width`, `min-height`, `max-height` | Parse + select |
| Positioning (`top`/`right`/`bottom`/`left`), `float`, `clear`, `visibility`, `direction`, `unicode-bidi`, `writing-mode` | Parse + select |
| `list-style-type`, `list-style-image`, `list-style-position` | Parse + select |
| `column-count`, `column-fill`, `column-width`, `column-rule-*`, `column-span` | Parse + select |
| `cursor`, `content`, `counter-increment`, `counter-reset`, `quotes`, `outline-*`, `clip`, `z-index`, `table-layout`, `empty-cells`, `caption-side` | Parse + select |
| Print / paged: `page-break-before/after/inside`, `break-before/after/inside`, `orphans`, `widows` | Parse + select |
| Aural media (speak, azimuth, pitch, volume, etc.) | Parse + select, not relevant |

### `calc()` coverage

Parsed for a specific subset: `autogenerated_speech_rate.c`,
`autogenerated_margin_side.c`, `autogenerated_letter_spacing.c`,
`autogenerated_widows.c`, `autogenerated_z_index.c`,
`autogenerated_pause_after.c`, `autogenerated_padding_side.c`, and a
handful more. Resolution happens at the computed-style stage
(`include/libcss/computed.h:289`, "This will resolve `calc()`
expressions to used values"). MacTrove's four `calc()` uses are
against widths and spacing and should parse.

Evidence unit `CSS_UNIT_CALC = 0x1d` in `include/libcss/types.h:114`
(the "unresolved calc" unit) confirms the infrastructure exists.

### Properties / features NOT supported at all

Grep verified absent from `include/libcss/properties.h`, `src/parse/propstrings.c`, and property file lists:

| Feature | Status |
|---|---|
| **CSS custom properties / `var(--x)`** | No `CSS_PROP_CUSTOM_*`, no `css__parse_var`, no `var` in `propstrings.c`. The lexer will tokenize `var(` as a function token, but no property accepts it. Every declaration whose value is `var(--…)` parses as an invalid value and is **silently discarded** per the CSS parse rules. |
| **`border-radius` (all corners)** | No `CSS_PROP_BORDER_RADIUS`, `BORDER_TOP_LEFT_RADIUS`, etc. No parser file. No selection handler. Silently dropped. |
| **`box-shadow`** | No `CSS_PROP_BOX_SHADOW`. No parser. Silently dropped. |
| **`transform` / `transition` / `animation`** | Not in enum. Only `CSS_PROP_TEXT_TRANSFORM` matches the string "TRANSFORM". Animation has no `@keyframes` parser either (confirmed absent). Silently dropped. |
| **CSS Grid** | `CSS_DISPLAY_GRID` and `CSS_DISPLAY_INLINE_GRID` **are** in the enum and parse (`CSS_PROP_DISPLAY`), but **no grid track properties**: `grid-template-columns`, `grid-template-rows`, `grid-template-areas`, `grid-area`, `grid-column`, `grid-row`, `row-gap`, `grid-auto-flow`, etc., none in enum, none parsed. Silently dropped. |
| **`gap` shorthand / `row-gap`** | Only `CSS_PROP_COLUMN_GAP` exists. `gap:` shorthand is not parsed. All 76 MacTrove `gap:` uses drop silently. |
| **`object-fit` / `object-position`** | Not in enum. Silently dropped. |
| **Logical properties** (`margin-inline-start`, `padding-block-end`, `inset-inline`, etc.) | None in enum. Silently dropped. |
| **`font-variant-numeric` (`tabular-nums`)** | Only `CSS_PROP_FONT_VARIANT` (the CSS 2.1 shorthand). `font-variant-numeric` is a separate property; not present. Silently dropped. |
| **CSS gradients** (`linear-gradient`, `radial-gradient`, `conic-gradient`) | No `gradient` in `propstrings.c`, no parse function. A `background-image: linear-gradient(…)` declaration parses as an invalid image value and is **silently discarded**. |
| **`clip-path` / `mask*`** | Not in enum. Silently dropped. |
| **`overscroll-behavior*`** | Not in enum. Silently dropped. |
| **`user-select`, `-webkit-font-smoothing`** | Vendor-prefixed; not handled. |

### Takeaway, the two biggest gaps

1. **Custom properties.** 219 `var(--)` uses across delta1 all drop
   silently. The 19 theme tokens in `:root` are unreachable. That
   means every body/hero/window/sidebar/button/card rule that says
   `color: var(--platinum-text); background: var(--platinum-bg);
   font-family: var(--platinum-font-body)` falls through to UA
   defaults. This is the single most visually consequential gap.
2. **Grid → block.** Display `grid` parses correctly but maps to
   `BOX_BLOCK` in `browser/netsurf/content/handlers/html/box_construct.c:116`.
   Grid containers flatten into block layout, items stack
   vertically, grid-template-columns is ignored. Flex is in better
   shape (there's a real `BOX_FLEX` path).

## A4. What the layout engine actually consumes

Grep of `browser/netsurf/content/handlers/html/*.c` for
`css_computed_*` call sites (all files, not just `layout.c`):

### Properties `layout.c` reads

```
css_computed_border_{top,right,bottom,left}_{color,style,width}
css_computed_border_collapse
css_computed_border_spacing
css_computed_bottom / left / right / top
css_computed_box_sizing
css_computed_clear
css_computed_direction
css_computed_flex_wrap
css_computed_float
css_computed_font_size
css_computed_format_list_style
css_computed_height / width / width_px
css_computed_line_height
css_computed_list_style_type
css_computed_margin_{top,right,bottom,left}
css_computed_max_height / max_width / min_width
css_computed_overflow_x / overflow_y
css_computed_padding_{top,right,bottom,left}
css_computed_position
css_computed_style / style_destroy
css_computed_text_align / text_indent
css_computed_vertical_align
css_computed_white_space
```

### Additional properties read elsewhere in the HTML handler

```
layout_flex.c:        css_computed_flex_basis / flex_grow / flex_shrink / flex_wrap
layout_internal.h:    css_computed_flex_direction
                      css_computed_align_self / align_items     (via lh__box_align_self)
box_construct.c:      css_computed_display_static               (→ BOX_* type)
box_construct.c:      css_computed_content, content_item
redraw.c / friends:   css_computed_background_color / image / position / repeat
                      css_computed_color
                      css_computed_clip / clip_rect
                      css_computed_cursor
                      css_computed_font_family / font_style / font_variant / font_weight
                      css_computed_list_style_image
                      css_computed_text_decoration / text_transform
                      css_computed_visibility
```

### Consumption gap, properties libcss computes but layout never reads

| libcss computed | Read by layout? |
|---|---|
| `align_content` | **No**, only `align_items` / `align_self` are read (via `lh__box_align_self`) |
| `justify_content` | **No** call to `css_computed_justify_content` anywhere in the HTML handler |
| `order` | **No** call to `css_computed_order` |
| `column_gap` | **No**, the sole libcss gap property is unread; flex items have no inter-item gap |
| `opacity` | Not read by layout (cosmetic only) |
| `z_index` | Read indirectly via stacking contexts in redraw, not layout |
| `position: sticky` | `css_computed_position` is read but sticky is not special-cased, behaves as `static` in practice |

Conclusion: even the flex properties libcss *does* support are
**underconsumed by layout**. MacTrove uses `justify-content: space-between`,
`justify-content: center`, `align-content: flex-start`, and
`gap: 16px` extensively, none of these affect anything even if
parsed, because layout doesn't read them.

## A5. The MacTrove visual diff

**No committed screenshot of MacTrove rendering on the G3 exists in
the repo** (`find screenshots/` turns up only the generic MacSurf
demos from 2026-04-16). The user has observed the rendering but
nothing is checked in. This section is therefore a **predicted visual
diff** from the CSS and layout analysis in A1-A4, which should be
falsified by spot-checking the G3 screen when possible.

### Predicted divergences (from CSS analysis)

Given 219 var() uses drop silently and Grid maps to block:

**Backgrounds / colors**

- Body `background: var(--platinum-bg); ...` → no background colour set,
  falls to UA white. MacTrove's signature grey "desktop" is lost.
- Body `background-image: linear-gradient(...), linear-gradient(...)` → both
  gradients drop. The Mac-desktop scanline texture never appears.
- `.mac-menubar` uses literal `background:#DDDDDD` and literal fonts
  (`"Charcoal","Chicago"...`), **this one element** should render
  close to correct (minus font availability, see below).
- `.window / .software-card / .card-*` all reference
  `background: var(--platinum-window-bg)` etc., they lose their chrome
  fill and fall to UA default (transparent / body colour).
- Link colours (`color: var(--platinum-link)`) drop, links render in
  the UA default (usually blue, so might coincidentally match).
- Text colour (`color: var(--platinum-text)`) drops, body text renders
  in UA default (usually black, so coincidentally matches; but any
  muted-text class using `color: var(--platinum-muted)` loses that
  distinction and draws full-black).

**Typography**

- Body `font: 13px/1.45 var(--platinum-font-body)` → only the `13px/1.45`
  size+line-height parse; font-family is invalid and drops. Body text
  falls to UA default (serif/Times in NetSurf defaults). **Site-wide
  loss of the "Chicago" UI look.**
- The Tiny5 `@font-face` parses (libcss has `@font-face` support), but
  downloadable-font loading itself requires the content handler to
  actually fetch TTFs, decode, and register, MacSurf has no such
  path. The font silently falls back.
- `.hero__title { font-family: var(--platinum-font-ui); font-size: 22px; … }`
  → 22 px size survives, font-family drops. Large heading in UA font.
- `.mac-menubar { font-family: "Charcoal","Chicago", …; }` → literal
  list survives parse, computes to first-available font. None of
  those font names are installed by default on a stock OS 9 system;
  MacSurf's plotter maps family → generic `kFontIDGeneva` via
  `macos9_font_id_from_style` in [plotters.c](../../browser/netsurf/frontends/macos9/plotters.c).
  Geneva is sans-serif and vaguely-Mac-feeling; visible as a serif-vs-sans
  distinction from body default Times.
- `font-variant-numeric: tabular-nums` (observed on `.sidebar .menu .count`)
  silently drops, digits pack as proportional.

**Layout**

- Every `display: flex` container's children wrap via layout_flex.c.
  Items lay out in a row (or whatever `flex-direction:` says). BUT:
  `gap: 8px` / `gap: 16px` / `gap: 6px` → 76 occurrences drop silently.
  **Flex items touch each other with no spacing.** This is likely the
  "text overlap" the user reports in the toolbar / sidebar / hero area.
- `justify-content: space-between` / `center` drop (layout doesn't read).
  Flex items pack flush-start even when the design says distribute.
- `align-items: center` **does** apply (`lh__box_align_self` reads it).
  Good.
- `display: grid` → `BOX_BLOCK`. The 7 grid containers
  (`.category-grid`, `.doc-list`, `.featured-app__body`,
  `.home-docs__intro`, etc.) **collapse to block layout**. Every grid
  item stacks vertically in document order. `grid-template-columns:
  96px 1fr auto` is ignored.
- `column-gap: 1` between grid tracks, unread.
- `position: sticky` parses but no sticky-positioning layout exists in
  NetSurf's HTML layout (grep of layout.c confirms no `STICKY`
  special-case). The two sticky elements (likely the menubar and a
  sidebar) render as `static`. However `.mac-menubar` actually uses
  `position: fixed`, which libcss + layout do support, so the menubar
  should stick at the top.

**Cosmetic, universally missing**

- 30 `border-radius` declarations → all rectangles are sharp-cornered.
  Buttons, cards, inputs all square.
- 40 `box-shadow` declarations → no drop shadows or "Platinum" bevel
  highlight/shadow illusion. The classic-Mac bevel effect on windows
  and buttons disappears entirely.
- 10 `transform:` and 11 `transition:` drop, static layout (fine, no
  regression since nothing else is animating).
- 1 `object-fit` drops, images fill their box without aspect-ratio
  fitting; will stretch or crop oddly.

**Images**

- Every `<img>` renders as a placeholder box (per `MS_LOG("plot
  bitmap")` in [plotters.c](../../browser/netsurf/frontends/macos9/plotters.c)).
  The plotter path is implemented via GWorld blit, but the content
  layer has no PNG/GIF/JPEG decoder registered, image content
  handlers are all `#ifdef WITH_BMP`/`WITH_GIF`/`WITH_PNG` gated and
  none are linked. So the plotter's `plot_bitmap` never gets called
  with a decoded buffer. Classic empty-box-with-alt behavior.

### What should render close to correct

- `.mac-menubar` (top fixed bar), literal colors, literal font
  stack, `position: fixed` supported. Should look like a grey strip
  with nav items. Flex sub-items (`.mac-menubar__inner`,
  `.mac-menubar__item`) use flex + literal backgrounds + no gaps
  between items, the menubar spec already says `padding: 0 10px`
  per item, so no gap dependency. **This one component should be
  MacSurf's best-case render.**
- Basic `<h1>/<h2>/<h3>`, literal font-sizes apply; text stacks
  vertically in block flow.
- `<ul>/<li>`, block layout, bullets via `list-style-type: disc` default.
- `<a>`, coincidentally blue on UA default.
- Body text paragraphs, correct size, correct flow, wrong font.

### Hardware probes that would confirm

The A5 section becomes evidence-based as soon as the user sends a
screenshot or describes specific observations. Useful targeted probes
to add on hardware:

1. `MS_LOG_FMT("body color: %lu", (unsigned long)body_style_computed_color)` ,
   should show `0x00000000` (fell to UA default) if var() is dropping,
   not `0xFF000000` (would indicate var resolved).
2. `MS_LOG_FMT("flex gap=%d", css_computed_column_gap(flex_style, …))` ,
   confirm that libcss reports `CSS_COLUMN_GAP_NORMAL` (i.e., `gap:` never
   mapped onto column-gap anyway, it's a separate property).
3. Dump the computed `font-family` string for `body` via
   `css_computed_font_family` and log the first family name, expect
   the NetSurf UA fallback, not `Chicago`.

## A6. CSS pipeline trace, `.hero__title`

**Target element (from live fetch):**
```html
<h1 class="hero__title">A free archive of classic Macintosh software &amp; documentation.</h1>
```

**Matching author rules** (from `mactrove-delta1.css`, desktop
breakpoint, under `@media (max-width:720px)` and `(max-width:620px)`
smaller sizes override):

```css
.hero__title {
    font-family: var(--platinum-font-ui);
    font-size: 22px;
    line-height: 1.2;
    margin: 0 0 8px;
    color: #000;
}
```

**Inherited from ancestor `body`:**
```css
body {
    background: var(--platinum-bg);
    color: var(--platinum-text);
    font: 13px/1.45 var(--platinum-font-body);
    min-height: 100vh;
    background-image: linear-gradient(rgba(255,255,255,.5) 1px,transparent 1px),
                      linear-gradient(90deg,rgba(0,0,0,.03) 1px,transparent 1px);
    background-size: 2px 2px;
    padding-top: 23px;
}
```

**What libcss actually computes for the h1:**

| Property | Author value | Parsed? | Computed result (predicted) |
|---|---|---|---|
| `font-family` | `var(--platinum-font-ui)` | **No**, var() not supported | **Invalid** → declaration dropped → inherits from body |
| `font-size` | `22px` | Yes | 22 CSS-pixels (via `fpmath.h` double math, fine post-fixes114) |
| `line-height` | `1.2` | Yes | 1.2 × font-size = 26.4 px |
| `margin` shorthand | `0 0 8px` | Yes | top=0, right=0, bottom=8 px, left=0 |
| `color` | `#000` | Yes | `0xFF000000` (black, opaque) |

**What inherits from body:**

| Property | Author value on body | Parsed? | Inherited to h1 |
|---|---|---|---|
| `font-family` | `var(--platinum-font-body)` inside `font: 13px/1.45 var(--platinum-font-body)` shorthand | The shorthand fails to parse as a whole (var() unresolvable) → **entire `font` declaration drops** → font-family still UA default | UA default (serif / Times in NetSurf default styles) |
| `color` | `var(--platinum-text)` | No | UA default (black on white), coincidentally matches `.hero__title`'s own `color:#000` |
| `background-color` | `var(--platinum-bg)` (part of `background:` shorthand) | Shorthand drops | body stays transparent → body paints white from UA |
| `background-image` | Two `linear-gradient(...)` | **No**, gradient functions not supported | Declaration dropped |

**Net computed style for `.hero__title`:**

- Font: **UA default** (likely serif / Times), **not** Chicago/Charcoal.
- Font size: **22 px ✓**.
- Line height: **26.4 px ✓**.
- Margin-bottom: **8 px ✓**.
- Colour: **black ✓**.
- Background: transparent (inherits body, which is also transparent in
  the absence of `var()` resolution).

**Expected box-tree behavior (no hardware confirmation yet):**

- `box->type = BOX_BLOCK` via `ns_computed_display` (h1 default display
  block survives).
- Width = parent container width - margins.
- Height = one text line × 1.2 = ~27 px (the title is a single-line
  phrase that wraps to 2-3 lines depending on container width).
- `plot_text` called once per run of text at (x=left-padding,
  y=top-padding+font-ascent).

**Where rendering likely diverges from desktop reference:**

1. **Font face.** Desktop shows Chicago (via the theme); MacSurf
   shows Times (or whatever NetSurf's UA default font family is).
   Ascender/descender metrics differ, line heights differ in the
   inherited body rules.
2. **Position in layout.** The hero block sits inside a
   `<div class="hero">` that is a flex child of its parent. Flex
   arrangement works (flex-basis survives); but `gap:` between flex
   siblings drops, so the title hugs the next sibling with 0 px
   separation. Visually: the title's `margin: 0 0 8px` still provides
   its own 8 px, but no `gap:` from a parent, so anywhere the design
   relies on a flex-container-level gap to space things (instead of
   per-child margin), collapses.
3. **Width / wrapping.** `.hero__body { padding: 14px 22px }` applies.
   `.hero` has `max-width: 640px` in `.hero__lede`, not the title.
   Title width = hero container width minus padding. Wraps at natural
   width; should look OK except for font-rendering shift.

**Probes that would answer the hardware-specific parts:**

- `MS_LOG_FMT("hero-title color=%08lX size=%d lh_int=%d", color,
  size_px, line_height_int)` added inside `box_construct` when
  `box->node` has class `hero__title`, confirms the computed-style
  numerics.
- Log the actual `plot_text` call site (font_id, face, size, x, y,
  length) for the h1, confirms where the title lands on screen.
- Walk up `box->parent` chain and log `box->type` and `display`, confirms
  whether `<div class="hero">` survived as `BOX_FLEX` and whether the
  h1's ancestor chain is what we'd expect.

These probes are small additions (5-10 lines in `box_construct.c` with
a class-name check); no fix depends on them yet. They unblock
confident hypothesis verification before the extension work below.

## A7. CSS3 extension layer scoping

The long-term plan per architecture docs is a post-processing extension
layer on top of libcss, not a fork of libcss. This section scopes what
that layer would have to do to substantially improve MacTrove's render.

### Impact-ranked feature list for "substantially correct" MacTrove

1. **Custom properties (`var(--x)`)**, 219 uses. Without this, 3-of-4
   theme colours and the entire UI font cascade are unreachable.
   **Biggest single win.**
2. **`gap:` shorthand (and `row-gap`)**, 76 uses. Turns stacked flex
   items back into properly-spaced rows. Also removes the "text
   overlap" complaint immediately.
3. **`display: grid` → flex or block-with-spacing emulation**, 7
   containers. Most MacTrove grid templates are `96px 1fr auto`-style
   3-column layouts that translate cleanly to a 3-child flex row. A
   literal grid engine is NOT needed, a preprocessor can rewrite
   `display: grid; grid-template-columns: A B C; gap: N` into an
   equivalent flex layout with per-child flex-basis.
4. **`border-radius`**, 30 uses. Cosmetic but universally visible:
   buttons, inputs, cards all square without it. Implementation in
   the plotter: `plot_rectangle` can branch on a "corner radius"
   style hint and use `FrameRoundRect` / `PaintRoundRect` from
   QuickDraw (already Classic-Mac-native).
5. **`box-shadow`**, 40 uses. Same impact as border-radius for
   Platinum-look cards. Can be approximated with 2-3 offset
   `FrameRect` calls in darker / lighter shades, or skipped entirely
   and accepted as a visual regression.
6. **`linear-gradient` on backgrounds**, the body texture. Cosmetic;
   emulable by a tiled 2×2 pattern-bitmap rendered once.
7. **`justify-content`, `align-content`, `order`, `column-gap`** ,
   already *computed* by libcss but not *read* by layout. This is
   not extension work, this is direct layout.c/layout_flex.c
   modification: ~20-40 lines to read the existing accessors and apply
   during flex main-axis distribution.

### Cosmetic vs fundamental

**Cosmetic (sits on top of existing block/flex layout, no engine
change):**

- `border-radius`, plotter-level; render path is already QuickDraw.
  Implementation cost: small. Plumb a `radius` through the plot_style.
- `box-shadow`, plotter-level; emulate with offset FrameRects in
  muted colour. Implementation cost: small; or skip and mark as
  known regression.
- `linear-gradient` backgrounds, plotter-level; render to a cached
  small PixMap on first use and blit. Implementation cost: small for
  axis-aligned gradients only.
- `object-fit`, plotter-level when `plot_bitmap` gets a rect larger
  than the bitmap; already have all the info we need. Implementation
  cost: trivial once real image content exists to exercise it.

**Fundamental (requires layout engine changes):**

- **CSS custom properties.** Needs resolution at computed-style
  stage, before layout runs. Two strategies:
  1. **Proxy preprocessor**, the MacSurf proxy already strips TLS;
     extend it to also substitute `var()` references. Drupal 11
     already ships a compiled CSS bundle where `:root` variables are
     known up front, so a pass that maps `var(--platinum-bg)` →
     `#dddddd` inline is a straight regex-level textual substitution.
     Cost: small, ~100-200 lines of Go. Avoids all browser-side work.
     Preserves `--platinum-bg` naming clarity in source, substitutes
     once per page.
  2. **Browser preprocessor**, intercept the CSS fetch in the
     resource fetcher (`macos9_fetcher_stubs.c`) or in a new
     `macos9_css_fetcher.c`, and do the same substitution. Cost:
     small but requires a sax-style tokenizer for `:root { --x: Y; }`
     and `var(--x)` references. No dependency on proxy.

   **Strategy 1 is strongly preferred**, CSS preprocessing belongs
   at the network boundary where we already rewrite; keeps the
   browser stdlib-lean; correctly scoped for the "render-and-flatten"
   tier of the architecture.

- **`gap` shorthand, `row-gap`, and `gap` in flex/grid layout.**
  Needs NEW libcss enum entries (`CSS_PROP_ROW_GAP`, `CSS_PROP_GAP`
  shorthand parser), new parse files, new select handlers, new
  layout consumption in `layout_flex.c`. Alternatively, preprocessor
  rewrites `gap: N` to `margin-right: N` on non-last-children
  (approximation; loses `gap` in vertical direction unless rewritten
  per flex-direction). **Layout engine change is the clean fix;
  preprocessor rewrite is the fast-and-dirty workaround that covers
  80 % of cases.**

- **`display: grid` → layout.** Two roads:
  1. Preprocessor: rewrite `grid-template-columns: A B C` containers
     into `display: flex; flex-direction: row;` with each child
     annotated `flex: 0 0 A|B|C`. Works for all fixed-track grids,
     falls back for `auto`. Most MacTrove grid uses are fixed 3-col.
  2. Accept grid→block as graceful degradation. Stacked vertically
     is still readable. Items keep their content, just no 2D
     arrangement.

- **`justify-content` / `align-content` / `order` / `column-gap` in
  flex layout.** Not extension work, **hole in NetSurf
  layout_flex.c itself**. About 100 lines of well-understood flex
  spec to fill in. Has to happen in-tree, not via proxy.

### Graceful-degradation spec for fundamentally-missing features

If a preprocessor path isn't implemented, here is the fallback
behaviour MacSurf already exhibits and can be documented as "known
degraded" for v0.3:

- Grid → vertical block stack in document order.
- Custom properties unresolved → UA defaults (sites must be designed
  with non-var fallbacks, which MacTrove isn't).
- `gap:` → 0 (items touch).
- `border-radius` → square corners.
- `box-shadow` → no shadow.
- Gradients → transparent / body default colour.
- Transforms / transitions / animations → static.

### Implementation cost estimates (ordered by ROI)

| Item | Scope | LOC | Risk | Visual impact |
|---|---|---:|---|---|
| Proxy-side var() substitution | Go, single file | ~150 | Low (reversible, proxy-only) | **Massive** (unlocks 3 of 4 colour/font declarations) |
| Proxy-side `gap:` → margin rewrite | Go | ~80 | Low | **High** (fixes text overlap site-wide) |
| Proxy-side grid → flex rewrite | Go | ~100 | Low | Medium (fixes 7 containers) |
| Plotter `border-radius` via `PaintRoundRect` | C, `plotters.c` | ~30 | Low | Medium (all buttons/cards) |
| layout_flex.c: read `justify-content` / `align-content` / `column-gap` / `order` | C | ~60 | Medium (flex correctness) | Medium (proper distribution) |
| Plotter `box-shadow` approximation | C | ~50 | Low | Low (bevel is subtle) |
| Plotter gradient emulation (axis-aligned) | C | ~100 | Medium (GWorld lifecycle) | Low (body texture only) |
| Content handlers for PNG/GIF (unlock real images) | Add `content/handlers/image/*.c` | ~200+talloc bindings | High (new subsystem) | **Massive** (replaces every placeholder box) |

---

# Track B, Chrome verification plan

Track B is almost entirely hardware-gated. This section documents the
questions and the tests needed to answer them, not the answers
themselves, which must come from G3 observation.

## B1. Initial-window URL field status

**Question:** with content now rendering on the initial window, does
the URL field still fail to accept typing?

The 2026-04-18 survey's §1 hypothesis was: content redraw during
`browser_window_create` overdraws the URL rect. Overdraw made typing
"appear not to work", internally the TE handle was fine, but the
visible URL bar was blanked so the user saw no caret.

**Cannot answer from Linux source alone.** Test needed (in order):

1. Launch MacSurf, land on the initial window with MacTrove loaded.
2. Without clicking anything, press a key. Observe: does a character
   appear in the URL field? (expected: **no**, because
   `url_field_active` is still false at startup).
3. Click once inside the URL rect. Observe: does a caret appear?
   (expected: **yes**, the click sets `url_field_active=true` and
   calls `TEActivate`).
4. Type characters. Observe: do they appear?
5. Compare with File → New: opens a window, type immediately.
   Observe: does typing work without a click? If **yes**, the
   initial-window differ-at-startup bug still stands; if **no** in both
   cases, the bug is the gate on `url_field_active` itself (click
   required by design, which may or may not be a bug).

**If B1 step 3 shows a caret appearing but no character survives typing,**
that points at a `url_rect` geometry mismatch (click lands outside
TextEdit's internal rect). Log `gw->url_rect` at window-creation and
compare against the click coordinate passed to
`macos9_window_te_activate_url`.

**If B1 step 3 shows the caret but then content redraw blanks the URL
area,** the §1 hypothesis still holds. Add a probe in `plot_clip` /
`plot_rectangle` that logs coordinates when they intersect
`gw->url_rect` and fire once per first-redraw. Confirm the overdraw.

## B2. Seven chrome acceptance criteria, retest against MacTrove

For each, test on hardware now that real content loads:

| # | Criterion | Hardware test | Expected outcome |
|---|---|---|---|
| 1 | URL field accepts typed input | Click URL bar, type. | Works in File→New today; status on initial window is the open B1 question. |
| 2 | Return submits URL | Type URL, hit Return. | Assumed working in File→New (fixes130+ landed this). Verify it still works after loading MacTrove, i.e. type a second URL in the same window and navigate away. |
| 3 | Home button | Click Home. | Should navigate back to `MACSURF_HOME_URL` (`http://mac.mp.ls/`). |
| 4 | Reload button | Click Reload after MacTrove loads. | Re-fetch. Status bar should show "Fetching…" transiently. Final content should match. |
| 5a | Resize without freeze, idle | Grab grow box, drag, release on an idle MacTrove window. | No hang. Deferred-flag pattern (see CLAUDE.md) should prevent re-entrancy. |
| 5b | Resize during in-flight navigation | Click a link, immediately grow the window while fetch is in progress. | No hang; reformat happens after fetch completes. |
| 6 | Vertical scroll bar works | Load MacTrove (taller than window), drag scroll thumb. | Page scrolls. Content offset = thumb position × total height. |
| 7 | Arrow-key scrolling | Click in content area (not URL bar), press Down / PgDn. | Content scrolls. |

None of these can be answered from Linux source. The request is that
the user runs each test and reports `works` / `broken: description` /
`not tested`.

## B3. Scroll bar extents on MacTrove

**Questions:**
- What do `content_get_width(bw)` and `content_get_height(bw)` return
  once MacTrove has finished loading? (Expected: height ≫ window,
  width ≈ window.)
- Does the scroll bar's max/min align with content height minus
  visible-area?
- Does thumb-drag actually shift the content region?

**Hardware-only test.** To make this recordable without MacsBug,
add a one-shot probe on first successful `GW_EVENT_NEW_CONTENT`:

```c
MS_LOG_FMT("content wxh=%dx%d win h=%d",
           content_get_width(bw), content_get_height(bw),
           gw->content_rect.bottom - gw->content_rect.top);
```

(probe would live in `macos9_gw_event` handler where we already fire
`invalidate_all`). The title bar would show the three numbers; user
reads them off.

**If the scroll bar appears enabled (thumb draggable) but dragging
does nothing**, the bug is in the CDEF action routine
(`scroll_action`) not being wired to `browser_window_set_scroll`.
If the thumb is grey / not draggable, the range hasn't been updated
`SetControlMaximum` / `SetControl32BitMaximum` likely not called
after content arrives.

## B4. Status bar and title bar during navigation

**Questions**, all hardware-only:
- Title bar updates to MacTrove's `<title>` ("MacTrove, Classic
  Macintosh software archive | MacTrove") after load?
- Status bar shows NetSurf fetch progress during load ("Fetching
  [url]…", "Receiving data…", etc.)?
- Status bar clears after load completes?
- Hovering over a link (e.g. one of the category cards) updates
  the status bar with the target URL?

CLAUDE.md claims all four work. Verify on MacTrove specifically.

## B5. Button enable/disable states

**Questions**, all hardware-only:
- After load of MacTrove (first page): Back grey, Forward grey,
  Reload active, Home active?
- Click any MacTrove link to navigate; Back now active, Forward grey?
- Click Back: Back grey, Forward active?
- Does `macos9_window_update_button_states` fire after each
  navigation?

The `update_button_states` logic should already handle this correctly
given the history API (`browser_window_history_back/forward/has_content`).

## B6. Error handling for bad URLs

**Test:** type `http://notarealsite.mactrove.example/` and press
Return. Observe:

- Does fetch start (status bar flash "Fetching…")?
- Does OT `OTConnect` fail quickly (host resolution failure via proxy)
  or time out (currently OT has no explicit timeout)?
- Is any error shown to the user? (Current code in
  `macos9_http_fetcher.c` returns `NSERROR` up the chain; NetSurf
  renders an error page via the `about:` scheme usually, needs
  `about:` fetcher verification.)
- Can the user type a working URL afterward and recover?

**The OT endpoint is synchronous.** `do_ot_fetch` calls `OTConnect`
then `OTRcv` in a loop with no explicit timeout, `YieldToAnyThread`
lets the UI progress but the fetch itself blocks until OT signals
error. On a bad proxy response (502 Bad Gateway or similar) the fetch
should terminate naturally at first byte. On a DNS-failure at the
proxy, OT reports `kETIMEDOUTErr` or `kEAGAINErr` from `OTConnect`.

This is a known weak area worth verifying in a dedicated test round;
not blocking for v0.3.

---

# Cross-cutting, priority recommendations for the next execution round

The goal is to balance:
1. Make MacTrove render substantially better (Track A).
2. Make chrome fully usable (Track B).
3. Move toward apple.com / facebook.com viability (roadmap).

The most important finding from A1-A4 is that **~90 % of MacTrove's
cosmetic CSS goes through `var(--platinum-*)` references**, and libcss
silently drops all of them. No amount of layout work will improve
MacTrove rendering while 219 of its colour/font/background
declarations evaporate at parse time.

## Recommended sequence, "must ship next round"

### 1. Proxy-side CSS preprocessor: `var()` substitution

**Impact:** Biggest single render improvement possible. Unblocks
nearly everything the `platinum` theme specifies.

**Complexity:** **Small.** ~150 LOC of Go. One file in
`proxy/css_preprocess.go` hooked into the response pipeline for
`Content-Type: text/css` responses.

**Algorithm:**
1. Buffer the CSS response.
2. Scan for `:root { ... }` blocks; extract `--name: value;` pairs into
   a map.
3. Regex-replace `var(--name)` / `var(--name, fallback)` with the
   resolved value. Use fallback when name is undefined.
4. Pass through.

**Risk:** Low. Pure textual substitution; bug = CSS doesn't match source,
user sees a slight regression from the already-broken state, trivial
to revert.

**Prerequisites:** None. Ships as a proxy-only update.

**Acceptance:** body background shows grey, text colour logs as
`0xFF000000`, menubar font-family computes to literal Chicago/Charcoal
list, hero title computes to literal Chicago UI stack.

### 2. Proxy-side `gap:` rewrite

**Impact:** Fixes the "text overlap" user complaint across all 76
flex containers.

**Complexity:** **Medium.** ~80-120 LOC Go. Per-rule parse of
`display: flex` + `gap: N`, inject equivalent `margin-right: N` onto
subsequent-sibling selectors. Approximation, doesn't cover every
flex-direction case, but covers the common row-flex case.

**Risk:** Medium. If the rewriter miswrites a rule, it breaks layout
in subtle ways. Test on a handful of representative cards / toolbars.

**Prerequisites:** (1) above, keeps all CSS work on the proxy.

**Acceptance:** adjacent flex siblings visibly separated on G3.

### 3. Confirm / close the initial-window URL-field bug (B1)

**Impact:** User-reported, user-blocking for the "type a URL and go"
flow on the startup window.

**Complexity:** **Small.** Add one probe in `plot_clip` /
`plot_rectangle` that logs coordinates when they intersect
`gw->url_rect`, fires once per first redraw. Attach one probe in the
`macos9_handle_mouse_down` URL-rect detection. One build cycle. Read
MacsBug / title-bar output, confirm or refute hypothesis §1 from the
prior survey.

**Risk:** Very low, observation only, no behaviour change.

**Prerequisites:** None.

**Acceptance:** A definite yes/no on whether content-redraw overdraws
the URL rect.

## "Can wait one more round"

### 4. Fill flex alignment holes in `layout_flex.c`

Read `justify_content`, `align_content`, `order`, `column_gap` via
existing libcss accessors. ~60 LOC in `layout_flex.c` and
`layout_internal.h`. Follows the established pattern of
`lh__box_align_self`. Medium risk (flex correctness has edge cases);
medium reward (proper distribution). Ship after #1/#2 show the
preprocessor is working.

### 5. Image content handlers

Add `content/handlers/image/{gif,png,jpeg}.c` (NetSurf has these
upstream) and bring up whatever talloc plumbing they need. This is
the bottleneck flagged in CLAUDE.md's "most likely bottleneck"
section. Large effort (~500+ LOC, new subsystem interactions); huge
reward (every placeholder box becomes a real image). Defer to a
dedicated milestone.

### 6. Plotter-level `border-radius`

Plumb an optional `corner_radius` through `plot_style_t` (NetSurf
core change) OR, cleaner, detect uniform `border-radius` in the layout
box-build stage and stash on `box->style` or a MacSurf-local shadow
struct. Call `PaintRoundRect`/`FrameRoundRect` on the `plot_rectangle`
path. Low risk, small LOC, medium cosmetic reward. Ship after the
preprocessor has made the other rule values actually apply, a round
rect with no background colour is invisible.

### 7. `display: grid` → flex preprocessor

Most MacTrove grids are `96px 1fr auto` or `1fr 1fr 1fr` style, all
rewritable to `display: flex; flex-direction: row;` with per-child
`flex` basis. Needs more careful parsing than #1/#2; defer until the
must-ships have landed.

## Out of scope for next round

- `box-shadow`, `transform`, `transition`, `animation`, gradients,
  `clip-path`, `mask*`, cosmetic, not blocking comprehension.
- `font-variant-numeric: tabular-nums`, minor, numeric-column cosmetic.
- Logical properties (`margin-inline`, `padding-block`, …), MacTrove
  uses none of them anyway.
- apple.com / facebook.com viability, those sites depend on things
  MacTrove doesn't (CSS Grid at scale, complex JS, dynamic content).
  Not reachable without the image pipeline and the flex/grid work.

## Ranked "next fix zip" proposal

For the single next fix zip (numbered **fixes133** per CLAUDE.md and
per user's monotonic convention, user has already flagged the
number):

**Option A, maximum visual impact, minimum risk:**
- Proxy-side var() substitution (goes to `proxy/macsurf-proxy`, gets
  redeployed on the Hetzner box).
- No browser .zip needed.

**Option B, probe + proxy combo:**
- Proxy-side var() substitution AND the initial-window URL-field
  probe one-liners in the browser (fixes133.zip for the browser).
- Ship both in the same round.

**Option C, layout fix combo:**
- Proxy-side var() substitution + layout_flex.c alignment reads +
  URL-field probe (fixes133 browser zip for the layout work).

Option B is the sweet spot, unblocks the biggest render gap with
zero browser risk, bundles a cheap probe that will close out a
user-visible bug, and defers the intrusive layout work to a
fully-observed next round.

---

# Appendix, corrections to CLAUDE.md

Found one claim in CLAUDE.md that is slightly imprecise (but not
seriously wrong):

**§Rendering Pipeline:** "Full NetSurf pipeline executes: fetch → parse
→ CSS cascade → layout → plot." This is accurate in the sense that the
code paths exist, but **CSS cascade is running on a stylesheet with
~90 % of its declarations silently discarded** because of var() and
missing CSS3 properties. The pipeline completes without returning
errors but the computed-style output is far from the author's
intended design. No change needed to CLAUDE.md, it's a surface claim
about pipeline connectivity, not about fidelity, but the next time
the build state section is updated, it's worth noting "var()
substitution not yet implemented, causing widespread style loss on
modern themes" under Rendering Pipeline.

Everything else cross-checked matches: partition size (16 MB / 8 MB
confirmed in MacSurf.mcp), fetcher registration, no_backing_store
behaviour, plot_bitmap implementation (GWorld path), Duktape in base
build, etc.

No update to CLAUDE.md is proposed from this survey alone, the
render-fidelity note should land together with whatever fix ships from
the next round, not as a standalone change.
