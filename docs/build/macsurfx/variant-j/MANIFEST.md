# Variant J evidence (known-good PowerPC Mach-O linker reference)

Date: 2026-09-22
Experiment: Variant J = base `Link-H-which-verified.xml` with a rename of the
project-visible `libSystem-ppc.dylib` filename to `libSystem.B.dylib` at five
XML PATH sites (FILE ×2, LINKORDER ×2, GROUPLIST ×1). File bytes unchanged.

## Rule (observed)

Under CW8's two-level Mach-O namespace, the project-visible dylib basename
must match the leaf of the dylib's `LC_ID_DYLIB` install name
(`/usr/lib/libSystem.B.dylib` → filename `libSystem.B.dylib`).

Two distinct findings:

1. Thin PPC compatibility copy for fat/universal import compatibility.
2. Install-name-leaf basename for two-level symbol identity.

Correct artifact: project-local thin PPC dylib named `libSystem.B.dylib`
whose `LC_ID_DYLIB` remains `/usr/lib/libSystem.B.dylib` (install name not
rewritten). Exact CW8 internal mechanism is inference; observed behavior is fact.

## Files

| Path | Role | md5 |
| --- | --- | --- |
| `SystemLibRef-PASS.xml` | Canonical export of the known-good project (alias of Link-J-rename-verified) | see below |
| `Link-J-rename.xml` | Intended J import XML | `dfdefa9e9a0887f81e52889a280054fc` |
| `Link-J-rename-verified.xml` | Export after J link pass | `782ab5877f672debaa5ae43ef05367a5` |
| `Link-H-which-verified.xml` | Base immediately before J rename | (H export) |
| `Link-J-rename.mcp` | CodeWarrior project after J | `7e16553459f3f63b7d8521e2771ff9ab` |
| `libSystem.B.dylib` | Thin PPC copy, LC_ID `/usr/lib/libSystem.B.dylib` — **private**: `macsurf-private/.private/research/macsurfx-link-20260922/dylibs/` | `7cf8c04b026bf108a52c6297eb325abe` |
| `libSystem-ppc.dylib` | Same bytes as above under old name — **private** same dir | `7cf8c04b026bf108a52c6297eb325abe` |
| `libz.1.2.3.dylib` | Thin libz used in link order — **private** same dir | `e36519762a4e40a1b36ff6917c1c2d40` |
| `mwcrt1.o` | Runtime startup object, first in link order — **private** same dir | `6051166d58b5552a39fdc28765f19375` |
| `SimpleHello-Debug` | Linked Mach-O ppc executable (PASS) | `9c9ccb0c95afbb7e963c05efa7a35052` |
| `link-{D,E,F,H,I}-errors*.txt` | Failed-variant diagnostics (4×348-byte undefineds) | |
| `tiger-otool-nm-evidence.txt` | otool/nm/file output from Tiger | |
| `MANIFEST.md` | This file | |

Large ktrace dumps `link-J-trace.txt` / `link-J2-trace.txt` are **not** in
public git; they live in `macsurf-private` under
`.private/research/macsurfx-link-20260922/`.

## Objective verification (Shipped — not hardware-verified)

- `file`: Mach-O executable ppc
- flags: NOUNDEFS DYLDLINK PREBOUND TWOLEVEL
- LC_LOAD_DYLIB: `/usr/lib/libSystem.B.dylib`, `/usr/lib/libz.1.dylib`,
  Carbon.framework
- nm -u still lists the six classic undefs; they are prebound at load
  (n_value nonzero, n_desc two-level). Link pass had 0 undefined.
- Machine-readable PASS recorded during link; maintainer hardware launch
  confirmation still required before calling this verified on the real case.

System dylibs / Metrowerks objects are kept in **macsurf-private**, not public git.
