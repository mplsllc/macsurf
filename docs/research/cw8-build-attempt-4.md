# CW8 Build Attempt 4, Error Analysis

## Source

`errors.txt` at repo root, **complete build run**, 18,494 lines after
CR→LF, **2,857 distinct `Error :` lines**.

## TL;DR

**Net change: 3,088 → 2,857 (231 errors removed).** Two of the three
attempt-3 fixes landed on the Mac and worked exactly as predicted.
**The third fix (libdom headers zip) was not unpacked yet**, every
single libdom missing-header error from attempt 3 is still present,
identical down to file/line. Errors are dominated by that one issue.

| Fix | Status | Errors removed |
|---|---|---:|
| #1 libdom headers zip | **NOT YET UNPACKED** | 0 |
| #2 propset.h decl hoists | ✓ landed | ~210 (27 `old_string_arr` + 18 `old_counter_arr` + 12 `__lwc_s` + ~150 cascade) |
| #3 jsthread rename | ✓ landed | ~21 (3 name overloading + ~18 cascade) |

Plus **8 brand-new errors** unmasked by the propset.h cleanup ,
`fpmath.h` was previously hidden behind the propset.h cascade and
now reaches the parser. Real fix needed.

## Error histogram (top 25)

| Count | Error | Δ vs attempt 3 | Category |
|---:|---|---|---|
| 1,357 | illegal function definition | +10 | cascade |
| 479 | identifier expected | +2 | cascade |
| 330 | declaration syntax error | +28 | cascade |
| 266 | `')'` expected | +14 | cascade |
| 94 | illegal use of incomplete struct `dom_node_internal` | 0 | cascade #1 |
| 48 | `';'` expected | -12 | cascade |
| 34 | illegal implicit conversion `'long *'` | 0 | cascade |
| 23 | the file `'dom/bindings/hubbub/parser.h'` cannot be opened | 0 | **root #1** |
| 21 | expression syntax error | -46 | cascade |
| 13 | illegal implicit conversion `'int *'` | 0 | cascade |
| 12 | undefined identifier `'exp'` | 0 | cascade |
| 12 | undefined identifier `'dom_html_document'` | 0 | cascade |
| 10 | undefined identifier `'doc'` | 0 | cascade |
| 8 | undefined identifier `'params'` | 0 | cascade |
| 7 | object `'node'` redefined | 0 | cascade |
| 6 | undefined identifier `'new_node'` | 0 | cascade |
| 6 | undefined identifier `'exc'` | 0 | cascade |
| 6 | illegal use of incomplete struct `'struct nsurl'` | 0 | cascade |
| 5 | undefined identifier `'rows'` | 0 | cascade |
| 5 | undefined identifier `'plot_style_bdr'` | 0 | cascade #1 |
| 5 | data type is incomplete | 0 | cascade |
| 4 | undefined identifier `'pointer'` | 0 | cascade |
| 4 | the file `'core/node.h'` cannot be opened | -1 | **root #1** |
| 4 | the file `'core/attr.h'` cannot be opened | 0 | **root #1** |
| **4** | **illegal use of type `'int (...)'`** | **+4 NEW** | **root #4** |
| **4** | **identifier `'css_fixed'` redeclared** | **+4 NEW** | **root #4** |

Categories that **vanished** between attempt 3 and 4:

| Removed | Count | Source |
|---|---:|---|
| `undefined identifier 'old_string_arr'` | 27 | propset.h fix |
| `undefined identifier 'old_counter_arr'` | 18 | propset.h fix |
| `undefined identifier '__lwc_s'` | 12 | propset.h fix |
| `illegal name overloading` (private.h:152) | 3 | jsthread fix |
| `_dom_*_initialise(...) redeclared as int` | 3 | partial, see below |
| **Cascade reduction** | ~165 | downstream of the above |

## Root cause #1, libdom headers (UNCHANGED, zip not unpacked)

Every libdom-related error from attempt 3 is present unchanged in
attempt 4:

```
23 × dom/bindings/hubbub/parser.h cannot be opened
 4 × core/node.h cannot be opened
 4 × core/attr.h cannot be opened
 3 × html/html_tablerow_element.h
 3 × html/html_document.h
 3 × html/html_collection.h
 ... (24 unique headers, identical to attempt 3)
94 × illegal use of incomplete struct dom_node_internal (identical)
12 × undefined identifier dom_html_document (identical)
 5 × undefined identifier plot_style_bdr (identical — cascade
     from missing libdom causing redraw_border.c → html/private.h
     to drop plot_style_t along the way)
```

Conclusion: **the user has not yet unpacked
`macsurf-libdom-headers.zip` on the Mac.** Until that happens, the
build can't make further progress. Fix is unchanged from attempt 3.

## NEW root cause #4, `fpmath.h` multi-line function declarations

**Unmasked by the propset.h fix.** Previously hidden inside a parser
recovery cascade, now visible. File:
`browser/libcss/include/libcss/fpmath.h`. 6 inline functions, all
written like this:

```c
static inline css_fixed
css_add_fixed(const css_fixed x, const css_fixed y) {
    int32_t ux = x;
    ...
}
```

The return type `css_fixed` and function name `css_add_fixed` are on
**separate lines**. This is legal C89, but CW8 misparses it as:

1. `static [implicit-int] css_fixed;`, a top-level variable
   declaration named `css_fixed` (storage class static, type
   implicit int because none specified).
2. `css_add_fixed(...)` then becomes a stray function definition
   with implicit-int return.
3. `(const css_fixed x, ...)` is parsed where `css_fixed` is now an
   identifier (the variable from step 1), so `const css_fixed x`
   reads as a `const`-qualified `int` parameter named `x`.
4. CW8 also internally registers `css_fixed` as having type
   `const int` (from the parameter parse), then later notices the
   real `typedef int32_t css_fixed;` on line 23 and reports
   "identifier 'css_fixed' redeclared, was 'const int', now 'int'".

Symptoms in errors.txt:

- 4× `identifier 'css_fixed' redeclared` (one per function except
  the last two, those probably abort the file early)
- 4× `illegal use of type 'int (...)'` at `parser.h:84/88/91/105` ,
  downstream of the same cascade reaching `dom/bindings/hubbub/parser.h`,
  where dom_hubbub_parser_* declarations get implicit-int treatment.
- ~150 cascade errors throughout the file

The 6 affected lines in fpmath.h:

```
line 25:  static inline css_fixed
line 42:  static inline css_fixed
line 59:  static inline css_fixed
line 71:  static inline css_fixed
line 83:  static inline css_fixed
line 95:  static inline css_fixed       (approximate — based on grep)
```

### Fix

Collapse the function signature onto a single line for each:

```c
static inline css_fixed css_add_fixed(const css_fixed x, const css_fixed y) {
    int32_t ux = x;
    ...
}
```

After `#define inline` from the prefix, that becomes
`static  css_fixed css_add_fixed(...)` on a single line, which CW8
parses correctly because the return type and function name are
unambiguously together.

This is a 6-line edit to one upstream header. Mechanical, no logic
change. Same pattern as the propset.h hoist, add to the libcss port
audit checklist for future scans: "look for two-line function
signatures in headers."

## Other observations

### `_dom_*_initialise(...) redeclared as int (...)`, DOWN from 3 to 0?

Wait, actually still present, I miscounted. The "object 'node'
redefined" count of 7 may include these. Will resolve with libdom
headers.

### `va_copy` redefined, still 1×

Unchanged. Low-priority cleanup. The fix is a `#ifndef va_copy` guard
around whichever definition we control (probably in `talloc.c` since
talloc is famous for redefining va_copy).

### `internal compiler error in CError.c line 861`, still 1×

CW8 bug, will go away once the real errors clear.

### `redraw.c:1006 continue;` and `libdom.c:56 break;`, still present

Cascade. Will resolve when libdom headers land.

## Proposed fix order

Now extra-strict because nothing else can move until the libdom
headers land.

1. **[blocks ~2,700 errors]** **Unpack
   `macsurf-libdom-headers.zip` on the Mac.** This was shipped
   alongside `macsurf-attempt3-fixes.zip` but does not appear to
   have been applied yet. The headers need to land in
   `Macintosh HD:…:browser:libdom:include:dom:` and
   `Macintosh HD:…:browser:libdom:src:`. The unpack will create
   new subfolders for `core/`, `events/`, `html/`, `treebuilder/`,
   `bindings/`, `dom/bindings/hubbub/`, etc.

2. **[blocks ~150 errors]** Patch `browser/libcss/include/libcss/fpmath.h`
   to collapse the 6 multi-line function signatures into single
   lines. Linux cross-check, commit. Ship as a one-file follow-up
   zip.

3. **[blocks ≤ 5 errors]** Re-run the full build. Whatever remains
   is the genuinely-novel set worth fixing one at a time.

4. **[< 5 errors]** Investigate and fix the long-tail singletons:
   `va_copy redefined`, the `internal compiler error`, anything new
   that surfaces from the libdom unmask.

## Forecast for attempt 5

If steps 1 and 2 both land cleanly, attempt 5 should drop to **under
50 total errors**. The remaining cascade is approximately:

- ~5 from `plot_style_bdr` redraw_border.c (already a cascade ,
  resolves with libdom)
- 1 va_copy redefined
- 1 internal compiler error (cosmetic)
- Possibly the multi-arg NSLOG / `%zu` punts surfacing for the
  first time

If attempt 5 reveals **another C89-in-an-upstream-header issue
similar to fpmath.h**, that's diagnostic gold, it means the libcss
audit checklist needs a "scan upstream headers" pass that we never
ran. But I'd expect at most one or two more of those.
