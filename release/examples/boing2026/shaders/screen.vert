// Boing 2026: one triangle over the whole target.
attribute vec2 a_pos;
varying vec2 v_uv;

void main()
{
    v_uv = a_pos * 0.5 + 0.5;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
