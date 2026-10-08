# MacSurfX target vs SystemLibRef-PASS (2026-09-22)

Export: `MacSurfX-pre-rebuild-verify-2026-09-22.xml`
- md5: `95a1089e777d8629a189dce521e98c99`
- byte-identical to `MacSurfX-pre-rebuild-2026-09-20.xml` (`4130e3c04fb927ef72b665c5ba84ff40`): **False**
- PASS md5: `782ab5877f672debaa5ae43ef05367a5`

Targets in export: `MacSurf`, `MacSurfX`

## Setting diffs (MacSurfX vs Toolbox Mach-O Debug)

| Setting | MacSurfX | PASS |
| --- | --- | --- |
| `which` | `0` | `1` |
| `why` | `0` | `1` |
| `reqfw` | `false` | `true` |
| framework only on MacSurfX | `CarbonCore` | — |
| framework only on PASS | — | `System` |

### Runtime paths only on MacSurfX
- `CarbonLib`
- `MSL_C_Mach-O.lib`
- `MSL_Runtime_Mach-O.a`
- `MSL_Runtime_Mach-O.lib`
- `ostls_d1_probe.c`
- `ostls_mul64_probe.c`

### Runtime paths only on PASS (to transfer)
- `libSystem.B.dylib`
- `libz.1.2.3.dylib`
- `mwcrt1.o`

### Link-order head (first 8)

- MacSurfX: `['CarbonLib', 'MSL_C_Mach-O.lib', 'MSL_Runtime_Mach-O.a', 'MSL_Runtime_Mach-O.lib', 'clipboard.c', 'MacSurf.rsrc', 'after_after_frameset.c', 'after_body.c']`
- PASS: `['mwcrt1.o', 'SimpleHello.c', 'SimpleHello.plc', 'SimpleHello.nib', 'libSystem.B.dylib', 'libz.1.2.3.dylib']`

### Prefix
- MacSurfX: `['MacHeaders.c', 'macsurf_prefix_osx.h', '', '', 'Types.r', '']`
- PASS: `['', 'probe_prefix.h', '', '', '', '']`

### Classic MacSurf (must remain untouched)
- linker `MacOS PPC Linker`, frameworks `[]`, runtime `['CarbonLib', 'MSL_C_Carbon.Lib', 'MSL_Runtime_PPC_D.Lib', 'ostls_d1_probe.c', 'ostls_mul64_probe.c']`, prefix `['MacHeaders.c', 'macsurf_prefix.h', '', '', 'Types.r', '']`

## Transfer checklist (Phase 8 → MacSurfX only)

1. FILE name must be `libSystem.B.dylib` — never `libSystem-ppc.dylib`.
2. Add project-local thin PPC dylib + `libz.1.2.3.dylib` + `mwcrt1.o` from `/Projects/MacSurfX-NativeRuntime/` (provenance + checksums).
3. Link order: `mwcrt1.o` first; remove `CarbonLib`, `MSL_C_Mach-O.lib`, `MSL_Runtime_Mach-O.*` (PEF/MSL model).
4. Frameworks: ensure `System` present (PASS has Carbon + System; current has Carbon + CarbonCore).
5. Settings: `RequireFrameworkStyleIncludes=true`, `whichfileloaded=1`, `whyfileloaded=1` (twolevel already 1).
6. Do not change Classic `MacSurf` target.
7. Prefix stays `macsurf_prefix_osx.h` until Phase 9 (minimal native prefix).

## Runtime staging (on iMac)

`/Projects/MacSurfX-NativeRuntime/` holds the project-local J runtime inputs
with `PROVENANCE.txt` + `MD5SUMS` + `SHA256SUMS`:

- `libSystem.B.dylib` md5 `7cf8c04b026bf108a52c6297eb325abe` (thin PPC, LC_ID `/usr/lib/libSystem.B.dylib`)
- `libz.1.2.3.dylib` md5 `e36519762a4e40a1b36ff6917c1c2d40`
- `mwcrt1.o` md5 `6051166d58b5552a39fdc28765f19375`
- `libSystem-ppc.dylib.provenance-only` — audit copy, **not** for FILE/link order

Transfer into the live `MacSurfX` target is via Edit → MacSurfX Settings…
(GUI only; Classic `MacSurf` target must not change).
