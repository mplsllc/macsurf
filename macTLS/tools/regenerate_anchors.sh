#!/usr/bin/env bash
#
# regenerate_anchors.sh — refresh os9/ostls_b3_anchors.c from public PEMs
#
# When a root CA rotates or a new one needs to be embedded, edit the
# ANCHOR_URLS list below and rerun this script. It downloads the PEMs,
# builds a host-side brssl from a fresh BearSSL upstream clone, runs
# `brssl ta` to produce trust-anchor byte arrays, and rewrites the
# byte-array section of ostls_b3_anchors.c in place. The runtime
# init_*_anchor() blocks at the bottom of the file are NOT touched —
# if a new anchor slot is added or the order changes, edit those by
# hand and bump OSTLS_B3_NUM_ANCHORS in ostls_b3_anchors.h.
#
# Requires: bash, curl, cc (for building brssl), git, openssl.
#
# Usage:    bash tools/regenerate_anchors.sh
#
# Tested:   Ubuntu 24.04 + system gcc.

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-/tmp}/macssl-anchors-$$"
UPSTREAM_REPO="https://www.bearssl.org/git/BearSSL"
OUT_C="$PROJECT_ROOT/os9/ostls_b3_anchors.c"

mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

# Order matters — this is the order brssl emits TAn_* in, and it must
# match the runtime init_*_anchor() block sequence in ostls_b3_anchors.c.
# If you change this list, update the .c file's runtime init AND bump
# OSTLS_B3_NUM_ANCHORS in the header.
declare -a ANCHOR_FILES=(
    "amazon_root_ca_1.pem"
    "digicert_global_root_g2.pem"
    "gts_root_r1.pem"
    "gts_root_r4.pem"
    "isrg_root_x1.pem"
    "isrg_root_x2.pem"
    "gts_root_r2.pem"
    "gts_root_r3.pem"
    "digicert_global_root_g3.pem"
    "starfield_services_root_g2.pem"
)
declare -A ANCHOR_URL=(
    [amazon_root_ca_1.pem]="https://www.amazontrust.com/repository/AmazonRootCA1.pem"
    [digicert_global_root_g2.pem]="https://cacerts.digicert.com/DigiCertGlobalRootG2.crt.pem"
    [digicert_global_root_g3.pem]="https://cacerts.digicert.com/DigiCertGlobalRootG3.crt.pem"
    [gts_root_r1.pem]="https://i.pki.goog/r1.pem"
    [gts_root_r2.pem]="https://i.pki.goog/r2.pem"
    [gts_root_r3.pem]="https://i.pki.goog/r3.pem"
    [gts_root_r4.pem]="https://i.pki.goog/r4.pem"
    [isrg_root_x1.pem]="https://letsencrypt.org/certs/isrgrootx1.pem"
    [isrg_root_x2.pem]="https://letsencrypt.org/certs/isrg-root-x2.pem"
    [starfield_services_root_g2.pem]="https://www.amazontrust.com/repository/SFSRootCAG2.pem"
)

echo "==> Working directory: $WORK"
cd "$WORK"

# ---- 1. fetch BearSSL upstream (for brssl tool source) ----
echo "==> Cloning BearSSL upstream (shallow)..."
git clone --depth 1 "$UPSTREAM_REPO" bearssl >/dev/null 2>&1

# ---- 2. build brssl on the host ----
echo "==> Building brssl..."
cd bearssl
make >/dev/null 2>&1
BRSSL="$PWD/build/brssl"
test -x "$BRSSL" || { echo "brssl build failed" >&2; exit 1; }
cd "$WORK"

# ---- 3. fetch PEMs ----
echo "==> Downloading $((${#ANCHOR_FILES[@]})) anchor PEMs..."
mkdir -p anchors
for f in "${ANCHOR_FILES[@]}"; do
    url="${ANCHOR_URL[$f]}"
    printf "    %-40s  %s\n" "$f" "$url"
    if [[ "$f" == "starfield_services_root_g2.pem" || "$f" == "digicert_global_root_g3.pem" ]]; then
        # These URLs serve DER; some serve PEM directly. Handle both.
        curl -fsS -o "anchors/$f.raw" "$url"
        if head -1 "anchors/$f.raw" | grep -q "BEGIN CERTIFICATE"; then
            mv "anchors/$f.raw" "anchors/$f"
        else
            openssl x509 -in "anchors/$f.raw" -inform DER -out "anchors/$f"
            rm -f "anchors/$f.raw"
        fi
    else
        curl -fsS -o "anchors/$f" "$url"
    fi
done

# ---- 4. run brssl ta in the configured order ----
echo "==> Generating trust-anchor byte arrays..."
ORDERED=()
for f in "${ANCHOR_FILES[@]}"; do
    ORDERED+=("anchors/$f")
done
"$BRSSL" ta "${ORDERED[@]}" > brssl.out 2>brssl.err

# ---- 5. strip the designated-init TAs[] block (CW8 C89 incompatible) ----
python3 - <<PYEOF
import re
content = open('brssl.out').read()
m = re.search(r'^static const br_x509_trust_anchor TAs', content, re.M)
arrays = content[:m.start()].rstrip()
# Also drop the stderr-mixed "Reading file..." lines that brssl emits
arrays = '\n'.join(l for l in arrays.split('\n') if not l.startswith("Reading file"))
open('arrays.c','w').write(arrays)
PYEOF

# ---- 6. splice the new byte arrays back into ostls_b3_anchors.c ----
echo "==> Splicing into $OUT_C..."
python3 - <<PYEOF
existing = open('$OUT_C').read()
new_arrays = open('arrays.c').read()
# Find the boundary between the preamble + arrays block and the
# runtime-init block.  We anchor on a sentinel comment we control.
SENTINEL = 'static br_x509_trust_anchor g_anchors[OSTLS_B3_NUM_ANCHORS];'
idx = existing.find(SENTINEL)
if idx < 0:
    raise SystemExit("sentinel not found in $OUT_C; refusing to corrupt the file")
# Walk back to the last blank line before the sentinel so we keep the
# section separator intact.
preamble_end = existing.rfind('\n\n', 0, idx)
# Walk back further to find the start of the byte-array section
# (after #include "bearssl.h").
header_end = existing.find('#include "bearssl.h"')
header_section_end = existing.find('\n', header_end) + 1
preamble = existing[:header_section_end]
postamble = existing[preamble_end:]
out = preamble + '\n' + new_arrays + '\n' + postamble
open('$OUT_C','w').write(out)
print('wrote', len(out), 'bytes')
PYEOF

echo "==> Done. Review the diff before committing:"
echo "    git diff os9/ostls_b3_anchors.c"
echo
echo "Remember to:"
echo "  - bump OSTLS_B3_NUM_ANCHORS in os9/ostls_b3_anchors.h if the count changed"
echo "  - add/remove init_rsa_anchor() / init_ec_anchor() calls in the .c file"
echo "  - update the anchor list comment in both files"
