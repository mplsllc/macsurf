# S0 — Linux + ASan reconvert-UAF harness.
# Builds the NetSurf render engine (libs + core html/box/reconvert) + QuickJS +
# the macos9 JS glue on Linux under AddressSanitizer, to reproduce the reconvert
# dom_string use-after-free deterministically. Linux dev tool only — NOT shipped.
#
# Usage: make -C harness          (build ./harness/reconvert_harness)
#        make -C harness run      (build + run it)
#        make -C harness clean
#
# Incremental: every object has its source as a real prerequisite plus -MMD
# header deps, so touching a header (e.g. html.h's struct html_script) rebuilds
# its dependents. If you ever find yourself reasoning about whether the binary
# picked up an edit, `make clean` -- but fix the dependency instead.

# The per-source rules below are generated with $(eval), so the first of THEM
# would otherwise become the default goal (make takes the first target it sees).
# Pin it explicitly.
.DEFAULT_GOAL := reconvert_harness

ROOT   := $(abspath ..)
NS     := $(ROOT)/browser/netsurf
LIBDIR := $(ROOT)/browser
QJS    := $(ROOT)/quickjs-macos9
OBJ    := obj

CC     := gcc
ASAN   := -fsanitize=address
CFLAGS := -O0 -g $(ASAN) -std=gnu99 -w -include stdint.h -include stdbool.h \
          -include include/macsurf_safe_alloc_decls.h

# include set: harness stubs FIRST (shadow Mac headers), then netsurf, real libs, quickjs
INC := -Iinclude \
       -I$(NS)/include -I$(NS) -I$(NS)/content/handlers \
       -I$(LIBDIR)/libwapcaplet/include -I$(LIBDIR)/libparserutils/include \
       -I$(LIBDIR)/libhubbub/include -I$(LIBDIR)/libdom/include -I$(LIBDIR)/libcss/include \
       -I$(QJS)
# libdom/libcss internal src subdirs (internal headers)
INC += $(addprefix -I,$(shell find $(LIBDIR)/libparserutils/src $(LIBDIR)/libhubbub/src \
        $(LIBDIR)/libdom/src $(LIBDIR)/libcss/src $(LIBDIR)/libwapcaplet/src -type d | grep -v __pycache__))
INC += -I$(LIBDIR)/libdom/binding

# QuickJS needs pthread + its own knobs
QJSFLAGS := -pthread -D_GNU_SOURCE -include pthread.h -DCONFIG_VERSION='"harness"'

# --- source sets ---
# CW8 flat-namespace rename left BOTH a prefixed "live" copy (p_/s_/hub_/dom_/cssh_/html_)
# and a stale non-prefixed original on disk. dedup.sh keeps the prefixed (live) copy,
# drops the stale twin, drops *_gen.c codegen (own main()), and the Mac/config-only utils.
LIB_SRC   := $(shell ./dedup.sh libs   $(LIBDIR))
CORE_SRC  := $(shell ./dedup.sh core   $(NS))
EXTRA_SRC := $(LIBDIR)/libdom/bindings/hubbub/dom_parser.c \
             $(wildcard $(NS)/utils/http/*.c) $(NS)/utils/corestrings.c \
             $(NS)/utils/nsurl/nsurl.c $(NS)/utils/nsurl/nsurl_parse.c
QJS_SRC   := $(QJS)/quickjs.c $(QJS)/libregexp.c $(QJS)/libunicode.c $(QJS)/dtoa.c
GLUE_SRC  := build/macsurf_qjs.c build/macos9_reconvert.c build/macsurf_dom_dispatch.c \
             build/macos9_content_registry.c build/misc_stub.c build/macos9_js_fetch.c
HARN_SRC  := harness_stubs.c harness_gen_stubs.c driver.c

INC += -I$(LIBDIR)/libdom/bindings/hubbub

# --- object mapping ---------------------------------------------------------
# Objects are FLAT (paths mangled to underscores) because sources come from a
# dozen directories and CW8's flat namespace is what the Mac build mirrors.
obj_of  = $(OBJ)/$(subst /,_,$(patsubst $(ROOT)/%,%,$1)).o
qjs_of  = $(OBJ)/qjs_$(notdir $(1:.c=.o))
glue_of = $(OBJ)/glue_$(notdir $(1:.c=.o))
harn_of = $(OBJ)/$(notdir $(1:.c=.o))

# Glue splits on ONE flag: the QuickJS-facing files need -DWITH_QUICKJS (it gates
# the whole engine, see macsurf_prefix.h), the rest must NOT have it.
GLUE_QJS_SRC   := build/macsurf_qjs.c build/macsurf_dom_dispatch.c build/macos9_js_fetch.c
GLUE_PLAIN_SRC := build/macos9_reconvert.c build/macos9_content_registry.c build/misc_stub.c

# $(sort) dedupes: dedup.sh's `core` set and EXTRA_SRC both name corestrings.c,
# and two rules for one object is a "overriding recipe" warning at best.
BASE_SRC := $(sort $(LIB_SRC) $(CORE_SRC) $(EXTRA_SRC))

BASE_OBJ  := $(foreach s,$(BASE_SRC),$(call obj_of,$s))
QJS_OBJ   := $(foreach s,$(QJS_SRC),$(call qjs_of,$s))
GLUE_OBJ  := $(foreach s,$(GLUE_QJS_SRC) $(GLUE_PLAIN_SRC),$(call glue_of,$s))
HARN_OBJ  := $(foreach s,$(HARN_SRC),$(call harn_of,$s))

ALL_OBJ := $(BASE_OBJ) $(QJS_OBJ) $(GLUE_OBJ) $(HARN_OBJ)

# --- rules ------------------------------------------------------------------
# One rule per source, generated, each with its source as a REAL prerequisite
# and -MMD header deps. This replaces a phony `objs` target whose recipe
# rebuilt every file unconditionally while the default goal
# (`reconvert_harness: $(ALL_OBJ)`) had an EMPTY compile recipe -- so a bare
# `make` (the invocation in this file's own usage note, and in the plan's
# checklist) compiled NOTHING, silently relinked nothing, and reported success.
# A stale binary then re-ran the previous run's tests and printed all-PASS:
# edits to driver.c could not fail, which is the exact false-green this harness
# exists to prevent. ALL_OBJ was also missing six objects, which is why the old
# link globbed $(OBJ)/*.o rather than trusting it.
define COMPILE_RULE
$(2): $(1) | $$(OBJ)
	$$(CC) -c $$(CFLAGS) -MMD -MP $$(INC) $(3) $$< -o $$@
endef

$(foreach s,$(BASE_SRC),$(eval $(call COMPILE_RULE,$s,$(call obj_of,$s),)))
$(foreach s,$(QJS_SRC),$(eval $(call COMPILE_RULE,$s,$(call qjs_of,$s),$$(QJSFLAGS))))
$(foreach s,$(GLUE_QJS_SRC),$(eval $(call COMPILE_RULE,$s,$(call glue_of,$s),-DWITH_QUICKJS)))
$(foreach s,$(GLUE_PLAIN_SRC),$(eval $(call COMPILE_RULE,$s,$(call glue_of,$s),)))
$(foreach s,$(HARN_SRC),$(eval $(call COMPILE_RULE,$s,$(call harn_of,$s),)))

$(OBJ):
	@mkdir -p $(OBJ)

reconvert_harness: $(ALL_OBJ)
	$(CC) $(ASAN) -pthread $(ALL_OBJ) -o $@ -lz -lm

-include $(ALL_OBJ:.o=.d)

.PHONY: all run clean
all: reconvert_harness

# Kept as aliases: `make link` / `make objs` are in muscle memory and in older
# notes. They now mean the same thing as the default goal instead of being the
# only invocations that actually built anything.
link objs: reconvert_harness

run: reconvert_harness
	./reconvert_harness

clean:
	rm -rf $(OBJ) reconvert_harness
