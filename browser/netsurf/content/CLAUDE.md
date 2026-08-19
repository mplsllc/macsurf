# browser/netsurf/content — fetch/parse/layout/redraw pipeline

Core NetSurf content pipeline, heavily patched for MacSurf. `ns_content.c` is the fetch/
convert orchestrator; `handlers/html/` has layout (`layout.c`) and redraw (`redraw.c`);
`handlers/css/cssh_css.c` is the MacSurf-specific CSS preprocessor bridge in front of
`browser/libcss`.

## CSS preprocessor bridge (`handlers/css/cssh_css.c`)

- **The grid-alignment bridge (`macsurf__rewrite_grid_alignment`) must EMIT a property for
  real once libcss gains native support for it, not go on shadowing it with an
  approximation.** It historically shadowed both `justify-items` and `justify-self` with a
  `text-align` substitute because neither was a real libcss property yet; when
  `justify-items` became real, the bridge had to start emitting the actual declaration.
  **Every emitted declaration needs its own `"; "` terminator** — without the separator,
  the emitted text fuses with the next declaration into something libcss silently rejects
  as invalid (e.g. `justify-items: centertext-align: center`). `justify-self` is still
  shadow-only pending its own native property.

## Layout / redraw (`handlers/html/`)

- **Defensive clamp thresholds in `html_redraw_box` (`redraw.c`) zero any box field that
  looks like layout-engine garbage.** Pick thresholds with headroom orders of magnitude
  above realistic content size, not just "bigger than the one garbage value that prompted
  the clamp." A page that legitimately grows taller than the current threshold gets its own
  real height clamped to zero, and redraw silently stops past the first block — this reads
  exactly like a missing-content bug, not a threshold bug, so check the clamp before
  chasing the tree.
- **Every box is born with `width = UNKNOWN_WIDTH` (`INT_MAX`) — don't confuse this with
  `AUTO` (`INT_MIN`), they're different sentinels for different states.** A layout path
  that tolerates a failed sub-layout and resets the degraded subtree's height to 0 but
  leaves width at `UNKNOWN_WIDTH` blows the whole document's content width out to
  `INT_MAX` once that box's extent reaches the root. Any new "tolerate a failed sub-layout"
  path must reset the degraded subtree's WIDTH too, not just its height — this bug class is
  X-axis-only for exactly that reason.
- **A text box split with a tiny remaining line width should return split-offset 0
  ("cannot split here"), not a mid-word offset.** Core then force-fits the word if it's
  first on the line, char-breaks it under `overflow-wrap`, or wraps it whole — whichever is
  correct for that case. If you gate a core char-break fallback, gate it to the first box on
  a line (`inline_count == 1`); otherwise no-space tokens (usernames, URLs) landing on a
  partial line get broken mid-word instead of wrapping whole to the next line.

## Fetch lifecycle (`ns_content.c`)

- **A subresource fetch that aborts partway (e.g. hitting a size cap) must still call
  `content_broadcast_error` from `content_convert`/`content_llcache_callback`.** Without it
  the parent document's active-fetch counter never reaches zero, and the page hangs waiting
  on a fetch that already gave up — visible as the browser stalling with a non-zero fetch
  count and no further progress. **A different root cause for the same symptom** — a custom
  fetcher that doesn't call both `fetch_remove_from_queues`/`fetch_free`, or doesn't check
  an abort flag before an early-return — is filed in `frontends/macos9/CLAUDE.md` →
  "Networking / fetchers".
