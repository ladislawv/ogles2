// Boing 2026: one piece of the second light's lens flare, a quad at u_rect
// (centre and half size, in clip space).
attribute vec2 a_pos;
uniform vec4 u_rect;
varying vec2 v_uv;

void main()
{
    v_uv = a_pos;
    gl_Position = vec4(u_rect.xy + a_pos * u_rect.zw, 0.0, 1.0);
}
