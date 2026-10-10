/*
 * The smooth-colour GLSL program the M1/M2 tests draw with. Its binary
 * (<hash>.v3dp) is built from exactly these bytes by `make test-v3dp`
 * (tools/v3dp_tests: written out, compiled with v3dglslc, named by the
 * program hash) and shipped next to the test programs (PROGDIR:).
 */
static const char smooth_vs[] =
    "attribute vec3 a_position;\n"
    "attribute vec4 a_color;\n"
    "uniform mat4 u_mvp;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    v_color = a_color;\n"
    "    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
    "}\n";
static const char smooth_fs[] =
    "precision mediump float;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    gl_FragColor = v_color;\n"
    "}\n";
