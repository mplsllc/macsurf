/*
 * mlink_prefix.h — CW8 prefix file for macLink-Proxy
 *
 * Set as the project's prefix file in CW8 (Target Settings → C/C++
 * Language → Prefix File). Injected before every translation unit.
 *
 * Mirrors MacSurf's macsurf_prefix.h conventions:
 *   - __MACOS9__ defined for code that needs Mac-only paths
 *   - NO_IPV6 — we're IPv4 only on this platform
 *   - TARGET_API_MAC_CARBON for Carbon code paths
 *   - MacTypes.h first to prevent bool/true/false collisions
 */
#ifndef MLINK_PREFIX_H
#define MLINK_PREFIX_H

#include <MacTypes.h>

#define __MACOS9__              1
#define NO_IPV6                 1
#define TARGET_API_MAC_CARBON   1

/* OS-level Carbon include is gated to a single place so future
 * subheader-collision suppression (per CLAUDE.md notes on Carbon.h
 * sub-header guards) goes here. */

#endif /* MLINK_PREFIX_H */
