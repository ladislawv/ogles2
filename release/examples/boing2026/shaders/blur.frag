precision mediump float;
// Boing 2026: one direction of a 9-tap Gaussian, in 5 bilinear reads.
uniform sampler2D u_tex;
uniform vec2 u_dir; // one texel along the blur, times the spread
varying vec2 v_uv;

void main()
{
    vec4 c = texture2D(u_tex, v_uv) * 0.2270270;
    c += (texture2D(u_tex, v_uv + u_dir * 1.3846154) + texture2D(u_tex, v_uv - u_dir * 1.3846154)) * 0.3162162;
    c += (texture2D(u_tex, v_uv + u_dir * 3.2307692) + texture2D(u_tex, v_uv - u_dir * 3.2307692)) * 0.0702703;
    gl_FragColor = c;
}
