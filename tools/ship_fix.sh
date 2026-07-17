#!/bin/bash
# Build a MacSurf fix tar: CR-convert, future-stamp, tar, verify, (optionally) scp.
#
# Encodes three rules that have each cost real rounds:
#  1. CR line endings. The tree is LF; CW8 on classic Mac OS needs CR-only. An
#     LF file reads as one giant line and the build sees NO usable change --
#     the symptom that looks like "stale file".
#  2. Strictly-increasing, WHOLE-DAY future mtime keyed to the fix number. CW8
#     compares dates at day granularity; two tars on the same calendar day
#     collide and the second is not seen as new. Must stay under the HFS 2040
#     ceiling (NN=876 -> ~2032, fine until NN~3600).
#  3. Verify before shipping: zero LF bytes, and the fix marker present.
#
# Usage: tools/ship_fix.sh <NN> <file> [file2 ...]
#        SHIP=1 tools/ship_fix.sh <NN> <file>   # also scp to the Mac

set -e
NN="$1"; shift
if [ -z "$NN" ] || [ $# -lt 1 ]; then
	echo "usage: $0 <NN> <file> [...]" >&2
	exit 2
fi

ROOT=/home/patrick/Webs/macsurf
STAGE=$(mktemp -d /tmp/macsurf_ship_XXXX)
TAR="$ROOT/fixes${NN}.tar"

STAMP=$(date -d "2030-01-01 +${NN} days" "+%Y%m%d1200.00")

for f in "$@"; do
	rel="${f#$ROOT/}"
	rel="${rel#./}"
	dest="$STAGE/$rel"
	mkdir -p "$(dirname "$dest")"
	# CR-convert: every LF becomes CR, no LF survives.
	sed 's/$/\r/' "$ROOT/$rel" | tr -d '\n' > "$dest"

	# VERIFY: zero LF bytes.
	lf=$(tr -cd '\n' < "$dest" | wc -c)
	if [ "$lf" -ne 0 ]; then
		echo "ABORT: $rel still has $lf LF bytes after CR-convert" >&2
		rm -rf "$STAGE"; exit 1
	fi
	touch -t "$STAMP" "$dest"
	echo "staged: $rel (CR-clean, mtime $STAMP)"
done

cd "$STAGE"
tar -cf "$TAR" browser
cd "$ROOT"
rm -rf "$STAGE"

echo "--- $TAR ---"
tar -tvf "$TAR"

if [ "$SHIP" = "1" ]; then
	scp -P 2222 -i ~/.ssh/macsurf_push -o StrictHostKeyChecking=no \
		"$TAR" patrick@localhost:/home/patrick/Documents/MacFiles/fixes${NN}.tar
	echo "shipped fixes${NN}.tar"
fi
