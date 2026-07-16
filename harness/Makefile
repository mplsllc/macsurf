# S0 — Linux + ASan reconvert-UAF harness.
# Builds the NetSurf render engine (libs + core html/box/reconvert) + QuickJS +
# the macos9 JS glue on Linux under AddressSanitizer, to reproduce the reconvert
# dom_string use-after-free deterministically. Linux dev tool only — NOT shipped.
#
# Usage: make -C harness          (build ./harness/reconvert_harness)
#        make -C harness run      (run it)
#        make -C harness clean

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
             build/macos9_content_registry.c build/misc_stub.c
HARN_SRC  := harness_stubs.c harness_gen_stubs.c driver.c

# object mapping (flatten paths)
obj_of = $(OBJ)/$(subst /,_,$(patsubst $(ROOT)/%,%,$1)).o
LIB_OBJ  := $(foreach s,$(LIB_SRC),$(call obj_of,$s))
CORE_OBJ := $(foreach s,$(CORE_SRC),$(call obj_of,$s))
GLUE_OBJ := $(OBJ)/glue_macsurf_qjs.o $(OBJ)/glue_macos9_reconvert.o
QJS_OBJ  := $(OBJ)/qjs_quickjs.o $(OBJ)/qjs_libregexp.o $(OBJ)/qjs_libunicode.o $(OBJ)/qjs_dtoa.o
HARN_OBJ := $(OBJ)/harness_stubs.o $(OBJ)/driver.o

ALL_OBJ := $(LIB_OBJ) $(CORE_OBJ) $(GLUE_OBJ) $(QJS_OBJ) $(HARN_OBJ)

reconvert_harness: $(ALL_OBJ)
	$(CC) $(ASAN) -pthread $(ALL_OBJ) -o $@ -lm

# generic rule via a per-source compile script would be cleaner; use explicit patterns:
$(OBJ)/%.o:
	@mkdir -p $(OBJ)
# lib + core objects (name-mangled): compiled by the phony 'objs' below

INC += -I$(LIBDIR)/libdom/bindings/hubbub

.PHONY: objs run clean
# object names are path-mangled (flatten). Compile each set with the right flags.
objs:
	@mkdir -p $(OBJ)
	@set -e; for s in $(LIB_SRC) $(CORE_SRC) $(EXTRA_SRC); do \
	  o=$(OBJ)/$$(echo $$s | sed "s|$(ROOT)/||; s|/|_|g").o; \
	  $(CC) -c $(CFLAGS) $(INC) $$s -o $$o; done
	$(CC) -c $(CFLAGS) $(INC) $(QJSFLAGS) $(QJS)/quickjs.c    -o $(OBJ)/qjs_quickjs.o
	$(CC) -c $(CFLAGS) $(INC) $(QJSFLAGS) $(QJS)/libregexp.c  -o $(OBJ)/qjs_libregexp.o
	$(CC) -c $(CFLAGS) $(INC) $(QJSFLAGS) $(QJS)/libunicode.c -o $(OBJ)/qjs_libunicode.o
	$(CC) -c $(CFLAGS) $(INC) $(QJSFLAGS) $(QJS)/dtoa.c       -o $(OBJ)/qjs_dtoa.o
	$(CC) -c $(CFLAGS) -DWITH_QUICKJS $(INC) build/macsurf_qjs.c        -o $(OBJ)/glue_macsurf_qjs.o
	$(CC) -c $(CFLAGS) -DWITH_QUICKJS $(INC) build/macsurf_dom_dispatch.c -o $(OBJ)/glue_macsurf_dom_dispatch.o
	$(CC) -c $(CFLAGS) $(INC) build/macos9_reconvert.c  -o $(OBJ)/glue_macos9_reconvert.o
	$(CC) -c $(CFLAGS) $(INC) build/macos9_content_registry.c -o $(OBJ)/glue_macos9_content_registry.o
	$(CC) -c $(CFLAGS) $(INC) build/misc_stub.c -o $(OBJ)/glue_misc_stub.o
	$(CC) -c $(CFLAGS) $(INC) harness_stubs.c     -o $(OBJ)/harness_stubs.o
	$(CC) -c $(CFLAGS) $(INC) harness_gen_stubs.c -o $(OBJ)/harness_gen_stubs.o
	$(CC) -c $(CFLAGS) $(INC) driver.c            -o $(OBJ)/driver.o

link: objs
	$(CC) $(ASAN) -pthread $(OBJ)/*.o -o reconvert_harness -lz -lm

run: link
	./reconvert_harness

clean:
	rm -rf $(OBJ) reconvert_harness
