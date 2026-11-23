// =============================================================================
//  text/font.hpp —— TTF 字体与字形图集（2026-11-18，TTF 第一版）
//
//  【它是什么】把"字体文件 + 像素高度"变成一个**可查询的字形供给器**：
//      输入：码点
//      输出：{ 图集页句柄, UV 矩形, 推进, 轴承, 位图尺寸, 缺字标记 }
//
//  【它不做什么】
//      · 不排版（换行/对齐/字距/内联指令仍在 Lua 侧）
//      · 不缺字策略（只给 missing 标记；L1 由 Lua 记录）
//      · 不做回退链（多字体 = 多个句柄）
//
//  【为什么图集在引擎侧】
//      像素数据如果经过 Lua 再上传，会多一次拷贝与一次解释器开销；
//      而“CPU 像素 → GPU 纹理”这条路只该有一条。
//      ⇒ 本模块用 renderer 的 create_image_from_rgba / update_image_region，
//        脚本侧没有 image.from_rgba。
//
//  【依赖方向】text → draw（renderer）；draw 不知道 text。引擎持有本模块。
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wag::draw {
class renderer;
}

namespace wag::text {

/// 一个字形在图集里的全部信息（**调用方拷走**；本结构由调用方持有）。
struct glyph_info {
    std::uint64_t image = 0;   ///< 图集页的图片句柄（直接喂 draw_glyphs）
    double u = 0.0;            ///< 纹理像素矩形（左上）
    double v = 0.0;
    double uw = 0.0;
    double uh = 0.0;
    double advance = 0.0;      ///< 水平推进（已按像素高度缩放）
    double bearing_x = 0.0;    ///< 相对笔位/基线的偏移（像素）
    double bearing_y = 0.0;
    double w = 0.0;            ///< 位图尺寸（像素）；空格等无位图的字形是 0
    double h = 0.0;
    bool missing = false;      ///< true = 这是 .notdef（缺字，画出来是豆腐块）
};

/// 字体的整体度量（全部已按像素高度缩放）。
struct font_metrics {
    double pixel_size = 0.0;
    double ascent = 0.0;
    double descent = 0.0;      ///< 正数（= -hhea.descender × scale）
    double line_gap = 0.0;
    double line_height = 0.0;  ///< ascent + descent + line_gap
    int num_glyphs = 0;
};

/// 字体库：名字 → 字体；每个字体一张（或几张）图集页。
///
/// 【线程】只在主线程用（与 Lua / GL 上下文同一条纪律）。
class font_library {
public:
    font_library();
    ~font_library();

    font_library(const font_library&) = delete;
    font_library& operator=(const font_library&) = delete;

    /// 把渲染器接进来（引擎构造时一次）。空 ⇒ load 会失败（不是崩溃）。
    void attach(draw::renderer& r) noexcept { rend_ = &r; }

    /// 载入一份字体。失败 ⇒ `fail_boundary`（L3，资源失败档）。
    ///
    /// @param name        资源名（同名再载 ⇒ 旧字体卸载、旧句柄失效）
    /// @param path        文件路径（.ttf / .otf）
    /// @param pixel_size  像素高度（`ScaleForPixelHeight` 口径）；必须 > 0
    /// @return            字体句柄（低 21 位槽位 / 高 43 位代数）
    std::uint64_t load(const char* name, const char* path, double pixel_size);

    /// 取一个字形。**句柄失效** ⇒ 返回 false（调用方报 L3）。
    /// 缺字**不是**失败：返回 true 且 `out.missing = true`（画出来是豆腐块）。
    /// **图集页满**（画不下了）也返回 false —— 那是 L2 条件，调用方据此画不出来就跳过。
    [[nodiscard]] bool glyph(std::uint64_t handle, std::uint32_t codepoint,
                             glyph_info& out);

    /// 句柄是否仍有效（不取字形、不栅格化）。**给"使用 vs 查询"分档用。**
    [[nodiscard]] bool valid(std::uint64_t handle) const noexcept;

    /// 有没有这个字（= .notdef 之外的字形）。句柄失效 ⇒ false（调用方报 L3）。
    [[nodiscard]] bool has_glyph(std::uint64_t handle,
                                 std::uint32_t codepoint) const;

    /// 字体度量。句柄失效 ⇒ false。
    [[nodiscard]] bool metrics(std::uint64_t handle, font_metrics& out) const;

    /// 按名卸载（幂等）。**图集页也一并销毁**。
    void unload(const char* name) noexcept;

    /// 关闭序列：释放全部字体与页（纹理随上下文一起消失，这里只清表）。
    void shutdown() noexcept;

    /// 已载入的字体数（诊断用）。
    [[nodiscard]] std::size_t size() const noexcept { return by_name_.size(); }

private:
    struct slot;

    /// 句柄布局与图片族同源：低 21 位槽位、高 43 位代数。
    [[nodiscard]] static std::uint64_t make_handle(std::uint32_t index,
                                                   const slot& s) noexcept;

    [[nodiscard]] slot* find(std::uint64_t handle) noexcept;
    [[nodiscard]] const slot* find(std::uint64_t handle) const noexcept;

    /// 把缺的字形栅格化并放进图集（命中缓存时不会调用）。
    bool rasterize_and_cache(slot& s, std::uint32_t codepoint, glyph_info& out);

    /// 给字体开一张新的图集页。
    bool open_page(slot& s);

    std::vector<std::unique_ptr<slot>> slots_;
    std::vector<std::uint32_t> free_;
    std::unordered_map<std::string, std::uint32_t> by_name_;
    draw::renderer* rend_ = nullptr;
};

} // namespace wag::text
