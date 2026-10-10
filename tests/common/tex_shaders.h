/*
 * Textured GLSL programs for the M2 texture test. Their binaries are built
 * from exactly these bytes by `make test-v3dp` (tools/v3dp_tests/emit.c).
 */
static const char tex1_vs[] =
    "attribute vec3 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "varying vec2 v_tc;\n"
    "void main() {\n"
    "    v_tc = a_texcoord;\n"
    "    gl_Position = vec4(a_position, 1.0);\n"
    "}\n";
static const char tex1_fs[] =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_tc;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_tex, v_tc);\n"
    "}\n";
/* Two textures multiplied: units from glUniform1i. */
static const char tex2_fs[] =
    "precision mediump float;\n"
    "uniform sampler2D u_a;\n"
    "uniform sampler2D u_b;\n"
    "varying vec2 v_tc;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_a, v_tc) * texture2D(u_b, v_tc);\n"
    "}\n";
