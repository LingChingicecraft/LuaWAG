// =============================================================================
//  audio_device_probe.cpp —— 一次性探针：问 SDL3"默认播放设备的真实格式"
//
//  为什么值得问：我们按 44100/2ch/s16 开流，设备若是 48000 或 5.1，
//  中间就有一次 **SDL 的重采样 / 声道映射**。这与"听起来更立体"直接相关。
//
//  编译：g++ -O2 -o %TEMP%\audio_device_probe.exe audio_device_probe.cpp -lSDL3
// =============================================================================

#include <cstdio>
#include <SDL3/SDL.h>

int main() {
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        std::printf("SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }

    SDL_AudioSpec spec{};
    int frames = 0;
    if (!SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &frames)) {
        std::printf("SDL_GetAudioDeviceFormat 失败：%s\n", SDL_GetError());
        return 1;
    }

    const char* fmt = "?";
    switch (spec.format) {
    case SDL_AUDIO_S16: fmt = "S16"; break;
    case SDL_AUDIO_F32: fmt = "F32"; break;
    case SDL_AUDIO_S32: fmt = "S32"; break;
    default: break;
    }

    std::printf("默认播放设备格式：format=%s channels=%d freq=%d buffer_frames=%d\n",
                fmt, (int)spec.channels, (int)spec.freq, frames);

    const char* name = SDL_GetCurrentAudioDriver();
    std::printf("音频驱动：%s\n", name ? name : "(未知)");

    // 列设备
    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
    std::printf("播放设备数：%d\n", count);
    if (ids) {
        for (int i = 0; i < count; ++i) {
            std::printf("  [%d] id=%u name=%s\n", i, (unsigned)ids[i],
                        SDL_GetAudioDeviceName(ids[i]));
        }
        SDL_free(ids);
    }

    SDL_Quit();
    return 0;
}
