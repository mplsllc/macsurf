# MacSurf capability audit — 2026-07-16

Source-verified status of the web-platform surface, section by section. Every verdict below was
checked against code, not against docs or memory. A name appearing in a stub, a no-op, or a
capability-detection shim does **not** count as implemented.

Method: seven parallel audits over the tree at `css-coverage` HEAD (`260c554a`, post-fixes875).
Tree state: fixes846 (native XHR/fetch) shipped, HW-unverified; fixes868-875 (Promise pump,
script.onload, createElementNS, DOMParser, timer ownership) shipped.

Verdict vocabulary:

| Verdict | Meaning |
|---|---|
| **REAL** | Genuinely implemented against libdom / libcss / the network, works |
| **PARTIAL** | Real foundation, named gaps |
| **STUB** | Symbol exists, does nothing (or lies) |
| **ABSENT** | No code at all |
| **WRONG** | Present and produces incorrect behaviour — worse than absent |

---

## The short version

Three findings outrank everything else on the checklist, because they are **defects in shipped
behaviour**, not missing features:

1. **Real clicks never reach JS.** The bridge is a `return 0;` stub. Every `onclick` /
   `addEventListener('click')` handler on the web is dead in MacSurf today.
2. **Invalid TLS certs silently downgrade to cleartext HTTP.** Full BearSSL validation runs, then
   the fetcher discards the result and refetches over `http://`.
3. **`cloneNode()` returns the element itself**, so the universal clone-and-append idiom *moves*
   the original node instead of copying it.

Beyond those, the pattern across the audit is consistent: **the C-side plumbing is usually real and
the JS-side exposure is usually fake.** libdom traversal, libcss cascade, the box tree, and the
event system all exist and work — they are simply not wired to the bindings. That is the roadmap's
centre of gravity, and it means most items below are wiring work, not engine work.

---

## Storage

| Item | Verdict |
|---|---|
| localStorage | PARTIAL — in-memory only |
| sessionStorage | PARTIAL — in-memory, not session-scoped |
| IndexedDB | STUB — deliberate failing shim |
| Storage partitioning by origin | ABSENT |
| Cookie policy controls | PARTIAL (third-party) / ABSENT (SameSite) |

**localStorage / sessionStorage** are a JS-eval'd shim over a plain object
(`macsurf_qjs.c:4301-4314`, `this._m={}`). The method surface is complete and correct
(getItem/setItem/removeItem/clear/key/length) but there is no C backing and no persistence: grep for
`_Storage` across `frontends/macos9` hits only `macsurf_qjs.c`, and `macos9_disk_cache.c` has zero
storage references. Data dies with the JS context — i.e. on every navigation. Both are constructed
fresh at context init, so sessionStorage has no lifetime distinct from localStorage; the two are
behaviourally indistinguishable, which is not the spec. The `storage` event is absent.
`docs/status.md:7` and `wiki/The-JavaScript-Engine.md:62` describe this as done — true only of the
method surface.

**IndexedDB** (`macsurf_qjs.c:4555-4586`) sits under a comment reading "capability-detection stubs".
`open()` asynchronously fires `onerror` with a hardcoded `{name:'UnknownError',message:'unsupported'}`;
`deleteDatabase` fires success without doing anything; `databases()` resolves `[]`; `IDBKeyRange`
methods return empty objects. Only `cmp` is real. It exists so feature detection doesn't throw.

**Storage partitioning** is absent and currently moot — nothing is persisted, so there is no
cross-origin store that could be partitioned. No origin key, no lookup by origin, no keying
structure anywhere.

**SameSite: absent.** `grep -rniE "samesite|same_site" browser/netsurf/` returns **zero hits tree-wide**.
Not parsed, not stored, not enforced; `struct cookie_internal_data` (`content/urldb.c:124`) has no
field for it.

**Third-party cookies: partial, and it's inherited upstream code.** The only mechanism is the
RFC 2109 §4.3.5 unverifiable-transaction check: `content/fetch.c:824-834` passes the referer to
`urldb_set_cookie` only when `fetch->verifiable` is false, and `content/urldb.c:3939-3962` then
requires a domain match. That blocks some third-party cookie *setting* on subresource fetches. It
does not block *sending* — `macos9_http_fetcher.c:588` and `macos9_tls_fetcher.c:1940` both call
`urldb_get_cookie(c->url, true)` with no origin argument (the second param is `include_http_only`,
not a third-party flag; `content/urldb.h:152`). No user-facing policy control exists:
`desktop/options.h:166-170` exposes only `cookie_file` / `cookie_jar` paths.

**Adjacent, high-reach:** `document.cookie` is hardcoded to `''` (`macsurf_qjs.c:3834`). JS has no
read or write access to the jar at all, even though the jar itself is real and disk-persistent
(`urldb.c:4409` load / `urldb.c:4578` save). Session-detection code on real sites reads
`document.cookie` constantly.

> The only real, persistent, C-backed store in this section is the HTTP cookie jar. Every Web
> Storage API is a JS shim inside `macsurf_qjs.c`.

---

## Network

| Item | Verdict |
|---|---|
| fetch() | PARTIAL — real async Promise, thin Response |
| XMLHttpRequest | PARTIAL — real, async-only, terminal events only |
| HTTP/2 | ABSENT |
| WebSockets | STUB — deliberate failing stub |
| CORS enforcement | ABSENT |
| Content-Security-Policy | ABSENT |
| Same-origin policy in JS bindings | ABSENT |

**fetch()** is genuinely real (`macsurf_qjs.c:4265-4299`): returns a native QuickJS `Promise`
(a real intrinsic via `JS_AddIntrinsicPromise`) wrapping the real XHR, reaching `fetch_start()` at
`macos9_js_fetch.c:420`, with resolution deferred through `macos9_schedule(0, xhr_deliver, s)`
(`macos9_js_fetch.c:364`) — truly async, never re-entrant into JS from the fetch callback. fixes846
holds up. Gaps are surface: Response has only `ok/status/statusText/url/headers.get/text()/json()`
(`:4282-4293`) — no `blob()`, `arrayBuffer()`, `formData()`, no `headers.has/forEach`, no
`Request`/`Response`/`Headers` constructors, no `redirect`/`type`, no `AbortSignal`, no
`credentials`/`mode` (opts reads only `method`/`headers`/`body`, `:4266-4274`). Non-string bodies are
stringified (`macos9_js_fetch.c:526-534`).

**XMLHttpRequest** is real on the network side: `send()` → `__xhrNativeSend`
(`macsurf_qjs.c:4246`) → `qjs_xhr_native_send` (`macos9_js_fetch.c:439`) → `fetch_start()`, with a
16-slot arena, 4 MB body cap, and correct 301/302/303/307/308 method downgrade
(`macos9_js_fetch.c:278-321`). Gaps:
- **Sync mode not implemented.** `open(...,false)` is accepted and stored (`macsurf_qjs.c:4191`),
  `_async` is passed to native (`:4247`), and the C side ignores argv[5] entirely. Delivery is
  always deferred. The comment at `macsurf_qjs.c:4172-4176` admits this.
- **Terminal events only.** `onreadystatechange`/`onload`/`onerror`/`onloadend`/`addEventListener`
  all fire, but only at readyState 4 (`__onNativeComplete`, `macsurf_qjs.c:4230-4238`; C sets
  readyState=4 at `macos9_js_fetch.c:243`). No readyState 2/3, no `progress`, no `abort` event, no
  `upload`, no `timeout`, no `withCredentials`.
- **responseType is a stored no-op** (`macsurf_qjs.c:4180`). `response` is always a JS string
  (`macos9_js_fetch.c:249`), so `json`/`blob`/`arraybuffer`/`document` are absent.
  `overrideMimeType` is an explicit no-op (`:4210`).

**HTTP/2: absent.** Both fetchers hardcode HTTP/1.1 request lines (`macos9_tls_fetcher.c:2071,2088`;
`macos9_http_fetcher.c:720,735`). ALPN exists but advertises **only http/1.1** —
`macTLS/os9/ostls_async.c:724-728` — and deliberately: the comment says it's present because
Cloudflare/HTTP2 edges require the extension, not to negotiate h2. No HPACK, no frame layer, no
`br_ssl_engine_get_selected_protocol()` call outside the BearSSL header.

**WebSockets: stub** (`macsurf_qjs.c:4564-4577`). The constructor never opens a socket;
`send()` returns false, and a `soon()` callback sets `readyState=3` and fires `onerror` then
`onclose({code:1006,wasClean:false})`. No `ws://`/`wss://` handling, no RFC 6455 framing, no
`Sec-WebSocket-Key` anywhere.

**CORS: absent.** Zero occurrences of `Access-Control-Allow-*`, preflight, or `OPTIONS` generation
tree-wide. `xhr_fetch_cb` accumulates response headers raw (`macos9_js_fetch.c:333-338`) and hands
the lot to JS via `__responseHeadersRaw` (`:252`) with no safelist filtering.

**CSP: absent.** No parser, no enforcement. The string appears twice, both as prose in buffer-sizing
comments (`macos9_tls_fetcher.c:59,64`). NetSurf core has none either.

**Same-origin policy: absent — and this is a live exposure.** `qjs_xhr_native_send` resolves against
the document base and fetches unconditionally (`macos9_js_fetch.c:486-497`); `responseText`/`response`
are set from the raw buffer with no check (`:247-249`); the fetcher auto-attaches cookies for
whatever host is targeted (`macos9_js_fetch.c:12-14`). **JS on any page can read any cross-origin
response with the user's credentials.** Compounding it, the `Sec-Fetch-*` headers are hardcoded
static strings, not computed from the real origin relationship — `Sec-Fetch-Mode: no-cors`
(`macos9_tls_fetcher.c:2021`, `macos9_http_fetcher.c:628`) and `Sec-Fetch-Site: same-origin`
(`macos9_tls_fetcher.c:2008,2022`; `macos9_http_fetcher.c:615,629`) — so MacSurf *asserts*
same-origin on every request regardless of truth.

What limits the blast radius today is omission, not policy: cross-frame DOM access isn't exposed at
all. `window.parent`, `window.top`, `window.frames`, `contentDocument`, `contentWindow` and
`postMessage` are all undefined (the only `parent` hits are C-side DOM params and a comment at
`macsurf_qjs.c:1204` describing "the window.parent work" as future). Each frame gets its own
JSRuntime/context (`macsurf_qjs.c:537`, `:74`) with no bridge. **Anything that adds cross-frame
access must land origin checks in the same round.**

Known hazard flagged in-tree at `macsurf_qjs.c:1195-1209`: the wrapper map is file-static and shared
across every runtime, so an iframe realm reset may release the parent's wrappers; the code
instruments `foreign` to decide whether it bites. See `[[project_hackaday_jquery_and_runtime_split]]`.

---

## JS engine surface

| Item | Verdict |
|---|---|
| Promises → event loop / microtask queue | REAL |
| setTimeout / setInterval | PARTIAL |
| requestAnimationFrame | PARTIAL — callback gets no timestamp |
| ES modules (`type="module"`, dynamic import) | ABSENT — and mis-handled for `src=` |
| Web Workers | ABSENT |
| History pushState / replaceState | **WRONG** — navigates the page |
| MutationObserver | STUB |
| IntersectionObserver | PARTIAL — fires once, geometry fake |
| ResizeObserver | STUB |
| Event model (capture/bubble/preventDefault) | **STUB — real clicks never reach JS** |
| DOMContentLoaded / load ordering | PARTIAL — inverted |

### Real clicks never reach JS — the headline defect

A click travels `interaction.c:1711` → `fire_generic_dom_event` → `dom_event_target_dispatch_event`.
libdom *does* implement capture and bubble correctly. But **nothing ever calls
`dom_event_target_add_event_listener`**, so there are zero JS listeners on that path. The bridge that
was meant to close the gap is a stub:

```c
/* macsurf_qjs.c:5939-5943 */
int macsurf_qjs_dispatch_dom_click(void *target) { (void)target; return 0; }
```

The comment at `interaction.c:1712-1717` claims it "dispatch[es] the click through the QuickJS
shadow-DOM event layer" — it does not, and the `js_default_prevented` check at `interaction.c:1724`
is dead code. **Every `onclick` and `addEventListener('click')` handler is dead from real mouse
input.** This is the true cause of the hackaday symptom recorded as #300 ("renders perfectly,
ignores every click"); the `"onclick" in el` detection fixed in fixes873 was one gate *in front of*
this one.

### The event model behind it

Three separate, unconnected listener registries, none of them libdom's real event target:
- element — `el._L[type]` (`macsurf_qjs.c:2133-2136`), dispatched via `el._H[type]` (`:2145-2151`)
- document — `document._listeners[type]` (`:3882-3894`)
- window — `this._winListeners[type]` (`:3976-4006`)

**Capture: absent** — `addEventListener(t,fn,opts)` accepts `opts` and ignores it entirely; no
capture, no `once`, no `passive`. **Bubbling: absent** — `dispatchEvent` only invokes listeners on the
target itself, never walks `parentNode`; `ev.bubbles` is set correctly by the `Event` constructor
(`:3769`) and read by nobody. Document-level delegation therefore works only when the page dispatches
at `document` directly. **preventDefault: name only** — sets `this.defaultPrevented=true` (`:3774`)
and nothing reads it; `stopPropagation`/`stopImmediatePropagation` are literally empty (`:3775-3776`).
**CustomEvent/dispatchEvent: real** for synthetic dispatch — `Event`, `CustomEvent` (with `detail`),
`MouseEvent`, `KeyboardEvent` all exist (`:3765-3800`) and dispatch synchronously. That is why the S0
harness looks healthy while hardware ignores clicks.

### Promises — real

`JS_ExecutePendingJob` **is** pumped, per-runtime, in `macsurf_qjs_pump_all()`
(`macsurf_qjs.c:6228-6241`), called every event-loop pass from `main.c:1414-1415`. It walks the
global heap list (each iframe/window has its own JSRuntime), drains up to `QJS_MAX_JOBS_PER_PUMP`
(256, `:529`) per pass, logs job exceptions, defers leftovers. fixes868 holds. Caveat: the drain runs
*after* `macsurf_qjs_run_timers` in the same pass and only at poll granularity, so microtasks don't
run between timer callbacks within one pump — a coarser task/microtask split than spec, but live.

### Timers

Real: `qjs_settimeout_impl` (`:645-676`), registered `:3699-3702`, fired by `macsurf_qjs_run_timers`
(`:788-885`) with index+id snapshot, per-realm ownership gating (fixes875), and a deadline kill for
runaway repeaters. Gaps:
- **Extra args dropped** — `JS_Call(qctx, fn, JS_UNDEFINED, 0, NULL)` (`:869`) passes zero args, so
  `setTimeout(fn, 0, a, b)` loses `a, b`.
- **String-code form never runs** — `setTimeout("foo()", 0)` returns 0 (`:657` requires `JS_IsFunction`).
- **64-timer cap** (`QJS_MAX_TIMERS`, `:531`); on overflow `timer_alloc` (`:608-643`) **evicts the
  soonest-expiring timer** — i.e. drops the one most likely to be needed.
- No `this` binding, no clamping/nesting rules.

**requestAnimationFrame** is a JS one-liner (`:3704-3711`): `setTimeout(fn,16)`. It does run, but
because the timer path passes zero args, **the callback receives no `DOMHighResTimeStamp`** —
`function(t){...}` sees `undefined`, so the ubiquitous `t - last` idiom yields NaN. Not tied to
paint/vsync; inherits the 64-timer arena.

### ES modules — absent, and actively mis-handled

No `JS_EVAL_TYPE_MODULE`, no `JS_SetModuleLoaderFunc` anywhere in the frontend; every eval is
`JS_EVAL_TYPE_GLOBAL` (`:211, 2168, 3348, 5633, 5835, 6092`). The loader exists only in QuickJS's
own unused `quickjs-libc.c`.
- **Inline** `<script type="module">`: `exec_inline_script` picks the handler from the `type`
  attribute (`content/handlers/html/script.c:953-959`); `"module"` isn't a registered JS MIME
  (`content/handlers/javascript/js_content.c:116-119` registers only `application/javascript`,
  `text/javascript`) → handler NULL → **silently skipped**.
- **External** `<script type="module" src=...>` is worse: `html_script_exec` picks the handler from
  the **fetched content type**, not the `type` attribute (`script.c:129-130`), so a module served as
  `text/javascript` executes as a **classic global script** and dies with a syntax error on its
  first `import`.
- **Dynamic `import()`** parses but has no loader registered → throws at runtime.

### The rest

- **Web Workers: absent.** No `Worker`/`SharedWorker`/`ServiceWorker`/`importScripts`. The only
  "Worker" hit is `XF.Push={...registerWorker:function(){}}` (`:5456`), a XenForo no-op.
  `new Worker(...)` throws ReferenceError.
- **pushState / replaceState: WRONG.** Both are `function(s,t,u){ _state=s; if(u) location.href=u; }`
  (`:4396-4412`), and `location.href`'s setter is `qjs_location_set` (`:435-452`) →
  `macos9_window_navigate`. **`pushState` with a URL triggers a full page navigation** — the exact
  opposite of its purpose. Every SPA router reloads the page. No `popstate` event exists anywhere
  (zero hits). `history.length` is hardcoded 0 (`:4392`). `history.state` getter works;
  `back`/`forward`/`go` are real C bindings (`:467-506`).
- **MutationObserver: stub** (`:3931-3937`) — `observe`/`unobserve`/`disconnect` empty,
  `takeRecords` returns `[]`, callback stored and never called. Deliberate, per the comment at
  `:3914-3916` ("firing them risks feedback loops with our own reconvert/relayout").
- **ResizeObserver: stub** — aliased to the same no-op `_Observer` (`:3938`).
- **IntersectionObserver: partial** (`:3940-3968`). `observe(el)` schedules `setTimeout(...,0)`
  delivering **one** entry with hardcoded `isIntersecting:true, intersectionRatio:1`, using
  `getBoundingClientRect()` — which is all zeros. No viewport testing, no thresholds
  (`thresholds` hardcoded `[0]`, `:3945`), no re-delivery on scroll, never reports leaving,
  `takeRecords()` always `[]`. Fires once per `observe` and says "visible" unconditionally: enough
  for lazy-load/hydration triggers, wrong for anything reading ratios.

### DOMContentLoaded / load — inverted

Both fire, in the **wrong order**. `html_finish_conversion` fires window `load` at
`content/handlers/html/html.c:701` — **before** the DOM→box conversion. Then `html_box_convert_done`
fires DOMContentLoaded at `html.c:418-427` → `js_fire_dom_ready` (`macsurf_qjs.c:5954-5987`), which
dispatches `DOMContentLoaded` at document, then at window, then `load` **at document** (never at
window). Observed order: **window load → DOMContentLoaded → document load** — the reverse of spec.

Worse: `document.readyState` is initialized to `'complete'` at realm setup (`:3833`) and set to
`'complete'` again in `js_fire_dom_ready` (`:5960`). It is **never** `'loading'` or `'interactive'`,
so the near-universal guard

```js
if (document.readyState === 'loading') addEventListener('DOMContentLoaded', init); else init();
```

always takes the immediate branch and runs `init` synchronously during parse, **before the box tree
exists**. `js_fire_dom_ready` is idempotent per realm via `document.__ms_ready_fired` (`:5958-5959`).
NetSurf's own comment at `html.c:1472` sits above no code — the upstream stub was never filled in;
the MacSurf path at `html.c:418` is the only one.

---

## DOM completeness

| Item | Verdict |
|---|---|
| createDocumentFragment | REAL |
| innerHTML= via fragment parser | REAL (write side only) |
| outerHTML | STUB — fabricates markup |
| insertAdjacentHTML | ABSENT |
| querySelector/All | PARTIAL (document) / **WRONG** (element-scoped) |
| classList | REAL |
| dataset | REAL |
| getComputedStyle | STUB — inline styles only |
| getBoundingClientRect | STUB — all zeros |
| FormData | PARTIAL — always empty for `new FormData(form)` |
| form.submit() | ABSENT |
| Constraint validation | ABSENT |

### Node traversal is hardcoded — the structural find

`qjs_el_install_native_attrs` (`:2270`) installs real natives, then calls
`qjs_el_install_js_helpers` (`:2235`), whose **last five lines overwrite the surface with
constants** (`:2161-2165`):

```js
el.cloneNode=function(){return el;};
el.contains=function(n){return false;};
el.childNodes=[];
el.firstChild=null;el.lastChild=null;
el.nextSibling=null;el.previousSibling=null;
```

Consequences:
- **`cloneNode` — STUB, and actively dangerous.** Returns `el` *itself*. So
  `parent.appendChild(node.cloneNode(true))` **moves** the original node rather than duplicating it.
  `deep` ignored. (The native `cloneNode` at `:2430` is `qjs_text_clone_node_data` — **text nodes
  only**; elements never get it.)
- **`firstChild`/`lastChild`/`nextSibling`/`previousSibling` — STUB.** Plain `null` data properties
  frozen at wrap time; never consulted libdom, never update as the tree mutates. The canonical
  clear-children idiom `while (node.firstChild) node.removeChild(node.firstChild)` sees an empty
  element and no-ops.
- **`childNodes` — STUB.** Fresh empty array per element, forever `length === 0`. Note the harness
  at `:4929` measures `r.childNodes.length` after appending 3000 children — against this array.
- **`contains` — STUB.** Always `false`.

**The C plumbing already exists**: `macsurf_dom_node_get_first_child` / `get_next_sibling` are used
freely in C (`:1546`, `:1573`, the tree walkers). This is exposure work, not engine work.

**Real:** `parentNode` (`qjs_el_get_parent_node_data`, `:1592` — a live getter in the `tc_src` block
at `:2245`, which runs *after* the helpers and therefore survives; returns null for non-element
parents, `:1604`), `children`, `nextElementSibling`, `previousElementSibling`, `textContent`,
`appendChild`, `removeChild`, `insertBefore`, `getAttribute`/`setAttribute`/`removeAttribute`/
`hasAttribute`.

> Net: **element**-oriented traversal is genuine libdom; **node**-oriented traversal is entirely
> hardcoded.

### The rest

- **createDocumentFragment: real.** `qjs_create_document_fragment` (`:2565`) →
  `macsurf_dom_document_create_document_fragment` → `qjs_wrap_fragment` (`:2475`), a real
  opaque-tagged node, nodeType 11, native append/remove/insertBefore/children/textContent/parentNode.
  Wired at `:3384`; the JS layer (`:3452`) prefers it, falling back to the old fake `mkfb('#fragment')`
  only if absent. The fragment wrapper has no `querySelector`, no `firstChild`, no `cloneNode`.
- **innerHTML= : real (write side).** `qjs_el_set_inner_html_data` (`:1506`) —
  `dom_hubbub_fragment_parser_create` → `parse_chunk` → `completed`, clears children via real
  `remove_child`, descends fragment → `<html>` → `<body>` (`:1558-1567`, working around the parser's
  missing context-element support), moves children in, marks DOM dirty (`:1588`). **Caveat:
  `enable_script = false` (`:1531`)** — injected `<script>` never runs. **The read side is fake**:
  `get:function(){return el.textContent||'';}` (`:2025`), so innerHTML never round-trips markup.
- **outerHTML: stub.** `'<'+el.tagName+'>'+el.innerHTML+'</'+el.tagName+'>'` (`:2031-2032`) — no
  attributes, no self-closing, and its `innerHTML` read is textContent. Returns invented markup.
  Getter only; assignment silently no-ops.
- **insertAdjacentHTML: absent** (also insertAdjacentElement/Text) — zero occurrences repo-wide.
- **querySelector: two different engines.**
  - *Document-level is a real matcher.* `qjs_sel_parse` (`:2738`) → `qjs_compound_match` (`:2807`) →
    `qjs_sel_match` (`:2859`), with proper whitespace-tokenized class matching (`qjs_class_has`,
    `:2688` — correctly rejects `.foo` against `class="foobar"`). Supports tag, `.class` (multiple),
    `#id`, and **descendant** combinators. Everything else — `[attr]`, `:pseudo`, `>`, `,`, `+`, `~` —
    is **swallowed and ignored** (`:2784-2788`), degrading to a tag/class/id approximation that sets
    `s.approx` and logs "APPROX selector" rather than throwing. So `a, b` silently matches a mangled
    first part, and `div > p` matches any descendant `p`.
  - *Element-scoped is much worse.* `qjs_el_qsa_data` (`:1883`) **doesn't use the parser at all** —
    it truncates the selector at the first `[`, `.`, `:` or space and calls `qjs_collect_by_tag`. So
    **`el.querySelector('.foo')` returns null** (`:1902`) and `el.querySelectorAll('div.bar')` returns
    **every** descendant div. `qjs_el_qs_data` (`:1911`) takes `[0]` of that. fixes871 fixed the
    document level only.
  - Both walkers start at `documentElement` and match the root itself; element-scoped matching
    includes the element itself.
- **classList: real** (`:1933-1959`) over `getAttribute`/`setAttribute('class')` —
  contains/add/remove/toggle/replace/toString all correct. Not a `DOMTokenList`: no `length`, no
  indexing, no iteration, no `item()`/`forEach`/`supports`.
- **dataset: real** (`:2035-2050`) — an ES6 `Proxy` with correct camelCase → `data-kebab-case`.
  Missing: `delete el.dataset.x` (no `deleteProperty` trap → attribute survives), and enumeration
  returns only trap-assigned keys, not existing `data-*` attributes.
- **getComputedStyle: stub** (`:4008-4014`). Returns `{getPropertyValue, cssText:''}` where
  `getPropertyValue` forwards to `el.style.getPropertyValue` — i.e. **reads back inline styles the
  script itself set**, from the JS-side `sc` cache. No cascade, no stylesheet, no defaults, no
  layout. Anything not set inline by the same script returns `''`.
- **getBoundingClientRect: stub** (`:2153-2154) — `return{top:0,left:0,right:0,bottom:0,width:0,
  height:0,x:0,y:0};` unconditionally, no reference to the box tree. `getClientRects` wraps the same
  zeros. `scrollIntoView`/`focus`/`blur`/`click` are empty no-ops (`:2155-2160`). Note the `mkfb`
  fallback (`:3433`) returns viewport-sized rects — **the fake elements report better geometry than
  the real ones**.
- **FormData: partial, effectively broken.** The map is real (`:4131-4162`:
  append/get/getAll/set/has/delete/forEach), but the constructor harvests via `form.elements`
  (`:4134`) and **`elements` is never defined on the element wrapper** (the only `_elements`, `:4703`,
  is an unrelated no-document fallback). **`new FormData(realForm)` always yields an empty set.**
  Also missing `entries`/`keys`/`values`/`Symbol.iterator`, File/Blob support — and no `<input>` value
  is read from the live control anyway (`value` reflects the *attribute*, `:1971-1974`; the comment at
  `:1996` acknowledges property/attribute divergence is unhandled).
- **form.submit(): absent.** No `submit`/`requestSubmit`/`reset` on the wrapper; `'submit'` appears
  only as an event-name string (`:3322`). Native form submission is unaffected (`:5937`), but JS
  cannot trigger it.
- **Constraint validation: absent.** Zero hits for `checkValidity`, `reportValidity`,
  `setCustomValidity`, `validity`, `validationMessage`, `willValidate`.

---

## Rendering / CSS

Prior ground truth: [css-gap-inventory-2026-07-13.md](css-gap-inventory-2026-07-13.md) — see the two
corrections at the end of this section.

| Item | Verdict |
|---|---|
| @font-face | PARTIAL — raw sfnt only |
| WOFF / WOFF2 | ABSENT — no brotli, no zlib |
| Canvas 2D | PARTIAL → effectively ABSENT (element only, no context) |
| CSS transitions | ABSENT — stripped before the parser |
| CSS animations / @keyframes | ABSENT — proprietary substitute exists |
| CSS transforms | PARTIAL — text-rewrite to a vendor prop |
| position: sticky | REAL — paint-time approximation, named gaps |
| Flexbox | PARTIAL — intrinsic sizing is the real gap |
| CSS Grid | PARTIAL — vendor-prop reimplementation |
| Custom properties / var() | REAL — documented scoping simplification |
| calc() | PARTIAL — essentially unresolved |
| Media queries | PARTIAL — four features |
| prefers-color-scheme | REAL — driven by a MacSurf option, not the OS |

**A structural note that governs this whole section:** much of "CSS support" here is not libcss at
all — it is **text-level rewriting in the `cssh_css.c` preprocessor** before libcss tokenizes
anything. `transform`, grid, and parts of `calc()` are synthesized into `-macsurf-*` vendor
properties by string matching; `transition`/`animation` are deleted outright. The consequence is that
anything the rewriter's string matching doesn't recognize is gone before the parser ever sees it, and
grepping libcss for a property name understates *and* overstates coverage depending on which side of
the rewriter it sits.

- **@font-face: real; WOFF2 absent.** The fetch+render path is a genuine MacSurf addition:
  `html_macsurf_font_face_url` (`content/handlers/html/html.c:470`) resolves src via
  `css_select_font_faces()` — which **stock NetSurf never calls** — and `macos9_webfont.c` parses the
  sfnt directory (head/maxp/hhea/hmtx/cmap/loca/glyf; cmap format 12 required for PUA icon glyphs),
  filling outlines as QuickDraw regions. **Only raw sfnt works.** No brotli anywhere (two comment
  mentions only: `macos9_webfont.c:14`, `html.c:468`), and no zlib inflate either —
  `macos9_webfont.c:383-384` explicitly rejects bodies starting `wOFF`/`wOF2`. The src picker
  (`html.c:500-503`) is three-pass (OPENTYPE → UNSPECIFIED → WOFF), but the WOFF pass is a dead
  future hook (`html.c:497`: "WOFF (zlib; presently rejected at parse)"); WOFF2 is never selected at
  all. **Net: `@font-face` renders only if the site ships a bare `.ttf`/`.otf`** — which in 2026 is
  approximately no site.
- **Canvas 2D: the element exists, the context does not.** `<canvas>` is recognized and
  box-constructed — `box_canvas` (`content/handlers/html/box_special.c:890`) marks it
  `IS_REPLACED | REPLACE_DIM` and suppresses children when scripting is on (fallback shows when off,
  `:895-897`). `redraw.c:4698-4710` even has a paint branch pulling a `struct bitmap` from the DOM
  user-data key `__ns_key_canvas_node_data`. **But nothing ever writes that key** — it appears in
  exactly two places tree-wide: the corestring declaration (`utils/corestringlist.h:382`) and the
  redraw read. There is **zero `getContext` binding** in the QuickJS layer (grep across
  `browser/netsurf/` returns nothing; the only hits are QuickJS's own `JS_GetContextOpaque`). No
  `CanvasRenderingContext2D`, no `fillRect`. `<canvas>` always paints empty. The paint side is
  already built and waiting for a producer.
- **CSS transitions: absent, and not "parsed-but-dropped".** No `CSS_PROP_TRANSITION*` slot exists.
  The preprocessor strips the shorthand and every longhand before libcss sees them —
  `cssh_css.c:3772-3776` lists `transition`, `-property`, `-duration`, `-timing-function`, `-delay`
  in `macsurf__rewrite_modern_compat`'s `DROP_PROPS[]`. Rationale stated at `:3766-3771`: there is no
  animation timer playback, and the final static computed value still applies via the normal cascade.
  **The bytes never reach the parser.**
- **CSS animations / @keyframes: absent as standard CSS — but a proprietary substitute ships.** All
  `animation-*` longhands plus the shorthand are in the same `DROP_PROPS[]` (`cssh_css.c:3777-3785`).
  No `@keyframes` handling exists anywhere. MacSurf instead has its own timer-driven path that
  authors must opt into **by hand**: `CSS_PROP_MACSURF_ANIMATION_OPACITY` /
  `CSS_PROP_MACSURF_ANIMATION_ROTATE` (`libcss/include/libcss/properties.h:149-150`) with real
  parsers (`p_macsurf_animation_opacity.c`, `p_macsurf_animation_rotate.c`), accessors
  (`select/computed.c:715-724`), redraw consumers (`redraw.c:1418, 1455, 2252, 2281`), and a ~10fps
  tick loop (`macos9_animation.c`, `ANIM_TICK_INTERVAL 6`). Syntax:
  `-macsurf-animation-opacity: <from> <to> <duration_ms>`, V1 ping-pong linear.
  **Nothing rewrites `@keyframes` into these**, so real-world CSS gets no animation. A rewriter
  bridging simple `@keyframes` → vendor props is a plausible cheap win.
- **CSS transforms: partial, via text rewrite.** `transform` is not native to libcss here —
  `macsurf__rewrite_transform` (`cssh_css.c:2147`) renames `transform:` → `-macsurf-transform:`
  before parsing, feeding `CSS_PROP_MACSURF_TRANSFORM` (`properties.h:147`). The value is a packed
  int consumed at `redraw.c:1439-1447` (block bg) and `:2268-2270` (inline), unpacked by
  `macos9_transform_unpack` (`plotters.c:685`) — rotation via a sin/cos LUT, translate from two
  **int8** pixel fields (`:691-692`), scale in a Q8.8 companion word `transform_b` (`:833-834`,
  identity sentinel `0x01000100`). Missing: `transform-origin`, `transform-style`, `transform-box`
  are deliberately unmatched (`cssh_css.c:2140-2142` — the post-needle scan requires `:` next, so
  those longhands fall through and are ignored); translate is capped at signed-8-bit pixels; no
  `matrix()`, no 3D; and the transform applies to background fill and glyph paint, **not** as a
  general compositing transform of the subtree.
- **position: sticky: real** (paint-time approximation). `CSS_POSITION_STICKY = 0x5`
  (`libcss/include/libcss/properties.h:1030`). Lays out as `relative`, then `redraw.c:3861-3863`
  onward clamps `x_parent`/`y_parent` against the viewport using top/bottom/left/right (viewport dims
  from `html->unit_len_ctx`, `:3875-3876`). Flex handles it per axis via
  `lh__box_is_flex_out_of_flow()` (`layout_internal.h:207-222`): out-of-flow on a row main axis,
  in-flow on a column main axis. Hit-testing has explicit sticky-overlay precedence
  (`interaction.c:719-722`, fixes656). Documented V2 limits (`redraw.c:3851-3859`): **no
  containing-block-bottom clamp** (sticky never "lets go" at its parent's bottom edge — it pins for
  the full document height), **no nested-scroll-container support** (always pins to the page
  viewport, never the nearest scrolling ancestor), and percentage offsets resolve to 0
  (`:3887-3888`).
- **Flexbox: partial — coverage is wide, sizing is the gap.** `layout_flex.c` (2198 lines) covers
  `flex-wrap` (:296), `flex-basis`/`shrink`/`grow` (:708-712), `order` (:767, :773),
  `justify-content` (:1309), `align-content` (:1639), `align-self` incl. baseline (:1608). Verified
  gaps:
  - **Intrinsic sizing** — `min-content`/`max-content`/`fit-content` don't exist as values anywhere
    in libcss, so `flex-basis: auto` resolution and shrink-to-fit are heuristic. This is the
    prerequisite named in CLAUDE.md:476.
  - **`row-gap` is not a real property** — `css__parse_gap` and `css__parse_row_gap` both emit
    `CSS_PROP_COLUMN_GAP` bytecode, so `layout_flex.c` reads `css_computed_column_gap` for *both*
    `ctx->main_gap` and `ctx->cross_gap`; two-value `gap: A B` silently discards A.
  - **Survival fallbacks mask failures silently** — `FLEX_SAFE_MAX 1000000`, `FLEX_MAX_ITEMS 512`
    (`:78-80`); unsafe input converts the container to block flow locally (`:66-77`), and unsupported
    child types get a zero-height box (`:661`, `:1808`). On a page this reads as a layout bug with no
    diagnostic.
- **CSS Grid: partial — a vendor-prop reimplementation, not spec grid.** `display: grid` runs
  `layout_grid.c` (1524 lines), driven by MacSurf vendor props (`CSS_PROP_MACSURF_GRID`
  `properties.h:148`, `_GRID_ROWS` :156, `_GRID_COL_SPAN` :157, `_GRID_FLOW` :161) that the **text
  preprocessor** synthesizes from standard syntax — `macsurf__rewrite_grid_template_columns`
  (`cssh_css.c:777`), `_grid_template_rows` (:880), `_grid_placement` (:1428), `_grid_template_areas`
  (:1900), `_grid_alignment` (:4762), `_grid_auto_flow` (:5061). **Anything the rewriter's string
  matching doesn't recognize is dropped before libcss.** Track units have grown to FR/PX/PERCENT/AUTO
  (`:322-326`; content-sized AUTO from fixes817 for #62, sized at `:621`), and `justify-items` landed
  natively (`:255`, `:1409`, #279/fixes833). Still missing: `justify-self` (**blocked** — the
  `bits[16]` array is full and must be extended first), placement/span-aware auto sizing, `minmax()`
  composition (collapsed to one token by the preprocessor at `cssh_css.c:716`), §12.8
  `align-items: stretch` default (cells leave empty space when the row track exceeds content), FR row
  distribution against a definite container height, named grid lines, negative line numbers beyond
  the `-1` fill-row sentinel, `grid-auto-flow: column`/`dense`, and subgrid. Explicit placements
  don't advance the auto-flow cursor, so an auto item can land in an occupied cell — last-wins, no
  collision avoidance (`:29-32`).
- **Custom properties / var(): real** (fixes267, a MacSurf implementation, not upstream libcss).
  `libcss/src/parse/custom_properties.c` (+ `.h`). Definitions captured at parse into the sheet's
  `custom_properties` list (`language.c:856`, `:1976` via `css__sheet_add_custom_property`).
  Declarations *referencing* `var()` can't resolve at parse time, so `language.c:1996` tests
  `css__value_contains_var()` and `:2025` attaches the verbatim token list as a `css_deferred_decl`;
  at selection `css_select.c:2847` calls `css__deferred_decl_resolve()`, which substitutes against
  the select ctx's aggregate table, re-runs the normal per-property parser, and applies via
  `prop_dispatch`. `var()` fallback (the `,` form) is handled (`custom_properties.c:614-673`).
  Element-scoped custom props are a "pragmatic V1" (`:252-266`) with an acknowledged leak:
  **cleanup on page navigation is a TODO** (`:266`). Documented simplification (`custom_properties.h:21-25`):
  every `--name` is treated as **globally scoped to its owning stylesheet** regardless of the selector
  it appeared under — per-element cascading/inheritance of custom props is approximated, not
  spec-correct.
- **calc(): partial, essentially unresolved.** libcss tokenizes `calc()` into `CSS_UNIT_CALC`
  ("Un-resolved calc()", `libcss/include/libcss/types.h:114`), set at
  `select/properties/helpers.c:304`. `select/calc.c` exists, but the value stays unresolved through
  computed style — `p_font.c:48` literally does `case CSS_UNIT_CALC: assert(0);`, and the only
  netsurf-side reference is the diagnostic dumper (`content/handlers/css/dump.c:148`). **No layout
  code resolves a CALC unit.** What works is a narrow **text-level** evaluator in the preprocessor:
  `macsurf__rewrite_calc_aspect` (`cssh_css.c:2852`, invoked :5540, :5700) folds exactly four shapes
  — `calc(<num> / <num> * <num>%)`, `calc(<num> * <num>%)`, `calc(<num>% * <num>)`,
  `calc(<num> / <num>)` (`:2793-2796`) — targeting the aspect-ratio padding hack. The comment at
  `:2798` says anything more complex "falls through unchanged for libcss's existing calc parser" —
  i.e. into `CSS_UNIT_CALC`, where **nothing consumes it**. General `calc()` (mixed units, nesting,
  `var()` inside) is dropped.
- **Media queries: partial — four features.** Parsing/matching is live in `libcss/src/select/mq.h`,
  but only `width` (:125), `height` (:135), `prefers-color-scheme` (:146), and `orientation` (:163)
  match. `min-`/`max-` prefixes work — the parser strips them and converts to range ops
  (`parse/mq.c:221`), matching through `mq_match_feature_range_length_op1/op2` — so
  `(min-width: 600px)` responsive CSS functions. Missing: `resolution`, `aspect-ratio`,
  `hover`/`pointer`, `prefers-reduced-motion`, `color`/`color-gamut`, `device-width`/`device-height`.
  The `orientation` branch is a direct `memcmp` rather than an interned-string compare (`mq.h:157-163`)
  to dodge a CW8 header-rebuild trap.
- **prefers-color-scheme: real.** Interned into `str->prefers_color_scheme`
  (`select/strings.c:213-214`), matched at `mq.h:146-153` (with `CSS_MQ_FEATURE_OP_BOOL` treated as a
  match), fed from the user's own preference — `html.c:789` reads `nsoption_bool(prefer_dark_mode)`
  and interns the result into `c->media.prefers_color_scheme` at `html.c:866`. Note it is driven by a
  **MacSurf option, not an OS-level setting** (OS 9 has no such concept).

### Two corrections to the CSS ground-truth doc

1. **`transition` and `animation`/`@keyframes` are not "parsed-but-dropped".** The inventory's
   category (d) lists them as "intentionally-unsupported/niche" alongside `clip-path`/`mask`/`filter`,
   and CLAUDE.md:485 groups them under "parsed-but-silently-dropped gaps". Both framings are wrong for
   these two specifically: there is **no `CSS_PROP_TRANSITION*`/`CSS_PROP_ANIMATION*` enum slot at
   all**, and the preprocessor deletes the text at `cssh_css.c:3772-3785` before libcss tokenizes it.
   "Parsed-but-dropped" (category (a)) requires an accessor + parser + getter with no consumer; these
   have none of the three.
2. **`layout_grid.c`'s own file header (:36-47) is stale** — it advertises a V1 scope that fixes817-820
   (#62) and fixes833 (#279) have since exceeded. `grid-template-columns` fr/auto and `justify-items`
   now work. Treat CLAUDE.md:478-479 as the current grid gap list, not the file header.

---

## Browser features

| Item | Verdict |
|---|---|
| Private browsing | ABSENT |
| Session restore | ABSENT (cookie jar only) |
| Downloads manager | REAL — but no menu item |
| Find in page | REAL — no find-next, doesn't scroll to hit |
| Zoom | REAL plumbing — no UI at all |
| Bookmarks | REAL |
| History | REAL |
| Tab management | ABSENT (not "disabled" — unimplemented). Multi-window REAL |

This section came back **stronger than the checklist implies**. Two items are one small commit from
done, because the plumbing works and only discoverability is missing.

- **Private browsing: absent.** Grepping `private|incognito` across `frontends/macos9/` returns only
  unrelated hits (Unicode PUA font work, `html/private.h` includes, a `Cache-Control: private`
  string at `macos9_tls_fetcher.c:1638`). No menu item, no flag, no cookie/history suppression.
- **Session restore: absent** as a window/tab feature. The only thing persisted across quit is the
  cookie jar (`main.c:1785-1788`, `macos9_cookies_load()`). No window set saved or reopened; startup
  unconditionally opens one window at `MACSURF_HOME_URL`. No crash recovery.
- **Downloads manager: real.** A genuine Carbon window, not just a save path: `dl_mgr_ensure()`
  (`macos9_download.c:217`) creates a `kDocumentWindowClass` "Downloads" window; `dl_mgr_paint()`
  (`:237`) draws a row per download with live `bytes_written / total_length` and a per-row Cancel;
  `dl_cancel()` (`:305`) aborts via `download_context_abort`, closes the refnum, `FSpDelete`s the
  partial; `dl_mgr_progress()` (`:331`) repaints live; `dl_list_evict()` (`:344`) ages out completed
  rows; `macos9_download_mgr_click()` (`:392`) hit-tests. **Reachable only by starting a download**
  (`dl_mgr_show()`) — the File/View menus (`main.c:209-268`) have no Downloads entry.
- **Find in page: real, with a usability wart.** `macos9_find_in_page()`
  (`macos9_chrome_extras.c:193`) builds a real window with a TextEdit field, Find/Cancel, its own
  modal pump, remembers the last term, and at `:363` calls core
  `browser_window_search(bw, NULL, 1<<1, term)` — driving NetSurf's real textsearch and highlight.
  Wired to View > Find…/Cmd-F (`main.c:243`, dispatch `:456-459`). Missing find-next/previous and a
  match count; and the code **deliberately resets scroll to (0,0) instead of scrolling to the hit**
  (`macos9_chrome_extras.c:375-386`) because NetSurf's scroll-to math lands out of bounds — the user
  hunts for the highlight manually.
- **Zoom: real plumbing, zero UI.** `main.c:1177-1200` handles Cmd `-`/`_`, `=`/`+`, `0` → core
  `browser_window_set_scale(bw, ±0.1, false)` / `(1.0, true)`, then forces `needs_reformat` +
  invalidate. Genuine page zoom. No menu items, no toolbar control, no indicator, no persistence —
  **the keystrokes are undiscoverable**. (`main.c:930-983` `inZoomIn`/`inZoomOut` is the *window*
  maximize box, unrelated.)
- **Bookmarks: real.** 128-entry array with bookmarks *and* folders
  (`macos9_chrome_extras.c:416-520`), persisted to a "MacSurf Bookmarks" tab-delimited file
  (`macos9_disk_cache.c:707` save / `:754` load). Menu at `main.c:250-256`: Add Bookmark/D, Manage
  Bookmarks…/B, dynamic entries. `macos9_bookmark_window_show()` (`:2012`) is a full manager with a
  scrolling list and New Folder / Rename / Delete / Move / Go.
  **Stale comment:** `macos9_chrome_extras.c:13-14` claims "session-only array" — disk persistence
  landed in fixes645.
- **History: real, with a manager UI.** Up to 400 visits (`macos9_chrome_extras.c:876-945`), recorded
  from the title-set path (`macos9_history_record`, called `window.c:1496`), save/load via
  `macos9_history_load`/`_save`. Menu (`main.c:260-267`): Show All History, Clear History, Clear
  Cache, dynamic recents. `macos9_history_window_show()` (`:1435`) is a real 520x400 window with
  day-grouped rows, scroll, selection, double-click-to-open, Go, Clear. Also feeds address-bar inline
  autocomplete and the suggestion dropdown (`window.c:718-860`). Separate from NetSurf's per-window
  back/forward (`window.c:1084-1095`).
- **Tabs: absent — "disabled by default" is wrong.** No tab code at all: no tab fields in
  `struct gui_window` (`macos9.h`), no tab bar, no `BW_CREATE_TAB` anywhere in `frontends/macos9/`.
  `macos9_window_create(bw, ex, f)` (`window.c:1199`) **ignores both `ex` and `f`** and
  unconditionally calls `CreateNewWindow(6, 0x1F, ...)` — so a core `BW_CREATE_TAB` request would
  silently produce a separate top-level window. **Multi-window is real**: `window_list` linked list
  (`window.c:189-190, 1256, 1299`), File > New Window/Cmd-N (`main.c:366-378`), per-window
  event/reformat loops.

**CLAUDE.md correction needed:** the "Tabs disabled by default" line in Components/Do-Not overstates
the state — they are unimplemented, not switched off.

---

## Media

| Item | Verdict |
|---|---|
| `<video>` | ABSENT |
| `<audio>` | ABSENT |
| Codec strategy | Images only (QuickTime); A/V ABSENT |
| MSE | ABSENT (not even a stub) |

- **`<video>` / `<audio>`: absent.** No handling in `box_special.c` or `box_construct.c`, no rule in
  `resources/default.css`, no corestring. The elements fall through as unknown inline boxes (fallback
  content renders; the media does not). The only video-aware code is *avoidance*:
  `box_special.c:1131-1151` skips iframes hosted at youtube.com/youtu.be/vimeo.com because "MacSurf
  cannot render" them.
- **Codec strategy: images only.** `ffmpeg`, `SndPlay`, `SndNewChannel`, and `Skua` return **zero
  hits tree-wide**. QuickTime *is* used, but strictly as an image decoder: `main.c:1618-1630` calls
  `EnterMovies()` at startup and `macos9_image.c` is "NetSurf image content handler backed by
  QuickTime" Graphics Importers (JPEG quirks at `:447`, `:493`, `:1560`). **Movies.h is entered but
  no `Movie` is ever opened.** No Sound Manager use, no A/V decode path.
- **MSE: absent, and the comment lies.** `macsurf_qjs.c:4556` lists MediaSource among
  "capability-detection stubs", but the block that follows (`:4558-4610`) defines WebSocket,
  indexedDB, Notification, caches, Blob, File, FileReader, URL.createObjectURL — **no `g.MediaSource`
  is ever assigned**. The only related thing is `:4620`, installing bare `function(){}` constructors
  named `HTMLVideoElement`/`HTMLAudioElement`/`HTMLMediaElement`/`HTMLSourceElement` purely so
  `typeof` checks don't throw.

---

## Security

| Item | Verdict |
|---|---|
| TLS cert validation | REAL — **but silently downgrades to HTTP on failure** |
| Cert error UI / override | ABSENT |
| Mixed content blocking | ABSENT |
| HSTS | PARTIAL — dynamic real, no preload, undercut by the fallback |
| Revocation (CRL/OCSP/CT) | ABSENT |
| CA bundle updatability | Not updatable at runtime |
| Sandboxing | ABSENT — architecturally impossible |

### Invalid certs silently downgrade to cleartext HTTP

`hctx_fail()` (`macos9_tls_fetcher.c:1185-1191`) unconditionally retries over `http://` on any
**pre-response** TLS failure — and cert errors are exactly that class (they fail at `HS_TLSING`, so
`https_worked == 0`):

```c
if (c->aborted == 0 && !https_worked && !host_is_fb_asset(c->host) &&
    c->url != NULL && c->pool_key[0] != '\0' &&
    !terminal_url_check(nsurl_access(c->url)) &&
    !macsurf_scheme_was_http_tried(c->pool_key)) {
        n = sprintf(c->redirect_url, "http://%s", u + 8);
        ...
        (void)fetch_set_http_code(c->parent, 301);
        rm.type = FETCH_REDIRECT;
```

The fixes317 comment says "always fall back to HTTP when HTTPS fails, regardless of…". The **only**
exemption is a hardcoded Facebook-host list (`host_is_fb_asset`), added for session-cookie reasons,
not security. So an attacker presenting a bad cert — or simply blocking/breaking the handshake —
gets the page loaded over plaintext with **no user notice**. The comment at `:1226` even calls cert
failures "session-permanent" while routing them through this path. This also **defeats HSTS**: the
fallback issues its redirect through `fetch_send_callback` directly and never consults
`urldb_get_hsts_enabled()` (`macos9_http_fetcher.c:1308` acknowledges "bounce loop with HSTS hosts"
as a known interaction).

**This is a defect in shipped behaviour, not a roadmap gap.** It discards validation work that is
otherwise done properly.

### What validation actually does (and it's good)

Full BearSSL `x509_minimal` on both TLS paths: `ostls_async.c:720-730` calls `OSTLS_B3_GetAnchors()`
+ `br_ssl_client_init_full(&conn->sc, &conn->xc, anchors, anchors_count)` then
`br_x509_minimal_set_time()`. That gives chain signature verification, notBefore/notAfter,
basicConstraints/CA + key usage, and hostname/SAN matching. TLS 1.2 passes the hostname via
`br_ssl_client_reset(&conn->sc, conn->server_name, resume_session)` (`ostls_async.c:819`); TLS 1.3
drives the same validator manually — `xc->start_chain(hs->x509_ctx, hostname)` … `end_chain()`
(`ostls_tls13_handshake.c:1975-1998`), with an empty-chain check (`:1876-1878`) and the fixes739
reordering pass feeding certs leaf-first, falling back to wire order on parse trouble. Anchors: 121
Mozilla CCADB roots compiled in (`ostls_b3_anchors.c`, 5911 lines). Clock via `GetDateTime` → GMT
(`ostls_time.c:72-105`), and a pre-2000 clock **hard-fails** rather than skipping date checks
(`ostls_async.c:714-718`).

### Cert error UI — absent

The macos9 fetcher never emits `FETCH_CERT_ERR`/`FETCH_SSL_ERR` — those exist only in the unused curl
fetcher (`content/fetchers/curl.c:1613-1619`). `misc_stub.c:55-56` states it plainly: "MacSurf handles
TLS natively via macTLS, so cert-chain queries never fire." `window.c` has no cert dialog, no padlock,
no page-info UI. A cert failure becomes a generic `FETCH_ERROR` with `"https: handshake/transport
failed"` (`macos9_tls_fetcher.c:2429`) → `about:fetcherror`. The specific X.509 reason
(X509_EXPIRED / BAD_SERVER_NAME / NOT_TRUSTED — decoded by `ostls_br_err_name`,
`macos9_tls_fetcher.c:1020-1069`) goes only to the debug log, never to the user. **No override path
exists.**

### The rest

- **Mixed content: absent.** Grepping `mixed` across `content/`, `desktop/`, `frontends/macos9/`
  returns only unrelated hits. An https page's http subresources fetch normally with no block, no
  indicator, no console signal. `Upgrade-Insecure-Requests: 1` is sent
  (`macos9_tls_fetcher.c:2010-2017`, `macos9_http_fetcher.c:617-624`) — a **hint to the server**, not
  client-side enforcement; nothing consumes CSP `upgrade-insecure-requests` or
  `block-all-mixed-content`.
- **HSTS: partial, better than expected.** Dynamic HSTS is live via upstream core: parsed in
  `llcache_hsts_update_policy()` (`content/llcache.c:2568-2577`), stored with
  `max-age`/`includeSubDomains` in `urldb_set_hsts_policy()` (`content/urldb.c:3673-3742`), applied
  per request by `llcache_hsts_transform_url()` (`llcache.c:2503-2541`), queried via
  `urldb_get_hsts_enabled()` (`urldb.c:3744-3795`), persisted to disk (`urldb.c:549-566` write,
  `:3097-3134` read). Both files are in `MacSurf.mcp`, so this is compiled in.
  Missing: **(a) no preload list** — `grep preload` returns nothing, so first contact with any host is
  always TOFU-vulnerable; **(b)** the http fallback above can downgrade an HSTS-pinned host; **(c)**
  `llcache.c:2564` correctly skips policy updates when `object->fetch.tainted_tls` is set, but
  **nothing in the macos9 fetcher ever sets `tainted_tls`** — the guard is inert here.
- **Revocation: absent.** No CRL, no OCSP, no stapling, no CT. Grepping `OCSP|CRL|revoc` across
  `macTLS/os9`, `macTLS/docs`, and the frontend returns only chunked-transfer `CRLF` matches plus one
  explicit statement of the gap: `macTLS/docs/mactls-integration-notes.md:212` — "No ALPN, no SNI for
  IP literals, no OCSP, no CT." (ALPN has since landed at `ostls_async.c:724-728`; the OCSP/CT gap
  stands.) BearSSL's `x509_minimal` has no revocation upstream and none was added. **A
  revoked-but-unexpired certificate validates cleanly.**
- **CA bundle: not runtime-updatable.** A compiled-in static array — `OSTLS_B3_NUM_ANCHORS 121`
  (`ostls_b3_anchors.h:29`), returned from a static-lifetime array by `OSTLS_B3_GetAnchors()`
  (`ostls_b3_anchors.c:5900-5911`, lazily inited once). No PEM loader, no file/resource-fork cert
  reading, no preference hook. Updating trust requires regenerating `ostls_b3_anchors.c` via
  `macTLS/tools/regenerate_anchors.sh` and shipping a new binary. **Users cannot add a private or
  enterprise root, nor remove a distrusted one, without a new build.**
- **Sandboxing: absent, architecturally impossible.** Mac OS 9 has no memory protection, no process
  isolation, no preemptive multitasking. MacSurf is a single Carbon app in one statically-allocated
  partition; renderer, QuickJS, image decoders, fetchers, and the TLS stack share one address space
  with full access to each other's memory and the machine. The repo already states this as policy —
  `SECURITY.md:22-25` lists under **Out of scope**: "Mac OS 9 has no memory protection between
  processes", "no preemptive multitasking, any sufficiently long-running computation can hang the
  system", "The Carbon application partition is statically allocated; running out of memory leads to
  undefined behavior." The cooperative loop means a hang anywhere freezes the machine, not just the
  browser. Mitigations are resource budgets and defensive parsing only — **no containment boundary
  exists, and none can**. The correct posture is to keep saying so plainly, which SECURITY.md does.

---

## Fix-first list (defects, not gaps)

Ranked by reach-per-effort. These are wrong-behaviour bugs in shipped code:

1. **Click → JS bridge** (`macsurf_qjs.c:5939`) — implement `macsurf_qjs_dispatch_dom_click`. Unblocks
   every interactive page. Almost certainly what #300 actually needs.
2. **TLS cert-failure → HTTP downgrade** (`macos9_tls_fetcher.c:1185`) — gate the http fallback on
   failure *class*: transport/connect failures may fall back; cert-validation failures must not. Also
   consult `urldb_get_hsts_enabled()` there.
3. **`cloneNode` returns `el`** (`macsurf_qjs.c:2161`) — wrong answers, silently. Wire to libdom.
4. **Node traversal constants** (`macsurf_qjs.c:2161-2165`) — `firstChild`/`lastChild`/`childNodes`/
   `nextSibling`/`previousSibling`/`contains`. The C calls already exist and are used elsewhere in the
   same file. Highest reach-per-line in the audit.
5. **`document.readyState` never `'loading'`** (`macsurf_qjs.c:3833`) + inverted load ordering
   (`html.c:701` vs `:418`) — makes the standard init guard run before the box tree exists.
6. **`pushState` navigates** (`macsurf_qjs.c:4396`) — drop the `location.href` assignment; add
   `popstate`. Every SPA router currently reloads the page.
7. **Element-scoped `querySelector`** (`macsurf_qjs.c:1883`) — route to `qjs_sel_parse`, the real
   matcher that already exists at document level (fixes871).
8. **`setTimeout` arg passing** (`macsurf_qjs.c:869`) — one `JS_Call` change; also gives rAF its
   timestamp and un-NaNs every animation loop.
9. **`document.cookie` hardcoded `''`** (`macsurf_qjs.c:3834`) — the jar is real; expose it.
10. **Timer-arena eviction picks the soonest-expiring timer** (`macsurf_qjs.c:608-643`) — evict the
    furthest instead.

Cheap wins with no engineering risk: **Downloads menu item**, **Zoom menu items** (plumbing already
works), **CLAUDE.md tabs correction**, **stale bookmarks comment** (`macos9_chrome_extras.c:13`),
**stale `layout_grid.c` file header**, **the inaccurate MediaSource stub comment**
(`macsurf_qjs.c:4556`), and **the false "shadow-DOM event layer" comment** (`interaction.c:1712`),
which actively misleads anyone debugging the click path.

Known leak worth tracking: **element-scoped custom properties are never cleaned up on navigation**
(`custom_properties.c:266`, an in-code TODO).

## Roadmap (genuinely untouched)

Nothing here exists; each is a build, not a fix.

- **Storage**: persistent localStorage/sessionStorage (C-backed), IndexedDB, origin partitioning,
  SameSite, cookie policy UI
- **Network**: HTTP/2, WebSockets, CORS, CSP, same-origin enforcement
- **JS**: Web Workers, ES modules + dynamic import, MutationObserver, ResizeObserver, real
  IntersectionObserver geometry, capture/bubble/preventDefault
- **DOM**: insertAdjacentHTML, real outerHTML + innerHTML read, real getComputedStyle, real
  getBoundingClientRect, FormData over live controls, form.submit(), constraint validation
- **CSS/render**: WOFF/WOFF2 (needs zlib **and** brotli), Canvas 2D context, real `transition`,
  real `@keyframes`, general `calc()` resolution, the intrinsic-sizing solver
  (`min-content`/`max-content`/`fit-content`), true `row-gap`, grid Round 2 (`justify-self`,
  minmax, §12.8 stretch, named lines, dense/column flow, subgrid), sticky containing-block +
  nested-scroll clamps, transform-origin/matrix/3D, the remaining media features
- **Browser**: private browsing, session restore, tabs
- **Media**: `<video>`, `<audio>`, any A/V codec path, MSE
- **Security**: cert error UI + override, mixed content blocking, HSTS preload, revocation,
  updatable CA bundle

Three of these are **prerequisites that unblock clusters**, and are worth sequencing first:

- **The intrinsic-sizing solver** (`min-content`/`max-content`/`fit-content`) unblocks
  `table-layout: auto`, flex shrink-to-fit, and grid auto sizing. Named as the prerequisite at
  CLAUDE.md:476 and confirmed by this audit from the flex side.
- **`bits[16]` is full** — the next bit-packed CSS property needs the array extended to `bits[17]`,
  a structural change to the arena-interning-sensitive `css_computed_style_i`. This blocks
  `justify-self` and everything after it. It is the gate on the whole grid Round 2 line.
- **zlib inflate** is needed before WOFF; **brotli** before WOFF2. Neither exists in the tree. Since
  `@font-face` currently renders only bare `.ttf`/`.otf`, this is the difference between webfonts
  working on approximately no sites and working on most.

**Sandboxing is not on the roadmap** — it is out of scope permanently, for stated architectural
reasons. That is the honest answer, and SECURITY.md already gives it.

---

## Cross-cutting observation

The recurring shape is **real C plumbing, fake JS exposure**. libdom traversal, the libcss cascade,
the box tree, and libdom's own capture/bubble event dispatch are all present and working; the
bindings hardcode constants over the top of them. Several of the highest-reach items in the fix-first
list are a handful of lines each, calling C functions the same file already uses. Canvas is the same
shape one layer over: the redraw branch is written and waiting on a `getContext` that was never
bound.

A second shape, specific to CSS: **the preprocessor is load-bearing and invisible.** `transform`,
grid, and the useful part of `calc()` are text-rewritten into `-macsurf-*` vendor props before libcss
sees them; `transition`/`animation` are deleted outright. So "does libcss support X" is the wrong
question — the answer depends on whether `cssh_css.c`'s string matching recognized it first. Anything
it doesn't match is gone silently, which is why several gaps here present as layout bugs rather than
as missing features.

Three audit lessons worth keeping:
- **A capability-detection stub reads as "implemented" to grep and to feature detection alike.** Every
  verdict above required reading the body.
- **The S0 harness dispatches events synthetically**, which is precisely the path that works — so it
  looks healthy while hardware ignores every click. Harness green ≠ hardware green when the harness
  bypasses the broken bridge. See `[[reference_harness_build_trap]]`.
- **Comments lie in a specific direction: they describe intent that was never finished.**
  `interaction.c:1712` says it dispatches clicks through the JS event layer (it calls a `return 0;`
  stub); `macsurf_qjs.c:4556` lists MediaSource among stubs that were never written; `layout_grid.c`'s
  header understates shipped work; `macos9_chrome_extras.c:13` calls persistent bookmarks
  session-only. When a comment and the code disagree here, the code has always been right.
