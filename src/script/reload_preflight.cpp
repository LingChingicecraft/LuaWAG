#include "script/reload_preflight.hpp"

#include "script/state.hpp"
#include "platform/utf8_path.hpp"

#include <lua.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace wag::script {
namespace {

namespace fs = std::filesystem;

void add_error(preflight_result& out, std::string path, std::string message) {
    out.errors.push_back(reload_error{std::move(path), std::move(message)});
}

constexpr char kModuleCacheKey[] = "wag.reload.module_cache";

std::string path_to_module(std::string rel) {
    if (rel.size() >= 4 && rel.compare(rel.size() - 4, 4, ".lua") == 0) {
        rel.resize(rel.size() - 4);
    }
    if (rel.size() >= 5 && rel.compare(rel.size() - 5, 5, "/init") == 0) {
        rel.resize(rel.size() - 5);
    }
    std::replace(rel.begin(), rel.end(), '/', '.');
    return rel;
}

int cached_module_searcher(lua_State* L) {
    const char* const name = luaL_checkstring(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, kModuleCacheKey);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    lua_getfield(L, -1, name);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 2);
        return 0;
    }

    lua_getfield(L, -1, "source");
    lua_getfield(L, -2, "path");
    const char* const path = lua_tostring(L, -1);
    size_t size = 0;
    const char* const source = lua_tolstring(L, -2, &size);
    if (source == nullptr || path == nullptr) {
        lua_pop(L, 4);
        return 0;
    }

    const std::string chunk = "@" + std::string(path);
    if (luaL_loadbuffer(L, source, size, chunk.c_str()) != 0) {
        // 返回错误对象，让 require 报出真实原因。
        return 1;
    }
    return 1;
}

} // namespace

bool parse_reload_scope(std::string_view text, reload_scope& out) noexcept {
    if (text == "scripts") {
        out = reload_scope::scripts;
        return true;
    }
    return false;
}

preflight_result preflight_scripts(state& script_state,
                                   const std::string& install_root) {
    preflight_result out;
    out.scope = reload_scope::scripts;

    const fs::path base = wag::paths::path_from_utf8(install_root) / "Luminosa";
    std::error_code ec;
    if (!fs::is_directory(base, ec)) {
        add_error(out, "Luminosa",
                  "Luminosa 目录不存在或不可读：" +
                      wag::paths::utf8_from_path(base));
        return out;
    }

    std::vector<std::pair<std::string, fs::path>> files;

    fs::recursive_directory_iterator it(
        base, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    if (ec) {
        add_error(out, "Luminosa", "目录扫描失败：" + ec.message());
        return out;
    }

    for (; it != end; it.increment(ec)) {
        if (ec) {
            add_error(out, "Luminosa", "目录扫描中断：" + ec.message());
            break;
        }

        const fs::directory_entry& entry = *it;
        if (entry.is_symlink(ec)) {
            if (entry.is_directory(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        if (entry.path().extension() != ".lua") {
            continue;
        }

        std::error_code rel_ec;
        const fs::path rel = fs::relative(entry.path(), base, rel_ec);
        if (rel_ec) {
            add_error(out, wag::paths::utf8_from_path(entry.path()),
                      "无法计算相对路径：" + rel_ec.message());
            continue;
        }
        std::string rel_text = wag::paths::utf8_from_path(rel);
        std::replace(rel_text.begin(), rel_text.end(), '\\', '/');
        files.emplace_back(std::move(rel_text), entry.path());
    }

    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    bool has_bootstrap = false;
    lua_State* const L = script_state.raw();
    if (L == nullptr) {
        add_error(out, "Luminosa", "Lua 状态未建立，无法预检");
        return out;
    }

    for (const auto& [rel, full] : files) {
        if (rel == "WagLib/init.lua") {
            has_bootstrap = true;
        }

        std::ifstream in(full, std::ios::binary);
        if (!in) {
            add_error(out, rel, "文件无法打开");
            continue;
        }
        std::string source((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
        if (in.bad()) {
            add_error(out, rel, "文件读取失败");
            continue;
        }
        // 与 state::do_file 一致：剥掉 UTF-8 BOM。
        if (source.size() >= 3 &&
            static_cast<unsigned char>(source[0]) == 0xEF &&
            static_cast<unsigned char>(source[1]) == 0xBB &&
            static_cast<unsigned char>(source[2]) == 0xBF) {
            source.erase(0, 3);
        }

        out.sources.push_back(reload_source{rel, source});
        ++out.checked;

        const int top = lua_gettop(L);
        const std::string chunk = "@" + wag::paths::utf8_from_path(full);
        const int rc = luaL_loadbuffer(L, source.data(), source.size(),
                                       chunk.c_str());
        if (rc != 0) {
            const char* const message = lua_tostring(L, -1);
            add_error(out, rel,
                      message != nullptr ? message : "Lua 编译失败");
        }
        lua_settop(L, top);
    }

    if (!has_bootstrap) {
        add_error(out, "WagLib/init.lua", "引导脚本缺失");
    }

    out.ok = out.errors.empty();
    return out;
}

void install_cached_modules(state& script_state,
                            const std::vector<reload_source>& sources) {
    lua_State* const L = script_state.raw();
    if (L == nullptr) {
        return;
    }

    lua_newtable(L);
    for (const reload_source& s : sources) {
        std::string full = s.path;
        if (full.size() >= 4 &&
            full.compare(full.size() - 4, 4, ".lua") == 0) {
            full.resize(full.size() - 4);
        }
        std::replace(full.begin(), full.end(), '/', '.');

        const std::string init = path_to_module(s.path);
        const std::string names[2] = {full, init};
        for (const std::string& name : names) {
            if (name.empty()) {
                continue;
            }
            lua_newtable(L);
            lua_pushlstring(L, s.source.data(), s.source.size());
            lua_setfield(L, -2, "source");
            lua_pushlstring(L, s.path.data(), s.path.size());
            lua_setfield(L, -2, "path");
            lua_setfield(L, -2, name.c_str());
        }
    }
    lua_setfield(L, LUA_REGISTRYINDEX, kModuleCacheKey);

    // 把 C 搜索器插到 package.loaders / package.searchers 的最前面。
    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "loaders");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_getfield(L, -1, "searchers");
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 2);
        return;
    }

    const int n = static_cast<int>(lua_objlen(L, -1));
    for (int i = n; i >= 1; --i) {
        lua_rawgeti(L, -1, i);
        lua_rawseti(L, -2, i + 1);
    }
    lua_pushcfunction(L, cached_module_searcher);
    lua_rawseti(L, -2, 1);

    lua_pop(L, 2);
}

} // namespace wag::script
