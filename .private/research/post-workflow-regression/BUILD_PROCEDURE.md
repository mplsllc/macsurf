# Hardware Build Procedure

For each of the five test points (A/B/C/D/E), follow these steps exactly.
Do NOT reuse object files from a different commit.

---

## Before anything: protect stabilization/performance

The current Mac source directory corresponds to commit E
(`8c3bb9118` — RECONVERT OFF).

Before switching to any other commit:

1. Zip or snapshot the current Mac source tree.
2. Record the binary mtime of the current MacSurf app.
3. Do NOT overwrite the current Mac source until you are ready to restore it.

Recommended: keep separate source trees for each build if disk space allows.

---

## Per-commit build sequence

### Step 1: Checkout on Linux

```bash
cd /home/patrick/Webs/macsurf
git fetch --all -q

# Example for commit A:
git checkout b318c3c0ce88cd0dfca4c72b5ff6c94d6713e7ff

# Verify:
git rev-parse HEAD
git status --short   # must be clean (or only macsurf_qjs.c dirty — that's OK)
```

### Step 2: Transfer source to Mac

Use drop-to-imac.sh with the full source set needed for that commit.
The safest approach for historical builds is to ship ALL changed files
relative to the commit, not just one file:

```bash
# Find files changed between the commit and current stabilization tip:
git diff --name-only 8c3bb9118 <target-sha> | grep -v '^\.github\|^tools\|^\.private' | head -30
```

Then ship each of those files. For A vs E there will be many.

Alternatively: use `git archive` to produce a full source zip of the
commit and unpack it on the Mac, overwriting the source directory.

**Critical:** every source file touched between the commits must be
correct on the Mac before building.

### Step 3: Full clean build in CodeWarrior

**Do not use Bring Up To Date from a previous commit's objects.**

In CodeWarrior:
1. Menu → Project → Remove Object Code (or equivalent full clean)
2. Confirm removal of ALL .o files
3. Menu → Project → Bring Up To Date (full rebuild from source)
4. Verify: zero errors in the message window
5. If there are errors, record them as BUILD FAIL — do NOT modify source to fix them

### Step 4: Verify the binary

After building:
1. Note the mtime of MacSurf.app (Get Info in Finder)
2. Record this timestamp alongside the source SHA
3. Launch MacSurf — confirm it starts
4. Check the debug log first line for a build-identifying marker if any

### Step 5: Run the test procedure

See TEST_PROCEDURE.md

### Step 6: Record results

Copy RUN_TEMPLATE.md to:
```
.private/research/post-workflow-regression/RUNS/<sha>-<site>-<date>.md
```
Fill every field. Do not leave fields blank.

Copy MacSurf Debug.log to:
```
.private/research/post-workflow-regression/logs/<sha>-<site>-<date>/Debug.log
```

---

## Restoring stabilization/performance after testing

```bash
git checkout stabilization/performance
git rev-parse HEAD  # must be 8c3bb9118...
```

Then re-drop macos9_reconvert.c (commit E) to the Mac if testing
had overwritten it.

---

## Commit A special note

Commit A is the workflow merge. Its parents are:
- Workflow side: `a0e7c3687a9265a526e8d69a656022ec83a9aba3`
- Pre-merge master: `b548be0e80afaba1cc8f591ecf553bf2f2e6afb8`

If A turns out to be BAD, test these two parents before declaring
the regression pre-workflow. See PHASE 6 / CASE 6 in the mission doc.

---

## Cache state control

For primary comparison runs (cold):
1. Before each build: in MacSurf, go to Preferences → Clear Cache (if available)
   OR manually delete all files in MacSurfData/Cache/ on the Mac.
2. Confirm cache is empty by checking file count in that folder.
3. Record: COLD_WARM = COLD

For warm comparison (secondary, same snapshot):
1. Run build A cold, then preserve a copy of MacSurfData/Cache/
2. Restore that same copy before each subsequent build's warm run
3. Record: COLD_WARM = WARM, CACHE_STATE = SHARED_SNAPSHOT_A
