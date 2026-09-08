# E4 asynchronous causal provenance

`async_id` identifies one accepted asynchronous continuation. It is a
monotonic scalar, never a pointer. `0` means that MacSurf has no trustworthy
causal identity. `parent_async` is the scalar identity active while a new
continuation was accepted; it is frozen at registration and may name a record
which has since rolled out of history.

Each record freezes registration origin (`nav`, `frame`, `doc`, `source`,
`script`, `task`, `realm`, `heap`, and context generation). Delivery facts are
recorded separately as state transitions. Timers receive one identity at
`setTimeout`/`setInterval`; one-shots retire after delivery, while intervals
return to `queued` until cancellation or retirement. An accepted XHR `send`
owns one identity across redirects and native request IDs. Abort is
`cancelled`; a realm which cannot legally receive delivery is `abandoned`.

Element listener identity is the accepted `(type, callback, capture)` tuple.
The native listener registry stores an index-aligned `_LA` array beside `_L`
and `_LC`; duplicate registration allocates no record. A firing listener
temporarily exposes its own identity, restoring the previous scope afterward.
`on*` replacement handlers remain a separate surface and are not represented
as ordinary listener registrations.

States are `registered`, `queued`, `firing`, `fired`, `cancelled`, `retired`,
and `abandoned`. Terminal states reject later updates. `fired` is deliberately
nonterminal so intervals can re-enter `queued` and normal delivery can retire.

`MSdg GET async after=<id> limit=<n>` is a bounded cursor stream. The reply
reports history loss via the standard header and advances `next_after` only to
the last emitted row; repeated reads are stable. Origin and parent scalars are
not looked up during serialization, so they remain useful after script, task,
source, or async history rolls over.

QuickJS exposes execution of pending jobs but no safe public per-job origin
hook. MacSurf does not monkey-patch Promise. Promise rejection diagnostics may
have realm/task facts, but use `async=0` unless a separately valid callback
boundary supplied an identity.

Example: `script 84 -> listener async 152 -> timer async 153 parent=152 ->
XHR async 154 parent=153 -> error async=154`. A Promise rejection without a
safe engine continuation hook is recorded with `async=0`.

Deferred surfaces: requestAnimationFrame, observers, fetch promise jobs,
module continuations, window `on*`, and script load/error callbacks require
their own accepted-registration boundary and are not inferred by E4.
