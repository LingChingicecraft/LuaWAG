# WARNING

[English](#English)
[简体中文](#简体中文)

# English

> This is an **architecture reading warning**, not a security advisory.
>
> This repository is not an approximation of any of these engines, including but not limited to Unity, Unreal, Godot, Love2D, and LuaSTG.  
> Before writing game logic, changing the architecture, adding APIs, or writing examples, make sure you are not carrying approximate concepts from other engines directly into this project.

Many of LuaWAG's designs are not "the usual approach under a different name." They are deliberately re-partitioned around **determinism, pause, replay, and the script boundary**.

If you enter with another engine's vocabulary, the most likely failure is not a compile error:

```text
You misunderstand the structure, yet the code still runs;
once it runs, it leaks everywhere.
```

---

## First, Change These Five Rulers

### 1. FrameLine Is Not a Timeline

A `FrameLine` is ordered by **frame sequence**, not wall-clock time.  
It is also not a show-authoring tool like Unity Timeline.

This project uses "frame line" instead of "timeline" because "timeline" is easily understood as:

```text
arranging animation, audio, and camera work on a time axis
```

A frame line here is:

```text
an execution protocol that partitions "who has the authority
to mutate game state on this frame"
```

The three logic frame lines are:

```text
ExtFrameFunc
MainFrameFunc
PauseFrameFunc
```

They are not three feature callbacks. They are three partitions of **state-mutation authority**.

### 2. The Platform Layer Is Not the World Layer

The platform layer owns:

```text
window, input, files, audio, GPU, frame loop, pause control, script state
```

The world layer owns:

```text
space, dimensions, time, physics, lighting, topology, gameplay rules
```

"the engine does not preset" constrains the **world layer**, not the platform layer.  
The platform layer must have an execution protocol; otherwise even "what is a frame?" would not exist.

Do not conflate these two statements:

```text
"the engine does not preset world rules"
≠
"the engine must not have an execution protocol either"
```

### 3. Logic Producers Are Not Presentation Consumers

`render` / `audio` consume state; they do not hold state-mutation authority.  
They are not a "fourth logic frame line."

Frame lines partition:

```text
who has the authority to mutate game state
```

not:

```text
every piece of code that runs once per frame is a frame line
```

So do not ask:

```text
"Why not split render / audio into two more frame lines?"
```

They belong to the presentation stage. That is a different classification dimension from logic frame lines.

### 4. Pause Is Not a Game Boolean

Pause is not an `ignore_pause` switch on an object,  
and it is not "a `paused` variable maintained by game code."

Pause is a control primitive held by the engine.  
When the system pauses, the logic frame lines are suspended as a whole.  
Script-side partial pause, slow motion, and time-stop are gameplay inside the Main line;  
they cannot penetrate the system pause.

This is why script-side pause tricks are safe to play with:

```text
Main line is suspended
→ every partial-pause timer inside the Main line freezes naturally
→ it continues naturally on resume
```

No subsystem needs to check a global `paused` flag.

### 5. Compute Load Is Not Protocol Semantics

Heavy AI inference and heavy physics simulation still run on the engine,  
and they still need pause.

What they change is:

```text
whether the frame budget is sufficient
```

not:

```text
whether the number of frame lines is sufficient
```

AI that cannot finish in one frame needs incremental computation, coroutine slicing, or background jobs.  
That is a performance/task-protocol problem, not "three lines are not enough."

---

## Three Most Common Walls

### Wall 1: Treating FrameLine as Timeline

Wrong understanding:

```text
"FrameLine is just another name for Timeline."
```

Correct understanding:

```text
Timeline arranges show assets;
FrameLine partitions state-mutation authority.
```

The former is a content tool. The latter is an execution protocol.

### Wall 2: Treating render / audio as a Fourth Line

Wrong understanding:

```text
"Since there are Ext / Main / Pause, why are there no Render / Audio lines?"
```

Correct understanding:

```text
render / audio are consumers of state, not producers.
They do not participate in the partition of "who has the authority to mutate state."
```

Logic frame lines and the presentation stage are two different dimensions.

### Wall 3: Treating Pause as `timeScale` or a Global Boolean

Wrong understanding:

```text
"Pause means the game maintains its own paused flag,
or scales time to zero."
```

Correct understanding:

```text
Pause is a control primitive held by the engine;
script-side pause tricks are state inside the Main line,
not a bypass around the system pause.
```

If every subsystem has to check the pause flag itself,  
eventually some timer, some animation, or some third-party library will be missed.

---

## Two More Common Misconceptions

### "A Higher Frame Rate Makes Graphics Smoother"

In the default frame loop:

```text
dtg = 1/60
```

Every time logic is called, game time advances by `1/60` second.

So raising the logic frame rate is not simply "smoother." It means:

```text
more game time advances per second
→ the game runs faster
```

The render frame rate only changes how often the picture is updated.  
Logic frame rate and render frame rate are not the same quantity.

### "`__gc` Can Be Used for Game Lifetime"

`__gc` does not run on the three logic frame lines,  
and finalizer order is not guaranteed.

Do not use it for:

```text
entity destruction
event unsubscription
timer cancellation
resource release
saving
loot / sound / UI state changes
```

At most, it is suitable for pure Lua memory cleanup with no observable gameplay side effects.

---

## Before Writing Game Logic or Changing the Architecture, Ask Three Questions

```text
1. Does this code mutate game state, or only read state / produce presentation?
2. If it mutates state, which logic frame line does it belong to?
   If it belongs to none, who gave it state-mutation authority?
3. Should it stop when paused?
   If it should "partially continue," is it continuing in the presentation layer,
   or bypassing system pause to mutate state?
```

If you cannot answer clearly, do not write it yet.

The third question is especially easy to get wrong:

```text
"the UI should keep moving while paused"
```

is not the same as:

```text
"the UI may mutate game state while paused"
```

The former is presentation consuming state. The latter opens a bypass outside the frame lines.

Watch especially for these bypasses:

```text
mutating game state in __gc
giving a single object ignore_pause to bypass the pause frame line
mutating logic state from a render callback
mutating Lua state directly from a background thread
```

---

## Further Reading

- [Timing and Frame Lines](doc/30-时序与时间/时序与帧线.md)
- [Pause and Resume](doc/30-时序与时间/暂停与恢复.md)
- [Errors and Diagnostics](doc/70-错误与诊断/错误的四档.md)
- [README](README.md)

# 简体中文

> 这是**架构阅读警告**，不是安全公告。
>
> 本仓库不是包括但不限于 Unity、Unreal、Godot、Love2D 或 LuaSTG 的近似实现。  
> 在编写游戏逻辑、修改架构、补 API、写示例之前，先确认你没有把其他引擎的“近似概念”直接代入本项目。

LuaWAG 的很多设计不是“换个名字的常见做法”，而是针对**确定性、暂停、回放和脚本边界**重新切分的。

如果你带着旧引擎的词汇表进来，最容易出现的不是编译错误，而是：

```text
结构理解错了，代码还能跑；
跑起来之后，到处漏风。
```

---

## 先换这五把尺子

### 1. FrameLine（帧线）不是 Timeline

`FrameLine` 的轴是**帧序**，不是墙钟时间。  
它也不是 Unity Timeline 那种演出编排工具。

本项目使用“帧线”而不是“时间线”，是因为“时间线”很容易被理解为：

```text
在时间轴上排动画、音频、镜头
```

而这里的帧线是：

```text
划分“这一帧谁有权改变游戏状态”的执行协议
```

三条逻辑帧线是：

```text
ExtFrameFunc
MainFrameFunc
PauseFrameFunc
```

它们不是三个功能回调，而是三种**状态变更权**的划分。

### 2. 平台层不是世界层

平台层负责：

```text
窗口、输入、文件、音频、GPU、帧循环、暂停控制、脚本状态
```

世界层负责：

```text
空间、维度、时间、物理、光照、拓扑、玩法规则
```

“引擎不预设”约束的是**世界层**，不是平台层。  
平台层当然要有执行协议；否则连“一帧是什么”都不存在。

不要把这两句话混在一起：

```text
“引擎不预设世界规则”
≠
“引擎连执行协议也不该有”
```

### 3. 逻辑生产者不是表现消费者

`render` / `audio` 消费状态，不持有状态变更权。  
它们不是“第四条逻辑帧线”。

帧线划分的是：

```text
谁有权改游戏状态
```

不是：

```text
每帧跑一次的代码都叫帧线
```

所以不要问：

```text
“为什么不再分出 render / audio 两条帧线？”
```

它们属于表现阶段，和逻辑帧线不是一个分类维度。

### 4. 暂停不是游戏布尔值

暂停不是某个对象上的 `ignore_pause` 开关，  
也不是“游戏代码里自己维护的 `paused` 变量”。

暂停是控制原语，由引擎持有。  
系统暂停发生时，逻辑帧线整体挂起；  
脚本侧的局部暂停、慢动作、时停是主线内部的玩法，  
它们不会穿透系统暂停。

这也是为什么脚本侧可以放心玩各种暂停花活：

```text
主帧线挂起
→ 主线里的所有局部暂停计时自然冻结
→ 恢复时自然继续
```

不需要每个子系统都去检查一个全局 `paused` 标志。

### 5. 计算负载不是协议语义

重 AI 推理、重物理仿真仍然在引擎上跑，  
也仍然需要暂停。

它们改变的是：

```text
帧预算够不够
```

不是：

```text
帧线数量够不够
```

单帧跑不完的 AI 要做增量计算、协程切片或后台任务，  
这是性能/任务协议的问题，不是“三条线不够用”。

---

## 三个最容易撞墙的地方

### 撞墙一：把帧线当 Timeline

错误理解：

```text
“帧线就是 Timeline 的另一种叫法。”
```

正确理解：

```text
Timeline 编排演出资源；
帧线划分状态变更权。
```

前者是内容工具，后者是执行协议。

### 撞墙二：把 render / audio 当第四条线

错误理解：

```text
“既然有 Ext / Main / Pause，为什么没有 Render / Audio 线？”
```

正确理解：

```text
render / audio 是状态的消费者，不是生产者。
它们不参与“谁有权改状态”的划分。
```

逻辑帧线和表现阶段是两个不同维度。

### 撞墙三：把暂停当 `timeScale` 或全局布尔量

错误理解：

```text
“暂停就是游戏自己写一个 paused 标志，或者把时间缩放成 0。”
```

正确理解：

```text
暂停是引擎持有的控制原语；
脚本侧的暂停花活是主线内部的状态，
不是绕过系统暂停的旁路。
```

如果每个子系统都要自己检查暂停标志，  
最终一定会漏掉某个计时器、某个动画、某个第三方库。

---

## 另外两个常见误解

### “调高帧率会让画面更流畅”

在默认帧循环里：

```text
dtg = 1/60
```

逻辑每被调用一次，游戏时间就推进 `1/60` 秒。

所以提高逻辑帧率不是单纯“更流畅”，而是：

```text
每秒推进更多游戏时间
→ 游戏变快
```

渲染帧率只改变画面更新频率；  
逻辑帧率和渲染帧率不是同一个量。

### “`__gc` 可以用来做游戏生命周期”

`__gc` 不走三条逻辑帧线，  
也不保证 finalizer 顺序。

不要用它做：

```text
实体销毁
事件退订
定时器取消
资源释放
存档
掉落 / 音效 / UI 状态变化
```

它最多适合没有可观察游戏副作用的纯 Lua 内存收尾。

---

## 写游戏逻辑或改架构前，先问这三个问题

```text
1. 这段代码是在改变游戏状态，还是只读状态 / 只产出表现？
2. 如果要改变状态，它属于哪条逻辑帧线？
   如果不在任何帧线上，谁给了它“状态变更权”？
3. 暂停时它应该停吗？
   如果“要局部继续”，它是在表现层继续，
   还是绕过系统暂停去改游戏状态？
```

如果答不清，先不要写。

第三个问题尤其容易答错：

```text
“暂停时 UI 还要动”
```

不等于：

```text
“UI 可以在暂停时改游戏状态”
```

前者是表现层继续消费状态，后者是在帧线之外开旁路。

尤其警惕这些旁路：

```text
用 __gc 改游戏状态
给单个对象加 ignore_pause 绕过暂停帧线
用渲染回调改逻辑状态
用后台线程直接改 Lua 状态
```

---

## 继续阅读

- [时序与帧线](doc/30-时序与时间/时序与帧线.md)
- [暂停与恢复](doc/30-时序与时间/暂停与恢复.md)
- [错误与诊断](doc/70-错误与诊断/错误的四档.md)
- [README](README.md)


