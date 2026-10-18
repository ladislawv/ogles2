// Boing 2026: the ball. A unit sphere, moved, squashed and spun by u_model;
// u_vp may hold a mirror about the floor (the reflection pass), the shading
// stays in the real world. Also lights each vertex for the Gouraud mode.
attribute vec3 a_pos;
uniform mat4 u_model;
uniform mat4 u_vp;
uniform mat3 u_nmat;
uniform vec3 u_light;
uniform vec3 u_eye;
varying vec3 v_obj;
varying vec3 v_wpos;
varying vec3 v_n;
varying vec2 v_gour;

void main()
{
    vec4 w = u_model * vec4(a_pos, 1.0);
    vec3 n = normalize(u_nmat * a_pos);
    vec3 l = normalize(u_light - w.xyz);
    vec3 v = normalize(u_eye - w.xyz);
    vec3 h = normalize(l + v);
    v_gour = vec2(max(dot(n, l), 0.0), pow(max(dot(n, h), 0.0), 60.0));
    v_obj = a_pos;
    v_wpos = w.xyz;
    v_n = n;
    gl_Position = u_vp * w;
}
