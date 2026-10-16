/*
 * Writes the examples' GLSL (release/examples/*_shaders.h) into files for
 * v3dglslc, byte for byte as the examples pass them to glShaderSource, so
 * the binaries' program hashes match (build_release.sh). Host program.
 */
#include <stdio.h>
#include "examples/triangle_shaders.h"
#include "examples/cube_shaders.h"
#include "examples/boingocean_shaders.h"

static int put(const char *name, const char *text)
{
    FILE *f = fopen(name, "wb");
    if (!f || fputs(text, f) < 0 || fclose(f)) {
        perror(name);
        return 1;
    }
    return 0;
}

int main(void)
{
    return put("triangle.vert", triangle_vs) | put("triangle.frag", triangle_fs) |
           put("cube.vert", cube_vs) | put("cube.frag", cube_fs) | put("boingocean.vert", boingocean_vs) |
           put("boingocean.frag", boingocean_fs) | put("blit.vert", blit_vs) | put("blit.frag", blit_fs) |
           put("height.vert", blit_vs) | put("height.frag", height_fs) | put("sky.vert", blit_vs) |
           put("sky.frag", sky_fs);
}
