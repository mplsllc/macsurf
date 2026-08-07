/* MacSurfSize.r — partition + code-fragment overrides for the Retro68 build.
 *
 * Both resources here REPLACE ones that Retro68's RetroCarbonAPPL.r template
 * emits with unusable defaults.  Overriding works because add_application()
 * passes each project .r file's compiled .rsrc.bin AFTER the template on the
 * final Rez command line (and deliberately does NOT use --copy, which would
 * reorder them).  This file must therefore stay LAST in the add_application()
 * argument list.
 *
 * Sizes are in BYTES.
 */
#include "Types.r"
#include "CodeFragments.r"

/* ---------------------------------------------------------------------------
 * 'SIZE' (-1) — application partition.  Template default is 1 MB / 1 MB.
 *
 * Matched to the CodeWarrior project (MWProject_PPC_size / _minsize), which
 * ships 224 MB preferred / 128 MB minimum.  These are not vanity numbers:
 * CLAUDE.md records 16 MB as the FLOOR below which libcss starves mid-cascade
 * and css_select_style starts returning CSS_NOMEM on a moderately sized page.
 * The previous 64/16 put the minimum exactly on that floor.
 *
 * On a RAM-tight Mac the preferred size is what the user lowers; leave the
 * minimum where the engine actually survives.
 * ------------------------------------------------------------------------ */
resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    doesActivateOnFGSwitch,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    isHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    reserved,
    reserved,
    reserved,
    224 * 1024 * 1024,   /* preferred: 224 MB */
    128 * 1024 * 1024    /* minimum:  128 MB */
};

/* ---------------------------------------------------------------------------
 * 'cfrg' (0) — code fragment descriptor.
 *
 * THE POINT OF THIS OVERRIDE IS appStackSize.
 *
 * RetroCarbonAPPL.r emits kDefaultStackSize, which Multiverse.r defines as 0
 * — "let CFM pick" — and CFM's pick for a PowerPC application is a few tens of
 * KB.  The CodeWarrior project sets a 16 MB stack (MWProject_PPC_stacksize).
 *
 * That gap is not cosmetic.  libcss's cascade, libdom's tree walks, NetSurf's
 * layout recursion and QuickJS evaluation all recurse deeply; on a default CFM
 * stack they smash it almost immediately.  The observed Retro68 symptom fits
 * exactly: CFM loads, the CRT runs, main() is reached, a handful of shallow
 * init calls survive, and the binary dies the moment it enters anything that
 * recurses.
 *
 * Every other field is copied verbatim from the template so this stays a
 * drop-in replacement.  CFRAG_NAME is supplied by add_application() via
 * -DCFRAG_NAME="MacSurf"; the #ifndef mirrors the template so the file can
 * also be Rez'd standalone.
 * ------------------------------------------------------------------------ */
#ifndef CFRAG_NAME
#define CFRAG_NAME "MacSurf"
#endif

resource 'cfrg' (0) {
    {
        kPowerPCCFragArch, kIsCompleteCFrag, kNoVersionNum, kNoVersionNum,
        16 * 1024 * 1024,        /* appStackSize: 16 MB, matches CW8 */
        kNoAppSubFolder,
        kApplicationCFrag, kDataForkCFragLocator, kZeroOffset, kCFragGoesToEOF,
        CFRAG_NAME
    }
};
