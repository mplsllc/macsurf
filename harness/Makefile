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
# fixes1022: the harness tests the FULL JS surface -- the Mac build quiesces
# these behind default-off switches until Phase 3 lands.
CFLAGS := -O0 -g $(ASAN) -std=gnu99 -w -include stdint.h -include stdbool.h \
          -DMACSURF_JS_GEOMETRY=1 -DMACSURF_JS_VIEW_EVENTS=1 \
          -DMACSURF_JS_FIRE_LOAD=1 -DMACSURF_JS_AUDIT=1 \
          -DMACSURF_RECONVERT_TEST_HOOK \
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
# fixes890 — the REAL desktop/scrollbar.c, not harness_gen_stubs' empty
# `void scrollbar_get_offset(void){}`. box_at_point calls scrollbar_get_offset
# on every box under the cursor, and the hover crash on hardware is that
# function dereferencing a wild pointer -- which a stub can never reproduce or
# verify. Test 27 needs the real accessors.
EXTRA_SRC := $(LIBDIR)/libdom/bindings/hubbub/dom_parser.c \
             $(wildcard $(NS)/utils/http/*.c) $(NS)/utils/corestrings.c \
             $(NS)/utils/nsurl/nsurl.c $(NS)/utils/nsurl/nsurl_parse.c \
             $(NS)/desktop/scrollbar.c
QJS_SRC   := $(QJS)/quickjs.c $(QJS)/libregexp.c $(QJS)/libunicode.c $(QJS)/dtoa.c
GLUE_SRC  := build/macsurf_qjs.c build/macos9_reconvert.c build/macsurf_dom_dispatch.c \
             build/macos9_content_registry.c build/misc_stub.c build/macos9_js_fetch.c
# fixes1268a (#167): cssprobe.c includes libcss's PRIVATE headers, whose
# enumerators collide with netsurf's in driver.c -- it must stay its own TU.
HARN_SRC  := harness_stubs.c harness_gen_stubs.c driver.c cssprobe.c

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
GLUE_QJS_SRC   := build/macsurf_qjs.c build/macsurf_dom_dispatch.c \
                  build/macos9_js_fetch.c build/macsurf_qjs_audit.c
GLUE_PLAIN_SRC := build/macos9_reconvert.c build/macos9_content_registry.c \
                  build/misc_stub.c build/macsurf_lwc_compat.c \
                  build/macsurf_gap.c build/macsurf_diag.c build/macsurf_trace.c

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

.PHONY: all run clean check check-c89 check-macdefault diag-phase2 webp-test
all: reconvert_harness

diag-phase2: reconvert_harness
	ASAN_OPTIONS=detect_leaks=0 ./reconvert_harness --diag-phase2

# Standalone libwebp regression.  This deliberately stays out of the large
# NetSurf/ASan harness: it exercises the exact decoder/demux source subset and
# its still/alpha/animation fixtures without needing Carbon bitmap stubs.
WEBP_DIR := $(ROOT)/browser/libwebp
WEBP_SRC := $(sort $(wildcard $(WEBP_DIR)/c/*.c))

webp_codec_test: webp_codec_test.c $(WEBP_SRC)
	$(CC) -std=c89 -O2 -U__SSE2__ -U__SSE2_MATH__ -I$(WEBP_DIR)/h $^ -o $@

webp-test: webp_codec_test
	./webp_codec_test

# fixes1023 — COMPILE THE MAC'S ACTUAL CONFIGURATION.
#
# The harness forces the fixes1022 quiesce switches ON, so it can never see a
# break that only exists with them OFF -- which is exactly what shipped in
# fixes1022: the switch #defines sat 160 lines BELOW two use sites, so with
# them off the preprocessor left bare identifiers and CW8 died on "undefined".
# Neither Linux check caught it: plain gcc cannot get past line 37 of this
# file (macos9.h needs Carbon), and the harness had the switches defined.
#
# This target compiles the gated sources with the SAME command line the
# harness uses MINUS the four -D switches, i.e. the configuration the Mac
# actually builds. Syntax-only, seconds to run.
#
# Verified with a control: renaming one #define makes it report
#   error: 'MACSURF_JS_VIEW_EVENTS' undeclared
# Run it before every drop that touches a switch or its use sites.
MACDEFAULT_STRIP := -DMACSURF_JS_GEOMETRY=1 -DMACSURF_JS_VIEW_EVENTS=1 \
                    -DMACSURF_JS_FIRE_LOAD=1 -DMACSURF_JS_AUDIT=1
check-macdefault: check-c89
	@echo "== compiling glue in the MAC's config (quiesce switches OFF) =="
	@$(CC) -fsyntax-only $(filter-out $(MACDEFAULT_STRIP),$(CFLAGS)) \
		-DWITH_QUICKJS $(INC) build/macsurf_qjs.c && echo "   macsurf_qjs.c OK"
	@$(CC) -fsyntax-only $(filter-out $(MACDEFAULT_STRIP),$(CFLAGS)) \
		-DWITH_QUICKJS $(INC) build/macsurf_dom_dispatch.c && echo "   macsurf_dom_dispatch.c OK"
	@$(CC) -fsyntax-only $(filter-out $(MACDEFAULT_STRIP),$(CFLAGS)) \
		-DWITH_QUICKJS $(INC) build/macos9_js_fetch.c && echo "   macos9_js_fetch.c OK"

# fixes1071a — MIXED DECLARATIONS, the one C89 rule this harness cannot see.
#
# CodeWarrior compiles the shipped tree as C89: every declaration must precede
# every statement in its block. This harness builds with -std=gnu99, so a
# statement placed above a declaration compiles clean here and is REJECTED on
# the Mac. fixes1071 shipped exactly that -- one `macsurf_qjs_ncalls++;` put
# immediately after the opening brace of js_call_c_function -- and CW8 answered
# with ~55 errors cascading down the function. Nothing on Linux could have
# caught it, which is why it is a Makefile target now and not a habit.
#
# -Wdeclaration-after-statement is the exact detector, and the baseline is
# ZERO warnings across both files, so this is a zero-noise gate: any hit is
# ours. Verified as a real control -- reinstating the fixes1071 placement makes
# this target fail.
#
# quickjs.c genuinely needs C99 to compile, so we do NOT build it as C89; we
# only ask GCC to point at mixed declarations, which is the rule CW8 enforces.
#
# NOTE the absence of -w. The first version of this target had it (copied from
# CFLAGS) and reported "OK" with the bug deliberately reinstated: -w inhibits
# ALL warnings and beats a later -W flag, so the gate was measuring nothing and
# its zero baseline was itself a false green. Caught only because the control
# was run. -Werror=<name> makes exactly this one diagnostic fatal and leaves
# any incidental warning as non-fatal noise.
C89CHK := -fsyntax-only -std=gnu99 -Werror=declaration-after-statement
check-c89:
	@echo "== C89 mixed-declaration check (CW8 rule; -std=gnu99 cannot see it) =="
	@$(CC) $(C89CHK) -I$(QJS) $(QJSFLAGS) $(QJS)/quickjs.c \
		&& echo "   quickjs.c OK"
	@$(CC) $(C89CHK) $(filter-out -w,$(CFLAGS)) -DWITH_QUICKJS $(INC) \
		build/macsurf_qjs.c && echo "   macsurf_qjs.c OK"
	@$(CC) $(C89CHK) $(filter-out -w,$(CFLAGS)) -DWITH_QUICKJS $(INC) \
		build/macsurf_qjs_audit.c && echo "   macsurf_qjs_audit.c OK"
	@$(CC) $(C89CHK) $(filter-out -w,$(CFLAGS)) -DWITH_QUICKJS $(INC) \
		build/macsurf_dom_dispatch.c && echo "   macsurf_dom_dispatch.c OK"
	@$(CC) $(C89CHK) $(filter-out -w,$(CFLAGS)) -DWITH_QUICKJS $(INC) \
		build/macos9_js_fetch.c && echo "   macos9_js_fetch.c OK"
	@$(CC) $(C89CHK) $(filter-out -w,$(CFLAGS)) $(INC) \
		build/macos9_reconvert.c && echo "   macos9_reconvert.c OK"
	@$(CC) $(C89CHK) $(filter-out -w,$(CFLAGS)) $(INC) \
		build/macos9_content_registry.c && echo "   macos9_content_registry.c OK"
	@$(CC) $(C89CHK) $(filter-out -w,$(CFLAGS)) $(INC) \
		build/macsurf_diag.c && echo "   macsurf_diag.c OK"
# fixes1093 — html.c joins the gate. It was never covered, yet it is the other
# file this project edits constantly (the pagemap, the reconvert resize/load
# fire, and now the slider probe all live here) and it ships to CW8 as C89
# exactly like macsurf_qjs.c does. The pre-existing implicit-declaration
# warnings below are NOT suppressed on purpose: -w is what made this target a
# false green once already, and implicit decls are their own CW8 hazard class.
	@$(CC) $(C89CHK) $(filter-out -w,$(CFLAGS)) $(INC) \
		$(NS)/content/handlers/html/html.c && echo "   html.c OK"
# fixes1137 — textplain.c joins the gate when the real text/plain handler
# (#233) is in the tree. Removed with the fixes1181-1183 revert batch; re-add
# when textplain.c is re-added.

# Kept as aliases: `make link` / `make objs` are in muscle memory and in older
# notes. They now mean the same thing as the default goal instead of being the
# only invocations that actually built anything.
link objs: reconvert_harness

run: reconvert_harness
	ASAN_OPTIONS=detect_leaks=0 ./reconvert_harness

check: check-macdefault run

clean:
	rm -rf $(OBJ) reconvert_harness
