/* harness stub: misc_stub.c's `#include "stat.h"` normally resolves to the
 * macos9 shim (frontends/macos9/shims/stat.h). On Linux, just pull in the
 * real POSIX struct stat so misc_stub.c's own stat() stub definition has a
 * matching prototype to compile against. */
#include <sys/stat.h>
