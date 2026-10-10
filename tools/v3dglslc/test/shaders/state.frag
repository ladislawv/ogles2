#version 100
// Exercises the .v3dp program tables: two samplers of different kinds,
// gl_FragCoord (state-tracker y transform), gl_PointCoord and gl_FrontFacing.
precision mediump float;
uniform sampler2D base;
uniform samplerCube env;
uniform vec2 viewport_size;
varying vec3 color;
varying float depth;
void main()
{
    vec2 uv = gl_FragCoord.xy / viewport_size;
    vec4 c = texture2D(base, uv) * textureCube(env, color);
    if (gl_FrontFacing)
        c.rgb *= 0.5;
    c.rg += gl_PointCoord * 0.1;
    gl_FragColor = vec4(c.rgb, depth);
}
