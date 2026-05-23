# MacSurf Mac OS resource fork

How the MacSurf application's Classic Mac OS resource fork is sourced,
generated, and wired into the build.

## Files

| Path | Role |
|---|---|
| [puffpuff.png](../puffpuff.png) | Source 512×512 RGBA icon artwork |
| [tools/png_to_mac_icon_rez.py](../tools/png_to_mac_icon_rez.py) | PNG → Mac icon family generator (Rez text **and** binary `.rsrc`) |
| [browser/netsurf/frontends/macos9/MacSurf.r](../browser/netsurf/frontends/macos9/MacSurf.r) | Canonical Rez source — declares `'carb'` and `#include`s `MacSurfIcon.r` |
| [browser/netsurf/frontends/macos9/MacSurfIcon.r](../browser/netsurf/frontends/macos9/MacSurfIcon.r) | Generated Rez source: ICN#/icl4/icl8/ics#/ics4/ics8 + FREF + BNDL |
| [browser/netsurf/frontends/macos9/MacSurf.rsrc](../browser/netsurf/frontends/macos9/MacSurf.rsrc) | Pre-built binary resource fork — what CW8 links into the binary when the Rez step is skipped |

## Resources in the fork

| Type | ID | Size | Purpose |
|---|---:|---:|---|
| `carb` | 0 | 0 | CarbonLib loader marker (mandatory — without it `*InContext` OT calls crash) |
| `ICN#` | 128 | 256 | 32×32 1-bit icon + 1-bit mask |
| `icl4` | 128 | 512 | 32×32 4-bit colour icon |
| `icl8` | 128 | 1024 | 32×32 8-bit colour icon |
| `ics#` | 128 | 64 | 16×16 1-bit small icon + mask |
| `ics4` | 128 | 128 | 16×16 4-bit small colour |
| `ics8` | 128 | 256 | 16×16 8-bit small colour |
| `FREF` | 128 | 7 | File type `'APPL'` → icon local-ID 0 |
| `BNDL` | 128 | 28 | Creator `'MPLS'` binding `ICN#` 128 + `FREF` 128 |

**Creator code is uppercase `'MPLS'`.** Classic Mac type and creator
codes are case-sensitive; do not change this to `'mpls'` or similar.

## Regenerating from a new PNG

```bash
python3 tools/png_to_mac_icon_rez.py \
    --src puffpuff.png \
    --rez browser/netsurf/frontends/macos9/MacSurfIcon.r \
    --rsrc browser/netsurf/frontends/macos9/MacSurf.rsrc \
    --icon-id 128 \
    --creator MPLS \
    --file-type APPL
```

The script needs Pillow (`pip install Pillow`).

It writes **both** the Rez text (humans + Rez-based builds read this)
and the binary `.rsrc` (CW8 links this directly when it can't run Rez).
Run it whenever `puffpuff.png` is edited.

## What it produces, internally

* Resizes the source artwork to 32×32 and 16×16 with Lanczos.
* Composites against white for the colour icon body; the alpha channel
  becomes a separate 1-bit mask. Mac icon resources have no alpha
  channel — Finder uses the mask to control transparency.
* Quantises to the Apple Mac System 8-bit palette (6×6×6 RGB cube +
  red/green/blue/gray 10-step ramps) for `icl8`/`ics8`.
* Quantises to the Apple Mac System 4-bit palette (16 specific
  colours: white, yellow, orange, red, magenta, purple, blue, cyan,
  green, dark-green, brown, tan, light-gray, medium-gray, dark-gray,
  black) for `icl4`/`ics4`.
* Threshold-converts the rendered icon to 1-bit for `ICN#` / `ics#`,
  ANDed with the alpha mask so transparent areas stay clear.

## Build paths

### Mac side (CodeWarrior 8 Pro with Rez)

`MacSurf.r` is the source of truth. Adding the file to the CW8 project
should trigger Rez at build time and produce a fresh resource fork.

If the CW8 project has the binary `MacSurf.rsrc` listed and Rez
disabled in File Mappings, the binary fork shipped in-repo is what
gets linked. Both files in-repo are kept in sync by the script
above.

### Linux cross-build

The Linux side does not run Rez. The pre-built `MacSurf.rsrc` in the
repo is the artifact CW8 will pick up over scp. Run the generator
whenever the source PNG changes.

## Verifying the icon takes effect on real hardware

1. Build / scp the binary so the app's resource fork contains the
   icon resources.
2. Boot OS 9 holding **Command + Option** until Finder offers to
   rebuild the desktop database. Confirm.
3. Locate MacSurf in Finder — it should display the puffpuff icon
   at both 32×32 and 16×16 view sizes.
4. *Get Info* on the application: **Kind: application program**;
   **Created by: MPLS**.

If the icon still shows the generic application icon after a desktop
rebuild, check that:

- `BNDL` (128) is present in the fork (use ResEdit or Resorcerer).
- `FREF` (128) maps to file type `'APPL'`.
- The application's file-type/creator on disk is `APPL` / `MPLS` —
  this is set when the binary is written, separately from the
  resource fork. Tools like `SetFile -c MPLS -t APPL MacSurf` (under
  Mac OS X with Developer Tools, or equivalent inside OS 9) can
  correct it post-hoc; CodeWarrior 8 sets it at build time from
  project settings.

## Background

* The `'carb'` resource is what tells CFM "this fragment is a
  Carbon app". CarbonLib reads only the resource's *presence*, not
  its content; it is intentionally zero bytes.
* `FREF` (File REFerence) describes a single file type the app can
  open / claim. Each FREF lives at a resource ID and is referenced
  from its application's `BNDL` by a **local** ID — an opaque
  small integer that BNDL also pairs with the real resource ID.
  The convention is to use local-ID 0 for the application's own
  `'APPL'` reference.
* `BNDL` (BuNDLe) is the desktop-database glue. Finder reads it
  during a desktop rebuild and records "creator MPLS → icon 128,
  app file-type APPL". After that, every file with creator 'MPLS'
  and matching type gets the right icon without further lookup.
