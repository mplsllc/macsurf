/* MacSurf Retro68 SIZE resource — overrides Retro68 template default (1 MB).
 * Must be listed AFTER RetroCarbonAPPL.r in the Rez command line so it wins.
 * Sizes are in BYTES. */
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
    64 * 1024 * 1024,    /* preferred: 64 MB */
    16 * 1024 * 1024     /* minimum:  16 MB */
};
