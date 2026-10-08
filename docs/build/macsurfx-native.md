# MacSurfX native PowerPC bring-up

## Captured state, 2026-09-20

Branch: `feature/macsurfx-macho`, HEAD `4e36b445177fdf056a0f12817db9cd5b960b6480`.
The named `/home/patrick/Webs/macsurf-x` worktree was absent; the checkout at
`/home/patrick/Webs/macsurf` was on that branch and clean. The existing
`forclaude/drop-to-imac.sh` resolves its source root from its own location,
so this checkout is the transfer source. No transfer was made during capture.

Tiger: `Darwin 8.11.0`, PowerPC. CodeWarrior IDE path:
`/Applications (Mac OS 9)/Metrowerks CodeWarrior 8.0/Metrowerks CodeWarrior/CodeWarrior IDE`.
The IDE's AppleScript `version` property returned no value; the exact 8.3
patch level remains to be verified. Active project: `Back40:Projects: MacSurfQ`.
Targets: `MacSurf`, `MacSurfX`. Original project mtime:
`2026-09-20 21:53:24`, size 335193 bytes.

The original project was copied using `ditto --rsrc` to
`/Projects/backups/MacSurfQ-pre-native-2026-09-20-221246` before any target
change. The IDE's File > Export Project command produced
[`macsurfx/MacSurfX-pre-rebuild-2026-09-20.xml`](macsurfx/MacSurfX-pre-rebuild-2026-09-20.xml).
The export includes both targets and their settings, file lists, frameworks,
and link order. The live `MacSurf` target was not changed.

## Installed native reference

CodeWarrior supplies `Mac OS C/Mac OS X Mach-O/Mac OS Toolbox/C Toolbox Mach-O Nib`.
It was copied with `ditto --rsrc` to `/Projects/MacSurfX-NativeReference`.
The target XML is in
[`macsurfx/MacSurfX-native-reference-2026-09-20.xml`](macsurfx/MacSurfX-native-reference-2026-09-20.xml).
The selected `Toolbox Mach-O Debug` target uses `MacOS X PPC Linker`,
`MW C/C++ PPC Mac OS X` for `.c`, an application package, `crt1.o` first in
link order, `Carbon.framework`, `System.framework`, and
`MSL_All_Mach-O_D.lib`. Its prefix is `MSL MacHeadersMach-O.h`.

This supplied stationery uses the MSL Mach-O runtime, so it is a reference for
the native compiler, linker, framework, and package setup, but not the selected
Darwin libc runtime model. Its first attempted Update Project opened an IDE
error window reporting `Illegal precompiled header version` from the installed
`MSLMacHeadersMach-O` prefix. No executable was produced. The reference must
be repaired and built before MacSurfX reconstruction. The installed
`MacHeadersMach-O.mcp` was opened to rebuild its `MSL MacHeadersMach-O`
precompiled header after a resource-preserving backup to
`/Projects/backups/MacHeaders-Mach-O-pre-rebuild-2026-09-20`.
That build fails in MSL's `size_t_mach.h` because `ppc/ansi.h` cannot be
opened. A search of the installed support tree and `/usr/include` found only
`MacOS X Support/Headers/(wchar_t Support fix)/machine/ansi.h`.
The MSL header's requested `ppc/ansi.h` is therefore not supplied at that
path. This supports bypassing the MSL precompiled prefix for the planned
Darwin-libc target; it does not justify adding a fake `ppc/ansi.h`.

## Confirmed MacSurfX differences

| Setting | Captured MacSurfX | Supplied native stationery |
| --- | --- | --- |
| `.c`, `.h`, `.cpp` mapping | `MW C/C++ PPC Mac OS X` | same |
| Linker | `MacOS X PPC Linker` | same |
| Package | `MacSurfX.app` | `SimpleHello Debug.app` |
| Prefix | `macsurf_prefix_osx.h` | `MSL MacHeadersMach-O.h` |
| Frameworks | CarbonCore, Carbon | Carbon, System |
| Runtime/startup in link order | `CarbonLib`, `MSL_C_Mach-O.lib`, `MSL_Runtime_Mach-O.a`, `MSL_Runtime_Mach-O.lib`; no `crt1.o` first | `crt1.o` first, `MSL_All_Mach-O_D.lib` |
| Require framework-style includes | false | true |

Metrowerks' installed `Making_Mach-O_Projects_Note.txt` explicitly says to
reset file mappings after changing linkers, remove PEF libraries, add Carbon
and System frameworks, and put `crt1.o` at the **top** of link order. The
`Targeting Mac OS` manual says the `OS X Volume` source tree is mandatory for
system framework access paths and that framework bundles should be added as
whole bundles.

## Next build steps

1. Select a source prefix for an isolated copy of the stationery or establish
   an equivalent native project without the MSL precompiled header, then
   build and launch it. Record `file`, `otool -hv`, and `otool -L` output.
2. Produce a native Darwin-libc reference from the proven compiler/linker and
   framework setup, with an explicit minimal probe. Do not import the MSL
   runtime libraries into the selected MacSurfX runtime model.
3. Reconstruct MacSurfX from that verified setup, retaining its source list
   and leaving the Classic target untouched. Build the libc/Carbon probe
   before the full application.

The probe and native MacSurfX build have **not** passed yet.


## Variant J: known-good PowerPC Mach-O link (2026-09-22)

Branch `feature/macsurfx-macho`. Evidence lives under
[`macsurfx/variant-j/`](macsurfx/variant-j/) (manifest:
[`macsurfx/variant-j/MANIFEST.md`](macsurfx/variant-j/MANIFEST.md)).
Large ktrace dumps are preserved in `macsurf-private` under
`.private/research/macsurfx-link-20260922/`.

### Observed rule

Under CodeWarrior 8's two-level Mach-O namespace, the **project-visible dylib
basename must match the leaf of the dylib's `LC_ID_DYLIB` install name**.
For Darwin's system C library that is:

- install name: `/usr/lib/libSystem.B.dylib`
- required project filename: `libSystem.B.dylib`

The successful mutation (Variant J) renamed the project-visible
`libSystem-ppc.dylib` entry to `libSystem.B.dylib` at five XML `PATH` sites
(FILE ×2, LINKORDER ×2, GROUPLIST ×1). File **bytes were unchanged**.

### Two findings — keep distinct

1. **Thin PPC compatibility copy** for fat/universal import compatibility
   (`libSystem-ppc.dylib` content is a thin `MH_MAGIC` PPC dylib, not the
   FAT system binary).
2. **Install-name-leaf basename** for two-level symbol identity: the file
   on disk / in the project must be named `libSystem.B.dylib`, while its
   `LC_ID_DYLIB` remains `/usr/lib/libSystem.B.dylib` (do **not** rewrite
   the install name).

The correct artifact is therefore a **project-local thin PPC dylib named
`libSystem.B.dylib` whose `LC_ID_DYLIB` is still
`/usr/lib/libSystem.B.dylib`**. Describing J as fixed only because it uses
"the PPC-only copy" is incomplete — both findings apply.

The exact CW8 internal mechanism is **inference** from observed behavior;
the observed pass/fail sequence is fact. Do not restate inference as
documented Metrowerks behavior.

### Experiment sequence (all end-to-end)

| Variant | Mutation vs base | Result |
| --- | --- | --- |
| D | thin `libz` | same 4×348-byte undefined (`exit`, `errno`, `atexit`, `__keymgr_dwarf2_register_sections`) |
| E | flat namespace | same |
| F | + `System.framework` only | same |
| H | `whichfileloaded` / `whyfileloaded` | same |
| I | `FILEKIND Unknown` | same |
| **J** | **rename `libSystem-ppc.dylib` → `libSystem.B.dylib` (5 PATH sites)** | **LINK PASS** |

### Objective artifacts

- Canonical export: [`macsurfx/SystemLibRef-PASS.xml`](macsurfx/SystemLibRef-PASS.xml)
  (byte-identical to `variant-j/Link-J-rename-verified.xml`, md5
  `782ab5877f672debaa5ae43ef05367a5`).
- Linked executable `SimpleHello Debug` / `variant-j/SimpleHello-Debug`:
  Mach-O ppc executable, 17852 bytes, md5
  `9c9ccb0c95afbb7e963c05efa7a35052`, flags `NOUNDEFS|DYLDLINK|PREBOUND|TWOLEVEL`,
  load commands: `/usr/lib/libSystem.B.dylib`, `/usr/lib/libz.1.dylib`,
  `Carbon.framework`. `nm -u` still shows the classic six undefined names;
  those symbols are **prebound** (nonzero `n_value`, two-level `n_desc`), not
  left for dyld to fail on.
- Project dylib `libSystem.B.dylib`: thin PPC, 2221800 bytes, md5
  `7cf8c04b026bf108a52c6297eb325abe`, `otool -D` →
  `/usr/lib/libSystem.B.dylib`.

**Hardware/runtime launch of SimpleHello has not been confirmed by the
maintainer.** Until they confirm on the real case, state this as
"shipped — awaiting verification," not "verified."

### Failed-variant evidence

`variant-j/link-{D,E,F,H,I}-errors*.txt` — each 348 bytes, the identical
four undefineds, preserved so the sequence is auditable.

### Project settings captured in SystemLibRef-PASS.xml

- Linker: `MacOS X PPC Linker` (both `Toolbox Mach-O Debug` and
  `Toolbox Mach-O Final`)
- `.c` compiler: `MW C/C++ PPC Mac OS X`
- Frameworks: `Carbon`, `System`
- `MWLinker_MachO_twolevelnamespace` = `1`
- `MWLinker_MachO_whichfileloaded` = `1`, `MWLinker_MachO_whyfileloaded` = `1`
- `RequireFrameworkStyleIncludes` = `true`, `AlwaysSearchUserPaths` = `false`
- Prefix: `probe_prefix.h` (not MSL, not `macsurf_prefix_osx.h`)
- Link order (both targets): `mwcrt1.o`, `SimpleHello.c`, `SimpleHello.plc`,
  `SimpleHello.nib`, `libSystem.B.dylib`, `libz.1.2.3.dylib`
- No `libSystem-ppc.dylib` PATH remains in the export.

### Constraints still in force

- Never modify Tiger's `/usr/lib/libSystem*.dylib`, `/usr/lib/libz*`, or
  `/System/Library/Frameworks/System.framework`.
- Compatibility runtime stays project-local under
  `/Projects/MacSurfX-NativeReference-NoPCH/`.
- Do not retry variants A–I without new evidence; do not reintroduce the
  universal-libSystem theory; do not retry System.framework-only (F) alone.
- Classic `MacSurf` target must keep building after any shared-source change.
- New shared code migrates to `MACSURF_CLASSIC` / `MACSURF_OSX` /
  `MACSURF_MACHO` — do not add new `__MACOS9__` conditionals.
