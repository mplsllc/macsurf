# JavaScript error provenance (E3)

Each detailed error owns scalar nav/frame/doc/source/script/task/realm/heap/ctx_gen
IDs. No runtime pointers survive in error storage. Unknown IDs stay zero.

## Design audit

- `ms_diag_scope.prev_source` saves/restores nested source state for scripts and
  tasks, independently of either history ring. A newly entered script starts at
  source=0 until its caller supplies the accepted discovery ID.
- `ms_diag_error_record_ex` accepts a frozen `ms_diag_error_provenance` and returns
  the exact `unsigned long` occurrence ID. It never fills zero fields by guessing
  from globals. The compatibility API preserves its original kind/boundary/reason.
- Synchronous failure captures the active execution before text coercion, then
  uses the returned error ID with `ms_diag_script_note_compile/execute` and a
  stable script ID. Asynchronous occurrences never modify that execution row.
- Timer and XHR slots add `origin_source_id` alongside their existing scalar
  origin script and realm identities. Timer delivery snapshots before one-shot
  clearing or callback reentry. Both deliveries freeze the task ID before calling
  JavaScript. Neither searches completed script/source history.
- XHR delivery scopes a scalar callback tuple with save/restore for nested calls.
  Listener catches use it only when realm, generation, and task match. A direct
  user call of `_fire` outside native delivery has source/script unavailable.
- Event listener registration provenance is not available in the listener model.
  Errors retain the executing realm/document and task; source/script remain zero.
  Existing element/window listener catches now report occurrences before their
  unchanged logging expression. Generic EventTarget exceptions that propagate to
  a caller remain attributed at the catching boundary.
- QuickJS invokes its rejection tracker when a promise becomes rejected without
  a handler, both synchronously and from later jobs. It also reports a later
  handled transition. E3 records the unhandled-at-report-time occurrence, not a
  claim of permanent non-handling. Queued jobs do not expose source/script origin;
  these IDs remain zero. Module evaluation can return a rejected Promise and
  thereby produce tracker occurrences while synchronous execution returns OK.
- Realm lookup uses the existing registry while the context is live, including
  its scalar tuple when navigation has been requested. Serialization does not
  inspect any QuickJS object or runtime.
- Failures: none, parse_failed, runtime_failed, promise_rejection, handler_failed.
  Phases: none, compile, execute, callback, promise. Boundaries: none, script,
  timer, xhr, event, promise, api, module. Generic callers retain numeric boundary.
- The common exception logger reuses its single exception-to-string conversion.
  Script/timer native boundaries each remain one conversion plus one stack read
  (and a string-only conversion if stack is a string). Module logging decreases
  from two exception conversions to one. Promise and native XHR paths retain their
  previous conversions. Swallowed XHR listener errors add zero text/property
  inspection; message_status=empty. Swallowed event listeners retain their
  original logging property reads/coercion and add none for E3.
- Paged rows use a 1536-byte scratch buffer; formatting failure or insufficient
  destination space stops before advancing the cursor. E1/E2 history, dictionary,
  loss, and pagination fields remain independent. Legacy row writes also require
  enough space for the complete row and footer.

## Memory measurements

Actual Linux harness DWARF sizes, before/after E3:

| Structure | Before | After |
|---|---:|---:|
| error record | 72 | 120 |
| timer | 200 | 208 |
| XHR slot | 520 | 528 |
| scope | 24 | 32 |

The Linux error ring has 128 records / 15360 bytes. The callback scalar tuple is
72 bytes globally, plus saved tuples on the C stack; no queued task record grows.

Clang PowerPC 32-bit layout of the extracted actual declarations measures error
40→64 bytes (ring 8192 bytes, +3072), scope 12→16, timer 120→120 (uses padding),
XHR 280→288. This is a cross-compiler layout check, not a CW8 sizeof measurement;
CW8 alignment may differ for timer/XHR. Each slot adds exactly one unsigned long.
The callback tuple is nine unsigned longs (36 bytes on the target).

## Local evidence

`make -C harness diag-e3` runs Test 102f and a coercion comparison probe. Coverage:
nested source restore; real successful/parse/runtime execution; arbitrary thrown
objects, proxy, number and string; compile-stage modules and Promise semantics;
actual timer callbacks after script and source rollover; event/XHR listener catches
(the XHR test replaces transport, runs real native delivery); aggregate/detail
counts; retirement and byte-stable reread; maximum 32-bit provenance values and
119-byte percent-encoded text; dictionary saturation; ring rotation, paging and
small-buffer cursor preservation.

The identical coercion probe passes on pre-E3 HEAD 7857a809a and E3:
script=1 toString + 1 stack getter; timer=1 toString + 1 stack getter.

The full harness reproduces Test 104a's failure on both untouched pre-E3 production
sources and E3. The focused `diag-phase3` mode passes 104a on both known run shapes;
the full run accumulates earlier failure counters. E3 does not modify 104a.

Private per-attempt logs are under `.private/research/e3-audit/`. No local result
is hardware verification. `harness/e3-fixture/` supplies the controlled hardware
workload; collect before and after navigating to `after.html`.

## G3 hardware evidence

Attempt 1421 built `c04be7c32` on the real G3 with an empty CodeWarrior
Errors & Warnings export, a changed binary identity, successful launch, and
`MacSurf1421.sit`. The controlled workload is committed in macsurf-web as
`c4e78b5` and served at `https://macsurf.org/e3-provenance.html`.

`2026-09-08_113446_msdiag.txt` recorded ten retained detailed errors with no
ring overwrite, dictionary drop, or transport truncation. They prove one parse
failure; four runtime failures; two separately reported Promise rejections;
three handler failures (event, timer, XHR); two isolated frame/document rows
for the same error text; frozen timer task/source/script ownership; and truthful
unavailable event/Promise origins. The controlled hostile-value assertions did
not produce `E3-COERCION-FAIL`.

`2026-09-08_113821_msdiag.txt`, after navigation to `e3-after.html`, retained
all ten E3 rows byte-for-byte, including their original nav/frame/doc/source/
script/task/realm/heap/context-generation scalars. The hardware checker output
is `attempt-1421/e3-before-check.txt` and `e3-after-check.txt`; both pass.

The source ledger's historical `script=` display field remains zero in this
capture, including pre-existing unrelated sources. E3 does not rely on that
field: each failure joins its source through the frozen error `source=` and the
authoritative execution-ledger `source=` scalar. This observation is outside
the closed E3 change.

JAVASCRIPT ERROR PROVENANCE GATE CLOSED.

## Remaining limits

Event listener and Promise origins are unavailable as described above. Swallowed
listener messages are deliberately unavailable. Rejection reports are occurrences,
not deduplicated promises. Repeated internal module promise rejections can therefore
produce multiple tracker occurrences. Other browser API surfaces are outside E3.
