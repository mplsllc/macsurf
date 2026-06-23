#!/usr/bin/env python3
"""
Emit the canonical list of BearSSL .c files to include in MacSurf.mcp for
the Stage A CW8 build.

This is a *positive* include list, computed by walking the vendored
bearssl/src/ tree and removing files matching documented exclusion
patterns. The exclusion list is small, explicit, and policy-driven:

  - 64-bit-math variants     (PPC LP32 + CW8 codegen risk)
  - x86 intrinsic variants    (wrong platform)
  - POWER8 vector variants    (wrong CPU)
  - Linux/Windows entropy     (replaced by macSSL/os9/ostls_entropy.c)
  - blocking I/O wrapper      (defeats cooperative multitasking)
  - SSL server code           (browser is client-only)

Outputs (all under tools/):
  bearssl_cw8_files.txt        - one path per line, MacSurf.mcp ready
  bearssl_cw8_excluded.txt     - excluded files + reason
  bearssl_cw8_manifest.md      - human-readable grouped manifest

Usage:
  python3 tools/make_bearssl_filelist.py
  python3 tools/make_bearssl_filelist.py --check  (compare with audit)
"""

import argparse
import os
import re
import sys
from collections import defaultdict


# (regex pattern, reason) -- order matters, first match wins
EXCLUDE = [
    (r"src/rand/sysrng\.c$",
     "POSIX/Win32 entropy bridge; replaced by os9/ostls_entropy.c"),

    (r"src/ssl/ssl_io\.c$",
     "blocking br_sslio_* wrapper; cooperative engine path only"),

    (r"src/ssl/ssl_server.*\.c$",
     "TLS server code; MacSurf is client-only"),
    (r"src/ssl/ssl_hs_server\.c$",
     "TLS server handshake; client-only"),
    (r"src/ssl/ssl_rec_ccm\.c$",
     "CCM record layer; not required for client v0.1"),

    (r"src/ec/ec_p256_m64\.c$",
     "64-bit P-256; CW8 PPC LP32 + 64-bit codegen risk. Use ec_p256_m31."),
    (r"src/ec/ec_p256_m62\.c$",
     "62-bit P-256 variant; same hazard. Use ec_p256_m31."),
    (r"src/ec/ec_c25519_m64\.c$",
     "64-bit Curve25519; same hazard. Use ec_c25519_m31."),
    (r"src/ec/ec_c25519_m62\.c$",
     "62-bit Curve25519 variant; same hazard. Use ec_c25519_m31."),

    (r"src/symcipher/aes_ct64.*\.c$",
     "64-bit constant-time AES. Use aes_ct (32-bit) family."),
    (r"src/symcipher/poly1305_ctmulq\.c$",
     "64-bit Poly1305. Use poly1305_ctmul (32-bit)."),
    (r"src/int/i62_modpow2\.c$",
     "62-bit big-int modpow; 64-bit math hazard."),
    (r"src/hash/ghash_ctmul64\.c$",
     "64-bit GHASH. Use ghash_ctmul (32-bit) or ghash_ctmul32."),

    (r"src/symcipher/aes_x86ni.*\.c$",
     "x86 AES-NI intrinsics; wrong platform"),
    (r"src/symcipher/chacha20_sse2\.c$",
     "x86 SSE2 ChaCha20; wrong platform"),
    (r"src/hash/ghash_pclmul\.c$",
     "x86 PCLMUL GHASH; wrong platform"),

    (r"src/symcipher/aes_pwr8.*\.c$",
     "POWER8 AES; wrong CPU (we target G3/G4 32-bit PPC)"),
    (r"src/hash/ghash_pwr8\.c$",
     "POWER8 GHASH; wrong CPU"),
]

EXCLUDE_RE = [(re.compile(pat), reason) for pat, reason in EXCLUDE]


# High-level groupings for the human-readable manifest. Order matters
# (first match wins). Filenames not matching any prefix go in "other".
GROUPS = [
    ("SSL engine / handshake",     r"^src/ssl/"),
    ("X.509 minimal validator",    r"^src/x509/"),
    ("Hash algorithms",            r"^src/hash/"),
    ("HMAC + KDF + PRF",           r"^src/(mac/|kdf/|codec/)"),
    ("Symmetric ciphers",          r"^src/symcipher/"),
    ("AEAD modes (GCM, CCM, EAX)", r"^src/aead/"),
    ("Big integer (i15/i31/i32)",  r"^src/int/"),
    ("Elliptic curves",            r"^src/ec/"),
    ("RSA (cert verify path)",     r"^src/rsa/"),
    ("RNG (HMAC-DRBG, AES-CTR)",   r"^src/rand/"),
    ("Settings / misc",            r"^src/settings"),
]

GROUP_RE = [(label, re.compile(pat)) for label, pat in GROUPS]


def group_for(path):
    for label, rx in GROUP_RE:
        if rx.search(path):
            return label
    return "other"


def walk_sources(root):
    """Yield rel-paths of every .c under `root`, sorted."""
    out = []
    for dirpath, _, files in os.walk(root):
        for fn in files:
            if fn.endswith(".c"):
                rel = os.path.relpath(os.path.join(dirpath, fn), root)
                # normalize to forward slashes regardless of host OS
                rel = rel.replace(os.sep, "/")
                out.append(rel)
    return sorted(out)


def classify(rel_path):
    """Return (included: bool, reason: str)."""
    full = "bearssl/" + rel_path if not rel_path.startswith("bearssl/") else rel_path
    # match against the path as stored in EXCLUDE (without bearssl/ prefix)
    key = rel_path
    for rx, reason in EXCLUDE_RE:
        if rx.search(key):
            return False, reason
    return True, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="bearssl",
                    help="Vendored BearSSL root (default: bearssl)")
    ap.add_argument("--out-dir", default="tools",
                    help="Where to write outputs (default: tools)")
    args = ap.parse_args()

    if not os.path.isdir(args.root):
        sys.stderr.write("error: %s is not a directory\n" % args.root)
        sys.exit(2)

    # Walk under root, but classify uses paths *relative to root* (e.g.
    # "src/ssl/ssl_engine.c"), which is what the EXCLUDE patterns expect.
    all_srcs = walk_sources(args.root)

    included = []
    excluded = []
    for rel in all_srcs:
        keep, reason = classify(rel)
        if keep:
            included.append(rel)
        else:
            excluded.append((rel, reason))

    # Output paths: write them as "bearssl/src/..." so they slot directly
    # into the MacSurf.mcp source list which uses tree-relative paths.
    files_txt = os.path.join(args.out_dir, "bearssl_cw8_files.txt")
    with open(files_txt, "w") as f:
        f.write(
            "# BearSSL .c files for MacSurf.mcp (Stage A, CW8/PPC build)\n"
            "# Generated by tools/make_bearssl_filelist.py\n"
            "# Do not edit by hand. To regenerate:\n"
            "#   python3 tools/make_bearssl_filelist.py\n"
            "#\n"
            "# Count: %d files\n"
            "#\n\n"
            % len(included)
        )
        for rel in included:
            f.write("macSSL/bearssl/%s\n" % rel)

    excl_txt = os.path.join(args.out_dir, "bearssl_cw8_excluded.txt")
    with open(excl_txt, "w") as f:
        f.write(
            "# BearSSL .c files EXCLUDED from MacSurf.mcp (Stage A)\n"
            "# Generated by tools/make_bearssl_filelist.py\n"
            "#\n"
            "# Count: %d files\n"
            "#\n\n"
            % len(excluded)
        )
        for rel, reason in excluded:
            f.write("macSSL/bearssl/%s\n    %s\n\n" % (rel, reason))

    # Human-readable grouped manifest
    grouped_in = defaultdict(list)
    for rel in included:
        grouped_in[group_for(rel)].append(rel)

    grouped_out = defaultdict(list)
    for rel, reason in excluded:
        grouped_out[group_for(rel)].append((rel, reason))

    manifest = os.path.join(args.out_dir, "bearssl_cw8_manifest.md")
    with open(manifest, "w") as f:
        f.write("# BearSSL Stage A source manifest (CW8 / PPC)\n\n")
        f.write(
            "Generated by `tools/make_bearssl_filelist.py`. Do not edit.\n"
            "Regenerate with:\n\n"
            "```sh\n"
            "python3 tools/make_bearssl_filelist.py\n"
            "```\n\n"
        )
        f.write("## Summary\n\n")
        f.write("| Result | Count |\n|---|---:|\n")
        f.write("| Included in MacSurf.mcp | %d |\n" % len(included))
        f.write("| Excluded | %d |\n" % len(excluded))
        f.write("| Total scanned | %d |\n\n" % len(all_srcs))

        f.write("## Included (%d)\n\n" % len(included))
        for label, _ in GROUPS + [("other", None)]:
            paths = grouped_in.get(label, [])
            if not paths:
                continue
            f.write("### %s (%d)\n\n" % (label, len(paths)))
            for p in paths:
                f.write("- `macSSL/bearssl/%s`\n" % p)
            f.write("\n")

        f.write("## Excluded (%d)\n\n" % len(excluded))
        for label, _ in GROUPS + [("other", None)]:
            paths = grouped_out.get(label, [])
            if not paths:
                continue
            f.write("### %s (%d)\n\n" % (label, len(paths)))
            for p, reason in paths:
                f.write("- `macSSL/bearssl/%s` - %s\n" % (p, reason))
            f.write("\n")

    print("Wrote:")
    print("  %s   (%d entries)" % (files_txt, len(included)))
    print("  %s   (%d entries)" % (excl_txt, len(excluded)))
    print("  %s" % manifest)


if __name__ == "__main__":
    main()
