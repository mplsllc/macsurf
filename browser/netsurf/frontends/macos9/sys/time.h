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

/* CW8's access path includes macos9:sys: so <time.h> finds THIS file
 * before MSL's time.h — the previous #include <time.h> was circular
 * (guarded out), so struct tm and localtime were never declared.
 * Provide the necessary declarations directly. Guards prevent conflicts
 * if MSL time.h is somehow reached via a later include. */
#ifndef _TIME_T
#define _TIME_T
typedef long time_t;
#endif

#ifndef _STRUCT_TM
#define _STRUCT_TM
struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};
#endif

extern struct tm *localtime(const time_t *timer);
extern struct tm *gmtime(const time_t *timer);
extern time_t    mktime(struct tm *tm);
extern time_t    time(time_t *timer);
extern char     *ctime(const time_t *timer);
extern size_t    strftime(char *s, size_t max, const char *fmt, const struct tm *tm);

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
