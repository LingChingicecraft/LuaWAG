// =============================================================================
//  engine_check.hpp —— 只读"契约检查"：像素探针依赖的引擎约定还在不在
//
//  【它不修改引擎，只读源码文本】
//  为什么需要它：像素探针（以及任何外部工具）依赖几条**没有机制保护**的约定
//  （y 翻转在哪、颜色是 ARGB 还是 float4、present 是不是 swap）。
//  这些约定一旦被改，探针会"静默测错"——这正好是公理 16 那类
//  "看起来像使用者自己的 bug" 的缺陷。所以给它一条只读的、可重跑的对账。
// =============================================================================
#pragma once

#include <string>
#include <vector>

namespace imgprobe::engine {

struct result {
    std::string name;      ///< 契约名
    bool ok = false;
    std::string evidence;  ///< 命中的文件:行号，或失败原因
};

/// 从 hint（或当前目录、可执行文件目录）向上找含 src/draw/renderer.hpp 的目录。
/// 找不到返回空串。
std::string find_root(const std::string& hint);

/// 逐条检查。root 是 LuaWAG-Vanilla 目录（含 src/）。
std::vector<result> check(const std::string& root);

} // namespace imgprobe::engine
