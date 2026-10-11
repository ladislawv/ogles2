#version 100
attribute vec4 pos;
attribute vec4 weights;
attribute vec4 bones;
uniform mat4 bone[24];
uniform mat4 viewproj;
varying float shade;
void main()
{
    mat4 m = bone[int(bones.x)] * weights.x
           + bone[int(bones.y)] * weights.y
           + bone[int(bones.z)] * weights.z
           + bone[int(bones.w)] * weights.w;
    vec4 p = m * pos;
    shade = p.y * 0.5 + 0.5;
    gl_Position = viewproj * p;
}
