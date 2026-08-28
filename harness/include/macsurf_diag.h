/*
 * MacSurf  -  macsurf_diag.h
 *
 * MacSurf Trace: the one MacSurf-owned diagnostic state boundary that the
 * `MSdg`/`GET ` AppleEvent class serialises on demand.
 *
 * Model:
 *   hot path        -> integer counters only (macsurf_diag_request_seen,
 *                      macsurf_gap_hit)
 *   NAV: DONE        -> freeze in-progress counters into a last-nav snapshot
 *                      (macsurf_diag_nav_done, macsurf_gap_emit_summary)
 *   AppleEvent query -> serialise the FROZEN snapshot into text, later, with
 *                      no traversal of hlcache / the fetch ring / windows and
 *                      no formatting on any request hot path
 *
 * Reads are non-destructive: repeated `MSdg GET summary` return the same block
 * until the next navigation completes.
 */

#ifndef MACSURF_DIAG_H
#define MACSURF_DIAG_H

/* Per-navigation wire-request tally. Called once per request at its terminal
 * state by the macos9 fetchers. `failed` != 0 for a network-level failure or
 * an HTTP >= 400. Cheap (1-2 int adds); nav_id is informational for now. */
void macsurf_diag_request_seen(unsigned long nav_id, int failed);

/* Called at NAV: DONE, AFTER macsurf_gap_emit_summary(). Freezes the
 * in-progress request tally into the last-nav snapshot and clears it. */
void macsurf_diag_nav_done(unsigned long nav_id);

/* On-demand serialisers for `MSdg GET summary` / `MSdg GET gaps`. Write a
 * versioned machine-oriented block (NUL-terminated) into buf and return its
 * length excluding the NUL, or 0 on bad args. Never mutate state.
 *
 *   MSDIAG 1 summary            MSDIAG 1 gaps
 *   nav=<n>                     nav=<n>
 *   requests=<n>               <slug>=<count>        (only non-zero, high first)
 *   failures=<n>               ...
 *   gaps_unique=<n>
 *   gaps_total=<n>
 */
long macsurf_diag_serialize_summary(char *buf, long cap);
long macsurf_diag_serialize_gaps(char *buf, long cap);

#endif /* MACSURF_DIAG_H */
