/*
 * macLink.r — Rez source for macLink's required resources
 *
 * CW8 compiles this at build time via the Rez tool and writes the
 * output into the application's resource fork. Mirrors MacSurf's
 * approach (browser/netsurf/frontends/macos9/MacSurf.r).
 *
 * Source-controllable; editable in any text editor; no fork drama.
 *
 * Required for an FBA Carbon app on OS 9:
 *   - 'carb' resource (CarbonLib loader marker)
 *   - 'SIZE' resource (heap sizes + Background-Only bit)
 *
 * Optional, can ship without for Phase 0:
 *   - 'vers' (version info shown in Get Info)
 *   - icon family + FREF + BNDL (custom Finder icon)
 */

#include "Types.r"
#include "SysTypes.r"


/* -----------------------------------------------------------------
 *  'carb' resource — CarbonLib loader marker
 *
 *  Without this, the binary is not recognized as a Carbon fragment,
 *  CarbonLib does not load as a dependency, and any *InContext OT
 *  call crashes inside OTClientLib. See MacSurf's CLAUDE.md "Carbon
 *  App Requirements" section for the full pathology.
 *
 *  Contents are unread — presence is what matters. We give it one
 *  byte of zero for completeness.
 * --------------------------------------------------------------- */

data 'carb' (0, "carb-loader-marker") {
    $"00"
};


/* -----------------------------------------------------------------
 *  'SIZE' (-1) — heap sizes + Process Manager flags
 *
 *  Heap: 8 MB preferred / 4 MB minimum. Sufficient for an FBA that
 *  hosts macTLS + macEntropy + a handful of cert-cache slots. Bumped
 *  later if leaf-cert minting under sustained load grows beyond
 *  this budget; revisit in Phase 2 measurement.
 *
 *  Flags:
 *    - acceptSuspendResumeEvents: standard cooperative behaviour
 *    - canBackground: we run while in the background
 *    - multiFinderAware: we coexist with other apps cleanly
 *    - onlyBackground: THIS IS THE FBA BIT — no menu bar, no
 *                      Application menu entry, no Dock equivalent.
 *                      Finder treats macLink as background-only.
 *    - is32BitCompatible: required on PPC
 *    - isHighLevelEventAware: required so AppleEvent 'quit' reaches
 *                             us at system shutdown (mlink_main.c
 *                             installs the kAEQuitApplication handler)
 * --------------------------------------------------------------- */

resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    multiFinderAware,
    onlyBackground,                /* the FBA bit                  */
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    isHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    notDisplayManagerAware,
    reserved,
    reserved,
    8388608,                       /* preferred: 8 MB              */
    4194304                        /* minimum:   4 MB              */
};


/* -----------------------------------------------------------------
 *  'vers' (1) — version info shown in Get Info
 *
 *  Shows up as the "Version" string in the Finder Get Info window.
 *  Optional for Phase 0; included so test builds are uniquely
 *  identifiable when more than one macLink build is on the disk.
 * --------------------------------------------------------------- */

resource 'vers' (1, "version-info") {
    0x00, 0x01,                    /* major / minor BCD            */
    development,                   /* release stage                */
    0x00,                          /* prerelease                   */
    verUS,                         /* region                       */
    "0.0.1-phase0",                /* short version                */
    "macLink 0.0.1-phase0 (Phase 0 FBA test build)"
};
