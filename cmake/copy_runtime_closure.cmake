# =============================================================================
#  cmake/copy_runtime_closure.cmake
#
#  把 exe 的**完整运行时依赖闭包**复制到框架目录。
#
#  【为什么不能只靠 TARGET_RUNTIME_DLLS】
#      `$<TARGET_RUNTIME_DLLS:LuaWAG>` 只解析**直接链接目标**的 DLL。
#      它不会继续解析 SDL3.dll 自己的导入表。
#      实际例子：SDL3.dll → libiconv-2.dll。
#      ⇒ 开发机 PATH 里有 MSYS2 时能跑；干净机器双击就缺 DLL。
#
#  【输入】
#      WAG_EXE         要分析的 exe
#      WAG_DEST_DIR    复制目标目录（exe 所在目录）
#      WAG_APP_DIR     先搜索 exe 旁边已有的 DLL
#      WAG_MINGW_BIN   MSYS2/UCRT64 的 bin（找 libiconv 等）
#
#  【规则】
#      · 递归解析 exe 的依赖；
#      · 排除 Windows/Program Files 下的系统 DLL；
#      · 排除 api-ms-win-* / ext-ms-* API set；
#      · 其余按文件名复制到 exe 旁边。
#
#  【为什么用 file(GET_RUNTIME_DEPENDENCIES)】
#      它由 exe 的真实导入表推导，不在这里手写 DLL 名字；
#      SDL3 以后新增传递依赖时，构建期会自动跟上。
# =============================================================================

if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

if(NOT EXISTS "${WAG_EXE}")
    message(FATAL_ERROR "copy_runtime_closure: exe 不存在：${WAG_EXE}")
endif()

set(_search_dirs "${WAG_APP_DIR}" "${WAG_MINGW_BIN}")

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${WAG_EXE}"
    DIRECTORIES ${_search_dirs}
    RESOLVED_DEPENDENCIES_VAR _resolved
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    PRE_EXCLUDE_REGEXES "api-ms-win-.*" "ext-ms-.*"
    POST_EXCLUDE_REGEXES
        ".*[/\\\\][Ww]indows[/\\\\].*"
        ".*[/\\\\][Pp]rogram Files[/\\\\].*"
        ".*[/\\\\][Pp]rogram Files \\(x86\\)[/\\\\].*"
)

foreach(_dep IN LISTS _resolved)
    file(TO_CMAKE_PATH "${_dep}" _dep_path)
    string(TOLOWER "${_dep_path}" _dep_lower)

    # 系统目录不随包分发。
    if(_dep_lower MATCHES "^[a-z]:/windows/" OR
       _dep_lower MATCHES "^[a-z]:/program files/" OR
       _dep_lower MATCHES "^[a-z]:/program files \\(x86\\)/")
        continue()
    endif()

    get_filename_component(_dep_name "${_dep_path}" NAME)
    if(_dep_name STREQUAL "")
        continue()
    endif()

    if(EXISTS "${WAG_DEST_DIR}/${_dep_name}")
        set(_dep_existed TRUE)
    else()
        set(_dep_existed FALSE)
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_dep_path}" "${WAG_DEST_DIR}"
        RESULT_VARIABLE _copy_result)
    if(NOT _copy_result EQUAL 0)
        message(FATAL_ERROR
            "copy_runtime_closure: 复制失败：${_dep_path}")
    endif()
    if(NOT _dep_existed)
        message(STATUS "运行时依赖已补：${_dep_name}")
    endif()
endforeach()

list(LENGTH _unresolved _unresolved_count)
if(_unresolved_count GREATER 0)
    # 未解析的通常是 API set；它们由 Windows 提供。
    message(STATUS "运行时依赖：${_unresolved_count} 个未解析项（按系统 API 忽略）")
endif()
