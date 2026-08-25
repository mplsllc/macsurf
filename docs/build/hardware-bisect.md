# Bisecting a regression on real hardware

Finding the commit that broke something on OS 9 / OS X, when the only authority
is the Mac. Written up after the 2026-08-25 Facebook hunt (#167); the mechanics
below were all paid for once already.

The Linux tree is never the thing under test. What gets tested is the source on
the Mac plus whatever the CodeWarrior project says to compile, so a bisect point
is defined by **both** — reverting files is not enough if the project's file
list disagrees with the commit you are testing.

---

## 1. Work out the real shape of the range first

```bash
git diff --name-status <baseline> HEAD -- '*.c' '*.h' | awk '{print $1}' | sort | uniq -c
```

The two counts that matter are different problems:

- **Modified (M)** — ship the baseline version of each. Straightforward.
- **Added (A)** — these files *exist on the Mac and are in the project*, but did
  not exist at the baseline. There is no baseline version to ship. Each one is
  either harmless or a build-breaker, and you have to know which.

An added file breaks the build only if it references symbols that live in files
you are reverting. Check that directly rather than assuming:

```bash
grep -oE "CSS_PROP_FILL|css__parse_fill|<other new symbols>" <added-file>
```

In the 2026-08-25 range, 131 files were added but only two mattered:
`p_fill.c` and `s_fill.c` needed `CSS_PROP_FILL` from the reverted libcss
headers. The other 129 (all of `libwebp`, `macos9_webp.c`) depend only on
headers that stay, so they compile as unreferenced dead code and can be left in
the project untouched.

**Anything that must come out goes out through the IDE's Apple Events**, not by
editing the project file on Linux — see `automated-ship.md` §3.7, and back the
project up first. Remember to put it back when the bisect crosses the commit
that introduced it.

---

## 2. Staging the revert

`drop-to-imac.sh` resolves its sources as `$(dirname $0)/..`, i.e. the **main
working tree** — not the current directory. A `git worktree` at the baseline is
therefore useful for *inspecting* the old tree but cannot be shipped from
directly. Stage into the main tree instead, ship, then restore:

```bash
git diff --name-only --diff-filter=M <baseline> HEAD -- '*.c' '*.h' \
    | grep -v '^harness/' > /tmp/bisect_files.txt
git checkout <baseline> -- $(cat /tmp/bisect_files.txt)
./forclaude/drop-to-imac.sh <fixnum> $(cat /tmp/bisect_files.txt)
git checkout HEAD -- $(cat /tmp/bisect_files.txt)      # restore immediately
```

Restore as soon as the transfer finishes, so the tree is never left dirty and
committed work is never at risk.

**Pushing dozens of files takes minutes** over the legacy SSH link (~6s/file).
Run it in the background rather than letting a foreground timeout kill it
mid-transfer, and confirm the count and `LF=0` on every row afterwards.

---

## 3. Building a bisect point

**Quit MacSurf first.** `Run` silently declines to build while the app is
running and the pipeline then packages the previous binary — the single most
expensive trap here, documented in `automated-ship.md` §3.2.1.

Then, since the files are already on the Mac:

```bash
./forclaude/ship.sh <fixnum>        # no paths: rebuild + relaunch only
```

**Verify the binary is the one you meant to build** before reading any log. The
cheapest check is a marker string that exists in one tree and not the other:

```bash
ssh imac 'strings /Projects/MacSurfBuilds/MacSurf | grep -c FBDOCREQ'
```

Diagnostics added *after* the baseline are ideal markers, because they are
absent by construction in the older build.

---

## 4. Cost control

Two things dominate the time per point, and neither is the transfer:

- **A force-included prefix header** (`macsurf_prefix.h`) invalidates every
  translation unit. Any bisect point that crosses a commit touching it is a full
  ~925-file rebuild. Order the search so that boundary is crossed as few times
  as possible.
- **Project file list changes** are only a relink, not a rebuild. They are
  cheap; the expensive part is remembering to make them.

Pick probe points that keep both stable where you can, rather than a naive
binary chop that oscillates across them.

---

## 5. Record the same fields at every point

Decide the fields before the first build and do not add instrumentation
mid-bisect — a diagnostic that exists at some points and not others makes the
comparison worthless, and the older builds cannot have it anyway. Count from a
channel that exists across the whole range.

For #167 the fields were: visible result, `JS PAGE scripts/bytes/failed`,
`FBSTATE`, `rootKids`, total DOM append count, and whether the mount element is
mutated after insertion.

**The visible result is the maintainer's call**, on the real reported case. A
log field is evidence, not a verdict — see the verification directive in
`CLAUDE.md`.
