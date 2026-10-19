precision highp float;
uniform sampler2D u_tex;
varying highp vec2 v_tc;
void main() {
    gl_FragColor = texture2D(u_tex, v_tc);
}
