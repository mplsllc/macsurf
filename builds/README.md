# MacSurf builds

Two StuffIt archives, the only files you need depending on what you want to do.

## `MacSurf.sit`, ready-to-run binary

The compiled MacSurf application, ready to launch on **Mac OS 9 with CarbonLib 1.5 or later**. Expand on the Mac with StuffIt Expander and double-click. No build step required.

- Target: PowerPC G3 / G4
- Mac OS 9.1, 9.2.2
- CarbonLib 1.5+
- 16&nbsp;MB application partition

## `MacSurf-BuildPack.sit`, build-it-yourself bundle

Everything you need to build MacSurf from source on a real Power Mac running CodeWarrior 8 Pro:

- All MacSurf C source (NetSurf core + the `macos9` frontend + libcss / libdom / libhubbub / libparserutils / libwapcaplet / Duktape)
- The `MacSurf.mcp` CodeWarrior project file with target settings, library search paths, and PEF / CFM output configured
- The shim headers needed for C89 / CW8 compatibility (in `frontends/macos9/shims/`)

Expand on the Mac, open `MacSurf.mcp` in CodeWarrior 8, and choose **Build**. See [../docs/codewarrior-setup.md](../docs/codewarrior-setup.md) for the full setup walkthrough.

---

These archives are updated alongside the source tree. The current binary corresponds to whatever fix round the surrounding commit shipped, see the root [README](../README.md) and [docs/HISTORY.md](../docs/HISTORY.md) for what's in the current build.
