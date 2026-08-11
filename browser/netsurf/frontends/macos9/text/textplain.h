/*
 * MacSurf stub -- text/textplain.h
 * Minimal C89-compatible stub for CodeWarrior 8 compilation.
 * Licensed under GPL v2.
 *
 * Must carry the REAL header's guard (NETSURF_HTML_TEXTPLAIN_H) so a TU
 * that reaches both this stub (via the macos9 access path) and the real
 * content/handlers/text/textplain.h only processes one — see the shim
 * header gotcha in CLAUDE.md. The real header declares nothing beyond
 * textplain_init, so this stub is a complete forwarder.
 */

#ifndef NETSURF_HTML_TEXTPLAIN_H
#define NETSURF_HTML_TEXTPLAIN_H

#include "utils/ns_errors.h"

nserror textplain_init(void);

#endif
