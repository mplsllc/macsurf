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
# So the usable compiler signal is: errors reported IN THE TARGET FILE ITSELF.
# That catches the C89 mistakes this check exists for (declaration after
# statement, // comments, for-scope decls, designated initialisers, VLAs...).
# The companion declorder_lint.py is also run for every target because C89 GCC
# otherwise permits implicit function declarations that CW8 later rejects.
#
# Usage: tools/retro68_check.sh <file.c> [file2.c ...]
#        tools/retro68_check.sh --selftest    # prove both gates can fail
# Exit 0 = clean, 1 = source errors, 2 = Retro68 could not parse target.

CC=/home/patrick/Retro68/toolchain/bin/powerpc-apple-macos-gcc
ROOT=/home/patrick/Webs/macsurf
DECL_LINT="$ROOT/tools/declorder_lint.py"

if [ ! -x "$CC" ]; then
	echo "retro68_check: toolchain missing at $CC" >&2
	exit 2
fi

# Strict C89 gate. -pedantic-errors catches language extensions that plain
# -pedantic only warns about. The explicit -Werror switches close important
# GCC-C89 loopholes and make the intended contract obvious:
#   - declaration-after-statement: CW8 requires declarations at block top
#   - implicit-function-declaration / implicit-int: legal old-C but a frequent
#     CW8 redeclaration failure and almost never intentional in MacSurf
#   - return-type: missing/wrong returns are source defects, not warnings
#   - vla: never permit a C99 VLA to sneak into Mac-target code
#
# -Wno-overlength-strings: C90 only requires compilers to SUPPORT 509-char
# literals; it does not forbid longer ones. macsurf_qjs.c is built from very
# large JS source strings and CW8 has always accepted them, so promoting this
# implementation-limit warning to an error would bury real findings.
FLAGS="-std=c89 -pedantic-errors -Wall
       -Werror=declaration-after-statement
       -Werror=implicit-function-declaration
       -Werror=implicit-int
       -Werror=return-type
       -Werror=vla
       -Wno-unused-parameter -Wno-unused-variable
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
# versus a baseline that hit the SAME fatal error, is a false green.
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

run_decl_lint() {
	local f="$1"
	if [ -x "$DECL_LINT" ]; then
		"$DECL_LINT" "$f"
		return $?
	fi
	if [ -f "$DECL_LINT" ]; then
		python3 "$DECL_LINT" "$f"
		return $?
	fi
	echo "retro68_check: declaration-order lint missing at $DECL_LINT" >&2
	return 2
}

if [ "$1" = "--selftest" ]; then
	# Negative controls: a check that cannot fail is worthless (#296 lesson).
	tmp=$(mktemp /tmp/retro68_selftest_XXXX.c)
	cat > "$tmp" <<'EOF'
static int later(void);
int f(void)
{
	int a = 1;
	a = a + 1;
	int b = 2;   /* C89 violation: declaration after statement */
	return a + b + undeclared_function();
}
static int later(void)
{
	return 0;
}
EOF
	out=$($CC -fsyntax-only $FLAGS "$tmp" 2>&1 | grep -cE "error:")
	rm -f "$tmp"
	if [ "$out" -le 0 ]; then
		echo "selftest FAIL: Retro68 did NOT reject strict-C89 violations" >&2
		exit 1
	fi

	# declorder_lint has its own real-world purpose: a static function called
	# before definition with no prototype is accepted by old C semantics but
	# rejected later by CW8 when the real type appears.
	tmp=$(mktemp /tmp/declorder_selftest_XXXX.c)
	cat > "$tmp" <<'EOF'
int f(void)
{
	return helper(1);
}
static int helper(int x)
{
	return x;
}
EOF
	if run_decl_lint "$tmp" >/dev/null 2>&1; then
		rm -f "$tmp"
		echo "selftest FAIL: declaration-order lint accepted an implicit static call" >&2
		exit 1
	fi
	rm -f "$tmp"
	echo "selftest PASS: Retro68 strict-C89 + declaration-order gates reject bad source"
	exit 0
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
		echo "DID NOT RUN: $(basename "$f") -- ${out#__DIDNOTRUN__ }"
		echo "             (compile aborted before this file's code was parsed;"
		echo "              Retro68 provides NO compiler coverage for it -- CW8"
		echo "              remains authoritative.)"
		if [ "$rc" -eq 0 ]; then rc=2; fi
		;;
	"")
		echo "strict-c89 clean: $(basename "$f")"
		;;
	*)
		echo "=== STRICT C89 ERRORS in $(basename "$f") ==="
		echo "$out"
		rc=1
		;;
	esac

	# Always run the source-level declaration-order check, even when Retro68
	# could not reach the target because its Mac headers are incomplete.
	if ! run_decl_lint "$f"; then
		drc=$?
		if [ "$drc" -eq 1 ]; then
			rc=1
		elif [ "$rc" -eq 0 ]; then
			rc=2
		fi
	fi
done

exit $rc
