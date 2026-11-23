#version 330 core
// seedgame / 远山背景：程序化天空 + 三层山脊 + 薄雾
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
        p = p * 2.03 + vec2(11.0, 17.0);
        a *= 0.5;
    }
    return s;
}

// 一层山脊：低频起伏 + 高频碎峰
float ridge(float x, float base, float scale, float phase) {
    return base
        + 0.050 * sin(x * scale + phase)
        + 0.025 * sin(x * scale * 2.7 + phase * 1.7)
        + 0.012 * fbm(vec2(x * scale * 4.0, phase));
}

void main() {
    vec2 uv = v_uv;
    float t = u_time * 0.008;
    vec3 top = vec3(0.055, 0.075, 0.13);
    vec3 bottom = vec3(0.48, 0.56, 0.63);
    vec3 sky = mix(bottom, top, smoothstep(0.10, 0.92, uv.y));

    // 低空薄雾
    float haze = smoothstep(0.30, 0.58, uv.y) * (1.0 - smoothstep(0.58, 0.80, uv.y));
    sky = mix(sky, vec3(0.66, 0.72, 0.76), haze * 0.22);

    // 远处太阳/月亮的冷光斑
    vec2 sun = vec2(0.74, 0.66);
    float sd = length((uv - sun) * vec2(1.0, 0.75));
    sky += vec3(0.46, 0.40, 0.28) * pow(max(0.0, 1.0 - sd * 2.2), 5.0) * 0.55;

    vec3 col = sky;

    // 三层山脊，越近越暗
    float r1 = ridge(uv.x, 0.50, 7.0 + t, 0.4);
    float r2 = ridge(uv.x, 0.43, 11.0 + t * 1.5, 2.1);
    float r3 = ridge(uv.x, 0.35, 18.0 + t * 2.2, 4.3);
    float m1 = smoothstep(r1 + 0.002, r1 - 0.002, uv.y);
    float m2 = smoothstep(r2 + 0.002, r2 - 0.002, uv.y);
    float m3 = smoothstep(r3 + 0.002, r3 - 0.002, uv.y);

    col = mix(col, vec3(0.24, 0.30, 0.38), m1 * 0.60);
    col = mix(col, vec3(0.13, 0.18, 0.24), m2 * 0.82);
    col = mix(col, vec3(0.055, 0.085, 0.11), m3);

    // 山脚的冷雾
    float mist = smoothstep(0.45, 0.30, uv.y) * (1.0 - smoothstep(0.18, 0.30, uv.y));
    col = mix(col, vec3(0.55, 0.62, 0.66), mist * 0.18);

    frag_color = vec4(col, 1.0);
}
