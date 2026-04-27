#!/bin/bash
# MacSurf Retro68 compile test
# Uses powerpc-apple-macos-gcc to approximate CW8 C89 errors before shipping to Mac

RETRO68=/home/patrick/Retro68/toolchain/bin
PPC_GCC=$RETRO68/powerpc-apple-macos-gcc
CARBON_GCC=$RETRO68/carbon-gcc

MACSURF=/home/patrick/Webs/macsurf/browser
NS=$MACSURF/netsurf
MACOS9=$NS/frontends/macos9

# Include paths that match CW8 Access Paths (key order preserved)
INCLUDES=(
  -I$MACOS9
  -I$MACOS9/shims
  -I$MACOS9/javascript
  -I$NS/include
  -I$NS/utils
  -I$NS/content
  -I$NS/desktop
  -I$NS/frontends
  -I$MACSURF/libdom/include
  -I$MACSURF/libdom/src
  -I$MACSURF/libdom/src/core
  -I$MACSURF/libdom/src/events
  -I$MACSURF/libdom/src/html
  -I$MACSURF/libdom/src/utils
  -I$MACSURF/libdom/bindings/hubbub
  -I$MACSURF/libcss/include
  -I$MACSURF/libcss/src
  -I$MACSURF/libhubbub/include
  -I$MACSURF/libhubbub/src
  -I$MACSURF/libparserutils/include
  -I$MACSURF/libparserutils/src
  -I$MACSURF/libduktape
)

# CW8 C89 approximation flags
FLAGS=(
  -std=c89
  -pedantic-errors
  -D__MACOS9__=1
  -DNO_IPV6=1
  -DWITHOUT_ICONV_FILTER=1
  -Dinline=
  -Drestrict=
  -include $MACOS9/macsurf_prefix.h
  -w  # suppress warnings, focus on errors
)

# Which files to test (frontend files most likely to have issues)
FRONTEND_FILES=(
  $MACOS9/main.c
  $MACOS9/window.c
  $MACOS9/plotters.c
  $MACOS9/macos9_font.c
  $MACOS9/macos9_fetch.c
)

# Core files that have had repeated CW8 issues
CORE_FILES=(
  $NS/content/handlers/html/script.c
  $NS/content/handlers/html/imagemap.c
  $MACSURF/libdom/src/events/ui_event.c
)

echo "=== MacSurf Retro68 compile test ==="
echo "GCC: $($PPC_GCC --version 2>&1 | head -1)"
echo ""

ERRORS=0

test_file() {
  local f=$1
  local name=$(basename $f)
  printf "  %-40s " "$name"
  err=$($PPC_GCC "${FLAGS[@]}" "${INCLUDES[@]}" -fsyntax-only "$f" 2>&1)
  if [ $? -eq 0 ]; then
    echo "OK"
  else
    echo "FAIL"
    echo "$err" | head -20
    ERRORS=$((ERRORS+1))
  fi
}

echo "--- Frontend files ---"
for f in "${FRONTEND_FILES[@]}"; do
  [ -f "$f" ] && test_file "$f"
done

echo ""
echo "--- Core files (known trouble spots) ---"
for f in "${CORE_FILES[@]}"; do
  [ -f "$f" ] && test_file "$f"
done

echo ""
if [ $ERRORS -eq 0 ]; then
  echo "All tests passed."
else
  echo "$ERRORS file(s) had errors."
fi
