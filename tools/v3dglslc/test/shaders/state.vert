#version 100
// Exercises the .v3dp program tables: a matrix attribute, struct, bool and
// int uniforms, a built-in state uniform and the point size.
struct Light {
    vec3 dir;
    float power;
};
attribute vec4 pos;
attribute mat2 warp;
attribute vec3 normal;
uniform mat4 mvp;
uniform Light lights[2];
uniform bool use_second;
uniform int mode;
varying vec3 color;
varying float depth;
void main()
{
    vec4 p = pos;
    p.xy = warp * p.xy;
    float l = max(dot(normal, lights[0].dir), 0.0) * lights[0].power;
    if (use_second)
        l += max(dot(normal, lights[1].dir), 0.0) * lights[1].power;
    color = vec3(l) * float(mode + 1);
    gl_Position = mvp * p;
    depth = gl_DepthRange.near + gl_DepthRange.diff * 0.5;
    gl_PointSize = 4.0;
}
