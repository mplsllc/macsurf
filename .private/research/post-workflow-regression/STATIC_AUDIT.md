# Static Audit — Post-Workflow Performance Commits

Range: `b318c3c0..4ef8bc71d`
Audited: 2026-09-08

**This audit is NOT a substitute for hardware.**
It identifies *candidate* regressions based on static code inspection only.
Hardware test classification is authoritative. Static findings must be labeled
HARDWARE STATUS: NOT TESTED until a hardware result is recorded.

---

## AUDIT-01

SHA: `9f583e6b421131e14d8ec3d69ce78dd1d1fa10cb`
TITLE: perf: remove disk-cache hot-path scans
AUTHOR: Patrick Britton / OpenAI Codex
DATE: 2026-09-02

INTENDED OPTIMIZATION:
Memoize HFS directory IDs (vRefNum + dirID) for MacSurfData and
MacSurfData/Cache so repeated PBGetCatInfoSync calls are skipped after
the first lookup.

ACTUAL SEMANTIC SURFACE:
- Adds two process-lifetime static globals: `g_data_dir_vref`,
  `g_data_dir_id`, `g_cache_dir_vref`, `g_cache_dir_id`, all
  initialized to zero.
- First call populates them; subsequent calls return cached values
  without re-validating with the OS.
- Also refactors cache_total tracking into a new `cache_total_replace()`
  helper and adds `cache_total_reset()` — these replace the inline
  `g_cache_total` budget check that used to call `macos9_cache_sweep()`
  when the budget was exceeded.
- Removes diagnostic comment blocks and store-timing explanation comments.
- Removes fixes-tagged comment explaining per-host POST bypass window.
- Removes fixes-tagged comment explaining `fixes981` backward-compat
  header reading.
- Removes fixes-tagged comment explaining `fixes984` LIFE prefix on CACHE HIT.

RISK: LOW-MEDIUM for the specific scrolling/crash symptom.
Directory ID memoization is safe as long as the app doesn't move its
own folder while running (which it cannot). The cache_total accounting
change is the higher-risk surface: if cache_total_replace() has a sign
error or fails to call macos9_cache_sweep() when it should, the budget
enforcement breaks silently — disk cache grows without bound and old
entries that should have been evicted remain, or entries are evicted
prematurely and the cache becomes useless (extra network fetches, slower
loads). Neither directly causes a crash in the scrolling/lifetime family.

DELETED DIAGNOSTIC VALUE: Medium. The removed `fixes981`, `fixes984`,
`fixes649` comments were explanatory; their removal doesn't delete
functioning code, only explanation. However the removed
`/* fixes985 - total-size budget enforcement */` block commentary WAS
adjacent to real budget-enforcement logic that was also refactored.

PROVEN REGRESSION: NO
HARDWARE STATUS: NOT TESTED

---

## AUDIT-02

SHA: `260c5f906699ae636d4375afdf7da9c8b1a3ec20`
TITLE: perf: streamline cache file access and checkpoint state
AUTHOR: Patrick Britton / OpenAI Codex
DATE: 2026-09-03

INTENDED OPTIMIZATION:
Bypass FSMakeFSSpec catalog lookup for cache hot-path by constructing
FSSpec structs directly from known vRefNum + dirID + Pascal leaf name.

ACTUAL SEMANTIC SURFACE:
- Adds `macos9_spec_from_pname()` which builds an FSSpec directly
  without calling `FSMakeFSSpec`. **This skips a catalog lookup that
  historically served as a validation step.** If the memoized dir IDs
  from AUDIT-01 are stale or wrong, `macos9_spec_from_pname()` builds
  a spec that HFS will reject silently or, worse, misroute.
- 540-line file diff with 278 lines removed — the largest diff in the
  A→B interval. Large removal count relative to the stated optimization.
- Continued comment/diagnostic deletion from AUDIT-01 baseline.
- Does not obviously delete any control-flow guard or error path that
  was previously load-bearing.

RISK: MEDIUM. FSSpec construction bypass is sound IF the memoized values
are valid; it is silent misrouting if they are wrong. Combined with
AUDIT-01's memoization, the risk is additive: both must be correct for
the cache hot-path to work.

PROVEN REGRESSION: NO
HARDWARE STATUS: NOT TESTED

---

## AUDIT-03

SHA: `7186439fef1730e2e1bda86a799109c0939be82d`
TITLE: perf: collapse scheduler queue walks
AUTHOR: Patrick Britton / OpenAI Codex
DATE: 2026-09-03

INTENDED OPTIMIZATION:
Eliminate a separate O(n) cycle-detection pass (`sched_queue_is_cyclic` /
`sched_guard`) that was called before every queue operation, by folding
the walk-count guard into each combined search+insert/remove operation.

ACTUAL SEMANTIC SURFACE:
- **Deletes `sched_queue_is_cyclic()`** — the function that detected
  a corrupted (cyclic) queue and triggered `sched_hard_reset()`.
- **Deletes `sched_guard()`** — the call site that invoked the above
  before every walk.
- **Deletes `sched_remove()`** as a standalone function — its logic is
  now inlined into the combined search+remove walks.
- The replacement claims to inline the walk-count guard into each
  combined walk. If this is correct, the cycle protection is preserved;
  if the combined walk misses a cycle case the scheduler can infinite-loop.
- The commit message does not prove the MACOS9_SCHED_WALK_MAX guard is
  correctly applied to every walk path, only that the intent was to do so.

RISK: **HIGH** for the specific scrolling/stability symptom.
The original `sched_guard()` was added specifically for a hardware-
observed freeze ("`sched_queue_is_cyclic` ... the cooperative event loop
hard-freezes with no crash" — the fixes584 comment, which was deleted).
Removing the separate guard adds scheduling risk in any UAF scenario.
UAF in content/lifetime is *exactly* the crash family under investigation.
If a freed pointer ends up in the scheduler queue's `->next` chain, the
guard no longer fires a `sched_hard_reset()` — the behavior depends on
whether the inline walk-count guard catches it first.

DELETED DIAGNOSTIC VALUE: HIGH — the fixes584 comment explaining WHY the
guard existed was deleted. Future debugging of a scheduler freeze will
not find that context.

PROVEN REGRESSION: NO
HARDWARE STATUS: NOT TESTED
PRIORITY: HIGH — test A vs B specifically looking for scheduler freeze
or hang behavior in addition to crash/scroll symptoms.

---

## AUDIT-04

SHA: `dbf74b51b4e837fd3f2e307435979ac7abdd11c3`
TITLE: perf: cut repeated fetch string work
AUTHOR: Patrick Britton / OpenAI Codex
DATE: 2026-09-03

INTENDED OPTIMIZATION:
Cache per-host UA selection and tracker-block decisions in a 16-slot
direct-mapped cache to avoid repeated suffix-match scans.

ACTUAL SEMANTIC SURFACE:
- Adds `macos9_host_policy_cache[16]` as a static global.
- Cache entries hold: hash, host_len, host[], ua, ua_valid, tracker,
  tracker_valid.
- Cache invalidation: only on hash+length+case-insensitive-equality
  mismatch. **There is no invalidation on UA table changes, config
  changes, or navigation.** If the UA table is ever modified at runtime
  (it isn't today, but the cache doesn't know that), stale UA answers
  persist for the life of the process.
- This is a direct-mapped cache (only one slot per hash bucket), so
  two different hosts that hash to the same slot evict each other —
  correct but worth noting.

RISK: LOW for the crash/scroll symptom. Per-process UA/tracker cache is
benign for a static UA table. The cache does not touch content lifetime,
scheduling, or the DOM.

PROVEN REGRESSION: NO
HARDWARE STATUS: NOT TESTED

---

## AUDIT-05

SHA: `b7bca9f7dab9d0c4e494fed36f569bc555c61108`
TITLE: perf(gzip): inline LZ match output loop
AUTHOR: Patrick Britton / OpenAI Codex
DATE: 2026-09-03

INTENDED OPTIMIZATION:
Inline `gz_out()` call inside DEFLATE match-copy loop to eliminate
per-byte function call overhead.

ACTUAL SEMANTIC SURFACE:
- Inlines the read-before-write, CRC update, total_out increment,
  cap check, and window-wrap flush into the match-copy loop body.
- The commit message states the ordering is "deliberately identical to
  gz_out()".
- The CRC table expression is reproduced inline:
  `gz_crc_table[(z->crc ^ b) & 0xFFUL] ^ (z->crc >> 8)`
  This matches the standard CRC-32 step. Correct if gz_crc_table
  is the standard table.
- Introduces a block-scoped `unsigned char b` declaration inside a
  loop body. In C89 this is a statement-before-declaration violation
  ONLY if it appears after any statement in the block. The diff shows
  it's the first declaration in the loop body — likely clean, but must
  be verified by CW8 build.

RISK: LOW-MEDIUM. If the inline ordering diverges from `gz_out()` in
any edge case (overlap read, cap enforcement order), compressed content
will be silently corrupted. The scroll/crash symptom would not obviously
trace to gzip, but corrupted CSS/JS could produce bad DOM or script
errors that cascade.

PROVEN REGRESSION: NO
HARDWARE STATUS: NOT TESTED

---

## AUDIT-06

SHA: `9eae6d6032b819532628084a80812d0e9c33e019`
TITLE: perf(layout): remove hot-path progress modulo
AUTHOR: Patrick Britton / OpenAI Codex
DATE: 2026-09-03

INTENDED OPTIMIZATION:
Remove a `(macsurf_layout_calls % 100000L) == 0` modulo + log call
from `layout_watchdog_enter()` on every recursive layout entry.

ACTUAL SEMANTIC SURFACE:
- Removes the per-100k WORK log line from the hot layout path.
- Does not change any layout decision or watchdog limit.

RISK: LOW. Pure diagnostic removal. Reduces observability in a hang
scenario (the WORK log progress line was the tool for distinguishing
"slow forward progress" from "stuck loop"), but does not change behavior.

DELETED DIAGNOSTIC VALUE: MEDIUM-HIGH — the comment explaining the
diagnostic's purpose (distinguishing slow-but-correct from stuck) was
deleted. If a layout hang occurs in a historical build between this
commit and fccd036ac, the log will not have the 100k progress marker.

PROVEN REGRESSION: NO
HARDWARE STATUS: NOT TESTED

---

## AUDIT-07

SHA: `fccd036ac6035df3654154dd8757f3cc5b2fde24`
TITLE: perf(layout): retire unused depth accounting
AUTHOR: Patrick Britton / OpenAI Codex
DATE: 2026-09-03

INTENDED OPTIMIZATION:
Remove the `macsurf_layout_depth++` / `macsurf_layout_depth--` pair
from every recursive layout entry/exit since the depth value no longer
drives any layout decision.

ACTUAL SEMANTIC SURFACE:
- Removes depth increment/decrement from `layout_watchdog_enter` /
  `layout_watchdog_exit`.
- Removes the depth cap check from `layout_watchdog_enter`.
- The return value is always 0 (never bail out).
- This is correct IF depth was genuinely not used anywhere. The comment
  confirms "no active depth cap."

RISK: LOW for the crash/scroll symptom. This is counter bookkeeping only.

PROVEN REGRESSION: NO
HARDWARE STATUS: NOT TESTED

---

## AUDIT-08 ⚠️ PROVEN REGRESSION

SHA: `7314f6493c773346e3322d83e1da7c88ca2ce40a`
TITLE: perf(flex): embed first line in layout context
AUTHOR: OpenAI Codex (automated apply_perf_flex_alloc_once.py)
DATE: 2026-09-03

INTENDED OPTIMIZATION:
Embed the first `flex_line_data` in `flex_ctx` to avoid one malloc/free
for single-line flex containers.

ACTUAL SEMANTIC SURFACE:
- Accidentally deleted `layout_flex__order_items(ctx)` and
  `layout_flex__collect_items_into_lines(ctx)` calls and their
  fallback/error path.
- Flex layout would silently produce unordered, uncollected items —
  all flex containers would render incorrectly.
- Repair commit: `1c548c5ef3be8e7ecdc79d3faf3b2f56b81f69a3`
  `fix(flex): restore item collection after performance pass`

RISK: CATASTROPHIC for any page with flex layout.

PROVEN REGRESSION: **YES**
HARDWARE STATUS: NOT TESTED AT THIS EXACT SHA (repair is included in C)

NOTE FOR BISECT: Any midpoint between B and C that lands between
`7314f6493` and `1c548c5ef` will exhibit broken flex layout. This is a
DIFFERENT regression from the scrolling/crash symptom under investigation.
Use `SKIP/MASKED:FLEX_REGRESSION` for such midpoints.

---

## AUDIT-09 ⚠️ RENDER SEMANTICS CHANGE

SHA: `b71f1b8eb939f12c2a2b697fa1fc61f7d7582eaf`
TITLE: perf(js): coalesce sync barriers per execution
AUTHOR: MacSurf Perf Bot (automated apply_perf_sync_coalesce_once.py)
DATE: 2026-09-04

INTENDED OPTIMIZATION:
After a geometry flush declines (transient blocker), latch a per-JS-
execution flag so subsequent geometry reads in the same callback don't
hammer the same guards.

ACTUAL SEMANTIC SURFACE:
- **This is not a pure performance change. It changes geometry-flush
  semantics.**
- Adds `g_geom_attempted` / `g_geom_attempted_c` static globals.
- After the first declined flush for content `c`, ALL subsequent
  geometry reads in the same JS execution for `c` return `undefined`
  without retrying the flush.
- Before this commit, each geometry read retried the flush independently.
- If the first decline is a false negative (a guard that clears between
  two getters in the same synchronous callback), this commit silences
  a flush that would previously have succeeded.
- Also described as: "Reuse proven style/inherited-color transactions in
  forced geometry before full reconvert" — adds a new code path that
  tries fast style/inherited-color reconvert BEFORE the full reconvert
  in the forced-flush path. This is a new execution path that was not
  in the baseline.

RISK: **MEDIUM-HIGH** for the scroll/crash symptom.
The latch logic interacts with reconvert lifecycle. If `g_geom_attempted_c`
holds a pointer to a content that is freed and replaced (ABA), the check
`g_geom_attempted_c == (void *) content` becomes a stale-pointer
comparison. The new style/inherited-color fast path also fires inside
the reconvert pipeline — a new surface for re-entrancy.

DELETED: `apply_perf_sync_coalesce_once.py` and its workflow — the
one-shot automation scaffolding is gone after this commit.

PROVEN REGRESSION: NO
HARDWARE STATUS: NOT TESTED
PRIORITY: MEDIUM-HIGH — this commit is in the B→C interval. If B is GOOD
and C is BAD, this is a candidate to isolate specifically.

---

## AUDIT-10

SHA: `57c9f78e6` (full: `57c9f78e6...`)
TITLE: perf(reconvert): reuse recascaded styles during rebuild
AUTHOR: OpenAI Codex (automated apply_perf_style_cache_once.py)
DATE: 2026-09-03

INTENDED OPTIMIZATION:
Cache recascaded Style-B results from the old box tree and reuse them
for stable nodes during the new tree construction.

ACTUAL SEMANTIC SURFACE:
- Adds `g_reconv_style_cache[8192]` — an 8192-entry direct-mapped cache
  keyed by dom_node pointer and box pointer.
- Adds `css_select_results_ref()` to libcss — a function that duplicates
  a css_select_results container while sharing the immutable computed
  styles by reference.
- Cache entries are populated during the old-tree walk and consumed
  during the new-tree build.
- **Lifetime concern:** the cache stores `dom_node *` pointers. If the
  DOM node is freed between the cache population and consumption (which
  can happen in a reconvert during navigation), the cache holds a stale
  pointer. The cache key check is a raw pointer equality (`node == e->node`),
  not a generation-token check.
- The 8192-slot static array is ~65KB on the stack or BSS — non-trivial
  on a 128MB G3 but not catastrophic.

RISK: **HIGH** for the crash family under investigation.
Stale dom_node pointers in an 8192-slot static cache that persists across
reconvert rounds is exactly the class of UAF that has caused previous
crashes. If the cache is not cleared on navigation or content teardown,
old node pointers can be compared against nodes in a new document.

PROVEN REGRESSION: NO
HARDWARE STATUS: NOT TESTED
PRIORITY: HIGH — if B→C bisect finds a bad commit, audit whether this
cache is properly invalidated on content teardown.

---

## AUDIT-11

SHA: `4357bbfbc358af652daf3c4df6fee660b9604aa2`
TITLE: perf(font): make macos9_font_position linear
AUTHOR: OpenAI Codex (automated apply_perf_font_position_once.py)
DATE: 2026-09-03

INTENDED OPTIMIZATION:
Replace O(n²) prefix-measurement loop (re-measure every UTF-8 prefix)
with a single MeasureText call that returns cumulative positions array,
making the function O(n).

ACTUAL SEMANTIC SURFACE:
- Replaces the character-by-character `macos9_font_measure()` loop with
  `MeasureText()` writing into a `static short char_locs[4097]` array.
- **`char_locs` is a static local — shared across all calls to the
  function, not re-entrant.** If `macos9_font_position` is called
  recursively or from a callback fired during MeasureText, `char_locs`
  will be corrupted. On OS 9 cooperative scheduling makes this unlikely
  but not impossible (OT notifier, timer callback).
- Preserves soft-hyphen, MacRoman expansion, synthetic spacing semantics
  per the commit message — but the inlined version of these paths
  must be verified to match the original exactly.
- The static array is 4097 * 2 = 8194 bytes in BSS — acceptable.

RISK: MEDIUM for crash symptom. Re-entrancy via static buffer is a
latent hazard; the cooperative scheduler makes it low-probability but
not zero. Incorrect position output produces layout errors that can
cascade into scroll jank (box positions wrong → layout passes repeat).

PROVEN REGRESSION: NO
HARDWARE STATUS: NOT TESTED

---

## AUDIT-12

SHA: `c6b026b05386c19323ebbffdbd9244d02f7812fb`
TITLE: perf(content): cache live-registry lookups
AUTHOR: OpenAI Codex (automated)
DATE: 2026-09-03

INTENDED OPTIMIZATION:
Add an 8-slot epoch-tagged cache inside `macos9_content_find()` to
avoid repeated linear scans of the content registry.

ACTUAL SEMANTIC SURFACE:
- Adds `macos9_content_find_cache[8]` with fields: content pointer,
  epoch, idx.
- Cache validated against `macos9_content_registry_epoch` — incremented
  on every register/unregister.
- If the epoch is correct, skips the linear scan.
- **The epoch approach is sound IF the epoch is incremented on every
  mutation to the registry.** If any registration/unregistration path
  fails to increment the epoch, the cache returns stale answers.
- The epoch is a global; any path that adds/removes content without
  going through the normal registry path would leave the cache valid
  but wrong.

RISK: MEDIUM. Cache staleness in the content registry maps directly to
the crash family: a freed content that still has a valid cache entry
would pass `macos9_content_is_live()` and be dereferenced as live.
This is the ABA problem the generation token was designed to prevent.
If the epoch works correctly, the risk is low; if any content teardown
path doesn't increment the epoch, the risk is HIGH.

PROVEN REGRESSION: NO
HARDWARE STATUS: NOT TESTED
PRIORITY: MEDIUM-HIGH — if bisect lands in the B→C interval near this
commit, audit whether all content teardown paths increment the epoch.

---

## Summary Risk Table

| Audit | SHA       | Risk    | Crash Family | Priority |
|-------|-----------|---------|--------------|----------|
| 01    | 9f583e6b  | LOW-MED | None direct  | LOW |
| 02    | 260c5f90  | MEDIUM  | None direct  | LOW |
| 03    | 7186439f  | **HIGH**| Scheduler freeze under UAF | **HIGH** |
| 04    | dbf74b51  | LOW     | None direct  | LOW |
| 05    | b7bca9f7  | LOW-MED | Corrupted content indirect | LOW |
| 06    | 9eae6d60  | LOW     | None direct  | LOW |
| 07    | fccd036a  | LOW     | None direct  | LOW |
| 08    | 7314f649  | PROVEN  | Flex layout  | SKIP/MASKED |
| 09    | b71f1b8e  | **HIGH**| Reconvert lifecycle re-entrancy | **HIGH** |
| 10    | 57c9f78e  | **HIGH**| UAF via style cache stale dom_node | **HIGH** |
| 11    | 4357bbfb  | MEDIUM  | Static buffer re-entrancy | MEDIUM |
| 12    | c6b026b0  | MED-HIGH| Content registry stale cache → UAF | **HIGH** |

### Top suspects for the specific nsurl/hlcache/deathrow crash family:
1. AUDIT-03 (`7186439f`) — scheduler cycle guard removal
2. AUDIT-10 (`57c9f78e`) — reconvert style cache with raw dom_node pointers
3. AUDIT-12 (`c6b026b0`) — content registry lookup cache epoch integrity
4. AUDIT-09 (`b71f1b8e`) — JS sync geometry latch (reconvert lifecycle)

These are STATIC FINDINGS ONLY. Hardware is authoritative.
