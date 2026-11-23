// =============================================================================
//  text/font.cpp —— TTF 字体与字形图集（2026-11-18）
//
//  这个文件负责：
//    · 用 stb_truetype（v1.28）读取字体文件并栅格化字形；
//    · 每个字体 = 文件字节（stb 借用，必须常驻）+ 若干图集页（RGBA8，白字 + alpha）；
//    · 页按 shelf 分配；页满开新页；到上限 ⇒ 停画新字 + L2 一条（只记一次）；
//    · 字形缓存按码点；命中时零分配。
// =============================================================================

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "text/font.hpp"

#include <algorithm>
#include <string_view>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_set>

#include "draw/renderer.hpp"
#include "error/error.hpp"
#include "log/log.hpp"
#include "platform/paths.hpp"
#include "platform/utf8_path.hpp"

namespace wag::text {

namespace {

/// 图集页边长（像素）。1024² RGBA8 = 4 MB/页。
constexpr int kPageSize = 1024;

/// 每个字体最多几页。到上限 ⇒ 停画新字 + L2（只记一次）。
constexpr std::size_t kMaxPages = 8;

/// 字形之间的边距（避免双线性采样串色）。
constexpr int kPad = 1;

/// 读整个文件。失败返回空。
///
/// 【路径】path 是 UTF-8；Windows 必须转宽字符后打开，
/// 否则中文文件名（例如“江西拙楷2.0.ttf”）会按 ACP 解释而读不到。
bool read_all(const char* path, std::vector<unsigned char>& out) {
    std::FILE* const file = wag::paths::fopen_utf8(path, "rb");
    if (file == nullptr) {
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    const long size = std::ftell(file);
    if (size <= 0) {
        std::fclose(file);
        return false;
    }
    if (std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    return got == out.size();
}

// -----------------------------------------------------------------------------
//  sfnt 白名单（2026-11-18）：只放行 TrueType 轮廓与 CFF 轮廓
//
//  【为什么需要】stb_truetype 两种都支持，但它也接受字体集合/Type1/畸形文件。
//    本引擎只承诺"glyf 或 CFF 的单个字体" ⇒ 在交给 stb 之前先判一次。
//    ⚠️ 这是**策略**不是能力缺口：要放开 TTC 只需改这里。
// -----------------------------------------------------------------------------

enum class sfnt_flavor { true_type, cff, cff2, collection, typ1, unknown };

std::uint32_t read_be32(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

std::uint16_t read_be16(const unsigned char* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}

/// 扫表目录：记下有没有 glyf / CFF / CFF2（都要求非零长度）。
/// 返回 false = 目录本身不可信（长度不够）。
///
/// 【为什么要单独认 CFF2】CFF2 是另一种轮廓表（OpenType 1.8，可变字体常用），
/// `stb_truetype` **完全不认识它**（连 `CFF2` 字样都没有）⇒ 必须在我们这一层
/// 显式拒绝并说清"这是 CFF2"，否则会退化成"缺少 CFF 表"那种误导性报错。
bool scan_tables(const std::vector<unsigned char>& bytes, bool& has_glyf, bool& has_cff,
                 bool& has_cff2) {
    has_glyf = false;
    has_cff = false;
    has_cff2 = false;
    if (bytes.size() < 12) {
        return false;
    }
    const std::uint16_t n = read_be16(bytes.data() + 4);
    if (bytes.size() < 12ull + 16ull * n) {
        return false;
    }
    for (std::uint16_t i = 0; i < n; ++i) {
        const unsigned char* rec = bytes.data() + 12 + 16ull * i;
        const std::uint32_t length = read_be32(rec + 12);
        if (std::memcmp(rec, "glyf", 4) == 0 && length > 0) {
            has_glyf = true;
        } else if (std::memcmp(rec, "CFF ", 4) == 0 && length > 0) {
            has_cff = true;
        } else if (std::memcmp(rec, "CFF2", 4) == 0 && length > 0) {
            has_cff2 = true;
        }
    }
    return true;
}

sfnt_flavor detect_sfnt_flavor(const std::vector<unsigned char>& bytes,
                               bool& has_glyf, bool& has_cff, bool& has_cff2) {
    has_glyf = false;
    has_cff = false;
    has_cff2 = false;
    if (bytes.size() < 12) {
        return sfnt_flavor::unknown;
    }
    const std::uint32_t tag = read_be32(bytes.data());
    if (tag == 0x00010000u || tag == 0x74727565u /* true */) {
        if (!scan_tables(bytes, has_glyf, has_cff, has_cff2)) {
            return sfnt_flavor::unknown;
        }
        return sfnt_flavor::true_type;
    }
    if (tag == 0x4F54544Fu /* OTTO */) {
        if (!scan_tables(bytes, has_glyf, has_cff, has_cff2)) {
            return sfnt_flavor::unknown;
        }
        if (has_cff) {
            return sfnt_flavor::cff;
        }
        if (has_cff2) {
            return sfnt_flavor::cff2; // 可变字体轮廓：stb 不支持 ⇒ 显式拒绝
        }
        return sfnt_flavor::cff; // 两者都没有 ⇒ 交给上层判"缺 CFF / CFF2 表"
    }
    if (tag == 0x74746366u /* ttcf */) {
        return sfnt_flavor::collection;
    }
    if (tag == 0x74797031u /* typ1 */) {
        return sfnt_flavor::typ1;
    }
    return sfnt_flavor::unknown;
}

/// 白名单拒绝：统一 L3（资源/边界失败）。
[[noreturn]] void fail_font_rejected(std::string_view got, std::string_view want) {
    error_value e;
    e.cls = error_class::resource_failure;
    e.where = "wag.font.load_ttf 的字体轮廓";
    e.got = got;
    e.want = want;
    e.sentence = "字体轮廓不被接受";
    fail_boundary(e);
}

} // namespace

// -----------------------------------------------------------------------------
//  slot / page
// -----------------------------------------------------------------------------

struct font_library::slot {
    struct page {
        std::uint64_t image = 0;            ///< 图片句柄（喂 draw_glyphs）
        std::string name;                   ///< 保留名 `__font:<字体>:p<i>`
        std::vector<unsigned char> cpu;     ///< 页的 CPU 镜像（RGBA8）
        int shelf_y = 0;                    ///< 当前 shelf 的 y
        int shelf_h = 0;                    ///< 当前 shelf 的高度（该行最高的字）
        int x = 0;                          ///< 当前 shelf 的写入位置
    };

    std::uint32_t gen = 0;
    std::string name;
    std::vector<unsigned char> bytes;       ///< 字体文件字节（stb 借用）
    stbtt_fontinfo info{};
    float scale = 0.0f;
    double pixel_size = 0.0;
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    int num_glyphs = 0;

    std::vector<page> pages;
    std::unordered_map<std::uint32_t, glyph_info> cache;
    /// 放过一次但**放不下**的字（页满）：不再重复栅格化，直接返回"画不出来"。
    std::unordered_set<std::uint32_t> rejected;
    bool page_cap_logged = false;           ///< L2 只记一次
};

std::uint64_t font_library::make_handle(std::uint32_t index,
                                        const slot& s) noexcept {
    return (static_cast<std::uint64_t>(s.gen) << 21) |
           static_cast<std::uint64_t>(index);
}

font_library::slot* font_library::find(std::uint64_t handle) noexcept {
    const std::uint32_t index = static_cast<std::uint32_t>(handle & 0x1FFFFFu);
    const std::uint32_t gen = static_cast<std::uint32_t>(handle >> 21);
    if (index >= slots_.size() || slots_[index] == nullptr) {
        return nullptr;
    }
    slot& s = *slots_[index];
    if (s.gen == 0 || s.gen != gen) {
        return nullptr;
    }
    return &s;
}

const font_library::slot* font_library::find(std::uint64_t handle) const noexcept {
    const std::uint32_t index = static_cast<std::uint32_t>(handle & 0x1FFFFFu);
    const std::uint32_t gen = static_cast<std::uint32_t>(handle >> 21);
    if (index >= slots_.size() || slots_[index] == nullptr) {
        return nullptr;
    }
    const slot& s = *slots_[index];
    if (s.gen == 0 || s.gen != gen) {
        return nullptr;
    }
    return &s;
}

// -----------------------------------------------------------------------------
//  生命周期
// -----------------------------------------------------------------------------

font_library::font_library() = default;
font_library::~font_library() = default;

void font_library::shutdown() noexcept {
    if (rend_ != nullptr) {
        for (auto& up : slots_) {
            if (up == nullptr) {
                continue;
            }
            for (auto& p : up->pages) {
                rend_->destroy_image_internal(p.name.c_str());
            }
        }
    }
    slots_.clear();
    free_.clear();
    by_name_.clear();
}

void font_library::unload(const char* name) noexcept {
    if (name == nullptr) {
        return;
    }
    const auto it = by_name_.find(name);
    if (it == by_name_.end()) {
        return; // 幂等
    }
    const std::uint32_t index = it->second;
    by_name_.erase(it);
    slot& s = *slots_[index];
    if (rend_ != nullptr) {
        for (auto& p : s.pages) {
            rend_->destroy_image_internal(p.name.c_str());
        }
    }
    s.pages.clear();
    s.gen += 1;                 // 旧句柄失效（与图片族同一条规则）
    s.bytes.clear();
    free_.push_back(index);
}

std::uint64_t font_library::load(const char* name, const char* path,
                                 double pixel_size) {
    // ---- ① 参数 ----
    if (name == nullptr || *name == '\0' || path == nullptr || *path == '\0' ||
        !(pixel_size > 0.0)) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "wag.font.load_ttf 的参数";
        e.got = (name == nullptr || *name == '\0')   ? "name 为空"
                : (path == nullptr || *path == '\0') ? "path 为空"
                                                     : "pixel_size <= 0";
        e.want = "非空字体名 + 非空路径 + > 0 的像素高度";
        e.sentence = "字体载入的参数不合法";
        fail_boundary(e);
    }
    if (rend_ == nullptr) {
        fail_internal("字体库还没接上渲染器（启动次序被改了？）");
    }

    // ---- ② 读文件 + 白名单（轮廓形态）----
    std::vector<unsigned char> bytes;
    if (!read_all(path, bytes)) {
        error_value e;
        e.cls = error_class::resource_failure;
        e.where = "wag.font.load_ttf 的字体文件";
        e.got = wag::paths::relativize(path);
        e.want = "一个可读的 .ttf / .otf";
        e.sentence = "字体文件读不到";
        fail_boundary(e);
    }

    // 【白名单：只接受两种轮廓】2026-11-18 定
    //   · TrueType 轮廓（sfnt 0x00010000 / 'true'）⇒ 必须含 glyf + loca
    //   · CFF 轮廓（sfnt 'OTTO' + 'CFF ' 表）⇒ 放行
    //   · **CFF2**（'CFF2' 表）⇒ **显式拒绝**：stb_truetype 完全不支持 CFF2，
    //     而它是可变字体的轮廓格式；报"缺少 CFF 表"会误导成文件损坏。
    //   · 'ttcf'（字体集合）/ 'typ1' / 其它 ⇒ 拒绝（L3）
    // 说明：stb_truetype 支持 glyf 与 CFF（不支持 CFF2）；这里是**策略** + 能力边界，
    //   要放开 TTC 只需改这一处判定。
    bool has_glyf = false;
    bool has_cff = false;
    bool has_cff2 = false;
    const sfnt_flavor flavor = detect_sfnt_flavor(bytes, has_glyf, has_cff, has_cff2);
    if (flavor == sfnt_flavor::collection) {
        fail_font_rejected("字体集合（ttcf）", "单个 .ttf / .otf（不支持 .ttc 集合）");
    } else if (flavor == sfnt_flavor::typ1) {
        fail_font_rejected("typ1（PostScript Type 1）",
                           "TrueType 轮廓（glyf）或 CFF 轮廓（OTTO）的字体");
    } else if (flavor == sfnt_flavor::unknown) {
        fail_font_rejected("未知的 sfnt 版本 / 损坏文件",
                           "TrueType 轮廓（glyf）或 CFF 轮廓（OTTO）的字体");
    } else if (flavor == sfnt_flavor::cff2) {
        fail_font_rejected("CFF2（可变字体轮廓）",
                           "CFF（'CFF ' 表）或 TrueType（glyf）的静态字体");
    } else if (flavor == sfnt_flavor::true_type && !has_glyf) {
        fail_font_rejected("缺少 glyf / loca 表",
                           "TrueType 轮廓（glyf + loca）的字体");
    } else if (flavor == sfnt_flavor::cff && !has_cff) {
        fail_font_rejected("缺少 CFF / CFF2 表", "CFF 轮廓（'CFF ' 表）的字体");
    }

    stbtt_fontinfo info{};
    const int offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&info, bytes.data(), offset)) {
        error_value e;
        e.cls = error_class::resource_failure;
        e.where = "wag.font.load_ttf 的字体格式";
        e.got = wag::paths::relativize(path);
        e.want = "stbtt_InitFont 能接受的 .ttf / .otf（不是损坏文件）";
        e.sentence = "字体格式不被接受";
        fail_boundary(e);
    }
    log::record("font", log::format("轮廓",
                            flavor == sfnt_flavor::cff ? "CFF（OpenType）"
                                                       : "glyf（TrueType）"));

    // ---- ③ 同名覆盖：先卸旧的（代数 +1，旧句柄失效）----
    unload(name);

    // ---- ④ 取槽位 ----
    std::uint32_t index = 0;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
    } else {
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back(nullptr);
    }

    auto up = std::make_unique<slot>();
    slot& s = *up;
    s.gen = (slots_[index] == nullptr) ? 1 : slots_[index]->gen;
    if (s.gen == 0) {
        s.gen = 1;
    }
    s.name = name;
    s.bytes = std::move(bytes);
    s.info = info;
    s.pixel_size = pixel_size;
    s.scale = stbtt_ScaleForPixelHeight(&s.info, static_cast<float>(pixel_size));
    stbtt_GetFontVMetrics(&s.info, &s.ascent, &s.descent, &s.line_gap);
    s.num_glyphs = s.info.numGlyphs;

    slots_[index] = std::move(up);
    by_name_[name] = index;
    return make_handle(index, s);
}

bool font_library::open_page(slot& s) {
    if (s.pages.size() >= kMaxPages) {
        if (!s.page_cap_logged) {
            s.page_cap_logged = true;
            log::warn("text", log::format("字体的图集页已达上限（停画新字）",
                                  s.name + "：" + std::to_string(kMaxPages) +
                                      " 页 / " + std::to_string(kPageSize) + "²"));
        }
        return false;
    }
    if (rend_ == nullptr) {
        return false;
    }

    slot::page p;
    p.name = "__font:" + s.name + ":p" + std::to_string(s.pages.size());
    p.cpu.assign(static_cast<std::size_t>(kPageSize) * kPageSize * 4, 0);
    // 白 RGB + 透明 alpha：着色器的 Mul 模式 × 顶点色 ⇒ 字形按覆盖率上色。
    for (std::size_t i = 0; i < p.cpu.size(); i += 4) {
        p.cpu[i + 0] = 255;
        p.cpu[i + 1] = 255;
        p.cpu[i + 2] = 255;
        p.cpu[i + 3] = 0;
    }
    p.image = rend_->create_image_from_rgba(p.name.c_str(), kPageSize, kPageSize,
                                            p.cpu.data());
    if (p.image == 0) {
        return false;
    }
    s.pages.push_back(std::move(p));
    return true;
}

bool font_library::rasterize_and_cache(slot& s, std::uint32_t codepoint,
                                       glyph_info& out) {
    const int cp = static_cast<int>(codepoint);
    const int glyph_index = stbtt_FindGlyphIndex(&s.info, cp);
    const bool missing = (glyph_index == 0);

    int w = 0;
    int h = 0;
    int xoff = 0;
    int yoff = 0;
    unsigned char* bmp = stbtt_GetCodepointBitmap(&s.info, s.scale, s.scale, cp,
                                                  &w, &h, &xoff, &yoff);

    int adv = 0;
    int lsb = 0;
    if (missing) {
        stbtt_GetGlyphHMetrics(&s.info, 0, &adv, &lsb);
        // ⚠️ 缺字也要有推进量（否则一行字会挤在一起）；用 .notdef 的推进。
    } else {
        stbtt_GetCodepointHMetrics(&s.info, cp, &adv, &lsb);
    }

    glyph_info g;
    g.missing = missing;
    g.advance = static_cast<double>(adv) * static_cast<double>(s.scale);

    if (bmp == nullptr || w <= 0 || h <= 0) {
        // 空格之类的"无位图字形"：不是缺字，只是没有像素。
        if (bmp != nullptr) {
            stbtt_FreeBitmap(bmp, nullptr);
        }
        s.cache[codepoint] = g;
        out = g;
        return true;
    }

    // ---- 分配矩形（shelf）----
    slot::page* page = nullptr;
    int gx = 0;
    int gy = 0;
    for (;;) {
        if (s.pages.empty()) {
            if (!open_page(s)) {
                stbtt_FreeBitmap(bmp, nullptr);
                return false;
            }
        }
        page = &s.pages.back();
        if (page->shelf_h == 0) {
            page->shelf_y = 0;
            page->shelf_h = h + kPad * 2;
            page->x = kPad;
        }
        if (page->x + w + kPad <= kPageSize) {
            gx = page->x;
            gy = page->shelf_y + kPad;
            page->x += w + kPad * 2;
            if (h + kPad * 2 > page->shelf_h) {
                // 这一行里来了个更高的字：把 shelf 抬高，避免压到下一行
                page->shelf_h = h + kPad * 2;
            }
            break;
        }
        // 换行
        page->x = kPad;
        page->shelf_y += page->shelf_h;
        page->shelf_h = h + kPad * 2;
        if (page->shelf_y + page->shelf_h + kPad > kPageSize) {
            if (!open_page(s)) {
                stbtt_FreeBitmap(bmp, nullptr);
                return false;
            }
            continue;
        }
        gx = page->x;
        gy = page->shelf_y + kPad;
        page->x += w + kPad * 2;
        break;
    }

    // ---- 写进页的 CPU 镜像（白 RGB + 覆盖率 alpha）----
    std::vector<unsigned char> sub(static_cast<std::size_t>(w) * h * 4, 0);
    for (int row = 0; row < h; ++row) {
        const int ty = gy + row;
        for (int col = 0; col < w; ++col) {
            const int tx = gx + col;
            const unsigned char a = bmp[row * w + col];
            const std::size_t dst =
                (static_cast<std::size_t>(ty) * kPageSize + tx) * 4;
            page->cpu[dst + 0] = 255;
            page->cpu[dst + 1] = 255;
            page->cpu[dst + 2] = 255;
            page->cpu[dst + 3] = a;
            std::size_t s_i = (static_cast<std::size_t>(row) * w + col) * 4;
            sub[s_i + 0] = 255;
            sub[s_i + 1] = 255;
            sub[s_i + 2] = 255;
            sub[s_i + 3] = a;
        }
    }
    stbtt_FreeBitmap(bmp, nullptr);

    // ---- 上传这一小块 ----
    if (!rend_->update_image_region(page->image, gx, gy, w, h, sub.data())) {
        fail_internal("字体图集页的上传失败（句柄失效或越界）");
    }

    g.image = page->image;
    g.u = static_cast<double>(gx);
    g.v = static_cast<double>(gy);
    g.uw = static_cast<double>(w);
    g.uh = static_cast<double>(h);
    g.bearing_x = static_cast<double>(xoff);
    g.bearing_y = static_cast<double>(yoff);
    g.w = static_cast<double>(w);
    g.h = static_cast<double>(h);

    s.cache[codepoint] = g;
    out = g;
    return true;
}

bool font_library::glyph(std::uint64_t handle, std::uint32_t codepoint,
                         glyph_info& out) {
    slot* s = find(handle);
    if (s == nullptr) {
        return false;
    }
    const auto it = s->cache.find(codepoint);
    if (it != s->cache.end()) {
        out = it->second;
        return true;
    }
    if (s->rejected.find(codepoint) != s->rejected.end()) {
        return false; // 之前已因页满放过一次：不再重复栅格化
    }
    if (!rasterize_and_cache(*s, codepoint, out)) {
        s->rejected.insert(codepoint);
        return false;
    }
    return true;
}

bool font_library::valid(std::uint64_t handle) const noexcept {
    return find(handle) != nullptr;
}

bool font_library::has_glyph(std::uint64_t handle,
                             std::uint32_t codepoint) const {
    const slot* s = find(handle);
    if (s == nullptr) {
        return false;
    }
    return stbtt_FindGlyphIndex(&s->info, static_cast<int>(codepoint)) != 0;
}

bool font_library::metrics(std::uint64_t handle, font_metrics& out) const {
    const slot* s = find(handle);
    if (s == nullptr) {
        return false;
    }
    const double scale = static_cast<double>(s->scale);
    out.pixel_size = s->pixel_size;
    out.ascent = static_cast<double>(s->ascent) * scale;
    out.descent = -static_cast<double>(s->descent) * scale;
    out.line_gap = static_cast<double>(s->line_gap) * scale;
    out.line_height = out.ascent + out.descent + out.line_gap;
    out.num_glyphs = s->num_glyphs;
    return true;
}

} // namespace wag::text
