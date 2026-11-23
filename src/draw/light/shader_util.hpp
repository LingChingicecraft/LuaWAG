// =============================================================================
//  draw/light/shader_util.hpp —— 光照通道内部：编译 / 链接 helper
//
//  【唯一一份】occluders.cpp / pass.cpp / bands.cpp 都 include 它。
//    不允许任何一处在自己的 .cpp 里再写一份 —— 那就是"第二路径"。
//  【错误级别】编译/链接失败一律 L4（fail_internal）：
//    这是引擎/脚本提供的 shader 不可用，不是运行期数据错误。
// =============================================================================
#pragma once

#include "draw/gl_api.hpp"
#include "error/error.hpp"

#include <cstddef>
#include <string>

namespace wag::draw {

inline gl_api::uint_t light_compile_stage(gl_api::enum_t type, const char* src,
                                          const char* label) {
    const gl_api::uint_t sh = gl_api::create_shader(type);
    if (sh == 0) {
        fail_internal(std::string("光照着色器：glCreateShader 失败（") + label + "）");
    }
    gl_api::shader_source(sh, 1, &src, nullptr);
    gl_api::compile_shader(sh);
    gl_api::int_t ok = 0;
    gl_api::get_shader_iv(sh, gl_api::compile_status, &ok);
    if (ok == 0) {
        char buf[1024] = {0};
        gl_api::get_shader_info_log(sh, sizeof(buf), nullptr, buf);
        gl_api::delete_shader(sh);
        fail_internal(std::string("光照着色器编译失败（") + label + "）\n" + buf);
    }
    return sh;
}

inline gl_api::uint_t light_link_stage(gl_api::uint_t vs, gl_api::uint_t fs) {
    const gl_api::uint_t prog = gl_api::create_program();
    if (prog == 0) {
        fail_internal("光照程序：glCreateProgram 失败");
    }
    gl_api::attach_shader(prog, vs);
    gl_api::attach_shader(prog, fs);
    gl_api::link_program(prog);
    gl_api::int_t ok = 0;
    gl_api::get_program_iv(prog, gl_api::link_status, &ok);
    if (ok == 0) {
        char buf[1024] = {0};
        gl_api::get_program_info_log(prog, sizeof(buf), nullptr, buf);
        gl_api::delete_program(prog);
        fail_internal(std::string("光照程序链接失败\n") + buf);
    }
    return prog;
}

} // namespace wag::draw
