/*
 * macsurf_runtime_profile.h
 *
 * Keep compiler identity separate from the runtime target. In particular,
 * GCC targeting the native macMAKE Classic PPC ABI is not Metrowerks, but it
 * still builds code that calls the Classic Toolbox and Open Transport.
 * Retro68 and host syntax checks are preflight builds and retain stubs.
 */
#ifndef MACSURF_RUNTIME_PROFILE_H
#define MACSURF_RUNTIME_PROFILE_H

#if defined(__MWERKS__) && defined(__MACMAKE_CLASSIC_PPC__)
#error "Select exactly one Classic Mac compiler identity"
#endif

#if defined(__MACMAKE_CLASSIC_PPC__) && defined(__RETRO68__)
#error "Native macMAKE and Retro68 preflight profiles are mutually exclusive"
#endif

#if defined(__MWERKS__)
#define MACSURF_COMPILER_CODEWARRIOR 1
#define MACSURF_COMPILER_MACMAKE_GCC 0
#define MACSURF_CLASSIC_RUNTIME 1
#define MACSURF_RETRO68_PREFLIGHT 0
#define MACSURF_HOST_STUB_BUILD 0
#elif defined(__MACMAKE_CLASSIC_PPC__)
#define MACSURF_COMPILER_CODEWARRIOR 0
#define MACSURF_COMPILER_MACMAKE_GCC 1
#define MACSURF_CLASSIC_RUNTIME 1
#define MACSURF_RETRO68_PREFLIGHT 0
#define MACSURF_HOST_STUB_BUILD 0
#elif defined(__RETRO68__)
#define MACSURF_COMPILER_CODEWARRIOR 0
#define MACSURF_COMPILER_MACMAKE_GCC 0
#define MACSURF_CLASSIC_RUNTIME 0
#define MACSURF_RETRO68_PREFLIGHT 1
#define MACSURF_HOST_STUB_BUILD 0
#elif defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
#define MACSURF_COMPILER_CODEWARRIOR 0
#define MACSURF_COMPILER_MACMAKE_GCC 0
#define MACSURF_CLASSIC_RUNTIME 0
#define MACSURF_RETRO68_PREFLIGHT 0
#define MACSURF_HOST_STUB_BUILD 1
#else
#error "Define an explicit MacSurf compiler/runtime profile for this target"
#endif

#endif /* MACSURF_RUNTIME_PROFILE_H */
