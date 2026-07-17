#ifndef MACSURF_TIMEBASE_H
#define MACSURF_TIMEBASE_H
double macsurf_qjs_get_now(void);      /* ms monotonic, defined in harness_stubs.c */
unsigned long macsurf_get_ticks(void);

/* fixes872: the REAL frontend header (frontends/macos9/macsurf_timebase.h:71)
 * declares this, and macsurf_qjs.c calls it from macsurf_qjs_get_now(). This
 * stub shadows that header for the harness build, and omitting the declaration
 * was NOT harmless: with no prototype in scope C89 implicitly declares the
 * function as returning INT, and on PPC a double comes back in f1 while an int
 * comes back in r3 -- so the harness was quietly modelling a DIFFERENT, broken
 * performance.now() than the Mac build actually has.
 *
 * Rule: a harness stub must never be more permissive than the real header it
 * stands in for. That turns the harness into a liar in the one direction that
 * hides bugs. (Masked until now because the harness compiles with -w.) */
double macsurf_monotonic_ms(void);
#endif
