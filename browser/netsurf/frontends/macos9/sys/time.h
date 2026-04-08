/*
 * MacSurf stub — sys/time.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 *
 * Symbols stubbed:
 *   types:  struct timeval
 *   funcs:  gettimeofday
 *   macros: timeradd, timersub, timercmp, timerclear, timerisset
 */

#ifndef MACOS9_SYS_TIME_H
#define MACOS9_SYS_TIME_H

struct timeval {
    long tv_sec;
    long tv_usec;
};

extern int gettimeofday(struct timeval *tv, void *tz);

#ifndef timeradd
#define timeradd(a, b, result) do {                         \
    (result)->tv_sec  = (a)->tv_sec  + (b)->tv_sec;        \
    (result)->tv_usec = (a)->tv_usec + (b)->tv_usec;       \
    if ((result)->tv_usec >= 1000000L) {                    \
        ++(result)->tv_sec;                                 \
        (result)->tv_usec -= 1000000L;                      \
    }                                                       \
} while (0)
#endif

#ifndef timersub
#define timersub(a, b, result) do {                         \
    (result)->tv_sec  = (a)->tv_sec  - (b)->tv_sec;        \
    (result)->tv_usec = (a)->tv_usec - (b)->tv_usec;       \
    if ((result)->tv_usec < 0) {                            \
        --(result)->tv_sec;                                 \
        (result)->tv_usec += 1000000L;                      \
    }                                                       \
} while (0)
#endif

#ifndef timercmp
#define timercmp(a, b, CMP) \
    (((a)->tv_sec == (b)->tv_sec)                           \
     ? ((a)->tv_usec CMP (b)->tv_usec)                     \
     : ((a)->tv_sec CMP (b)->tv_sec))
#endif

#ifndef timerclear
#define timerclear(tvp) ((tvp)->tv_sec = (tvp)->tv_usec = 0)
#endif

#ifndef timerisset
#define timerisset(tvp) ((tvp)->tv_sec || (tvp)->tv_usec)
#endif

#endif /* MACOS9_SYS_TIME_H */
