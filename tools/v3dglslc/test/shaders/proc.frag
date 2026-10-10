#version 100
precision highp float;
uniform float time;
uniform vec2 resolution;
const float k_exp = exp2(3.5);
const float k_log = log2(10.0);
const vec3 k_mix = mix(vec3(0.1, 0.2, 0.3), vec3(0.9, 0.8, 0.7), 0.25);
void main()
{
    vec2 p = gl_FragCoord.xy / resolution;
    float v = fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
    vec3 c = k_mix * step(0.5, v) + vec3(p.yx, fract(time * k_exp / k_log));
    if (p.x > 0.5)
        c = c.bgr;
    else if (p.y > 0.75)
        c *= vec3(exp2(1.0 / 3.0), log2(7.0), inversesqrt(2.0));
    gl_FragColor = vec4(c, 1.0);
}
