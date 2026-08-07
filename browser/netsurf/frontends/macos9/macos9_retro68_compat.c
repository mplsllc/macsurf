/*
 * macos9_retro68_compat.c — symbols the Retro68 link needs that the CFM/newlib
 * environment does not supply.
 *
 * WHY THIS FILE EXISTS, AND WHAT IT REPLACES
 *
 * tools/apply_retro68_fixes.sh used to GENERATE a file like this at build
 * time, with sed and python heredocs, and it had drifted: one of its stubs
 * redefined netsurf_version, which desktop/version.c already defines, so the
 * generated file could never have been in a link that succeeded. Everything
 * that build genuinely needs belongs in the tree, tracked, reviewable, and
 * compiled like any other source.
 *
 * The whole file is guarded on __RETRO68__ so it is inert under CodeWarrior,
 * whose own project supplies these from the Mac side. Nothing here can
 * collide with the shipping toolchain.
 *
 * Two distinct kinds of thing live below, and the difference matters:
 *
 *   1. REAL implementations. The POSIX directory calls are thin wrappers over
 *      shims/mac_dirent.c, which has always had working Carbon File Manager
 *      code -- it was simply never compiled here, because CMakeLists only
 *      globbed the top level of frontends/macos9 and never shims/. The old
 *      generated stubs returned NULL from opendir() unconditionally; these
 *      actually enumerate directories.
 *
 *   2. HONEST FAILURE stubs. The zlib entry points and the base64 URL decoder
 *      have no implementation anywhere in the Linux tree; on the CodeWarrior
 *      side they resolve against files that live only on the Mac. Rather than
 *      invent behaviour, each returns its API's error value, so the callers
 *      take their existing failure paths:
 *
 *        - zlib: reached only by utils/ns_hashtable.c's COMPRESSED Messages
 *          reader. MacSurf loads Messages uncompressed, so this path is not
 *          taken; if it ever is, gzopen() returning NULL is what that code
 *          already handles.
 *        - nsu_base64_decode_alloc_url: reached only by utils/ssl_certs.c,
 *          i.e. certificate detail display on about:certificate. Returning an
 *          error degrades that page; it does not affect TLS itself, which is
 *          macTLS and never goes near this.
 *
 *      Both are tracked gaps, not "done". Neither is on a page-rendering path.
 */

#ifdef __RETRO68__

#include <stddef.h>
#include <string.h>

#include "shims/mac_types.h"
#include "shims/stat.h"
#include <sys/stat.h>   /* mode_t, and newlib's own prototypes for the
                        * POSIX stubs below, so the signatures cannot drift */

/* ------------------------------------------------------------------------
 * POSIX directory enumeration, over the real Carbon implementation.
 *
 * shims/dirent.h declares the POSIX trio in terms of MAC_DIR (an opaque
 * struct _DIR) and struct dirent, while shims/mac_dirent.c implements the
 * same operations as mac_opendir/mac_readdir/mac_closedir over mac_DIR and
 * struct mac_dirent. Nothing ever bridged the two, so utils/utils.c's
 * readdir() call was an unresolved symbol. These are that bridge.
 *
 * struct dirent carries only d_name, so the mac_dirent d_type is dropped --
 * every caller in this tree filters by name.
 * --------------------------------------------------------------------- */

struct _DIR;   /* opaque to callers; really a mac_DIR */

extern mac_DIR *mac_opendir(const char *path);
extern struct mac_dirent *mac_readdir(mac_DIR *dir);
extern int mac_closedir(mac_DIR *dir);

struct dirent {
	char d_name[256];
};

struct _DIR *opendir(const char *path)
{
	return (struct _DIR *)mac_opendir(path);
}

struct dirent *readdir(struct _DIR *dir)
{
	static struct dirent ent;
	struct mac_dirent *m;

	m = mac_readdir((mac_DIR *)dir);
	if (m == NULL)
		return NULL;

	strncpy(ent.d_name, m->d_name, sizeof ent.d_name - 1);
	ent.d_name[sizeof ent.d_name - 1] = '\0';
	return &ent;
}

int closedir(struct _DIR *dir)
{
	return mac_closedir((mac_DIR *)dir);
}

/* utils/utils.c needs stat() by its POSIX name alongside the directory trio.
 * shims/mac_stat.c has the Carbon implementation under the mac_ prefix.
 *
 * All four have to be supplied together. Retro68's ld SEGFAULTS rather than
 * reporting an undefined symbol, and defining any one of them is enough to
 * pull utils.c's object back into the link, at which point the remaining
 * three go unresolved and ld dies with signal 11 and no diagnostic. That
 * failure mode is worth remembering: a linker crash here means a missing
 * symbol, not a corrupt object. */
extern int mac_stat(const char *path, struct stat *buf);

int stat(const char *path, struct stat *buf)
{
	return mac_stat(path, buf);
}

/* ------------------------------------------------------------------------
 * POSIX calls newlib declares but does not implement for this target.
 *
 * These are referenced from inside newlib itself (stdio, locale, tmpfile
 * paths) rather than from MacSurf. With -gc-sections they were discarded
 * along with their callers; without it they have to resolve, and Retro68's
 * ld segfaults rather than naming them.
 *
 * Classic Mac OS has no process working directory, no Unix permissions, no
 * symlinks and no signals, so each returns the failure its API defines. If
 * MacSurf ever genuinely needs one of these, it wants a Carbon File Manager
 * implementation in shims/, not a change here.
 * --------------------------------------------------------------------- */

int chdir(const char *path)                 { (void)path; return -1; }
char *getcwd(char *buf, size_t size)        { (void)buf; (void)size; return NULL; }
int mkdir(const char *path, mode_t mode)    { (void)path; (void)mode; return -1; }
int truncate(const char *path, long length) { (void)path; (void)length; return -1; }
int symlink(const char *t, const char *l)   { (void)t; (void)l; return -1; }
int readlink(const char *p, char *b, size_t s) { (void)p; (void)b; (void)s; return -1; }
long pathconf(const char *path, int name)   { (void)path; (void)name; return -1; }
int fchmod(int fd, mode_t mode)             { (void)fd; (void)mode; return -1; }
int fchmodat(int fd, const char *p, mode_t m, int f)
                                            { (void)fd; (void)p; (void)m; (void)f; return -1; }
int sigprocmask(int how, const void *set, void *old)
                                            { (void)how; (void)set; (void)old; return -1; }
int getentropy(void *buf, size_t len)       { (void)buf; (void)len; return -1; }

/* Cooperative multitasking: there is no preemption to sleep against, and
 * blocking the single thread is exactly what must not happen. Both are
 * no-ops that report success. */
unsigned int sleep(unsigned int seconds)    { (void)seconds; return 0; }
int usleep(unsigned int useconds)           { (void)useconds; return 0; }

/* ------------------------------------------------------------------------
 * zlib entry points — see the header comment. Honest failures only.
 * Signatures mirror shims/zlib.h, which declares but never defines them.
 * --------------------------------------------------------------------- */

#include "shims/zlib.h"

int inflateInit2_(z_stream *strm, int windowBits,
		  const char *version, int stream_size)
{
	(void)strm; (void)windowBits; (void)version; (void)stream_size;
	return -1;            /* Z_ERRNO: caller reports and bails */
}

int inflate(z_stream *strm, int flush)
{
	(void)strm; (void)flush;
	return -1;
}

int inflateEnd(z_stream *strm)
{
	(void)strm;
	return -1;
}

gzFile gzopen(const char *path, const char *mode)
{
	(void)path; (void)mode;
	return NULL;          /* the uncompressed path is the one MacSurf uses */
}

char *gzgets(gzFile file, char *buf, int len)
{
	(void)file; (void)buf; (void)len;
	return NULL;
}

int gzclose(gzFile file)
{
	(void)file;
	return -1;
}

/* ------------------------------------------------------------------------
 * nsu_base64_decode_alloc_url — see the header comment.
 * nsuerror's OK value is 0; any non-zero is an error to every caller.
 * --------------------------------------------------------------------- */

#include "nsutils/base64.h"

nsuerror nsu_base64_decode_alloc_url(const unsigned char *input,
				     size_t input_length,
				     unsigned char **output,
				     size_t *output_length)
{
	(void)input; (void)input_length;
	if (output != NULL)
		*output = NULL;
	if (output_length != NULL)
		*output_length = 0;
	return (nsuerror)1;
}

#endif /* __RETRO68__ */
