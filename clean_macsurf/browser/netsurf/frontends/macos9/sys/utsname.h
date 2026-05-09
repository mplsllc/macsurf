/*
 * MacSurf stub -- sys/utsname.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 */

#ifndef MACOS9_SYS_UTSNAME_H
#define MACOS9_SYS_UTSNAME_H

struct utsname {
	char sysname[32];
	char nodename[32];
	char release[32];
	char version[32];
	char machine[32];
};

static int uname(struct utsname *buf)
{
	if (buf != NULL) {
		buf->sysname[0] = '\0';
		buf->nodename[0] = '\0';
		buf->release[0] = '\0';
		buf->version[0] = '\0';
		buf->machine[0] = '\0';
	}
	return 0;
}

#endif /* MACOS9_SYS_UTSNAME_H */
