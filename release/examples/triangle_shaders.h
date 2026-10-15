/*
 * The triangle example's shaders. The release ships their compiled program
 * (<hash>.v3dp beside the example): ogles2.library finds a program's binary
 * by a hash of its exact source text, so a program runs without the shader
 * compiler installed. Change a byte here and the program needs the compiler
 * (LIBS:v3dglslc, or one beside the program) or a new binary.
 */
static const char triangle_vs[] =
    "attribute vec2 a_position;\n"
    "attribute vec3 a_color;\n"
    "uniform float u_angle;\n"
    "varying vec3 v_color;\n"
    "void main() {\n"
    "    float c = cos(u_angle), s = sin(u_angle);\n"
    "    gl_Position = vec4(c * a_position.x - s * a_position.y,\n"
    "                       s * a_position.x + c * a_position.y, 0.0, 1.0);\n"
    "    v_color = a_color;\n"
    "}\n";

static const char triangle_fs[] =
    "precision mediump float;\n"
    "varying vec3 v_color;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(v_color, 1.0);\n"
    "}\n";
