#version 100
attribute vec3 position;
attribute vec3 normal;
uniform mat4 mvp;
uniform mat3 normal_matrix;
uniform vec3 light_dir[4];
uniform vec3 light_col[4];
uniform int light_count;
varying vec3 color;
const float ambient = 0.125 * 2.0;
void main()
{
    vec3 n = normalize(normal_matrix * normal);
    vec3 c = vec3(ambient);
    for (int i = 0; i < 4; i++) {
        if (i >= light_count)
            break;
        c += light_col[i] * max(dot(n, light_dir[i]), 0.0);
    }
    color = clamp(c, 0.0, 1.0);
    gl_Position = mvp * vec4(position, 1.0);
}
