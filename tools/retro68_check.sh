#!/bin/bash
# Retro68 PPC GCC Linux-side syntax check for MacSurf sources.
#
# Green Retro68 != green CW8 (CW8 is the source of truth), but red Retro68 is
# always real. Run before every fix tar.
#
# SCOPE NOTE (important, learned 2026-07-16): Retro68's multiversal header set
# is PARTIAL -- it has no Aliases.h/Appearance.h/TextEdit.h, so the Carbon
# branch of macos9.h (#ifdef __MACOS__) cannot be compiled here at all. We
# therefore deliberately compile the NON-Carbon branch, which leaves a fixed
# set of pre-existing errors in HEADERS (unknown WindowRef/Rect/ControlRef in
# macos9.h, trailing enum commas in netsurf/libdom/libcss headers). Those are
# noise from the incomplete header set, not defects -- CW8 has the real
# Universal Interfaces and accepts them.
#
# So the usable signal is: errors reported IN THE TARGET FILE ITSELF.
# That still catches the C89 mistakes this check exists for (declaration after
# statement, // comments, for-scope decls, designated initialisers, VLAs...).
#
# Usage: tools/retro68_check.sh <file.c> [file2.c ...]
#        tools/retro68_check.sh --selftest    # prove the check can fail
# Exit 0 = clean, 1 = errors in the target file.

CC=/home/patrick/Retro68/toolchain/bin/powerpc-apple-macos-gcc
ROOT=/home/patrick/Webs/macsurf

if [ ! -x "$CC" ]; then
	echo "retro68_check: toolchain missing at $CC" >&2
	exit 2
fi

# -pedantic-errors, NOT -pedantic: plain -pedantic only WARNS on C89
# violations (declaration-after-statement etc), which made --selftest report a
# false green. The errors it promotes in upstream headers are filtered out by
# check_one()'s target-file-only match.
# -Wno-overlength-strings: C90 only requires compilers to SUPPORT 509-char
# literals; it does not forbid longer ones. macsurf_qjs.c is built from very
# large JS source strings and CW8 has always accepted them, so -pedantic-errors
# promoting this to an error is pure noise that buries real findings.
FLAGS="-std=c89 -pedantic-errors -Wall -Wno-unused-parameter -Wno-unused-variable
       -Wno-long-long -Wno-overlength-strings
       -Dinline= -D__MACOS9__=1 -DTARGET_API_MAC_CARBON=1
       -DWITH_QUICKJS -DNO_IPV6"

INCS="-I $ROOT/browser/libwapcaplet/include
      -I $ROOT/browser/libdom/include -I $ROOT/browser/libdom/include/dom
      -I $ROOT/browser/libdom/src
      -I $ROOT/browser/libcss/include -I $ROOT/browser/libhubbub/include
      -I $ROOT/browser/libparserutils/include
      -I $ROOT/browser/netsurf -I $ROOT/browser/netsurf/include
      -I $ROOT/browser/netsurf/content/handlers
      -I $ROOT/browser/netsurf/frontends/macos9
      -I $ROOT/browser/netsurf/frontends/macos9/shims
      -I $ROOT/browser/libquickjs
      -I $ROOT/macTLS/os9
      -I $ROOT/macTLS/bearssl/inc
      -include stdbool.h"

# A "fatal error" (a missing header) ABORTS the compile, so the target's own code
# is never parsed. Reporting that as "clean", or as an unchanged error count
# versus a baseline that hit the SAME fatal error, is a false green -- it says
# "no new errors" while checking nothing at all. That is how fixes886's
# macos9_zoom_apply redeclaration reached CW8: main.c dies at a missing
# OpenTransport.h on line 17, ~450 lines before the bug.
#
# So: detect the abort and say DID-NOT-RUN, loudly, distinct from clean.
check_one() {
	local f="$1"
	local base out fatal
	base=$(basename "$f")
	out=$($CC -fsyntax-only $FLAGS $INCS "$f" 2>&1)

	fatal=$(echo "$out" | grep -E "fatal error:" | head -1)
	if [ -n "$fatal" ]; then
		echo "__DIDNOTRUN__ $fatal"
		return
	fi
	echo "$out" | grep -E "/${base}:[0-9]+.*error:"
}

if [ "$1" = "--selftest" ]; then
	# Negative control: a check that cannot fail is worthless (#296 lesson).
	tmp=$(mktemp /tmp/retro68_selftest_XXXX.c)
	cat > "$tmp" <<'EOF'
int f(void)
{
	int a = 1;
	a = a + 1;
	int b = 2;   /* C89 violation: declaration after statement */
	return a + b;
}
EOF
	out=$($CC -fsyntax-only -std=c89 -pedantic-errors "$tmp" 2>&1 | grep -cE "error:")
	rm -f "$tmp"
	if [ "$out" -gt 0 ]; then
		echo "selftest PASS: check correctly rejects C89 violations ($out error(s))"
		exit 0
	fi
	echo "selftest FAIL: check did NOT catch a C89 violation -- do not trust it" >&2
	exit 1
fi

if [ $# -lt 1 ]; then
	echo "usage: $0 <file.c> [...] | --selftest" >&2
	exit 2
fi

rc=0
for f in "$@"; do
	out=$(check_one "$f")
	case "$out" in
	__DIDNOTRUN__*)
		# Not a pass and not a fail: the compiler never reached this file's
		# code. Retro68's multiversal headers are a PARTIAL set (no
		# OpenTransport.h, Aliases.h, ...), so some frontend files simply
		# cannot be checked here. Say so instead of implying coverage.
		echo "DID NOT RUN: $(basename "$f") -- ${out#__DIDNOTRUN__ }"
		echo "             (compile aborted before this file's code was parsed;"
		echo "              this check provides NO coverage for it -- CW8 is the"
		echo "              only thing that will see these errors)"
		rc=2
		;;
	"")
		echo "clean: $(basename "$f")"
		;;
	*)
		echo "=== ERRORS in $(basename "$f") ==="
		echo "$out"
		rc=1
		;;
	esac
done

exit $rc
