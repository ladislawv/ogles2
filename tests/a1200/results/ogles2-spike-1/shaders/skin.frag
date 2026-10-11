#version 100
precision mediump float;
varying float shade;
void main()
{
    gl_FragColor = vec4(vec3(shade), 1.0);
}
