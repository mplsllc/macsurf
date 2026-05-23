/* -----------------------------------------------------------------------
 * MacSurf.r — Rez source for the MacSurf Classic Mac OS application
 *
 * What this file describes (in order):
 *   'carb' (0)       — CarbonLib loader marker. Required, even though the
 *                      resource itself carries zero bytes. Without it CFM
 *                      treats the binary as classic PEF and CarbonLib
 *                      never loads, so any *InContext OT call crashes.
 *
 *   icon family at ID 128
 *     ICN# (128)     — 32x32 1-bit icon + 1-bit mask
 *     icl4 (128)     — 32x32 4-bit colour
 *     icl8 (128)     — 32x32 8-bit colour
 *     ics# (128)     — 16x16 1-bit small icon + mask
 *     ics4 (128)     — 16x16 4-bit small colour
 *     ics8 (128)     — 16x16 8-bit small colour
 *
 *   FREF (128)       — file type 'APPL' mapped to icon local-ID 0
 *   BNDL (128)       — creator 'MPLS' binding ICN# 128 + FREF 128
 *
 * Build paths:
 *   * Mac side (Rez under CodeWarrior 8): MacSurf.r is the canonical
 *     source.  This file #includes MacSurfIcon.r which carries the
 *     data blocks generated from puffpuff.png.
 *   * Linux cross-build: tools/png_to_mac_icon_rez.py emits a binary
 *     MacSurf.rsrc fork directly. Both paths produce the same set of
 *     resources; keep MacSurf.r and MacSurf.rsrc in sync by running
 *     the script after editing puffpuff.png.
 *
 * Creator code is uppercase 'MPLS'. Classic Mac type/creator codes
 * are case-sensitive; do not change this to 'mpls' or similar.
 * ----------------------------------------------------------------------- */

#include "Types.r"

/* 'carb' resource type. The CarbonLib loader looks up this type by
 * name only; the resource has no payload.  Keeping the type local
 * (rather than relying on a system .r) avoids a build dependency on
 * CarbonAccessors.r / SysTypes.r in the CW8 install. */
type 'carb' {};

resource 'carb' (0) {};

/* Icon family + FREF + BNDL. Generated; do not hand-edit. */
#include "MacSurfIcon.r"
