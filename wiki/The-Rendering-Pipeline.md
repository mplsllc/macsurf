# The Rendering Pipeline

This page traces what happens between typing a URL and seeing a page draw: fetch, HTML parse, CSS cascade, layout, and the QuickDraw plot to screen. It's for anyone who wants to understand how MacSurf turns the modern web into pixels on a 1999 Mac, or who wants to add a rendering feature and needs to know where their code fits.

If you want the higher-level map of the whole system, start with the [Architecture Overview](Architecture-Overview). This page zooms into the part that turns bytes into pixels.

## The shape of it

MacSurf is a fork of [NetSurf](https://www.netsurf-browser.org/about/), a lightweight browser engine written entirely from scratch in C (no WebKit, no Gecko). NetSurf was built to be portable and to run on constrained hardware, which is exactly why it's a sensible base for a Power Mac G3. The engine splits cleanly into a portable core plus per-platform *frontends* — and MacSurf is a new frontend, `macos9`, hung off that core. The whole pipeline below is NetSurf's, running natively on the Mac, with the final draw step rewritten in QuickDraw.

A page moves through five stages:

1. **Fetch** — pull the bytes over the network (HTTP/1.1, HTTPS via [macTLS](Networking-and-TLS)).
2. **HTML parse** — turn the byte stream into a DOM tree (libhubbub + libdom).
3. **CSS cascade** — parse stylesheets, resolve `var()`, and compute a style for every element (libcss).
4. **Layout** — walk the DOM + computed styles and assign every box an x, y, width, and height.
5. **Plot** — paint the laid-out boxes to the screen with QuickDraw, through an offscreen buffer.

Each stage hands its output to the next. The interesting thing about doing this on OS 9 is that none of it can block: the machine is cooperatively multitasked, so a long network wait has to yield the CPU back to the system by hand. The fetch stage handles that (see [Networking & TLS](Networking-and-TLS)); the rest of the pipeline runs to completion once the bytes are in.

## Stage 1 — Fetch

A navigation starts when the address bar hands a URL to NetSurf core, which normalizes it (prepending `http://` if there's no scheme) and kicks off a fetch. MacSurf registers fetchers for the `http:` and `https:` schemes; HTTPS goes straight to the origin server over TLS 1.3 using macTLS, with no intermediary. There is no proxy — the Mac does the TLS itself.

The networking details (Open Transport, the cooperative-yield model, chunked transfer, keep-alive, redirects, the CA bundle) all live on the [Networking & TLS](Networking-and-TLS) page. What matters for the pipeline is the handoff: the fetcher delivers the response body to NetSurf's content system, which sniffs the content type and routes it to the right handler. HTML goes to the HTML content handler, which owns the next three stages.

## Stage 2 — HTML parse (libhubbub + libdom)

NetSurf doesn't hand-roll an HTML parser. It uses two of its standalone libraries, both ported to compile under CodeWarrior 8's strict C89:

- **libhubbub** is an HTML5-compliant tokenizer and tree builder. It takes the raw byte stream and produces parse events — "start tag `div`", "text node", "end tag" — following the HTML5 parsing algorithm, including the error-recovery rules that make real-world tag soup render the way browsers have agreed it should.
- **libdom** is a C implementation of the W3C DOM. It consumes libhubbub's events and builds the actual tree of nodes that the rest of the engine (and JavaScript) walks.

Both are MIT-licensed NetSurf sub-projects that version independently of the browser, which is part of why they were portable in the first place (see the per-repo READMEs: [Hubbub](https://www.netsurf-browser.org/projects/), [libdom](https://github.com/netsurf-browser/libdom)). Underneath them sits **libwapcaplet**, a reference-counted string-interning library, so the millions of repeated strings in a document (tag names, class names, attribute names) collapse to single shared, fast-to-compare references — a real memory win when you have 16 MB to work with.

The output of this stage is a DOM tree. The [JavaScript engine](The-JavaScript-Engine) attaches to this same tree, which is how scripts can read and mutate the page.

## Stage 3 — CSS cascade (libcss, native `var()`)

**libcss** is NetSurf's CSS parser and selection engine. This is the largest of the ported libraries (a few hundred source files) and the one MacSurf has invested in most heavily, because CSS is where modern pages either render correctly or fall apart.

The cascade does three jobs. It parses every stylesheet — the page's own, any `@import`ed sheets, and MacSurf's built-in user-agent sheets (`resource:default.css` and friends, served by the resource fetcher). It matches selectors against the DOM, applying specificity and `!important` the way the spec requires. And it composes the result down to one *computed style* per element: the final, resolved value of every property after inheritance and the cascade have had their say.

CSS custom properties — `var()` and the `--foo` definitions behind them — are resolved here, **natively in libcss, at cascade time**, by token substitution. This was a deliberate architectural choice. The fast-looking shortcut would have been to preprocess `var()` away before the browser ever saw it, but that would have meant the browser never actually understood custom properties, and every future CSS feature that interacts with them would have been blocked. So MacSurf taught libcss to do it properly. The keystone was a lexer fix: stock libcss tokenized `--foo` as two separate tokens (the CSS 2.1 grammar only expected one leading dash, for vendor prefixes), so a custom-property-heavy modern theme dropped every `:root` definition and every `var()` reference before any parser logic even ran. Once the lexer learned that `--` followed by an identifier character is a single token, native custom properties worked. (The fix lives in `browser/libcss/src/lex/lex.c`.)

The computed style for each element is what layout reads next.

### FULL vs PARSED_NOT_CONSUMED

Here's the honest part. libcss parses far more CSS than the layout and paint engine actually *uses*. A property can be in one of a few states:

- **FULL** — parsed, cascaded, and consumed by layout/redraw. It works, and there's a probe page that proves it on hardware.
- **PARTIAL** — it works, with documented limits.
- **PARSED_NOT_CONSUMED** — libcss parses and cascades it correctly, but no layout or paint code ever reads the value back. It accepts your CSS without complaint and then silently does nothing. This is the category that produces the "this page looks slightly wrong" bugs.
- **MISSING_PARSER** — libcss doesn't even parse it; the value is discarded at parse time.

As of the current tree libcss parses on the order of **190 properties** and exposes around **130 computed accessors**, about **110 of which** are read back somewhere in `browser/netsurf/` — leaving roughly **20 accessors with no consumer**. (Counted at the property level rather than the accessor level, around 150 properties land in layout or redraw.) The handful of accessors that parse but aren't read are the silent-fail list: `unicode-bidi`, `writing-mode`, the print/paged properties (`break-*`, `page-break-*`, `orphans`, `widows`), `caption-side`, `table-layout: fixed`, and a few SVG-only properties like `fill-opacity` / `stroke-opacity`. None of them block real-site rendering; they're tracked and picked off over time.

The full, property-by-property breakdown — what's FULL, what's PARTIAL with which limits, what's PARSED_NOT_CONSUMED — lives in two living documents that are the ground truth, not this page: [`CSS_SUPPORT_MATRIX.md`](https://github.com/mplsllc/macsurf/blob/master/CSS_SUPPORT_MATRIX.md) (the curated status table) and [`docs/css-status.md`](https://github.com/mplsllc/macsurf/blob/master/docs/css-status.md) (the audit narrative with the fix-round history). If you're about to implement or rely on a property, check those first.

### What's actually implemented

The list of CSS that renders correctly on hardware is long, and it goes well past the CSS 2.1 era that stock NetSurf targets. The headline features:

- **Flexbox** — `flex-direction`, `flex-wrap`, `flex-grow/shrink/basis`, `justify-content` (all six values), `align-content`, `align-items`, `align-self`, `order`, and single-value `gap`. This is a complete enough flex implementation to lay out most modern card-and-column designs.
- **CSS Grid (V1 + V2)** — the track grammar (`fr`, `repeat()`, `minmax()`, `fit-content()`), `grid-template-columns` and `grid-template-rows`, explicit placement (`grid-column`, `grid-row`, and the `grid-area` shorthand), `grid-template-areas` name lowering, auto-flow occupancy avoidance so unplaced items flow around explicitly-placed ones, and `align-items`/`align-self`. There are real limits — `justify-*` is constrained, no subgrid, a maximum of 8 tracks per axis and 256 children per grid — but a `200px 1fr` sidebar-plus-content layout works the way the author intended.
- **Multi-column (V1)** — `column-count`, `column-width`, `column-gap`, and painted `column-rule-*` dividers.
- **Visual polish** — `border-radius` (via QuickDraw's `PaintRoundRect`/`FrameRoundRect`), `box-shadow` (independent horizontal/vertical offsets and a custom color; blur radius is dropped to zero because QuickDraw has no blur), `opacity` (approximated with QuickDraw stipple patterns at 1.0 / 0.80 / 0.50 / 0.25), linear and radial gradients, `text-shadow`, and `transform` (`rotate`/`translate`/`scale`/`matrix`, 2D only — QuickDraw is a 2D engine).
- **Generated content** — CSS counters (decimal), `content:` strings, and `::before`/`::after` pseudo-elements.
- **Modern units and sizing** — viewport units (`vh`, `vw`, `vmin`, `vmax`), `aspect-ratio`, `object-fit` and `object-position` for replaced images, `background-size` for bitmap backgrounds, the `inset` shorthand, and a vertical-only `position: sticky` (V1).
- **z-index and stacking** — CSS 2.1 Appendix E painting order at the sibling level, with stacking contexts triggered by positioned elements, `position: fixed`, `opacity < 1`, and transforms.

A few of these are worth a note on *how* they got there. Several modern standard properties — `text-shadow`, standard `transform`, `inset` — are bridged into MacSurf's own vendor properties by a CSS preprocessor step (`cssh_css`) before libcss sees them, which let the features ship without rewriting the layout engine to understand every new grammar from scratch. Standard `transform` lives behind that bridge specifically because an earlier attempt to alias it directly inside libcss hung the engine; the preprocessor route was the safe path.

## Stage 4 — Layout

Layout is where the DOM tree plus the computed styles become a *box tree* with real geometry. NetSurf builds a tree of boxes (block, inline, table-cell, flex item, grid item, and so on), then walks it to assign each box a position and a size: x, y, width, height, margins, the works. Floats are placed, flex items are distributed along their main axis, grid items are dropped into their tracks, inline runs are broken into lines.

This stage is also where the platform's quirks bite. QuickDraw's coordinate space is signed 16-bit (roughly ±32767), so a box that the CSS engine briefly computes as enormous or negative — `box->x = 30728`, a descendant bottom of `-39845888` — is real garbage that has to be caught before it reaches the painter. MacSurf has a defensive clamp in `redraw.c` that zeroes box fields outside a sane range. The threshold matters more than it looks: it was originally ±10000, which was fine until a tall test page legitimately grew past 10000 pixels and the clamp started nuking *real* content, rendering pages blank. It's now ±200000 for the y/height axis (x and width stay narrow because no real page is that wide). The lesson — pick clamp thresholds orders of magnitude past the largest realistic value, not barely past it — is one of several captured on the [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas) page.

Layout is re-run (a *reformat*) whenever something changes that affects geometry: a stylesheet or image arrives mid-load, the window is resized, or a `:hover` rule re-cascades. Resize reformats go through a deferred-flag pattern rather than firing synchronously, because calling reformat straight from the window's grow handler causes a re-entrant layout loop. There's also a re-entrancy guard that bails if a reformat arrives while one is already running.

## Stage 5 — Plot (QuickDraw plotters, offscreen GWorld)

The last stage paints the laid-out box tree to the screen. NetSurf abstracts drawing behind a *plotter* table — a set of function pointers for "draw text", "fill a rectangle", "set a clip region", "blit a bitmap" — and each frontend supplies its own. MacSurf's plotters are implemented in QuickDraw, the classic Mac 2D graphics API, in `plotters.c`.

The plotting does not draw directly to the window. It draws into an **offscreen GWorld** — a QuickDraw offscreen graphics buffer — and the finished frame is copied to the window in one shot. Drawing offscreen and compositing avoids the flicker you'd get painting a complex page element-by-element on a visible window, and it's how compositing-style effects (opacity, transforms) get a surface to work on before the result lands on screen.

That offscreen step introduced one subtle, recurring trap worth knowing about. NetSurf's plotters originally figured out which window they were drawing for by reading the current QuickDraw port and pulling a back-pointer off it. That works fine when you paint straight to the window, but the moment any code switches the current port to the offscreen GWorld mid-redraw, that back-pointer read returns garbage, every clip region collapses to empty, and the page renders blank with every log counter looking perfectly normal. The fix is a helper, `macos9_find_gw_for_plot()`, that reads a global set around the redraw call and only falls back to the port trick. Any new code path that changes the current port has to route through that helper.

### The colorizing-copy hazard

There's one QuickDraw behavior that bites hard enough to deserve a flag here, even though the full write-up lives on the [CodeWarrior 8 & C89 Gotchas](CodeWarrior-8-and-C89-Gotchas) page. QuickDraw's `CopyBits`/`CopyMask` blit routines *colorize* the transfer with the port's current foreground and background colors — even a 32-bit-to-32-bit copy is tinted toward whatever `RGBForeColor` was last set. Because MacSurf draws blue link text by setting the foreground to blue, an image blitted right afterward without resetting the color comes out tinted blue (or washed out, depending on the leftover color). It looks like a decoder bug — "images go blue after I scroll" — but the decode is perfect; it's the paint. The rule, for every blit in the frontend, is to reset foreground to black and background to white immediately before `CopyBits`/`CopyMask` unless you specifically want colorizing. If you add any new QuickDraw blit anywhere, assume the port's color is whatever the last text or rectangle draw left it.

### Images

Image decoding feeds the plot stage with bitmaps. MacSurf supports the formats that matter for the real web:

- **PNG** — decoded with **lodepng**, and it's the one format with *real* per-pixel alpha. Transparent PNGs composite correctly against whatever's behind them.
- **GIF, BMP, TIFF, JPEG** — all decode and render. (NetSurf's own libnsgif and libnsbmp handle GIF and BMP; libjpeg and libtiff cover the rest.) TIFF currently renders opaque only — the alpha-aware path needs a 32-bit-ARGB GWorld that classic QuickDraw refuses to allocate on OS 9 (`QTNewGWorld` returns `cDepthErr` for that pixel format).
- **Inline SVG (V1)** — a DOM-walking renderer paints `<rect>`, `<circle>`, `<ellipse>`, `<line>`, `<polygon>`, `<polyline>`, a useful subset of `<path>` (the M/L/H/V/C/Q/Z commands), and `<g>` group inheritance, with `viewBox` and width/height resolving correctly. This covers most page-chrome icons and logos. Gradients, `<text>`, `<use>`, `transform=`, and arc path commands are deferred to V2, and external `.svg` files (`<img src="*.svg">`) aren't decoded at all yet.

There's a known performance note here: JPEG photos are slow to re-blit while scrolling, because the image is composited at full resolution on every paint rather than pre-scaled at decode time.

## The pipeline doesn't run once

The five stages aren't a straight line you walk one time. A first load runs fetch → parse → cascade → layout → plot, but plenty of things kick the back half of that chain off again. A late stylesheet or an image that streams in mid-load forces a re-cascade and a *reformat* (a fresh layout pass). Resizing the window reformats. Hovering a link can re-cascade if a `:hover` rule changes geometry. Each of those repaints the affected boxes into the GWorld and copies the result back to the window — without re-fetching what's already in hand.

It's a lot of machinery to run in 16 MB on a 233 MHz processor, and the remarkable thing isn't that some properties are still PARSED_NOT_CONSUMED — it's how much of the real, modern web comes out looking right. The gaps are tracked openly; the [Contributing & Expanding](Contributing-and-Expanding) page walks through how to close one.

## Sources

- NetSurf — engine overview, "written from scratch," GPL-2.0 core, portability goal: <https://www.netsurf-browser.org/about/>
- NetSurf sub-projects (libparserutils, libwapcaplet, hubbub, libcss, libdom, plus the image decoders): <https://www.netsurf-browser.org/projects/>
- libcss (CSS parser + selection engine), libdom (C DOM), libwapcaplet (string interning) — per-repo READMEs and MIT licensing: <https://github.com/netsurf-browser/libcss>, <https://github.com/netsurf-browser/libdom>, <https://github.com/netsurf-browser/libwapcaplet>
- In-repo CSS status (ground truth for FULL / PARTIAL / PARSED_NOT_CONSUMED): `CSS_SUPPORT_MATRIX.md` and `docs/css-status.md`
