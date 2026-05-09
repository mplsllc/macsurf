/*
 * MacSurf — macsurf_js_fatal.c
 *
 * Fatal error handler passed to duk_create_heap.  We do NOT exit or
 * abort: the engine invoking the fatal handler is trying to tell us
 * the heap is unusable, but we can still tear down the heap cleanly
 * and report the error to the page.  Calling exit() would kill the
 * whole browser over a single broken script, which is unacceptable
 * on OS 9 where a crashing app often takes the OS with it.
 *
 * Per Duktape's contract, a fatal handler MUST NOT return normally
 * to the Duktape engine.  We honour that by longjmp'ing to a recovery
 * jmp_buf installed by the caller.  If no recovery buffer is set yet,
 * we fall through to a tight sleep loop rather than returning — the
 * parent thread will notice the stuck state on the next pump and tear
 * the context down.
 */

#include <setjmp.h>
#include <stddef.h>

#include "utils/log.h"

#include "macsurf_js.h"

/*
 * Recovery jmp_buf.  Set by the thin wrapper that invokes Duktape
 * (e.g. around duk_peval).  If zeroed out we park instead of returning.
 */
static jmp_buf macsurf_js_recovery_jmp;
static int     macsurf_js_recovery_armed = 0;

void
macsurf_js_fatal_arm(void)
{
	macsurf_js_recovery_armed = 1;
}

void
macsurf_js_fatal_disarm(void)
{
	macsurf_js_recovery_armed = 0;
}

jmp_buf *
macsurf_js_fatal_jmp(void)
{
	return &macsurf_js_recovery_jmp;
}

void
macsurf_js_fatal(void *udata, const char *msg)
{
	(void)udata;
	nslog_log(__FILE__, "", __LINE__,
			"js fatal: %s",
			msg != NULL ? msg : "(null)");

	if (macsurf_js_recovery_armed != 0) {
		macsurf_js_recovery_armed = 0;
		longjmp(macsurf_js_recovery_jmp, 1);
	}

	/* No recovery buffer installed.  Duktape forbids returning, so
	 * spin until the parent thread notices and tears us down. */
	for (;;) {
		/* Intentionally empty — caller will abandon this thread. */
	}
}
