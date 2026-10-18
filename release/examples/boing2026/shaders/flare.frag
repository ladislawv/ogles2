precision mediump float;
// Boing 2026: the lens flare's pieces, added to the window. u_kind: 0 the
// light itself (a hot core, a glow and a four-point star), 1 a halo ring,
// 2 a soft ghost disc.
uniform vec3 u_col;
uniform float u_kind;
varying vec2 v_uv;

void main()
{
    float r = length(v_uv);
    vec3 c;
    if (u_kind < 0.5) {
        float core = exp(-r * r * 60.0) * 3.0;
        float glow = exp(-r * 4.5) * 0.7;
        float star = (exp(-abs(v_uv.x) * 70.0) + exp(-abs(v_uv.y) * 70.0)) * exp(-r * 2.5) * 0.9;
        c = vec3(core) + u_col * (glow + star);
    } else if (u_kind < 1.5) {
        c = u_col * exp(-abs(r - 0.8) * 30.0) * 0.35;
    } else {
        c = u_col * smoothstep(1.0, 0.7, r) * (0.10 + 0.12 * r);
    }
    gl_FragColor = vec4(c * (1.0 - smoothstep(0.85, 1.0, r * 0.95)), 1.0);
}
