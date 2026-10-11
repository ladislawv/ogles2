#version 100
attribute vec4 pos;
attribute vec2 tc;
uniform mat4 mvp;
varying vec2 uv;
void main()
{
    uv = tc;
    gl_Position = mvp * pos;
}
