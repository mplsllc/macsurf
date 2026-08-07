/*
 * ostls_log.h
 *
 * File-backed logger for MacTLSTest. Writes one CR-terminated line per
 * call to "MacTLSTest.log" on the Desktop. After each line, the file
 * mark is updated and the volume is flushed so a crash or hang leaves
 * the latest entry on disk.
 *
 * Same pattern MacSurf uses for its in-browser diagnostic channel.
 * Hand-rolled formatter rather than MSL's vsnprintf because CW8
 * Carbon MSL's vsnprintf has been unreliable in MacSurf's experience
 * and we don't need the full spec -- only:
 *
 *   %s    NUL-terminated string
 *   %d    int  (signed decimal)
 *   %ld   long (signed decimal)
 *   %u    unsigned int
 *   %lu   unsigned long
 *   %x    unsigned int  (lowercase hex)
 *   %X    unsigned int  (uppercase hex, no zero-pad)
 *   %04X  unsigned int  (uppercase hex, zero-pad to 4)
 *   %p    pointer       (uppercase hex)
 *   %c    char
 *   %%    literal '%'
 *
 * On the OS 9 side after a MacTLSTest run, the log can be pulled
 * back to Linux via the existing scp tunnel:
 *
 *   scp -P 2222 -i ~/.ssh/macsurf_push \
 *       patrick@localhost:Desktop/MacTLSTest.log /tmp/
 */

#ifndef OSTLS_LOG_H
#define OSTLS_LOG_H

#if defined(__MWERKS__) || defined(__RETRO68__)  /* Mac target, either toolchain */
#include <Types.h>
#else
#ifndef OSTLS_OSERR_DEFINED
#define OSTLS_OSERR_DEFINED
typedef short OSErr;
#endif
#endif

#include <stddef.h>     /* size_t */

/*
 * Initialise (or re-initialise) the log. Creates / truncates
 * MacTLSTest.log on the Desktop. Returns noErr on success; on any
 * file-manager failure the function is a no-op for the rest of the
 * run -- subsequent OSTLS_Log* calls become silent. The probe code
 * NEVER fails because the log can't be opened, so this lets the
 * actual TLS work proceed on a Mac whose Desktop folder is unusual
 * or whose volume is read-only.
 */
OSErr OSTLS_LogInit(void);

/*
 * Close the log cleanly. Safe to call even if init failed.
 */
void OSTLS_LogClose(void);

/*
 * Write one line of literal text plus a trailing CR. NUL-terminated
 * input. Truncates at ~250 bytes per line.
 */
void OSTLS_LogLine(const char *s);

/*
 * Printf-like (subset above). Bounded internally to ~250 bytes per
 * line; longer formatted output is truncated.
 */
void OSTLS_LogLinef(const char *fmt, ...);

/*
 * Flush a blank line. Useful between sections in the log.
 */
void OSTLS_LogBlank(void);

#endif /* OSTLS_LOG_H */
