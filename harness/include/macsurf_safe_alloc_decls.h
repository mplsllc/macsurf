/* harness: force-included so every TU sees the REAL void*-returning
 * prototypes for macsurf_safe_alloc/calloc/realloc before first use.
 *
 * On the real Mac build, macsurf_prefix.h is injected before every
 * compilation unit (CW8 project prefix-file mechanism) and declares these.
 * The harness compiles files standalone without that injection, so a file
 * like macsurf_qjs.c that calls macsurf_safe_calloc() without including
 * macsurf_prefix.h/macsurf_alloc_override.h gets an IMPLICIT (int-returning)
 * declaration under -w -- on a 64-bit target that silently truncates every
 * returned pointer to 32 bits (0x515000002100 -> 0x2100), a real bug class
 * but one that cannot occur on the real Mac build. This header closes that
 * harness-only gap; it does not redirect malloc/calloc/realloc themselves. */
#ifndef MACSURF_SAFE_ALLOC_DECLS_H
#define MACSURF_SAFE_ALLOC_DECLS_H
#include <stddef.h>
extern void *macsurf_safe_alloc(size_t size);
extern void *macsurf_safe_calloc(size_t count, size_t size);
extern void *macsurf_safe_realloc(void *ptr, size_t size);
extern void *macsurf_try_alloc(size_t size);
extern void *macsurf_try_calloc(size_t count, size_t size);
extern void *macsurf_try_realloc(void *ptr, size_t size);
#endif

/* fixes1027 -- N_ELEMENTS, which the MAC BUILD GETS AND THE HARNESS DID NOT.
 *
 * macsurf_prefix.h:325 defines it for every Mac TU precisely because the
 * access path resolves "utils/utils.h" to NetSurf's copy, which lacks it.
 * The harness force-includes no prefix, so libcss sources that use
 * N_ELEMENTS without including libcss's own utils.h -- parse/language.c is
 * one -- compiled it as an IMPLICIT FUNCTION CALL under -w, linked with an
 * undefined symbol, and returned 0 at runtime.
 *
 * Consequence, and it is severe for a test harness: in
 * parseSelectorSpecific the guard is
 *     for (lut_idx = 0; lut_idx < N_ELEMENTS(pseudo_lut); lut_idx++)
 *     if (lut_idx == N_ELEMENTS(pseudo_lut)) return CSS_INVALID;
 * so with N_ELEMENTS == 0 the loop never ran and EVERY pseudo-class and
 * pseudo-element selector -- :hover, :link, :visited, :first-child,
 * ::before, ::after -- was rejected at parse time. The harness therefore
 * reported that MacSurf drops clearfix float containment when the Mac does
 * no such thing. A tool that silently disagrees with the target build is
 * worse than no tool. */
#ifndef N_ELEMENTS
#define N_ELEMENTS(x) (sizeof((x)) / sizeof((x)[0]))
#endif
#ifndef NOF_ELEMENTS
#define NOF_ELEMENTS(x) (sizeof((x)) / sizeof((x)[0]))
#endif
