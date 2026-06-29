# QuickJS DOM port — Phase One scope

**Date:** 2026-06-29. **Status:** scope/plan only — no code yet.
**Depends on:** [teardown-ordering-audit.md](teardown-ordering-audit.md),
[nsgenbind-quickjs-scope.md](nsgenbind-quickjs-scope.md) (Q4 = port the hand-written
lifecycle pattern, not the generator).

Phase one proves the **lifecycle bridge** on the smallest real DOM surface. It does
**not** deliver DOM coverage — coverage is phase two onward, built on a validated
bridge. Success = `appendChild`/`removeChild`/`parentNode` round-trip with correct
node identity, on real libdom nodes, with no UAF across all three teardown paths.

## 0. Current state (what phase one replaces)

`macsurf_qjs.c` is **not** a pre-finalizer mock. It already has:
- a real element class `s_el_class_id` (1560-1562) with finalizer `qjs_el_finalizer`
  (715) that calls `dom_node_unref` on the opaque node;
- real libdom-backed `createElement` / `qjs_wrap_element` (725) / `qjs_wrap_element_full`
  (1426); the element methods read the node via `JS_GetOpaque(..., s_el_class_id)`.

Two gaps, and they are coupled:
1. **No node-identity map.** Every `qjs_wrap_element` mints a *fresh* `JS_NewObjectClass`
   object. One `dom_node` → N wrappers. Refcount stays balanced (each wrapper consumes
   one owned node ref; its finalizer drops one), so it doesn't crash — but identity is
   broken: `el.parentNode !== el.parentNode`, and `parent.removeChild(child)` can't
   match the child JS already holds. **This is the hiddenscroll/removeChild bug.**
2. **No explicit owner-document keepalive.** Document survival currently rests on
   libdom's intrinsic node→owner-doc ref, not on a structural guarantee the binding owns.

## 1. The atomic-unit constraint (the spine of this plan)

**The node-identity map, the real wrappers, the finalizer, and the owner-document
keepalive are ONE unit and must land in a single change. Never wrappers-first.**

Why (from the audit, and sharpened by §0): MacSurf documents are content-scoped (freed
at `html_destroy`, html.c:1915) but wrappers are heap-scoped (one QuickJS context per
window, finalized only at `js_destroyheap`→`JS_FreeContext` or the per-navigation realm
reset). A content can die while the heap lives (subframe / reload / navigation). The
audit proved this ordering **cannot** be fixed by sequencing — only the per-wrapper
owner-document keepalive makes it safe, by holding the document alive as long as any
wrapper references it, regardless of which scope dies first.

Sharpened by the real code: the finalizer that unrefs the node **already exists**. The
only thing keeping that non-crashing today is the per-wrapper ref balance — and
introducing a lookup-then-create cache (which §2 must do for identity) *changes that ref
contract*. So you cannot add the map without simultaneously owning the ref math, and you
cannot own the ref math safely without the keepalive. They are mutually load-bearing:
- the **map** makes identity correct AND guarantees the finalizer runs **exactly once**
  per node (≤1 wrapper/node);
- "exactly once" is the precondition that makes the **keepalive's** balanced
  `ref(node)+ref(owner_doc)` / `unref(node)+unref(owner_doc)` sound.

Staging "map now, keepalive later" or "wrappers now, identity later" reintroduces
exactly the UAF the audit proved is structurally unavoidable by ordering. The plan
forbids it.

## 2. Node-identity map + ref contract (concrete)

**Structure.** Per-context map keyed by `dom_node *` → **weak** `JSValue` (the map does
NOT `JS_DupValue`; it stores the raw `JSValue` so the wrapper can be GC'd). C89/CW8:
open-addressing hash or sorted-insert array of `{ dom_node *node; JSValue weak; }`;
size it for a real page (start 1024 slots, grow x2). Lives on `g_heap` so its lifetime
matches the context that owns the wrappers (see §4 for realm reset).

**`qjs_wrap_node(ctx, node)` — lookup-then-create, BORROWS `node`:**
1. `node == NULL` → `JS_NULL`.
2. (guard, §3) validate `node`.
3. lookup `node` in map. **Hit** → `JS_DupValue(map.weak)` and return (same object →
   identity holds). **Miss** → continue.
4. create `JS_NewObjectClass(ctx, s_el_class_id)`; on exception return `JS_NULL` (no
   refs taken).
5. `dom_node_ref(node)` (wrapper's own ref) **and** `dom_node_ref(owner_doc)` where
   `owner_doc = dom_node_get_owner_document(node)` (the keepalive). Store BOTH the node
   and the owner_doc pointer in the wrapper (opaque struct, not a bare `dom_node*`, so
   the finalizer can drop both — see below).
6. insert `{node, weak=obj}` into map.
7. return `obj`.

**Ref contract change.** Today `qjs_wrap_element` *consumes* an owned ref. Phase one
flips the contract to **borrow**: `qjs_wrap_node` takes its own refs in step 5 and the
caller keeps owning whatever ref it had. Every current call site that passes an owned ref
(e.g. the `dom_node_ref(child)` before wrap at 1650, the `parent`/`sib` owned refs in the
parentNode/sibling accessors) must be updated to drop its own ref after wrapping. This
contract flip is part of the atomic change; getting it wrong is the double-unref vector,
so each converted call site is listed and reviewed.

**Opaque payload.** Replace the bare `JS_SetOpaque(obj, el)` with a small heap struct
`qjs_node_priv { dom_node *node; dom_document *owner_doc; uint32_t gen; }` set as the
opaque. `JS_GetOpaque` returns it; accessors read `priv->node`. The finalizer frees the
struct after dropping refs.

**Finalizer `qjs_el_finalizer` (rewritten):**
1. `priv = JS_GetOpaque(val, s_el_class_id)`; `priv == NULL` → return.
2. remove `priv->node` from the map (so a freed wrapper is never re-dup'd).
3. `dom_node_unref(priv->node)`; `dom_node_unref(priv->owner_doc)`.
4. `free(priv)`.
Runs **exactly once** because the map guarantees ≤1 wrapper per node. That is the
double-finalize prevention; there is no other path that frees `priv`.

## 3. Pointer-validate guard — ON from the start

The donor defaults `QJS_VALIDATE_DOM_POINTERS` **off**; the audit + this session's UAF
history require it **on** for the cooperative-scheduler build. Bake it into phase one,
not a later hardening pass. `qjs_get_node(val)` (the single chokepoint every accessor
goes through) validates before returning `priv->node`:
- non-NULL, pointer-aligned (4-byte), within the heap range used elsewhere in the
  frontend (the `LLCACHE_OBJECT_WILD` range pattern);
- **generation check:** `priv->gen` vs the context's current generation token (the
  generation infra already exists but is unwired). A stale wrapper surviving a realm
  reset (it shouldn't — §4 — but defense in depth) fails the gen check and the accessor
  throws/returns null instead of dereferencing.
On failure: return `JS_NULL` / `JS_ThrowTypeError`, never dereference.

## 4. Teardown-path coverage — all three audited paths

The keepalive must make each safe; state each explicitly.

1. **Content destroy** (`html_destroy`, html.c:1905→1915): `js_destroythread` is
   `free(thread)` (frees no wrappers); then `dom_node_unref(document)`. The document
   does **not** free here because every live wrapper holds a `dom_node_ref(owner_doc)`.
   Document survives until its last wrapper finalizes. ✓ via keepalive.
2. **Window destroy** (`browser_window.c` 1952 content release → 1966 `js_destroyheap`):
   document unref at 1952 is outlived by wrapper keepalive refs; `JS_FreeContext` at 1966
   then runs every wrapper finalizer, each dropping node + owner_doc; the document frees
   when the last one runs. ✓ via keepalive (order irrelevant, as the audit requires).
3. **Navigation realm reset** (`js_newthread`, macsurf_qjs.c:2793-2804) — the subtle one,
   audit-flagged "timing-dependent" because the realm is *reused*, not torn down cleanly.
   Current order: flush timers → `JS_FreeContext(old)` → build fresh ctx. **Required
   behavior to confirm in implementation:** `JS_FreeContext(old)` must run **all** old
   wrappers' finalizers synchronously, so every old node + owner_doc ref is dropped and
   the map is emptied *before* the fresh context wraps anything. The map lives on
   `g_heap` and is shared across the reset, so the invariant is: after `JS_FreeContext(old)`
   returns, the map MUST be empty (assert/log it). If QuickJS leaves any object alive past
   `JS_FreeContext` (it shouldn't for a context with no surviving runtime roots), those
   entries would leak node+doc refs into the next page — so the implementation explicitly
   checks map-empty post-free and, if not empty, drains remaining entries (unref + clear)
   as a backstop. The keepalive guarantees no *premature* free during the window between
   old-doc content-destroy and the realm reset.

## 5. Validation plan — try to trigger the UAF the keepalive prevents

The proof is a test that frees a content while a wrapper still references one of its
nodes and confirms the document survives until the wrapper finalizes:
1. **Reload-with-live-wrapper:** script grabs `var n = document.body.firstChild` (or a
   subframe node), stash it on a long-lived JS object; trigger a reload/navigation
   (realm reset path). Confirm: old document stays valid until `n`'s wrapper is finalized
   at `JS_FreeContext(old)`; no crash; map empty after reset.
2. **Detached-node survival:** `var c = parent.removeChild(child)`; keep `c` referenced;
   navigate away (content destroy). Confirm `dom_node_unref(document)` at 1915 does not
   free the doc out from under `c`; finalizer later frees both cleanly.
3. **Identity round-trip (functional):** `el.parentNode === el.parentNode`,
   `parent.appendChild(child); child.parentNode === parent`,
   `parent.removeChild(child); child.parentNode === null` — the hiddenscroll/removeChild
   bug, now fixed.
4. **Bulk teardown:** the residual-hazard scenario — load a real XenForo page (many
   wrapped nodes), navigate; confirm no UAF in `css_computed_style_destroy`-class
   crashes and the debug log shows finalizers draining + map empty.
Instrument the map (count entries, log on grow / post-reset-empty) so each test has an
externally visible artefact, per the regression-audit checklist.

## 6. Scope boundary — the smallest surface that proves the bridge

**In phase one:** `Node`/`Element`/`Document` core only —
- `parentNode`, `appendChild`, `removeChild` (the functional target);
- whatever the identity map needs to round-trip node↔wrapper (`qjs_wrap_node` used by
  every accessor that returns a node: parentNode, siblings, firstChild/childNodes
  walk, createElement result, getElementById result);
- the map + finalizer + keepalive + pointer-validate machinery (§§2-3).

**Explicitly NOT in phase one** (research flagged these as multipliers needing separate,
larger work — a CSS-selector backend and a live-collection proxy):
- `classList` (→ DOMTokenList), `querySelector`/`querySelectorAll`,
- live collections (`childNodes`/`children` as live `NodeList`/`HTMLCollection`),
- `attributes`/`NamedNodeMap`, `style`, events beyond what already exists.
Phase one wires these node returns through `qjs_wrap_node` so identity is correct, but
does not widen coverage. Coverage rides on the validated bridge in phase two+.

## 7. Files touched (single atomic change)

- `frontends/macos9/javascript/macsurf_qjs.c` — the map, `qjs_wrap_node`,
  `qjs_get_node` guard, rewritten finalizer + opaque payload, ref-contract flip at every
  node-returning accessor, realm-reset map-drain backstop.
- (likely) a small `qjs_node_priv` struct + map helpers kept in the same TU (no new file
  unless it grows; if a new `.c` is added, flag it for MacSurf.mcp — do not edit .mcp).
- No prefix/header change → no forced full rebuild (this is `macsurf_qjs.c`-local). Ships
  as a normal changed-file tar once the swap's prefix flip lands or independently of it.

## 8. Bottom line

Phase one is "add wrappers **safely**," and the safety is not an add-on: the identity
map and the owner-document keepalive are mutually load-bearing and ship together with the
finalizer rewrite and pointer guard as one atomic change. That is the audit's proven
constraint made concrete. The deliverable surface is deliberately tiny — parentNode /
appendChild / removeChild with real identity — because the point of phase one is to
validate the lifecycle bridge, not to cover the DOM.
