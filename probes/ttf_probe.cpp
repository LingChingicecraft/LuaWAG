// =============================================================================
//  probes/ttf_probe.cpp
//
//  一次性验证：**stb_truetype 能不能出中文**，以及三个字号下的观感。
//
//  它不接引擎，只回答两个问题：
//    ① 这两份 .ttf 里有没有我们要的那些码点（stbtt_FindGlyphIndex）
//    ② 用 stb_truetype 的默认光栅器渲染出来是什么样（重叠笔画会不会偏粗）
//
//  用法：ttf_probe <font.ttf> <out.png> [text]
// =============================================================================

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

/// 读整个文件（二进制）。
std::vector<unsigned char> read_file(const char* path, bool& ok) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        ok = false;
        return {};
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> data(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    ok = (got == data.size());
    return data;
}

/// UTF-8 → 码点（够用就行：不处理非法序列，遇到非法就跳过 1 字节）。
std::vector<std::uint32_t> decode_utf8(const std::string& s) {
    std::vector<std::uint32_t> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { ++i; continue; }
        if (i + extra >= s.size()) { break; }
        for (int k = 1; k <= extra; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

struct canvas {
    int w = 0;
    int h = 0;
    std::vector<unsigned char> rgba; // 4 通道

    canvas(int width, int height) : w(width), h(height), rgba(static_cast<std::size_t>(w) * h * 4, 0) {
        // 深灰底：既能看白字，也能看出笔画过覆盖
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                unsigned char* p = &rgba[(static_cast<std::size_t>(y) * w + x) * 4];
                p[0] = 24; p[1] = 24; p[2] = 28; p[3] = 255;
            }
        }
    }

    /// 把一张 alpha 位图按 coverage 混到 (dx,dy) 处（白字）。
    void blit_alpha(int dx, int dy, int bw, int bh, const unsigned char* src) {
        for (int y = 0; y < bh; ++y) {
            const int ty = dy + y;
            if (ty < 0 || ty >= h) continue;
            for (int x = 0; x < bw; ++x) {
                const int tx = dx + x;
                if (tx < 0 || tx >= w) continue;
                const int a = src[y * bw + x];
                unsigned char* p = &rgba[(static_cast<std::size_t>(ty) * w + tx) * 4];
                for (int c = 0; c < 3; ++c) {
                    p[c] = static_cast<unsigned char>((p[c] * (255 - a) + 255 * a) / 255);
                }
            }
        }
    }
};

struct layout {
    int width = 0;
    int height = 0;
};

/// 只用 hmtx + kern 的推进量算宽度（够用来排版）。
layout measure(const stbtt_fontinfo& info, float scale,
               const std::vector<std::uint32_t>& text, int ascent_px) {
    float x = 0.0f;
    for (std::size_t i = 0; i < text.size(); ++i) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&info, static_cast<int>(text[i]), &adv, &lsb);
        x += adv * scale;
        if (i + 1 < text.size()) {
            x += stbtt_GetCodepointKernAdvance(&info, static_cast<int>(text[i]),
                                               static_cast<int>(text[i + 1])) * scale;
        }
    }
    (void)ascent_px;
    return layout{static_cast<int>(x + 0.5f), 0};
}

/// 一行文字画到 canvas 上（x 从 pen 开始，基线 base_y）。
void draw_line(canvas& c, const stbtt_fontinfo& info, float scale,
               const std::vector<std::uint32_t>& text, int pen_x, int base_y) {
    float x = static_cast<float>(pen_x);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const int cp = static_cast<int>(text[i]);
        int w = 0, h = 0, xoff = 0, yoff = 0;
        unsigned char* bmp = stbtt_GetCodepointBitmap(&info, scale, scale, cp, &w, &h, &xoff, &yoff);
        if (bmp != nullptr) {
            c.blit_alpha(static_cast<int>(x) + xoff, base_y + yoff, w, h, bmp);
            stbtt_FreeBitmap(bmp, nullptr);
        }
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&info, cp, &adv, &lsb);
        x += adv * scale;
        if (i + 1 < text.size()) {
            x += stbtt_GetCodepointKernAdvance(&info, cp, static_cast<int>(text[i + 1])) * scale;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("用法: ttf_probe <font.ttf> <out.png> [text]\n");
        return 2;
    }
    const char* font_path = argv[1];
    const char* out_path = argv[2];
    const std::string text_utf8 = (argc >= 4) ? argv[3] : "海内存知己，天涯若比邻";

    // 可选第 4 个参数：逗号分隔的字号，例如 "20,16"（默认 48,32,20）
    std::vector<int> sizes;
    {
        const std::string spec = (argc >= 5) ? argv[4] : "48,32,20";
        std::size_t start = 0;
        while (start < spec.size()) {
            const std::size_t comma = spec.find(',', start);
            const std::string piece = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!piece.empty()) {
                sizes.push_back(std::atoi(piece.c_str()));
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (sizes.empty()) sizes = {48, 32, 20};
    }

    bool ok = false;
    std::vector<unsigned char> bytes = read_file(font_path, ok);
    if (!ok) {
        std::printf("读不到字体: %s\n", font_path);
        return 1;
    }

    stbtt_fontinfo info{};
    const int offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&info, bytes.data(), offset)) {
        std::printf("stbtt_InitFont 失败（不是它认识的 TTF/OTF？）\n");
        return 1;
    }

    std::printf("字体: %s（%zu 字节）\n", font_path, bytes.size());
    std::printf("  numGlyphs = %d\n", info.numGlyphs);
    {
        int ascent = 0, descent = 0, line_gap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
        std::printf("  VMetrics: ascent=%d descent=%d lineGap=%d\n", ascent, descent, line_gap);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetFontBoundingBox(&info, &x0, &y0, &x1, &y1);
        std::printf("  BBox: (%d,%d)..(%d,%d)\n", x0, y0, x1, y1);
    }

    const std::vector<std::uint32_t> text = decode_utf8(text_utf8);
    std::printf("  文本: %zu 个码点；缺字检查：\n", text.size());
    int missing = 0;
    for (std::uint32_t cp : text) {
        const int gi = stbtt_FindGlyphIndex(&info, static_cast<int>(cp));
        if (gi == 0) {
            std::printf("    U+%04X 缺字\n", cp);
            ++missing;
        }
    }
    if (missing == 0) {
        std::printf("    全部命中（0 个缺字）\n");
    }

    // ---- 三种字号的排布 ----
    // 字号来自命令行（见上）
    const int margin = 16;
    const int gap = 10;
    const std::string ascii = "ABC abc 123 一下";

    struct line_plan {
        int size = 0;
        std::vector<std::uint32_t> cps;
        float scale = 1.0f;
        int ascent_px = 0;
        int descent_px = 0;
        int height = 0;
        int width = 0;
        int base_y = 0;
    };
    std::vector<line_plan> plan;
    int total_h = margin;
    int max_w = 0;
    for (int s : sizes) {
        const float scale = stbtt_ScaleForPixelHeight(&info, static_cast<float>(s));
        int ascent = 0, descent = 0, line_gap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
        line_plan p;
        p.size = s;
        p.cps = text;
        p.scale = scale;
        p.ascent_px = static_cast<int>(ascent * scale + 0.5f);
        p.descent_px = static_cast<int>(-descent * scale + 0.5f);
        p.height = p.ascent_px + p.descent_px;
        p.width = measure(info, scale, p.cps, p.ascent_px).width;
        p.base_y = total_h + p.ascent_px;
        total_h += p.height + gap;
        max_w = std::max(max_w, p.width);
        plan.push_back(p);
    }
    // 末尾再放一行 ASCII（32 号）
    {
        const float scale = stbtt_ScaleForPixelHeight(&info, 32.0f);
        int ascent = 0, descent = 0, line_gap = 0;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
        line_plan p;
        p.size = 32;
        p.cps = decode_utf8(ascii);
        p.scale = scale;
        p.ascent_px = static_cast<int>(ascent * scale + 0.5f);
        p.descent_px = static_cast<int>(-descent * scale + 0.5f);
        p.height = p.ascent_px + p.descent_px;
        p.width = measure(info, scale, p.cps, p.ascent_px).width;
        p.base_y = total_h + p.ascent_px;
        total_h += p.height + gap;
        max_w = std::max(max_w, p.width);
        plan.push_back(p);
    }

    canvas c(max_w + margin * 2, total_h + margin);
    for (const auto& p : plan) {
        draw_line(c, info, p.scale, p.cps, margin, p.base_y);
    }

    if (!stbi_write_png(out_path, c.w, c.h, 4, c.rgba.data(), c.w * 4)) {
        std::printf("写 PNG 失败: %s\n", out_path);
        return 1;
    }
    std::printf("已写出: %s（%dx%d）\n", out_path, c.w, c.h);
    return 0;
}
