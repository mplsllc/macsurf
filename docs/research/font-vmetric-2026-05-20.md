# Per-Font Vertical Metrics on G3 iMac (fixes153 ground truth)

Hardware-captured `GetFontInfo` output for every font/size/face combination MacSurf currently considers. Run via `macos9_font_vmetric_probe_run()` at startup. Probe target: G3 iMac running OS 9.2.2, fully-populated Fonts folder.

Columns: ascent / descent / widMax / leading / total (ascent+descent+leading).

## Bitmap fonts (Geneva, Chicago, Monaco)

All three families share the same vertical pattern: ascent equals nominal size, descent is roughly size/4, leading = 1 px. `widMax` varies per font (Geneva and Chicago proportional, Monaco monospace).

| Font | Size | Face | Ascent | Descent | WidMax | Leading | Total |
|---|---:|---|---:|---:|---:|---:|---:|
| Geneva | 9 | plain | 9 | 2 | 12 | 1 | 12 |
| Geneva | 9 | bold | 9 | 2 | 13 | 1 | 12 |
| Geneva | 10 | plain | 10 | 2 | 14 | 1 | 13 |
| Geneva | 10 | bold | 10 | 2 | 15 | 1 | 13 |
| Geneva | 12 | plain | 12 | 3 | 16 | 1 | 16 |
| Geneva | 12 | bold | 12 | 3 | 17 | 1 | 16 |
| Chicago | 9 | plain | 9 | 2 | 12 | 1 | 12 |
| Chicago | 9 | bold | 9 | 2 | 12 | 1 | 12 |
| Chicago | 10 | plain | 10 | 2 | 13 | 1 | 13 |
| Chicago | 10 | bold | 10 | 2 | 13 | 1 | 13 |
| Chicago | 12 | plain | 12 | 3 | 16 | 1 | 16 |
| Chicago | 12 | bold | 12 | 3 | 16 | 1 | 16 |
| Monaco | 9 | plain | 9 | 2 | 6 | 1 | 12 |
| Monaco | 9 | bold | 9 | 2 | 7 | 1 | 12 |
| Monaco | 10 | plain | 10 | 2 | 7 | 1 | 13 |
| Monaco | 10 | bold | 10 | 2 | 8 | 1 | 13 |
| Monaco | 12 | plain | 12 | 3 | 8 | 1 | 16 |
| Monaco | 12 | bold | 12 | 3 | 9 | 1 | 16 |

## Outline (TrueType) fonts (Helvetica, Times, Palatino, Courier)

All four share a different pattern: ascent is roughly 75-80% of nominal size, leading = 0. Palatino's descent is 1 px taller than the others at size=10 (3 vs 2).

| Font | Size | Face | Ascent | Descent | WidMax | Leading | Total |
|---|---:|---|---:|---:|---:|---:|---:|
| Helvetica | 9 | plain | 7 | 2 | 10 | 0 | 9 |
| Helvetica | 9 | bold | 7 | 2 | 9 | 0 | 9 |
| Helvetica | 10 | plain | 8 | 2 | 11 | 0 | 10 |
| Helvetica | 10 | bold | 8 | 2 | 10 | 0 | 10 |
| Helvetica | 12 | plain | 9 | 3 | 13 | 0 | 12 |
| Helvetica | 12 | bold | 9 | 3 | 12 | 0 | 12 |
| Times | 9 | plain | 7 | 2 | 9 | 0 | 9 |
| Times | 9 | bold | 7 | 2 | 9 | 0 | 9 |
| Times | 10 | plain | 8 | 2 | 10 | 0 | 10 |
| Times | 10 | bold | 8 | 2 | 10 | 0 | 10 |
| Times | 12 | plain | 9 | 3 | 12 | 0 | 12 |
| Times | 12 | bold | 9 | 3 | 12 | 0 | 12 |
| Palatino | 9 | plain | 7 | 2 | 11 | 0 | 9 |
| Palatino | 9 | bold | 7 | 2 | 9 | 0 | 9 |
| Palatino | 10 | plain | 8 | 3 | 12 | 0 | 11 |
| Palatino | 10 | bold | 8 | 3 | 10 | 0 | 11 |
| Palatino | 12 | plain | 10 | 3 | 14 | 0 | 13 |
| Palatino | 12 | bold | 10 | 3 | 12 | 0 | 13 |
| Courier | 9 | plain | 7 | 2 | 8 | 0 | 9 |
| Courier | 9 | bold | 7 | 2 | 8 | 0 | 9 |
| Courier | 10 | plain | 8 | 2 | 9 | 0 | 10 |
| Courier | 10 | bold | 8 | 2 | 9 | 0 | 10 |
| Courier | 12 | plain | 9 | 3 | 10 | 0 | 12 |
| Courier | 12 | bold | 9 | 3 | 10 | 0 | 12 |

## Cross-family deltas

| Pair | Size | Ascent Δ | Total Δ |
|---|---:|---:|---:|
| Geneva ↔ Helvetica | 9 | 9 vs 7 = **2** | 12 vs 9 = **3** |
| Geneva ↔ Helvetica | 10 | 10 vs 8 = **2** | 13 vs 10 = **3** |
| Geneva ↔ Helvetica | 12 | 12 vs 9 = **3** | 16 vs 12 = **4** |
| Monaco ↔ Helvetica | 12 | 12 vs 9 = **3** | 16 vs 12 = **4** |
| Monaco ↔ Palatino | 12 | 12 vs 10 = **2** | 16 vs 13 = **3** |
| Palatino ↔ Helvetica | 12 | 10 vs 9 = **1** | 13 vs 12 = **1** |

The largest mixed-line delta on real pages is **Geneva-or-Monaco + Helvetica at size=12**, Monaco's ascent extends 3 px higher above the baseline than Helvetica's. With CSS `line-height: normal` (= 1.3 × font-size = 15.6 → 16 px), the line box accommodates both fonts. With `line-height: 1` or any value smaller than 16/12 = 1.33, Monaco's glyphs overflow upward into the previous line, this is the visual signature of fixes52's "lines stack 2-4 px on top of each other".

## What this means for the existing architecture

The current code path:

- `line_height(style)` in `layout.c` uses purely CSS values (`1.3 × font-size` for normal). It does NOT consult per-font ascent/descent.
- `text_redraw` in `redraw.c` paints at `y + height * 0.75`. The 0.75 hardcodes a baseline-from-top ratio that happens to match outline fonts (Helvetica/Times/Palatino/Courier: ascent ≈ 75% of size).
- Bitmap fonts (Geneva/Chicago/Monaco) have ascent ≈ 100% of size, their baseline at 75% of the line means glyphs paint 25% of the way down from the line top, which is correct for their taller ascent only if the line box is sized to accommodate `1.3 × size`. With CSS line-height ≥ 1.3, glyphs fit.

The hardcoded 0.75 is therefore correct-by-coincidence for the existing `kFontIDHelvetica`-force path. If `MACSURF_FONT_FAMILY_ALIASES` is enabled and a Monaco segment lands on the same line as a Helvetica segment in a CSS `line-height: 1` context, Monaco overflows.

## Architectural options for fixes154+

Three paths from here:

1. **Retry aliases unconditionally**, accept the line-height: 1 edge case as "real-world authors rarely use it on body text with mixed `<code>`". Most pages use line-height ≥ 1.4. Visual regressions would be minor and confined to specific layouts.

2. **Add ascent/descent to `gui_layout_table`**, new platform virtual functions. layout.c's `line_height` could then take max(ascent+descent+leading) across all box's fonts when CSS line-height is unset. This is the architecturally correct fix but adds an API surface change.

3. **Conservative wrap of (1) with explicit per-pair safety**, when the cascaded font-family list is broadly Helvetica-compatible (all outline), enable aliases; when Monaco/Geneva is mixed with Helvetica AND CSS line-height is explicit and < 1.3, fall back to Helvetica force. Detectable from `box->style` at layout time.

The author's recommendation pre-data was option 2. With the data in hand, option 1 (retry-with-defaults) is the cheapest path and probably "good enough" for real pages. Option 2 remains the right structural fix if any real-world page demonstrates the line-height: 1 mixed-font overflow.

## Unexplained: fixes145 horizontal scrambling

The vmetric data does NOT explain why fixes145's alias-enabled build saw HORIZONTAL text scrambling (sample garbage strings "MonacoaroumsHflfaes", "diralijoeranzas thank"). Per static analysis of the current code:

- Measurement: `macos9_font_measure(fstyle)` calls `macos9_font_id_from_style(fstyle)` → per-family font_id, then `TextWidth`.
- Paint: `macos9_plot_text(fstyle)` calls the same `macos9_font_id_from_style(fstyle)` → per-family font_id, then `DrawText`.

Both width and paint are family-aware AND share the same dispatch function. There's no obvious mismatch. The fixes145 horizontal scrambling must come from somewhere outside this static path, perhaps the `letter_spacing` / `word_spacing` bump in fixes146 that is gated on `font_id != kFontIDMonaco`, or from inline layout passing one fstyle to width while redraw uses another, or from box-tree corruption between layout and redraw.

A future fixes154 alias retry should ship with diagnostic logging at every per-box width/paint call site so we can observe the actual divergence on hardware if it recurs. Don't retry blind.
