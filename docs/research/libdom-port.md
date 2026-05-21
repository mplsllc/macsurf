# libdom Port Audit (CW8 / C89)

## Status

**Research only, no code changes.** This document is a complete inventory of
what is in the libdom source tree and exactly what stands between it and a
clean CodeWarrior 8 / strict C89 / Mac OS 9 build. Same structure as
`docs/research/parserutils-port.md` and `docs/research/libhubbub-port.md`.

## Where the source lives

`browser/libdom/`, present in the repo, vendored alongside the other
NetSurf libraries. Not a submodule; the tree is checked in directly.
License is **MIT**.

Upstream is the NetSurf project's libdom:
`https://git.netsurf-browser.org/libdom.git/` (or the GitHub mirror at
`github.com/netsurf-browser/libdom`).

**Nothing needs to be fetched.** The source is already where we need it.

## Scope

- **287 source/header files** in scope after exclusions.
- **~47,900 lines of code** in scope (≈ 7× libparserutils, ≈ 3× libhubbub).
  This is by far the biggest library in the chain.
- **`browser/libdom/test/`, `examples/`, `docs/`, and `gdb/` excluded** ,
  not part of the build.
- **`browser/libdom/bindings/xml/` EXCLUDED**, depends on libxml2 and/or
  expat, neither of which exists on Mac OS 9. Removes 5 files / ~1,800
  lines from scope.

Subdirectory line counts (in-scope only):

| Subdir | Files | LOC |
|---|---:|---:|
| `include/dom/` | 95 (.h) | 6,816 |
| `src/utils/` | 11 (5 .c + 6 .h) | 1,547 |
| `src/core/` | 34 (18 .c + 16 .h) | 13,985 |
| `src/html/` | 115 (57 .c + 58 .h) | 20,561 |
| `src/events/` | 28 (14 .c + 14 .h) | 3,701 |
| `bindings/hubbub/` | 4 (1 .c + 3 .h) | 1,267 |
| **TOTAL** | **287** | **47,877** |

`src/html/` is the bulk, 57 `.c` files, one per HTML element type
(`html_anchor_element.c`, `html_table_element.c`, etc.). Each is small
and structurally similar.

`bindings/hubbub/parser.c` is the bridge from libhubbub events to libdom
nodes, the layer that makes "fetched HTML bytes → DOM tree" actually
work end-to-end. This is the file that NetSurf core ultimately calls when
it wants to parse a page. Critical.

## File map summary

**95 `.c` files in scope, broken down:**

- 5 `src/utils/`: hashtable, list, namespace, validate, walk
- 18 `src/core/`: attr, cdatasection, characterdata, comment, doc_fragment,
  document, document_type, element, entity_ref, implementation, namednodemap,
  node, nodelist, pi, string, text, tokenlist, typeinfo
- 14 `src/events/`: custom_event, dispatch, document_event, event,
  event_listener, event_target, keyboard_event, mouse_event,
  mouse_multi_wheel_event, mouse_wheel_event, mutation_event,
  mutation_name_event, text_event, ui_event
- 57 `src/html/`: one per element type, plus `html_collection.c`,
  `html_document.c`, `html_element.c`, `html_form_controls_collection.c`,
  `html_options_collection.c`
- 1 `bindings/hubbub/parser.c`

## C99 / CW8 incompatibilities

### 1. `inline` keyword, 21 files

```
include/dom/core/{attr,characterdata,document,document_type,element,
                  node,string,text}.h
include/dom/html/{html_document,html_element}.h
include/dom/events/event_target.h
src/core/{element.h,node.h,node.c,string.c}
src/events/event.h
src/html/html_document.c
src/utils/{hashtable.c,list.h,walk.c}
```

Total: ~50 `static inline` definitions across 21 files. Same fix as the
prior libraries, `#define inline` is already in `macsurf_prefix.h` from
the parserutils port. **No prefix change needed.**

Notable: many of these are `static inline` accessor wrappers in **public
headers** (e.g. `dom_string_ref`, `dom_node_ref`, `dom_node_unref`).
Every translation unit that includes those headers will see the inline
definition; under `#define inline` they become regular `static`
definitions duplicated across compilation units. CW8's linker handles
identical-static duplication fine; the only cost is a slight binary
bloat.

### 2. `//` comments, none

Same situation as the previous two libraries. Both
`grep -E '^[[:space:]]*//'` and `grep -E ';[[:space:]]*//[^/]'` returned
zero matches across all 287 in-scope files. Every `//` substring is the
URL `http://www.opensource.org/licenses/mit-license.php` inside `/* */`
license headers.

**Step is a no-op.**

### 3. C99 designated initializers, **REAL, 5 instances in 3 files**

This is the **first time** we have hit real C99 designated initializers
in any of the NetSurf libraries we have audited. CW8 strict C89 will
reject these.

```
src/html/html_table_element.c:468        struct dom_html_element_create_params { .type = ..., .doc = ..., .name = ..., .namespace = ..., .prefix = ... }
src/html/html_table_element.c:541        (same struct, different element type)
src/html/html_tablerow_element.c:378     (same struct, TD)
src/html/html_tablesection_element.c:241 (same struct, TR)
bindings/xml/libxml_xmlparser.c          (EXCLUDED — libxml binding not built)
```

**Good news:** all 4 in-scope instances initialize the **same struct**
(`dom_html_element_create_params`) and the field order in the
initializers **exactly matches the struct layout**. The struct definition
in `src/html/html_element.h:50`:

```c
struct dom_html_element_create_params {
    dom_html_element_type type;
    struct dom_html_document *doc;
    dom_string *name;
    dom_string *namespace;
    dom_string *prefix;
};
```

**Fix shape:** mechanical conversion to positional initializers. Each
`{ .type = X, .doc = Y, .name = Z, .namespace = W, .prefix = V }` becomes
`{ X, Y, Z, W, V }`. Five edits across three files. Zero semantic
change, zero ordering risk because the source already lists fields in
struct order.

Alternative if we ever feel paranoid: convert each to a temp `params`
followed by member assignments (`params.type = X; params.doc = Y; ...`).
More verbose but bulletproof against future struct reordering.

### 4. `<stdint.h>` and `<stdbool.h>`, used pervasively

- 78 files include `<stdbool.h>`
- 2 files include `<stdint.h>` directly
- 10 files include `<inttypes.h>`
- 143 files use `bool`/`uint8_t`/`uint32_t` somewhere

Same situation as libparserutils and libhubbub. Already covered by:

- `frontends/macos9/shims/stdint.h`, provides all the integer types
- `frontends/macos9/shims/inttypes.h`, forwards to `stdint.h`
- The MacSurf prefix file's predefines block MSL's C++ stdint chain
- `<MacTypes.h>` provides `bool`/`true`/`false`

**No new shim work required.**

### 5. `<inttypes.h>`, `PRIu32` macro IS used

Unlike libparserutils and libhubbub, libdom **does** use a `PRI*` macro.
Two call sites in `src/html/html_element.c`:

```c
src/html/html_element.c:521  if (snprintf(numbuffer, 32, "%"PRIu32, value) == 32)
src/html/html_element.c:610  if (snprintf(numbuffer, 32, "%"PRIu32, value) == 32)
```

Used to format a `uint32_t` (table column count, etc.) into a string.

**Existing infrastructure:** `frontends/macos9/shims/inttypes.h` already
defines `PRIu32` as `"lu"` (line 18), which is correct on CW8 PPC where
`uint32_t` is `unsigned long`. **No fix needed**, the existing shim
handles it.

### 6. `snprintf`, 1 in-scope file uses it

```
src/html/html_element.c        2 calls (lines 521 and 610, see §5 above)
bindings/xml/libxml_xmlparser.c 2 calls (EXCLUDED with the xml binding)
```

`snprintf` is C99. CW8's MSL **does** provide it as a non-standard
extension, it ships in `MSL C.Carbon.Lib` and is declared in `<stdio.h>`.
Confirmed by the fact that other NetSurf core files in the existing
build (`utils/log.c`, etc.) already use it without issue.

**No fix expected.** If CW8 surprises us, the fallback is to write the
two integers via `sprintf` into a buffer that's known to be large enough
(`uint32_t` max digit count is 10, plus null = 11, and the buffer is 32
bytes). Trivial workaround.

### 7. `<time.h>`, 1 file uses `time(NULL)`

```
src/events/event.c:11         #include <time.h>
src/events/event.c:257        evt->timestamp = time(NULL);
```

This is the only `<time.h>` use in libdom. Used to set the `timestamp`
field on a freshly-created `dom_event`. The value is informational, DOM
event listeners can read `event->timestamp` to see when the event was
created.

**Concern:** the MacSurf prefix file (`macsurf_prefix.h:18-23`) has a
specific note that `<time.h>` cannot be included from inside the prefix
file because CW8 finds NetSurf's `utils/time.h` first. That comment is
about the prefix-file context specifically; from a regular `.c` file
post-prefix, `<time.h>` should still resolve to MSL's `<time.h>` via the
SystemSearchPath because none of the user search paths put `utils/` on
the include path under that name.

**Likely outcome:** works as-is. **First-build risk:** if the include
order does end up finding `utils/time.h` instead of MSL's, we'll need to
either include MSL's time.h directly via a CW8-specific path, or replace
`time(NULL)` with `clock()` (also C89, also in MSL), or stub it to
return 0. Worth flagging.

### 8. POSIX dependencies, none

```
<unistd.h>      not used
<sys/stat.h>    not used
<sys/mman.h>    not used
<fcntl.h>       not used
<sys/types.h>   not used
<dirent.h>      not used
<iconv.h>       not used
<errno.h>       not used
<strings.h>     not used (libdom does not use strncasecmp)
```

**Zero POSIX dependencies in libdom.** Cleanest of any of the audited
libraries on this front.

### 9. Cross-platform headers used by libdom

Complete `#include` list across all in-scope source/header files:

**Standard C89 / will work on CW8:**
```
<assert.h>
<ctype.h>
<stddef.h>
<stdio.h>
<stdlib.h>
<string.h>
<time.h>          (one file — see §7)
```

**Need a shim (already exist):**
```
<stdbool.h>
<stdint.h>
<inttypes.h>      (PRIu32 used — see §5)
```

**Excluded by skipping `bindings/xml/`:**
```
<expat.h>         (libxml2/expat — EXCLUDED with the binding)
<libxml/parser.h>
<libxml/SAX2.h>
<libxml/xmlerror.h>
```

**libdom internal:**
```
<dom/...>             ← public headers from include/dom/
"core/...", "html/...", "events/...", "utils/..."  ← internal relative includes
```

**Dependencies on already-ported libraries:**
```
<libwapcaplet/libwapcaplet.h>
<parserutils/...>
<hubbub/...>
```

All resolve via the existing `MacSurf.mcp` user search paths from the
parserutils and libhubbub ports.

## What is NOT a problem

I checked for and did not find:

| Feature | Result |
|---|---|
| `__VA_ARGS__` variadic macros | Not used |
| For-scope declarations | Not used |
| `restrict` keyword | Not used |
| Compound literals | Not used |
| `long long` | Not used |
| Flexible array members | Not used |
| Forward enum declarations | Not used |
| Variable-length arrays | Not used |
| `__attribute__` / `__builtin_*` | Not used |
| `vsnprintf` | Not used |
| `%zu` printf format | Not used |
| `iconv` | Not used |
| `<errno.h>` | Not used |
| `<strings.h>` | Not used (no `strncasecmp` in libdom) |
| Build-time codegen (`gperf`, perl scripts, `.inc` files) | **None, zero codegen anywhere in libdom** |

This is the cleanest library for build dependencies, **no codegen at
all**. The Makefile has no generation rules, no `.inc` includes, no
gperf, no perl. Everything libdom builds from is in the tree.

## Existing MacSurf shims that conflict

### `frontends/macos9/dom/dom.h`, **MAJOR conflict**

This is a **monolithic 1,638-line stub** that recreates ~942 typedefs,
struct decls, and function declarations from libdom's API. It was built
during the v0.1 stub-everything-out phase to let NetSurf core compile
without libdom available.

```
$ wc -l browser/netsurf/frontends/macos9/dom/dom.h
1638
```

Header sections (excerpted):

```
1. Bool compat
2. Integer types
3. Forward declarations
... (30 sections in total)
```

**Once the real libdom headers are on the include path
(`{Project}/../../../../libdom/include`), this stub will collide with
them at every level**, duplicate `dom_string` typedef, duplicate
`dom_node` typedef, duplicate every function prototype. Compile errors
will be in the thousands.

**Fix shape:** delete `browser/netsurf/frontends/macos9/dom/dom.h` and
the now-empty `browser/netsurf/frontends/macos9/dom/` directory as part
of the wiring step. The real libdom headers take over via the include
path. Same playbook as the parserutils stub deletion.

**This is the largest single deletion in any port so far.**

### `frontends/macos9/html/{html.h,form_internal.h}`

Same situation as during the libhubbub audit. These are
**NetSurf-core HTML content handler stubs**, not libdom stubs. They
define `struct html_stylesheet`, `struct content_html_object`, etc. ,
types used by `content/handlers/html/html.c`, which is a layer above
both libhubbub and libdom.

**They will need attention when we wire `html_init` into NetSurf core**
(later milestone). They are not directly conflicting with libdom
headers because libdom uses the `dom/` namespace and these stubs use
the `html/` namespace. **Out of scope for this milestone.**

### No `frontends/macos9/libdom/` directory exists

There is no separate `libdom/` directory under `frontends/macos9/`.
Only the `dom/` stub above. Single point of conflict.

## Dependency layering inside libdom

Approximate internal dependency graph (depends on libwapcaplet,
libparserutils, libhubbub throughout):

```
Tier 0 (leaf, depends only on libwapcaplet/parserutils/system):
  src/utils/hashtable.c
  src/utils/list.h
  src/utils/namespace.c
  src/utils/validate.c
  src/utils/walk.c
  src/core/string.c

Tier 1 (depends on Tier 0 + dom_string):
  src/core/attr.c
  src/core/characterdata.c
  src/core/typeinfo.c
  src/core/tokenlist.c

Tier 2 (depends on Tier 1 + node infrastructure):
  src/core/node.c
  src/core/nodelist.c
  src/core/namednodemap.c

Tier 3 (depends on Tier 2 — leaf node types):
  src/core/cdatasection.c
  src/core/comment.c
  src/core/doc_fragment.c
  src/core/element.c
  src/core/entity_ref.c
  src/core/pi.c
  src/core/text.c

Tier 4 (depends on Tier 3 — document):
  src/core/document.c
  src/core/document_type.c
  src/core/implementation.c

Tier 5 (depends on Tier 4 — events):
  src/events/event.c
  src/events/event_listener.c
  src/events/event_target.c
  src/events/dispatch.c
  src/events/custom_event.c
  src/events/document_event.c
  src/events/keyboard_event.c
  src/events/mouse_event.c
  src/events/mouse_multi_wheel_event.c
  src/events/mouse_wheel_event.c
  src/events/mutation_event.c
  src/events/mutation_name_event.c
  src/events/text_event.c
  src/events/ui_event.c

Tier 6 (depends on Tier 5 — HTML element infrastructure):
  src/html/html_collection.c
  src/html/html_options_collection.c
  src/html/html_form_controls_collection.c
  src/html/html_element.c
  src/html/html_document.c

Tier 7 (depends on Tier 6 — the 52 individual element files):
  src/html/html_anchor_element.c
  src/html/html_applet_element.c
  src/html/html_area_element.c
  ... (49 more, in any order — they don't depend on each other)
  src/html/html_ulist_element.c

Tier 8 (depends on Tier 7 — the libhubbub bridge):
  bindings/hubbub/parser.c
```

**Total: 95 `.c` files to add to MacSurf.mcp in tier order.**

## What is required to ship this on the Mac

The porting work breaks down into **eight** concrete steps.

1. **Convert designated initializers to positional** in 3 files:
   - `src/html/html_table_element.c` (2 instances at lines ~468, ~541)
   - `src/html/html_tablerow_element.c` (1 instance at line ~378)
   - `src/html/html_tablesection_element.c` (1 instance at line ~241)
   Mechanical edit; field order already matches struct layout. Total
   ~25 lines changed across the three files. Same edits should be done
   on the upstream files in-place, these are vendored libraries and we
   own them in this repo.

2. **No `inline` work needed**, `#define inline` already in
   `macsurf_prefix.h`.

3. **No `//` comment work needed**, zero real line comments in libdom.

4. **No new shim required.** All `#include`d headers are already covered
   by the existing shim layer. (This is the first port that adds zero
   new shims.)

5. **Delete `browser/netsurf/frontends/macos9/dom/dom.h`** (1,638 lines)
   and the now-empty `frontends/macos9/dom/` directory. The real libdom
   headers take over via the include path.

6. **Verify `<time.h>` resolves to MSL** in `src/events/event.c` on the
   Mac side. This is a first-build risk; on Linux gcc finds the system
   `<time.h>` and the file builds clean. If CW8 finds NetSurf's
   `utils/time.h` instead, swap to a `time(NULL)` → `clock()` fallback
   or stub the timestamp to 0.

7. **No build-time codegen**, libdom has none.

8. **Wire libdom into `MacSurf.mcp`:**
   - Add 3 user search paths:
     `{Project}/../../../../libdom/include`,
     `{Project}/../../../../libdom/src`,
     `{Project}/../../../../libdom/bindings`
   - Add 95 `<FILE>` entries in tier order
   - Add 95 corresponding `<FILEREF>` entries in `LINKORDER`
   - Insert after the libhubbub block, before the NetSurf Core Utils
     block, so the link order resolves bottom-up
     (parserutils → hubbub → dom → netsurf core)

## What this audit does NOT cover

Out of scope for this document, deferred to follow-ups:

- Actually doing any of the eight steps above (this is research only).
- Auditing `bindings/xml/` to confirm we can safely skip it. We assume
  `libxml2`/`expat` are unavailable on OS 9 and that NetSurf core's
  `content/handlers/html/html.c` only needs `bindings/hubbub/parser.c`
  to feed parsed HTML into the DOM. **First-build risk:** if
  `content/handlers/html/html.c` needs `bindings/xml` symbols too, the
  link will fail and we'll revisit.
- Whether the existing `frontends/macos9/html/` stubs need to be retired
  once libdom + libcss + NetSurf HTML content handler are wired. Out of
  scope until later milestones.
- The `frontends/macos9/dom/dom.h` stub might be referenced by other
  files in the existing MacSurf build. A quick grep before deletion will
  confirm whether anyone `#include`s it directly. If they do, those
  references need to be migrated to the real `<dom/dom.h>`.
- libdom's `dom_event_target` interface is a virtual-method-through-vtable
  abstraction. NetSurf core is the consumer; libdom itself is fine. No
  porting work needed.
- The `dom_string` API has both interned and computed variants. The
  existing `lwc_stub.c` provides interning; libdom's `string.c` provides
  computed strings. We assume they coexist correctly because that's what
  upstream libdom does. Worth verifying at first build.

## Bottom line

libdom is **the biggest library so far but not the hardest**. The audit
checklist:

- **Zero** build-time codegen (cleanest of any audited library on this
  front).
- **Zero** new POSIX dependencies.
- **Zero** new shims required.
- **Five real C99 designated initializers** to convert by hand, first
  time we've hit this, but the conversion is mechanical and risk-free
  because field order already matches struct layout.
- **One major stub deletion** (1,638 lines in
  `frontends/macos9/dom/dom.h`), the largest deletion in any port so
  far, but the stub was always meant to be temporary.
- **95 `.c` files to add** to `MacSurf.mcp`, the largest tier list yet,
  but per-file structure is repetitive (one file per HTML element type).

Hardest parts will be:

1. **The CW8 project file edit**, 95 `<FILE>` and 95 `<FILEREF>`
   entries is a lot of XML. Probably easier to write once with a
   loop-and-template approach than to hand-edit, but the result is the
   same.
2. **First-build link errors** when the real libdom headers replace the
   monolithic stub. Anything in MacSurf or NetSurf core that referenced
   the stub's slightly-different type signatures will need to migrate.
3. **`<time.h>` resolution** in `src/events/event.c` is the only real
   correctness risk. Watch for it on first build.

Most likely first-build failure is **a residual reference to a typedef
that existed in `dom/dom.h` but does not match the real libdom header**.
That's a discoverable, fixable error class. No deep architectural
unknowns here.

## Files

- Audit subject: `browser/libdom/`
- Stub to delete: `browser/netsurf/frontends/macos9/dom/dom.h`
  (1,638 lines, ~942 declarations)
- Existing prefix file: `browser/netsurf/frontends/macos9/macsurf_prefix.h`
  (no change needed)
- CW8 project file to update: `browser/netsurf/frontends/macos9/MacSurf.mcp`
- Three files needing designated-init conversion:
  - `browser/libdom/src/html/html_table_element.c`
  - `browser/libdom/src/html/html_tablerow_element.c`
  - `browser/libdom/src/html/html_tablesection_element.c`
- Excluded subdirectory: `browser/libdom/bindings/xml/` (libxml2/expat,
  unavailable on OS 9)
- Companion audits: `docs/research/parserutils-port.md`,
  `docs/research/libhubbub-port.md`, same structure, same playbook.
