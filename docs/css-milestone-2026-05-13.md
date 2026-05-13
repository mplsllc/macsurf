# CSS rendering live on hardware — 2026-05-13 — fixes33

After ten diagnostic rounds the actual bug was identified and fixed in a single line of code. On real Power Mac G3 hardware running MacSurf, simple.html now renders with:

- H1 in navy (`fg=0/0/128`), 15pt, bold
- H2 in maroon (`fg=128/0/0`), 12pt, bold
- Body background `#cccccc` grey
- H1 card with white background and proper padding
- Block-level layout with 32 block boxes (previously 1)
- Author CSS rules from `<style>` AND UA default.css both taking effect

This is the first MacSurf session on hardware where the libcss cascade end-to-end actually drives QuickDraw output. Verified against the user-supplied debug log and a screenshot from the G3.

## The bug

`libcss`'s selector hash uses two functions for hashing tag names — one on insert, one on find:

**Insert path** (`browser/libcss/src/select/hash.c`):

```c
#define _hash_name(name) lwc_string_hash_value(name->insensitive)
```

Returns `name->insensitive->hash`. That hash was computed when `lwc__intern_caseless_string` interned the caseless form — using libwapcaplet's `lwc__calculate_lcase_hash` (FNV-1a with constants `0x811c9dc5` and `0x01000193`).

**Find path** (`browser/libcss/src/select/hash.c css__selector_hash_find`):

```c
lwc_string_caseless_hash_value(req->qname.name, &name_hash);
index = name_hash & mask;
```

`lwc_string_caseless_hash_value` is declared `extern` in `libwapcaplet.h` with the comment "MacSurf: out-of-line. Defined in macos9_extra_stubs.c." The actual definition lived in `browser/netsurf/frontends/macos9/misc_stub.c` and used a completely different hash:

```c
for (i = 0; i < len; i++) {
    unsigned char c = ...; if (uppercase) c += 32;
    h = (h * 31) + c;
}
```

Inserts went into one bucket. Finds looked in a different bucket. The selector hash silently never returned matches even though it was being populated correctly. `node_has_name` never fired for any element on any sheet. `css_select_style` returned libcss initial values for every node. Every page rendered as 12pt black text in libcss's default font.

## The fix

```c
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

After the change, insert and find compute identical hashes. The hashtable lookups return the chains that were inserted into them. Selector matching proceeds normally. The cascade applies UA defaults and author rules to elements. Computed styles flow through `font_plot_style_from_css` to QuickDraw with the colour/size/weight the CSS says they should have.

One file changed. 14 lines net. Eleven rounds of diagnostic probing led to it.

## How we got here — the diagnostic chain

| Fix | Probe | Finding |
|---|---|---|
| 24 | per-slot cascade state | inline `<style>` slot has handle + nscss_get returns non-NULL |
| 25 | RGB at plot_text / plot_rectangle | every text plot is `fg=0/0/0 sz=12 face=0` — cascade returns initial values |
| 26 | sheet_size + tag names selected | sheets are 15KB / 6KB; HTML, BODY, H1, P, H2 all reach selection |
| 27 | computed style after `css_select_style` | every element returns identical `color=FF000000 fsz=17476 weight=1` — no rule matched |
| 28 | `node_has_name` callback | never fires — libcss isn't reaching the qname comparison |
| 29 | `match_selectors_in_sheet` entry | no probe fires; suspect duplicate-file issue |
| 30 | same probe in `css_select.c` (sibling file) | `msis_cs[N]` fires — confirmed `css_select.c` is what's compiled, not `select.c` |
| 31 | iterator results inside `msis_cs` | `*node=00000000 *univ=00000000 pending=0` — hash has no chains for any query |
| 32 | `_add_selectors` + `css__selector_hash_insert` | 30 selectors inserted into the hash during parse |
| 33 | **THE FIX** | insert vs find hash algorithm mismatch in `misc_stub.c` |

## What still needs work (visible artefacts)

The screenshot also exposes the next set of bugs:

- **List bullets render as `;`** instead of disc/circle/square. UA's `li { display: list-item; list-style-type: disc }` is applying display but the glyph rendering doesn't reach a real bullet character.
- **Right-edge text spills outside content rect.** Words like "borders" overflow into "features" on the right margin; some text duplicates ("p" alone appears next to a paragraph). Line-break / shaping bug.
- **Letter spacing is tight.** "MacSurf Test Page" reads as "MacSurf Test Pag" cut off — likely the snippet log truncating at 16 chars, but font metrics on Mac classic should be checked.

These are now real next-fix candidates. Up to this point we've been chasing a single bug that pretended every element was unstyled.

## What this unlocks

- Real CSS rendering for any page MacSurf can fetch through the proxy.
- All UA defaults take effect (display: block, body margins, h1-h6 sizes/weights, link colours, list semantics).
- Author CSS via `<style>` works (already proven on simple.html).
- Author CSS via `<link rel=stylesheet>` should work the same way once a page exercises it.
- The diagnostic logging added in fixes24-32 stays in the build for now. A cleanup round can strip it once we're sure everything is stable.

## Reference

Commit: `9bc0db7f fixes33: THE bug -- caseless hash mismatch between insert and find`
Tag: `v0.4-css-applies`
Hardware: Power Mac G3 minitower, Mac OS 9.1, CodeWarrior 8 build.
Verified by: user-supplied `MacSurf Debug.log` showing post-fix selector matches + screenshot showing styled simple.html.
