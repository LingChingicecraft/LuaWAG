# English

[简体中文](#简体中文)

## Introduction

LuaWAG is a lightweight, general-purpose, Windows-first cross-platform game engine that uses Lua for game logic.

The engine is built with C++20, SDL3, OpenGL 3.3 and LuaJIT. The technology stack itself is portable to other systems. For platform-specific details, see [Platform Support](#platform-support).

LuaWAG does not try to decide what the world should be like for the developer. Gravity, dimensions, space, physics, cameras and gameplay rules are not preset by the engine. The engine handles the frame loop, window, input, audio, rendering primitives and the script boundary; game logic is left entirely to Lua.

It is suitable for games whose rules are defined by the author—from text adventures, platformers and management simulations to bullet-hell shooters—and for people who want to use the engine itself as creative material.

> **Read this before adapting an architecture from another engine.**  
> LuaWAG deliberately uses a different execution model around frame lines, pause and state-mutation authority. If you are coming from any of these engines—including but not limited to Unity, Unreal, Godot, Love2D, and LuaSTG—read [WARNING.md](WARNING.md) first: it lists the concepts that must not be mapped one-to-one.

## Key Features

- **No constants are preset at the game-world layer.**  
  The engine does not hard-code gravity direction, dimensions, physics rules, cameras or gameplay constants. The world has no "default answer"; these values are defined in Lua and can be replaced.

- **Pause is a frame line controlled directly by the engine.**  
  Pause and resume are not a `paused` boolean maintained separately by every game; they are part of the engine's frame structure. While paused, main-loop logic stops, the image is retained, and resume semantics are uniform.

- **The architecture fully supports replay.**  
  Logic advancement and presentation are separated, and input can be recorded and replayed. The sample game can already record, replay, and pause during replay.

- **Thin core, thick script.**  
  C++ keeps only what the platform and runtime require; game logic, UI, physics, scenes and narrative are all done on the Lua side. The engine does not make the game for you—it gives you the language to make it.

- **Rendering does not preset an art style.**  
  The engine provides rendering primitives and custom GLSL entry points, but no "official look". The image is the script's output, not the engine's aesthetic.

- **Runtime hot reload and companion tools.**  
  Lua scripts can be reloaded while the game is running. The companion blueprint console, node inspector, and performance/error data protocols target real development work, not demo screenshots.

- **Windows-first, portable technology stack.**  
  The currently tested platform is Windows 11. The SDL3 / OpenGL / LuaJIT stack leaves room for ports to other desktop platforms, but those platforms have not been verified and no compatibility is promised.

## Download Releases

Version history and known issues: [CHANGELOG.md](CHANGELOG.md).

### Hello World
Download the [Windows release package](./releases/tag/v0.1.2), extract it, and double-click `LuaWAG.exe`.

You should see the engine name and a slogan on the splash screen.

> Note: extract the engine to a writable directory.
> Runtime files such as logs and replays are written to the engine directory or the user directory.

### Run the SeedGame Sample

Rename `Luminosa/Project` to `Luminosa/Theme` (`Theme` is just an example name).

Rename `Luminosa/SeedGame` to `Luminosa/Project`.

You can then play a bullet-hell action game sample with a built-in replay system.

### Writing Game Logic

You can modify the sample game's code, or use it as a reference to build your game from scratch.

For details, see the [API manual](./doc/).

## Building from Source

If you only want to play or write games directly, just download a Release; you do not need this section.

The official Windows build path is below.  
Visual Studio is not required; the official build uses the MSYS2 MinGW toolchain, not MSBuild.

### 1. Prepare MSYS2

After installing MSYS2, open the **UCRT64** terminal—not MSYS, MINGW64 or CLANG64.

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-sdl3 \
  mingw-w64-ucrt-x86_64-luajit
```

These packages provide:

```text
g++ / cc1plus     C++20 compiler
cmake             configuration
ninja             build executor
sdl3              window / input / audio / files
luajit            Lua runtime
```

The SDL3 version used to validate this build is **3.4.18** (the MSYS2 UCRT64 package). SDL3 is still evolving quickly; if you use a different version, check its API/ABI changes first. LuaJIT uses the `luajit-2.1` headers from MSYS2.

Do not mix different MSYS2 subsystems.  
Pair UCRT64 g++ with UCRT64 SDL3 and LuaJIT. Mixing libraries from MINGW64 or CLANG64 most often produces link errors or missing DLLs at runtime, not compile errors.

### 2. Configure

Run this from the repository root:

```bash
cmake -S . -B build/Release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DWAG_NO_EXTERNAL_DEPS=OFF
```

Do not omit `WAG_NO_EXTERNAL_DEPS=OFF`.  
It defaults to `ON`, which is only for the core-check build with no external dependencies. A normal build must enable SDL3 and LuaJIT.

### 3. Build

```bash
cmake --build build/Release
```

The first build compiles everything; after that Ninja rebuilds only changed files.

### 4. Output

```text
build/Release/bin/LuaWAG-Luminosa-ver0.1.2/LuaWAG.exe
```

After the build, `runtime/Luminosa/` is copied next to the executable, so that directory can be launched directly.

This build is an **empty engine**: `runtime/Luminosa/` currently contains only the runtime layers, not `Project/` or `Theme/`. With no content, the window shows the sentinel blue; this is expected, not a rendering failure.

Actual content lives in the Release package (the splash screen and SeedGame sample) or in the repository's `Projects/` directory. See `Projects/README.md` for the folder-as-cartridge layout.

```bash
./build/Release/bin/LuaWAG-Luminosa-ver0.1.2/LuaWAG.exe
```

### Troubleshooting

**`SDL3` is not found**

Make sure `mingw-w64-ucrt-x86_64-sdl3` is installed.
Make sure `WAG_NO_EXTERNAL_DEPS=OFF` is present in the configure command.

**`luajit-2.1/lua.h` is not found**

Make sure `mingw-w64-ucrt-x86_64-luajit` is installed.
Do not mix the system Lua 5.5 headers into the include path.

**g++ reports that `cc1plus` is not found**

This is almost always another toolchain mixed into `PATH`.  
Open a fresh UCRT64 terminal and check:

```bash
which g++
which cmake
which ninja
```

All three should point to `C:\msys64\ucrt64\bin`.

**CMake arguments changed but behavior did not**

CMake caches configuration.  
If it was previously configured with the wrong arguments, deleting `build/Release` and configuring again is usually faster than fighting the cache.

## Platform Support

So far the project has only been built on Windows 11 x86_64.  
There are no Linux or macOS packages, and no ARM or 32-bit builds.

The only machine configuration I have actually built on is:

- OS: Windows 11 x86_64
- CPU: AMD Ryzen 7 PRO 8845HS w/ Radeon 780M Graphics
- GPU: NVIDIA RTX 2000 Ada Generation Laptop

Besides my own machine, the bullet-hell action game sample has also been tested on:

- OS
        Windows 10 x86_64
        Windows 11 x86_64
- CPU
        Intel64 Family 6
        AMD64 Family 23
- GPU
        NVIDIA GeForce RTX 5060 Laptop
        NVIDIA GeForce RTX 4060 Ti
        NVIDIA GeForce RTX 3070
        NVIDIA GeForce RTX 3060 Laptop
        NVIDIA GeForce RTX 4060 Laptop
        AMD Radeon(TM) Graphics


The build environment is MSYS2 UCRT64.  
Windows 10 runs normally, but it is not the primary support target. Windows 11 remains the primary target.
Linux, macOS and Windows ARM have not been built or run.

Rendering requires **OpenGL 3.3 core**.  
Normal desktop GPUs should be fine, but old integrated GPUs, outdated drivers, virtual machines and software rendering in Remote Desktop are not guaranteed.
If context creation fails, the engine reports an L4-level error in the log instead of pretending to run.

The technology stack itself is cross-platform: SDL3 handles window/input/audio, OpenGL handles rendering, and LuaJIT handles scripting.  
The platform layer, however, is still Windows-specific: executable paths, DPI, window notices and file path handling all contain Win32 code.  
So Linux/macOS is not just "swap the compiler"; some porting work is required. macOS also has to deal with OpenGL 3.3 being deprecated.

Two more things:
- Put the engine in a writable directory, not in a location such as Program Files.
  Logs, replays and debug files need to be written to disk.

- The executable is not code-signed.
  SmartScreen may appear on first launch; this is normal.


If you try to build on another platform, you are welcome to send the system version, compiler, SDL3 version and full logs.

## Contact

Email: LingChingicecraft@outlook.com
Engine QQ group: 185485426

For related information, see [NOTICE](./NOTICE.md).

## Companion Projects (in development)

### LuaWAGBluePrintController (BPC)

Blueprint console.

### LuminosaGraphNodeInspector (LGN)

Node inspector.

### LuminosaLuminosaEditor (LLE)

Lighting editor.

### LuminosaFontLayoutStudio (FLS)

Font processor.


# 简体中文

[English](#english)

## 介绍

LuaWAG 是一款使用 Lua 编写游戏逻辑的Windows-First跨平台通用轻量游戏引擎。

引擎采用C++20、SDL3、OpenGL 3.3 与 LuaJIT 开发。技术栈本身可移植到其他系统。具体跨平台内容请见 [平台支持](#平台支持)。

LuaWAG 不试图替开发者决定“世界应该是什么样”。重力、维度、空间、物理、镜头、玩法规则，都不由引擎预设；引擎负责帧循环、窗口、输入、音频、渲染原语和脚本边界，游戏逻辑则完全交给 Lua。

它适合从文字冒险、平台动作、模拟经营到弹幕射击这类“规则由作者定义”的游戏，也适合想把引擎本身当作创作材料的人。

> **在把其他引擎的架构套进来之前，先读这个。**  
> LuaWAG 在帧线、暂停和状态变更权上有意采用了不同的执行模型。如果你来自包括但不限于 Unity、Unreal、Godot、Love2D 或 LuaSTG，请先读 [WARNING.md](WARNING.md)：那里列出了不能直接一一对应的概念。

## 主要功能与特色

- **游戏世界层不预设任何常量。**  
  引擎不内置重力方向、维度、物理规则、相机或玩法常量。世界没有“默认答案”，这些量由 Lua 定义，也可以被替换。

- **暂停是一条帧线，由引擎直接控制。**  
  暂停和恢复不是让每个游戏自己维护一个 `paused` 布尔值，而是引擎帧结构的一部分。暂停期间主循环逻辑停止、画面保留、恢复语义统一。

- **架构完全支持 replay 回放。**  
  逻辑推进与呈现分离，输入可以被记录和重放。示例游戏已经能够录制、回放、在回放中暂停。

- **薄核心，厚脚本。**  
  C++ 只保留平台与运行时必需的部分；游戏逻辑、UI、物理、场景、叙事都在 Lua 侧完成。引擎不替你做游戏，只给你做游戏的语言。

- **渲染不预设美术风格。**  
  引擎提供渲染原语和自定义 GLSL 入口，但不内置“官方画风”。画面是脚本的输出，不是引擎的审美。

- **运行时热重载与配套工具。**  
  游戏运行中可以重载 Lua 脚本；配套的蓝图控制台、结点检查器、性能与错误数据协议，面向的是实际开发过程，而不是演示截图。

- **Windows-first，技术栈可移植。**  
  当前实测平台是 Windows 11；SDL3 / OpenGL / LuaJIT 的技术栈为其他桌面平台保留了移植空间，但没有经过验证，也不做兼容性承诺。

## 下载 Release

版本变化与已知问题见 [CHANGELOG.md](CHANGELOG.md)。

### Hello World
下载[Windows发行包](./releases/tag/v0.1.2)，解压后双击打开LuaWAG.exe。

应该能看到启动画面上的引擎名字和一句Slogan。

> 注意：请将引擎解压到可写目录。
> 日志、rep 等运行期文件会写到引擎目录/用户目录。

### 运行 SeedGame 示例

将 Luminosa/Project 重命名为 Luminosa/Theme（Theme为示例名字）

将 Luminosa/SeedGame 重命名为 Luminosa/Project

即可游玩一个自带replay系统的弹幕ACT小游戏示例。

### 编写游戏逻辑

你可以修改这个游戏示例的代码。也可以参考其代码从零开发你的游戏。

详细内容请参阅[API手册](./doc/)

## 从源码构建

如果你只是想游玩和直接编写游戏，直接下载 Release 就行，不需要看这一节。

下面这条是 Windows 官方构建路径。  
不要求安装 Visual Studio；官方构建走的是 MSYS2 的 MinGW 工具链，不是 MSBuild。

### 1. 准备 MSYS2

安装 MSYS2 后，打开 **UCRT64** 终端，不要用 MSYS、MINGW64 或 CLANG64。

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-sdl3 \
  mingw-w64-ucrt-x86_64-luajit
```

这几个包分别对应：

```text
g++ / cc1plus     C++20 编译器
cmake             配置
ninja             构建执行器
sdl3              窗口 / 输入 / 音频 / 文件
luajit            Lua 运行时
```

本次验证使用的 SDL3 版本是 **3.4.18**（MSYS2 UCRT64 包）。SDL3 仍在快速迭代；如果使用不同版本，请先对照它的 API/ABI 变更。LuaJIT 使用 MSYS2 的 `luajit-2.1` 头文件。

不要混用 MSYS2 的不同子系统。  
UCRT64 的 g++ 配 UCRT64 的 SDL3 和 LuaJIT；混进 MINGW64 或 CLANG64 的库，最容易出现的不是编译错误，而是链接或运行期找不到 DLL。

### 2. 配置

在仓库根目录执行：

```bash
cmake -S . -B build/Release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DWAG_NO_EXTERNAL_DEPS=OFF
```

`WAG_NO_EXTERNAL_DEPS=OFF` 不能漏。  
它默认是 `ON`，只用于“完全不接外部依赖”的内核检查构建；正常构建必须打开 SDL3 和 LuaJIT。

### 3. 构建

```bash
cmake --build build/Release
```

第一次会完整编译一遍，之后 Ninja 只重编改动过的文件。

### 4. 产物

```text
build/Release/bin/LuaWAG-Luminosa-ver0.1.2/LuaWAG.exe
```

构建完成后，`runtime/Luminosa/` 会自动复制到 exe 旁边，所以这个目录可以直接启动。

构建出来的是**空引擎**：当前 `runtime/Luminosa/` 只有运行时层，没有 `Project/` 或 `Theme/`。没有内容时窗口显示哨兵蓝色，这是正常现象，不是渲染失败。

实际内容在 Release 包（启动画面 + SeedGame 示例）或仓库根目录的 `Projects/` 文件夹里。卡带式目录的用法见 `Projects/README.md`。

```bash
./build/Release/bin/LuaWAG-Luminosa-ver0.1.2/LuaWAG.exe
```

### 常见问题

**`SDL3` 找不到**

确认装的是 mingw-w64-ucrt-x86_64-sdl3
确认配置命令里 WAG_NO_EXTERNAL_DEPS=OFF

**`luajit-2.1/lua.h` 找不到**

确认装的是 mingw-w64-ucrt-x86_64-luajit
不要在 include 路径里混入系统里的 Lua 5.5

**g++ 报 `cc1plus` 找不到**

基本都是 PATH 里混进了别的工具链。  
重新打开 UCRT64 终端，确认：

```bash
which g++
which cmake
which ninja
```

三条都应该指向 `C:\msys64\ucrt64\bin`。

**改了 CMake 参数但行为没变**

CMake 会缓存配置。  
如果之前用错参数配置过，直接删掉 `build/Release` 重新来，通常比和缓存搏斗快。

## 平台支持

当前只构建过 Windows 11 x86_64。  
没有 Linux 包，没有 macOS 包，也没有 ARM 或 32 位版本。

我实际构建过的机器配置只有：

- 系统：Windows 11 x86_64
- CPU：AMD Ryzen 7 PRO 8845HS w/ Radeon 780M Graphics 
- 图形卡：NVIDIA RTX 2000 Ada Generation Laptop

除了我自己的机器以外，实测过上文那个弹幕ACT小游戏示例的机器配置还有：

- 系统
        Windows 10 x86_64
        Windows 11 x86_64
- CPU
        Intel64 Family 6
        AMD64 Family 23
- 图形卡    
        NVIDIA GeForce RTX 5060 Laptop
        NVIDIA GeForce RTX 4060 Ti
        NVIDIA GeForce RTX 3070
        NVIDIA GeForce RTX 3060 Laptop
        NVIDIA GeForce RTX 4060 Laptop
        AMD Radeon(TM) Graphics


构建环境是 MSYS2 UCRT64。  
Windows 10 能正常跑，但不是首要支持方向。首要方向依然是Windows 11。
Linux、macOS、Windows ARM 目前都没构建过，也没跑过。

渲染要求是 **OpenGL 3.3 core**。  
正常桌面显卡基本没问题，但老集显、旧驱动、虚拟机和远程桌面里的软件渲染不保证能起来。
上下文创建失败时引擎会直接在日志里报 L4 级错误，不会假装自己在跑。

技术栈本身是能跨平台的：SDL3 管窗口/输入/音频，OpenGL 管渲染，LuaJIT 管脚本。  
但平台层现在还是 Windows 的：exe 路径、DPI、窗口提示条、文件路径处理，都有 Win32 代码。  
所以 Linux/macOS 不是“换个编译器”就能跑，需要做一些移植工作。macOS 还要额外面对 OpenGL 3.3 已被弃用这件事。

另外两件事：
- 请把引擎放在可写目录，别放 Program Files等位置
  日志、rep、调试文件要写盘

- exe 没有代码签名
  首次运行可能弹 SmartScreen，属于正常现象


如果你在别的平台上尝试构建，欢迎把系统版本、编译器、SDL3 版本和完整日志发来。  

## 联系方式

邮箱：LingChingicecraft@outlook.com
引擎QQ交流群：185485426

还有一些相关内容请见[NOTICE](./NOTICE.md)

## 配套项目（开发中）

### LuaWAGBluePrintController（BPC）

蓝图控制台。

### LuminosaGraphNodeInspector（LGN）

结点检查器。

### LuminosaLuminosaEditor（LLE）

光照编辑器。

### LuminosaFontLayoutStudio（FLS）

字体处理器。