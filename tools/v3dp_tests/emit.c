/*
 * Writes the test programs' GLSL (tests/common/*.h) into files for
 * v3dglslc, byte for byte as the tests pass them to glShaderSource, so the
 * binaries' program hashes match (make test-v3dp). Host program.
 */
#include <stdio.h>
#include "../../tests/common/smooth_shaders.h"
#include "../../tests/common/tex_shaders.h"
#include "../../tests/common/lit_shaders.h"
#include "../../tests/common/ext_shaders.h"

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
    return put("smooth.vert", smooth_vs) | put("smooth.frag", smooth_fs) |
           put("tex1.vert", tex1_vs) | put("tex1.frag", tex1_fs) |
           put("tex2.vert", tex1_vs) | put("tex2.frag", tex2_fs) |
           put("lit.vert", lit_vs) | put("lit.frag", lit_fs) |
           put("deriv.vert", deriv_vs) | put("deriv.frag", deriv_fs) |
           put("texscale.vert", tex1_vs) | put("texscale.frag", texscale_fs) |
           put("rect.vert", tex1_vs) | put("rect.frag", rect_fs);
}
