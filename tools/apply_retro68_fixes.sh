#!/bin/bash
#
# OBSOLETE — DO NOT RUN. Kept only as the record of what it used to do.
#
# Every step below has been folded into the tree, where it is tracked,
# reviewable and compiled like any other source. Running this now would
# re-apply sed/python rewrites on top of the real versions.
#
# Where each step went:
#   1  QuickJS generated headers        -> committed under browser/libquickjs/
#   2  resource.c escaped quotes        -> fixed in the file
#   3  macTLS Boolean guard             -> in the file
#   4  macTLS entropy Retro68 path      -> in the file; the __MWERKS__ gates
#                                          across macTLS/os9 are now
#                                          "Mac target, either toolchain"
#   5  generated POSIX/Toolbox stubs    -> macos9_retro68_compat.c.
#                                          NOTE: this step was BROKEN. It
#                                          redefined netsurf_version, which
#                                          desktop/version.c already defines,
#                                          so the file it generated could
#                                          never have been part of a link that
#                                          succeeded. Its opendir() also
#                                          returned NULL unconditionally,
#                                          papering over the fact that
#                                          shims/mac_dirent.c -- which has
#                                          real Carbon code -- was simply
#                                          never compiled.
#   6  fallthrough macro removal        -> macsurf_prefix.h
#   7  isascii macro removal            -> macsurf_prefix.h
#   8  SLEN + limits                    -> macsurf_prefix.h
#   9  Universal MacTypes include       -> macsurf_prefix.h
#  10  MacSurfIcon.r FREF/BNDL as data  -> NOT MIGRATED. This is the one
#                                          outstanding item; the built binary
#                                          currently carries no FREF or BNDL,
#                                          so the Finder icon does not bind.
#  11  utils/time.h symlink             -> committed as a symlink
#  12  iconv stubs                      -> macsurf_prefix.h
#
exit 1

set -e

echo "=== Applying Retro68 compatibility fixes ==="

# 1. QuickJS missing generated headers
for f in builtin-array-fromasync.h builtin-iterator-zip.h builtin-iterator-zip-keyed.h quickjs-c-atomics.h; do
  [ -f quickjs-macos9/$f ] && cp quickjs-macos9/$f browser/libquickjs/ && echo "  QuickJS: $f"
done

# 2. Fix resource.c escaped quotes
python3 -c "
f='browser/netsurf/content/fetchers/resource.c'
with open(f) as fh: c=fh.read()
c=c.replace(r'SLEN(\\\"If-None-Match:\\\")', 'SLEN(\"If-None-Match:\")')
with open(f,'w') as fh: fh.write(c)
" 2>/dev/null && echo "  resource.c: fixed escaped quotes"

# 3. Fix macTLS Boolean typedef (must be MWERKS-only)
for f in macTLS/os9/ostls_async.c macTLS/os9/ostls_d1_probe.c; do
  python3 -c "
with open('$f') as fh: c=fh.read()
# Only add guard if not already there (prevent double-nesting)
if 'typedef int Boolean;' in c and '#ifdef __MWERKS__\ntypedef int Boolean;' not in c:
    c=c.replace('typedef int Boolean;','#ifdef __MWERKS__\ntypedef int Boolean;\n#endif /* __MWERKS__ */')
    with open('$f','w') as fh: fh.write(c)
    print('  $f: guarded Boolean')
"
done

# 4. Fix macTLS entropy.c Retro68 path
python3 -c "
f='macTLS/os9/ostls_entropy.c'
with open(f) as fh: c=fh.read()
if '#elif defined(__RETRO68__)' not in c:
    c=c.replace('#else\n#include <stdint.h>',
        '#elif defined(__RETRO68__)\n#include <stdint.h>\n#include <Files.h>\n#include <Errors.h>\n#include <Script.h>\n#include <Folders.h>\n#else\n#include <stdint.h>')
    with open(f,'w') as fh: fh.write(c)
    print('  entropy.c: added Retro68 path')
"

# 5. Add Retro68 stubs (POSIX missing from newlib)
cat > browser/netsurf/frontends/macos9/macos9_retro68_stubs.c << 'EOF'
#include <sys/types.h>
void closedir(void *p) { (void)p; }
void *opendir(const char *n) { (void)n; return 0; }
const char *netsurf_version = "3.0";
int netsurf_version_major = 3;
int netsurf_version_minor = 0;
void TEInit(void) {}
void InitGraf(void *p) { (void)p; }
void InitFonts(void) {}
EOF
echo "  retro68_stubs: created"

# 6. Remove fallthrough macro (breaks utils.h __has_c_attribute)
sed -i '/^#define fallthrough do {} while(0)$/d' browser/netsurf/frontends/macos9/macsurf_prefix.h
echo "  prefix: removed conflicting fallthrough macro"

# 7. Remove isascii macro (conflicts with Universal MacTypes)
sed -i '/^#ifndef isascii$/,/^#endif$/d' browser/netsurf/frontends/macos9/macsurf_prefix.h
echo "  prefix: removed conflicting isascii macro"

# 8. Add SLEN and missing limits to prefix common section
grep -q 'define SLEN' browser/netsurf/frontends/macos9/macsurf_prefix.h || \
  sed -i '/^#define NOF_ELEMENTS/a #define SLEN(x) (sizeof((x)) - 1)' browser/netsurf/frontends/macos9/macsurf_prefix.h
grep -q 'define SIZE_MAX' browser/netsurf/frontends/macos9/macsurf_prefix.h || \
  sed -i '/^#define SLEN/a #ifndef SIZE_MAX\n#define SIZE_MAX ((size_t)-1)\n#endif\n#ifndef UINT32_MAX\n#define UINT32_MAX 0xffffffffUL\n#endif' browser/netsurf/frontends/macos9/macsurf_prefix.h
echo "  prefix: added SLEN + missing limits"

# 9. Add Universal MacTypes include for RETRO68
grep -q 'RETRO68.*MacTypes' browser/netsurf/frontends/macos9/macsurf_prefix.h || \
  python3 -c "
with open('browser/netsurf/frontends/macos9/macsurf_prefix.h') as fh: c=fh.read()
old='#endif\n\n#include <stddef.h>'
new='#endif\n\n  #ifdef __RETRO68__\n  #include \"/home/patrick/Retro68/toolchain/universal/CIncludes/MacTypes.h\"\n  #endif\n\n#include <stddef.h>'
c=c.replace(old,new)
with open('browser/netsurf/frontends/macos9/macsurf_prefix.h','w') as fh: fh.write(c)
print('  prefix: added Universal MacTypes for Retro68')
"

# 10. Fix MacSurfIcon.r FREF/BNDL as data blocks
python3 -c "
f='browser/netsurf/frontends/macos9/MacSurfIcon.r'
with open(f) as fh: c=fh.read()
old_fref = \"resource 'FREF' (128, \\\"MacSurf APPL\\\") {\n\t'APPL', 0, \\\"\\\"\n};\"
new_fref = \"data 'FREF' (128, \\\"MacSurf APPL\\\") {\n\t\\$\\\"4150504C 0000 00\\\"\n};\"
if old_fref in c:
    c=c.replace(old_fref, new_fref)
    with open(f,'w') as fh: fh.write(c)
    print('  MacSurfIcon.r: FREF as data block')
old_bndl = \"resource 'BNDL' (128, \\\"MacSurf BNDL\\\") {\n\t'MPLS', 0,\n\t{\n\t\t'ICN#', { 0, 128 },\n\t\t'FREF', { 0, 128 }\n\t}\n};\"
new_bndl = \"data 'BNDL' (128, \\\"MacSurf BNDL\\\") {\n\t\\$\\\"4D504C53\\\"\n\t\\$\\\"0000\\\"\n\t\\$\\\"0002\\\"\n\t\\$\\\"49434E23\\\"\n\t\\$\\\"0080\\\"\n\t\\$\\\"46524546\\\"\n\t\\$\\\"0080\\\"\n};\"
if old_bndl in c:
    c=c.replace(old_bndl, new_bndl)
    with open(f,'w') as fh: fh.write(c)
    print('  MacSurfIcon.r: BNDL as data block')
"

# 11. Create utils/time.h symlink
[ -L browser/netsurf/utils/time.h ] || ln -sf ns_time.h browser/netsurf/utils/time.h
echo "  utils/time.h: symlink to ns_time.h"

# 12. Add iconv stubs to prefix
grep -q 'iconv_open_' browser/netsurf/frontends/macos9/macsurf_prefix.h || \
  python3 -c "
with open('browser/netsurf/frontends/macos9/macsurf_prefix.h') as fh: c=fh.read()
stubs='''
#ifdef __RETRO68__
typedef void *iconv_t;
static iconv_t iconv_open_(const char *t, const char *f) { (void)t;(void)f; return (iconv_t)-1; }
static size_t iconv_(iconv_t cd, char **ib, size_t *il, char **ob, size_t *ol)
    { (void)cd;(void)ib;(void)il;(void)ob;(void)ol; return (size_t)-1; }
static int iconv_close_(iconv_t cd) { (void)cd; return -1; }
#define iconv_open iconv_open_
#define iconv iconv_
#define iconv_close iconv_close_
#endif
'''
c=c.replace('#include <stdint.h>\n#include <inttypes.h>', '#include <stdint.h>\n#include <inttypes.h>'+stubs)
with open('browser/netsurf/frontends/macos9/macsurf_prefix.h','w') as fh: fh.write(c)
print('  prefix: added iconv stubs')
"

echo "=== All Retro68 fixes applied ==="
