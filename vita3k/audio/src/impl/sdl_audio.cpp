// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include "audio/impl/sdl_audio.h"
#include "util/log.h"
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_hints.h>

#include <algorithm>
#include <chrono>

#define SDL_CHECK_EXT(condition, ret)                         \
    do {                                                      \
        if (!(condition)) {                                   \
            LOG_ERROR("SDL audio error: {}", SDL_GetError()); \
            return ret;                                       \
        }                                                     \
    } while (0)

#define SDL_CHECK(f_call) SDL_CHECK_EXT(f_call, {})
#define SDL_CHECK_VOID(f_call) SDL_CHECK_EXT(f_call, )
#define SDL_CHECK_NEG(f_call) SDL_CHECK_EXT((f_call) >= 0, {})

// SDL pulls whole device buffers, so keep enough queued that a pull still leaves two behind it
static int get_threshold_samples(const int device_buffer_samples, const int device_freq) {
    return std::max(3 * device_buffer_samples, AUDIO_MIN_QUEUE_MS * device_freq / 1000);
}

static uint64_t steady_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void SDLCALL SDLAudioAdapter::thread_wakeup_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    assert(userdata != nullptr);
    assert(stream != nullptr);
    SDLAudioOutPort *port = static_cast<SDLAudioOutPort *>(userdata);
    const uint64_t now_us = steady_now_us();
    const uint64_t pull_us = static_cast<uint64_t>(port->adapter.device_buffer_samples) * 1000000 / std::max(port->adapter.dst_spec.freq, 1);
    const uint64_t play_end = std::max(port->play_end_us.load(std::memory_order_relaxed), now_us) + pull_us;
    port->play_end_us.store(std::min(play_end, now_us + 2 * pull_us), std::memory_order_relaxed);
    // every pull takes data out of the stream: let the producer re-check the queue now rather than at its timeout
    port->pull_seq.fetch_add(1, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(port->mutex);
    }
    port->cond_var.notify_one();
}

SDLAudioAdapter::SDLAudioAdapter(AudioState &audio_state)
    : AudioAdapter(audio_state) {}

SDLAudioAdapter::~SDLAudioAdapter() {
    if (device_id > 0) {
        SDL_CloseAudioDevice(device_id);
    }
}

bool SDLAudioAdapter::init() {
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    SDL_SetHint(SDL_HINT_ANDROID_LOW_LATENCY_AUDIO, "1");
    // SDL's AAudio device advertises its 44.1 kHz default, so ask for the Vita's 48 kHz explicitly
    const SDL_AudioSpec spec = {
        .format = SDL_AUDIO_S16LE,
        .channels = 2,
        .freq = 48000
    };
    device_id = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
#else
    // SDL3 default is 1024 sample frames for 48kHz audio, which is higher than cubeb.
    // Request smaller device buffer for lower latency callbacks.
    // 512 sample frames = 2048 bytes for stereo 16-bit, matching cubeb's callback size.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    device_id = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
#endif
    SDL_CHECK_EXT(device_id > 0, false);
    return true;
}

void SDLAudioAdapter::switch_state(const bool pause) {
    if (pause)
        SDL_CHECK_VOID(SDL_PauseAudioDevice(device_id));
    else
        SDL_CHECK_VOID(SDL_ResumeAudioDevice(device_id));
}

AudioOutPortPtr SDLAudioAdapter::open_port(int nb_channels, int freq, int nb_sample) {
    SDL_AudioSpec src_spec = {
        .format = SDL_AUDIO_S16LE,
        .channels = nb_channels,
        .freq = freq
    };
    SDL_CHECK(SDL_GetAudioDeviceFormat(device_id, &dst_spec, &device_buffer_samples));
    const AudioStreamPtr stream(SDL_CreateAudioStream(&src_spec, &dst_spec), SDL_DestroyAudioStream);
    SDL_CHECK(stream);
    SDL_CHECK(SDL_BindAudioStream(device_id, stream.get()));
    auto port = std::make_shared<SDLAudioOutPort>(stream, *this);
    SDL_CHECK(SDL_SetAudioStreamGetCallback(stream.get(), SDLAudioAdapter::thread_wakeup_callback, port.get()));
    port->channels = nb_channels;
    port->len_microseconds = (nb_sample * 1'000'000ULL) / freq;
    port->len_bytes = nb_sample * nb_channels * sizeof(int16_t);
    switch_state(false);
    return port;
}

void SDLAudioAdapter::audio_output(AudioOutPort &out_port, const void *buffer) {
    if (out_port.stopping)
        return;

    //  Put audio to the port's stream and see how much is left to play.
    SDLAudioOutPort &port = static_cast<SDLAudioOutPort &>(out_port);
    const int threshold = get_threshold_samples(device_buffer_samples, dst_spec.freq);
    const uint64_t deadline = steady_now_us() + port.len_microseconds * 2;
    for (;;) {
        const uint32_t seen_pulls = port.pull_seq.load(std::memory_order_acquire);
        const int queued = get_rest_sample(port);
        const uint64_t now = steady_now_us();
        const uint64_t play_end = port.play_end_us.load(std::memory_order_relaxed);
        const int unplayed = (play_end > now && dst_spec.freq > 0) ? static_cast<int>((play_end - now) * dst_spec.freq / 1000000) : 0;
        if (queued + unplayed <= threshold || now >= deadline)
            break;
        uint64_t wait_us = deadline - now;
        if (queued <= threshold)
            wait_us = std::min<uint64_t>(wait_us, static_cast<uint64_t>(queued + unplayed - threshold) * 1000000 / dst_spec.freq + 1);
        std::unique_lock<std::mutex> lock(port.mutex);
        port.cond_var.wait_for(lock, std::chrono::microseconds(wait_us), [&] {
            return port.pull_seq.load(std::memory_order_relaxed) != seen_pulls || out_port.stopping;
        });
        if (out_port.stopping)
            return;
    }
    SDL_CHECK_VOID(SDL_PutAudioStreamData(port.stream.get(), buffer, out_port.len_bytes));
}

void SDLAudioAdapter::set_volume(AudioOutPort &out_port, float volume) {
    SDL_CHECK_VOID(SDL_SetAudioStreamGain(static_cast<SDLAudioOutPort &>(out_port).stream.get(), volume));
}

int SDLAudioAdapter::get_rest_sample(AudioOutPort &out_port) {
    auto &port = static_cast<SDLAudioOutPort &>(out_port);
    const int bytes_available = SDL_GetAudioStreamAvailable(port.stream.get());
    SDL_CHECK_NEG(bytes_available);
    SDL_AudioSpec out_spec;
    if (!SDL_GetAudioStreamFormat(port.stream.get(), nullptr, &out_spec))
        out_spec = dst_spec;
    const int frame_size = SDL_AUDIO_FRAMESIZE(out_spec);
    return frame_size > 0 ? bytes_available / frame_size : 0;
}

void SDLAudioAdapter::wake_all_ports() {
    for (auto &[_, port_ptr] : state.out_ports) {
        auto &port = static_cast<SDLAudioOutPort &>(*port_ptr);
        {
            std::lock_guard<std::mutex> lock(port.mutex);
        }
        port.cond_var.notify_all();
    }
}
