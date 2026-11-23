// =============================================================================
//  audio/audio.hpp
//
//  音频子系统：PCM 资源、voice、混音线程与主线程之间的命令队列。
//
//  【三条设计原则】
//    ① 资源与 voice 分离：PCM 资源（句柄）只管“一段解码后的采样”；
//       播放是另一个东西（voice）。同一音效可以同时响多次。
//    ② 命令只走队列：脚本在钩子里调 wag.audio.* ⇒ 只写主线程队列；
//       音频线程每回调读一次队列、改它自己的 voice 表；反向只有“结束/被抢”事件。
//    ③ 失败禁音，不终止：设备打不开 ⇒ 全局静音 + 一条 L2，引擎照常跑。
//
//  【线程模型】
//    · 混音跑在 SDL 的音频线程（`SDL_OpenAudioDeviceStream` 的 pull 回调），
//      不起我们自己的线程。
//    · 音频线程只碰 POD：voice 表、PCM 采样、两个无锁队列。
//    · 跨线程两条单向队列（SPSC）：发送（主→音频）、回执（音频→主）。
//      不共享可变的复杂结构，也不加锁。
// =============================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "audio/spectrum.hpp"

struct SDL_AudioStream;   // SDL3 的前置声明：本头文件不包含 SDL

namespace wag::audio {

/// 一段解码后的 PCM。**固定格式**：44100 Hz / 2 声道 / s16。
///
/// 【为什么固定】第一版不做重采样。素材不是这个格式 ⇒ 加载时 L3
///   （明确报错，不静默改格式）。设备格式不同由 SDL 自己转换。
struct pcm {
    std::vector<std::int16_t> samples;   ///< 交错：L,R,L,R…
    std::uint32_t frames = 0;            ///< 采样帧数
    std::uint32_t channels = 0;          ///< 恒为 2
    std::uint32_t rate = 0;              ///< 恒为 44100
    std::uint32_t gen = 0;               ///< 代数（同名覆盖 +1）
    std::string name;

    /// 【为什么需要引用计数】音频线程正在播一段采样时，主线程可能 `unload`。
    ///   立刻释放 ⇒ 音频线程读已释放内存。⇒ 音频线程接管时 +1、结束时 −1；
    ///   主线程只在 refs == 0 时真正释放（否则挂到 retired_，pump 时再判）。
    std::atomic<int> refs{0};
};

/// 音频系统的对外接口。**除特别标注外，全部只能在主线程调用。**
class system {
public:
    /// SE 声部数（1 BGM + 32 SE）
    static constexpr int kSeVoices = 32;
    /// 一帧最多投递多少条命令（队列满 ⇒ L2 + 丢弃超出）
    static constexpr int kMaxPending = 1024;

    system();
    ~system();

    system(const system&) = delete;
    system& operator=(const system&) = delete;

    /// 打开设备。失败 ⇒ 静音模式（**不终止**）。
    void init();

    /// 关闭设备（SDL 自己停线程）。**必须在引擎资源释放之前**。
    void shutdown();

    /// 是否可用（设备已打开）。静音模式 ⇒ false。
    [[nodiscard]] bool available() const noexcept { return !muted_.load(std::memory_order_relaxed); }

    // ---- 资源 ----

    /// 加载音频并登记。按扩展名分派：`.wav` ⇒ SDL_LoadWAV；`.ogg` ⇒ stb_vorbis。
    /// 失败 ⇒ `fail_boundary`（L3，与图片同族）。
    std::uint64_t load(const char* name, const char* path);

    /// 卸载（幂等）。名字立刻从查找表移除；采样在没人播时才真正释放。
    void unload(const char* name);

    [[nodiscard]] bool lookup(const char* name, std::uint64_t& out) const;

    /// 时长（秒）。失效句柄 ⇒ -1。
    [[nodiscard]] double duration(std::uint64_t handle) const;

    // ---- 播放（主线程：只排队）----

    /// 请求播放。返回 voice_id（**总是返回**；池满时抢最早的）。
    ///
    /// `pan` 的语义是**平衡（balance）**：∈ [-1, 1]，-1 = 纯左 / 0 = 居中（−3 dB）/ +1 = 纯右；
    /// 两声道各乘一个系数，**不折叠成立体声**。
    ///
    /// `loop_start` / `loop_end` 是**采样帧**（不是秒），语义：
    /// 首次从 `start_frame` 播到 `loop_end`，之后在 `[loop_start, loop_end)` 循环。
    ///   · `loop_end == 0` ⇒ 视为"到结尾"（整段循环，即老行为）
    ///   · 非法区间（`loop_start >= loop_end` 或超出总帧数）⇒ **L3**
    ///
    /// `start_frame` 是**起点**（采样帧），不是"中途跳转"（seek 不做）：
    /// 脚本侧"跳到某处继续播"= 再调一次 `play(start_frame = …)`，BGM 槽直接替换。
    std::uint32_t play(std::uint64_t handle, float volume, float pan, bool loop, bool music,
                       std::uint32_t loop_start = 0, std::uint32_t loop_end = 0,
                       std::uint32_t start_frame = 0);

    void stop(std::uint32_t voice);
    /// **暂停**：位置保留，可 `resume` 继续。
    void pause(std::uint32_t voice);
    /// **继续**：从暂停处接着播。
    void resume(std::uint32_t voice);
    void set_voice_volume(std::uint32_t voice, float volume);
    /// 播放中改**平衡**。立刻生效、**无斜坡**：别逐帧扫值。
    void set_voice_pan(std::uint32_t voice, float pan);
    /// **终止全部**（不可继续）。
    void stop_all();
    /// 热重载用：只终止 music=false 的 voice，BGM 专用槽不动。
    /// v1 不暴露 Lua；唯一调用者是重载协调器。
    void stop_all_se();
    /// 热重载用：查询当前 BGM 的 voice + handle。
    /// 没有 BGM 或 voice 已结束 ⇒ false；不改变播放状态。
    [[nodiscard]] bool current_music(std::uint32_t& voice,
                                     std::uint64_t& handle) const noexcept;
    /// **暂停全部**（位置保留）。与 `stop_all` 对称，**无布尔参数**。
    void pause_all();
    /// **继续全部**。
    void resume_all();
    /// 槽位是否仍被占用（**暂停也算**）。
    [[nodiscard]] bool is_active(std::uint32_t voice) const;
    /// 是否处于暂停。
    [[nodiscard]] bool is_paused(std::uint32_t voice) const;

    /// **频谱 / 波形的快照读取**（音频线程算、主线程读）。
    void read_spectrum(spectrum_frame& out) const { spectrum_.read(out); }

    // ---- 三组音量（线性 0..1；边界 clamp）----
    void set_master_volume(float v);
    void set_music_volume(float v);
    void set_se_volume(float v);
    [[nodiscard]] float master_volume() const;
    [[nodiscard]] float music_volume() const;
    [[nodiscard]] float se_volume() const;

    // ---- 帧结构调用 ----

    /// 阶段 1：把本帧累积的命令压入发送队列。
    void commit();

    /// 阶段 1：排空回执队列（voice 结束 / 被抢）⇒ 更新 is_active 的状态。
    void pump();

private:
    // ---- 命令（POD；跨线程只准传这种）----
    enum class cmd_kind : std::uint8_t {
        play = 0,       ///< id, pcm*, vol, pan, loop, music, loop_*, start_frame
        stop = 1,       ///< id
        set_volume = 2, ///< id, vol
        set_pan = 3,    ///< id, pan
        stop_all = 4,
        pause = 5,      ///< id（位置保留）
        resume = 6,     ///< id
        pause_all = 7,
        resume_all = 8,
        stop_all_se = 9,
    };

    struct command {
        cmd_kind kind = cmd_kind::stop;
        std::uint32_t id = 0;
        pcm* data = nullptr;
        std::uint64_t handle = 0;    ///< play：音频资源句柄（BGM 接管）
        float a = 1.0f;      ///< volume（play / set_volume）
        float b = 0.0f;      ///< 平衡（字段名 pan；play / set_pan）
        bool loop = false;
        bool music = false;
        std::uint32_t loop_start = 0;   ///< 采样帧（play）
        std::uint32_t loop_end = 0;     ///< 采样帧（play；已 clamp 到总帧数）
        std::uint32_t start_frame = 0;  ///< 采样帧（play 的**起点**，不是中途跳）
    };

    struct event {
        std::uint32_t id = 0;
    };

    /// 单生产者/单消费者环形队列（**无锁**）。
    template <typename T, int N>
    class ring {
    public:
        bool push(const T& v);
        bool pop(T& out);
        [[nodiscard]] bool empty() const noexcept;
    private:
        T buf_[N]{};
        std::atomic<std::uint32_t> head_{0};  ///< 写者推进
        std::atomic<std::uint32_t> tail_{0};  ///< 读者推进
    };

    struct voice_slot {
        bool active = false;
        std::uint32_t id = 0;
        pcm* data = nullptr;
        std::uint64_t pos = 0;       ///< 采样帧位置
        float vol = 1.0f;
        float pan = 0.0f;
        bool loop = false;
        bool music = false;
        bool paused = false;            ///< 暂停（位置保留，可 resume）
        std::uint32_t loop_start = 0;   ///< 采样帧
        std::uint32_t loop_end = 0;     ///< 采样帧（无循环时 = 总帧数）
        std::uint64_t started = 0;   ///< 抢占用：越小越早
    };

    /// SDL 的音频流回调（**只被 SDL 的音频线程调用**）。
    ///
    /// 【为什么这里不写 SDLCALL】本头文件**不包含 SDL**（它被 engine.hpp 间接包含，
    ///   不该把 SDL 头拖进来）。x64 上 `SDLCALL` 就是默认的 `__cdecl` ⇒ 签名兼容；
    ///   若将来做 32 位构建，这里要补 `__cdecl`。
    static void stream_callback(void* userdata, SDL_AudioStream* stream,
                                int additional_amount, int total_amount);

    /// 解码整段到 `samples`（交错 s16）。成功返回 true，并填 channels / rate。
    ///
    /// 【为什么拆成两条解码路径】WAV 与 OGG 只在"怎么得到 PCM"上不同；
    ///   之后的登记、句柄、代数、引用计数**完全共用**。
    static bool decode_wav(const char* path, std::vector<std::int16_t>& samples,
                           int& channels, int& rate, std::string& error);
    static bool decode_ogg(const char* path, std::vector<std::int16_t>& samples,
                           int& channels, int& rate, std::string& error);

    /// 音频线程：把一条命令应用到 voice 表。
    void apply_(const command& c);
    /// 音频线程：渲染 `bytes` 字节的 PCM 到 `out`。
    void render_(std::int16_t* out, int bytes);
    /// 音频线程：结束一个 voice（释放引用 + 回执）。
    void finish_(voice_slot& v);

    // ---- 主线程状态 ----
    std::atomic<bool> muted_{true};
    void* stream_ = nullptr;              ///< SDL_AudioStream*

    std::vector<std::unique_ptr<pcm>> pcm_slots_;
    std::vector<std::uint32_t> pcm_free_;
    std::unordered_map<std::string, std::uint32_t> pcm_by_name_;
    std::vector<std::unique_ptr<pcm>> retired_;
    mutable std::atomic<std::uint32_t> next_voice_id_{1};
    /// BGM 槽的当前 voice/handle（音频线程写、主线程读）。
    std::atomic<std::uint32_t> music_voice_{0};
    std::atomic<std::uint64_t> music_handle_{0};

    std::vector<command> pending_;        ///< 本帧待提交（主线程独占）
    std::unordered_map<std::uint32_t, std::uint8_t> active_;  ///< 回执驱动的活跃表
    std::unordered_set<std::uint32_t> paused_;                ///< 回执驱动的暂停表

    // ---- 跨线程 ----
    ring<command, kMaxPending> send_{};
    ring<event, kMaxPending> recv_{};

    // ---- 频谱 / 波形（音频线程写、主线程读）----
    spectrum_analyzer spectrum_;

    // ---- 音频线程状态 ----
    voice_slot voices_[kSeVoices + 1]{};  ///< 最后一个是 BGM 专用槽
    std::uint64_t seq_ = 0;
    std::atomic<float> master_{1.0f};
    std::atomic<float> music_{1.0f};
    std::atomic<float> se_{1.0f};

    // ---- 诊断（只记一次的标记）----
    bool warned_muted_ = false;
    bool warned_queue_full_ = false;
    bool device_failed_ = false;
};

} // namespace wag::audio
