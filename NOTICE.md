# NOTICE

LuaWAG 使用或依赖的第三方组件、系统接口与许可证说明。

本文件不是法律意见；它只记录本仓库实际使用到的组件、许可证名称与上游入口。

---

## 1. 项目自身

LuaWAG 本体的许可证见仓库根目录的 `LICENSE`：

```text
Apache License 2.0
```

## 2. 作者说明

以下内容是关于项目协作与品牌标识的说明，**不是对 Apache License 2.0 的额外限制**，也不改变、不缩小 `LICENSE` 对源码的授权。

### 源码分发

LuaWAG 源码仍按 Apache License 2.0 授权，可以自由使用、修改和分发。
分发时请保留 `LICENSE`、`NOTICE` 以及源码中的版权声明。

### 名称与 Logo

引擎名称、Logo、图标等品牌标识的著作权由作者保留。
Apache License 2.0 第 6 节明确不授予商标使用权，因此这不构成对源码授权的附加限制。

Logo 可以随源码一起分发，但只应用于标识 LuaWAG 原项目；
基于 LuaWAG 的派生项目如果使用自己的名称和 Logo，请勿让用户误以为它是 LuaWAG 官方版本。

### PR 与修改

- 小修可以直接改：文档、注释、构建修复、明显的兼容性修复、拼写错误等。
- 破坏性更新需要先联系作者讨论，包括但不限于：
  - Lua API 或用户词汇改名；
  - C++ 接口、结构体字段、布局或二进制格式改名；
  - 文件格式、rep 格式、配置字段改名；
  - 会改变已有工程行为或兼容性的修改。

这条是协作约定，不是许可证条款；它不限制 Apache-2.0 已授予的权利。

### Fork 与独立分支

如果你 Fork 本项目，并**明确不打算把修改合并回上游**：

- 可以随意修改、魔改、重写，不需要遵守上面“破坏性更新先联系作者”的协作流程；
- Apache License 2.0 仍然适用：分发时保留 `LICENSE`、`NOTICE`、版权声明与许可声明；
- 如果之后改变主意，希望把修改合并回上游，破坏性变更仍需先联系作者讨论。

这条同样是协作约定，不是对 Apache-2.0 的额外限制。

### 交流

无论你是否打算把修改合并回上游、无论做了何种程度的魔改、无论用于个人项目还是商业项目，都欢迎联系作者交流。

问题、设计讨论、分支实现、使用反馈都可以聊；联系方式见 `README.md` 的“联系方式”一节，或直接在仓库开 Issue。

作者欢迎交流，但不承诺固定的响应时间或支持义务。

---

## 3. SDL3

| 项目 | 说明 |
|---|---|
| 组件 | SDL3（Simple DirectMedia Layer 3） |
| 用途 | 窗口、输入、音频、文件与平台子系统 |
| 许可证 | **zlib License** |
| 上游 | https://github.com/libsdl-org/SDL |
| 许可证入口 | https://www.libsdl.org/license.php |
| 许可 FAQ | https://wiki.libsdl.org/SDL3/FAQLicensing |

本仓库不包含 SDL3 的源码或二进制；Windows 构建通过 MSYS2 UCRT64 的 SDL3 包获取。
SDL3 的版权归 Sam Lantinga 与 SDL 贡献者所有，按其 zlib License 使用。

SDL 官方 FAQ 明确说明：

```text
SDL 可免费用于商业应用，不收取许可费或版税。
```

---

## 4. LuaJIT

| 项目 | 说明 |
|---|---|
| 组件 | LuaJIT 2.1 |
| 用途 | Lua 运行时与脚本执行 |
| 许可证 | **MIT License** |
| 上游 | https://github.com/LuaJIT/LuaJIT |
| 许可证入口 | https://github.com/LuaJIT/LuaJIT/blob/v2.1/COPYRIGHT |

本仓库不包含 LuaJIT 的源码或二进制；Windows 构建通过 MSYS2 UCRT64 的 LuaJIT 包获取。
LuaJIT 的版权归 Mike Pall 与贡献者所有，按其 MIT License 使用。

---

## 5. stb_truetype

| 项目 | 说明 |
|---|---|
| 组件 | `stb_truetype.h` v1.28 |
| 用途 | TTF/OTF 字体解析与字形栅格化 |
| 许可证 | **Public Domain 或 MIT License（二选一）** |
| 上游 | https://github.com/nothings/stb/blob/master/stb_truetype.h |
| 仓库内文件 | `third_party/stb_truetype.h` |

该文件头部标注：

```text
stb_truetype.h - v1.28 - public domain
authored from 2009-2021 by Sean Barrett / RAD Game Tools
```

文件末尾同时给出两套可选许可：

```text
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett

ALTERNATIVE B - Public Domain (www.unlicense.org)
```

本项目采用其 **Public Domain / MIT 双许可**；完整文本保留在 `third_party/stb_truetype.h` 末尾。

---

## 6. stb_vorbis

| 项目 | 说明 |
|---|---|
| 组件 | `stb_vorbis.c` v1.24 |
| 用途 | Ogg Vorbis 音频解码 |
| 许可证 | **Public Domain 或 MIT License（二选一）** |
| 上游 | https://github.com/nothings/stb/blob/master/stb_vorbis.c |
| 仓库内文件 | `third_party/stb_vorbis.c` |

该文件同样在末尾提供两套可选许可：

```text
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett

ALTERNATIVE B - Public Domain (www.unlicense.org)
```

本项目采用其 **Public Domain / MIT 双许可**；完整文本保留在 `third_party/stb_vorbis.c` 末尾。

---

## 7. OpenGL

| 项目 | 说明 |
|---|---|
| 组件 | OpenGL 3.3 Core API |
| 用途 | 渲染管线 |
| 性质 | **API / 规范，不是本仓库捆绑的库** |
| 实现来源 | Windows 自带 `opengl32.dll` 与显卡驱动 |
| Khronos OpenGL | https://www.khronos.org/opengl/ |
| Khronos 法律信息 | https://www.khronos.org/legal/ |
| Khronos 商标指南 | https://www.khronos.org/legal/khronos-trademark-guidelines |
| 规范版权/许可 | https://registry.khronos.org/speccopyright.html |

本仓库**不包含 OpenGL 实现源码，也不分发 OpenGL 驱动**。
引擎通过系统提供的 `opengl32.dll` 和驱动创建 OpenGL 3.3 Core 上下文；因此不存在“把 Khronos 的 OpenGL 实现打包进 LuaWAG”这件事。

`OpenGL` 是 Khronos Group 的商标。
本仓库只在描述兼容性时使用该名称，不表示 Khronos 对本项目的认可或背书。

---

## 8. 许可兼容性

上述组件的许可证类型：

```text
SDL3              zlib License
LuaJIT            MIT License
stb_truetype      Public Domain / MIT
stb_vorbis        Public Domain / MIT
OpenGL            系统 API / Khronos 规范，不是捆绑实现
LuaWAG 本体        Apache License 2.0
```

这些组件与 Apache-2.0 项目一起分发时不存在额外的 copyleft 约束。
如果将来把 SDL3、LuaJIT 的源码或二进制直接放进发布包，应同时保留它们各自的版权与许可文本。

---

## 9. 上游链接汇总

```text
SDL3
https://github.com/libsdl-org/SDL
https://www.libsdl.org/license.php
https://wiki.libsdl.org/SDL3/FAQLicensing

LuaJIT
https://github.com/LuaJIT/LuaJIT
https://github.com/LuaJIT/LuaJIT/blob/v2.1/COPYRIGHT

stb_truetype
https://github.com/nothings/stb/blob/master/stb_truetype.h

stb_vorbis
https://github.com/nothings/stb/blob/master/stb_vorbis.c

OpenGL / Khronos
https://www.khronos.org/opengl/
https://www.khronos.org/legal/
https://www.khronos.org/legal/khronos-trademark-guidelines
https://registry.khronos.org/speccopyright.html
```
