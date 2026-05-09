/*
 * MacSurf - macos9_fetcher_init.c
 *
 * Replaces the misc_stub.c::fetcher_init() no-op with a real init that
 * registers our HTTP fetcher and nothing else. v0.2 ships as a
 * 1-fetcher build (HTTP only via Open Transport). Other URL schemes
 * (data:, file:, about:) return "no fetcher" errors at hlcache time —
 * fine for v0.2.
 *
 * NetSurf core's content/fetch.c::fetcher_init() upstream tries to
 * register data/file/about/curl fetchers; we replace it entirely with
 * this thin shim. The corresponding stub function in misc_stub.c is
 * removed so the linker picks up this real symbol.
 */

#include "utils/errors.h"

extern nserror macos9_http_fetcher_register(void);

nserror fetcher_init(void)
{
	return macos9_http_fetcher_register();
}
