/* UnsignedWide for Retro68: needed by textarea.c and QuickJS cutils.h.
 * Frontend files get it from Carbon.h -> Universal MacTypes.h.
 * This header is for NetSurf core/library files that don't include Carbon. */
#ifdef __RETRO68__
#ifndef __MACTYPES__
struct UnsignedWide { unsigned long hi; unsigned long lo; };
typedef struct UnsignedWide UnsignedWide;
#endif
extern void Microseconds(UnsignedWide *tickCount);
#endif
