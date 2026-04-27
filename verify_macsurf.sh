#!/bin/bash

# verify_macsurf.sh
# Runs a cross-compilation syntax check using Universal Interfaces and PowerPC GCC.

TARGET_FILE=$1

if [ -z "$TARGET_FILE" ]; then
    echo "Usage: $0 <source_file.c>"
    exit 1
fi

UNIV_HDRS="$HOME/universal_headers"
SHIMS="$(pwd)/browser/netsurf/frontends/macos9/shims"
PREFIX="$(pwd)/browser/netsurf/frontends/macos9/macsurf_prefix.h"

# We must mock __option() for ConditionalMacros.h
# And we must use -D__dest_os=... for MacTypes.h
# And we define __MWERKS__ but we must skip some of its internal checks

powerpc-linux-gnu-gcc \
-fsyntax-only -std=c89 -pedantic -Dinline= -Werror=declaration-after-statement \
-D__MWERKS__=0x8000 -D__MSL__=0x8000 -DTARGET_API_MAC_CARBON=1 -D__MACOS__=1 \
-D"__option(x)=0" -D"__dest_os=8" -D"__MSL_CPP__=0" \
-I"$SHIMS" \
-I$(pwd)/browser/libcss/include -I$(pwd)/browser/libcss/src \
-I$(pwd)/browser/libdom/include -I$(pwd)/browser/libdom/src -I$(pwd)/browser/libdom/bindings/hubbub \
-I$(pwd)/browser/libhubbub/include -I$(pwd)/browser/libhubbub/src \
-I$(pwd)/browser/libparserutils/include -I$(pwd)/browser/libparserutils/src \
-I$(pwd)/browser/libwapcaplet/include \
-I$(pwd)/browser/netsurf/include -I$(pwd)/browser/netsurf \
-I$(pwd)/browser/netsurf/frontends \
-I$(pwd)/browser/netsurf/frontends/macos9 \
-I$(pwd)/browser/netsurf/content/handlers \
-I$(pwd)/browser/libduktape \
-isystem "$UNIV_HDRS" \
-include "$PREFIX" \
"$TARGET_FILE" 2>&1
