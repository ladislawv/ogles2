# ogles2.library, libogles2.a and their tests. Run in WSL:  make [-j]
# Output goes to $(B) (default ~/ogles2-build/m68k), outside the source tree.
#
# Toolchain: GCC 6.5.0b + clib2 headers (plan section 2.3), the compiler
# PiStorm3D 29.1 is built with. The library is freestanding: no C library
# startup, no libc; exec/dos/utility are called directly.

B      ?= $(HOME)/ogles2-build/m68k
TC     ?= $(HOME)/amiga-gcc-install
CROSS  := $(TC)/bin/m68k-amigaos-
CC     := $(CROSS)gcc
AR     := $(CROSS)ar
STRIP  := $(CROSS)strip
PYTHON ?= python3

# -fbbb=...: bebbo's optimiser without "e" (eliminate dead assignments +
# redundant loads), which deletes the fintrz of every float->integer
# conversion, so (int)255.5f became 256 (found on the A1200 in M1 run 1;
# tools/check_fpconv.py, plan section 2.3). The other letters are the default.
CPU    := -mcpu=68020 -m68881 -fbbb=abcfhilmnprsz0
# EXTRA_CFLAGS=-DOGLES2_LOG_SYNC: write every log line at once (hardware bring-up:
# a hang loses nothing). Use its own B, e.g. B=~/ogles2-build/m68k-sync.
CFLAGS := -std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing $(CPU) -mcrt=clib2 \
          $(EXTRA_CFLAGS)
INC    := -Iinclude -I$(B)/gen/include -Iogles2 -I$(B)/gen/ogles2
# Library objects must not pull C library or startup code.
LIBLINK := $(CPU) -mcrt=clib2 -nostdlib -nostartfiles -nodefaultlibs -noixemul

# v3dcore: PiStorm3D's backend, built with PiStorm3D 29.1's own flags.
# -fno-strict-aliasing is required (v3d_commands.c's packed writes).
V3D_FLAGS := -std=c99 -O2 -fno-strict-aliasing -fno-builtin-cos -fno-builtin-sin -finline-functions \
             -DMGLV3D_NO_LOGGING $(CPU) -mcrt=clib2 -Iv3dcore/include
V3D_SRC   := v3d_assembler v3d_clbuf v3d_commands v3d_context v3d_device v3d_frame v3d_hw \
             v3d_mem_allocvec v3d_submit_timeout v3d_texture v3d_debug v3d_gpu_lock v3d_tfu
V3D_OBJ   := $(V3D_SRC:%=$(B)/obj/v3dcore/%.o)
V3D_HDR   := $(wildcard v3dcore/include/*.h v3dcore/hw/*.h)

GEN_STAMP := $(B)/gen/.stamp
LIB_SRC   := ogles2/library.c ogles2/log.c ogles2/context.c ogles2/device.c ogles2/runtime.c \
             ogles2/state.c ogles2/display.c ogles2/job.c ogles2/frame.c ogles2/program.c \
             ogles2/draw.c ogles2/v3dp.c ogles2/blake3.c ogles2/texture.c \
             ogles2/buffer.c ogles2/compiler.c ogles2/etc1.c ogles2/fbo.c ogles2/share.c
# Library files that use v3dcore directly
V3D_USERS := device job draw v3dp texture buffer fbo share
GEN_LIB   := $(B)/gen/ogles2/dispatch_table.c $(B)/gen/ogles2/stubs.c
CLIENT_SRC := client/ogles2_open.c client/agl.c
GEN_CLIENT := $(B)/gen/client/agl_gen.c $(B)/gen/client/procaddr.c

LIB_OBJ    := $(B)/obj/ogles2/start.o $(patsubst %.c,$(B)/obj/%.o,$(LIB_SRC)) \
              $(B)/obj/gen/dispatch_table.o $(B)/obj/gen/stubs.o
CLIENT_OBJ := $(patsubst %.c,$(B)/obj/%.o,$(CLIENT_SRC)) $(B)/obj/client/lvo.o \
              $(B)/obj/gen/agl_gen.o $(B)/obj/gen/procaddr.o

.PHONY: all gen clean vbcc
all: $(B)/fpcheck.ok $(B)/libv3dcore.a $(B)/ogles2.library $(B)/libogles2.a $(B)/tests/abi_layout $(B)/tests/smoke \
     $(B)/tests/abi_probe $(B)/tests/m1_clear $(B)/tests/m1_probe $(B)/tests/m1_draw $(B)/tests/m1_hwprobe $(B)/tests/m1_soak $(B)/tests/m2_link $(B)/tests/m2_texture $(B)/tests/m2_elements $(B)/tests/m2_buffers $(B)/tests/m4_stress $(B)/tests/m4_depth $(B)/tests/m4_setbitmap $(B)/tests/gears_gles2 $(B)/tests/cachetime $(B)/tests/emuspeed $(B)/tests/m5_fbo \
     $(B)/tests/rtt_churn $(B)/tests/m6_share $(B)/tests/m6_tasks $(B)/tests/m6_ext $(B)/tests/m6_tfu $(B)/cmd/OGLES2Log $(B)/amictl/amictld

# ---- generated code (plan section 6.6). Re-run when the inputs or the set of
# implemented ogles2_* functions change.
$(GEN_STAMP): gen/gen_ogles2.py gen/api.txt gen/agl.txt gen/abi_order.txt gen/khronos/gl.xml $(LIB_SRC)
	$(PYTHON) gen/gen_ogles2.py --out $(B)/gen --impl-dir ogles2
	@touch $@
$(GEN_LIB) $(GEN_CLIENT) $(B)/gen/tests/abi_layout.c: $(GEN_STAMP)
gen: $(GEN_STAMP)

# ---- float->integer check: everything in the library compiled to assembly,
# no conversion without fintrz (see CPU above; tools/check_fpconv.py).
$(B)/fpcheck.ok: $(LIB_SRC) $(V3D_SRC:%=v3dcore/hw/%.c) $(V3D_HDR) tools/check_fpconv.py $(GEN_STAMP) Makefile
	@mkdir -p $(B)/asm
	@for f in $(LIB_SRC); do \
	  $(CC) $(CFLAGS) $(INC) -Iv3dcore/include -Iv3dcore/hw -S $$f -o $(B)/asm/$$(basename $$f .c).s || exit 1; \
	done
	@for f in $(V3D_SRC); do \
	  $(CC) $(V3D_FLAGS) -S v3dcore/hw/$$f.c -o $(B)/asm/v3dcore_$$f.s || exit 1; \
	done
	$(PYTHON) tools/check_fpconv.py $(B)/asm/*.s
	@touch $@

# ---- v3dcore
$(B)/obj/v3dcore/v3d_debug.o: v3dcore/hw/v3d_debug.c $(V3D_HDR) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(V3D_FLAGS) -DDEBUG -c $< -o $@
$(B)/obj/v3dcore/%.o: v3dcore/hw/%.c $(V3D_HDR) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(V3D_FLAGS) -c $< -o $@
$(B)/libv3dcore.a: $(V3D_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

# ---- library
$(V3D_USERS:%=$(B)/obj/ogles2/%.o): $(B)/obj/ogles2/%.o: ogles2/%.c ogles2/ogles2_internal.h $(V3D_HDR) $(GEN_STAMP) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -Iv3dcore/include -Iv3dcore/hw -c $< -o $@
$(B)/obj/ogles2/start.o: ogles2/start.s
	@mkdir -p $(dir $@)
	$(CC) $(CPU) -c $< -o $@
$(B)/obj/ogles2/%.o: ogles2/%.c ogles2/ogles2_internal.h $(GEN_STAMP) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@
$(B)/obj/gen/%.o: $(B)/gen/ogles2/%.c $(GEN_STAMP) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

# start.o first: the file's first code is "return -1" (start.s).
$(B)/ogles2.library.debug: $(LIB_OBJ) $(B)/libv3dcore.a
	$(CC) $(LIBLINK) -Wl,-Map,$(B)/ogles2.library.map -o $@ $(LIB_OBJ) $(B)/libv3dcore.a -lgcc
# The linked table, checked slot by slot against abi_order.txt before stripping.
$(B)/ogles2.library: $(B)/ogles2.library.debug gen/verify_table.py gen/abi_order.txt
	$(PYTHON) gen/verify_table.py $< gen/abi_order.txt
	$(STRIP) -o $@ $<

# ---- import library
$(B)/obj/client/%.o: client/%.c $(GEN_STAMP) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@
$(B)/obj/client/lvo.o: client/lvo.s
	@mkdir -p $(dir $@)
	$(CC) $(CPU) -c $< -o $@
$(B)/obj/gen/%.o: $(B)/gen/client/%.c $(GEN_STAMP) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@
$(B)/libogles2.a: $(CLIENT_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

# ---- tests (Amiga programs)
$(B)/tests/abi_layout: $(B)/gen/tests/abi_layout.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -o $@
$(B)/tests/smoke: tests/smoke/smoke.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@

$(B)/tests/abi_probe: tests/abi_probe/abi_probe.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@
$(B)/tests/m1_clear: tests/m1_clear/m1_clear.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@
$(B)/tests/m1_probe: tests/m1_probe/m1_probe.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@
$(B)/tests/m1_hwprobe: tests/m1_hwprobe/m1_hwprobe.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@
$(B)/tests/m2_link: tests/m2_link/m2_link.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@
$(B)/tests/m2_elements: tests/m2_elements/m2_elements.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@
$(B)/tests/m2_buffers: tests/m2_buffers/m2_buffers.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@
$(B)/tests/m4_stress: tests/m4_stress/m4_stress.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@
$(B)/tests/m4_depth: tests/m4_depth/m4_depth.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@
$(B)/tests/cachetime: tests/perf/cachetime.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-builtin $< -o $@
$(B)/tests/emuspeed: tests/perf/emuspeed.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@
$(B)/tests/m5_fbo: tests/m5_fbo/m5_fbo.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@
$(B)/tests/m6_share: tests/m6_share/m6_share.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@
$(B)/tests/m6_tasks: tests/m6_share/m6_tasks.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@
$(B)/tests/m6_ext: tests/m6_ext/m6_ext.c tests/common/ext_shaders.h $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@
$(B)/tests/m6_tfu: tests/m6_tfu/m6_tfu.c tests/common/smooth_shaders.h tests/common/tex_shaders.h \
                   $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@
$(B)/tests/m4_setbitmap: tests/m4_setbitmap/m4_setbitmap.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@
$(B)/tests/gears_gles2: tests/perf/gears_gles2.c tests/perf/perf_gears.h tests/perf/perf_target.h \
                       tests/common/lit_shaders.h $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-builtin $(INC) $< -L$(B) -logles2 -lm881 -o $@
$(B)/tests/rtt_churn: tests/perf/rtt_churn.c tests/perf/perf_gears.h tests/perf/perf_target.h $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-builtin $(INC) $< -L$(B) -logles2 -lm881 -o $@

# ---- gl4es (desktop OpenGL on GLES2, M6): ports/gl4es builds libgl4es.a from
# gl4es's sources (cloned into GL4ES_SRC); gears_gl4es is gears_mgl.c on it.
GL4ES_SRC ?= $(HOME)/gl4es
.PHONY: gl4es
gl4es: $(B)/libogles2.a $(GEN_STAMP)
	B=$(B) GL4ES_SRC=$(GL4ES_SRC) bash ports/gl4es/build.sh
	@mkdir -p $(B)/tests
	$(CC) $(CFLAGS) -fno-builtin -DGEARS_GL4ES -I$(GL4ES_SRC)/include -Iports/gl4es/include $(INC) 	    tests/perf/gears_mgl.c -L$(B)/gl4es -L$(B) -lgl4es -logles2 -lm881 -o $(B)/tests/gears_gl4es

# ---- the MiniGL side of the speed comparison (tests/perf): needs the SDK
# directory of PiStorm3D 29.1 (Aminet util/libs/PiStorm3D.lha, SDK/).
MGL_SDK ?= $(HOME)/minigl-29.1/SDK
.PHONY: perf-mgl
perf-mgl: $(B)/tests/gears_mgl
$(B)/tests/gears_mgl: tests/perf/gears_mgl.c tests/perf/perf_gears.h tests/perf/perf_target.h
	@test -f $(MGL_SDK)/lib/libminigl.a || { echo "MGL_SDK=$(MGL_SDK): no PiStorm3D 29.1 SDK there"; exit 1; }
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-builtin -Wno-sign-compare -I$(MGL_SDK)/minigl-shared-library/include $< -L$(MGL_SDK)/lib -lminigl -lm881 -o $@

$(B)/tests/m2_texture: tests/m2_texture/m2_texture.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@
$(B)/tests/m1_soak: tests/m1_soak/m1_soak.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-builtin $(INC) $< -L$(B) -logles2 -lm881 -o $@
$(B)/tests/m1_draw: tests/m1_draw/m1_draw.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@

# ---- vbcc clients (plan section 6.3): libogles2 built with vbcc, and the
# smoke test and ABI probe built with it, against the same GCC library.
# make vbcc   (needs vbcc in $(VBCC): dev/host-setup/build_vbcc.sh)
VBCC   ?= $(HOME)/vbcc
NDK    ?= $(TC)/m68k-amigaos/ndk-include
VC     := PATH=$(VBCC)/bin:$$PATH VBCC=$(VBCC) $(VBCC)/bin/vc +aos68k
VCFLAGS := -c99 -cpu=68020 -fpu=68881 -O1 -Iinclude -I$(B)/gen/include -I$(NDK)
VB     := $(B)/vbcc
VCLIENT_OBJ := $(VB)/obj/ogles2_open.o $(VB)/obj/agl.o $(VB)/obj/agl_gen.o $(VB)/obj/procaddr.o \
               $(VB)/obj/lvo_vasm.o

vbcc: $(VB)/ogles2.lib $(VB)/tests/smoke $(VB)/tests/abi_probe
$(VB)/obj/%.o: client/%.c $(GEN_STAMP)
	@mkdir -p $(dir $@)
	$(VC) $(VCFLAGS) -c $< -o $@
$(VB)/obj/%.o: $(B)/gen/client/%.c $(GEN_STAMP)
	@mkdir -p $(dir $@)
	$(VC) $(VCFLAGS) -c $< -o $@
$(VB)/obj/lvo_vasm.o: client/lvo_vasm.s
	@mkdir -p $(dir $@)
	$(VBCC)/bin/vasmm68k_mot -quiet -Fhunk -m68020 $< -o $@
# An AmigaOS link library: the hunk objects one after another.
$(VB)/ogles2.lib: $(VCLIENT_OBJ)
	cat $^ > $@
$(VB)/tests/smoke: tests/smoke/smoke.c $(VB)/ogles2.lib
	@mkdir -p $(dir $@)
	$(VC) $(VCFLAGS) $< $(VB)/ogles2.lib -lamiga -lm881 -o $@
$(VB)/tests/abi_probe: tests/abi_probe/abi_probe.c $(VB)/ogles2.lib
	@mkdir -p $(dir $@)
	$(VC) $(VCFLAGS) $< $(VB)/ogles2.lib -lamiga -lm881 -o $@

# ---- remote test station (tools/amictl, plan section 10.1)
AMICTL_SRC := tools/amictl/server/amictld.c tools/amictl/server/amictl_proto.h tools/amictl/server/os.h
$(B)/amictl/amictld: $(AMICTL_SRC) tools/amictl/server/os_amiga.c Makefile
	@mkdir -p $(dir $@)
	$(CC) -std=gnu99 -O2 -Wall -Wno-pointer-sign -mcpu=68020 -mcrt=nix20 -fbbb=abcfhilmnprsz0 \
	    -o $@ tools/amictl/server/amictld.c tools/amictl/server/os_amiga.c
$(B)/amictl/amictld_host: $(AMICTL_SRC) tools/amictl/server/os_posix.c Makefile
	@mkdir -p $(dir $@)
	gcc -std=c99 -O2 -Wall -Wextra -o $@ tools/amictl/server/amictld.c tools/amictl/server/os_posix.c
.PHONY: amictl-test
amictl-test: $(B)/amictl/amictld_host
	$(PYTHON) tools/amictl/test_host.py $<

# ---- the built-in program's machine code (plan section 9, M1): PiStorm3D's
# assembler run under vamos, so the library carries neither the assembler nor
# its shader sources. The output is checked in; rerun only if the shaders change.
VAMOS ?= $(HOME)/amitools-venv/bin/vamos
$(B)/tools/mkshaders: tools/builtin_shaders/mkshaders.c $(B)/libv3dcore.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iv3dcore/include $< -L$(B) -lv3dcore -o $@
.PHONY: builtin-shaders
builtin-shaders: $(B)/tools/mkshaders
	cd $(dir $<) && $(VAMOS) -C 68020 -- mkshaders > "$(CURDIR)/ogles2/builtin_shaders.h.new"
	mv ogles2/builtin_shaders.h.new ogles2/builtin_shaders.h

# ---- the test programs' binaries (plan 8.2): the GLSL of tests/common,
# compiled by v3dglslc (host build) and named <hash>.v3dp, as the runtime
# looks them up. Packages ship $(B)/v3dp/*.v3dp next to the tests.
V3DGLSLC ?= $(HOME)/ogles2-build/v3dglslc/host/v3dglslc
.PHONY: test-v3dp
test-v3dp: $(B)/v3dp/.stamp
$(B)/v3dp/.stamp: $(wildcard tests/common/*.h) tools/v3dp_tests/emit.c v3dp/v3dp_hashname.py \
               $(wildcard tools/v3dglslc/test/shaders/*)
	@mkdir -p $(B)/v3dp/src
	rm -f $(B)/v3dp/*.v3dp $(B)/v3dp/*.v3dw
	gcc -O2 -o $(B)/v3dp/emit tools/v3dp_tests/emit.c
	cp tools/v3dglslc/test/shaders/light.* tools/v3dglslc/test/shaders/skin.* \
	    tools/v3dglslc/test/shaders/state.* tools/v3dglslc/test/shaders/tex.* $(B)/v3dp/src/
	cd $(B)/v3dp/src && ../emit && for v in *.vert; do \
	    $(V3DGLSLC) -o .. $$v $${v%.vert}.frag || exit 1; done
	$(PYTHON) v3dp/v3dp_hashname.py $(B)/v3dp/*.v3dp $$(ls $(B)/v3dp/*.v3dw 2>/dev/null)
	touch $@

# ---- host check of ogles2/etc1.c against Mesa's ETC1 decoder
.PHONY: etc1-test
etc1-test:
	@mkdir -p $(B)/host
	gcc -O2 -DETC1_HOST_TEST -DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0 -Iogles2 -I$(MESA_SRC) -I$(MESA_SRC)/../include \
	    tools/etc1_test/etc1_test.c ogles2/etc1.c $(MESA_SRC)/util/format/u_format_etc.c \
	    -o $(B)/host/etc1_test
	$(B)/host/etc1_test

# ---- host check of ogles2/blake3.c against Mesa's BLAKE3
MESA_SRC ?= $(HOME)/mesa-build/mesa/src
.PHONY: blake3-test
blake3-test:
	@mkdir -p $(B)/host
	gcc -O2 -DBLAKE3_HOST_TEST -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 \
	    -DBLAKE3_NO_AVX512 -DBLAKE3_USE_NEON=0 -Iogles2 -I$(MESA_SRC) -I$(MESA_SRC)/util/blake3 \
	    tools/blake3_test/blake3_test.c ogles2/blake3.c $(MESA_SRC)/util/blake3/blake3.c \
	    $(MESA_SRC)/util/blake3/blake3_dispatch.c $(MESA_SRC)/util/blake3/blake3_portable.c \
	    -o $(B)/host/blake3_test
	$(B)/host/blake3_test

# ---- commands
$(B)/cmd/OGLES2Log: cmd/OGLES2Log.c $(B)/libogles2.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< -L$(B) -logles2 -o $@

clean:
	rm -rf $(B)
