/*
 * MacSurf shim - <time.h>
 *
 * Top-level interceptor for `#include <time.h>`. The `macos9:` directory
 * is on the CW8 access path (the `macos9:sys:` sub-directory is NOT), so
 * a bare `<time.h>` was falling through to MSL's own <time.h>, whose
 * `ctime`/struct-tm definition collides with the one macsurf_prefix.h
 * already provides (guarded by _STRUCT_TM) - yielding
 * "struct/union/enum/class tag 'tm' redefined" in every core file that
 * includes <time.h> (browser_history.c, global_history.c, cookie_manager.c,
 * urldb.c, frames.c, fs_backing_store.c, event.c, fetch.c ...).
 *
 * This file is C89 and pulls in NO MSL headers. struct tm / time_t are
 * already defined by macsurf_prefix.h under _STRUCT_TM / _TIME_T, so the
 * blocks below are fall-backs only (skipped when the prefix ran first).
 */

#ifndef MACOS9_TIME_H
#define MACOS9_TIME_H

/* Claim MSL's guard too, so if MSL's <time.h> is ever reached via a
 * different path it does not re-define struct tm on top of ours. */
#ifndef _TIME_H
#define _TIME_H
#endif

#include <stddef.h>   /* size_t */

#ifndef _TIME_T
#define _TIME_T
typedef long time_t;
#endif

#ifndef _CLOCK_T
#define _CLOCK_T
typedef long clock_t;
#endif

#ifndef CLOCKS_PER_SEC
#define CLOCKS_PER_SEC 60
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
extern time_t     mktime(struct tm *tmptr);
extern time_t     time(time_t *timer);
extern char      *ctime(const time_t *timer);
extern char      *asctime(const struct tm *tmptr);
extern double     difftime(time_t end, time_t start);
extern clock_t    clock(void);
extern size_t     strftime(char *s, size_t maxsize,
                           const char *format, const struct tm *tmptr);

#endif /* MACOS9_TIME_H */
