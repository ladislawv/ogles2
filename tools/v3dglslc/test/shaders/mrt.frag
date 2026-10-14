#extension GL_EXT_draw_buffers : require
precision mediump float;
varying vec4 v_color;
void main() {
    gl_FragData[0] = v_color;
    gl_FragData[1] = v_color;
    gl_FragData[2] = v_color;
    gl_FragData[3] = v_color;
}
