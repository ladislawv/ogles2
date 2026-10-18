# Boing 2026, with bebbo's amiga-gcc (clib2):  make
# From the SDK's examples/boing2026/ drawer; the headers and import libraries
# are in ../../include and ../../lib.
#
# The shaders' sources are in shaders/; boing2026_shaders.h holds them as C
# strings (make it again with "make header" after editing them: python3). The
# program needs their compiled forms (.v3dp/.v3dw) in a shaders/ drawer beside
# it, or the shader compiler: the release ships them, with the shaders'
# records (.v3ds), which keep an installed compiler from starting. "make
# shaders" makes all three with the offline compiler of the ogles2 source
# (OGLES2, V3DGLSLC) into build/v3dp.

SDK      ?= ../..
CC       ?= m68k-amigaos-gcc
# -fbbb=...: bebbo's optimiser without "e" (it breaks float-to-int rounding).
CFLAGS   ?= -O2 -m68020 -m68881 -mcrt=clib2 -fno-builtin -fbbb=abcfhilmnprsz0 -Wall
OGLES2   ?= /mnt/c/GitHub/ogles2
V3DGLSLC ?= $(HOME)/ogles2-build/v3dglslc/host/v3dglslc

SHADERS  := shaders/ball.vert shaders/ball.frag shaders/room.vert shaders/room.frag \
            shaders/screen.vert shaders/blur.frag shaders/composite.frag \
            shaders/shadow.vert shaders/shadow.frag shaders/flare.vert shaders/flare.frag
# vertex + fragment shader of each program, as link_program() pairs them
PAIRS    := ball.vert ball.frag room.vert room.frag screen.vert blur.frag \
            screen.vert composite.frag shadow.vert shadow.frag flare.vert flare.frag

all: boing2026

boing2026: boing2026.c boing2026_shaders.h
	$(CC) $(CFLAGS) -I$(SDK)/include boing2026.c -L$(SDK)/lib -logles2 -lm881 -o $@

header: $(SHADERS) tools/embed_shaders.py
	python3 -I tools/embed_shaders.py boing2026_shaders.h $(SHADERS)

shaders: $(SHADERS)
	rm -rf build/v3dp && mkdir -p build/v3dp
	cd shaders && $(V3DGLSLC) -o ../build/v3dp $(PAIRS)
	python3 -I $(OGLES2)/v3dp/v3dp_hashname.py build/v3dp/*
	python3 -I $(OGLES2)/v3dp/v3ds_record.py build/v3dp $(SHADERS)

clean:
	rm -rf boing2026 build

.PHONY: all header shaders clean
