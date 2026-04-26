/*
 * MacSurf shim -- html/html.h
 *
 * This stub DOES NOT define NETSURF_HTML_HTML_H. It forwards to the real
 * html.h via the content/handlers path so CW8 processes the full struct
 * definitions (html_script, html_script_type, etc.) from the canonical file.
 *
 * The macos9/html/ access path is higher priority than content/handlers/html/
 * in the CW8 project, so this shim is found first -- but by NOT setting the
 * real guard here, we hand off to the real file which owns that guard.
 */

#ifndef MACSURF_SHIM_HTML_HTML_H
#define MACSURF_SHIM_HTML_HTML_H
#include "content/handlers/html/html.h"
#endif
