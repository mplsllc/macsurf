/*
 * mlink_log.h — macLink diagnostic logging
 *
 * Thin wrapper around macTLS's ostls_log.c so macLink-Proxy and the
 * Control Panel share one channel. File-backed, flushed after every
 * write (like MacSurf's MS_LOG), so survives crashes.
 *
 * On a stock install the log file lives on the Desktop as
 * "macLink Debug.log". Lines are CR-terminated (Classic Mac OS
 * convention). Prefix every line with "[mlink] " so it interleaves
 * cleanly with MacSurf and macTLS lines in shared diagnostic runs.
 */
#ifndef MLINK_LOG_H
#define MLINK_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the log file. Idempotent. Returns 0 on success. */
int mlink_log_init(void);

/* Close the log file. Idempotent. */
void mlink_log_close(void);

/* Write a literal line. Adds the "[mlink] " prefix and a trailing CR. */
void mlink_log(const char *s);

/* Formatted-print variant. Supports the same restricted format set as
 * MacSurf's macsurf_debug_log_writef: %d, %ld, %p, %s, %%. Anything
 * else prints literally without consuming the argument and may scramble
 * subsequent %s reads — restrict to the supported set. */
void mlink_logf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* MLINK_LOG_H */
