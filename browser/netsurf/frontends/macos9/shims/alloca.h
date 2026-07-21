/*
 * MacSurf stub -- shims/alloca.h
 * Stack allocation for CodeWarrior 8 / Mac OS.
 *
 * <alloca.h> is a GNU/Solaris-ism. Metrowerks MSL ships no such header, so
 * every translation unit that reaches an `#include <alloca.h>` fails with
 * "the file 'alloca.h' cannot be opened" -- and because the include failed,
 * alloca() is then undeclared, so C89's implicit-int rule makes it return
 * `int` and every call site dies a second time with
 * "illegal implicit conversion from 'int' to '<type> *'".
 *
 * Two headers in the QuickJS engine trigger this:
 *   browser/libquickjs/cutils.h:269   (reached by quickjs.c AND libregexp.c)
 *   browser/libquickjs/libregexp.c:31
 * and the four call sites that then fail to compile are
 *   quickjs.c:17420  arg_buf   = alloca(sizeof(arg_buf[0]) * arg_count)
 *   quickjs.c:17531  arg_buf   = alloca(sizeof(JSValue) * arg_count)
 *   quickjs.c:17676  local_buf = alloca(alloca_size)
 *   libregexp.c:2524 stack_buf = alloca(alloca_size)
 *
 * cutils.h already documents the intended contract:
 *
 *     "alloca: CW8 exposes __alloca as a compiler built-in via <alloca.h>.
 *      Include it; its #define alloca(x) __alloca(x) is exactly what we need."
 *
 * That is precisely what this file supplies. The declaration matches the
 * in-tree precedent in quickjs-macos9/QuickJSPrefix.h, which does the same
 * __alloca mapping for the same compiler.
 *
 * NOTE ON THE RISK: if this CodeWarrior install has no __alloca in its PPC
 * runtime either, the failure moves to a LINK error naming __alloca
 * specifically -- unambiguous, and not a silent miscompile. The fallback in
 * that case is to remove alloca from the four call sites above rather than to
 * emulate it: a malloc-based alloca would leak, because alloca memory is
 * reclaimed by the function epilogue and none of those sites free anything.
 * (libregexp.c's is trivially convertible -- its size is bounded by
 * STACK_SIZE_MAX=255, i.e. 1020 bytes on PPC32, so a plain local array works.
 * The three quickjs.c sites sit in JS_CallInternal and need more care.)
 *
 * C89, CW8-clean. Licensed under GPL v2.
 */

#ifndef MACOS9_SHIMS_ALLOCA_H
#define MACOS9_SHIMS_ALLOCA_H

#ifdef __MWERKS__

/* CodeWarrior PPC. __alloca adjusts the stack frame in the caller, so it must
 * be a compiler-known name rather than an ordinary library call; declaring it
 * keeps C89 from defaulting the return type to int. */
extern void *__alloca(unsigned int size);

#ifdef alloca
#undef alloca
#endif
#define alloca(x) ((void *)__alloca((unsigned int)(x)))

#else

/* Linux syntax-check / harness build. GCC and Clang both provide the
 * builtin, so no libc header is needed and this file stays self-contained
 * even though it shadows the system <alloca.h> on the shim include path. */
#ifdef alloca
#undef alloca
#endif
#define alloca(x) __builtin_alloca(x)

#endif /* __MWERKS__ */

#endif /* MACOS9_SHIMS_ALLOCA_H */
