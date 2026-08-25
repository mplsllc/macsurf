# The Automated Ship Pipeline (`ship.sh`)

One command takes a code change from the Linux working tree to a running MacSurf
on the test Mac and a versioned `.sit` on the file server:

```
./forclaude/ship.sh <fixnum> [repo-relative-path]...
```

```
./forclaude/ship.sh 1329 browser/netsurf/frontends/macos9/window.c
```

With no paths it rebuilds and relaunches whatever is already on the iMac, which
is useful for re-running after a crash or confirming the tree is clean.

This replaces the old manual loop: drop files, switch to the Mac, hit Make in
the CodeWarrior IDE, watch for errors, drag the built app onto DropStuff,
copy the archive to the macfiles share, launch the app.

---

## 1. What it does, in order

| # | Step | Mechanism |
|---|------|-----------|
| 1 | Push changed sources to the Mac | delegates to `drop-to-imac.sh` |
| 2 | Build **and** launch MacSurf | AppleScript: `Run project document 1` |
| 3 | Check for compile errors | AppleScript: `messages of project document 1` |
| 4 | Stuff the built app | AppleScript: `tell app "DropStuff" to open …` |
| 5 | Copy the archive to macfiles | `ditto --rsrc` to the mounted AFP volume |

Steps 4 and 5 run *after* the app is already up, by design — the build blocks,
the launch happens, and the packaging proceeds while MacSurf is starting.

Hardware verification is **not** part of this pipeline and never will be. The
script stops at "the `.sit` is on macfiles"; whether a fix actually works is the
maintainer's call on the real reported case (see the verification directive in
`CLAUDE.md`).

---

## 2. Why this works at all: the Tiger AppleScript unlock

The whole pipeline rests on one non-obvious fact, verified empirically on
2026-08-25:

> **`osascript` invoked over SSH reaches GUI applications in the console login
> session on Mac OS X 10.4.11.**

This is the step that usually blocks this kind of automation. On later versions
of OS X, an SSH session lands in a different Mach bootstrap namespace from the
window server, so Apple Events sent to GUI apps fail with error `-609`
("connection is invalid"), and you need `launchctl bsexec` (10.5+) or a
resident helper agent in the console session to get around it. Tiger does not
enforce that separation the same way. Sending `tell app "Finder" to get name of
startup disk` over SSH returns `Back40`.

That means no polling daemon, no folder actions, no helper app — the Linux side
can drive the Mac's GUI applications directly and synchronously.

Two supporting facts make the rest fall into place:

- **CodeWarrior 8 runs natively, not under Classic.** `ps` shows a plain
  `CodeWarrior IDE` process. CW8's IDE is Carbon, so it is a first-class OS X
  application and a first-class Apple Event target.
- **CodeWarrior's IDE is AppleScript-scriptable**, and so is DropStuff.

If this pipeline is ever ported to a newer OS X test machine, **the SSH-to-GUI
hop is the first thing that will break**, and the fix is a resident agent in
the console session rather than direct `osascript` calls.

---

## 3. Step-by-step mechanics

### 3.1 Push (`drop-to-imac.sh`)

Unchanged and unwrapped — `ship.sh` calls the existing script rather than
duplicating it. It CR-converts each file, stamps a future mtime derived from
the fix number, `scp`s to the matching path under `/Projects/MacSurfSource/`,
and verifies size and CR/LF counts on the far side. See that script's header
for the path mapping and the `scp -O` / `-P` requirements.

If the push fails, `ship.sh` **does not build**. Building against a
half-delivered set of files produces a result that means nothing.

### 3.2 Build and launch — one call

```applescript
tell application "CodeWarrior IDE" to Run project document 1
```

Three things about this that are worth knowing before you change it:

- **`Run` covers both build and launch.** CodeWarrior's `Run` compiles, links,
  and then launches the resulting application — which is exactly the last step
  of the desired workflow, so there is no separate "open the app" call.
- **It blocks until the build completes.** Verified: the `osascript` call
  returns only after the link finishes, and MacSurf is running by the time
  control comes back. This is what lets packaging safely follow in sequence
  without racing an unfinished binary.
- **The direct parameter is required and specific.** Bare `Make` fails with
  `-10001` (descriptor type mismatch); `Make current project` is a syntax
  error. `Run project document 1` and `Make project document 1` both work.
  `count documents` is `1` and `get name of document 1` returns ` MacSurfQ`
  (note the leading space in the project name — it is real).

### 3.3 Error detection

```applescript
tell application "CodeWarrior IDE" to get messages of project document 1
```

`messages` is CodeWarrior's error/warning list for the project. It comes back
empty on a clean build. **If it is non-empty, `ship.sh` prints it and exits
without packaging** — a failed build must never put a `.sit` on macfiles.

Two notes:

- This replaces scraping `errors.rtf`. That file is only written when the
  messages window is saved by hand, so it goes stale and has misreported build
  state before. The live `messages` property cannot.
- **The populated-messages format is not yet confirmed.** Every build since the
  pipeline was written has been clean, so the failure branch has been reasoned
  about but not observed. Expect to adjust the printing when the first real
  build error comes through.

As a secondary signal the script compares the binary's mtime across the build
and reports "nothing needed recompiling" when it is unchanged, so a no-op build
is never mistaken for a successful relink. It also confirms via `ps` that
MacSurf actually came up.

### 3.4 Stuffing

```applescript
tell application "DropStuff" to open POSIX file "/tmp/macsurf-ship/MacSurf"
```

This is the scripted equivalent of dragging the app onto the DropStuff alias —
the same tool and the same settings the maintainer uses by hand, so the output
matches existing practice rather than introducing a new archive format.
DropStuff writes `MacSurf.sit` next to its input.

**DropStuff is asynchronous.** The `osascript` call returns immediately; the
archive appears later. The script therefore waits for the file to exist, then
waits for its size to stop changing, before touching it. Copying a `.sit` while
StuffIt is still writing it yields a truncated archive that fails to expand on
the OS 9 side.

Stuffing happens in a scratch directory (`/tmp/macsurf-ship`), not in
`/Projects/MacSurfBuilds/`, so it never collides with or overwrites the
`MacSurf.sit` files already sitting in the build folder.

### 3.5 Delivery to macfiles

`/Volumes/macfiles` is already mounted over AFP on the iMac and is writable
from the shell, so delivery is a local filesystem copy — no second transfer hop
and nothing to authenticate.

**Use `ditto --rsrc`, never `cp` or `mv`.** Both copies in this script cross a
fork boundary:

- The built MacSurf is a **CFM Carbon application whose resource fork carries
  the mandatory `'carb'` resource**. Without `'carb'`, CFM treats the binary as
  classic PEF, CarbonLib never engages, and any `*InContext` Open Transport
  call crashes at a fixed address inside OTClientLib. A plain `cp` strips the
  resource fork and produces exactly that.
- The `.sit` itself carries Finder type/creator metadata; losing it means the
  archive no longer double-clicks on OS 9.

Verified after the fact through the Finder: the delivered archive reports
creator `SIT!`, identical to the `.sit` files the maintainer produced by hand.

### 3.6 Naming — never overwrite

Archives land as `MacSurf<fixnum>.sit`. If that name already exists, the script
appends `-2`, `-3`, and so on rather than clobbering. Tying the archive name to
the fix number keeps the artifact traceable to the change that produced it;
the anti-collision suffix means re-shipping the same fix number (a `fixesNNb`
follow-up, a rebuild after a crash) never destroys the earlier artifact.

---

## 4. Prerequisites

- **The reverse tunnel must be up on the LAN side.** The VPS has no route to
  the Mac's network:

  ```
  ssh -p 52463 -i ~/.ssh/lunaxp -N -R 2223:192.168.137.2:22 patrick@116.202.231.103
  ```

- **`Host imac` in `~/.ssh/config`** supplies the legacy crypto the Mac's
  OpenSSH needs (`diffie-hellman-group1-sha1`, `ssh-rsa`, `aes128-cbc`) and an
  RSA key — ed25519 postdates this machine by a decade.
- **The CodeWarrior IDE must be running with the MacSurf project open.** The
  script addresses `project document 1`; it does not open the project itself.
- **`/Volumes/macfiles` must be mounted.** The script checks and fails loudly
  if it isn't.
- The Mac must be **logged in at the console** — the Apple Events go to that
  session.

---

## 5. Portability traps (Tiger's shell is old)

The remote half runs under Mac OS X 10.4's **bash 2.05b**. Two things bit
during development and are worth remembering before editing the remote block:

- **No `set -o pipefail`.** It fails with `set: pipefail: invalid option name`.
  The remote block uses plain `set -u`.
- **No `seq`.** `for i in $(seq 1 120)` silently expands to nothing, so the
  wait-for-DropStuff loop *did not run at all* on the first version — it only
  appeared to work because the archive happened to already exist. Counter loops
  are written out longhand.

This is the general shape of the hazard: a missing tool on Tiger tends to make
a loop vanish rather than error, so a guard can look present and do nothing.
The same failure mode as instrumenting a guard's presence instead of its
effect.

Also note `/Developer/Tools/GetFileInfo` is **not** installed on this machine —
type/creator has to be inspected through the Finder via AppleScript instead.

---

## 6. Failure modes and what they mean

| Symptom | Cause |
|---|---|
| `execution error: … (-609)` on any `osascript` | Console session gone, or the OS was upgraded past Tiger's namespace behaviour |
| `A descriptor type mismatch occurred. (-10001)` | AppleScript command sent without its required direct parameter |
| `The variable r is not defined. (-2753)` | `Run`/`Make` returns no value; don't assign the result |
| Push fails | Script stops before building — intentional |
| Non-empty `messages` | Compile errors; nothing is packaged |
| "binary mtime unchanged" | Nothing needed recompiling; not an error |
| `** MacSurf did not launch` | Build linked but the app failed to start — check the crash log |
| `** $MACFILES is not mounted` | AFP share dropped; remount on the Mac |
| `** DropStuff produced no archive` | DropStuff didn't start or was blocked by a dialog |

For pulling results back — build errors, the debug log, the newest crash entry
— use the companion `pull-from-imac.sh`.

---

## 7. Where the script lives, and why it isn't in git

`forclaude/ship.sh`, alongside `drop-to-imac.sh` and `pull-from-imac.sh`.
`forclaude/` is gitignored: these scripts encode one machine's tunnel layout,
share mounts, and SSH aliases, and are not part of the shipped project. This
document is the durable record; the script is the local instrument.
