#!/bin/bash
# Deduped .c list for the harness. CW8 rename left a prefixed live copy
# (p_/s_/hub_/dom_/cssh_/html_) + a stale non-prefixed twin on disk; keep prefixed.
# Drop *_gen.c codegen (own main) + Mac/config-only utils. libs recurse (nested src);
# core is top-level per dir (avoid image/javascript/fetchers handlers not needed here).
what=$1; base=$2
prefixes="p_ s_ hub_ dom_ cssh_ html_"
if [ "$what" = libs ]; then
  FILES=$(find $base/libwapcaplet/src $base/libparserutils/src $base/libhubbub/src \
               $base/libdom/src $base/libcss/src -name '*.c' 2>/dev/null | grep -v /__pycache__/)
else
  FILES=$(find $base/content/handlers/html $base/content/handlers/css $base/content $base/utils \
               -maxdepth 1 -name '*.c' 2>/dev/null)
fi
for f in $FILES; do
  b=$(basename "$f"); d=$(dirname "$f")
  case "$b" in *_gen.c) continue;; esac
  if [ "$what" = core ]; then case "$b" in log.c|ns_file.c|nsoption.c) continue;; esac; fi
  skip=0
  case "$b" in p_*|s_*|hub_*|dom_*|cssh_*|html_*) ;; *)
    for p in $prefixes; do [ -f "$d/$p$b" ] && skip=1 && break; done;; esac
  [ $skip = 0 ] && echo "$f"
done
