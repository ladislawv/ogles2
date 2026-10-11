#ifndef OGLES2_ETC1_H
#define OGLES2_ETC1_H
/* ETC1 (OES_compressed_ETC1_RGB8_texture) images to RGBA8 (etc1.c). */
#ifdef ETC1_HOST_TEST
typedef unsigned int ULONG;
typedef unsigned char UBYTE;
#else
#include <exec/types.h>
#endif

/* `width` x `height` texels from ((width+3)/4) x ((height+3)/4) blocks of 8
 * bytes, rows in data order, into rgba (width * 4 bytes a row, alpha 255). */
void ogles2_etc1_decode(const UBYTE *data, ULONG width, ULONG height, UBYTE *rgba);
#endif
