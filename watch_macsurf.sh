#!/bin/bash
# watch_macsurf.sh
# Uses 'entr' to run verify_macsurf.sh whenever files change.

# Find all source files to watch
FILES=$(find browser -name "*.c" -o -name "*.h")

echo "$FILES" | entr ./verify_macsurf.sh browser/netsurf/frontends/macos9/main.c
