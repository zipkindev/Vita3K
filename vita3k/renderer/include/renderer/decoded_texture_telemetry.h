// Vita3K emulator project
// Copyright (C) 2026 Vita3K contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <renderer/decoded_texture_cache.h>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace renderer {
// One cumulative snapshot/second, at most four hours per process. No texture
// identifiers/content, per-request logging, unbounded queues or render-thread IO.
// A fresh output path opts in; existing files are never intentionally replaced.
class DecodedTextureTelemetry {
public:
    explicit DecodedTextureTelemetry(DecodedTextureCache &cache, const char *path,
        std::chrono::milliseconds period = std::chrono::seconds(1)) noexcept {
        if (!path || !*path)
            return;
        try {
            worker = std::jthread([this, &cache, path = std::string(path), period](std::stop_token stop) {
                try {
                    const auto output = std::filesystem::path(std::u8string(path.begin(), path.end()));
                    if (std::filesystem::exists(output))
                        return;
                    std::ofstream file(output);
                    if (!file)
                        return;
                    file << "Schema,UnixNs,SteadyNs,Qpc,QpcFrequency,Requests,Hits,Misses,Bypasses,Evictions,ByteEvictions,EntryEvictions,BothEvictions,ClearEvictions,Decodes,DecodeNs,DecodeGE2ms,DecodeGE10ms,DecodeGE50ms,MaxDecodeNs,IntervalMaxDecodeNs,IntervalMaxWidth,IntervalMaxHeight,ResidentBytes,Entries,BudgetBytes\n";
                    // 14401 rows including the initial/final sample; <8MiB with
                    // fixed-width numeric fields. Failure/cap disables output.
                    for (unsigned row = 0; row < 14401; ++row) {
                        const bool final_sample = stop.stop_requested();
                        const auto unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                        const auto steady_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                        uint64_t qpc = 0, frequency = 0;
#ifdef _WIN32
                        LARGE_INTEGER counter, hz;
                        if (QueryPerformanceCounter(&counter) && QueryPerformanceFrequency(&hz)) {
                            qpc = counter.QuadPart;
                            frequency = hz.QuadPart;
                        }
#endif
                        const auto s = cache.telemetry_stats();
                        file << "1," << unix_ns << ',' << steady_ns << ',' << qpc << ',' << frequency
                             << ',' << s.requests << ',' << s.hits << ',' << s.misses << ',' << s.bypasses
                             << ',' << s.evictions << ',' << s.byte_evictions << ',' << s.entry_evictions << ',' << s.both_evictions << ',' << s.clear_evictions
                             << ',' << s.decodes << ',' << s.decode_ns << ',' << s.decode_ge2ms << ',' << s.decode_ge10ms << ',' << s.decode_ge50ms
                             << ',' << s.max_decode_ns << ',' << s.interval_max_decode_ns << ',' << s.interval_max_width << ',' << s.interval_max_height
                             << ',' << s.resident_bytes << ',' << s.entries << ',' << s.budget_bytes << '\n';
                        file.flush();
                        if (!file || final_sample)
                            break;
                        std::unique_lock lock(wait_mutex);
                        wake.wait_for(lock, period, [&] { return stop.stop_requested(); });
                    }
                } catch (...) {
                    // Diagnostics must never affect decoding or app shutdown.
                }
            });
        } catch (...) {
            // An unavailable helper leaves normal cache operation unchanged.
        }
    }

    ~DecodedTextureTelemetry() {
        worker.request_stop();
        wake.notify_all();
        // jthread joins before the condition variable/mutex are destroyed.
    }

private:
    std::mutex wait_mutex;
    std::condition_variable wake;
    std::jthread worker;
};
} // namespace renderer
