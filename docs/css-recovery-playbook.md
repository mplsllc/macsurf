# MacSurf CSS recovery playbook — fixes1 → fixes35

Written 2026-05-13. Captures the climb back from "page renders but no
author CSS applies" through to "real modern CSS rendering on G3
hardware." If we ever lose ground on the cascade again, start here.

## Headline timeline

- **Baseline (fixes318 / fixes1)** — build links, page fetches and parses, layout produces ~110 boxes, text plots at libcss initial values. No author CSS applied. No UA rules applied. Window is grey, text is 12pt black on white.
- **fixes2–17** — build-recovery work. Library link errors, C89 hygiene, CW8 header collisions, MSL library swap, strdup/strcasecmp stubs. Build links again. Page still renders without any CSS.
- **fixes18–22** — premature "v0.4" feature work. Walker for inline `<style>`, link-click dispatch, hover tracking, URL bar reorder, image fallback. None of it actually compiled into the binary because of the **duplicate-file ambiguity** described below. Round 18's claimed fix didn't ship until round 23 cleared the file collision.
- **fixes23** — deleted `content/handlers/html/html_css.c`. The on-Mac project was building `html_css.c` instead of `css.c`. Same root issue surfaces again at fixes30 for `select.c` vs `css_select.c`.
- **fixes24–32** — eleven-round diagnostic chain. Each round added one probe, narrowed the symptom by one layer. See [Diagnostic chain](#diagnostic-chain) below.
- **fixes33** — **the one-line fix.** Aligned `lwc_string_caseless_hash_value` in `misc_stub.c` with libcss's selector-hash insert path. Insert and find finally compute the same bucket index. Selector matching works. UA and author CSS both apply.
- **fixes34** — `plot_text` UTF-8 → MacRoman conversion; home URL → `advanced.html`. Bullets, em-dashes, smart quotes render correctly.
- **fixes35** — expanded the UTF-8 → MacRoman table for list markers (circle, square), math operators, arrows, fractions, currency.

Current state at the time of writing: `advanced.html` renders with the full cascade — `var()` custom properties resolve, H1 navy bold 28px, H2 maroon bold, cards with `border-radius` and `box-shadow`, tables with `nth-child(even)` zebra striping, flexbox rows, blockquotes with accent borders, form chrome, inline pills, swatches with text-shadow. 559 boxes, 85 block-level, 2705px content height.

## The actual bug — fixes33

`libcss` builds a selector hash inside each parsed stylesheet. When you call `css_select_style` for a DOM node, libcss hashes the node's tag name, finds the hash bucket, and walks the chain of selectors that landed there. If the bucket is empty, no rules try to match that element.

Two functions are involved:

**Insert** — `select/hash.c`:
```c
#define _hash_name(name) lwc_string_hash_value(name->insensitive)
/* in css__selector_hash_insert: */
index = _hash_name(selector->data.qname.name) & mask;
```
Returns `name->insensitive->hash`. That hash was computed by `lwc__intern_caseless_string` (in libwapcaplet.c) calling `lwc__calculate_lcase_hash` — an FNV-1a variant with constants `0x811c9dc5` and `0x01000193`.

**Find** — `select/hash.c`:
```c
lwc_string_caseless_hash_value(req->qname.name, &name_hash);
index = name_hash & mask;
```
That function is declared `extern` in `libwapcaplet.h` with the comment "MacSurf: out-of-line. Defined in macos9_extra_stubs.c." Our actual definition lived in `browser/netsurf/frontends/macos9/misc_stub.c` and computed:
```c
for (i = 0; i < len; i++) {
    unsigned char c = ...; if (uppercase) c += 32;
    h = (h * 31) + c;
}
```

**FNV-1a vs. `h*31 + c`. Two completely different algorithms.** Inserts went into bucket X computed by FNV. Finds looked in bucket Y computed by `*31`. The hash table was correctly populated and correctly queried — just on different sides of an unbridgeable arithmetic gap. Every selector ever inserted into the hash was invisible to the matching code.

### The fix

```c
extern lwc_error lwc__intern_caseless_string(lwc_string *str);

lwc_error lwc_string_caseless_hash_value(lwc_string *str, lwc_hash *hash)
{
    lwc_error err;
    if (str == NULL || hash == NULL) return lwc_error_range;
    if (str->insensitive == NULL) {
        err = lwc__intern_caseless_string(str);
        if (err != lwc_error_ok) return err;
    }
    *hash = lwc_string_hash_value(str->insensitive);
    return lwc_error_ok;
}
```

The stub now interns the caseless form (if it doesn't exist yet), then returns `lwc_string_hash_value(str->insensitive)` — the exact value libcss reads in the insert path. Insert and find agree.

That fix lives in `browser/netsurf/frontends/macos9/misc_stub.c`. It is **the single most load-bearing line of CSS support in MacSurf**. If CSS rendering ever stops working again, this is the first thing to check.

## Diagnostic chain

Each row is a single fix round, the probe it added, and the finding that pointed at the next round.

| Round | Probe | Finding |
|---|---|---|
| 24 | per-slot cascade state at `html_css_new_selection_context` | Inline `<style>` slot has a non-NULL handle and `nscss_get_stylesheet` returns non-NULL → cascade contains the sheets |
| 25 | RGB at plot_rectangle / plot_text first N calls | Every text plot is `fg=0/0/0 sz=12 face=0` → cascade returns libcss initial values for everything |
| 26 | `css_stylesheet_size` per slot; tag name being selected | Sheets are 15KB / 6KB; HTML, BODY, H1, P, H2 all reach selection with non-NULL ctx |
| 27 | `css_computed_color`/`css_computed_font_size`/`css_computed_font_weight` after `box_get_style` | Every element returns identical `color=0xFF000000 fsz=17476 weight=1` — proves NO rule matched anything |
| 28 | `node_has_name` callback in `cssh_select.c` | Probe never fires — libcss isn't reaching the qname comparison at all |
| 29 | `match_selectors_in_sheet` entry in `select/select.c` | Probe never fires — strongly suggests duplicate-file ambiguity (same pattern as fixes23) |
| 30 | Same probe in sibling `select/css_select.c`, tagged `msis_cs` | `msis_cs[N]` fires → confirmed `css_select.c` is what's compiled, not `select.c` |
| 31 | Iterator outputs inside `msis_cs` after the hash finds | `*node=00000000 *univ=00000000 pending=0` — hash returns no chains for any element on any sheet |
| 32 | `_add_selectors` + `css__selector_hash_insert` at parse time | 30 selectors inserted per sheet, all going into `hash=...` at parse time → insert side is healthy |
| 33 | **The fix.** Aligned the caseless hash function in `misc_stub.c` with libcss's insert path | Insert and find now compute the same bucket index. Selectors visible. Cascade works. |

Total elapsed: 11 rounds. Each round shipped one or two probes, the user applied + rebuilt + sent the log, the next round read the log and added the next probe. There is no shortcut — each layer of "what does this pointer look like" had to be checked before the next.

## Two related bugs — duplicate source files

This was the second-biggest time sink across the session block. The pattern is identical for three pairs of files in the tree:

| Canonical (per Linux `.mcp` or actual compiled file) | Sibling (also present on disk) |
|---|---|
| `content/handlers/html/css.c` | `content/handlers/html/html_css.c` (deleted at fixes23) |
| `libcss/src/select/css_select.c` (per fixes30) | `libcss/src/select/select.c` |
| `libcss/src/parse/parse.c` | `libcss/src/parse/css_parse.c` |
| `frontends/macos9/macos9_font.c` (per fixes36) | `frontends/macos9/font.c` (deleted at fixes36 — stub only) |

These pairs are NetSurf's normal files plus longer-named sibling copies that came in through some earlier rename / fix-round. **Both files define the same external symbols.** If both are in the CW8 project, the linker rejects "multiply-defined." So exactly one ends up in the build — but which one isn't always obvious from Linux.

Symptoms:
- A diagnostic probe added to one file silently doesn't fire, because the project is compiling the OTHER file
- A feature added to one file silently doesn't ship for the same reason
- Symptoms look like "user didn't apply the zip" or "CW8 has stale .o" — but the real cause is which file the on-Mac project lists

Recovery path:
1. Look in `MacSurf.mcp` (Linux side) to see what the canonical file is supposed to be
2. Add the same probe to BOTH siblings on disk, tag them differently in the log (`msis[N]` vs `msis_cs[N]`), ship both
3. Look at the next log to see which one fires
4. Once you know, delete the obsolete sibling from the tree so future rounds don't re-hit the same trap

fixes23 cleared `html_css.c`. fixes30 confirmed `css_select.c` was the compiled select file; we did NOT delete `select.c` after that, so the trap is still live there. fixes36 cleared `font.c` after the gap-test screenshot showed `fixes35`'s circle/square UTF-8 mappings hadn't reached the rendered output (root cause was ambiguous between "not applied" and "wrong file in project", but deleting the stub eliminates the second possibility outright).

**Recommendation:** in a quiet round, delete the remaining obsolete siblings (`select.c`, `parse.c`, `font_face.c`) once we're sure the build is stable on the current canonical files. The trap will keep biting otherwise.

## The box-tree shape invariant — fixes37 / fixes38 / fixes39

A second-class trap that swallowed three rounds while trying to land `::before` / `::after` generated content. Two stacked bugs, only the first of which the obvious-looking patch catches.

### Bug A: `c_item` is only initialised when `css_computed_content` returns SET

`css_computed_content(style, &c_item)` returns one of four states:

| Value | Name | `c_item` valid? |
|---|---|---|
| 0 | `CSS_CONTENT_INHERIT` | **No** (libcss does not write to it) |
| 1 | `CSS_CONTENT_NONE`    | **No** |
| 2 | `CSS_CONTENT_NORMAL`  | **No** |
| 3 | `CSS_CONTENT_SET`     | **Yes** — points to a `CSS_COMPUTED_CONTENT_NONE`-terminated array |

The pre-fixes37 stub only checked `== CSS_CONTENT_NORMAL` to bail out. fixes37 then started walking `c_item` to extract the content text — but on INHERIT or NONE the pointer was uninitialised stack memory, and the walk found no `CONTENT_NONE` terminator. Effective infinite loop; fixes38 traced the hang to this and switched the check to `!= CSS_CONTENT_SET || c_item == NULL`.

**Rule:** always gate iteration of `c_item` on `css_computed_content(...) == CSS_CONTENT_SET` AND a `c_item != NULL` belt-and-braces test, in that order so short-circuit evaluation never even reads `c_item` on the non-SET paths.

### Bug B: text under a block must live inside an INLINE_CONTAINER

This is the one that crashed the browser even after Bug A was fixed. NetSurf's box tree has a strict shape contract that `box_normalise` enforces during the post-construction cleanup pass:

- A `BOX_BLOCK` can have `BOX_BLOCK` children, or
- A `BOX_BLOCK` can have `BOX_INLINE_CONTAINER` children, which contain `BOX_INLINE` and `BOX_TEXT` children
- A `BOX_BLOCK` **may NOT have a `BOX_TEXT` directly as a child**

The fixes37 generated-content code created a `BOX_BLOCK` for the pseudo-element and attached a `BOX_TEXT` directly under it. The element built fine. The cascade selected fine. `dom_to_box` walked the DOM, called `box_construct_element`, called `box_construct_generate` for the `:before` pseudo — but the moment `box_normalise` walked the result post-construction, it found the bare `BOX_TEXT` under `BOX_BLOCK` and crashed. The debug log truncated immediately after `css_select_ctx: appended=...` with no `box convert` line.

**Symptom signature:**
- Log ends right at `css_select_ctx` (cascade built, OK)
- No `box convert: layout=... total=...` line
- No crash trap in the log file
- App exits silently on launch, or hangs hard if there's also a loop

**Rule:** when synthesising boxes in `box_construct_generate` or any similar pseudo-element / generated-content path, the only safe pattern for text content is:

```
BOX_INLINE  (the pseudo-element itself; text fields set directly on it)
  └── (no separate BOX_TEXT child)
```

Where this inline box gets inserted INTO an `BOX_INLINE_CONTAINER` that either already exists at the insertion point or is auto-created at the same time. Do **not** create:

```
BOX_BLOCK
  └── BOX_TEXT   ❌  box_normalise crash
```

or

```
BOX_BLOCK
  └── BOX_INLINE  ❌  same problem, missing INLINE_CONTAINER wrapper
```

The list-marker code (`box_construct_marker`) shows the safe pattern: the marker is a `BOX_BLOCK` with `marker->text` set **directly on the marker box itself** — no separate `BOX_TEXT` child — and the marker is then attached as `box->list_marker` (a SIDE pointer, not a regular child). The block-with-text-field-set pattern works only because `list_marker` is treated specially throughout layout.

### Recovery if generated content support is attempted again

The right approach (untried as of fixes39):
1. Generate the pseudo-element as a `BOX_INLINE`, not `BOX_BLOCK` / `BOX_TABLE`
2. Concatenate the content-item strings into a buffer (the fixes37 walk loop was correct here)
3. Set `gen->text = concat; gen->length = total;` directly on the inline box
4. Find or auto-create a `BOX_INLINE_CONTAINER` at the insertion point in the parent's child list
5. Add the inline box as a child of that container

If the test page exercises generated content and the browser silently doesn't open after applying — Bug B is back. Check the box tree shape first, before adding diagnostic probes.

## How CSS flows end-to-end in MacSurf

When CSS works (current state, post-fixes33), the path is:

1. **HTTP fetch** (`macos9_http_fetcher.c` via OT-through-proxy) pulls the HTML body bytes.
2. **HTML parsing** (`libhubbub`) tokenises, **libdom** builds the DOM tree from the tokens.
3. **Stylesheet discovery** — three sources:
   - UA `resource:default.css` served by the stub fetcher (`macos9_fetcher_stubs.c`) → goes to `STYLESHEET_BASE` slot 0
   - UA `resource:user.css` served by stub → slot 3 (often unaccepted because body is 0 bytes)
   - Inline `<style>` and `<link rel=stylesheet>` discovered by the post-parse walker added in fixes18 (`html_css_discover_stylesheets` in `content/handlers/html/css.c`). Inline styles get `x-ns-css:KEY` URLs served by `html_css_fetcher` (`css_fetcher.c`).
4. **CSS parsing** — each fetched body goes through `nscss_create` / `nscss_process_data` / `nscss_convert` in `cssh_css.c`. The libcss parser builds a `css_stylesheet` with a populated `rule_list` and a `selectors` hashtable.
5. **Selector hash population** (THIS is where fixes33 saved us) — during parsing, `_add_selectors` in `stylesheet.c` calls `css__selector_hash_insert` for every selector. The hash bucket is computed from `lwc_string_hash_value(name->insensitive)`.
6. **Cascade assembly** — `html_css_new_selection_context` (in `css.c`) iterates the html_content's stylesheets and calls `css_select_ctx_append_sheet` for each. The select_ctx is now the cascade root.
7. **Per-element selection** — for each DOM element, `box_construct.c` calls `box_get_style` → `nscss_get_style` → `css_select_style`. libcss walks each sheet in the cascade and asks our selection_handler about each rule's selector. `node_has_name` (in `cssh_select.c`) is the key callback for type selectors. The hash is queried by `lwc_string_caseless_hash_value(qname, &h)` — and now (post-fixes33) that returns the same value the insert path used.
8. **Computed style** — selection produces a `css_computed_style *` per element, stored on `box->style`.
9. **Layout** — NetSurf's layout.c reads from `box->style` to position boxes, compute widths, etc.
10. **Plotting** — for each box's redraw, `font_plot_style_from_css` reads colour, font-size, weight from `box->style` and constructs a `plot_font_style_t`. The MacSurf plotters (`plotters.c`) translate that to `RGBForeColor` / `TextFont` / `TextSize` / `TextFace` and call QuickDraw's `DrawText`. Backgrounds and borders go through `plot_rectangle`, with `PaintRect` and `FrameRect` (or `PaintRoundRect` / `FrameRoundRect` for `border-radius`).

Steps 1-4 had been working since the long-ago v0.3 milestone. Step 5 was poisoned by the hash mismatch — the hashtable was being correctly populated but unqueryable. Steps 6-10 worked all along but were operating on empty selection results.

## How to verify CSS is alive

In `MacSurf Debug.log` after a page load, look for:

- `stylesheet count after discovery: N` — should be ≥ 2 (UA + USER) for any HTML page. Higher if the page has `<style>` or `<link>`.
- `css_select_ctx: appended=N unused=0 null=K total_slots=T` — `appended` should be ≥ 2.
- `msis_cs[N] sheet=... selectors=... rule_list=...` — both `selectors` and `rule_list` should be non-NULL.
- `iter[N] node_sel=... *node=... univ=... *univ=... pending=N` — `*node` or `*univ` should be **non-zero** when an element has any rule that could match it. `pending=1` means a match is going to be tried.
- `post_select[N] tag=H1 color=FF003366 fsz=28672 weight=2` — for any element with a rule, color / fsz / weight should differ from the boring defaults (`FF000000` / `17476` / `1`).
- `plot_text[N] fg=...` — non-default colors confirm the styles reach the plotter.

If all of those look healthy but the page still looks unstyled, the bug has moved to layout or to plotters — that's a different round.

## How to recover if CSS breaks again

In likely order:

### 1. Did somebody re-introduce `html_css.c`?

```bash
ls /home/patrick/Webs/macsurf/browser/netsurf/content/handlers/html/css.c \
   /home/patrick/Webs/macsurf/browser/netsurf/content/handlers/html/html_css.c
```

If both exist, **delete `html_css.c`**. Same goes for `css_select.c` vs `select.c` and `parse.c` vs `css_parse.c`. The Linux `.mcp` is the source of truth for what should be in the build. If the on-Mac project disagrees, the on-Mac project is the bug.

### 2. Did somebody change `misc_stub.c`'s `lwc_string_caseless_hash_value`?

```bash
grep -A 15 "lwc_string_caseless_hash_value" /home/patrick/Webs/macsurf/browser/netsurf/frontends/macos9/misc_stub.c
```

The function MUST end with:

```c
*hash = lwc_string_hash_value(str->insensitive);
```

after first ensuring `str->insensitive` is non-NULL by calling `lwc__intern_caseless_string(str)`. Any other implementation will mismatch libcss's insert-time hash and the cascade goes dark again.

### 3. Did `_hash_name` in `libcss/src/select/hash.c` change?

```bash
grep -A 2 "_hash_name(name)" /home/patrick/Webs/macsurf/browser/libcss/src/select/hash.c
```

It must read:

```c
#define _hash_name(name) lwc_string_hash_value(name->insensitive)
```

If it changes to something else (e.g. someone "optimises" it to call `lwc_string_caseless_hash_value`), the insert/find pair has to be re-aligned across both sides.

### 4. Did the walker stop firing?

```bash
grep "html_css_discover_stylesheets" \
  /home/patrick/Webs/macsurf/browser/netsurf/content/handlers/html/css.c \
  /home/patrick/Webs/macsurf/browser/netsurf/content/handlers/html/css.h \
  /home/patrick/Webs/macsurf/browser/netsurf/content/handlers/html/html.c
```

All three should have a match. `css.h` declares it, `css.c` defines it, `html.c` calls it from `html_begin_conversion` (gated by `htmlc->stylesheets_discovered`). If any one is missing, inline `<style>` blocks stop reaching the cascade.

### 5. Did `htmlc->stylesheets_discovered` stop being a struct field?

```bash
grep "stylesheets_discovered" /home/patrick/Webs/macsurf/browser/netsurf/content/handlers/html/private.h
```

Must be present as a `bool` field in `struct html_content`. Was added in fixes18.

### 6. Run the probe chain again

If steps 1-5 all look fine and CSS is still broken, ship the diagnostic chain again — probably fewer rounds this time:

- Probe `match_selectors_in_sheet` to confirm selectors / rule_list are non-NULL
- Probe iterator output to see whether `*node` and `*univ` are populated
- If the hash returns empty, walk back through the parse-time `hash_ins` probe
- Compare insert-time hash and find-time hash for the same string

The eleven-round chain is documented above — copy the probes, don't re-derive them.

## What the screenshots from 2026-05-13 show working

For posterity, snapshot of capabilities confirmed by the user's hardware test of `advanced.html`:

- **Custom properties (`var()`)** — `:root { --brand: #003366; --accent: ...; }` and `color: var(--brand)` resolve correctly through inheritance
- **Block + inline flow** — 559 boxes, 85 blocks, proper margins, line wrap, padding
- **Typography** — multiple font sizes, weights, italic, monospace, sub/sup positioning
- **Colors** — hex, named, all rendering through `nscss_color_to_ns` → `RGBForeColor`
- **Borders** — border-radius (via QuickDraw RoundRect), border-color, border-width, border-style
- **Backgrounds** — solid colour fills via PaintRect for any computed `background-color`
- **Tables** — `display: table`, caption, thead, tbody, `border-collapse: collapse`, `tr:nth-child(even)`, cell padding, header text-align
- **Lists** — ul/ol/li with markers (disc after fixes34 UTF-8 fix, circle/square after fixes35), nested levels, `padding-left`, dl/dt/dd
- **Flexbox** — `display: flex; flex-direction: row; gap: N` distributes children evenly (layout_flex.c reads main-axis)
- **Floats** — `float: left` + `overflow: hidden` clearfix for column layouts
- **Pseudo-classes** — `:link`, `:visited`, `:hover`, `:nth-child(even)` all match
- **Pseudo-elements** — generated content not exercised; `::before` / `::after` is the next gap
- **Forms** — input/textarea/select/button styled via attribute selectors and class
- **Inline elements** — code, kbd, mark, ins, del, abbr, strong, em, sub, sup
- **Text decorations** — underline (link), line-through (del, .struck), `text-transform: uppercase`, `letter-spacing`

Still cosmetic gaps:
- **box-shadow** parses (per fixes175) but doesn't render — QuickDraw has no shadow primitive
- **Gradients** not parsed
- **Transforms / transitions / animations** — out of reach on QuickDraw
- **CSS Grid** — collapses to block layout
- **`text-shadow`** — silently ignored

These are next-decade work, not regressions.

## Filed under

- [docs/css-milestone-2026-05-13.md](css-milestone-2026-05-13.md) — milestone screenshot + log
- [docs/state-2026-05-13.md](state-2026-05-13.md) — the honest pre-fixes33 state writeup (note: predates the win)
- Tag: `v0.4-css-applies` on origin
- Last shipped fix at time of writing: `fixes35` (cbb16d95)
