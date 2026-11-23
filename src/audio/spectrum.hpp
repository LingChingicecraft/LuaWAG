// =============================================================================
//  audio/spectrum.hpp
//
//  频谱（FFT）与时域波形。
//
//  【它只做三件事】
//    ① 收集音乐槽的样本（单声道混合）
//    ② 每满 1024 帧算一次：Hann → FFT → 32 个对数频段；同时算 128 点峰值包络
//    ③ 用双缓冲 + 原子序号发布结果，主线程只读
//
//  【为什么必须是双缓冲】音频线程写、主线程读；同一块缓冲上读写会撕裂
//    （主线程可能拿到“一半新一半旧”的数组）。写后换序号 ⇒ 主线程永远读到完整的那一份。
//
//  【为什么 FFT 在音频线程算】1024 点 FFT 约几十微秒；放在这里不占主线程。
//    绝不大块分配：所有数组都是成员，构造时一次定型。
// =============================================================================

#pragma once

#include <atomic>
#include <cmath>
#include <utility>
#include <cstdint>

namespace wag::audio {

/// 一次快照的量。主线程拿到的是**完整的一份**（不是半个）。
struct spectrum_frame {
    static constexpr int kBands = 32;
    static constexpr int kWavePoints = 128;

    float bands[kBands]{};        ///< 0..1（已做 dB 归一化 + 一阶平滑）
    float wave[kWavePoints]{};    ///< 0..1（每点是该小段的峰值绝对值）
};

class spectrum_analyzer {
public:
    static constexpr int kFftSize = 1024;
    static constexpr int kWavePoints = spectrum_frame::kWavePoints;
    static constexpr int kBands = spectrum_frame::kBands;
    static constexpr int kFramesPerWavePoint = kFftSize / kWavePoints;   // 8

    spectrum_analyzer() { build_tables(); }

    /// **音频线程**：推入一个单声道样本（0 附近的浮点，未归一化也行）。
    void push(float s) {
        mono_[fill_] = s;
        ++fill_;
        if (fill_ >= kFftSize) {
            publish();
            fill_ = 0;
        }
    }

    /// **主线程**：拷贝最新一份快照。没算过就返回全 0。
    void read(spectrum_frame& out) const {
        const int idx = published_.load(std::memory_order_acquire);
        if (idx < 0) {
            out = spectrum_frame{};
            return;
        }
        out = buffers_[idx];
    }

private:
    // ---- 初始化：Hann 窗 + 频段边界 ----
    void build_tables() {
        for (int i = 0; i < kFftSize; ++i) {
            window_[i] = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) /
                                                static_cast<float>(kFftSize - 1));
        }
        // 对数频段：40 Hz – 16 kHz，32 段（含两端，共 33 条边界）
        constexpr float kLo = 40.0f;
        constexpr float kHi = 16000.0f;
        const float ratio = std::pow(kHi / kLo, 1.0f / static_cast<float>(kBands));
        for (int i = 0; i <= kBands; ++i) {
            const float f = kLo * std::pow(ratio, static_cast<float>(i));
            int bin = static_cast<int>(f * static_cast<float>(kFftSize) / 44100.0f);
            if (bin < 1) { bin = 1; }
            if (bin > kFftSize / 2) { bin = kFftSize / 2; }
            band_bin_[i] = bin;
        }
        // 保证每个频段至少有一个 bin（低音区边界会撞在一起）
        for (int i = 1; i <= kBands; ++i) {
            if (band_bin_[i] <= band_bin_[i - 1]) {
                band_bin_[i] = band_bin_[i - 1] + 1;
            }
        }
    }

    // ---- 1024 点基-2 FFT（原地，复数）----
    void fft(float* re, float* im) {
        // 位反转置换
        for (int i = 1, j = 0; i < kFftSize; ++i) {
            int bit = kFftSize >> 1;
            for (; j & bit; bit >>= 1) {
                j ^= bit;
            }
            j ^= bit;
            if (i < j) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (int len = 2; len <= kFftSize; len <<= 1) {
            const float ang = -2.0f * kPi / static_cast<float>(len);
            const float wr = std::cos(ang);
            const float wi = std::sin(ang);
            for (int i = 0; i < kFftSize; i += len) {
                float cr = 1.0f;
                float ci = 0.0f;
                for (int k = 0; k < len / 2; ++k) {
                    const int a = i + k;
                    const int b = a + len / 2;
                    const float xr = re[b] * cr - im[b] * ci;
                    const float xi = re[b] * ci + im[b] * cr;
                    re[b] = re[a] - xr;
                    im[b] = im[a] - xi;
                    re[a] += xr;
                    im[a] += xi;
                    const float nr = cr * wr - ci * wi;
                    ci = cr * wi + ci * wr;
                    cr = nr;
                }
            }
        }
    }

    // ---- 每满一窗：算 FFT + 波形，并发布 ----
    void publish() {
        // ① 波形：每 8 帧取一个峰值
        float wave[kWavePoints];
        for (int p = 0; p < kWavePoints; ++p) {
            float peak = 0.0f;
            for (int k = 0; k < kFramesPerWavePoint; ++k) {
                const float a = std::fabs(mono_[p * kFramesPerWavePoint + k]);
                if (a > peak) { peak = a; }
            }
            wave[p] = peak;
        }

        // ② FFT（Hann 窗）
        for (int i = 0; i < kFftSize; ++i) {
            re_[i] = mono_[i] * window_[i];
            im_[i] = 0.0f;
        }
        fft(re_, im_);

        // ③ 频段能量 → dB → 0..1 → 一阶平滑
        //   【为什么用 write_ 而不是 1 - published_】published_ 初始是 -1，
        //   1-(-1) = 2 ⇒ 越界写（第一版就是这么崩的：访问冲突 0xC0000005）。
        const int next = write_;
        spectrum_frame& f = buffers_[next];
        for (int b = 0; b < kBands; ++b) {
            float mag = 0.0f;
            const int lo = band_bin_[b];
            const int hi = band_bin_[b + 1];
            for (int i = lo; i < hi; ++i) {
                const float m = std::sqrt(re_[i] * re_[i] + im_[i] * im_[i]) /
                                (static_cast<float>(kFftSize) * 0.5f);
                if (m > mag) { mag = m; }
            }
            // dB → 0..1（−60..0 dB）
            const float db = 20.0f * std::log10(mag + 1e-6f);
            float v = (db + 60.0f) / 60.0f;
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);

            // 一阶平滑：攻击快、释放慢（看起来"跟得上"，但不会闪）
            const float prev = smoothed_[b];
            const float k = (v > prev) ? kAttack : kRelease;
            smoothed_[b] = prev + (v - prev) * k;
            f.bands[b] = smoothed_[b];
        }
        for (int i = 0; i < kWavePoints; ++i) {
            f.wave[i] = wave[i];
        }

        published_.store(next, std::memory_order_release);
        write_ = 1 - next;      // 下次写另一块（发布前那一块可以安全被读）
    }

    static constexpr float kPi = 3.14159265358979f;
    static constexpr float kAttack = 0.55f;
    static constexpr float kRelease = 0.18f;

    float window_[kFftSize]{};
    float mono_[kFftSize]{};
    float re_[kFftSize]{};
    float im_[kFftSize]{};
    int band_bin_[kBands + 1]{};
    float smoothed_[kBands]{};
    int fill_ = 0;

    spectrum_frame buffers_[2]{};
    std::atomic<int> published_{-1};
    int write_ = 0;
};

} // namespace wag::audio
