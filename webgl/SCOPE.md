# MacSurf WebGL — Project Scope

**Status:** initial scope, pre-Stage 0.
**Date:** 2026-05-26.
**Author:** Claude (research).
**Target shipping shape:** mirrors the macTLS arc (vendored upstream, audited once, Carbon CFM test harness, Stage A → E hardware gates, library-mode integration into MacSurf).

---

## 0. Executive summary

WebGL 1.0 is OpenGL ES 2.0 bound to JavaScript. Two hard facts collide on Mac OS 9:

1. OS 9 OpenGL maxes at 1.2.2 (9.2.2) / 1.2.1 (9.1) — **fixed-function only, no shaders, no ARB_vertex_program, no GLSL of any era**. Apple shipped the OS-9 OpenGL stack four years before GLSL existed.
2. WebGL is **shader-mandatory**. Every WebGL draw call references a linked program built from a GLSL ES 1.00 vertex shader and a GLSL ES 1.00 fragment shader. There is no fixed-function fallback path in the API.

The implication is unavoidable: **shader execution has to happen on the PPC CPU inside MacSurf**, because no GPU on OS 9 has ever executed a programmable shader. Hardware AGL can be wired in as an optional fast path for the narrow subset of shader programs that match recognisable fixed-function patterns (textured Gouraud quad, basic lit triangle), but the primary architecture is a software rasterizer driven by an in-browser GLSL ES compiler+interpreter.

The user directive is **"work on any level."** That collapses the scope question: we are not trying to run Aquarium at 60fps. We are trying to make `canvas.getContext('webgl')` return a real context, compile a real shader, draw a real triangle, and have the pixels land on the page. Anything beyond that is gravy. The Khronos "Hello Triangle" demo is the proof-of-life target.

This document scopes that work end to end: what to vendor, what to write from scratch, what the file budget looks like, what the memory budget looks like, what each shipping stage proves, and what to defer. It follows the discipline that macTLS established: **each stage isolates one new failure boundary**, the gate is hardware-verified on a real Mac, failed stages get archived with a written post-mortem.

---

## 1. The problem reduction

### What WebGL actually does

1. Page creates a `<canvas>` element with width / height attributes.
2. Page calls `canvas.getContext('webgl')`. Returns a `WebGLRenderingContext` object exposing ~140 methods.
3. Page creates a vertex shader and a fragment shader (`createShader`, `shaderSource`, `compileShader`).
4. Page creates a program, attaches both shaders, links it, makes it current (`createProgram`, `attachShader` ×2, `linkProgram`, `useProgram`).
5. Page uploads vertex data and (optionally) textures (`createBuffer` + `bufferData`; `createTexture` + `texImage2D`).
6. Page describes attribute layout (`vertexAttribPointer`, `enableVertexAttribArray`) and sets uniforms (`uniformMatrix4fv` etc.).
7. Page calls `drawArrays(TRIANGLES, ...)` or `drawElements`. Pixels appear in the canvas's drawing buffer.
8. Browser composites the canvas onto the page on the next paint.

The minimum "Hello Triangle" exercises 14 GL calls plus the `Float32Array` typed-array constructor.

### Where each step lands on OS 9

| Step | OS 9 mapping |
|---|---|
| 1 | NetSurf DOM gets `<canvas>` element type. libdom already supports arbitrary element creation; no library change required. Layout treats it as a replaced element sized by `width`/`height` attributes. |
| 2 | Duktape native function exposed on the canvas wrapper. Returns a JS object whose methods are Duktape natives bridging into the WebGL state machine. |
| 3 | Source string captured; `compileShader` invokes our GLSL ES 1.00 compiler → AST → typed IR (or bytecode). |
| 4 | `linkProgram` resolves uniform/attribute names to slots; bakes vertex+fragment IRs into a `glProgram` record. |
| 5 | `bufferData` `memcpy`s into a heap-allocated buffer; `texImage2D` allocates a 32-bit RGBA texture (origin top-left, the WebGL convention). |
| 6 | Slot writes into the GL state-machine arrays. |
| 7 | Triangle setup loops over indices, runs vertex shader once per vertex (interpreted/transpiled), assembles triangles, clips, rasterizes scanlines, runs fragment shader once per pixel (interpreted/transpiled), blends to drawing buffer. |
| 8 | Drawing buffer (an offscreen GWorld at canvas size) blitted to the page during normal NetSurf redraw via the existing `ctx->plot->bitmap` dispatch path that `macos9_image.c` already uses for `<img>`. |

Steps 3, 4, 7 are the project. Everything else is mechanical.

### The shader execution problem

GLSL ES 1.00 is a small typed C-like language with the following surface:

- 14 named types (`float`, `int`, `bool`, `vec2/3/4`, `ivec2/3/4`, `bvec2/3/4`, `mat2/3/4`, `sampler2D`, `samplerCube`), arrays (constant-size only), and user structs.
- C-like operators (no bitwise, no integer `%`), `if`/`for`/`while`/`do-while`, `discard` in fragment shaders.
- ~80 built-in functions (`sin`/`cos`/`pow`/`mix`/`normalize`/`dot`/`cross`/`texture2D`/...).
- Hard restrictions: `for`-loop bounds must be constant-foldable; no recursion; no function pointers; no `#include`.
- Mandatory outputs: `gl_Position` (vertex), `gl_FragColor` (fragment).

Three execution strategies, in order of porting cost:

1. **AST walker.** Parse to AST, walk the tree per vertex and per fragment. Correctness floor — handles any well-formed shader. Slowest (probably 10–100× slower than option 2). Smallest implementation (~3 KLOC parser + ~1.5 KLOC walker). **This is the floor we ship.**

2. **Bytecode interpreter.** Lower AST to a small register-machine bytecode (think ARB_fragment_program era: `MUL`, `MAD`, `DP3`, `DP4`, `TEX`, `KIL`, `MOV`, `RSQ`, `LOG`, `EXP`, `LRP`, `CMP`, conditional jump for control flow). Mesa's `prog_execute.c` is the historical reference, ~2 KLOC for the executor. **Tier 2 acceleration, lands later.**

3. **GLSL → C transpiler.** Translate each shader to a C function on the fly, compile, link, call. No on-Mac C compiler available. Workable only if precompiled on a build host and shipped alongside the page (PortableGL's design assumes user-written C shaders — not GLSL-source-driven). **Out of scope; preserves PortableGL's model but adds a build-time dependency we don't want.**

The bytecode interpreter is the long-term winner. AST walking is the right starting point because it forces the parser and type system to be correct first; the bytecode lowering is a strict mechanical descent from there.

### Performance frame

- G3 233 MHz with software rasterization at 320×240: hello-triangle should run at single-digit ms per frame (it's three vertex-shader calls and ~25k pixels at default 320×240 viewport — single-digit fps under AST walker, low double-digit fps under bytecode).
- A rotating textured cube at 320×240 (six faces × two triangles × ~13k pixels each, mostly culled): probably 0.5–2 fps under AST walker, 2–5 fps under bytecode.
- A three.js scene of any complexity: not a target. Three.js auto-generates 500–2000-line shaders with `#define`-heavy lighting models; these will compile but draw at fractions-of-fps and likely overflow the partition. Three.js is a v0.3+ aspiration, not the proof-of-life.

The proof-of-life targets are: hello-triangle, rotating-coloured-cube, textured-quad, single Shadertoy fragment-only demo. Anything that renders at all on real G3 hardware closes the project.

---

## 2. The shape of the solution

### Architecture diagram

```
PAGE (HTML+JS+GLSL)
   │
   ▼
NetSurf core               (HTML parse → DOM)
   │
   ▼
libdom                     (<canvas> as generic dom_element)
   │
   ▼
macsurf_js_dom.c           (existing) — Duktape ↔ DOM bridge
   │
   ▼
macsurf_js_canvas.c        (NEW) — canvas.width/height/getContext binding
   │                                returns {2D context} or {WebGL context}
   ▼
macsurf_js_webgl.c         (NEW) — ~140 Duktape natives, one per GL call
   │                                marshals args, dispatches to state machine
   ▼
mwebgl_state.c             (NEW) — pure-C GL state machine: buffers, textures,
   │                                programs, attribute arrays, uniforms,
   │                                framebuffers, error stack, capability flags
   ▼
mwebgl_glsl.c              (NEW) — GLSL ES 1.00 lexer + parser → typed AST
mwebgl_glsl_check.c        (NEW) — semantic check / type inference / linking
mwebgl_exec.c              (NEW) — AST walker; later, bytecode interp
   │
   ▼
mwebgl_raster.c            (NEW) — software rasterizer: triangle setup,
   │                                edge functions, perspective-correct varying
   │                                interpolation, depth test, blend
   ▼
macos9_bitmap.c            (existing) — backing GWorld for drawing buffer
   │
   ▼
ctx->plot->bitmap          (existing dispatch via macos9_plot_bitmap → CopyBits)
   │
   ▼
WindowRef                  (NetSurf paint)
```

The shape mirrors `macos9_image.c`: decode-once into a `struct macos9_bitmap`, then ride the existing plotter pipeline to the screen. The canvas's drawing buffer **is** a NetSurf bitmap, so all of the existing offscreen-GWorld + CopyBits + clip-correctness work (fixes77g and friends) applies for free.

### File budget (initial)

| File | Approx LOC | Purpose |
|---|---:|---|
| `macsurf_js_canvas.c` | 400 | Duktape wrapper for `HTMLCanvasElement` instance methods (`width`, `height`, `getContext`, `toDataURL`). |
| `macsurf_js_canvas2d.c` | 1200 | CanvasRenderingContext2D bindings — Stage A, optional but high-leverage. Maps to QuickDraw + our own pixel ops. |
| `macsurf_js_webgl.c` | 2500 | ~140 Duktape natives; mostly argument marshalling and dispatch into `mwebgl_state`. Tedious but mechanical. |
| `mwebgl_state.h` / `.c` | 1500 | GL state machine: contexts, buffers, textures, programs, error stack, parameter queries. |
| `mwebgl_glsl.h` / `.c` | 2500 | GLSL ES 1.00 lexer + recursive-descent parser → AST. |
| `mwebgl_glsl_check.c` | 1200 | Type inference, semantic checks (for-loop constant bounds, no recursion, sampler usage), linker (resolve uniform/attribute names to slots, match vertex→fragment varyings). |
| `mwebgl_exec.c` | 1800 | AST walker for vertex+fragment execution. Built-in function table. Texture sampling. |
| `mwebgl_raster.c` | 1500 | Triangle setup, edge equations, perspective-correct varying interpolation, depth test, alpha blend, stipple-degraded MSAA (optional). |
| `mwebgl_math.h` | 400 | vec2/3/4, mat2/3/4 with the standard ops. Aligned with PortableGL's `crsw_math` for ease of reference. |
| `mwebgl_log.c` | 200 | Mirror of `ostls_log.c` — file-backed channel writing to `MacSurf WebGL.log` on Desktop. |
| `mwebgl_prefix.h` | 100 | CW8 workarounds, build-config defines. |
| **Stage A subtotal (canvas + 2D, no GL)** | **~1700** | |
| **Stage E subtotal (full software WebGL)** | **~13000** | |

For comparison: macTLS's `os9/` directory is ~12 KLOC (excluding vendored BearSSL at 53 KLOC). The same shape.

### Memory budget per WebGL context

| Component | Bytes | Notes |
|---|---:|---|
| Drawing buffer (RGBA) at 300×150 default canvas | 180 KB | Spec default size. |
| Drawing buffer at 800×600 (typical demo) | 1.9 MB | One per canvas. |
| Depth buffer (16-bit) at 800×600 | 940 KB | Per spec default `depth: true`. |
| Stencil buffer (8-bit) at 800×600 | 470 KB | Only if `stencil: true` (default false). |
| Vertex buffer storage | variable, typical 10 KB–500 KB | Bound by the page. |
| Texture storage | variable, typical 4 MB cap | Like image cache, evictable on memory pressure. |
| Compiled programs (AST + symbol tables) | ~5 KB per program | A few hundred per page in worst-case three.js. |
| GL state machine resident | ~80 KB | Constant per context. |
| **Per-context typical** | **~6–8 MB** | Two-canvas page exceeds the 16 MB partition; canvas-cap policy required. |

Implication: **partition raise to at least 32 MB preferred / 16 MB minimum** for WebGL builds. The image cache LRU pattern from `macos9_image.c` (lines 107–148) transplants directly to a per-context drawing-buffer cap — most pages have one canvas, but the LRU keeps a misbehaving page from starving the heap.

---

## 3. Backend selection

Software primary, hardware optional. The decision tree at `getContext('webgl')` time:

```
1. parse contextAttributes
2. if !webgl_enabled (pref) -> return null
3. if canvas area > MWEBGL_MAX_PIXELS -> return null (defends 16 MB partition)
4. allocate context, init state machine
5. if hardware path enabled AND OpenGLLibrary present (Gestalt 'opgl')
       AND AGL_OFFSCREEN succeeds on a w×h pixmap
       AND first compileShader succeeds at fingerprint match
       -> route draws through AGL fixed-function (TIER 0: hardware)
6. else
   -> route draws through software rasterizer (TIER 1: AST walker, Stage E)
   -> later: TIER 1B bytecode interpreter when lowering is done
   -> later: TIER 2 PortableGL-derived rasterizer fast path
7. return context object
```

**Tier 0 — hardware AGL (Stage F, optional, later).** Only the shader subset that matches one of a small set of fingerprints (passthrough position+color, single-texture sampler, basic Lambert/Phong) gets routed to fixed-function GL calls. Fingerprint match is strict (lexical comparison of the canonicalised AST against templates). Tier 0 covers maybe 10–20% of content but at hardware speed on machines that have it (Rage 128 / Radeon / GeForce). **Note from research:** Apple's AGL_OFFSCREEN attribute is **software-rasterized on every OS 9 GPU** — so the only way to get hardware acceleration is to draw to a real on-screen `WindowRef` and `glReadPixels` the framebuffer back. Awkward (the read introduces stalls; we'd need a hidden helper window), and on the user's G3 iMac with Rage Pro the result is software-rasterized anyway. **Conclusion: Tier 0 is low-priority Phase F polish, not a near-term shipping target.**

**Tier 1 — AST walker (Stage E).** Correctness floor. Slow but always works. Walks the GLSL ES AST per vertex / per fragment from C. Built-in functions are a switch table over function names. Texture sampling honours min/mag filter + wrap mode. **This is what ships first.**

**Tier 1B — bytecode interpreter (Stage E+1).** Mesa `prog_execute.c`-style register-machine over an op set of maybe 40 instructions (`ADD`, `MUL`, `MAD`, `DP3`, `DP4`, `TEX`, `KIL`, `MOV`, `RSQ`, `RCP`, `LOG`, `EXP2`, `LRP`, `CMP`, `SLT`, `SGE`, `MIN`, `MAX`, `ABS`, `FRC`, `FLR`, `SIN`, `COS`, branch, label). Per-fragment cost drops 5–20× over AST walking. Compiled from the AST during `linkProgram`.

**Tier 2 — PortableGL-derived rasterizer fast path (Stage E+2).** PortableGL's `gl_impl.c` rasterizer is mature, conformance-tested, and assumes shaders-as-C-function-pointers. We can adopt its rasterizer (perspective-correct varying interpolation, depth test, alpha blend) while supplying our own "C function pointer" shaper that closes over a compiled GLSL program. This decouples the rasterizer correctness story from the shader execution story. PortableGL is C99 (`stdint`, designated initializers, mid-block declarations, single-header concat of 13166 lines — ~20 KLOC across `src/`), so it needs the same C89-cleanup pass that Duktape and lodepng went through. **Not a hard requirement** — rolling our own rasterizer in 1500 LOC of C89 is also workable, and the wider PortableGL surface (GL 3.x-ish, 13k LOC) probably costs more to port than it saves.

**Recommendation:** roll our own rasterizer in C89 (~1500 LOC, based on the textbook scanline + edge-function approach with perspective-correct varyings). Reference PortableGL's `gl_impl.c` and `crsw_math.c` for algorithmic correctness without depending on the code. Bellard's TinyGL is **not** useful — it's fixed-function only, no shader execution model, irrelevant to our problem.

---

## 4. GLSL ES 1.00 strategy in detail

### Lexer

Hand-rolled. Tokens: identifier, integer literal, float literal, keyword (~50 reserved words), operator (~30), punctuation, preprocessor directive, EOF. Comment handling (`//` and `/* */`). Roughly 600 LOC. Skip `#include` (not in spec). Skip `#extension` (no extensions supported in v0.1). Honour `#version 100` and `#ifdef GL_ES`.

### Parser

Recursive-descent over the ESSL grammar (spec Appendix 1, ~10 pages of BNF). Reference: the `glsl_parser.yy` file in glsl-optimizer (cleanest open-source ESSL grammar in production use). Output: typed AST nodes.

AST node types:
- declarations: variable, function, struct, uniform, attribute, varying
- statements: block, if, for, while, do-while, return, break, continue, discard, expression
- expressions: literal, identifier, binary, unary, ternary, function-call, swizzle, array-index, struct-access, assignment, comma

About 30 AST kinds. Roughly 2000 LOC of parser + AST construction.

### Semantic check / linker

- Type inference (every node gets a type).
- For-loop constant-bound check (Appendix A §4).
- Recursion check (build call graph, detect cycles).
- Attribute / uniform / varying location assignment.
- Vertex→fragment varying matching (name + type must agree across shaders in a linked program).
- Sampler usage restrictions (only as uniforms, function parameters, or texture function args; no arithmetic).
- Built-in function resolution (~80 functions, ~200 overloads counting vec/mat polymorphism).

Roughly 1200 LOC.

### AST walker (execution)

For each vertex / fragment:
1. Push attribute / `gl_FragCoord` / uniform values into the symbol table.
2. Walk the `main()` function body. Standard tree-walking interpreter: evaluate expressions, execute statements, follow control flow.
3. On `texture2D(sampler, uv)`: look up the sampler's bound texture, apply wrap mode, apply min/mag filter, return RGBA `vec4`.
4. On `discard` (fragment only): set a flag, return immediately.
5. After return: read out `gl_Position` (vertex) or `gl_FragColor` (fragment).

Optimisation note: **the inner loop is per-fragment, not per-frame.** At 320×240 = 76800 fragments per fullscreen draw, every cycle in the fragment walker matters. We won't beat a bytecode interpreter here, but we should at least avoid `malloc` in the per-fragment path (use scratch arenas reset per scanline).

Roughly 1800 LOC.

### Built-in function table

Hand-implemented in C, one function per built-in. `sin`, `cos`, `sqrt`, etc. dispatch directly to `<math.h>` (PPC has hardware FPU). Vector and matrix forms are component-wise. Geometric functions (`length`, `dot`, `normalize`, `cross`, `reflect`, `refract`, `faceforward`) are textbook. `texture2D` and `textureCube` are the only ones that touch GL state.

About 80 functions, ~400 LOC of implementations, ~200 LOC of dispatch table. Folded into `mwebgl_exec.c`.

### Bytecode lowering (Stage E+1, deferred)

When `linkProgram` is called, walk the typed AST and emit a register-machine bytecode. Each register is `vec4` (so a `float` lives in `.x`, a `vec3` in `.xyz`). Output: a flat `uint32_t` instruction stream plus a constant pool. Execution: a switch in a tight loop. The full Mesa ARB_fragment_program executor template applies.

Skip this in v0.1. Land it when AST-walker performance becomes the limiting factor (probably immediately after Stage E proof-of-life).

---

## 5. Phased plan (Stage A — Stage F)

Following the macTLS pattern. Each stage has a hardware acceptance gate. Failed stages get archived under `webgl/archive/` with a written post-mortem.

### Stage 0 — vendor audit & decision lock

**Goal.** Decide what to vendor vs roll our own. Audit PortableGL for C89 portability (the way macTLS audited BearSSL). Decide whether to take its rasterizer wholesale, fork it heavily, or write our own.

**Deliverables.**
- `webgl/AUDIT.md` — list of PortableGL's C99-isms, file inclusion/exclusion lists, rationale.
- `webgl/SCOPE.md` (this document).
- `webgl/tools/audit_c89.py` — mechanical regex sweep (mirror of macTLS's `audit_cw8_compat.py`).
- Decision lock: vendor PortableGL? Roll our own rasterizer? Hybrid (vendor math + roll rasterizer)?

**Gate.** Document published. No code yet.

**Likely outcome (based on this scoping pass):** roll our own rasterizer in C89 (~1500 LOC); reference PortableGL's `crsw_math.c` and `gl_impl.c` for algorithmic correctness; do not vendor PortableGL into the build.

### Stage A — `<canvas>` element + 2D context

**Goal.** Prove that `<canvas>` is a renderable DOM element in MacSurf with at least a partial 2D context, before WebGL machinery exists at all.

**Deliverables.**
- `macsurf_js_canvas.c` — `HTMLCanvasElement` wrapper, `width`/`height` properties, `getContext('2d')`.
- `macsurf_js_canvas2d.c` — `fillRect`, `fillStyle`, `strokeRect`, `clearRect`, `drawImage`, `fillText`, `getImageData`/`putImageData`. (Skip `arc`, `bezierCurveTo`, gradients, complex paths.)
- libdom registers `canvas` as a known element tag (probably already does — verify).
- Layout: canvas treated as replaced element sized by `width`/`height` attributes (defaults 300×150 per spec).
- Canvas backing: `struct macos9_bitmap` allocated at canvas creation; updated by 2D context calls; rendered via existing `ctx->plot->bitmap` dispatch.

**Acceptance.** Hand-written test page draws a red rectangle on a yellow background and renders correctly on the G3 iMac.

**Test harness.** A standalone Carbon CFM app — `MWebGLTest/` — that does NOT integrate into MacSurf yet. Mirror of `MacTLSTest`. The app exercises the canvas binding + 2D context against a fixed in-memory page.

**Why 2D first.** Two reasons. (1) Many sites probe canvas existence before requesting WebGL; getting 2D working buys real compatibility immediately. (2) The plumbing path — Duktape wrapper → backing bitmap → page redraw — is identical to WebGL's. Proving it for 2D de-risks the WebGL Stage E plumbing.

**Approximate file count:** 2 new .c files, ~1700 LOC.

### Stage B — WebGL stub context

**Goal.** `canvas.getContext('webgl')` returns a non-null object that exposes every method as a stub. The methods do nothing useful, but they don't throw, and `getError()` returns `NO_ERROR`. Goal: prove plumbing, identify missing typed-array support, surface the Duktape↔native dispatch cost.

**Deliverables.**
- `macsurf_js_webgl.c` — all ~140 entry points as Duktape natives. Bodies are TODO stubs (`return 0;` or push correct return type). Constants table (~300 enums) exposed on the context object.
- `mwebgl_state.h` / `.c` — context allocation, error stack, parameter queries. Real `getParameter`, real `getError`. Most state setters are stubs.
- `Float32Array` / `Uint8Array` / `Uint16Array` / `Int32Array` in Duktape. Confirm Duktape 2.7.0 has typed arrays compiled in (it does, in the default config).

**Acceptance.** Hello-triangle page runs without throwing JS errors. Canvas remains the cleared clear-color (initially black). `getError()` returns 0 throughout.

**Approximate file count:** 3 new .c files, ~4000 LOC.

### Stage C — GLSL ES 1.00 compiler

**Goal.** Real `compileShader` and `linkProgram`. After Stage C, hello-triangle goes through the compile path successfully and `getShaderParameter(s, COMPILE_STATUS)` returns true, but draws still produce nothing (rasterizer not yet wired).

**Deliverables.**
- `mwebgl_glsl.c` — lexer + parser.
- `mwebgl_glsl_check.c` — semantic check + linker.
- Diagnostic accumulator into `getShaderInfoLog` and `getProgramInfoLog`.

**Acceptance.** Compile the hello-triangle shaders, the rotating-cube shaders, and three standalone Shadertoy fragment shaders. All four produce no diagnostics. Reject one known-malformed shader with an informative message. The Khronos GLSL ES 1.00 spec's BNF parses end-to-end on the conformance shader set.

**Approximate file count:** 2 new .c files, ~3700 LOC.

### Stage D — software rasterizer (no shaders yet)

**Goal.** Rasterize triangles with hard-coded passthrough vertex and constant-color fragment shaders. Tests rasterizer correctness in isolation.

**Deliverables.**
- `mwebgl_raster.c` — triangle setup, edge equations, perspective-correct attribute interpolation, depth test, alpha blend, viewport transform.
- `mwebgl_math.h` — vec/mat ops.

**Acceptance.** Hand-driven test renders a colored triangle, a textured quad, and an interpenetrating-triangles depth-test scene to a `struct macos9_bitmap`, blitted to a window. No GLSL involvement — the "shaders" are C functions hard-wired into the rasterizer's setup.

**Approximate file count:** 2 new .c files, ~1900 LOC.

### Stage E — full WebGL pipeline (proof of life)

**Goal.** Connect Stage C's compiled programs into Stage D's rasterizer via the AST walker.

**Deliverables.**
- `mwebgl_exec.c` — AST walker, built-in function table.
- Final state-machine wiring in `mwebgl_state.c` — buffers, textures, programs, attributes, uniforms all live.

**Acceptance — the project milestone gate.**
1. Khronos WebGL Hello Triangle (the literal canonical demo) renders correctly on a G3 iMac running OS 9.1.
2. Rotating textured cube renders.
3. One Shadertoy fragment-only demo renders (even if at 0.5 fps).
4. `mactrove.com/webgl-test.html` (a page we author) loads, runs the demo, doesn't crash.

**This is "v0.1 OSWebGL" — the first shippable version. Library mode, single function call surface, mirrors `OSTLS_Fetch`.**

**Approximate file count:** 1 new .c file, ~2000 LOC. Cumulative: ~13 KLOC.

### Stage E+1 — bytecode interpreter

**Goal.** Performance.

**Deliverables.** Bytecode lowering pass in `mwebgl_glsl_check.c`; bytecode executor in `mwebgl_exec.c` (alongside the AST walker, switched by build config).

**Acceptance.** Rotating-cube benchmark frame time drops 5–10×.

### Stage F — hardware AGL fast path (optional)

**Goal.** Detect shader programs that match recognisable fixed-function patterns; route those to AGL on machines that have OpenGLLibrary.

**Deliverables.**
- `mwebgl_agl.c` — weak-linked AGL bindings; fingerprint matcher; fixed-function translator for the matched subset.
- Gestalt check at context creation; graceful degradation when missing.

**Acceptance.** On a Power Mac G4 with Radeon, a known-pattern shader (vertex-color triangle) renders at full hardware speed; on a G3 iMac it falls back to software.

**Note on hardware reality.** AGL_OFFSCREEN is software on every OS 9 GPU per Apple's own docs. The only path to hardware acceleration is drawing to an on-screen WindowRef and reading back pixels with `glReadPixels`. This introduces a hidden helper window and a per-frame readback stall. For the user's G3 iMac dev hardware (Rage Pro, no hardware OpenGL anyway), Tier 0 returns immediately to software. This stage is low-priority polish — implement only after Stage E+1 shows the software path is the actual bottleneck for content the user cares about.

### Stage G — MacSurf integration

**Goal.** Land `macos9_webgl_integration.c` in MacSurf that wires the OSWebGL library into the browser. Held until Stages E + E+1 are both hardware-verified.

**Deliverables.**
- Canvas backing-bitmap allocated at element-creation time.
- Drawing-buffer presentation: tied to NetSurf's content-redraw cycle so the canvas pixels are picked up by the page paint.
- `<canvas>` registered as a recognised tag with appropriate replaced-element layout behaviour.
- Memory-pressure handling: per-canvas drawing-buffer LRU mirroring `macos9_image.c`'s pattern.

**Acceptance.** A page on `mactrove.com` containing a WebGL demo renders the demo correctly inside MacSurf, with surrounding HTML and CSS unaffected. Khronos hello-triangle accessible at `webgl.mp.ls/hello.html` or similar.

### Stage H — v0.2 expanded coverage (post-shipping)

Items to land after v0.1 is in the wild:
- Framebuffer objects (render-to-texture, shadow mapping, postprocessing).
- `OES_standard_derivatives` (`dFdx`/`dFdy`/`fwidth` — required for SDF text).
- `OES_element_index_uint` (>65k vertices per draw).
- `LINES` / `POINTS` / `TRIANGLE_FAN` / `TRIANGLE_STRIP` primitives beyond `TRIANGLES`.
- Cube maps.
- Mipmap generation.
- `getActiveUniform` / `getActiveAttrib` for debug tooling.
- `ANGLE_instanced_arrays` (particle systems).
- v0.3: three.js compatibility pass — patch up specific failure modes encountered on real three.js pages.

---

## 6. Existing-code integration points

### Files that must change (Stage A)

| File | Change |
|---|---|
| `browser/netsurf/frontends/macos9/javascript/macsurf_js_dom.c` | At element wrapper creation (`macsurf_push_element`, line 129–173 of current source), check `tagName == "CANVAS"` and call into `macsurf_js_canvas_register(duk, el)` to add canvas-specific properties and methods. |
| `browser/netsurf/frontends/macos9/MacSurf.mcp` | Add new .c files. (User maintains this; we list new files in the ship message per CLAUDE.md convention.) |
| `browser/netsurf/frontends/macos9/macos9_bitmap.c` | No change required initially — canvas drawing buffer uses the existing `macos9_bitmap_create` + `macos9_bitmap_get_buffer` path. |
| `browser/netsurf/frontends/macos9/macsurf_prefix.h` | Add `#define MWEBGL_ENABLED 1`, plus build-config flags for the WebGL subsystem (debug log on/off, max canvas pixels, tier override). |
| `browser/netsurf/frontends/macos9/main.c` | At `main()` after Duktape setup: call `mwebgl_init()` (registers log channel, prepares state). Symmetric to `macsurf_debug_log_init`'s init pattern. |
| Carbon partition in `MacSurf.mcp` | Bump preferred to at least 32 MB (currently 194 MB per CLAUDE.md, well within range). |

### Files that must NOT change

- No changes to libdom, libcss, libhubbub, libparserutils, libwapcaplet. WebGL is a leaf subsystem.
- No changes to NetSurf core. Canvas is a DOM element; libdom already supports arbitrary tags.
- No changes to the CSS pipeline. Canvas styling is handled by existing CSS (the canvas content is just a bitmap that the existing replaced-element layout path positions).

### Plotter pipeline reuse

The canvas drawing buffer is a `struct macos9_bitmap`. To present it during page paint:

1. NetSurf's box-tree walker hits the canvas element's layout box.
2. Existing replaced-element handler dispatches `ctx->plot->bitmap(ctx, bitmap, x, y, w, h, bg, flags)`.
3. `macos9_plot_bitmap` in `plotters.c` does the existing `CopyBits` (or `CopyMask` if there's an alpha channel — which there is, by default `WebGLContextAttributes.alpha = true`).
4. The fixes77g `macos9_find_gw_for_plot()` lookup correctly routes to the current paint GWorld; we don't need to revisit any port-management logic.

In other words: **once the canvas exists as a renderable replaced element with a `struct macos9_bitmap` backing it, the entire blit-to-screen path is already solved.** Stages A through E touch only the JavaScript binding, GL state, and rasterizer layers — not the redraw machinery. This is a big win and the reason Stage A (canvas + 2D) gets to be small.

---

## 7. Test harness shape

`webgl/MWebGLTest/` is a standalone Carbon CFM app — NOT integrated into MacSurf until Stage G. Mirror of `MacTLSTest/`.

Files:
- `main.c` — drives a fixed test sequence: init, stage smoke (E, E+1), benchmark, dump log, quit.
- `mwebgltest_prefix.h` — includes `mwebgl_prefix.h`.
- `MWebGLTest.rsrc` — byte-identical 'carb'(0) + icon family as MacSurf (per CLAUDE.md "Carbon App Requirements").
- `README.md` — step-by-step CW8 .mcp build instructions (file list, access paths, libraries, partition). The .mcp itself is NOT in source control.
- `Access Paths.xml` — documentation, not built (per macTLS convention).

The test harness exercises each Stage in isolation:
- Stage A: open a 320×240 canvas, fill rectangles, dump as PNG via lodepng (already in MacSurf).
- Stage B: open a WebGL context, call every method, confirm no exceptions, dump `getError()` history.
- Stage C: compile each shader in a curated set, dump info logs.
- Stage D: rasterize a fixed triangle to bitmap, dump as PNG.
- Stage E: full hello-triangle from Duktape script (the test app embeds its own Duktape instance). Compare output to a reference PNG.

The Stage E reference PNG becomes the regression gate. Any change that perturbs hello-triangle output pixel-for-pixel (within a tolerance) is a regression.

---

## 8. Logging and debugging

File-backed log channel: `webgl/os9/mwebgl_log.c`, mirrors `macTLS/os9/ostls_log.c`. Writes to `MacSurf WebGL.log` on Desktop (not `MacSurf Debug.log` — separate channel so WebGL spam doesn't drown other diagnostics). CR-terminated lines, `FlushVol` after every write, hand-rolled formatter with documented specifier set, init-failure silent.

Per-stage log channels (gated by build flags):
- `MWEBGL_LOG_API` — every WebGL call from JS, with arg summary.
- `MWEBGL_LOG_COMPILE` — shader compile diagnostics.
- `MWEBGL_LOG_DRAW` — per-draw-call stats (vertex count, fragment count, ms).
- `MWEBGL_LOG_FRAGMENT` — per-fragment trace (TURN OFF except for tiny test scenes — this is megabytes per frame).

These are off by default; `mwebgl_prefix.h` flips them for diagnostic builds. Same pattern as `MACSURF_FONT_ALIAS_DIAG` in plotters.c.

Title-bar instrumentation: per CLAUDE.md's title-bar probe pattern (with the "last writer wins" caveat), `MS_LOG` writes for hot-path checkpoints — `webgl: ctx`, `webgl: compile OK`, `webgl: draw N`. Strip these aggressively per the established discipline.

---

## 9. Risk register

### High-severity risks

1. **Duktape↔native call cost dominates per-draw overhead on JS-heavy scenes.**
   Mitigation: bundle vertex data uploads (`bufferData` from typed array → memcpy is cheap), batch uniform updates, accept the cost on draw calls themselves (one Duktape call per draw is fine; one per attribute setup is also fine).

2. **Three.js shader complexity overflows the AST walker.**
   500–2000-line auto-generated shaders with deep `#define`-expanded lighting code may not complete in reasonable time even for a single fragment. Mitigation: defer three.js compatibility to v0.3 explicitly. Land hello-triangle, rotating-cube, Shadertoy fragment-only for v0.1.

3. **Memory exhaustion on real pages.**
   A page that allocates a 1024×768 canvas with double-buffer + depth = 6 MB before any textures. Mitigation: hard cap canvas area at context creation, fail gracefully (`getContext` returns null) when over limit, document the cap.

4. **CW8 PPC `long long` codegen miscompiles attribute interpolation.**
   Established hazard per CLAUDE.md. Mitigation: route all rasterizer arithmetic through `float`/`double` (PPC has hardware FPU); avoid 64-bit fixed-point. The same fpmath.h pattern that fixed libcss.

5. **GLSL ES grammar edge cases.**
   Hand-rolled parser misses an obscure form (e.g. `precision mediump float;` declaration at file scope) and rejects valid shaders. Mitigation: test against a curated shader corpus before declaring Stage C closed. The corpus must include shaders from at least three independent codebases (Khronos conformance, Shadertoy, hand-written).

### Medium-severity risks

6. **libdom HTMLCanvasElement type identification.**
   libdom may not distinguish `<canvas>` from generic `<div>`. Mitigation: tag-string comparison at wrapper-creation time is sufficient; no libdom changes required.

7. **`getError` state stack and the `INVALID_OPERATION` cascade.**
   Real apps check `getError` rarely; if our error-stack semantics differ from spec, debug-build pages will lie about the bug. Mitigation: implement exactly per WebGL 1.0 spec (sticky first error, cleared on read).

8. **`preserveDrawingBuffer = false` semantics.**
   Default is `false`, meaning the drawing buffer is cleared after each composite. If we don't honour this, demos that redraw incrementally accumulate junk. Mitigation: implement correctly from Stage D onwards; test with a Shadertoy demo (which often relies on the clear).

9. **Typed arrays in Duktape 2.7.**
   Typed arrays are present in default config but if MacSurf's `duk_config.h` disables `DUK_USE_BUFFEROBJECT_SUPPORT`, we have no `Float32Array`. Mitigation: verify the config at Stage 0; the config in CLAUDE.md ("hand-crafted for Mac OS 9 PPC") needs an audit of buffer-object support.

### Low-severity risks

10. **Cooperative-yield deadline overruns during fragment shading.**
    Long-running shader execution blocks `WaitNextEvent` and freezes the browser. Mitigation: cooperative-yield checkpoint every N fragments (e.g. 4096) that calls `YieldToAnyThread` matching the OT yield pattern. Yield is essentially free on modern G3/G4 but should be present.

11. **Hardware AGL never lands.**
    Phase F slips indefinitely. Mitigation: this is acceptable. Software path covers all functional correctness; hardware is performance polish only.

12. **PortableGL adoption blocked by C99 cleanup cost.**
    If we adopt PortableGL and the C89-port turns out as big as Duktape (28 KLOC), it eats the v0.1 schedule. Mitigation: decision at Stage 0 lock; default to "roll our own rasterizer" unless PortableGL's audit shows trivial C89 conversion.

---

## 10. Out of scope (explicitly deferred)

- **WebGL 2.0 (OpenGL ES 3.0).** Adds `webgl2` context type, integer types, transform feedback, uniform buffer objects, sampler objects, instanced draws as core, vertex array objects as core, multiple render targets, occlusion queries, 3D textures, sRGB framebuffer formats, GLSL ES 3.00 (a different language with explicit `in`/`out` qualifiers and `texture()` instead of `texture2D()`). Hard veto. WebGL 1.0 is the target.

- **WebGPU.** Not on the radar.

- **Compute shaders.** Not in WebGL 1.0.

- **Geometry / tessellation / hull / domain shaders.** Not in WebGL 1.0.

- **MSAA / hardware antialiasing.** Not in software rasterizer for v0.1. Possible later as ordered-grid 2x2 supersampling (4× pixel cost — expensive on a G3).

- **Anisotropic filtering.** Extension only. Defer.

- **Compressed texture formats.** Extension only. Defer.

- **Render-to-texture / framebuffer objects.** Defer to v0.2. Without these, shadow mapping, postprocessing, and ping-pong simulations don't work — but hello-triangle, rotating-cube, and most Shadertoy demos don't need them.

- **Context loss / restore events.** Implement as no-ops returning sane defaults. Our context never loses (no GPU driver to crash); pages just see `isContextLost() === false` forever.

- **`toDataURL` / `toBlob` from a WebGL context.** Requires implementing `readPixels` correctly and PNG encoding. Defer to v0.2.

- **Non-Triangle primitives (`LINES`, `POINTS`, `TRIANGLE_STRIP`, `TRIANGLE_FAN`).** v0.1 supports `TRIANGLES` only. Real content that needs strips/fans will silently fail to draw. Defer to v0.2.

- **Stencil buffer.** Not in default attributes; defer until needed.

---

## 11. Vendored upstream

The vendoring decision at Stage 0:

### What we definitely vendor

None, in v0.1. Roll our own rasterizer (~1500 LOC) is cheaper than porting PortableGL (~20 KLOC across `src/`, C99-heavy) for our specific small subset (TRIANGLES only, RGBA only, no MSAA, perspective-correct varyings).

### What we reference (read but don't vendor)

- **PortableGL** ([github.com/rswinkle/PortableGL](https://github.com/rswinkle/PortableGL)) — algorithmic reference for `gl_impl.c` (rasterizer) and `crsw_math.c` (vec/mat ops). Their shader-as-C-function-pointer pattern is the architectural insight even if we don't take the code.
- **TinyGL** ([bellard.org/TinyGL](https://bellard.org/TinyGL/) and the C-Chads fork) — reference for the rasterizer skeleton (`ztriangle.c`, `zline.c`, `zbuffer.c`). Useful as a "minimal complete" software pipeline; not directly usable because it's fixed-function-only.
- **glsl-optimizer** ([github.com/aras-p/glsl-optimizer](https://github.com/aras-p/glsl-optimizer)) — `src/glsl/glsl_parser.yy` is the cleanest open-source ESSL grammar we can pattern-match against. C++ implementation; we won't lift code, but we'll consult the grammar.
- **Mesa `prog_execute.c`** (historical pre-GLSL software shader executor) — reference for the bytecode interpreter layout in Stage E+1.
- **tinyrenderer** ([github.com/ssloy/tinyrenderer](https://github.com/ssloy/tinyrenderer)) — pedagogical reference for the rasterizer.
- **Khronos WebGL conformance suite** ([github.com/KhronosGroup/WebGL](https://github.com/KhronosGroup/WebGL)) — `sdk/tests/conformance/` for Stage E acceptance test selection.
- **Khronos GLSL ES 1.00 spec** ([registry.khronos.org/.../GLSL_ES_Specification_1.00.pdf](https://registry.khronos.org/OpenGL/specs/es/2.0/GLSL_ES_Specification_1.00.pdf)) — authoritative source for the parser.

### What we vendor only if hardware AGL Stage F lands

- **Apple OpenGL SDK for Mac OS 9** ([macintoshrepository.org/18818-opengl-sdk](https://www.macintoshrepository.org/18818-opengl-sdk)) — for `agl.h`, `gl.h`, `glu.h`, `aglMacro.h`, plus the CW8 stub libraries `OpenGLLibraryStub`, `OpenGLUtilityStub`, `OpenGLMemoryStub`.

---

## 12. Acceptance gate matrix

| Stage | Gate | Verified on |
|---|---|---|
| 0 | Scope locked, audit complete | N/A (paper) |
| A | 2D canvas red-rect-on-yellow renders | G3 iMac OS 9.1 |
| B | Hello-triangle script runs without throwing | G3 iMac OS 9.1 |
| C | Compile shader corpus (5+ shaders) without diagnostics | G3 iMac OS 9.1 |
| D | Hand-driven triangle/cube/textured-quad rendered to bitmap | G3 iMac OS 9.1 |
| E | Khronos hello-triangle renders correctly in MWebGLTest | G3 iMac OS 9.1, **project milestone** |
| E+1 | Rotating-cube benchmark 5× faster than Stage E | G3 iMac OS 9.1 + G4 OS 9.2.2 |
| F | Vertex-color triangle accelerated on a real OpenGL-capable Mac | G4 OS 9.2.2 (only if available) |
| G | WebGL demo on `webgl.mp.ls/hello.html` renders inside MacSurf | G3 iMac OS 9.1 |
| H | Three.js spinning-cube demo loads and renders (any framerate) | G3 iMac OS 9.1 |

SheepShaver gates: **none**. Per macTLS discipline, hardware-only.
Linux gates: per-stage Retro68 `-std=c89 -pedantic-errors` syntax check before every hardware drop.

---

## 13. Open questions for Stage 0

1. **Does Duktape 2.7.0 in MacSurf's build have `Float32Array` / `Uint8Array` / `Int32Array` enabled?** The default Duktape build does; MacSurf's hand-crafted `duk_config.h` might disable buffer-object support to save heap. If disabled, we must re-enable it. **Action: read `browser/libduktape/duk_config.h` and check `DUK_USE_BUFFEROBJECT_SUPPORT`, `DUK_USE_ES6_TYPED_ARRAYS`.**

2. **Does libdom register `<canvas>` as a known tag, or does it fall through to a generic HTMLElement?** If generic, our wrapper just needs to tag-check at creation time; if specialised, we may need to handle an HTMLCanvasElement-specific dispatch. **Action: grep libdom for `HTMLCanvas`.**

3. **What's the current state of MacSurf's content-redraw cycle for arbitrary DOM-mutated elements?** If a canvas's drawing buffer changes via JS without a layout reflow, do we need to manually invalidate the page rect? Per CLAUDE.md's `gw->content_rect` and `InvalWindowRect` discipline, this is well-understood but needs explicit confirmation for the canvas case. **Action: look at how XHR-driven DOM mutations trigger redraws today, mirror that.**

4. **How big is a typical compiled-shader AST in memory?** A 100-line shader probably parses to ~5–10 KB of AST nodes. Times a few hundred shaders on a worst-case three.js page = a few MB. Within budget but worth tracking. **Action: instrument early, fail loud at 1 MB per shader.**

5. **What's the maximum canvas size we permit?** Spec allows arbitrary; we need a hard cap. Proposal: 1024×768 (3 MB RGBA + 1.5 MB depth = 4.5 MB per canvas). 800×600 default cap, 1024×768 absolute max, anything larger returns `null` from `getContext`. **Action: lock in Stage 0.**

6. **Is there cooperative threading risk during long shaders?** A pathological fragment shader (raymarcher with high iteration count) on a 800×600 canvas could lock the browser for seconds. Need a per-fragment-batch yield. **Action: prototype the yield checkpoint in Stage E.**

7. **What does `unpremultipliedAlpha = true` mean for our blit path?** Default WebGL is `premultipliedAlpha = true`; if a page requests false, we have to multiply at compositing time. `CopyMask` in plotters.c expects opaque-or-1bit-mask. **Action: review `macos9_plot_bitmap` flag handling for alpha cases.**

---

## 14. Library-mode shape

Following macTLS: a single public function (well, a small constellation) as the v0.1 surface.

```c
/* webgl/os9/oswebgl.h */

typedef struct OSWebGLContext OSWebGLContext;

OSWebGLContext * OSWebGL_New(int width, int height, const OSWebGLAttribs *attrs);
void             OSWebGL_Dispose(OSWebGLContext *ctx);

/* The bulk of the API lives behind a single dispatch entry point that
 * the Duktape glue calls. Internal use only; not for direct host-app
 * consumption. */
int              OSWebGL_Call(OSWebGLContext *ctx, OSWebGLCallID id,
                              const OSWebGLArg *argv, int argc,
                              OSWebGLArg *retv);

/* Get the canvas drawing buffer as a macos9_bitmap-compatible RGBA
 * pixel buffer for blitting. */
const void *     OSWebGL_GetDrawingBuffer(OSWebGLContext *ctx,
                                          int *out_w, int *out_h, int *out_rowstride);

/* Shader compile / link, decoupled from the dispatch table for the
 * benefit of any future non-WebGL caller. */
OSWebGLProgram * OSWebGL_CompileProgram(OSWebGLContext *ctx,
                                        const char *vs_src, const char *fs_src,
                                        char *out_err, int err_cap);
void             OSWebGL_DeleteProgram(OSWebGLContext *ctx, OSWebGLProgram *p);
```

The Duktape glue in `macsurf_js_webgl.c` translates each `WebGLRenderingContext` method to an `OSWebGL_Call` dispatch. This keeps the public library API stable and small while letting the JS-binding TU stay decoupled from internal state representations.

Host wiring inside MacSurf:
```c
/* once, at canvas creation */
OSWebGLAttribs attrs = { .alpha = 1, .depth = 1, ... };
OSWebGLContext *ctx = OSWebGL_New(canvas_w, canvas_h, &attrs);
/* attached to canvas element wrapper */

/* on every page paint */
const void *pixels;
int w, h, stride;
OSWebGL_GetDrawingBuffer(ctx, &pixels, &w, &h, &stride);
/* feed those pixels into a macos9_bitmap that ctx->plot->bitmap can dispatch */

/* on canvas destruction */
OSWebGL_Dispose(ctx);
```

This shape:
- Decouples WebGL from MacSurf for testing (MWebGLTest harness uses it directly with no Duktape).
- Decouples WebGL from Duktape for any future host (a Carbon CFM app that just wants software 3D could call OSWebGL directly).
- Keeps the public surface tiny and stable across v0.1 → v0.x changes.
- Mirrors macTLS's `OSTLS_Fetch` discipline.

---

## 15. Schedule shape

This is a rough sketch; macTLS's actual cadence was nine stages over about three months of focused work, with most of the time in Stages A through B3 (compiler-and-handshake) and Stages C and D (the abandoned proxy and the async-API replumbing).

WebGL's analogous stages:
- **Stage 0** (this scope + audit): 1 sprint.
- **Stage A** (canvas + 2D): 1–2 sprints.
- **Stage B** (WebGL stub): 1 sprint.
- **Stage C** (GLSL compiler): 3–5 sprints. **The long pole.**
- **Stage D** (rasterizer): 2–3 sprints.
- **Stage E** (full pipeline, AST walker): 2 sprints (mostly debugging the connections).
- **Stage E+1** (bytecode): 2 sprints.
- **Stage F** (hardware AGL): optional, deferred.
- **Stage G** (MacSurf integration): 1 sprint after Stage E.

If a sprint is "a focused multi-day push," realistic v0.1 ship is on the order of three months. If sprints are slower, longer. The compiler is the long pole; everything else is mechanical-but-tedious.

---

## 16. The recommendation

1. **Lock Stage 0 with this scope document and an audit pass.**
2. **Adopt the macTLS staging discipline literally** — `webgl/AUDIT.md`, `webgl/MWebGLTest/`, `webgl/os9/`, hardware-gated stages, library-mode public surface, archive-failed-attempts-with-postmortems.
3. **Roll our own rasterizer in C89.** Don't vendor PortableGL.
4. **Roll our own GLSL ES 1.00 compiler in C89.** Don't vendor glsl-optimizer or ANGLE.
5. **Start with Stage A (canvas + 2D context).** It's small, it's mostly mechanical, it unblocks the redraw plumbing, and it ships measurable page-compatibility wins (many pages probe canvas existence) before WebGL machinery exists.
6. **Software-first, hardware never (until proven necessary).** AGL Tier 0 is interesting but the offscreen path is software anyway on every OS 9 GPU. Don't spend cycles on it until Stage E+1 says we need it.
7. **The proof-of-life target is the Khronos hello-triangle demo rendering on a G3 iMac.** That's the project. Anything that renders at all closes the bet.

WebGL on OS 9 is not impossible. It is — like macTLS — a question of patience, staging, and refusing to chase the wrong rabbit. The shader-on-CPU problem reduces to "compile a small language and walk an AST," both of which are bounded engineering tasks. The rasterizer reduces to textbook scanline triangle filling. The integration into MacSurf reduces to "another `struct macos9_bitmap`, blitted via the existing plotter." There is no novel research required — only careful, disciplined construction in C89 against a deliberately small subset of a deliberately small API.

Let's build it.

---

## Appendix A — file tree at v0.1

```
webgl/
├── SCOPE.md                          this document
├── AUDIT.md                          Stage 0 audit + decision log (to write)
├── README.md                         project overview (to write)
├── os9/                              the actual code
│   ├── oswebgl.h                     public library API
│   ├── oswebgl.c                     dispatch entry point
│   ├── mwebgl_state.{h,c}            GL state machine
│   ├── mwebgl_glsl.{h,c}             lexer + parser → AST
│   ├── mwebgl_glsl_check.c           semantic check + linker
│   ├── mwebgl_exec.{h,c}             AST walker (+ bytecode at E+1)
│   ├── mwebgl_raster.{h,c}           software rasterizer
│   ├── mwebgl_math.h                 vec/mat types and ops
│   ├── mwebgl_log.{h,c}              file-backed log channel
│   ├── mwebgl_prefix.h               CW8 build config
│   ├── macsurf_js_canvas.c           HTMLCanvasElement Duktape wrapper
│   ├── macsurf_js_canvas2d.c         2D context bindings
│   ├── macsurf_js_webgl.c            WebGL context bindings (~140 natives)
│   └── archive/                      dead-end attempts with READMEs
├── MWebGLTest/                       standalone Carbon test harness
│   ├── main.c
│   ├── mwebgltest_prefix.h
│   ├── MWebGLTest.rsrc
│   ├── Access Paths.xml              documentation (not built)
│   └── README.md                     CW8 build instructions
├── tools/
│   ├── audit_c89.py                  mechanical C99-ism sweep
│   ├── shader_corpus/                test shaders for Stage C
│   └── make_carb_rsrc.py             (copy of macTLS's)
└── docs/
    ├── glsl-es-grammar-notes.md      our parser's deviations / additions
    ├── rasterizer-notes.md           algorithm notes, references
    ├── stage-X-postmortem.md         per stage close-out
    └── runs/YYYY-MM-DD-stage-X.txt   canonical green-light run logs
```

## Appendix B — reference URLs

- PortableGL — [github.com/rswinkle/PortableGL](https://github.com/rswinkle/PortableGL)
- TinyGL C-Chads fork — [github.com/C-Chads/tinygl](https://github.com/C-Chads/tinygl)
- TinyGL Bellard original — [bellard.org/TinyGL](https://bellard.org/TinyGL/)
- glsl-optimizer — [github.com/aras-p/glsl-optimizer](https://github.com/aras-p/glsl-optimizer)
- glsl-optimizer parser — [github.com/aras-p/glsl-optimizer/blob/master/src/glsl/glsl_parser.yy](https://github.com/aras-p/glsl-optimizer/blob/master/src/glsl/glsl_parser.yy)
- tinyrenderer — [github.com/ssloy/tinyrenderer](https://github.com/ssloy/tinyrenderer)
- ANGLE — [chromium.googlesource.com/angle/angle](https://chromium.googlesource.com/angle/angle)
- Khronos WebGL conformance — [github.com/KhronosGroup/WebGL](https://github.com/KhronosGroup/WebGL)
- Khronos WebGL 1.0 spec — [registry.khronos.org/webgl/specs/latest/1.0/](https://registry.khronos.org/webgl/specs/latest/1.0/)
- Khronos GLSL ES 1.00 spec — [registry.khronos.org/OpenGL/specs/es/2.0/GLSL_ES_Specification_1.00.pdf](https://registry.khronos.org/OpenGL/specs/es/2.0/GLSL_ES_Specification_1.00.pdf)
- Apple OpenGL SDK for Mac OS 9 — [macintoshrepository.org/18818-opengl-sdk](https://www.macintoshrepository.org/18818-opengl-sdk)
- Apple OpenGL 1.2.1 runtime — [macintoshrepository.org/52177-opengl-1-2-1](https://www.macintoshrepository.org/52177-opengl-1-2-1)
- Khronos WebGL hello-triangle reference (from agent research) — see Appendix C.

## Appendix C — the proof-of-life code

```html
<!DOCTYPE html>
<html>
<head><title>MacSurf WebGL Hello Triangle</title></head>
<body>
<canvas id="c" width="320" height="240" style="border:1px solid black"></canvas>
<script>
var canvas = document.getElementById('c');
var gl = canvas.getContext('webgl');
if (!gl) { document.title = 'no webgl'; }
else {
  var vs = gl.createShader(gl.VERTEX_SHADER);
  gl.shaderSource(vs, 'attribute vec2 p; void main(){ gl_Position=vec4(p,0.,1.); }');
  gl.compileShader(vs);

  var fs = gl.createShader(gl.FRAGMENT_SHADER);
  gl.shaderSource(fs, 'void main(){ gl_FragColor=vec4(1.,.5,0.,1.); }');
  gl.compileShader(fs);

  var prog = gl.createProgram();
  gl.attachShader(prog, vs);
  gl.attachShader(prog, fs);
  gl.linkProgram(prog);
  gl.useProgram(prog);

  var buf = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, buf);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0,1, -1,-1, 1,-1]), gl.STATIC_DRAW);

  var loc = gl.getAttribLocation(prog, 'p');
  gl.enableVertexAttribArray(loc);
  gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);

  gl.clearColor(0,0,0,1);
  gl.clear(gl.COLOR_BUFFER_BIT);
  gl.drawArrays(gl.TRIANGLES, 0, 3);
  document.title = 'WebGL hello triangle';
}
</script>
</body>
</html>
```

When this renders an orange triangle on a black background inside MacSurf on a G3 iMac running Mac OS 9.1, the project is closed.

---

*End of scope document. Next: Stage 0 audit.*
