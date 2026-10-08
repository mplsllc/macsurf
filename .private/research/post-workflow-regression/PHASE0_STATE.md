# Phase 0 — State Preservation Snapshot

Captured: 2026-09-08T21:27 CDT

## Current Branch
`stabilization/performance`

## Current HEAD
`8c3bb9118876ea955d926f633364fa4d835b0327`
`diag(reconvert): activate CONTROL_RECONVERT_OFF build (in-file define)`

This is build E from the A/B experiment — `#define MACSURF_RECONVERT_DISABLED` is
LIVE in macos9_reconvert.c as shipped to the Mac.

## git status --short
```
 M browser/netsurf/frontends/macos9/javascript/macsurf_qjs.c
```

The dirty file is macsurf_qjs.c — unrelated to the reconvert gate.
This file was NOT shipped in the current build drop; its dirty state
carries forward from earlier work and does not affect the current Mac binary.

## Recent log
```
8c3bb9118 (HEAD) diag(reconvert): activate CONTROL_RECONVERT_OFF build (in-file define)
972994c2d diag(reconvert): add MACSURF_RECONVERT_DISABLED compile-time A/B gate
18e9c3c56 fix(hlcache): retain content pin through entry retirement
d0d566fef fix(lifetime): initialize deferred content-user state
18924579f feat(diag): aggregate JavaScript Web API gaps
```

## Five A/B Test Points

| Label | SHA | Description |
|-------|-----|-------------|
| A | `b318c3c0ce88cd0dfca4c72b5ff6c94d6713e7ff` | Merge branch 'workflow' — baseline |
| B | `fccd036ac6035df3654154dd8757f3cc5b2fde24` | Early optimization snapshot (11 commits after A) |
| C | `4ef8bc71d4c29b87695b6baf26954025fe4554ba` | Performance tip (131 commits after A) |
| D | `972994c2d1203ccfd8fe26651440777165e540de` | Current stabilization — RECONVERT ON |
| E | `8c3bb9118876ea955d926f633364fa4d835b0327` | Current stabilization — RECONVERT OFF |

## Working tree before regression test
No destructive changes made to `stabilization/performance`.
The regression test checkout MUST use a separate worktree or a fresh checkout
directory — never `git checkout` on top of this working tree.
