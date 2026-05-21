# Contributing to macsurf

Thanks for your interest in macsurf, a modern web browser for Classic Mac OS 9 PowerPC. Contributions of any size are welcome, from compatibility reports on real hardware to deep dives into the CSS engine.

macsurf is in **early alpha**. The codebase is moving fast, the platform constraints are unusual, and the appetite for a few odd corners (CodeWarrior 8.3, Carbon API, cooperative multitasking, a 16 MB application partition) is non-negotiable. If you're up for that, read on.

By participating in this project you agree to abide by our [Code of Conduct](CODE_OF_CONDUCT.md).

---

## Where to start

- **`good first issue`**, small, well-scoped tasks suitable for a first contribution: [open issues with `good first issue`](https://github.com/mplsllc/macsurf/issues?q=is%3Aissue+is%3Aopen+label%3A%22good+first+issue%22).
- **`help wanted`**, issues where outside help would meaningfully accelerate the roadmap: [open issues with `help wanted`](https://github.com/mplsllc/macsurf/issues?q=is%3Aissue+is%3Aopen+label%3A%22help+wanted%22).
- **`docs/status.md`**, the current punch list of in-flight work, deferred items, and known limitations.
- **[Discussions](https://github.com/mplsllc/macsurf/discussions)**, the right place to ask "is anyone already working on X?" before diving in.

## Reporting bugs

Use the [Bug report template](https://github.com/mplsllc/macsurf/issues/new?template=bug_report.yml). The fields that matter most:

- **Mac OS version** (9.0, 9.2.2, or SheepShaver).
- **Hardware**, model, CPU speed, RAM. Quirks are often hardware-tied.
- **macsurf build**, a release tag (e.g. `0.1a1`) or the most recent `fixesNN` you applied.
- **URL or HTML test case**, a tiny `.html` file that reproduces the bug is worth more than ten paragraphs of description.
- **Logs**, the tail of `macsurf_debug.log` (or `MacSurf.log`) around the failure.

## Reporting site compatibility

If you tried a real-world site and it works, partially works, or breaks, please file it with the [Website compatibility report template](https://github.com/mplsllc/macsurf/issues/new?template=compatibility_report.yml). **Positive reports are just as valuable as negative ones**, they tell us what's already good enough to keep working.

Testing on real hardware (G3, G4, Pismo, iMac, Beige PowerMac) is especially appreciated.

## Submitting code changes

1. **Fork** the repository and clone your fork.
2. **Create a branch** off `master`:
   - `fix/<issue-num>-short-description` for bug fixes
   - `feature/<short-description>` for new features
   - `chore/<short-description>` for tooling/build work
3. **Make focused commits**, one logical change per PR. Use the existing commit message style: a short subject line with a category prefix (`fixesNNN:`, `docs:`, `chore:`, etc.) optionally followed by a body explaining *why*.
4. **Reference issues** in the PR description: `Closes #N` (auto-closes on merge) or `Refs #N` (links without closing).
5. **Open a PR** against `master` and wait for review. CI will be added as it lands; in the meantime expect a manual code review.

## Code style, non-negotiable constraints

These aren't preferences. They're realities of the target platform. Code that violates these will not build:

- **Strict C89**. No `//` comments, no `inline`, no designated initializers, no variadic macros, no for-scope variable declarations.
- **All variable declarations at the top of their block**. Mixed declarations and statements break CodeWarrior 8.3.
- **CodeWarrior 8.3** is the canonical compiler. Retro68 PowerPC GCC is used for fast Linux-side syntax checks (see `docs/cross-dev-from-linux.md`).
- **Carbon API only**. No Cocoa, no modern macOS APIs. Anything outside the Carbon umbrella does not exist on Mac OS 9.
- **Cooperative multitasking only**. No threads, no preemption. Yield via `WaitNextEvent` or Thread Manager; long loops must yield periodically or the OS locks up.
- **Open Transport is used in synchronous non-`InContext` mode**. Yield via `kOTSyncIdleEvent`.
- **Mind the 16 MB partition**. Large allocations need justification. We have global budgets for decoded image bytes, stylesheet processing, and other heavy resources, see `frontends/macos9/macos9_image.c` and `content/handlers/css/cssh_css.c` for the pattern.

If you're unsure whether something will compile under CW8, do a Linux-side syntax check with the Retro68 cross-compiler before opening the PR. See `docs/cross-dev-from-linux.md`.

## Building

- **Mac side** (canonical): `docs/codewarrior-setup.md`. CodeWarrior 8.3 project lives at `browser/macsurf.mcp`.
- **Linux side** (syntax + structure checks only, does not produce a runnable browser): `docs/cross-dev-from-linux.md`.

## Testing

- `tests/css/`, minimal `.html` pages exercising specific CSS features. Add a test page for any new CSS property or selector you implement.
- `scripts/verify_macsurf.sh`, a helper for sanity-checking that the binary loads.
- Real-hardware testing matters. SheepShaver is fine for smoke tests but the bugs that actually bite users tend to be hardware-specific.

## Commit messages

Style matches existing history:

- **Subject**: imperative, lowercase prefix + colon + short description. Examples:
  - `fixes161b: global decoded image memory budget`
  - `docs: add CONTRIBUTING.md`
  - `chore: bump retro68 syntax-check flags`
- **Body** (optional): explain *why*, not *what*. The diff shows what. Reference issues with `Refs #N` or `Closes #N` where applicable.

## Licensing

- Browser code (everything inheriting from NetSurf) is licensed under **GPLv2**.
- The proxy and macSSL components carry their own LICENSE files, refer to those for their terms.
- By submitting a PR you agree to license your contribution under the same terms as the file(s) you're modifying.

## Questions?

For anything that isn't a reproducible bug or a concrete feature request, please use [Discussions](https://github.com/mplsllc/macsurf/discussions). Issues are for actionable, tracked work.

Thanks for helping make a browser that nobody should be able to write run on hardware that nobody expects to see browsing the modern web.
