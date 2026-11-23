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

const float PI = 3.14159265359;
const float SUN_PERIOD = 45.0;

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
    float clock_seconds = u_params.x;
    float t = clock_seconds * 0.008;
    vec3 top = vec3(0.055, 0.075, 0.13);
    vec3 bottom = vec3(0.48, 0.56, 0.63);
    vec3 sky = mix(bottom, top, smoothstep(0.10, 0.92, uv.y));

    // 低空薄雾
    float haze = smoothstep(0.30, 0.58, uv.y) * (1.0 - smoothstep(0.58, 0.80, uv.y));
    sky = mix(sky, vec3(0.66, 0.72, 0.76), haze * 0.22);
    // 太阳绕 (0,-400) 旋转：半径 800，45 秒一圈，顺时针。
    // 正上方时太阳世界坐标 y = -400 + 800 = 400。
    float phase = fract(clock_seconds / SUN_PERIOD) * (2.0 * PI);
    float sunAngle = PI - phase;
    vec2 sunWorld = vec2(0.0, -400.0) + vec2(cos(sunAngle), sin(sunAngle)) * 800.0;
    vec2 sunUV = vec2(0.5 + sunWorld.x / 1920.0,
                      0.5 + sunWorld.y / 1080.0);
    float altitude = sin(sunAngle);
    float day = smoothstep(-0.05, 0.30, altitude);
    float low = 1.0 - smoothstep(0.0, 0.70, max(altitude, 0.0));

    // 清晨/夕阳偏红，正午偏黄白。
    vec3 dawnColor = vec3(1.0, 0.34, 0.18);
    vec3 noonColor = vec3(1.0, 0.94, 0.72);
    vec3 sunColor = mix(dawnColor, noonColor,
                        smoothstep(0.0, 0.95, max(altitude, 0.0)));

    // 地平线暖色；正午自动变淡。
    sky = mix(sky, dawnColor, low * 0.20 * day);
    sky *= mix(vec3(0.48, 0.56, 0.78), vec3(1.0), day);

    // 太阳光斑：只用柔和光晕，不做硬边日轮。
    vec2 sdv = (uv - sunUV) * vec2(1.0, 0.75);
    float sd = length(sdv);
    float above = smoothstep(-0.12, 0.08, altitude);
    float halo = pow(max(0.0, 1.0 - sd * 1.65), 3.0) * above;
    float core = exp(-sd * sd * 180.0) * above;
    sky += sunColor * (halo * 0.58 + core * 1.05) * (0.80 + low * 0.50);

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

    // 低角度阳光给山脊一点暖色，正午偏中性。
    float lit = smoothstep(-0.10, 0.25, altitude);
    col = mix(col, col * sunColor, 0.14 * lit);

    // 全局薄雾：让天空、山脊和光晕融在一起，而不是硬边相切。
    vec3 fogTint = mix(vec3(0.50, 0.57, 0.65), sunColor, low * 0.22);
    col = mix(col, fogTint, 0.05);
    col = mix(col, vec3(dot(col, vec3(0.299, 0.587, 0.114))), 0.03);

    // 不再在山脚盖灰条：底部直接显示最近一层山脊的底色。

    frag_color = vec4(col, 1.0);
}

