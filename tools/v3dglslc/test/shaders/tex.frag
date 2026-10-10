#version 100
precision mediump float;
uniform sampler2D tex;
uniform vec4 tint;
varying vec2 uv;
void main()
{
    vec4 c = texture2D(tex, uv) * tint;
    if (c.a < 0.5)
        discard;
    gl_FragColor = vec4(pow(c.rgb, vec3(1.0 / 2.2)), c.a);
}
