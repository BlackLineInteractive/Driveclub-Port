#include "hle/audio_out/audio_out.h"

// libSceAudioOut on SDL3 audio streams.
//
// The game (through FMOD) pushes one fixed-size PCM buffer per call and
// relies on sceAudioOutOutput blocking until the hardware has room for the
// next one; that blocking is what paces its mixer. Guest threads only copy
// into a per-port ring; SDL pulls from it on its own audio thread, so no SDL
// call ever runs on a guest thread.

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <vector>

#include <SDL3/SDL.h>

#include "common/log.h"
#include "common/types.h"
#include "hle/hle.h"

namespace Hle::AudioOut {

namespace {

constexpr s32 kErrorInvalidPort = static_cast<s32>(0x80260004);
constexpr s32 kErrorPortFull = static_cast<s32>(0x8026000A);
constexpr s32 kErrorInvalidFormat = static_cast<s32>(0x80260005);
constexpr u32 kRingBuffers = 3; // buffers a port can hold before Output blocks
constexpr float kVolume0dB = 32768.0f;

struct Port {
    bool open = false;
    SDL_AudioStream* stream = nullptr;
    u32 buffer_bytes = 0;
    u32 channels = 0;
    // Ring of PCM bytes between the guest and the SDL callback.
    std::mutex lock;
    std::condition_variable cv;
    std::vector<u8> ring;
    u64 read = 0, write = 0; // byte counters
    float gain = 1.0f;       // applied on the SDL thread
    float applied_gain = 1.0f;
    bool is_float = false;
    // Diagnostics: peak level, logged every few seconds while there is sound.
    float peak = 0.0f;
    u64 meter_buffers = 0;
};

std::mutex g_lock; // open/close and handle lookup
std::array<Port, 16> g_ports; // handle = index + 1

// main() initializes SDL audio on the main thread before the game starts.
bool InitSdlAudio() {
    return SDL_WasInit(SDL_INIT_AUDIO) != 0;
}

Port* GetPort(s32 handle) {
    if (handle < 1 || handle > static_cast<s32>(g_ports.size()) || !g_ports[handle - 1].open) {
        return nullptr;
    }
    return &g_ports[handle - 1];
}

// SDL audio thread: feed what the guest has queued.
void SDLCALL Feed(void* user, SDL_AudioStream* stream, int additional, int /*total*/) {
    Port& p = *static_cast<Port*>(user);
    std::unique_lock lk(p.lock);
    if (p.gain != p.applied_gain) {
        SDL_SetAudioStreamGain(stream, p.gain);
        p.applied_gain = p.gain;
    }
    const u64 size = p.ring.size();
    u64 want = std::min<u64>(static_cast<u64>(additional), p.write - p.read);
    while (want > 0) {
        const u64 off = p.read % size;
        const u64 n = std::min(want, size - off);
        SDL_PutAudioStreamData(stream, p.ring.data() + off, static_cast<int>(n));
        p.read += n;
        want -= n;
    }
    lk.unlock();
    p.cv.notify_all();
}

void Meter(Port& p, const u8* src) {
    // Every 16th sample is plenty for a level meter.
    if (p.is_float) {
        const auto* f = reinterpret_cast<const float*>(src);
        for (u32 k = 0; k < p.buffer_bytes / 4; k += 16) {
            p.peak = std::max(p.peak, std::abs(f[k]));
        }
    } else {
        const auto* v = reinterpret_cast<const s16*>(src);
        for (u32 k = 0; k < p.buffer_bytes / 2; k += 16) {
            p.peak = std::max(p.peak, std::abs(v[k] / 32768.0f));
        }
    }
    if (++p.meter_buffers == 1875) { // ~10 s of 256-sample buffers at 48 kHz
        if (p.peak > 0.0f) {
            DC_INFO("audio: port %td peak %.3f over the last ~10 s", &p - g_ports.data() + 1, p.peak);
        }
        p.peak = 0.0f;
        p.meter_buffers = 0;
    }
}

// Copies one buffer in, blocking while the ring is full (that is the pacing).
void Queue(Port& p, const void* data) {
    std::unique_lock lk(p.lock);
    const u64 size = p.ring.size();
    p.cv.wait_for(lk, std::chrono::milliseconds(200), [&] { return p.write - p.read + p.buffer_bytes <= size; });
    if (p.write - p.read + p.buffer_bytes > size) {
        p.read = p.write - (size - p.buffer_bytes); // device stalled: drop the oldest audio
    }
    const auto* src = static_cast<const u8*>(data);
    Meter(p, src);
    u64 left = p.buffer_bytes;
    while (left > 0) {
        const u64 off = p.write % size;
        const u64 n = std::min(left, size - off);
        std::memcpy(p.ring.data() + off, src, n);
        src += n;
        p.write += n;
        left -= n;
    }
}

// Blocks until the port has room for one more buffer.
void WaitRoom(Port& p) {
    std::unique_lock lk(p.lock);
    p.cv.wait_for(lk, std::chrono::milliseconds(200),
                  [&] { return p.write - p.read + p.buffer_bytes <= p.ring.size(); });
}

s32 sceAudioOutInit() {
    return InitSdlAudio() ? 0 : -1;
}

s32 sceAudioOutOpen(s32 user_id, s32 port_type, s32 index, u32 len, u32 freq, u32 param) {
    if (!InitSdlAudio()) {
        return -1;
    }
    // Data format: S16/float x mono/stereo/8ch (6 and 7 are 8ch in standard order).
    const u32 fmt = param & 0xFF;
    static const u32 channels_of[8] = {1, 2, 8, 1, 2, 8, 8, 8};
    if (fmt > 7) {
        return kErrorInvalidFormat;
    }
    const bool is_float = fmt >= 3 && fmt != 6;
    const u32 channels = channels_of[fmt];

    std::scoped_lock lk(g_lock);
    for (size_t i = 0; i < g_ports.size(); ++i) {
        Port& p = g_ports[i];
        if (p.open) {
            continue;
        }
        p.channels = channels;
        p.is_float = is_float;
        p.buffer_bytes = len * channels * (is_float ? 4 : 2);
        p.ring.assign(u64(p.buffer_bytes) * kRingBuffers, 0);
        p.read = p.write = 0;
        SDL_AudioSpec spec{is_float ? SDL_AUDIO_F32 : SDL_AUDIO_S16, static_cast<int>(channels),
                           static_cast<int>(freq ? freq : 48000)};
        p.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Feed, &p);
        if (!p.stream) {
            DC_ERROR("audio: cannot open stream: %s", SDL_GetError());
            return kErrorPortFull;
        }
        p.open = true;
        SDL_ResumeAudioStreamDevice(p.stream);
        DC_INFO("audio: port %zu type %d: %d Hz, %u ch, %s, %u samples per buffer", i + 1, port_type, spec.freq,
                channels, is_float ? "float" : "s16", len);
        return static_cast<s32>(i + 1);
    }
    return kErrorPortFull;
}

s32 sceAudioOutClose(s32 handle) {
    std::scoped_lock lk(g_lock);
    Port* p = GetPort(handle);
    if (!p) {
        return kErrorInvalidPort;
    }
    SDL_DestroyAudioStream(p->stream); // stops the callback first
    p->stream = nullptr;
    p->open = false;
    return 0;
}

s32 sceAudioOutOutput(s32 handle, const void* ptr) {
    Port* p;
    {
        std::scoped_lock lk(g_lock);
        p = GetPort(handle);
    }
    if (!p) {
        return kErrorInvalidPort;
    }
    // A null buffer only waits for room.
    if (ptr) {
        Queue(*p, ptr);
    } else {
        WaitRoom(*p);
    }
    return 0;
}

struct OutputParam {
    s32 handle;
    const void* ptr;
};

s32 sceAudioOutOutputs(const OutputParam* params, u32 num) {
    if (!params) {
        return kErrorInvalidPort;
    }
    for (u32 i = 0; i < num; ++i) {
        const s32 r = sceAudioOutOutput(params[i].handle, params[i].ptr);
        if (r < 0) {
            return r;
        }
    }
    return 0;
}

s32 sceAudioOutSetVolume(s32 handle, s32 flag, const s32* vol) {
    std::scoped_lock lk(g_lock);
    Port* p = GetPort(handle);
    if (!p) {
        return kErrorInvalidPort;
    }
    // Per-channel volumes; SDL has one gain per stream, so take the first set one.
    if (vol) {
        for (u32 ch = 0; ch < 8; ++ch) {
            if (flag & (1 << ch)) {
                std::scoped_lock plk(p->lock);
                p->gain = static_cast<float>(vol[ch]) / kVolume0dB;
                break;
            }
        }
    }
    return 0;
}

struct PortState {
    u16 output;
    u8 channel;
    u8 reserved8;
    s16 volume;
    u16 reroute_counter;
    u64 flag;
    u64 reserved64[2];
};

s32 sceAudioOutGetPortState(s32 handle, PortState* state) {
    std::scoped_lock lk(g_lock);
    Port* p = GetPort(handle);
    if (!p || !state) {
        return kErrorInvalidPort;
    }
    *state = {};
    state->output = 1; // primary output
    state->channel = static_cast<u8>(p->channels > 2 ? 2 : p->channels);
    state->volume = 127;
    return 0;
}

} // namespace

void RegisterAudioOut() {
    DC_HLE("libSceAudioOut", sceAudioOutInit);
    DC_HLE("libSceAudioOut", sceAudioOutOpen);
    DC_HLE("libSceAudioOut", sceAudioOutClose);
    DC_HLE("libSceAudioOut", sceAudioOutOutput);
    DC_HLE("libSceAudioOut", sceAudioOutOutputs);
    DC_HLE("libSceAudioOut", sceAudioOutSetVolume);
    DC_HLE("libSceAudioOut", sceAudioOutGetPortState);
}

} // namespace Hle::AudioOut
