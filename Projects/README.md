# Projects

这里是 LuaWAG 的“文件夹卡带”目录。

## 先把卡带主机说明白

老式卡带主机是这样的：

```text
主机       固定不动
卡槽       只有一个
卡带       一盘一盘换
```

LuaWAG 借用了这个结构：

```text
主机       Luminosa/ 这一整套运行时
卡槽       Luminosa/Project/ 这个固定名字
卡带       Projects/ 下的每个工程文件夹
卡带架     Projects/ 本身（只是存放处）
```

引擎启动时只看一个位置：

```text
Luminosa/Project/
```

它**不会**去扫描 `Projects/`，也不会列出“有哪些游戏”。  
`Projects/` 只是一个放卡带的架子。

---

## 一张卡带包含什么

一张卡带就是一个完整的工程文件夹，通常包含：

```text
main.lua          入口脚本：定义 GameInit / MainFrameFunc / MainRenderFunc 等
shaders/          这个工程自己的 GLSL
audio/            这个工程自己的音频
*.lua             这个工程的模块
字体 / 图片等      这个工程自己的资源
```

卡带内部可以使用：

```text
WagLib/           引导层
Lumi/Basic/       官方基础库
```

但这两层属于“主机”，不属于卡带。  
换卡带时只换 `Project`，不要把 `WagLib` 或 `Lumi/Basic` 一起换掉。

---

## 插入卡带

假设现在 `Luminosa/Project` 里是启动画面，你想换成 SeedGame：

```text
1. 关闭引擎
2. 把 Luminosa/Project 改名为 Luminosa/Theme
3. 把 Projects/SeedGame 整个文件夹连同内容复制到 Luminosa/ 下
4. 把复制出来的 Luminosa/SeedGame 改名为 Luminosa/Project
5. 启动 LuaWAG.exe
```

`Theme` 只是你给旧卡带起的暂存名。  
引擎完全无视 `Luminosa/Theme`，也完全无视 `Luminosa/Theme2`、`Luminosa/OldProject` 之类——除了 `Project`，`Luminosa/` 下的目录名都不参与装载。  
`Theme` 与 `Projects/` 也不冲突：前者在 `Luminosa/` 里当暂存槽，后者在仓库根目录当卡带架。

**为什么必须关引擎？**  
因为卡槽根目录是在启动装载时确定的，脚本模块也已经进入 `require` 缓存。  
热重载只会重载已经装载的脚本，不会重新选择 `Project` 根目录，也不会清空“旧卡带”的模块缓存。  
运行中改名还可能让后续资源路径指向另一个目录，或在 Windows 上被文件占用拒绝。  
换卡带不是热重载能覆盖的操作，所以按“关引擎 → 换目录 → 再启动”处理。

结果必须是：

```text
Luminosa/
└─ Project/
   ├─ main.lua
   ├─ game.lua
   ├─ ...
   └─ shaders/
```

不是：

```text
Luminosa/
└─ Project/
   └─ SeedGame/
      └─ main.lua
```

`Project` 是卡槽名，卡带内容要直接位于 `Luminosa/Project/` 下。  
不能直接把 `SeedGame` 文件夹放进 `Project` 里面；那会变成 `Project/SeedGame`，装载器不会把它当成卡带入口。

换回启动画面：

```text
1. 关闭引擎
2. 把 Luminosa/Project 改名或移走
3. 把 Projects/Begin 整个文件夹连同内容复制到 Luminosa/ 下，并改名为 Project
4. 启动 LuaWAG.exe
```

注意是复制卡带，不是把 `Projects/SeedGame` 原地搬空。  
`Projects/` 是卡带架，最好保留原卡带，方便下次再换。

---

## 卡槽规则

### 同一时刻只有一盘卡带

```text
Luminosa/Project 只能有一个。
```

不要同时放：

```text
Luminosa/Project/
Luminosa/Project2/
```

然后期待引擎自己选一个。  
引擎只认 `Project` 这个名字，多出来的目录不会被自动识别成“第二盘卡带”。

### 空卡槽

空卡槽也能启动引擎，而且不是错误。

两种情况都算空卡槽：

```text
Luminosa/ 下没有 Project 目录
```

或：

```text
Luminosa/Project/ 存在，但里面没有任何工程脚本
```

这时引擎仍然会：

```text
初始化窗口、渲染器、音频、输入
装载 WagLib / Lumi
跳过 Project 层
进入帧循环
```

因为没有 Project 绘制任何东西，窗口会显示清屏用的哨兵蓝色。  
这是正常现象，不是渲染失败，也不是“卡带坏了”。

如果你看到哨兵蓝，先检查：

```text
1. Luminosa/Project 是否存在
2. Luminosa/Project/main.lua 是否存在
3. Project 里的文件是不是被多套了一层，例如 Project/SeedGame/main.lua
```

### `Project` 这个名字是接口

卡槽名不是装饰，是装载器实际使用的路径：

```text
Luminosa/Project/main.lua
Luminosa/Project/shaders/...
Luminosa/Project/audio/...
```

所以：

- 卡带必须放在 `Luminosa/Project`；
- 它不能直接放在 `Projects/` 里运行；
- `Projects/` 下的名字只是卡带名，不是引擎读取的路径。

### 卡带内部的模块名

卡带内部用 `require` 引用自己的模块时，模块名要以**当前卡槽名**为根：

```lua
local S = require("Project.state")
```

如果卡带被复制成 `Project`，上面这条就能工作。

本目录里的 `SeedGame` 使用了“从当前模块名推导前缀”的写法：

```lua
local PREFIX = (...):match("^(.*)%.[^.]+$")
local S = require(PREFIX .. ".state")
```

所以它无论是叫 `SeedGame` 还是被复制成 `Project`，内部模块路径都能对上。

### 卡带内部的资源路径

卡带自己的 shader、音频、字体，应当相对于卡带根目录找：

```text
Luminosa/Project/shaders/...
Luminosa/Project/audio/...
Luminosa/Project/字体.ttf
```

不要把路径写死成旧卡带名，例如：

```text
Luminosa/SeedGame/shaders/...   ← 复制成 Project 后就错了
```

---

## 现有卡带

### Begin

启动画面。  
它仍然只是一个普通工程；不想要启动画面时，可以完全不用它。

“不用”不等于要特殊配置：

- `Projects/Begin` 本身不会被引擎扫描；删掉、改名、留着吃灰都不影响引擎。
- 如果不想看到启动画面，不要把 `Begin` 放进 `Luminosa/Project`，直接让游戏卡带占用 `Project` 即可。
- 如果 `Project` 空着或根本不存在，引擎仍然能启动，只是显示空卡槽的哨兵蓝色。

### SeedGame

弹幕 ACT 小游戏示例。  
带菜单、暂停、重开和 replay 回放。

---

## 总结

```text
Luminosa/ 是主机；
Luminosa/Project 是卡槽；
Projects/ 下的文件夹是卡带；
换游戏 = 关引擎、换卡带、再启动。
```

`Projects/` 不会被自动扫描，`Project` 也不能同时存在两份。  
记住这两条，卡带设计就不会用错。
