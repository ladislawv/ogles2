/*
 * GLSL programs for the extension test (tests/m6_ext). Their binaries are
 * built from exactly these bytes by `make test-v3dp` (tools/v3dp_tests/emit.c).
 */

/* OES_standard_derivatives: the position's derivatives, scaled so that one
 * pixel's step on a 256x128 target reads 0.25 (64 of 255). */
static const char deriv_vs[] =
    "attribute vec3 a_position;\n"
    "varying vec2 v_p;\n"
    "void main() {\n"
    "    v_p = a_position.xy;\n"
    "    gl_Position = vec4(a_position, 1.0);\n"
    "}\n";
static const char deriv_fs[] =
    "#extension GL_OES_standard_derivatives : enable\n"
    "precision mediump float;\n"
    "varying vec2 v_p;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(dFdx(v_p.x) * 32.0, dFdy(v_p.y) * 16.0, fwidth(v_p.x) * 32.0, 1.0);\n"
    "}\n";

/* ARB_texture_rectangle: a 4x2 rectangle texture over the viewport,
 * addressed in texels. Vertex shader: tex1_vs. */
static const char rect_fs[] =
    "#extension GL_ARB_texture_rectangle : enable\n"
    "precision mediump float;\n"
    "uniform sampler2DRect u_rect;\n"
    "varying vec2 v_tc;\n"
    "void main() {\n"
    "    gl_FragColor = texture2DRect(u_rect, v_tc * vec2(4.0, 2.0));\n"
    "}\n";

/* OES_texture_float: the texture times a scale, so values beyond 0..1 (and
 * below 0) show in an RGBA8 framebuffer. Vertex shader: tex1_vs. */
static const char texscale_fs[] =
    "precision highp float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_scale;\n"
    "varying vec2 v_tc;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_tex, v_tc) * u_scale;\n"
    "}\n";
