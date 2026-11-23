// =============================================================================
// script/collide_hook.hpp
//
// // 的"执行点"：脚本在本帧声明了组对之后，由帧管线在
// 阶段 6 的 Main 分支末尾调用这里。
//
// 【为什么放在 script 模块】因为执行时要调 Lua 回调（批量数组）；
// 容器/碰撞模块本身不持有 lua_State（见 collide.hpp 的"它不做的事"）。
// =============================================================================

#pragma once

namespace wag {
class engine;
}

namespace wag::script {

/// 执行本帧声明的碰撞请求；没有请求时立即返回。
/// 每个请求：rebuild 一次 → 宽相/窄相 → 一次 Lua 回调（的批量数组）。
void run_collisions(engine& eng);

/// 丢弃本帧未执行的请求（例如 Pause 分支）：只 unref 回调，不做检测。
void drop_collisions(engine& eng);

} // namespace wag::script
