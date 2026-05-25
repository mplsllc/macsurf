# MacSurf security notes

This file records security-relevant context for the shipped Mac OS 9
binary. The goal is to make it cheap to answer "is this CVE/finding
relevant to MacSurf?" without re-deriving the build configuration
each time an automated scanner files an issue or PR.

## What's actually in the Mac binary

The CodeWarrior 8 project (`browser/netsurf/frontends/macos9/MacSurf.mcp`)
is the ground truth for what gets compiled into the shipped binary.
The repository carries a lot of vendored upstream source that is NOT
in the project file list and never reaches the Mac:

- `browser/libnsgif/src/*.c` — not in `MacSurf.mcp`. GIF decoding is
  handled by QuickTime's GraphicsImporter via
  [`browser/netsurf/frontends/macos9/macos9_image.c`](../browser/netsurf/frontends/macos9/macos9_image.c).
- `browser/libnspng/` (if present) — not used. PNG decoding uses
  lodepng (real per-pixel alpha) or QuickTime depending on the
  format-alpha sniff. See macos9_image.c.
- Other NetSurf frontends (`browser/netsurf/frontends/{atari,amiga,...}`)
  — only `frontends/macos9/` is compiled on the Mac side.
- NetSurf's reference HTTP fetchers, libcurl integration, IPv6 paths,
  JavaScript SpiderMonkey bindings — all replaced or stubbed for OS 9.

When evaluating an external security report, the question to ask is
**"is this code reachable in the Mac binary?"**, not just "is it in
the repo?" The answer is usually no for anything outside
`frontends/macos9/`, `content/handlers/{html,css,image}/`, and the
five linked libraries (`libcss`, `libdom`, `libhubbub`,
`libparserutils`, `libwapcaplet`).

## Policy on automated security PRs

MacSurf is a small, maintainer-driven project. PRs from automated
vulnerability scanners are reviewed but **closed without merging by
default**, even when the technical content is correct. The reasons:

- The submission rarely respects the project's build configuration,
  so "critical" findings often turn out to apply to dead code in the
  vendored tree.
- Maintainer authorship matters for a small project; we'd rather
  apply a fix ourselves than carry a third-party commit.
- Automated PRs are usually marketing campaigns — the submitter's
  conversion funnel benefits even when the merge doesn't help us.

If a scanner report turns out to be relevant to code we actually
ship, the maintainer applies the fix manually in a normal
`fixesNN.tar` round, with the scanner's report cited in the commit
message for attribution.

## Recorded reports

### libnsgif integer overflow (OrbisAI Security PR #21, closed 2026-05-24)

- **Where the bug lives**: `browser/libnsgif/src/gif.c`,
  `nsgif__record_frame()`, around line 311.
- **What the bug is**: `width * height * pixel_bytes` is computed
  using `size_t` (32 bits on classic Mac OS 9 PowerPC). With
  attacker-controlled GIF header dimensions, the multiplication can
  overflow, producing a small `realloc()` allocation followed by a
  large `memcpy()` write — a heap buffer overflow with
  attacker-controlled contents.
- **CWE**: CWE-120 (classic buffer overflow).
- **Why it doesn't affect the Mac binary today**: the file is not
  in `MacSurf.mcp`. GIF decoding routes through QuickTime's
  GraphicsImporter (`macos9_qt_image_mime` table registers
  `image/gif` against a content handler that calls QT, not libnsgif).
  The patched function is never linked.
- **The fix, if we ever need it**: insert an overflow guard before
  the `realloc()` call.

  ```c
  /* in nsgif__record_frame(), before the realloc */
  if (height == 0 ||
      width > SIZE_MAX / pixel_bytes / height) {
      return;
  }
  ```

  Short-circuit zero-height to dodge the divide-by-zero, then the
  multiplication-safe form of "would `width * pixel_bytes * height`
  overflow size_t?". This is the standard guard and the PR's fix is
  correct in C terms.

- **When to actually apply it**: only if we ever switch the GIF
  decode path from QT GraphicsImporter to libnsgif (e.g. as a
  fallback when QT chokes on a file). At that point libnsgif's
  source files would join `MacSurf.mcp` and this overflow would
  become reachable; apply the fix as part of that switch.

## Mac OS 9 attack-surface notes

Some things worth being explicit about even though no scanner has
flagged them:

- **No JavaScript sandboxing.** Duktape runs ES5 directly in the
  app process. We accept that for the platform's threat model
  (single-user, no live financial sessions, no modern web identity).
  Pages with hostile JS are blocked at the network layer (proxy
  refuses to fetch) rather than at the engine layer.
- **HTTP fetching goes through a TLS-stripping proxy by default.**
  All traffic between the Mac and the proxy is plain HTTP. This is
  a known and accepted property of the platform; OS 9 has no modern
  TLS stack in the base build. The `macTLS` sibling library is an
  optional native HTTPS path for users who don't want the proxy.
- **The OT fetcher uses `*InContext` calls.** A `'carb'` resource is
  mandatory (see `docs/resources.md`) — without it, CFM treats the
  binary as classic PEF and CarbonLib never initialises its OT
  context, producing crashes inside OTClientLib at fixed addresses.
  This is a build-correctness concern, not a security one, but worth
  recording.
- **Carbon CFM platform limit: no passive OT bind.** Documented in
  CLAUDE.md. Not security-relevant but a frequent source of bug
  reports.

## How to log a future report

If a scanner or human files a security report and it survives the
"is this in MacSurf.mcp?" test, record it here with:

1. The reachable code path (file + function + line)
2. The CWE classification
3. Whether a fix has shipped or is queued
4. Cross-reference to the `fixesNN` round that resolved it

If the report does NOT survive the reachability test, add a short
entry under "Recorded reports" above explaining why it doesn't apply,
so the next maintainer doesn't have to re-derive it.
