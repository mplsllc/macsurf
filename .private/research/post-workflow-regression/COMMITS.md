# Regression-Test Commit Matrix

Post-workflow performance regression investigation.
Created: 2026-09-08

---

## A — WORKFLOW BASELINE

**SHA:** `b318c3c0ce88cd0dfca4c72b5ff6c94d6713e7ff`
**Title:** Merge branch 'workflow'
**Branch point:** This is the last stable pre-performance commit.
**Position:** 0 commits into the optimization campaign.

Hardware status: NOT TESTED

---

## B — EARLY OPTIMIZATION SNAPSHOT

**SHA:** `fccd036ac6035df3654154dd8757f3cc5b2fde24`
**Title:** `perf(layout): retire unused depth accounting`
**Position:** 11 commits after A (2026-09-03)

This is the tip of the first optimization block:
disk-cache memoization, scheduler queue-walk collapse,
fetch host-policy cache, gzip inline, layout watchdog cleanup.

Hardware status: NOT TESTED

---

## C — PERFORMANCE TIP

**SHA:** `4ef8bc71d4c29b87695b6baf26954025fe4554ba`
**Title:** `fix(time): avoid TickCount conversion overflow`
**Position:** 131 commits after A (120 commits after B)

Contains all automated `apply_perf_*.py` patches through the tip of
the performance branch before the stabilization merge.

**Known contained regression:**
`7314f6493` — `perf(flex): embed first line in layout context`
accidentally deleted `layout_flex__order_items(ctx)` and
`layout_flex__collect_items_into_lines(ctx)`.
Repaired at `1c548c5ef`. That repair IS included in C since C is after it.
A midpoint bisect between B and C that lands before `1c548c5ef` may show
a DIFFERENT rendering regression (broken flex layout), unrelated to the
current scrolling/crash symptom.

Hardware status: NOT TESTED

---

## D — CURRENT STABILIZATION (RECONVERT ON)

**SHA:** `972994c2d1203ccfd8fe26651440777165e540de`
**Title:** `diag(reconvert): add MACSURF_RECONVERT_DISABLED compile-time A/B gate`

Contains the compile-time gate but does NOT activate it.
This is identical to current production behavior on the stabilization branch.

Hardware status: NOT TESTED

---

## E — CURRENT STABILIZATION (RECONVERT OFF)

**SHA:** `8c3bb9118876ea955d926f633364fa4d835b0327`
**Title:** `diag(reconvert): activate CONTROL_RECONVERT_OFF build (in-file define)`

Differs from D by one line:
```c
#define MACSURF_RECONVERT_DISABLED
```
in macos9_reconvert.c.

JS-driven reconvert (mark_dom_dirty → html_reconvert_content) is
suppressed. Initial document conversion is untouched.
JS, timers, events, networking, navigation: fully operational.

Hardware status: NOT TESTED (file dropped to Mac 2026-09-08T20:55 CDT)

---

## Interval sizes

| Interval | Commits |
|----------|---------|
| A → B    | 11      |
| B → C    | 120     |
| C → D    | 50      |
| D → E    | 1       |

---

## A → B commit list (in chronological order, oldest first)

```
9f583e6b4  perf: remove disk-cache hot-path scans
260c5f906  perf: streamline cache file access and checkpoint state
23896e3c3  chore: restore cache source commentary
11f5bbe0b  docs: align disk-cache cap commentary
7186439fe  perf: collapse scheduler queue walks
fdb8ed27b  build: fix Retro68 lint exit handling
b8ecf092d  build: harden Retro68 strict C89 gate
dbf74b51b  perf: cut repeated fetch string work
b7bca9f7d  perf(gzip): inline LZ match output loop
9eae6d603  perf(layout): remove hot-path progress modulo
fccd036ac  perf(layout): retire unused depth accounting
```

High-risk commits for scrolling/lifetime/scheduler:
- `7186439fe` — scheduler queue-walk collapse (removed sched_guard / sched_queue_is_cyclic / sched_remove as a separate function; merged into combined walk)
- `dbf74b51b` — fetch host-policy cache (new per-process cached global state)
- `9f583e6b4` — disk-cache directory memoization (new process-lifetime static state)
- `260c5f906` — second disk-cache rewrite (FSSpec bypass, comment deletions)

---

## Result table (to be filled by maintainer after hardware runs)

| Build | SHA (short) | HN Scroll | HN Crash | 68kMLA Scroll | 68kMLA Crash | Hackaday Scroll | Hackaday Crash |
|-------|-------------|-----------|----------|---------------|--------------|-----------------|----------------|
| A     | b318c3c0    | —         | —        | —             | —            | —               | —              |
| B     | fccd036a    | —         | —        | —             | —            | —               | —              |
| C     | 4ef8bc71    | —         | —        | —             | —            | —               | —              |
| D     | 972994c2    | —         | —        | —             | —            | —               | —              |
| E     | 8c3bb911    | —         | —        | —             | —            | —               | —              |
