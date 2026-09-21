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
be repaired and built before MacSurfX reconstruction.

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

1. Resolve the stationery's incompatible installed precompiled header by
   rebuilding that prefix or selecting a source header, then build and launch
   the reference. Record `file`, `otool -hv`, and `otool -L` output.
2. Produce a native Darwin-libc reference from the proven compiler/linker and
   framework setup, with an explicit minimal probe. Do not import the MSL
   runtime libraries into the selected MacSurfX runtime model.
3. Reconstruct MacSurfX from that verified setup, retaining its source list
   and leaving the Classic target untouched. Build the libc/Carbon probe
   before the full application.

The probe and native MacSurfX build have **not** passed yet.
