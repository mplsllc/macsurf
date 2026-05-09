/*
 * MacSurf stub — nsutils/endian.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 *
 * Symbols stubbed:
 *   funcs: endian_host_is_le
 */

#ifndef NSUTILS_ENDIAN_H
#define NSUTILS_ENDIAN_H

/* true/false come from MacTypes.h as enum constants */
#ifndef bool
typedef unsigned char bool;
#endif

/*
 * Return non-zero if the host is little-endian.
 * PowerPC Mac is big-endian, so default to false.
 */
static bool endian_host_is_le(void)
{
    unsigned short x = 1;
    return *((unsigned char *)&x) != 0;
}

#endif /* NSUTILS_ENDIAN_H */
