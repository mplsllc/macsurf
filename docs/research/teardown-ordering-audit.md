# JS-context / DOM teardown-ordering audit

**Date:** 2026-06-29. Prerequisite guard for the ClassicNetSurf lifecycle port.
**Verdict:** the JS-before-DOM invariant is *nominally* satisfied per-content but
**hollow** in MacSurf's single-context model. No teardown reorder can fix it; the
port **must** carry CN's owner-document keepalive. Research/analysis only — no code change.

**Attribution:** MacSurf's owner-document keepalive matches what **sempaisquad** independently worked out in ClassicNetSurf (a concurrent NetSurf-on-OS 9 effort, different scope). Credit sempaisquad (<https://github.com/sempaisquad>) as a contributor for the convergent lifecycle pattern. (Rule: "ported" only for code lifted directly; anything MacSurf reached before reading CN's source is convergent/independent.)

## What the invariant requires

A finalizer-based DOM binding (the port, item 3) registers a QuickJS finalizer per
node wrapper that calls `dom_node_unref(node)`. If the libdom document (and its
nodes) is freed **before** that finalizer runs, the finalizer dereferences freed
memory — the exact UAF family we just closed, reopened by the new layer. So:
**every wrapper must be finalized while its node is still valid.**

## MacSurf's binding shape (the thing that breaks the easy answer)

- Wrappers are **heap-scoped**: they live in the single `heap->ctx` (one QuickJS
  context per browser window). `macsurf_qjs.c`.
- Documents are **content-scoped**: freed in `html_destroy` at
  `dom_node_unref(html->document)` (html.c:1915), once per content.
- `js_destroythread` (macsurf_qjs.c:2832) is **`free(thread)` only** — it runs no
  finalizers and evicts no wrappers. `js_closethread` (2826) is a **no-op**.
- Finalizers therefore run at exactly **two** points:
  `js_destroyheap` → `JS_FreeContext` (2767), and the per-navigation realm reset
  `js_newthread` → `JS_FreeContext(old)` (2798).

Because the document is content-scoped and the wrappers are heap-scoped, a content
can be destroyed (document freed) while the window's JS heap lives on (sub-frame,
navigation, reload). **There is no teardown order that frees the heap context
before every content's document.** Reordering is not a fix.

## Teardown paths, mapped against a finalizer-based DOM

| Path | Document freed | Wrappers finalized | Order | Safe w/o keepalive? |
|---|---|---|---|---|
| Content destroy (`html_destroy`) | html.c:1915, per content | NOT here — `js_destroythread` is free-only; later at heap/realm | document **before** wrappers | ❌ dangling wrappers |
| Window destroy (`browser_window_destroy`) | 1952 `safe_hlcache_handle_release(&current_content)` → content_destroy → html_destroy | 1966 `js_destroyheap` → `JS_FreeContext` | document **before** wrappers | ❌ |
| Navigation (`js_newthread` realm reset) | old content's doc freed at its content_destroy | 2798 `JS_FreeContext(old)` | timing-dependent | ❌ |

All three free the document before the wrappers are finalized. **Currently harmless**
— today's ad-hoc wrappers (`qjs_wrap_element`) carry no `dom_node_unref` finalizer,
so `JS_FreeContext` touches no DOM. After the port, all three are UAF surfaces.

Note html.c:1905 *does* put `js_destroythread` before the document unref at 1915 —
the order looks right, but it's empty: `js_destroythread` frees nothing JS-side, so
the per-content wrappers survive in `heap->ctx` past their document's death anyway.

## Required port invariant (CN pattern, confirmed applicable)

1. **Per-wrapper owner-document keepalive.** At wrap: `dom_node_ref(node)` **and**
   `dom_node_ref(owner_document)`. Finalizer drops **both** exactly once. The
   document's refcount cannot reach 0 while any wrapper references it, so the
   document structurally outlives its wrappers — teardown order becomes irrelevant.
   This is why CN can do `unref document → JS_FreeContext` and stay safe
   (nsgenbind scope doc, Q4).
2. **Per-document wrapper eviction in `js_destroythread`** (MacSurf-specific): when a
   content dies, walk the node-identity map and `JS_FreeValue` + drop entries whose
   `owner_doc` is this thread's document. With (1) this is an *optimization* (bounds
   the shared-context node map across content lifetimes), not a correctness need.
3. **Enable `QJS_VALIDATE_DOM_POINTERS`** for the cooperative-scheduler build
   (scope doc Q4, hazard 1).

## Bottom line

The audit's load-bearing output is a **hard requirement on the port, not a fix now**:
do not attempt to make teardown safe by reordering `js_destroyheap`/content release —
in the single-context model that is both inert today and a non-solution after the
port. The owner-document keepalive (1) is mandatory; it is what makes all three
teardown paths safe simultaneously. The port (item 3) starts from this invariant.
