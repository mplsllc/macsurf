/*
 * harness/include/macsurf_alloc_override.h
 *
 * S0 Linux+ASan harness shim. The real macos9 header redirects
 * malloc/calloc/realloc to macsurf_safe_alloc (the Mac safe allocator).
 * For the harness we want the REAL libc malloc so AddressSanitizer
 * tracks every allocation/free and traps the reconvert dom_string UAF
 * with clean stacks. So: NO redirect — just pull in the standard decls.
 */
#ifndef MACSURF_ALLOC_OVERRIDE_H
#define MACSURF_ALLOC_OVERRIDE_H
#include <stddef.h>
#include <stdlib.h>
#endif /* MACSURF_ALLOC_OVERRIDE_H */
