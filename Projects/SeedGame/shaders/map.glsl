#version 330 core
// seedgame / 地图：岩层 + 草皮顶边 + 裂缝
in vec2 v_uv;
in vec4 v_color;
uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform vec4 u_box;
uniform vec4 u_params;
out vec4 frag_color;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x),
               mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x), u.y);
}

float fbm(vec2 p) {
    float s = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; ++i) {
        s += a * noise(p);
        p = p * 2.11 + vec2(5.0, 9.0);
        a *= 0.5;
    }
    return s;
}

void main() {
    vec2 uv = v_uv;
    float n = fbm(vec2(uv.x * 9.0, uv.y * 5.0) + vec2(0.0, u_time * 0.02));
    float cracks = smoothstep(0.82, 0.86, fbm(vec2(uv.x * 18.0, uv.y * 8.0)));
    vec3 rock = mix(vec3(0.13, 0.12, 0.11), vec3(0.28, 0.25, 0.21), n * 0.75);
    rock = mix(rock, vec3(0.07, 0.06, 0.055), cracks * 0.45);

    // 顶边草皮/苔藓：一条窄亮带
    float top = smoothstep(0.955, 0.995, uv.y);
    vec3 moss = mix(vec3(0.16, 0.27, 0.16), vec3(0.30, 0.42, 0.22), n);
    rock = mix(rock, moss, top * 0.9);

    // 边缘压暗，避免盒子边界像贴纸
    float edge = smoothstep(0.0, 0.025, uv.x) * smoothstep(1.0, 0.975, uv.x);
    rock *= mix(0.55, 1.0, edge);

    frag_color = vec4(rock, 1.0);
}
