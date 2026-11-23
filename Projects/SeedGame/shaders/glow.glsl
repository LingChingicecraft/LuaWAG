#version 330 core
// seedgame / 加法光晕（灯光用 shader 拟合）
in vec2 v_uv;
in vec4 v_color;
uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform vec4 u_box;
uniform vec4 u_params;
out vec4 frag_color;

void main() {
    vec2 p = v_uv * 2.0 - 1.0;
    float d = length(p);
    if (d > 1.0) {
        discard;
    }
    float a = 1.0 - d;
    a *= a;
    a *= a;
    frag_color = vec4(u_params.rgb * a * u_params.a, a * u_params.a);
}
