// Boing 2026: the room, a box seen from inside (no depth buffer needed).
attribute vec3 a_pos;
attribute vec3 a_nrm;
uniform mat4 u_vp;
uniform mat4 u_lightvp;
uniform mat4 u_lightvp2;
varying vec3 v_wpos;
varying vec3 v_n;
varying vec4 v_lpos;
varying vec4 v_lpos2;

void main()
{
    v_wpos = a_pos;
    v_n = a_nrm;
    v_lpos = u_lightvp * vec4(a_pos, 1.0);
    v_lpos2 = u_lightvp2 * vec4(a_pos, 1.0);
    gl_Position = u_vp * vec4(a_pos, 1.0);
}
