#!/bin/bash
# list-files-for-build.sh
# Usage: bash list-files-for-build.sh <target-sha>
# Prints the repo-relative paths that differ between <target-sha> and
# the current stabilization tip (8c3bb9118), filtered to source files only.
# These are the files you need to ship to the Mac for a clean historical test.

CURRENT="8c3bb9118876ea955d926f633364fa4d835b0327"
TARGET="$1"

if [ -z "$TARGET" ]; then
    echo "Usage: $0 <target-sha>"
    echo ""
    echo "Known test points:"
    echo "  A: b318c3c0ce88cd0dfca4c72b5ff6c94d6713e7ff"
    echo "  B: fccd036ac6035df3654154dd8757f3cc5b2fde24"
    echo "  C: 4ef8bc71d4c29b87695b6baf26954025fe4554ba"
    echo "  D: 972994c2d1203ccfd8fe26651440777165e540de"
    exit 1
fi

cd "$(git rev-parse --show-toplevel)" || exit 1

echo "Files changed between $TARGET and $CURRENT (ship these for build $TARGET):"
echo ""
git diff --name-only "$TARGET" "$CURRENT" \
    | grep -v '^\.github\|^tools\|^\.private\|^ci\|^docs\|^harness\|^perf\|^quickjs-macos9\|^macLink\|^macTLS\|^webgl\|^NanoKernel' \
    | grep '\.\(c\|h\)$'
