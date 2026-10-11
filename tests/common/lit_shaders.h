/*
 * The lit program of the gears speed comparison (tests/perf): GL's fixed
 * function lighting for one directional light from (5, 5, 10) in eye space,
 * diffuse, plus the default global ambient 0.2, per vertex -- what MiniGL
 * computes on the CPU. Its binary is built by `make test-v3dp`.
 */
static const char lit_vs[] =
    "attribute vec3 a_position;\n"
    "attribute vec3 a_normal;\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_mv;\n"
    "uniform vec4 u_color;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    vec3 n = normalize((u_mv * vec4(a_normal, 0.0)).xyz);\n"
    "    float d = max(dot(n, vec3(0.408248, 0.408248, 0.816497)), 0.0);\n"
    "    v_color = vec4(min(u_color.rgb * (0.2 + d), 1.0), u_color.a);\n"
    "    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
    "}\n";
static const char lit_fs[] =
    "precision mediump float;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    gl_FragColor = v_color;\n"
    "}\n";
