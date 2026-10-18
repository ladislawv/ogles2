precision mediump float;
// Boing 2026: the room, drawn at the render scale, brought up to the window
// with the bloom and the vignette, in one opaque pass. The ball, drawn after
// it at the window's size, adds its own bloom and vignette (ball.frag).
uniform sampler2D u_scene;
uniform sampler2D u_tex;  // the bloom
uniform float u_bloom;
uniform float u_aspect;
uniform float u_vig;      // 0 in the 1984 look
varying vec2 v_uv;

void main()
{
    vec3 s = texture2D(u_scene, v_uv).rgb;
    vec3 b = texture2D(u_tex, v_uv).rgb * 4.0 * u_bloom;
    vec2 q = (v_uv - 0.5) * vec2(u_aspect, 1.0);
    float vig = 1.0 - dot(q, q) * 0.55 * u_vig;
    gl_FragColor = vec4(1.0 - exp(-b) + s * vig, 1.0);
}
