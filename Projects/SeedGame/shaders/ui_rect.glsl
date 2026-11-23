#version 330 core
// seedgame / 菜单按钮：纯灰色矩形，颜色由 u_params 决定
in vec2 v_uv;
in vec4 v_color;
uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform vec4 u_box;
uniform vec4 u_params;
out vec4 frag_color;

void main() {
    frag_color = u_params;
}
