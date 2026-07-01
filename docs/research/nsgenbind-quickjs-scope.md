# nsgenbind QuickJS DOM Bindings — Adoption Scope & Go/No-Go

**Date:** 2026-06-29. **Verdict: NO-GO on adopting nsgenbind-generated QuickJS bindings.**
Evidence from the ClassicNetSurf + netsurf-libs source trees (another dev's NetSurf-on-OS9 port). Research/estimation only — no build changes, no committed bindings, no integration.

**Attribution:** ClassicNetSurf is by **sempaisquad** (<https://github.com/sempaisquad>), a concurrent NetSurf-on-OS 9 effort at a different scope. MacSurf developed its own DOM lifecycle largely independently; where the two converge on the same hand-written `quickjs.c` discipline, credit sempaisquad as a contributor.

## TL;DR

Adopting nsgenbind to replace MacSurf's hand-wired DOM is **not viable as a shortcut**, because the thing it would give you (class scaffolding) is the cheap part, and the parts that actually matter (libdom marshalling logic, the node-identity cache, the refcount/finalizer lifecycle — exactly MacSurf's UAF danger zone) are **not generated**. Decisively: **ClassicNetSurf itself abandoned the nsgenbind-quickjs generator** — it's unwired, doesn't build as-shipped, has no node cache — and hand-wrote the entire QuickJS DOM binding into a forked `quickjs.c` instead.

**The real takeaway:** the donor isn't the generator. It's ClassicNetSurf's **hand-written `quickjs.c` lifecycle discipline** (node-identity map + finalizer unref + per-wrapper owner-document keepalive + generation guard + ordered teardown), which is a battle-tested solution to precisely the parentNode/removeChild + cross-system-UAF problems MacSurf has. Port that *pattern* into `macsurf_qjs.c`; do not adopt the generator.

## Q1 — Generator viability
- `netsurf-libs/nsgenbind-quickjs/src/qjs-libdom*.c` emits **genuine QuickJS C** (`JSValue fn(JSContext*, JSValueConst, int, JSValueConst*)`, real `JSClassDef`/`JS_NewClass`/finalizer, `JS_DefinePropertyGetSet`, `JS_NewInt32/StringLen/ThrowTypeError`) — not Duktape with a shim. Confirmed via the `outputf()` emitter sites (`qjs-libdom-interface.c:304-360, 394-396, 559-567`; `qjs-libdom.c:334-350`).
- **Does not build as shipped:** references a phantom enum `GENBIND_METHOD_TYPE_PRIVATE` (`qjs-libdom.c:118`) absent from `nsgenbind-ast.h:46-57`. One-line fix → builds (226 KB host binary). Deps: flex + bison + C99 host compiler (pure host tool).
- **No committed QuickJS output anywhere.** Only the Duktape backend's 223 generated files exist. To see QuickJS output you must run the (fixed) generator.

## Q2 — Output volume + CW8 hostility
- Full IDL set generated → **255 files, ~84K lines**. (dom-only slice ≈ 29 files.)
- **Generated scaffolding is essentially C89-clean** — violation density ≈ 0.0–0.4 / 100 lines, concentrated in one ~200-line `binding.c` (1 designated init, 14 `//` comments, 1 `snprintf`, 1 after-statement decl). The mechanical CW8 audit is trivial.
- **The blocker is not C89.** The `.bnd` method bodies are spliced **verbatim** and they are **100% Duktape**: **1,043 `duk_*` lines across 250 of 251 files**, plus `#include "duktape.h"` in 250 files. The "QuickJS" output won't compile/link against QuickJS until every method body is rewritten to the QuickJS API. That per-interface rewrite is the dominant cost.

## Q3 — Emitter CW8-cleanliness (one-time fix vs permanent tax)
- **GOOD:** emission is a flat `outputf(fmt, …)` printf layer; the backend builds prototypes **imperatively** (`JS_SetPropertyStr`/`JS_NewCFunction`/`JS_DefinePropertyGetSet`) and emits **zero** `JSCFunctionListEntry`/`JS_CFUNC_DEF` designated-init tables (the worst C89 offender — absent by design).
- The generator's own scaffolding violations each originate in a single template literal (~22 edits across `qjs-libdom-generated.c`, `qjs-libdom-dictionary.c`, `qjs-libdom.c`). **Fix the emitter once (~2–3 person-days) → every regeneration is C89-clean. No re-audit treadmill** for scaffolding.
- The verbatim `.bnd` C fragments are author-controlled (edited once each, stay clean) — but must be written from scratch for QuickJS regardless (see Q2/Q5).
- **Verdict: the emitter is fixable once.** This question alone does *not* block adoption.

## Q4 — Refcount / lifecycle / UAF bridge  ← the decisive finding
**There are two QuickJS binding architectures, and the scoped one is dead:**
1. `nsgenbind-quickjs/src/qjs-libdom*.c` — the generator backend. **Unwired/abandoned:** no generated per-class `.c` in the tree, the QuickJS Makefile builds only `quickjs/quickjs.c`, `js_dispatch.c` never calls any `qjsky_*` symbol, and **it has no node-wrapper cache at all**. Adopting this regresses every lifecycle guarantee below.
2. `netsurf/content/handlers/javascript/quickjs/quickjs.c` — the **real** binding: a ~69K-line hand-modified QuickJS where the DOM binding was hand-written inline, with a documented `[QJS_REFCOUNT]` discipline (notes at lines 840-889). **This is what ClassicNetSurf actually ships.**

The hand-written binding's discipline is **robust and matches MacSurf's hazards**:
- **1:1 node-identity map** (`thread->event_map[]`, `qjs_node_entry`, keyed by raw `dom_node*`, **weak** JSValue) — `qjs_wrap_node` (25777), lookup-then-create.
- **Balanced ref/unref:** `dom_node_ref(node)` at wrap (25844) + a **per-wrapper keepalive ref on the owner document** (25817-25819); finalizer `js_node_finalizer` (2729) drops both exactly once.
- **Ordered teardown** (`qjs_be_destroythread`, 66660): teardown listeners → unref document → `JS_FreeContext` (runs finalizers) → destroy map. The document-outlives-children invariant holds **structurally** (per-wrapper `owner_doc` keepalive + libdom's own owner-ref in `node.c:135`), not by GC luck. `html.c:9397-9410` matches: `js_destroythread` before `dom_node_unref(document)`.
- **Generation counter** invalidates stale wrappers across reparse/reset.

**But three residual UAF surfaces — exactly where MacSurf's cooperative scheduler bites:**
1. `QJS_VALIDATE_DOM_POINTERS` **defaults to 0** (line 833) — the pointer alignment/range/refcnt sanity net in `qjs_get_node` is compiled out. Must be enabled for an OS-9 build.
2. **Raw-pointer identity map** assumes the node finalizer runs **before** libdom recycles that heap address. QuickJS refcount-GC is prompt, so mostly safe — but MacSurf's **bulk `hlcache_clean`/`content_destroy` synchronous teardown** can free a whole DOM tree outside any JS GC. Safety rests entirely on *always* driving `js_destroythread` before the document's final unref; any early-free/bulk-evict path that bypasses that order reintroduces the UAF family.
3. It's a single hand-modified 69K-line file — sound as written, not mechanically verifiable.

**This is the heart of the no-go:** the refcount/UAF bridge MacSurf needs is **not** produced by nsgenbind (its scaffolding has no cache). It lives in the hand-written binding — so the value to harvest is that *pattern*, not the generator.

## Q5 — Minimal slice
- Selectable only at **IDL-file granularity** (top `.bnd` lists `webidl "dom.idl"` etc.). `dom.idl` is **monolithic: 32 interfaces in 479 lines** — can't load "just Node+Element" without authoring a trimmed IDL.
- Phase-one interfaces (all in dom.idl; bases mandatory): EventTarget→Node→Element→Document + NodeList, HTMLCollection, DOMTokenList(+DOMSettableTokenList), NamedNodeMap, Attr, CharacterData/Text/Comment, plus folded mixins ParentNode/ChildNode/NonElementParentNode.
- **Three multipliers blow up "minimal":** (a) `classList` → DOMTokenList + DOMSettableTokenList; (b) `querySelector`/`childNodes`/`attributes` need a **CSS-selector backend** and a **live-collection proxy layer** (Duktape ships `generics.js`) — **nsgenbind generates neither**; (c) **all ~9 phase-one `.bnd` bodies are Duktape and must be rewritten in the QuickJS API** (zero `qjs_libdom` bindings exist in the tree).

## Q6 — Integration footprint
- **No QuickJS handler glue exists** in ClassicNetSurf (`quickjs/` builds only the engine); `macsurf_qjs.c` (3370 lines) is already hand-playing `dukky.c`'s role.
- Generated entry points (if adopted): `qjsky_create_prototypes(ctx)` (call from `js_newheap`), `qjsky_create_object`/`qjsky_get_private`, document via existing `qjs_set_document`.
- Footprint: ~29 generated `.c` (dom slice) to ~200 (full IDL) + 3 headers added to MacSurf.mcp; retires `qjs_wrap_element*`, the `JS_NewCFunctionData` method block, per-document accessors.
- **Must stay hand-written (NOT generated):** heap/context/realm lifecycle, **`qjs_push_node` + node-identity cache**, timers, fetch/XHR, console, bootstrap shims, and the live-collection proxy.

## Recommendation: NO-GO (generator), GO (port the hand-written pattern)

**Do not adopt nsgenbind-generated QuickJS bindings.** Reasons, all evidence-backed:
1. The nsgenbind-quickjs generator is **unwired and unfinished** (phantom-enum build break, no node cache) and **ClassicNetSurf abandoned it** for a hand-written binding.
2. nsgenbind emits only **scaffolding**; the libdom-marshalling method bodies are Duktape-only (1,043 `duk_*` lines / 250 files) and must be rewritten for QuickJS — a large fraction of what `macsurf_qjs.c` already does.
3. The **node-identity cache, selector backend, and live-collection proxy are not generated** — they're hand-work either way.
4. The **refcount/finalizer/UAF bridge — MacSurf's exact danger zone — is not generated.** Adopting the generator's cache-less scaffolding would *regress* the lifecycle guarantees.

**What to do instead (the actual win from this deep-dive):** port ClassicNetSurf's **hand-written lifecycle pattern** from `quickjs/quickjs.c` into `macsurf_qjs.c`, replacing the parallel-mock DOM with **real libdom-backed wrappers**:
- node-identity map keyed by `dom_node*` (weak JSValue), lookup-then-create;
- `dom_node_ref` at wrap + per-wrapper **owner-document keepalive ref**;
- finalizer that `dom_node_unref`s node + owner_doc exactly once;
- **generation counter** + **enable the pointer-validate guard** (given the cooperative scheduler);
- ordered teardown driven from `js_destroythread` before the document unref, and an audit that **every** MacSurf teardown path (`content_destroy`, bulk `hlcache_clean`) drives JS-thread teardown before freeing DOM.

This fixes `hiddenscroll`/`removeChild`/`parentNode` (real parent/child links) AND gives a UAF-safe lifecycle using a proven design — without finishing an abandoned generator or rewriting 250 method bodies. Phase one = Node/Element/Document core wrappers + the identity-map/finalizer machinery, scoped small enough to validate the refcount bridge before widening coverage.

**What would change the no-go:** if someone (a) finished + CW8-cleaned the nsgenbind-quickjs emitter (~2-3 pd, Q3), (b) authored a complete QuickJS `.bnd` body set from scratch, AND (c) hand-wrote the node cache + selector + proxy the generator omits — i.e. did all the hard parts by hand anyway — then the generator would only be saving the (cheap) class-registration boilerplate. The cost/benefit doesn't justify it.
