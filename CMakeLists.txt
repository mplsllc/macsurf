cmake_minimum_required(VERSION 3.9)
project(MacSurf C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(UNIVERSAL_CINCLUDES /home/patrick/Retro68/toolchain/universal/CIncludes)
set(RETRO68_RINCLUDES /home/patrick/Retro68/toolchain/powerpc-apple-macos/RIncludes)
set(MULTIVERSAL_INCLUDES /home/patrick/Retro68/toolchain/powerpc-apple-macos/include)
set(MACSURF_ROOT ${CMAKE_SOURCE_DIR})
set(FRONTEND_DIR ${MACSURF_ROOT}/browser/netsurf/frontends/macos9)
set(NETSURF_DIR ${MACSURF_ROOT}/browser/netsurf)
set(LIBCSS_DIR ${MACSURF_ROOT}/browser/libcss)
set(LIBDOM_DIR ${MACSURF_ROOT}/browser/libdom)
set(LIBHUBBUB_DIR ${MACSURF_ROOT}/browser/libhubbub)
set(LIBPARSERUTILS_DIR ${MACSURF_ROOT}/browser/libparserutils)
set(LIBNSBMP_DIR ${MACSURF_ROOT}/browser/libnsbmp)
set(LIBNSGIF_DIR ${MACSURF_ROOT}/browser/libnsgif)
set(LIBWAPCAPLET_DIR ${MACSURF_ROOT}/browser/libwapcaplet)
set(LIBQUICKJS_DIR ${MACSURF_ROOT}/browser/libquickjs)
set(MACTLS_DIR ${MACSURF_ROOT}/macTLS)
set(BEARSSL_DIR ${MACTLS_DIR}/bearssl)

file(GLOB LIBPU_SOURCES ${LIBPARSERUTILS_DIR}/src/charset/*.c ${LIBPARSERUTILS_DIR}/src/charset/codecs/*.c ${LIBPARSERUTILS_DIR}/src/charset/encodings/*.c ${LIBPARSERUTILS_DIR}/src/input/*.c ${LIBPARSERUTILS_DIR}/src/utils/*.c)
file(GLOB LIBHUB_SOURCES ${LIBHUBBUB_DIR}/src/charset/*.c ${LIBHUBBUB_DIR}/src/*.c ${LIBHUBBUB_DIR}/src/tokeniser/*.c ${LIBHUBBUB_DIR}/src/treebuilder/*.c ${LIBHUBBUB_DIR}/src/utils/*.c)
list(FILTER LIBHUB_SOURCES EXCLUDE REGEX "/examples/|/perf/")
file(GLOB LIBWAP_SOURCES ${LIBWAPCAPLET_DIR}/src/*.c)
list(FILTER LIBWAP_SOURCES EXCLUDE REGEX "/test/")
file(GLOB LIBBMP_SOURCES ${LIBNSBMP_DIR}/src/*.c)
file(GLOB LIBGIF_SOURCES ${LIBNSGIF_DIR}/src/*.c)
file(GLOB_RECURSE LIBCSS_SOURCES ${LIBCSS_DIR}/src/*.c)
list(FILTER LIBCSS_SOURCES EXCLUDE REGEX "/test/")
file(GLOB_RECURSE LIBDOM_SOURCES ${LIBDOM_DIR}/src/*.c ${LIBDOM_DIR}/bindings/hubbub/*.c)
list(FILTER LIBDOM_SOURCES EXCLUDE REGEX "/examples/|/xml/")
set(QUICKJS_SOURCES ${LIBQUICKJS_DIR}/dtoa.c ${LIBQUICKJS_DIR}/libregexp.c ${LIBQUICKJS_DIR}/libunicode.c ${LIBQUICKJS_DIR}/quickjs.c)
file(STRINGS "${MACTLS_DIR}/tools/bearssl_cw8_files.txt" BEARSSL_FILE_LIST REGEX "^macTLS/bearssl/src/.*\\.c$")
list(TRANSFORM BEARSSL_FILE_LIST REPLACE "^macTLS/" "${MACTLS_DIR}/")
file(GLOB MACTLS_OS9_SOURCES ${MACTLS_DIR}/os9/*.c)
list(FILTER MACTLS_OS9_SOURCES EXCLUDE REGEX "/archive/")
file(GLOB_RECURSE NETSURF_UTILS_SOURCES ${NETSURF_DIR}/utils/*.c)
file(GLOB_RECURSE NETSURF_CONTENT_SOURCES ${NETSURF_DIR}/content/*.c)
# about/imagecache.c is the only remaining consumer of content/handlers/image/
# (image_cache_snsummaryf / image_cache_snentryf), which is excluded above --
# MacSurf supplies its own image layer in frontends/macos9/macos9_image.c. It
# is not in MacSurf.mcp either, so dropping it is build parity with CW8.
list(FILTER NETSURF_CONTENT_SOURCES EXCLUDE REGEX "/curl\\.c$|/curl/|/duktape/|testament|fs_backing_store|/fetchers/file/|/handlers/image/|/fetchers/about/imagecache\\.c$|handlers/javascript/js\\.c$")
set(NETSURF_DESKTOP_SOURCES ${NETSURF_DIR}/desktop/bitmap.c ${NETSURF_DIR}/desktop/browser.c ${NETSURF_DIR}/desktop/browser_history.c ${NETSURF_DIR}/desktop/browser_window.c ${NETSURF_DIR}/desktop/cookie_manager.c ${NETSURF_DIR}/desktop/cw_helper.c ${NETSURF_DIR}/desktop/download.c ${NETSURF_DIR}/desktop/frames.c ${NETSURF_DIR}/desktop/global_history.c ${NETSURF_DIR}/desktop/gui_factory.c ${NETSURF_DIR}/desktop/knockout.c ${NETSURF_DIR}/desktop/local_history.c ${NETSURF_DIR}/desktop/mouse.c ${NETSURF_DIR}/desktop/plot_style.c ${NETSURF_DIR}/desktop/print.c ${NETSURF_DIR}/desktop/save_text.c ${NETSURF_DIR}/desktop/scrollbar.c ${NETSURF_DIR}/desktop/search.c ${NETSURF_DIR}/desktop/selection.c ${NETSURF_DIR}/desktop/system_colour.c ${NETSURF_DIR}/desktop/textarea.c ${NETSURF_DIR}/desktop/textinput.c ${NETSURF_DIR}/desktop/version.c ${NETSURF_DIR}/desktop/netsurf.c ${NETSURF_DIR}/desktop/searchweb.c
    # treeview.c backs cookie_manager.c / global_history.c / local_history.c,
    # all three of which are already in this list and reference ~20
    # treeview_* symbols. It was missing, and the link only ever succeeded
    # because -gc-sections discarded the three managers along with every
    # reference. Building it makes those features actually present.
    ${NETSURF_DIR}/desktop/treeview.c)
list(FILTER NETSURF_UTILS_SOURCES EXCLUDE REGEX "ns_file\\.c$")
file(GLOB FRONTEND_CORE_SOURCES ${FRONTEND_DIR}/*.c)
list(FILTER FRONTEND_CORE_SOURCES EXCLUDE REGEX "js_stub")
#
# The POSIX shim layer. CodeWarrior compiles all of these (mac_stat,
# mac_file_io, mac_dirent, mac_time are all in MacSurf.mcp); this build never
# did, because the glob above only covers the top level of the frontend dir.
#
# That gap is what tools/apply_retro68_fixes.sh was compensating for when it
# generated no-op opendir()/closedir() stubs -- the real implementations were
# sitting in shims/mac_dirent.c, simply never compiled. Building the shims
# gives this target the same POSIX surface CodeWarrior has instead.
#
# mac_iconv.c stays out for now: macsurf_prefix.h carries __RETRO68__ iconv
# stubs that #define iconv_open/iconv/iconv_close to static no-ops, so
# compiling the real one here would collide. Removing those stubs is a prefix
# change (full CW8 rebuild), so it is deliberately a separate step.
#
# Currently OPT-IN. Adding the shims plus macos9_retro68_compat.c resolves the
# POSIX names (stat, opendir/readdir/closedir) that NetSurf's utils.c wants,
# which in turn keeps a large amount of code that -gc-sections was previously
# discarding -- and at that point Retro68's ld dies with SIGSEGV before it
# emits even a -t trace line, i.e. in its own output writer rather than in
# symbol resolution. Isolated and reproducible: a single object defining just
# closedir() is enough to trigger it; the same link without it succeeds.
#
# So this is a Retro68 ld limit, not a MacSurf defect, and closing it is its
# own piece of work. Everything needed is written and compiles; only the final
# link is blocked. Build with -DMACSURF_POSIX_SHIMS=ON to work on it.
option(MACSURF_POSIX_SHIMS "Compile the POSIX shim layer (currently blocked by a Retro68 ld crash)" OFF)
if(MACSURF_POSIX_SHIMS)
    file(GLOB FRONTEND_SHIM_SOURCES ${FRONTEND_DIR}/shims/*.c)
    list(FILTER FRONTEND_SHIM_SOURCES EXCLUDE REGEX "mac_iconv")
    list(APPEND FRONTEND_CORE_SOURCES ${FRONTEND_SHIM_SOURCES})
else()
    list(FILTER FRONTEND_CORE_SOURCES EXCLUDE REGEX "macos9_retro68_compat")
endif()
set(FRONTEND_JS_SOURCES ${FRONTEND_DIR}/javascript/macsurf_qjs.c ${FRONTEND_DIR}/javascript/macos9_js_fetch.c)

set(COMMON_INCLUDES ${FRONTEND_DIR} ${FRONTEND_DIR}/shims ${FRONTEND_DIR}/javascript ${FRONTEND_DIR}/css ${FRONTEND_DIR}/html ${FRONTEND_DIR}/image ${FRONTEND_DIR}/text ${FRONTEND_DIR}/utils ${FRONTEND_DIR}/nsutils ${FRONTEND_DIR}/libwapcaplet ${FRONTEND_DIR}/sys ${FRONTEND_DIR}/parserutils ${FRONTEND_DIR}/parserutils/charset ${MULTIVERSAL_INCLUDES} ${NETSURF_DIR}/include ${NETSURF_DIR}/content ${NETSURF_DIR}/content/handlers ${NETSURF_DIR}/content/fetchers ${NETSURF_DIR}/desktop ${NETSURF_DIR} ${NETSURF_DIR}/utils/http ${NETSURF_DIR}/utils/nsurl ${LIBCSS_DIR}/include ${LIBCSS_DIR}/src ${LIBCSS_DIR}/src/lex ${LIBCSS_DIR}/src/parse ${LIBCSS_DIR}/src/parse/properties ${LIBCSS_DIR}/src/select ${LIBCSS_DIR}/src/select/properties ${LIBCSS_DIR}/src/charset ${LIBCSS_DIR}/src/utils ${LIBDOM_DIR}/include ${LIBDOM_DIR}/include/dom ${LIBDOM_DIR}/src ${LIBDOM_DIR}/src/core ${LIBDOM_DIR}/src/events ${LIBDOM_DIR}/src/html ${LIBDOM_DIR}/src/utils ${LIBDOM_DIR}/bindings/hubbub ${LIBHUBBUB_DIR}/include ${LIBHUBBUB_DIR}/src ${LIBHUBBUB_DIR}/src/charset ${LIBHUBBUB_DIR}/src/tokeniser ${LIBHUBBUB_DIR}/src/treebuilder ${LIBHUBBUB_DIR}/src/utils ${LIBPARSERUTILS_DIR}/include ${LIBPARSERUTILS_DIR}/src ${LIBPARSERUTILS_DIR}/src/charset ${LIBPARSERUTILS_DIR}/src/charset/codecs ${LIBPARSERUTILS_DIR}/src/charset/encodings ${LIBPARSERUTILS_DIR}/src/input ${LIBPARSERUTILS_DIR}/src/utils ${LIBNSBMP_DIR}/include ${LIBNSGIF_DIR}/include ${LIBWAPCAPLET_DIR}/include ${LIBQUICKJS_DIR} ${MACTLS_DIR}/os9 ${BEARSSL_DIR}/inc ${BEARSSL_DIR}/src)

set(COMMON_DEFS __RETRO68__ __MACOS9__=1 NO_IPV6=1 PATH_MAX=256 WITH_QUICKJS=1 MACSURF_JS_FIRE_LOAD=1 MACSURF_DEBUG=1 LODEPNG_NO_COMPILE_ENCODER=1 LODEPNG_NO_COMPILE_DISK=1 LODEPNG_NO_COMPILE_CPP=1 CONFIG_THREADS=0 CONFIG_BIGNUM=1 MAC_OS_9=1 BR_64=0 BR_LOMUL=0 BR_CT_MUL31=0 BR_CT_MUL15=0 BR_SLOW_MUL=0 BR_SLOW_MUL15=0 BR_BE_UNALIGNED=0 BR_LE_UNALIGNED=0 BR_AES_X86NI=0 BR_SSE2=0 BR_RDRAND=0 BR_POWER8=0 BR_INT128=0 BR_UMUL128=0 BR_ARMEL_CORTEXM_GCC=0 BR_USE_URANDOM=0 BR_USE_GETENTROPY=0 BR_USE_WIN32_RAND=0 BR_USE_UNIX_TIME=0 BR_USE_WIN32_TIME=0)

#
# Warning policy.
#
# The build used to pass -w, which silences everything.  That is exactly the
# trap CLAUDE.md documents for the Linux harness: with warnings off, a missing
# declaration compiles as "int f()", so a Toolbox call that really returns a
# pointer or takes a struct is called through the wrong ABI and simply crashes
# at runtime with nothing in the build log.  The frontend is currently clean
# under -Wall, which is precisely when this is cheap to turn on and keep on.
#
# -Werror=implicit-function-declaration is the load-bearing one; the rest are
# left as warnings so third-party sources (QuickJS, BearSSL, lodepng) don't
# fail the build over style.
#
set(MACSURF_WARN_FLAGS -Wall -Werror=implicit-function-declaration)

#
# Bring-up vs shipping codegen.
#
# While the Retro68 runtime is still being brought up, optimisation and
# section GC are pure liability: -Os reorders and inlines away the frames you
# need to read a crash address, and -gc-sections can strip code that is only
# reached indirectly.  MACSURF_BRINGUP builds -O0 -g with no GC so a crash
# address resolves to a source line via addr2line.
#
# Turn this OFF for a build that is meant to be fast or shipped.
#
# NOTE on -gc-sections: it stays ON in BOTH configurations, because on this
# toolchain it is load-bearing rather than an optimisation. Retro68's newlib
# and libgcc REFERENCE but never DEFINE __gcc_qadd/qdiv/qmul/qsub (IBM
# long-double soft float), _memalign_r, and _jp2uc_l/_uc2jp_l. Link without
# GC and those archive members get pulled in and go unresolved -- and this ld
# answers an unresolved symbol with SIGSEGV and no message, which is a very
# expensive thing to debug from scratch. GC discards the code that references
# them before that happens.
#
# It costs little for bring-up: -O0 still keeps frames intact and -g still
# gives line numbers; GC only drops sections nothing reaches.
option(MACSURF_BRINGUP "Retro68 runtime bring-up build: -O0 -g (frames and line numbers intact)" ON)
if(MACSURF_BRINGUP)
    set(MACSURF_OPT_FLAGS -O0 -g -ffunction-sections)
else()
    set(MACSURF_OPT_FLAGS -Os -ffunction-sections)
endif()
set(MACSURF_LINK_FLAGS -Wl,-gc-sections)

function(macsurf_static_lib name)
    add_library(${name} STATIC ${ARGN})
    target_include_directories(${name} PRIVATE ${COMMON_INCLUDES})
    target_include_directories(${name} SYSTEM PRIVATE ${UNIVERSAL_CINCLUDES})
    target_compile_definitions(${name} PRIVATE ${COMMON_DEFS})
    target_compile_options(${name} PRIVATE -include ${FRONTEND_DIR}/macsurf_prefix.h
                           ${MACSURF_OPT_FLAGS} ${MACSURF_WARN_FLAGS})
    set_target_properties(${name} PROPERTIES LINKER_LANGUAGE C)
endfunction()

#
# Per-library include precedence.
#
# libcss, libdom, libhubbub and libparserutils each ship their OWN
# "utils/utils.h", and every one of them includes it by that exact relative
# path.  COMMON_INCLUDES lists ${NETSURF_DIR} before the library source dirs,
# so all four resolved to browser/netsurf/utils/utils.h instead -- NetSurf's,
# which declares none of their internals.
#
# macsurf_prefix.h has been papering over the fallout for the MACRO cases
# (N_ELEMENTS, SLEN, NOF_ELEMENTS, UNUSED, FLEX_ARRAY_LEN_DECL -- see the note
# at macsurf_prefix.h:353).  It cannot paper over the FUNCTION cases, and those
# are the dangerous ones: libcss/src/lex/lex.c calls isDigit() and isHex()
# (both return bool) and charToHex() (returns uint32_t) with no declaration in
# scope, so they were compiled as int-returning.  That is an ABI mismatch in
# the CSS lexer, i.e. the code path var() support depends on.
#
# Giving each library its own source root BEFORE the shared list fixes the
# whole class at the root instead of one symbol at a time.
#
function(macsurf_lib_own_includes name)
    target_include_directories(${name} BEFORE PRIVATE ${ARGN})
endfunction()

macsurf_static_lib(macsurf_parserutils ${LIBPU_SOURCES})
macsurf_lib_own_includes(macsurf_parserutils ${LIBPARSERUTILS_DIR}/src ${LIBPARSERUTILS_DIR}/include)

macsurf_static_lib(macsurf_hubbub ${LIBHUB_SOURCES})
macsurf_lib_own_includes(macsurf_hubbub ${LIBHUBBUB_DIR}/src ${LIBHUBBUB_DIR}/include)

macsurf_static_lib(macsurf_wapcaplet ${LIBWAP_SOURCES})
macsurf_lib_own_includes(macsurf_wapcaplet ${LIBWAPCAPLET_DIR}/include)

macsurf_static_lib(macsurf_nsbmp ${LIBBMP_SOURCES})
macsurf_static_lib(macsurf_nsgif ${LIBGIF_SOURCES})

macsurf_static_lib(macsurf_css ${LIBCSS_SOURCES})
macsurf_lib_own_includes(macsurf_css ${LIBCSS_DIR}/src ${LIBCSS_DIR}/include)

macsurf_static_lib(macsurf_dom ${LIBDOM_SOURCES})
macsurf_lib_own_includes(macsurf_dom ${LIBDOM_DIR}/src ${LIBDOM_DIR}/include)

macsurf_static_lib(macsurf_quickjs ${QUICKJS_SOURCES})
macsurf_static_lib(macsurf_bearssl ${BEARSSL_FILE_LIST} ${BEARSSL_DIR}/src/ssl/ssl_rec_ccm.c ${BEARSSL_DIR}/src/rand/sysrng.c)
macsurf_static_lib(macsurf_mactls ${MACTLS_OS9_SOURCES})
macsurf_static_lib(macsurf_netsurf_utils ${NETSURF_UTILS_SOURCES})
macsurf_static_lib(macsurf_netsurf_content ${NETSURF_CONTENT_SOURCES})
macsurf_static_lib(macsurf_netsurf_desktop ${NETSURF_DESKTOP_SOURCES})

set(REZ_INCLUDE_PATH "${RETRO68_RINCLUDES}:${FRONTEND_DIR}" CACHE STRING "Rez")
add_application(MacSurf TYPE APPL CREATOR MPLS ${FRONTEND_CORE_SOURCES} ${FRONTEND_JS_SOURCES} ${FRONTEND_DIR}/retro68_malloc.c ${FRONTEND_DIR}/MacSurf.r ${FRONTEND_DIR}/MacSurfIcon.r ${FRONTEND_DIR}/MacSurfSize.r)
target_include_directories(MacSurf PRIVATE ${COMMON_INCLUDES})
target_include_directories(MacSurf SYSTEM PRIVATE ${UNIVERSAL_CINCLUDES})
target_compile_definitions(MacSurf PRIVATE ${COMMON_DEFS})
target_compile_options(MacSurf PRIVATE -include ${FRONTEND_DIR}/macsurf_prefix.h
                       ${MACSURF_OPT_FLAGS} ${MACSURF_WARN_FLAGS})
target_link_options(MacSurf PRIVATE ${MACSURF_LINK_FLAGS})
#
# These libraries are mutually recursive -- utils/log.c wants netsurf_version
# from desktop/version.c, desktop wants content, content wants utils -- and a
# single left-to-right pass over static archives cannot satisfy that: once ld
# has walked past an archive it will not go back. It only became visible now
# because -gc-sections used to discard the referring code before the reference
# had to resolve.
#
# --start-group makes ld re-scan the set until it reaches a fixed point, which
# is the standard answer and removes any dependence on listing order.
#
target_link_libraries(MacSurf
    -Wl,--start-group
    macsurf_netsurf_content macsurf_netsurf_desktop macsurf_netsurf_utils
    macsurf_css macsurf_dom macsurf_parserutils macsurf_hubbub
    macsurf_wapcaplet macsurf_nsbmp macsurf_nsgif macsurf_quickjs
    macsurf_mactls macsurf_bearssl
    -Wl,--end-group
    m)
set_target_properties(MacSurf PROPERTIES LINKER_LANGUAGE C)
message(STATUS "MacSurf Retro68: TYPE=APPL CREATOR=MPLS")
if(MACSURF_BRINGUP)
    message(STATUS "MacSurf Retro68: BRING-UP build (-O0 -g, no -gc-sections)")
else()
    message(STATUS "MacSurf Retro68: optimised build (-Os, -gc-sections)")
endif()
