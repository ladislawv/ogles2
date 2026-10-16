# The SDL examples, with bebbo's amiga-gcc (clib2):  make
# From the SDK's examples-sdl/ drawer, the SDL archive unpacked over the ogles2
# SDK: ogles2's headers and import library in ../include and ../lib, SDL's in
# ../SDL2 and ../SDL3, MiniGL's in ../MiniGL (SDL's desktop-GL path; SDL's
# SDL_opengl.h includes <proto/minigl.h>). See ../doc/SDL.md.
#
# The flags SDL was built with: 68040 with FPU, clib2, and for SDL3
# -fshort-wchar (clib2's wchar_t is 2 bytes; SDL3 checks it). -fbbb=...:
# bebbo's optimiser without "e", which deletes the fintrz of float-to-integer
# conversions in GCC 6.5. Link -lm881, not -lm (that would be libnix's maths).

CC     ?= m68k-amigaos-gcc
SDK    ?= ..
CFLAGS ?= -O2 -m68040 -mhard-float -mcrt=clib2 -fbbb=abcfhilmnprsz0 -D__AMIGAOS3__ -Wall
MGL     = -I$(SDK)/MiniGL/minigl-shared-library/include
GLLIBS  = -L$(SDK)/lib -logles2 -L$(SDK)/MiniGL/lib -lminigl -ldebug -lamiga -lm881

all: sdl2_sprites sdl3_gles2

sdl2_sprites: sdl2_sprites.c
	$(CC) $(CFLAGS) -I$(SDK)/include -I$(SDK)/SDL2/include $(MGL) sdl2_sprites.c \
	    -L$(SDK)/SDL2/lib -lSDL2 $(GLLIBS) -lstubs -o $@

sdl3_gles2: sdl3_gles2.c
	$(CC) $(CFLAGS) -fshort-wchar -I$(SDK)/include -I$(SDK)/SDL3/include $(MGL) sdl3_gles2.c \
	    -L$(SDK)/SDL3/lib -lSDL3 $(GLLIBS) -latomic -lstubs -o $@

clean:
	rm -f sdl2_sprites sdl3_gles2

.PHONY: all clean
