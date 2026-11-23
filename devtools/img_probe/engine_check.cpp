#include "engine_check.hpp"

#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace imgprobe::engine {
namespace {

namespace fs = std::filesystem;

struct source_file {
    bool ok = false;
    std::vector<std::string> lines;

    [[nodiscard]] bool has(std::string_view needle) const {
        for (const std::string& l : lines) {
            if (l.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    /// 1 基行号；0 = 没有。
    [[nodiscard]] int line_of(std::string_view needle) const {
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find(needle) != std::string::npos) {
                return static_cast<int>(i) + 1;
            }
        }
        return 0;
    }

    /// 只出现在注释里的行数（用于"这条调用还在不在"的反向检查）。
    [[nodiscard]] int live_line_of(std::string_view needle) const {
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find(needle) == std::string::npos) {
                continue;
            }
            std::size_t p = lines[i].find_first_not_of(" \t");
            const bool comment = (p != std::string::npos) && lines[i].compare(p, 2, "//") == 0;
            if (!comment) {
                return static_cast<int>(i) + 1;
            }
        }
        return 0;
    }
};

source_file load(const fs::path& p) {
    source_file f;
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return f;
    }
    f.ok = true;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        f.lines.push_back(line);
    }
    return f;
}

std::string loc(const char* rel, int line) {
    return std::string(rel) + ":" + std::to_string(line);
}

} // namespace

std::string find_root(const std::string& hint) {
    std::vector<fs::path> starts;
    if (!hint.empty()) {
        starts.emplace_back(hint);
    }
    std::error_code ec;
    starts.push_back(fs::current_path(ec));
    starts.push_back(fs::absolute(fs::path("."), ec));

    for (const fs::path& s : starts) {
        if (s.empty()) {
            continue;
        }
        fs::path p = fs::absolute(s, ec);
        for (int i = 0; i < 8 && !p.empty(); ++i) {
            if (fs::exists(p / "src" / "draw" / "renderer.hpp", ec)) {
                return p.string();
            }
            if (!p.has_parent_path() || p.parent_path() == p) {
                break;
            }
            p = p.parent_path();
        }
    }
    return {};
}

std::vector<result> check(const std::string& root) {
    std::vector<result> out;
    if (root.empty()) {
        out.push_back(result{"找到引擎根目录", false, "没找到含 src/draw/renderer.hpp 的目录（用 --root 指定）"});
        return out;
    }
    const fs::path base(root);

    const source_file renderer_hpp = load(base / "src" / "draw" / "renderer.hpp");
    const source_file renderer_cpp = load(base / "src" / "draw" / "renderer.cpp");
    const source_file rect_hpp = load(base / "src" / "space" / "rect.hpp");
    const source_file window_cpp = load(base / "src" / "platform" / "window.cpp");
    const source_file frame_cpp = load(base / "src" / "time" / "frame.cpp");

    // ---- 1. 顶点色是 ARGB 的 u32（不是 float4）------------------------------
    if (!renderer_hpp.ok) {
        out.push_back(result{"顶点色是 ARGB u32", false, "读不到 src/draw/renderer.hpp"});
    } else {
        const int a = renderer_hpp.line_of("std::uint32_t color");
        const int b = renderer_hpp.line_of("ARGB");
        out.push_back(result{"顶点色是 ARGB u32", a != 0 && b != 0,
                             (a != 0 && b != 0) ? loc("src/draw/renderer.hpp", a)
                                                : "没有 'std::uint32_t color' 或 'ARGB'"});
    }

    // ---- 2. 变换含第一次 y 翻转（m11 = -s.y）------------------------------
    if (!rect_hpp.ok) {
        out.push_back(result{"变换含 y 翻转（m11=-s.y）", false, "读不到 src/space/rect.hpp"});
    } else {
        const int a = rect_hpp.line_of("/* m11 */ -s.y");
        const int b = a != 0 ? a : rect_hpp.line_of("-s.y");
        out.push_back(result{"变换含 y 翻转（m11=-s.y）", b != 0,
                             b != 0 ? loc("src/space/rect.hpp", b) : "没找到 -s.y（y 翻转可能被删）"});
    }

    // ---- 3. present 就是 swap --------------------------------------------
    if (!window_cpp.ok) {
        out.push_back(result{"present = SDL_GL_SwapWindow", false, "读不到 src/platform/window.cpp"});
    } else {
        const int a = window_cpp.live_line_of("SDL_GL_SwapWindow");
        out.push_back(result{"present = SDL_GL_SwapWindow", a != 0,
                             a != 0 ? loc("src/platform/window.cpp", a) : "没有活的 SwapWindow 调用"});
    }

    // ---- 4. 每帧设 viewport（begin_frame 里）------------------------------
    if (!renderer_cpp.ok) {
        out.push_back(result{"begin_frame 每帧设 viewport", false, "读不到 src/draw/renderer.cpp"});
    } else {
        const int a = renderer_cpp.line_of("void renderer::begin_frame");
        const int b = renderer_cpp.live_line_of("glViewport(");
        out.push_back(result{"begin_frame 每帧设 viewport", a != 0 && b != 0,
                             (a != 0 && b != 0) ? loc("src/draw/renderer.cpp", b)
                                                : "begin_frame 或 glViewport 不在了"});
    }

    // ---- 5. 帧末顺序：先快照，后 present ----------------------------------
    //   探针若在 present 之后抓帧，读到的是未定义内容 ⇒ 这条必须成立。
    if (!frame_cpp.ok) {
        out.push_back(result{"抓帧窗口：快照在 present 之前", false, "读不到 src/time/frame.cpp"});
    } else {
        const int snap = frame_cpp.line_of("stage_snapshot(eng)");
        const int pres = frame_cpp.line_of("stage_present(eng)");
        const bool ok = snap != 0 && pres != 0 && snap < pres;
        std::string ev = "stage_snapshot 不在 stage_present 之前";
        if (ok) {
            ev = loc("src/time/frame.cpp", snap) + " < " + loc("src/time/frame.cpp", pres);
        }
        out.push_back(result{"抓帧窗口：快照在 present 之前", ok, ev});
    }

    // ---- 6. 主路径画默认帧缓冲（glBindFramebuffer 不得有活调用）----------
    if (!renderer_cpp.ok) {
        out.push_back(result{"主路径画默认帧缓冲", false, "读不到 src/draw/renderer.cpp"});
    } else {
        const int live = renderer_cpp.live_line_of("glBindFramebuffer");
        const int any = renderer_cpp.line_of("glBindFramebuffer");
        std::string ev;
        if (live != 0) {
            ev = "有活的 glBindFramebuffer 调用：" + loc("src/draw/renderer.cpp", live) +
                 "（探针将抓错缓冲）";
        } else if (any != 0) {
            ev = "只在注释里出现：" + loc("src/draw/renderer.cpp", any);
        } else {
            ev = "没有任何 glBindFramebuffer（主路径直画默认帧缓冲）";
        }
        out.push_back(result{"主路径画默认帧缓冲", live == 0, ev});
    }

    // ---- 7. 阶段个数对账：trace.hpp 的镜像 == frame.cpp 的 stage_* 定义数 ----
    //   【为什么需要这一条】trace.hpp 里的 12 个阶段是**手抄的镜像**。
    //   引擎将来增/删一个阶段，工具不会报错，只会**静默少记一条** —— 这正好是
    //   "两侧同名同形但没有机制"的那个病。重命名不用担心（WAG_SEAM_ENTER(name)
    //   会直接编译失败），会静默漂移的只有**个数**，所以只对个数。
    if (!frame_cpp.ok) {
        out.push_back(result{"阶段个数对账（trace 镜像）", false, "读不到 src/time/frame.cpp"});
    } else {
        int engine_stages = 0;
        for (const std::string& l : frame_cpp.lines) {
            if (l.find("void frame_pipeline::stage_") != std::string::npos) {
                ++engine_stages;
            }
        }

        const source_file trace_hpp = load(base / "devtools" / "trace_probe" / "trace.hpp");
        int tool_stages = 0;
        if (trace_hpp.ok) {
            bool inside = false;
            for (const std::string& raw : trace_hpp.lines) {
                if (!inside) {
                    if (raw.find("enum class stage") != std::string::npos) {
                        inside = true;
                    }
                    continue;
                }
                if (raw.find("};") != std::string::npos) {
                    break;
                }
                // 枚举项都以 ',' 结尾；`count` 哨兵没有逗号，自然被排除。
                std::size_t p = raw.find_first_not_of(" \t");
                if (p != std::string::npos && raw.back() == ',') {
                    ++tool_stages;
                }
            }
        }

        const bool ok = trace_hpp.ok && tool_stages > 0 && engine_stages == tool_stages;
        std::string ev;
        if (!trace_hpp.ok) {
            ev = "读不到 devtools/trace_probe/trace.hpp";
        } else {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "frame.cpp 有 %d 个 stage_*，trace.hpp 镜像 %d 个%s",
                          engine_stages, tool_stages, ok ? "" : "（不一致 ⇒ 镜像要跟着改）");
            ev = buf;
        }
        out.push_back(result{"阶段个数对账（trace 镜像）", ok, ev});
    }

    return out;
}

} // namespace imgprobe::engine
