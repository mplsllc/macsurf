# MacSurf Page-Load Speed Analysis (2026-05-25)

> Cold-load performance audit done after v0.6 macTLS first-light. Reads
> the live tree at commit-current; no code changes proposed in this doc.
> Time estimates are derived from code structure plus hardware
> characteristics (G3 233 MHz, 16 MB Carbon partition, ethernet to local
> proxy / wider internet). Where a number can't be inferred from code
> alone, the doc says "needs instrumentation."

## TL;DR

Five wins dominate. In order of estimated wall-clock saved on a cold
mactrove load (40-ish sub-resources):

1. **HTTPS keep-alive pool.** Every sub-resource currently does a
   full TCP+TLS handshake (~600-900ms each on a 233 MHz G3). 40
   handshakes per page; pooling at host:port collapses the page's
   wall-clock TLS cost from ~30 s to ~1.5 s. **Estimated saving on
   cold mactrove: 20-25 s.**
2. **Re-enable the disk-cache HIT path** (currently disabled per
   [fixes222 / macos9_https_fetcher.c:856](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L856)).
   STORE is live, so visiting any page twice already produces a
   complete cache; we just refuse to read from it. Re-enabling lifts
   warm reloads from "as slow as cold" to "near-instant" for
   text/CSS/JSON. **Estimated saving on warm mactrove reload: 25-30 s
   (the entire fetch phase collapses).**
3. **Drive the event loop sleep from the scheduler queue.** The
   `WaitNextEvent` sleep ticks are hardcoded to `1` regardless of
   whether anything is pending; we should sleep up to
   `macos9_get_next_delay()` when no fetch is active, and 0 ticks
   only while a fetch is actively pumping. Today even idle UI burns
   ~60 main-loop passes/sec. **Estimated saving on cold load: small
   (~0.5-1 s reclaimed CPU for libcss/layout), but a big win for
   battery/heat and for any future preemptive yield window.**
4. **Coalesce the hover-driven recascade+reformat storm.** Every
   `BROWSER_MOUSE_HOVER` that crosses a new DOM node triggers
   [html_recascade_tree + browser_window_schedule_reformat](../../browser/netsurf/content/handlers/html/interaction.c#L1383).
   Recascade walks up to 4000 boxes; reformat then re-lays out the
   whole document. On a hover-heavy page this fires several times per
   second as the cursor moves. The hover poll runs once per main-loop
   pass with no debounce. **Estimated saving while interacting with a
   loaded page: 1-3 s of jank per hover trace.**
5. **Raise the resource-class image cap (and re-think IMAGE refusal).**
   `MAX_IMG_F=8` is fine in concurrency terms but combined with the
   "refuse IMG on global cap" path in [macos9_http_setup](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L1317),
   sub-resource fetches on rich pages get NULL-returned and never
   retry; the broken-image placeholder cost is paid for free. With
   HTTPS keep-alive (#1) the global cap can go to 24 or 32 without
   blowing memory. **Estimated saving on image-heavy pages: 2-5 s,
   plus visual completeness wins.**

Single biggest one-week ROI: **HTTPS keep-alive (#1)** unlocks
everything else and gives an immediate cold-load 5-10x improvement
that's directly user-visible.

## Baseline: what one cold mactrove load looks like today

Assumptions: 233 MHz G3, 100 Mbit ethernet, MacSurf v0.6 (macTLS
native HTTPS, no proxy for https). mactrove.com: ~200 KB HTML, ~50
KB CSS in a few files, ~500 KB images across ~25 sub-resources. Cold
= no DNS in OS cache, no MacSurf disk cache hit.

Phase budget (per-resource where applicable):

| Phase | Cost | Pipelined? | Yield? |
|---|---|---|---|
| DNS resolution (OTInitDNSAddress) | 30-80ms first time, ~5ms cached by OT/DNR | No | Inside OT, opaque |
| TCP connect (OTConnect, sync blocking) | 30-90ms (1 RTT to internet) | Per slot; up to 4 HTTPS slots in flight | Blocking; only macTLS notifier yields |
| TLS handshake (ECDHE + cert chain validate) | 400-800ms cold | Pipelined w/ TCP within a slot | Yes — OSTLS_Pump 8-step slice yields per main-loop pass |
| HTTP request send | 1-3ms | n/a | Yes |
| Response headers (1st byte after request) | 30-90ms (1 RTT) | n/a | Yes |
| Body streaming (chunked decode + FETCH_DATA per OTRcv) | 50-200ms for 10-50 KB body | n/a | Yes |
| Connection teardown (OTSndOrderlyDisconnect + OTCloseProvider) | 5-20ms | n/a | Yes |
| NetSurf parse (libdom, libhubbub) | 100-400ms for 200 KB HTML | No | At fetch-pump boundary only |
| CSS cascade (css_select_style per element) | 1-3ms per element × ~1000 elements = 1-3 s | No | None during selection |
| Layout (layout_document, full tree) | 200-500ms typical page | No | None during layout |
| Redraw (plotters.c, ~5-50 box visits per dirty rect after fixes77f walker prune) | 10-150ms | n/a | Yes |

**Cold-load wall-clock (approximate, fixes98-107 fetch path):**

- 40 HTTPS handshakes × ~700ms average = ~28 s of TLS alone
  (handshakes pipeline across ≤4 slots, so the wall-clock floor is
  not 28 s — it's ~28 s / 4 = ~7 s if perfectly pipelined). The
  problem isn't slot concurrency, it's that **per-slot TLS is paid
  every time** because of #1.
- 200 KB HTML parse + ~1000-element cascade: ~2-4 s.
- Initial layout + reformat ×N (see "reformat thrashing" below):
  ~1-3 s.
- Image decode (PNG via lodepng, others via QT): ~50-300ms per
  image × ~10 images on screen = ~1-3 s.

Rough total cold-load mactrove on G3 today: **20-40 s**, dominated
by repeated TLS handshakes. That matches the user's "page loading is
functional but slow" description.

Realistic floor with keep-alive (#1) + cache HIT (#2) + caps (#5):
**5-8 s cold, < 2 s warm.** That gets into iCab/IE5-on-G3 territory.

## Bottleneck inventory (ranked)

### 1. HTTPS handshake N times per page (biggest single win)

**Evidence.** [macos9_https_fetcher.c:12](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L12):

> V1 scope: - No keep-alive pool (every fetch opens a fresh TLS endpoint)

[macos9_https_fetcher.c:137-148](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L137) `hctx_clear()` disposes
the `OSTLSConnection` unconditionally at every terminal callback.
[macos9_https_fetcher.c:675](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L675) hard-codes
`Connection: close` in the GET. There is no `https_ep_pool` and no
`OSTLSConnection_reset_for_reuse()` API in macTLS.

For comparison, the HTTP fetcher has a 16-entry keyed endpoint pool
([macos9_http_fetcher.c:252-332](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L252))
and the `ep_pool_take` / `ep_pool_return` machinery survives the
fixes92/94/99 sprint. Pool reuse hits within the same page (CSS,
images on the same origin reusing the document-fetch TCP connection)
and across navigations to the same host.

**Cost.** A cold TLS 1.2 ECDHE-ECDSA-AES128-GCM-SHA256 handshake on
a 233 MHz G3 with BearSSL is dominated by:
- TCP 1 RTT to internet (30-90ms)
- ClientHello (1 RTT)
- ECDHE keygen + cert chain validate (200-400ms CPU on G3) + cert chain transmission (~5 KB, ~1ms on 100 Mbit)
- ServerHelloDone + ClientKeyExchange + ChangeCipherSpec + Finished (1 RTT + ~50ms CPU)

Per-handshake: ~600-900ms wall-clock. With 40 sub-resources and a
4-slot concurrency cap, the wall-clock TLS floor is ~6-9 s; with 32
sub-resources it's ~5-7 s.

**Fix shape.** Add an `https_ep_pool[POOL_SIZE]` keyed on
`host:port`, parallel to the HTTP pool. Replace `hctx_clear`'s
`OSTLS_Close + OSTLS_Dispose` with `https_pool_return` when the
response was fully consumed AND no `Connection: close` came back AND
the response status is 200/304/204 etc. (don't pool an aborted
endpoint or one mid-failure).

The macTLS side needs one helper: `OSTLSConnection *
OSTLS_NewFromExisting(OSTLSConnection *prev)` or equivalent that
resets the BearSSL state without throwing away the TLS session keys,
so the next request just writes plaintext into `sendapp`. BearSSL
supports session resumption (`br_ssl_client_get_session_parameters` /
`br_ssl_client_use_session_parameters`); MacSurf currently doesn't
exercise it because every connection is a new OSTLSConnection. Even
without session resumption, **just keeping the TLS-layer open and
sending another `GET` on the same connection** is the standard
HTTP/1.1 keep-alive pattern and saves 95% of the handshake cost.

Flip the request line to `Connection: keep-alive`. Add the same
`Connection: close` parsing the HTTP fetcher does at
[mfs_parse_headers:783](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L783).
Both `content_length`-framed and chunked-framed responses make a
keep-alive endpoint safe to reuse — those framing paths already
exist in the HTTPS fetcher.

**Estimated savings on cold mactrove: 20-25 s.** This is the single
biggest available win.

**Risk.** Pool soundness — half-closed connections, ECONNRESET
between requests, server-side idle timeouts. The HTTP fetcher's
`ep_pool_take` drains OT events and rejects connections with
non-zero OTLook; mirror that pattern. CF and Google CDN's habit of
closing TLS after handshake (already worked around in
[fixes228 retry path](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L619))
will trip pool reuse harder, may need a `keep_alive_first_attempt`
flag that falls back to fresh connection on benign-close.

### 2. Disk-cache HIT path disabled

**Evidence.** [macos9_https_fetcher.c:855-861](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L855):

> /* fixes222 — disabled: the cache-hit serving path in fixes218 sent
>    NetSurf into about:query/fetcherror because the synthetic
>    FETCH_HEADER / FETCH_DATA / FETCH_FINISHED sequence didn't match
>    what html_create expects. Re-enable once the header dispatch is
>    reworked to look exactly like the live path. The cache STORE side
>    stays on (cached bodies just go unused). */

The HTTP fetcher's cache hit path is *enabled* and apparently working
([macos9_http_fetcher.c:392-417](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L392),
[mfs_poll_one:896-925](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L896)).
Body is delivered in one `FETCH_DATA` shot; a synthetic
`Content-Type:` header is emitted; `fetch_set_http_code` is called.
The HTTPS fast-path in [hctx_poll:540-587](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L540)
mirrors it but is unreachable because `setup` never sets
`c->state = HS_CACHEHIT`.

The cache STORE path runs at every fetch terminal callback. After
one cold mactrove load, the cache folder on the Desktop already
contains the full set of text/html, text/css, application/javascript
bodies (subject to `MACSURF_CACHE_MAX_BYTES`).

**Cost of leaving it disabled.** Every navigation, including Back
button and Reload, refetches every resource from the network with
full TLS handshake. A user that visits mactrove.com twice in a
session pays 60-80 s of wall-clock TLS time that's avoidable.

**Fix shape.** Reproduce the HTTP fetcher's working cache-hit pump
in the HTTPS fetcher's setup + poll. The fixes222 failure mode
("about:query/fetcherror") was specifically that the synthetic
header dispatch didn't match what `html_create` expects — diff the
HTTP path's pump against the HTTPS path's pump to find the missing
piece. Likely candidates: ordering of `FETCH_HEADER status_line`
vs. `Content-Type:`, presence of `fetch_set_http_code` before the
FETCH_FINISHED, or one of `FETCH_HEADER` taking a trailing CRLF that
the synthetic version omits.

Add a cache lookup call at the top of `macos9_https_setup` (between
slot allocation and `HS_QUEUED` transition):
```c
if (macos9_cache_lookup(url_str, &c->cache_hit_body,
        &c->cache_hit_len, c->cache_hit_mime,
        sizeof(c->cache_hit_mime), &c->cache_hit_status)) {
    c->state = HS_CACHEHIT;
    // skip OSTLS_New / handshake entirely
}
```
The HS_CACHEHIT branch at [macos9_https_fetcher.c:540](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L540)
is already there waiting.

**Estimated savings on warm reload: 25-30 s** (the entire fetch
phase). On a fresh navigation to a different page that shares assets
(common CSS/JS): 5-15 s.

**Risk.** Cache-staleness on a busy site that's actively changing.
The cache header has no Expires/ETag/Last-Modified, so a stale cache
hit serves potentially old content forever. Either add a TTL
(simplest: cache invalidates after N days, reuse the existing
`disc_cache_age` nsoption default of 28 days) or a Cmd-Shift-Reload
that sets `macsurf_http_skip_next_cache=1` (the flag already exists
and is exercised by HTTP).

### 3. Sleep tick / event-loop pacing

**Evidence.** [main.c:561](../../browser/netsurf/frontends/macos9/main.c#L561):

```c
if (WaitNextEvent(MACOS9_EVENT_MASK, &ev, 1, NULL)) {
```

The sleep argument is **the literal constant `1`** — one tick (~16.7
ms). That means even with no fetcher active and no scheduler
callback due, the main loop yields exactly 1 tick to other
processes and immediately wakes for the next idle pass. With a
fetcher active, the loop runs at ~60 Hz regardless of OSTLS_Pump's
actual readiness state.

[schedule.c:182](../../browser/netsurf/frontends/macos9/schedule.c#L182) exposes
`macos9_get_next_delay()` which returns `MACOS9_SCHED_IDLE_SLEEP=15`
when the queue is empty, the actual delta otherwise. **This function
is never called from main.c.**

Compare to peer ports:
- [riscos/gui.c:1864-1890](../../browser/netsurf/frontends/riscos/gui.c#L1864):
  `wimp_poll_idle` is given the next scheduled callback's deadline
  as the poll timeout, so the WIMP wakes only when there's work or
  when the deadline fires.
- [atari/gui.c:120](../../browser/netsurf/frontends/atari/gui.c#L120):
  `aes_event_in.emi_tlow = schedule_run()` — the scheduler returns
  the next-event ms, and `evnt_multi_fast` sleeps for that long.

**Cost.** Two flavors:
- **Idle CPU burn.** At 60 wakeups/sec, every wake fires
  `macos9_schedule_run`, `fetch_pump`, `macos9_windows_te_idle`,
  `macos9_windows_process_deferred`, `macos9_poll_mouse_hover`,
  `macos9_animation_tick`. Most of these are cheap, but
  `fetch_pump` iterates all four fetchers' `.poll` callbacks
  unconditionally ([fetch.c:290-299](../../browser/netsurf/content/fetch.c#L290)).
  Each `.poll` walks its slot array. **Adds tens of ms/sec of
  pure-overhead CPU** that isn't moving the page load forward.
- **No coalescing of pump cycles to OT readiness.** OSTLS_Pump
  with `max_steps=8` will frequently return having done nothing
  because BearSSL is waiting on bytes; calling it again 16 ms
  later doesn't change that.

**Fix shape.** Two-tier:
- When `macos9_https_fetcher_active() == 0 && macos9_http_fetcher_active() == 0 && macos9_stub_fetcher_active() == 0`, set the WNE sleep to `macos9_get_next_delay()` (clamped to ~15 ticks). This is the "idle UI" path.
- When a fetcher is active, sleep with `1` tick (current behavior) but consider falling back to 0 only briefly — a 0-tick yield is a true polling loop that starves background processes (USB Overdrive, etc.).

An even tighter version uses OT notifier signals to drive the loop —
the notifier already fires `kOTSyncIdleEvent` and we can `WakeUpProcess`
the host. That's a bigger refactor; do the simple "honor next_delay"
first.

**Estimated savings on cold load:** small — maybe 0.5-1 s of CPU
reclaimed for parsing/layout/cascade. **Big win for idle behavior**:
post-load, MacSurf will stop being a 60 Hz polling loop, which helps
heat/fan noise on retro hardware and behavior under USB Overdrive.

### 4. Hover-driven recascade+reformat storm

**Evidence.** [main.c:595-615](../../browser/netsurf/frontends/macos9/main.c#L595) `macos9_poll_mouse_hover`
fires `browser_window_mouse_track(... BROWSER_MOUSE_HOVER, ...)` on
every main-loop pass when the cursor moves. Inside NetSurf core,
[interaction.c:1383-1388](../../browser/netsurf/content/handlers/html/interaction.c#L1383):

```c
if (changed && bw != NULL) {
    macsurf_debug_log_write("dyn change: recascade + reformat");
    html_recascade_tree(html);
    browser_window_schedule_reformat(bw);
}
```

`html_recascade_tree` is a full DOM walk capped at 4000 boxes
([box_construct.c:2010-2150](../../browser/netsurf/content/handlers/html/box_construct.c#L2010)). For a moderate page (~1000 boxes), that's an O(n) walk with per-node `nscss_get_style` calls (1-3ms each) — **easily 1-3 s of CPU per recascade**.

`browser_window_schedule_reformat` queues a full layout pass
(`html_reformat → layout_document`). The full path is gated by
`reformat_in_progress` ([window.c:251-253](../../browser/netsurf/frontends/macos9/window.c#L251)) so re-entrance is OK,
but back-to-back recascades from sequential hover ticks each pay the
full cost.

**Cost.** Visible as: cursor moves slowly across a complex page,
each cross of a hoverable element pegs CPU for a second-plus. Looks
like the browser is "thinking" when actually it's recascading.

**Fix shape.** Two layers:
- **Debounce.** Coalesce hover events: queue the new hover_node and
  process at most once per ~100ms via the scheduler. If the cursor
  moves through 5 elements in 100ms (likely on a nav bar), only the
  final element triggers recascade.
- **Scope recascade to subtree.** `html_recascade_tree` re-cascades
  the entire DOM. A hover change on element X only affects X and
  potentially descendants where `:hover` selectors target a
  descendant of X. Real fix: walk just the affected subtree. This
  is closer to a CSS selector-matching design than a one-week
  feature.

The debounce alone is a 1-day fix and probably catches 80% of the
benefit. Add a scheduler-driven `hover_resolve_callback` that runs
~100ms after the last hover_node change.

**Estimated savings** while interacting with a loaded page: 1-3 s of
jank per cursor sweep across hoverable elements. Cold-load doesn't
benefit (cursor isn't typically over content while loading).

### 5. Resource governor caps + IMAGE refusal

**Evidence.** [macos9_http_fetcher.c:226-235](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L226):

```c
#define MAX_DOC_F     2
#define MAX_CSS_F     4
#define MAX_IMG_F     8
#define MAX_SCRIPT_F  4
#define MAX_FONT_F    2
#define MAX_OTHER_F   4
#define MAX_GLOBAL_F  16
```

HTTPS fetcher has its own pool of `MAX_HTTPS_F=32` slots
([macos9_https_fetcher.c:42](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L42))
but **the per-class governor in macos9_http_setup does not apply to
HTTPS fetches** — the HTTPS fetcher only checks `state == HS_IDLE`
when allocating slots ([macos9_https_fetcher.c:786-792](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L786)).
That's actually fine for HTTPS but it means the two fetcher paths
have different concurrency stories.

NetSurf core's `nsoption_int(max_fetchers_per_host)` is set to 16
and `max_fetchers` to 128 ([main.c:719-720](../../browser/netsurf/frontends/macos9/main.c#L719)). So the platform cap
is the bottleneck, not the core option. The HTTPS path can run up
to 32 in flight in principle.

**Critical sub-issue: IMAGE refusal returns NULL.**
[macos9_http_fetcher.c:1317-1327](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L1317):

```c
if (rc == MACOS9_RC_IMAGE) {
    if (global_active >= MAX_GLOBAL_F ||
        class_active >= MAX_IMG_F) {
        macos9_rgov_bump_skip(rc);
        return NULL;
    }
}
```

When NetSurf wants to start the 9th image fetch on an http:// page
while 8 are in flight, it gets NULL. NetSurf treats this as "broken
image" and **never retries**. On image-heavy pages this is a real
visual completeness issue, not just a speed issue. Note this only
hurts HTTP — HTTPS doesn't run through the governor.

**Cost.** Two effects on a mixed-scheme page:
- For HTTP, images past the 8-in-flight mark drop to placeholders.
  Page looks half-loaded.
- For HTTPS, the concurrency is fine but `nsoption(max_fetchers_per_host)=16`
  may not be high enough if a single origin serves 40 sub-resources
  (typical for CDN-fronted sites). Worth measuring.

**Fix shape.**
- Bump `MAX_IMG_F` to 12 or 16 after #1 lands and per-TLS memory
  pressure drops (no more cold-TLS state in flight, just reused
  endpoints).
- Replace `return NULL` on IMG cap with a proper deferral: NetSurf
  re-queues to `queue_ring`. The HTTP fetcher should hold the
  setup until a slot frees. That's a structural change to the
  setup contract; alternative is to bump caps to where they
  basically never bind.
- Audit `max_fetchers_per_host` against real-world CDN counts.
  At 16 it's high by HTTP/1.1 spec convention (rfc2616 says 2) but
  may still bind on sites that subresource-load from a single CDN
  host. Could go to 24 or 32 once TLS handshake cost is gone.

**Estimated savings** on cold load of an image-heavy page: 2-5 s,
plus visual completeness.

### 6. CSS recascade not cached across reformats

**Evidence.** `css_select_style` is called from
[box_construct.c:335](../../browser/netsurf/content/handlers/html/box_construct.c#L335) (once per element at box construction)
and from `html_recascade_tree` ([box_construct.c:2010](../../browser/netsurf/content/handlers/html/box_construct.c#L2010))
on dynamic style changes. **It is NOT called from `html_reformat`**
itself — layout reuses the cached `style` pointer hung off the box
struct ([layout.c](../../browser/netsurf/content/handlers/html/layout.c)).

So the news is good: a window resize / scroll / image arrival
triggers `html_reformat` → `layout_document`, which does NOT
re-cascade. Only structural DOM changes and `:hover` / `:focus` /
`:active` transitions cause recascade.

**This means the recascade pain is concentrated in two paths:**
- Hover (see #4)
- Stylesheet arrival mid-parse (each new `<link rel=stylesheet>`
  that completes fires `html_recascade_tree` to apply the new
  sheet's rules). On mactrove there are ~3-5 sheets; each costs a
  full recascade.

**Fix shape.** Stylesheet-driven recascade is unavoidable in V1 —
NetSurf's CSS pipeline is built around immutable computed-style
nodes that need full re-resolution when the selection context
changes. Mitigations:
- **Defer recascade until all `<link>` sheets have arrived.** Today
  each sheet triggers one. Could batch with a 50ms scheduler debounce
  so a flurry of sheet-arrivals only triggers one recascade. Saves
  the 1-3s × (N-1) sheets cost.
- **Walk only the changed subtree** for `:hover` (#4 above).

Stylesheet-arrival recascade savings on cold mactrove: 2-6 s.

### 7. Reformat thrashing during parse/fetch

**Evidence.** Sources of `browser_window_schedule_reformat` in the
Mac frontend and core HTML:

- [window.c:400](../../browser/netsurf/frontends/macos9/window.c#L400) — GW_EVENT_NEW_CONTENT (one per navigation, fine)
- [window.c:252](../../browser/netsurf/frontends/macos9/window.c#L252) — window resize (fine)
- [interaction.c:1387](../../browser/netsurf/content/handlers/html/interaction.c#L1387) — every hover change (see #4)
- Late stylesheet/image arrival inside NetSurf core (multiple fires per page)

CLAUDE.md mentions "~18 full reformats per mactrove load" in past
notes. The combined fire pattern is roughly:
- 1 initial reformat on NEW_CONTENT
- 1 per stylesheet arrival (~3-5)
- 1 per image-with-dimensions arrival (~10-20)
- 1 per recascade-triggering DOM mutation

Each `html_reformat` is gated by
`c->reformat_time = ms_after + 3 * ms_interval`
([html.c:1354-1361](../../browser/netsurf/content/handlers/html/html.c#L1354)) so back-to-back
reformats within the 3x interval window get coalesced by the core,
but the coalescing is per-content, not per-window — multiple
sub-resources arriving simultaneously can still each force one
reformat.

**Fix shape.** This is a `min_reflow_period` tuning issue plus a
NetSurf-core question. Cheapest win: bump `min_reflow_period` from
its default and let coalescing chew on more. Caveat: that delays
visible page completion. Better: a scheduler-driven coalescing pass
specific to the macos9 frontend.

**Estimated savings on cold load: 1-3 s** depending on how aggressively
reformats are currently firing. Needs instrumentation to confirm.

### 8. OT receive buffer + copy overhead

**Evidence.** HTTP fetcher: 32 KB `RECV_B` buffer per `OTRcv`
([macos9_http_fetcher.c:38-41](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L38)). HTTPS fetcher:
`READ_CHUNK=1024` ([macos9_https_fetcher.c:44](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L44)) for app-layer reads, and BearSSL's
internal record buffer (`BR_SSL_BUFSIZE_INPUT`, default 16 KB) for
the OT-layer reads in [pump_ot_recv_into_bearssl:710](../../macTLS/os9/ostls_async.c#L710).

BearSSL records can be up to ~16 KB; a 1 KB application read is
fine since the ring at [ostls_async.c:855-885](../../macTLS/os9/ostls_async.c#L855) buffers up to one full
record. **The 1 KB chunk size is not a bottleneck** — it just
controls how often `feed_body` runs. Each pass through the HTTPS
fetcher's poll, `OSTLS_Read` drains up to 1 KB into `rd[]`, copies
into the fetch_msg buffer, and dispatches `FETCH_DATA`. NetSurf core
copies again into its content-handler's internal buffer.

**Potential win.** Bumping `READ_CHUNK` to 8 KB cuts the per-poll
loop iterations by 8x. On a 50 KB page body, that's 6 polls instead
of 50. Each saved poll-pass avoids ~1 ms of plumbing overhead.
Modest: maybe 50-200ms per page.

**Bigger win, harder.** Eliminate the FETCH_DATA copy by having the
fetcher write directly into the content handler's buffer. NetSurf's
core architecture makes this hard; not worth pursuing in 0.6.x.

### 9. DNS resolution overhead

**Evidence.** [macos9_https_fetcher.c:592](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L592) and the HTTP fetcher both pass `host` to `OSTLS_New` / `OTInitDNSAddress`. OT's DNR (DNS resolver) does its own caching internally, but it's not warmed by anything we control.

**Cost.** First DNS lookup per host: 30-80ms. Subsequent: ~0-5ms
(OT cache). For a page with ~5 unique hostnames, that's ~150-400ms
total cold-DNS cost. Not a top-tier bottleneck but worth noting.

**Fix shape.** Pre-warm DNS for the document host as soon as the
URL hits `nsurl_create`, in parallel with parse-prep. Single
`OTInitDNSAddress` call from a scheduler callback at navigation
start. Modest savings unless OT's cache is unexpectedly weak.

### 10. QuickDraw paint cost (NOT a bottleneck on static pages)

**Evidence.** The fixes77f offscreen GWorld composite + walker
pruning is in place ([main.c:180-365](../../browser/netsurf/frontends/macos9/main.c#L180)). Per-redraw
counters show typical static-page redraws hitting < 60 boxes,
single-digit DrawText calls per scroll line ([plotters.c:34-35](../../browser/netsurf/frontends/macos9/plotters.c#L34) macos9_plot_text_count / macos9_plot_rect_count are
reset and dumped per redraw in [main.c:285-326](../../browser/netsurf/frontends/macos9/main.c#L285)).

For a static page after load completes, paint is cheap — a single
CopyBits at the end of `BeginUpdate/EndUpdate` and the per-glyph
DrawText calls scope to the dirty rect. **This isn't where the time
goes.**

The exceptions:
- Animation tick (continuous redraws). Per fixes77g this is bound
  by the per-tick CopyBits + the walker-pruned box visits, ~5 boxes
  per tick. Adequate for a single decorative animation.
- Initial post-load full repaint. This is one big CopyBits of the
  whole viewport — fine.

**No action.** Mention as confirmation that the speed problem is
network-and-parse, not paint.

### 11. DOM/box-tree allocation churn (libdom talloc)

**Evidence.** libdom uses Samba's talloc allocator. CLAUDE.md
mentions talloc was the "most likely bottleneck" for HTML handler
landing but it did compile cleanly. Allocation patterns during parse
are unknown without instrumentation.

**Cost.** Unmeasured. On a 200 KB HTML parsing into ~1000 DOM
nodes, talloc does roughly 2000-5000 malloc calls. CW8 MSL malloc
is reasonably fast on PPC but not Apple-DLMalloc-tuned.

**Fix shape.** Profile first. Replace MSL malloc with dlmalloc only
if profiling implicates it. Not a one-week fix.

**Status: needs instrumentation** (TickCount-bracketed allocator
counters during one cold mactrove parse).

### 12. Image decode cost

**Evidence.** [macos9_image.c:608-620](../../browser/netsurf/frontends/macos9/macos9_image.c#L608) `lodepng_decode32` is the PNG path. Pre-decode at convert
time, not redraw time (correct per the
project_netsurf_content_handler_dispatch memory). One decode pass
per image.

**Cost.** lodepng on a 200 KB PNG on 233 MHz G3: typically 200-500ms.
QT Graphics Importers for JPEG/GIF/BMP/TIFF: typically 100-300ms per
image.

Image decode runs serially with everything else on the cooperative
loop. ~5-10 images per page × 200-300ms each = 1-3 s. Already
counted in baseline.

**Fix shape.** Lower-priority deferred load for decorative images.
Today every image fires immediately on FETCH_FINISHED. A
"prioritize-by-viewport" pass would queue offscreen images for
post-paint decode. Real win but real engineering. Out of scope for
0.6.x.

### 13. Layout O(n²) hotspots

**Evidence.** None directly observed in code structure. NetSurf's
layout is mostly O(n) over the box tree. Tables and nested flexbox
can have quadratic behavior if minmax pass and final pass each walk
descendants.

**Status: needs instrumentation.** Per-page layout time is logged in
[html.c:1252](../../browser/netsurf/content/handlers/html/html.c#L1252) (`nsu_getmonotonic_ms`); a real measurement of layout_document time would say whether this is a bottleneck.

## Peer-port comparison

Where MacSurf differs from the closest analogs, and what's worth
borrowing.

### RISC OS

- **Idle-driven event loop.** [riscos/gui.c:1874-1890](../../browser/netsurf/frontends/riscos/gui.c#L1874): `wimp_poll_idle` sleeps until the next scheduled deadline. MacSurf hardcodes `1`. **Borrow.**
- **Backing store + disc cache disabled by default** ([gui.c:332](../../browser/netsurf/frontends/riscos/gui.c#L332): `nsoption_set_uint(disc_cache_size, 0)`). RISC OS relies on the in-memory llcache. Modern retro setups (SSD) favor a disc cache; MacSurf's custom on-disk cache (`macos9_disk_cache.c`) is the right call. No change needed.
- **fetcher_poll re-arms via scheduler.** Same model as MacSurf's `fetch_pump` but RISC OS doesn't have the "pump unconditionally" path; it relies on the scheduler chain. **MacSurf's fetch_pump-every-loop is more conservative; OK to keep.**
- **No connection pooling specific to RISC OS** — it uses libcurl, which pools its own.

### Atari

- **Scheduler-derived poll timeout.** [atari/gui.c:120](../../browser/netsurf/frontends/atari/gui.c#L120): `aes_event_in.emi_tlow = schedule_run()`. **Same pattern as RISC OS. Borrow.**
- **Same libcurl backend.** Connection pooling free.
- **Redraw queue: explicit areas_used buffer** ([gui.c:131](../../browser/netsurf/frontends/atari/gui.c#L131)). MacSurf invalidates via Toolbox `InvalWindowRect`; the Toolbox does its own coalescing. Probably equivalent.

### Amiga

- **Signal-driven wait** ([amiga/gui.c:3588](../../browser/netsurf/frontends/amiga/gui.c#L3588): `signal = Wait(signalmask)`). Truly event-driven — wakes only on actual signal, not polling. MacSurf can't do this cleanly because `WaitNextEvent` is the only Toolbox primitive that knows about UI events; we'd need an OT notifier that signals via the OS 9 Notification Manager, which adds complexity.
- **Scheduler-driven redraw** ([gui.c:2316](../../browser/netsurf/frontends/amiga/gui.c#L2316)): `ami_schedule_redraw` queues a callback that performs the actual paint. Could be useful for batching multiple invalidates into one.

### Framebuffer (libnsfb)

- **Used as the testing path on Linux.** Not directly relevant for OS 9 architecture choices.

### Key takeaway

MacSurf's HTTP fetcher already implements connection pooling, which
neither RISC OS nor Atari needs (libcurl). **MacSurf's HTTPS fetcher
is the outlier — it should mirror its sibling HTTP fetcher's pool
discipline.** That's bottleneck #1 above.

The "scheduler-driven sleep timeout" pattern is unanimous across the
two cooperative-multitasking ports (RISC OS, Atari). MacSurf is the
exception by hard-coding `1`. Bottleneck #3.

## OS 9 era context

How did the period browsers handle this on the same class of
hardware?

**Classilla / WaMCom (Mozilla 1.3-era, Carbon CFM).**
- Async sockets via NSPR over OT (`macsockotpt.c`). True non-blocking
  fetch with notifier callbacks driving the dispatch — no synchronous
  OTRcv loop.
- HTTP keep-alive built into Mozilla netwerk. Connection pool per
  origin.
- HTTPS via NSS (full TLS stack, large). Slower per-handshake than
  BearSSL but pooled.
- Cooperative scheduling via the WaitNextEvent loop, like MacSurf.
- **Cold mactrove on G3**: typically 8-15 s back in the day. Faster
  than MacSurf today because of keep-alive, slower per-CPU-step
  because NSS was heavier than BearSSL.

**iCab 2.x/3.x.**
- Closed source. The "iCab is fast" reputation came from:
  - Aggressive HTTP pipelining (multiple requests on one connection without waiting for the response).
  - Connection pooling.
  - Custom layout engine designed for OS 9 cooperative multitasking from day one (not a port).
- Best-of-period cold-load on G3 ethernet was ~5-8 s for a
  representative page.

**Internet Explorer 5 Mac (Tasman).**
- Pipelined HTTP/1.1 but not aggressive about it.
- Slow rendering engine — text layout was famously slow.
- Cold-load on G3: 15-25 s typical. The "IE5 is slow" reputation
  was mostly the layout engine, not the network stack.

**Netscape Communicator 4.7.**
- HTTP/1.0-era. No keep-alive in early versions.
- Cold-load on G3: 30-60 s for a complex page.
- This is roughly where MacSurf sits today.

### Frame

In 1999-2003, modem users loaded pages in 30-60 s, ethernet users
3-10 s. Today's MacSurf cold-loads land in the **20-40 s** band on
ethernet — solidly in the "Communicator 4.7" reputation zone but for
the wrong reason (it's not HTTP/1.0, it's per-fetch TLS).

**Target after fixes98-107 + keep-alive (#1) + cache (#2):**
4-8 s cold, sub-1 s warm. That's iCab territory.

## Recommendations (priority order)

A staged plan for v0.6.x → v0.7.

### Stage A — Keep-alive landing (1 week)

1. **Build a macTLS connection-reuse primitive.** Either:
   - `OSTLS_Reset(OSTLSConnection *conn)` that re-readies the
     connection for a new request without TLS handshake (preserves
     BearSSL session state), OR
   - Keep the OSTLSConnection alive and write a second HTTP request
     into its `sendapp` directly.
   The second is much simpler.

2. **Build the HTTPS endpoint pool.** Copy the
   `ep_pool_take`/`ep_pool_return` pattern from
   `macos9_http_fetcher.c`. Key on host:port. Pool size 8-16.

3. **Flip request line.** `Connection: keep-alive` in the GET.
   Parse `Connection: close` response header to gate pool return.

4. **Hardware-verify on mactrove + Apple + one other site.** Expect
   ~5x cold-load improvement. Watch for benign-close behaviors from
   CF / Google CDN (already worked around in retry path; pool needs
   the same).

**Estimated saving on cold mactrove: 20-25 s.**
**Effort: 1 week.**

### Stage B — Cache HIT re-enable (2-3 days)

1. Read the HTTP fetcher's working cache-hit pump
   ([mfs_poll_one:896-925](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L896))
   and the HTTPS fetcher's disabled HS_CACHEHIT pump
   ([hctx_poll:540-587](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L540)).

2. Diff them. Find the difference that made HTTPS fail with
   about:query/fetcherror. Likely candidates:
   - Missing `fetch_set_http_code` before FETCH_FINISHED
   - Wrong order of synthetic FETCH_HEADER lines
   - Trailing CRLF on status line

3. Add a `macos9_cache_lookup` call to `macos9_https_setup` between
   slot allocation and HS_QUEUED.

4. Add Reload (Cmd-R) → set `macsurf_http_skip_next_cache=1` semantics
   to HTTPS as well as HTTP. Already supported by the cache layer.

5. Add cache TTL (28 days default, mirror NetSurf's `disc_cache_age`).

**Estimated saving on warm reload: 25-30 s.**
**Effort: 2-3 days.**

### Stage C — Event-loop pacing (1 day)

1. Replace the `WaitNextEvent(MASK, &ev, 1, NULL)` literal with a
   dynamic sleep:
   ```
   short sleep_ticks = 1;
   if (no fetcher active)
       sleep_ticks = macos9_get_next_delay();
   if (sleep_ticks > 15) sleep_ticks = 15;
   ```
2. Verify that scroll-bar tracking, URL field caret blink, and TE
   idle still fire smoothly. Caret blink is ~30 ticks; safe.

**Estimated saving on cold load: ~1 s.**
**Estimated saving for idle UI: ~50% CPU drop.**
**Effort: 1 day.**

### Stage D — Hover debounce (2 days)

1. Add a scheduler-driven `hover_resolve_callback(void *p)` that
   reads the latest queued hover node and fires the real
   `browser_window_mouse_track(HOVER)` call.
2. In `macos9_poll_mouse_hover`, queue the new position via
   `macos9_schedule(100, hover_resolve_callback, gw)` — schedule
   already dedupes per (callback, p) pair, so successive moves
   reset the timer.

**Estimated saving on interactive UX: 1-3 s of jank per hover sweep.**
**Effort: 2 days.**

### Stage E — Image caps + governor tune (3-4 days)

1. After Stage A lands, bump `MAX_IMG_F` from 8 to 12 or 16.
2. Replace HTTP fetcher's `return NULL` on IMG cap with a proper
   deferral: queue the setup until a slot frees (return a sentinel
   that NetSurf's queue_ring re-tries, mirroring stub fetcher
   queued-deferred patterns).
3. Audit `max_fetchers_per_host=16` against real CDN counts;
   probably push to 24.

**Estimated saving: 2-5 s on image-heavy cold loads.**
**Effort: 3-4 days.**

### Stage F — Stylesheet-batched recascade (1 week)

1. Add a scheduler-driven `recascade_callback` debouncer in the
   HTML content handler.
2. Instead of calling `html_recascade_tree` immediately on every
   sheet arrival, queue with 50-100ms debounce.
3. Verify on mactrove (3-5 sheets typical) and on apple.com (8-12
   sheets).

**Estimated saving: 2-6 s on cold mactrove.**
**Effort: 1 week (touches core; needs care).**

### Stage G — Bigger reads (half day)

1. Bump `READ_CHUNK` in `macos9_https_fetcher.c` from 1024 to 4096
   or 8192. BearSSL records are bounded by `BR_SSL_BUFSIZE_INPUT`
   anyway, so a bigger app-read just means fewer poll-loop passes.

**Estimated saving: 50-200ms per page.**
**Effort: half day, mostly testing.**

### Summary timeline

| Stage | Saves (cold) | Saves (warm) | Effort |
|---|---|---|---|
| A — Keep-alive | 20-25 s | n/a | 1 week |
| B — Cache HIT | 5-15 s (shared assets) | 25-30 s | 2-3 days |
| C — Sleep tick | ~1 s | ~1 s | 1 day |
| D — Hover debounce | 0 (cold) | UX jank fix | 2 days |
| E — Image caps | 2-5 s | n/a | 3-4 days |
| F — Sheet batching | 2-6 s | 1-2 s | 1 week |
| G — Bigger reads | 50-200ms | 50-200ms | half day |

A + B + C alone (about 2 weeks of work) take mactrove cold-load from
~25 s to **~5-8 s**, and warm reload to **< 2 s**. This is the
focused speed sprint.

## Out of scope for this analysis

- **DOM/talloc allocator behavior.** Needs profiling on hardware. Not
  approachable from static read.
- **Layout O(n²) hotspots in table/flex.** Same.
- **libdom parse throughput.** Same.
- **Image decode parallelism / deferred decode.** Real engineering
  project, not 0.6.x.
- **HTTP pipelining** (multiple in-flight requests per connection).
  Considered for v0.7+; not landing in 0.6.x. The keep-alive win is
  enough.
- **OT notifier-driven event loop** (true event-driven, no polling).
  v0.7+ project, requires Notification Manager integration.
- **NSS-level TLS session resumption.** BearSSL supports it
  (`br_ssl_client_get_session_parameters`); MacSurf doesn't exercise
  it. The keep-alive pool (#1) gets 95% of the benefit without
  touching this. Visit if cold-cross-page TLS becomes the next
  bottleneck.
- **HTTP/2 / QUIC.** No path on OS 9. Not relevant.
- **TLS 1.3.** BearSSL is 1.2 only. Not relevant.
- **Brotli / Zstd content encoding.** BearSSL doesn't ship them;
  adding them is its own port. Plain HTTP gzip already isn't enabled
  (we send `Accept-Encoding: identity` — see
  [macos9_https_fetcher.c:674](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L674)).
  Adding gzip would cut body bytes 5-10x for HTML/CSS/JSON, but
  decode on a 233 MHz G3 is meaningful CPU cost. Worth measuring as
  a separate sprint after Stage A/B.

---

## Appendix: file-line reference index

Key code sites cited above, for ease of navigation:

- HTTPS fetcher slot pool: [macos9_https_fetcher.c:42-58](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L42)
- HTTPS no-pool comment: [macos9_https_fetcher.c:12](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L12)
- HTTPS hctx_clear (disposes connection): [macos9_https_fetcher.c:132-149](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L132)
- HTTPS `Connection: close` hardcode: [macos9_https_fetcher.c:675](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L675)
- HTTPS HS_CACHEHIT branch (unreachable): [macos9_https_fetcher.c:540-587](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L540)
- HTTPS cache lookup disabled: [macos9_https_fetcher.c:855-861](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L855)
- HTTPS retry path (fixes228): [macos9_https_fetcher.c:619-660](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L619)
- HTTP endpoint pool: [macos9_http_fetcher.c:252-332](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L252)
- HTTP RECV_B = 32 KB: [macos9_http_fetcher.c:38-41](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L38)
- HTTP cache-hit working path: [macos9_http_fetcher.c:896-925](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L896)
- HTTP cache lookup at setup: [macos9_http_fetcher.c:392-417](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L392)
- HTTP resource governor caps: [macos9_http_fetcher.c:226-235](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L226)
- HTTP IMAGE refusal: [macos9_http_fetcher.c:1317-1327](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L1317)
- HTTP keep-alive Connection: close detection: [macos9_http_fetcher.c:783-825](../../browser/netsurf/frontends/macos9/macos9_http_fetcher.c#L783)
- main.c hardcoded WNE sleep=1: [main.c:561](../../browser/netsurf/frontends/macos9/main.c#L561)
- main.c hover poll: [main.c:595-615](../../browser/netsurf/frontends/macos9/main.c#L595)
- main.c fetch_pump unconditional: [main.c:579](../../browser/netsurf/frontends/macos9/main.c#L579)
- main.c nsoption_set memory cache 32MB: [main.c:732](../../browser/netsurf/frontends/macos9/main.c#L732)
- main.c max_fetchers=128, per_host=16: [main.c:719-720](../../browser/netsurf/frontends/macos9/main.c#L719)
- schedule.c idle sleep default 15: [schedule.c:28](../../browser/netsurf/frontends/macos9/schedule.c#L28)
- schedule.c macos9_get_next_delay: [schedule.c:175-193](../../browser/netsurf/frontends/macos9/schedule.c#L175)
- disk cache STORE eligible MIME list: [macos9_disk_cache.c:152-163](../../browser/netsurf/frontends/macos9/macos9_disk_cache.c#L152)
- disk cache MAX_BYTES: macos9_disk_cache.h (MACSURF_CACHE_MAX_BYTES)
- macTLS OSTLS_Pump: [ostls_async.c:1036-1117](../../macTLS/os9/ostls_async.c#L1036)
- macTLS pump 8-step caller: [macos9_https_fetcher.c:613](../../browser/netsurf/frontends/macos9/macos9_https_fetcher.c#L613)
- macTLS recv into BearSSL (no extra copy): [ostls_async.c:689-728](../../macTLS/os9/ostls_async.c#L689)
- NetSurf core fetch_pump: [fetch.c:290-299](../../browser/netsurf/content/fetch.c#L290)
- NetSurf core fetch_dispatch_jobs: [fetch.c:231-260](../../browser/netsurf/content/fetch.c#L231)
- html_recascade_tree: [box_construct.c:2010-2150](../../browser/netsurf/content/handlers/html/box_construct.c#L2010)
- hover-triggered recascade: [interaction.c:1383-1388](../../browser/netsurf/content/handlers/html/interaction.c#L1383)
- html_reformat: [html.c:1236-1364](../../browser/netsurf/content/handlers/html/html.c#L1236)
- nscss_get_style (CSS selection): [select.c:259-310](../../browser/netsurf/content/handlers/css/select.c#L259)
- box_construct_element calls nscss_get_style: [box_construct.c:335](../../browser/netsurf/content/handlers/html/box_construct.c#L335)
- RISC OS scheduler-driven poll timeout: [riscos/gui.c:1864-1890](../../browser/netsurf/frontends/riscos/gui.c#L1864)
- atari scheduler-driven poll timeout: [atari/gui.c:120](../../browser/netsurf/frontends/atari/gui.c#L120)
- amiga signal-driven wait: [amiga/gui.c:3555-3608](../../browser/netsurf/frontends/amiga/gui.c#L3555)
