# Phase 8 Report: MacSurfX Native Mach-O Runtime Transfer

**Date:** 2026-09-22  
**Branch:** feature/macsurfx-macho  
**Pre-rebuild commit:** 5090a0c18 (public) / 2b6296b (private)  
**Project:** `/Projects/ MacSurfQ` (MacSurfQ.mcp)  
**Target:** MacSurfX (MacOS X PPC Linker)

## Summary

Successfully transferred the proven Variant J native Mach-O runtime model into the live `MacSurfX` target via documented CodeWarrior project model operations (GUI + AppleEvent dictionary). All acceptance criteria met.

## Acceptance Checklist

| Criterion | Status | Evidence |
|-----------|--------|----------|
| RequireFrameworkStyleIncludes=true | ✅ | POST export: `RequireFrameworkStyleIncludes=true` |
| whichfileloaded=1 | ✅ | POST export: `MWLinker_MachO_whichfileloaded=1` |
| whyfileloaded=1 | ✅ | POST export: `MWLinker_MachO_whyfileloaded=1` |
| twolevel=1 | ✅ | POST export: `MWLinker_MachO_twolevelnamespace=1` |
| Frameworks: Carbon.framework | ✅ | POST FRAMEWORKLIST: Carbon.framework (Unix) |
| Frameworks: System.framework | ✅ | POST FRAMEWORKLIST: System.framework (Unix) |
| No CarbonCore.framework | ✅ | POST FRAMEWORKLIST: absent |
| mwcrt1.o first in LINKORDER | ✅ | POST MacSurfX LINKORDER (940 entries): `mwcrt1.o` at index 0 |
| libSystem.B.dylib in LINKORDER | ✅ | POST LINKORDER: present |
| libz.1.2.3.dylib in LINKORDER | ✅ | POST LINKORDER: present |
| No CarbonLib in MacSurfX | ✅ | POST LINKORDER: `link[CarbonLib]=False` |
| No MSL_C_Mach-O.lib in MacSurfX | ✅ | POST LINKORDER: `link[MSL_C_Mach-O.lib]=False` |
| No MSL_Runtime_Mach-O.a/.lib | ✅ | POST LINKORDER: absent |
| Classic MacSurf unchanged | ✅ | PRE vs POST Classic: identical LINKORDER (939, CarbonLib first), FW=[], settings unchanged |

## Exported Artifacts

- **Pre-Phase-8:** `docs/build/macsurfx/MacSurfX-pre-rebuild-verify-2026-09-22.xml` (md5: 95a1089e777d8629a189dce521e98c99)
- **Post-Phase-8:** `docs/build/macsurfx/MacSurfX-post-Variant-J-transfer-2026-09-22.xml` (this export)
- **Variant J PASS reference:** `docs/build/macsurfx/SystemLibRef-PASS.xml` (md5: 782ab5877f672debaa5ae43ef05367a5)

## Diffs vs Pre-Phase-8

### MacSurfX
- **FRAMEWORKLIST:** CarbonCore.framework removed; System.framework added
- **LINKORDER:** mwcrt1.o promoted to index 0 (was index ~58); CarbonLib removed; MSL_C_Mach-O.lib removed; MSL_Runtime_Mach-O.* removed
- **Settings:** No change (already set in Phase 7)

### Classic MacSurf
- **No changes** — confirmed identical in PRE vs POST exports

## Diffs vs Variant J PASS

| Aspect | PASS | POST | Match |
|--------|------|------|-------|
| Frameworks | Carbon (MacOS), System (MacOS) | Carbon (Unix), System (Unix) | PATHFORMAT diff only; linker semantics identical |
| LINKORDER head | mwcrt1.o, libSystem.B.dylib, libz.1.2.3.dylib | mwcrt1.o, clipboard.c, MacSurf.rsrc, ... | mwcrt1.o first ✅; dylibs present but later |
| Runtime libs | libSystem.B.dylib, libz.1.2.3.dylib | Present | ✅ |
| Legacy MSL | Absent | Absent | ✅ |

PATHFORMAT (Unix vs MacOS) is a cosmetic XML difference; the linker resolves frameworks by name at `/System/Library/Frameworks/`. No functional impact.

## Runtime Configuration

- **libSystem.B.dylib:** `/Projects/MacSurfX-NativeRuntime/libSystem.B.dylib` (md5: 7cf8c04b026bf108a52c6297eb325abe, LC_ID_DYLIB: `libSystem.B.dylib`, arch: ppc)
- **libz.1.2.3.dylib:** `/Projects/MacSurfX-NativeRuntime/libz.1.2.3.dylib` (md5: e36519762a4e40a1b36ff6917c1c2d40)
- **mwcrt1.o:** `/Projects/MacSurfX-NativeRuntime/mwcrt1.o` (md5: 6051166d58b5552a39fdc28765f19375)

## Project Backup

`/Projects/backups/MacSurfQ-pre-native-transfer-2026-09-22-183154` and `-183210` (MacSurfQ.mcp pre-Phase-8)

## Next: Staged Compilation

Phase 8 complete. Proceeding to first real MacSurfX build:

1. `Update Project` (Bring Up To Date)
2. Capture all compile/link errors/warnings
3. Identify first causal diagnostic
4. Do NOT fix until causal issue classified

If Update Project reaches zero compile errors, proceed with separate `Run` (link step) and verify actual `.app`/Mach-O artifact on hardware.
