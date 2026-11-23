// =============================================================================
//  audio/audio.cpp —— 音频子系统的实现
//
//  【本文件里唯一"跨线程"的地方】
//    · stream_callback —— 跑在 **SDL 的音频线程**
//    · apply_ / render_ / finish_ —— 只被 callback 调用，只碰 POD
//    其余全部在主线程。两条 ring 是唯一的通信手段。
// =============================================================================

#include "audio/audio.hpp"
#include "audio/loop_logic.hpp"
#include "audio/soft_clip.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "error/error.hpp"
#include "log/log.hpp"
#include "platform/utf8_path.hpp"

#if defined(WAG_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif

// -----------------------------------------------------------------------------
//  OGG 解码：vendored stb_vorbis（单文件、公共域，v1.24）
//
//  【为什么只包含"声明"】实现由 third_party/stb_vorbis.c 单独编译成一个 TU；
//    这里用 STB_VORBIS_HEADER_ONLY 只取声明与 stb_vorbis_info 结构，
//    避免两处都生成实现。
// -----------------------------------------------------------------------------
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include <cctype>

namespace wag::audio {

namespace {

constexpr int kSampleRate = 44100;
constexpr int kChannels = 2;
constexpr int kChunkFrames = 1024;                 ///< 音频线程一次最多渲染这么多帧
constexpr std::uint64_t kHandleIndexMask = 0x1FFFFFull;

inline float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/// **等功率平衡**（字段名仍叫 pan）。
///
/// ```
/// p ∈ [-1, 1]：-1 = 纯左（R 置 0）／ 0 = 居中（左右各 0.707）／ +1 = 纯右（L 置 0）
/// ```
///
/// 【它是平衡，不是"声像"】L、R 各乘一个系数 ⇒ **不把立体声折叠成单声道**
///   （折叠会压掉相位/混响信息，听感变窄）。
/// 【为什么中间是 0.707 而不是 1.0】恒功率（L²+R²=1）；顺带买到 −3 dB 抗削顶
///   余量 —— 满刻度素材若按 1.0 播会削顶 2.2% 的样本，听感明显更平。
inline void pan_gains(float pan, float& l, float& r) {
    const float theta = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.25f * 3.14159265358979f;
    l = std::cos(theta);
    r = std::sin(theta);
}

} // namespace

// -----------------------------------------------------------------------------
//  无锁环形队列（SPSC）
// -----------------------------------------------------------------------------
template <typename T, int N>
bool system::ring<T, N>::push(const T& v) {
    const std::uint32_t head = head_.load(std::memory_order_relaxed);
    const std::uint32_t next = (head + 1) % static_cast<std::uint32_t>(N);
    if (next == tail_.load(std::memory_order_acquire)) {
        return false;   // 满
    }
    buf_[head] = v;
    head_.store(next, std::memory_order_release);
    return true;
}

template <typename T, int N>
bool system::ring<T, N>::pop(T& out) {
    const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) {
        return false;   // 空
    }
    out = buf_[tail];
    tail_.store((tail + 1) % static_cast<std::uint32_t>(N), std::memory_order_release);
    return true;
}

template <typename T, int N>
bool system::ring<T, N>::empty() const noexcept {
    return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
}

// -----------------------------------------------------------------------------
//  构造 / 设备
// -----------------------------------------------------------------------------
system::system() = default;

system::~system() {
    shutdown();
}

void system::init() {
#if defined(WAG_HAVE_SDL3)
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        device_failed_ = true;
        log::warn("audio", log::format("音频子系统初始化失败（进入静音模式）", SDL_GetError()));
        return;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = kChannels;
    spec.freq = kSampleRate;

    // 【pull 模型】混音跑在 SDL 的音频线程里；我们的代码在回调里不碰 Lua、不碰引擎对象。
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &system::stream_callback, this);
    if (stream == nullptr) {
        device_failed_ = true;
        log::warn("audio", log::format("打开播放设备失败（进入静音模式）", SDL_GetError()));
        return;
    }
    stream_ = stream;
    muted_.store(false, std::memory_order_relaxed);
    SDL_ResumeAudioStreamDevice(stream);
    log::record("audio", log::format("音频设备已打开",
                            "44100 Hz / 2 ch / s16；设备格式不同由 SDL 转换"));
#else
    device_failed_ = true;
    log::record("audio", "无 SDL 构建：音频静音");
#endif
}

void system::shutdown() {
#if defined(WAG_HAVE_SDL3)
    if (stream_ != nullptr) {
        auto* stream = static_cast<SDL_AudioStream*>(stream_);
        stream_ = nullptr;
        muted_.store(true, std::memory_order_relaxed);
        // 【先暂停再销毁】SDL 在这里停掉音频线程；之后回调不会再进来，
        //   所以 voice 表与 PCM 都可以安全丢弃。
        SDL_PauseAudioStreamDevice(stream);
        SDL_DestroyAudioStream(stream);
        log::record("audio", "音频设备已关闭");
    }
#endif
}

// -----------------------------------------------------------------------------
//  音频线程：回调
// -----------------------------------------------------------------------------
void system::stream_callback(void* userdata, SDL_AudioStream* stream,
                             int additional_amount, int total_amount) {
    (void)total_amount;
    auto* self = static_cast<system*>(userdata);
    if (self == nullptr || stream == nullptr || additional_amount <= 0) {
        return;
    }

    const int frame_bytes = kChannels * static_cast<int>(sizeof(std::int16_t));
    int bytes = additional_amount - (additional_amount % frame_bytes);
    if (bytes <= 0) {
        return;
    }

    SDL_AudioStream* out = stream;
    const int frames = bytes / frame_bytes;

    std::int16_t buf[kChunkFrames * kChannels];
    int done = 0;
    while (done < frames) {
        const int n = std::min(kChunkFrames, frames - done);
        self->render_(buf, n);
        SDL_PutAudioStreamData(out, buf,
                               n * static_cast<int>(sizeof(std::int16_t)) * kChannels);
        done += n;
    }
}

// -----------------------------------------------------------------------------
//  音频线程：应用命令
// -----------------------------------------------------------------------------
void system::apply_(const command& c) {
    switch (c.kind) {
    case cmd_kind::play: {
        voice_slot* target = nullptr;
        if (c.music) {
            target = &voices_[kSeVoices];       // BGM 专用槽：直接替换
        } else {
            // 找空槽；没有就抢**最早开始**的那条
            voice_slot* oldest = nullptr;
            for (int i = 0; i < kSeVoices; ++i) {
                voice_slot& v = voices_[i];
                if (!v.active) { target = &v; break; }
                if (oldest == nullptr || v.started < oldest->started) { oldest = &v; }
            }
            if (target == nullptr) { target = oldest; }
        }
        if (target == nullptr) { return; }
        if (target->active) { finish_(*target); }
        target->active = true;
        target->id = c.id;
        target->data = c.data;
        target->pos = c.start_frame;   // 起点（不是中途跳）
        target->vol = clamp01(c.a);
        target->pan = c.b;
        target->loop = c.loop;
        target->music = c.music;
        target->loop_start = c.loop_start;
        target->loop_end = c.loop_end;
        target->paused = false;
        target->started = ++seq_;
        if (c.music) {
            music_voice_.store(c.id, std::memory_order_relaxed);
            music_handle_.store(c.handle, std::memory_order_relaxed);
        }
        if (c.data != nullptr) {
            c.data->refs.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    }
    case cmd_kind::stop: {
        for (auto& v : voices_) {
            if (v.active && v.id == c.id) { finish_(v); break; }
        }
        break;
    }
    case cmd_kind::set_volume: {
        for (auto& v : voices_) {
            if (v.active && v.id == c.id) { v.vol = clamp01(c.a); break; }
        }
        break;
    }
    case cmd_kind::set_pan: {
        for (auto& v : voices_) {
            if (v.active && v.id == c.id) { v.pan = c.b; break; }
        }
        break;
    }
    case cmd_kind::stop_all: {
        for (auto& v : voices_) {
            if (v.active) { finish_(v); }
        }
        music_voice_.store(0, std::memory_order_relaxed);
        music_handle_.store(0, std::memory_order_relaxed);
        break;
    }
    case cmd_kind::stop_all_se: {
        // 热重载：只停 music=false 的 voice；BGM 专用槽保持。
        for (int i = 0; i < kSeVoices; ++i) {
            if (voices_[i].active) { finish_(voices_[i]); }
        }
        break;
    }
    case cmd_kind::pause: {
        for (auto& v : voices_) {
            if (v.active && v.id == c.id) { v.paused = true; break; }
        }
        break;
    }
    case cmd_kind::resume: {
        for (auto& v : voices_) {
            if (v.active && v.id == c.id) { v.paused = false; break; }
        }
        break;
    }
    case cmd_kind::pause_all: {
        for (auto& v : voices_) {
            if (v.active) { v.paused = true; }
        }
        break;
    }
    case cmd_kind::resume_all: {
        for (auto& v : voices_) {
            if (v.active) { v.paused = false; }
        }
        break;
    }
    }
}

void system::finish_(voice_slot& v) {
    if (v.music &&
        music_voice_.load(std::memory_order_relaxed) == v.id) {
        music_voice_.store(0, std::memory_order_relaxed);
        music_handle_.store(0, std::memory_order_relaxed);
    }
    if (v.data != nullptr) {
        v.data->refs.fetch_sub(1, std::memory_order_relaxed);
        v.data = nullptr;
    }
    event e;
    e.id = v.id;
    recv_.push(e);        // 回执（音频线程是这条队列的生产者）
    v.active = false;
    v.id = 0;
    v.pos = 0;
}

// -----------------------------------------------------------------------------
//  音频线程：混音
// -----------------------------------------------------------------------------
void system::render_(std::int16_t* out, int frames) {
    // ① 先把所有待应用命令取走（每条只应用一次）
    command c;
    while (send_.pop(c)) {
        apply_(c);
    }

    const float gmaster = master_.load(std::memory_order_relaxed);
    const float gmusic = music_.load(std::memory_order_relaxed);
    const float gse = se_.load(std::memory_order_relaxed);

    float mix[kChunkFrames * kChannels];
    const int n = frames * kChannels;
    for (int i = 0; i < n; ++i) {
        mix[i] = 0.0f;
    }

    for (auto& v : voices_) {
        if (!v.active || v.data == nullptr || v.paused) {
            continue;
        }
        const float group = v.music ? gmusic : gse;
        const float gain = gmaster * group * v.vol;
        if (gain <= 0.0f) {
            continue;
        }

        float gl = 0.0f;
        float gr = 0.0f;
        pan_gains(v.pan, gl, gr);
        gl *= gain;
        gr *= gain;

        const pcm& p = *v.data;
        int produced = 0;
        while (produced < frames) {
            // 【唯一的循环判定】见 audio/loop_logic.hpp：
            //   首次从 0 播到 loop_end，之后在 [loop_start, loop_end) 循环。
            if (!wrap_position(v.pos, v.loop_start, v.loop_end, v.loop)) {
                finish_(v);
                break;
            }
            const auto* src = &p.samples[v.pos * kChannels];
            const float sl = (static_cast<float>(src[0]) / 32768.0f) * gl;
            const float sr = (static_cast<float>(src[1]) / 32768.0f) * gr;
            mix[produced * kChannels + 0] += sl;
            mix[produced * kChannels + 1] += sr;

            // 【频谱/波形的取样点】只取**音乐槽**，而且是"混音前"的那一份：
            //   单独乘 voice 自带的音量与平衡，**不含** master / 组音量 ——
            //   否则把音乐音量拉小，画面就跟着"死"了（那不是曲子的问题）。
            if (v.music) {
                const float g = v.vol;
                float pl = 0.0f;
                float pr = 0.0f;
                pan_gains(v.pan, pl, pr);
                const float mono = ((static_cast<float>(src[0]) / 32768.0f) * pl * g +
                                    (static_cast<float>(src[1]) / 32768.0f) * pr * g) * 0.5f;
                spectrum_.push(mono);
            }

            ++produced;
            ++v.pos;
        }
    }
    // 音乐没在响时喂静音：让柱子和波形自然衰减到 0（而不是"冻住"）。
    {
        bool bgm_alive = false;
        for (auto& v : voices_) {
            if (v.active && v.music && !v.paused && v.data != nullptr) { bgm_alive = true; break; }
        }
        if (!bgm_alive) {
            for (int i = 0; i < frames; ++i) {
                spectrum_.push(0.0f);
            }
        }
    }
    // ② **软限幅**（audio/soft_clip.hpp）：拐点 0.8 以下恒等，之上平滑压到 1.0。
    //    为什么不用硬 clamp：削顶会让声音更平、更难听。
    for (int i = 0; i < n; ++i) {
        const float x = soft_clip(mix[i]);
        out[i] = static_cast<std::int16_t>(x * 32767.0f);
    }
}

// -----------------------------------------------------------------------------
//  主线程：解码（两条路径只负责"得到 PCM"，之后的登记完全共用）
// -----------------------------------------------------------------------------
bool system::decode_wav(const char* path, std::vector<std::int16_t>& samples,
                        int& channels, int& rate, std::string& error) {
#if defined(WAG_HAVE_SDL3)
    SDL_AudioSpec spec{};
    std::uint8_t* data = nullptr;
    std::uint32_t len = 0;
    if (!SDL_LoadWAV(path, &spec, &data, &len)) {
        error = std::string("WAV 解码失败：") + SDL_GetError();
        return false;
    }

    // 【固定格式】只接受 44100 / 2 声道 / s16；其余 L3（不静默改格式）
    if (spec.format != SDL_AUDIO_S16 || spec.channels != kChannels || spec.freq != kSampleRate) {
        char got[128];
        std::snprintf(got, sizeof(got), "format=0x%X channels=%d rate=%d",
                      static_cast<unsigned>(spec.format),
                      static_cast<int>(spec.channels),
                      static_cast<int>(spec.freq));
        SDL_free(data);
        error = std::string("WAV 格式不符（") + got + "）";
        return false;
    }

    samples.resize(len / sizeof(std::int16_t));
    std::memcpy(samples.data(), data, len);
    SDL_free(data);
    channels = kChannels;
    rate = kSampleRate;
    return true;
#else
    (void)path;
    (void)samples;
    (void)channels;
    (void)rate;
    error = "无 SDL 构建：不能解码 WAV";
    return false;
#endif
}

bool system::decode_ogg(const char* path, std::vector<std::int16_t>& samples,
                        int& channels, int& rate, std::string& error) {
    int open_error = 0;
    std::FILE* const file = wag::paths::fopen_utf8(path, "rb");
    if (file == nullptr) {
        error = "OGG 打不开（文件不可读）";
        return false;
    }
    stb_vorbis* v = stb_vorbis_open_file(file, 1, &open_error, nullptr);
    if (v == nullptr) {
        // stb_vorbis_open_file 失败时已按 close_on_free 关闭 FILE。
        error = std::string("OGG 打不开（stb_vorbis 错误码 ") +
                std::to_string(open_error) + "）";
        return false;
    }

    const stb_vorbis_info info = stb_vorbis_get_info(v);

    // 与 WAV 同一条格式约定：44100 / 2 声道；其余 L3（不重采样）
    if (info.channels != kChannels || info.sample_rate != kSampleRate) {
        char got[128];
        std::snprintf(got, sizeof(got), "channels=%d rate=%d",
                      static_cast<int>(info.channels),
                      static_cast<int>(info.sample_rate));
        stb_vorbis_close(v);
        error = std::string("OGG 格式不符（") + got + "）";
        return false;
    }

    const unsigned int frames = stb_vorbis_stream_length_in_samples(v);
    if (frames == 0) {
        stb_vorbis_close(v);
        error = "OGG 长度未知或为空";
        return false;
    }

    samples.resize(static_cast<std::size_t>(frames) * kChannels);
    const int got = stb_vorbis_get_samples_short_interleaved(
        v, kChannels, samples.data(),
        static_cast<int>(samples.size()));
    stb_vorbis_close(v);

    if (got <= 0) {
        samples.clear();
        error = "OGG 解码得到 0 帧";
        return false;
    }

    // 以实际解出的帧数为准（头部声明的长度偶尔不准）
    samples.resize(static_cast<std::size_t>(got) * kChannels);
    channels = kChannels;
    rate = kSampleRate;
    return true;
}

// -----------------------------------------------------------------------------
//  主线程：加载 + 登记（两种格式共用这一条）
// -----------------------------------------------------------------------------
std::uint64_t system::load(const char* name, const char* path) {
    if (name == nullptr || *name == '\0' || path == nullptr || *path == '\0') {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "wag.audio.load 的 name / path";
        e.got = (name == nullptr || *name == '\0') ? "name 为空" : "path 为空";
        e.want = "非空音频名 + 非空路径";
        fail_boundary(e);
    }

    // 按扩展名分派（大小写不敏感）
    std::string path_text = path;
    std::string lower = path_text;
    for (auto& ch : lower) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    const bool is_ogg = lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".ogg") == 0;
    const bool is_wav = lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".wav") == 0;

    std::vector<std::int16_t> decoded;
    int channels = 0;
    int rate = 0;
    std::string error;

    bool ok = false;
    if (is_ogg) {
        ok = decode_ogg(path_text.c_str(), decoded, channels, rate, error);
    } else if (is_wav) {
        ok = decode_wav(path_text.c_str(), decoded, channels, rate, error);
    } else {
        error = "只认识 .wav / .ogg（第一版不猜格式）";
    }

    if (!ok) {
        error_value e;
        // 【级别】文件打不开 / 解不出来 ⇒ 资源失败（与图片同族）；
        //   格式不符 ⇒ 边界校验。两者都是 L3，但 want 的措辞不同。
        e.cls = (error.rfind("格式不符", 0) == 0 || error.find("格式不符") != std::string::npos)
                    ? error_class::boundary_validation
                    : error_class::resource_failure;
        e.where = "wag.audio.load";
        e.got = std::string(name) + "（" + path_text + "）：" + error;
        e.want = "44100 Hz / 2 声道 / 16bit 的 .wav 或 .ogg";
        fail_boundary(e);
    }

    // 槽位分配。
    //
    // 【为什么不能无脑复用】**有人还在播**这一份时，就地覆盖采样 = 让音频线程
    //   读到半新半旧的内存（比崩溃更难查）。⇒ 复用的前提是 `refs == 0`；
    //   否则另开一个槽，让旧样本自然播完、引用归零后再回到空闲表。
    std::uint32_t slot = 0;
    bool reused = false;
    const auto it = pcm_by_name_.find(name);
    if (it != pcm_by_name_.end()) {
        slot = it->second;
        reused = true;
    } else {
        // 只在"没人播"的空闲槽里挑；挑不到就新建
        while (!pcm_free_.empty()) {
            const std::uint32_t candidate = pcm_free_.back();
            pcm_free_.pop_back();
            if (pcm_slots_[candidate]->refs.load(std::memory_order_relaxed) == 0) {
                slot = candidate;
                reused = true;
                break;
            }
        }
        if (!reused) {
            slot = static_cast<std::uint32_t>(pcm_slots_.size());
            pcm_slots_.push_back(std::make_unique<pcm>());
        }
    }
    // 同名覆盖但旧样本仍在播 ⇒ 换一个新槽（旧槽留在空闲表里等引用归零）
    if (reused && pcm_slots_[slot]->refs.load(std::memory_order_relaxed) > 0) {
        slot = static_cast<std::uint32_t>(pcm_slots_.size());
        pcm_slots_.push_back(std::make_unique<pcm>());
        reused = false;
    }

    pcm& p = *pcm_slots_[slot];
    p.samples = std::move(decoded);
    p.frames = static_cast<std::uint32_t>(p.samples.size() / kChannels);
    p.channels = kChannels;
    p.rate = kSampleRate;
    p.name = name;
    if (!reused || p.gen == 0) {
        p.gen = 1;
    } else {
        p.gen += 1;
    }
    p.refs.store(0, std::memory_order_relaxed);
    pcm_by_name_[name] = slot;

    log::record("audio", log::format("音频已加载",
                            p.name + "（" + std::to_string(p.frames) + " 帧，" +
                                (is_ogg ? "ogg" : "wav") + "）"));
    return (static_cast<std::uint64_t>(p.gen) << 21) | slot;
}

void system::unload(const char* name) {
    if (name == nullptr) {
        return;
    }
    const auto it = pcm_by_name_.find(name);
    if (it == pcm_by_name_.end()) {
        return;
    }
    const std::uint32_t slot = it->second;
    pcm_by_name_.erase(it);

    pcm& p = *pcm_slots_[slot];
    p.gen += 1;            // 旧句柄立刻失效（与图片同族的"代数 +1"）
    if (p.refs.load(std::memory_order_relaxed) == 0) {
        p.samples.clear();
        p.samples.shrink_to_fit();
        p.frames = 0;
    }
    // 有人在播 ⇒ 采样先留着；pump 里在 refs 归零后回收（见 retired_ 的处理）
    pcm_free_.push_back(slot);
}

bool system::lookup(const char* name, std::uint64_t& out) const {
    if (name == nullptr) {
        return false;
    }
    const auto it = pcm_by_name_.find(name);
    if (it == pcm_by_name_.end()) {
        return false;
    }
    const pcm& p = *pcm_slots_[it->second];
    out = (static_cast<std::uint64_t>(p.gen) << 21) | it->second;
    return true;
}

double system::duration(std::uint64_t handle) const {
    const std::uint32_t slot = static_cast<std::uint32_t>(handle & kHandleIndexMask);
    const std::uint32_t gen = static_cast<std::uint32_t>(handle >> 21);
    if (slot >= pcm_slots_.size()) {
        return -1.0;
    }
    const pcm& p = *pcm_slots_[slot];
    if (p.gen == 0 || p.gen != gen || p.frames == 0) {
        return -1.0;
    }
    return static_cast<double>(p.frames) / static_cast<double>(p.rate);
}

// -----------------------------------------------------------------------------
//  主线程：播放（只排队）
// -----------------------------------------------------------------------------
std::uint32_t system::play(std::uint64_t handle, float volume, float pan, bool loop,
                           bool music, std::uint32_t loop_start, std::uint32_t loop_end,
                           std::uint32_t start_frame) {
    if (muted_.load(std::memory_order_relaxed)) {
        return 0;
    }
    const std::uint32_t slot = static_cast<std::uint32_t>(handle & kHandleIndexMask);
    const std::uint32_t gen = static_cast<std::uint32_t>(handle >> 21);
    if (slot >= pcm_slots_.size()) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "wag.audio.play 的句柄";
        e.got = std::to_string(handle);
        e.want = "一个仍有效的音频句柄（同名覆盖 / 卸载后旧句柄失效）";
        fail_boundary(e);
    }
    pcm& p = *pcm_slots_[slot];
    if (p.gen == 0 || p.gen != gen || p.frames == 0) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "wag.audio.play 的句柄";
        e.got = std::to_string(handle);
        e.want = "一个仍有效的音频句柄（同名覆盖 / 卸载后旧句柄失效）";
        fail_boundary(e);
    }

    const std::uint32_t id = next_voice_id_.fetch_add(1, std::memory_order_relaxed);

    // ---- 循环区间 ----
    //   单位是**采样帧**；loop_end == 0 表示"到结尾"（整段循环的老行为）。
    //   【什么时候校验】只在 loop = true 时才有意义：不循环时 loop_end 恒等于总帧数。
    std::uint32_t ls = 0;
    std::uint32_t le = p.frames;
    if (loop) {
        ls = loop_start;
        le = (loop_end == 0) ? p.frames : loop_end;
        if (ls >= le || ls >= p.frames || le > p.frames) {
            error_value e;
            e.cls = error_class::boundary_validation;
            e.where = "wag.audio.play 的循环区间";
            e.got = "loop_start=" + std::to_string(ls) + " loop_end=" + std::to_string(le) +
                    "（总帧数 " + std::to_string(p.frames) + "）";
            e.want = "0 <= loop_start < loop_end <= 总帧数（单位是采样帧）";
            fail_boundary(e);
        }
    }

    // ---- 起点（从哪儿开始，不是中途跳）----
    //   【校验】必须落在轨道内；循环时还必须早于 loop_end（否则一起播就回绕，没意义）。
    if (start_frame >= p.frames) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "wag.audio.play 的 start_frame";
        e.got = std::to_string(start_frame) + "（总帧数 " + std::to_string(p.frames) + "）";
        e.want = "0 <= start_frame < 总帧数（单位是采样帧）";
        fail_boundary(e);
    }
    if (loop && start_frame >= le) {
        error_value e;
        e.cls = error_class::boundary_validation;
        e.where = "wag.audio.play 的 start_frame";
        e.got = std::to_string(start_frame) + "（loop_end = " + std::to_string(le) + "）";
        e.want = "循环时 start_frame < loop_end（否则一开始就回绕）";
        fail_boundary(e);
    }

    command c;
    c.kind = cmd_kind::play;
    c.id = id;
    c.data = &p;
    c.handle = handle;
    c.a = clamp01(volume);
    c.b = std::clamp(pan, -1.0f, 1.0f);
    c.loop = loop;
    c.music = music;
    c.loop_start = ls;
    c.loop_end = le;
    c.start_frame = start_frame;

    if (static_cast<int>(pending_.size()) >= kMaxPending) {
        if (!warned_queue_full_) {
            warned_queue_full_ = true;
            log::warn("audio", log::format("一帧投递过多（超出上限的部分被丢弃）",
                                  "上限 " + std::to_string(kMaxPending) + " 条"));
        }
        return id;
    }
    pending_.push_back(c);
    active_[id] = 1;
    return id;
}

void system::stop(std::uint32_t voice) {
    if (muted_.load(std::memory_order_relaxed) || voice == 0) {
        return;
    }
    command c;
    c.kind = cmd_kind::stop;
    c.id = voice;
    pending_.push_back(c);
}

void system::set_voice_volume(std::uint32_t voice, float volume) {
    if (muted_.load(std::memory_order_relaxed) || voice == 0) {
        return;
    }
    command c;
    c.kind = cmd_kind::set_volume;
    c.id = voice;
    c.a = clamp01(volume);
    pending_.push_back(c);
}

void system::set_voice_pan(std::uint32_t voice, float pan) {
    if (muted_.load(std::memory_order_relaxed) || voice == 0) {
        return;
    }
    command c;
    c.kind = cmd_kind::set_pan;
    c.id = voice;
    c.b = std::clamp(pan, -1.0f, 1.0f);
    pending_.push_back(c);
}

void system::pause(std::uint32_t voice) {
    if (muted_.load(std::memory_order_relaxed) || voice == 0) {
        return;
    }
    command c;
    c.kind = cmd_kind::pause;
    c.id = voice;
    pending_.push_back(c);
    paused_.insert(voice);          // 主线程侧的"查询用"状态（回执驱动）
}

void system::resume(std::uint32_t voice) {
    if (muted_.load(std::memory_order_relaxed) || voice == 0) {
        return;
    }
    command c;
    c.kind = cmd_kind::resume;
    c.id = voice;
    pending_.push_back(c);
    paused_.erase(voice);
}

void system::pause_all() {
    if (muted_.load(std::memory_order_relaxed)) {
        return;
    }
    command c;
    c.kind = cmd_kind::pause_all;
    pending_.push_back(c);
    for (const auto& kv : active_) {
        paused_.insert(kv.first);
    }
}

void system::resume_all() {
    if (muted_.load(std::memory_order_relaxed)) {
        return;
    }
    command c;
    c.kind = cmd_kind::resume_all;
    pending_.push_back(c);
    paused_.clear();
}

bool system::is_paused(std::uint32_t voice) const {
    return paused_.find(voice) != paused_.end();
}

void system::stop_all() {
    if (muted_.load(std::memory_order_relaxed)) {
        return;
    }
    command c;
    c.kind = cmd_kind::stop_all;
    pending_.push_back(c);
}

void system::stop_all_se() {
    if (muted_.load(std::memory_order_relaxed)) {
        return;
    }
    command c;
    c.kind = cmd_kind::stop_all_se;
    pending_.push_back(c);
}

bool system::current_music(std::uint32_t& voice,
                           std::uint64_t& handle) const noexcept {
    const std::uint32_t v = music_voice_.load(std::memory_order_relaxed);
    const std::uint64_t h = music_handle_.load(std::memory_order_relaxed);
    if (v == 0 || h == 0) {
        return false;
    }
    voice = v;
    handle = h;
    return true;
}

bool system::is_active(std::uint32_t voice) const {
    return active_.find(voice) != active_.end();
}

void system::set_master_volume(float v) { master_.store(clamp01(v)); }
void system::set_music_volume(float v) { music_.store(clamp01(v)); }
void system::set_se_volume(float v) { se_.store(clamp01(v)); }
float system::master_volume() const { return master_.load(); }
float system::music_volume() const { return music_.load(); }
float system::se_volume() const { return se_.load(); }

// -----------------------------------------------------------------------------
//  帧结构调用
// -----------------------------------------------------------------------------
void system::commit() {
    if (pending_.empty()) {
        return;
    }
    int dropped = 0;
    for (const auto& c : pending_) {
        if (!send_.push(c)) {
            ++dropped;
        }
    }
    pending_.clear();
    if (dropped > 0 && !warned_queue_full_) {
        warned_queue_full_ = true;
        log::warn("audio", log::format("发送队列满（丢弃命令）",
                              std::to_string(dropped) + " 条"));
    }
}

void system::pump() {
    // 排空回执：voice 结束（含被抢）⇒ 从"活跃表 / 暂停表"里同时移除。
    event e;
    while (recv_.pop(e)) {
        active_.erase(e.id);
        paused_.erase(e.id);
    }
}

} // namespace wag::audio
