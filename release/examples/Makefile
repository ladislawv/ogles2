# The ogles2 examples, with bebbo's amiga-gcc (clib2):  make
# or with vbcc:  make vbcc
# From the SDK's examples/ drawer; the headers and import libraries are in
# ../include and ../lib. The programs need their .v3dp files (the compiled
# shaders) beside them, or the shader compiler: see ../doc/ogles2.doc.

CC     ?= m68k-amigaos-gcc
# -fbbb=...: bebbo's optimiser without "e", which deletes the fintrz of
# float-to-integer conversions in GCC 6.5 ((int)255.5f became 256).
CFLAGS ?= -O2 -m68020 -m68881 -mcrt=clib2 -fno-builtin -fbbb=abcfhilmnprsz0 -Wall
VC     ?= vc +aos68k
VCFLAGS ?= -c99 -cpu=68020 -fpu=68881 -O1

all: triangle cube

triangle: triangle.c triangle_shaders.h
	$(CC) $(CFLAGS) -I../include triangle.c -L../lib -logles2 -o $@

cube: cube.c cube_shaders.h
	$(CC) $(CFLAGS) -I../include cube.c -L../lib -logles2 -lm881 -o $@

vbcc: triangle.c triangle_shaders.h cube.c cube_shaders.h
	$(VC) $(VCFLAGS) -I../include triangle.c ../lib/ogles2.lib -lamiga -lm881 -o triangle
	$(VC) $(VCFLAGS) -I../include cube.c ../lib/ogles2.lib -lamiga -lm881 -o cube

clean:
	rm -f triangle cube

.PHONY: all vbcc clean
