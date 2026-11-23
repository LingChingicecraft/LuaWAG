#version 330 core
// seedgame / 圆形角色、BOSS、子弹
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

    float outer = smoothstep(1.0, 0.93, d);
    float rim = smoothstep(0.70, 0.98, d) * (1.0 - smoothstep(0.98, 1.0, d));
    float core = 1.0 - smoothstep(0.0, 0.70, d);

    vec3 base = u_params.rgb;
    vec3 col = mix(base * 0.55, base * 1.25, core);
    col = mix(col, vec3(1.0), rim * 0.55);

    // 左上高光
    float spec = smoothstep(0.35, 0.0, length(p - vec2(-0.28, 0.28)));
    col += vec3(1.0) * spec * 0.18;

    frag_color = vec4(col, outer * u_params.a);
}
