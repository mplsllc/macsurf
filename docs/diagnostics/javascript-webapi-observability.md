# JavaScript and Web API observability

Phase 4 records observed JavaScript failures and Web API compatibility outcomes
as bounded, pointer-free aggregates. It does not inventory every unsupported API:
a row exists only after a page reaches an instrumented binding outcome.

`MSdg GET javascript` reports script-load failure, parse failure, runtime
exception, unhandled Promise rejection, and event-handler exception classes.
`MSdg GET js_api_gaps` reports normalized Web API rows such as
`js.api.ResizeObserver.missing` and `js.api.Element.animate.partial`.
`MSdg GET promise_rejections` and `MSdg GET event_handlers` provide the focused
aggregates. `MSdg GET gapreport` includes the normalized Web API keys alongside
the existing CSS and execution evidence.

Each retained row copies only bounded text plus first/last navigation,
document, frame, script, and task IDs. No QuickJS value, context, exception,
or DOM pointer is retained. Repeated equal keys increment `count`; distinct
interface/member/outcome triples remain distinct. The 64-row Web API table
reports `dropped` and `loss_explicit=1` when it fills.

Capability outcomes map to `missing`, `partial`, `stub_used`,
`fallback_used`, and `call_failed`. Existing generic capability hooks retain
their original output and additionally produce an observed-use Web API row.
The extended hook accepts separate interface and member names where a binding
can provide them.

QuickJS exposes no safe universal hook for arbitrary global lookup, so an
unhandled `window.X` lookup remains explicitly classified as unobservable in
the coverage matrix unless it reaches an instrumented host binding. This is
deliberately different from claiming that every absent global was observed.
Promise rejection tracking has realm/document attribution but QuickJS does not
provide safe per-job causal origin, so it must not inherit an ambient async ID.

The deterministic `diag-phase4` harness includes Test 112 (normalization,
deduplication, distinct outcomes, and explicit bounded loss) and Test 113
(runtime, handler, and Promise scalar provenance). It runs after the Phase 2/3
retention tests, which cover realm retirement and context-history loss.
