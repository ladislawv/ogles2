/*
 * The cube example's shaders. As with triangle_shaders.h, the release ships
 * their compiled program beside the example (<hash>.v3dp): change a byte and
 * the program needs the shader compiler or a new binary.
 */
static const char cube_vs[] =
    "attribute vec3 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "uniform mat4 u_mvp;\n"
    "varying vec2 v_tc;\n"
    "void main() {\n"
    "    v_tc = a_texcoord;\n"
    "    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
    "}\n";

static const char cube_fs[] =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_tc;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_tex, v_tc);\n"
    "}\n";
