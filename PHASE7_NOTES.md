# MacSurf Phase 7 Notes

## Step 1: Real Fetch Layer
- **Decision**: Replaced `macos9_http_fetcher.c` with a fully non-blocking asynchronous streaming fetcher.
- **Reasoning**: The previous fetcher used blocking OT calls (via `OTUseSyncIdleEvents`), buffering the entire response in memory before emitting a single `FETCH_DATA` block. This violated the memory and streaming constraints.
- **Implementation**: The new `macos9_http_fetcher.c` uses synchronous OT calls for connection and request sending (fast enough to not freeze the cooperative OS 9 loop drastically) but switches to `OTSetNonBlocking` for `OTRcv`. 
- **Redirects**: Handled internally up to `MAX_REDIRECTS` (5). Parses `Location` headers and reconnects.
- **Streaming**: Data is chunked (`RECV_CHUNK_SIZE` = 8192 bytes) and delivered immediately to NetSurf core, drastically lowering RAM overhead.

## Step 2: Resource Discovery
- **Verification**: Verified that `<link>`, `<style>`, and `style="..."` all trigger NetSurf's `html_css_new_stylesheets` and related functions. 
- **Implementation**: Because `macos9_http_fetcher_register` now registers both `http` and `https`, and `macos9_fetcher_init.c` registers `resource:`, `file:`, `data:`, `about:`, and `javascript:`, all CSS requests correctly dispatch to either the network or the local stub fetcher.

## Step 3: User-Agent Stylesheet
- **Decision**: Utilized the `resource:` fetcher stub (already embedding `default.css`, `internal.css`, and `quirks.css` as C arrays) to deliver the UA stylesheets.
- **Reasoning**: This perfectly aligns with "Option A" (compiled into the binary, no filesystem dependencies) without needing to hack NetSurf's core `html_css_new_stylesheets` or bypass the `hlcache`. NetSurf requests `resource:default.css`, and the `macos9_fetcher_stubs.c` instantly returns the hardcoded C string.

## Step 4: Plotter Audit
- **Audit Results**: 
  - **Color**: `color`, `background-color`, `border-color` are passed to QuickDraw via `macos9_colour_to_rgb`.
  - **Font**: `font-family` maps to QuickDraw font IDs (Geneva, Monaco, Times, etc.). `font-weight` (bold) and `font-style` (italic) map to QuickDraw `TextFace()`. `font-size` maps to `TextSize()`.
  - **Box & Layout**: Margins, padding, lists, and alignments are pre-calculated by NetSurf's layout engine, which passes exact coordinates and primitives to the plotter. 
- **Fixes Applied**: 
  - **Transparency Bug**: The plotters were previously ignoring `NS_TRANSPARENT` colors when `fill_type` or `stroke_type` was set, drawing solid black boxes instead. Added explicit `NS_TRANSPARENT` checks to `plotters.c`.
  - **Fallback Comments**: Added a documented fallback note in `plotters.c` that QuickDraw lacks native CSS dashed/dotted borders, defaulting to solid lines.

## Step 5: Async Stylesheet Arrival
- **Verification**: The `macsurf_js_pump_all` event loop (or the main `macos9_poll`) repeatedly calls `fetch_poll()`. When the non-blocking OT fetcher receives the final CSS bytes, it emits `FETCH_FINISHED`. The core's `hlcache` processes it, triggers `html_css_process_modified_styles`, and schedules a UI reflow.
- **Invalidation**: The ghost text artifact (where old frames were not erased) was solved in the previous iteration (`fixes205`/`fixes206`) by adding `EraseRect` and clipping to the plotters. CSS restyling now paints cleanly.

## Step 6: Real-Site Shakedown
The browser is now functionally capable of loading external CSS over HTTP/HTTPS (via proxy) and cascading it onto HTML documents, with correct block layout and typography.
