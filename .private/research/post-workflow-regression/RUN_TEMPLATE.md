# Hardware Test Run Template

Copy this file to `RUNS/<build-label>-<site>-<YYYYMMDD>.md` for each run.

---

COMMIT: <full SHA>
BUILD: PASS / FAIL
BUILD_TIMESTAMP: <from binary mtime or launch log>
COLD_WARM: COLD / WARM
CACHE_STATE: EMPTY / PRESERVED / SHARED_SNAPSHOT_<label>

SITE: HN / 68kMLA / Hackaday / MacGarden / Other
URL: <exact URL>

NAV_DONE: YES / NO / PARTIAL
FIRST_RENDER: <seconds from launch to visible page content>

SCROLL_INITIAL: SMOOTH / MINOR_JANK / HEAVY_JANK / PROGRESSIVE_DEGRADATION / FREEZE / NOT_TESTED
SCROLL_AFTER_IDLE: SMOOTH / MINOR_JANK / HEAVY_JANK / PROGRESSIVE_DEGRADATION / FREEZE / NOT_TESTED
SCROLL_AFTER_NAV: SMOOTH / MINOR_JANK / HEAVY_JANK / PROGRESSIVE_DEGRADATION / FREEZE / NOT_TESTED

INTERACTION: <describe click/button/link behavior>
EDITOR_UI: <did comment/reply/post UI appear>
SUBMIT: TESTED_OK / TESTED_FAIL / NOT_TESTED

RECONVERT_ACTIVITY: <count from log if readable, else 'NOT_MEASURED'>
LAYOUT_ACTIVITY: <layout pass count / LAYPROF lines if present>

CRASH: NONE / YES
CRASH_FAMILY: NSURL / HLCACHE / DEATHROW / BOX_LAYOUT / QUICKJS / FETCH / UNKNOWN / NOT_APPLICABLE
CRASH_STACK: |
  <exact MacsBug / crash report lines>
LAST_RELEVANT_LOG: |
  <final LIFE/WORK lines before crash or session end>

NOTES: |
  <anything else observed>

LOG_PRESERVED_AT: .private/research/post-workflow-regression/logs/<build>-<site>-<date>/
