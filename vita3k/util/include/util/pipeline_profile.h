// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace util {
enum class PipelineKind : uint32_t {
    AsyncQueued,
    AsyncSkipped,
    FailedCached,
    QueueDelay,
    AsyncCompile,
    QueueCancelled,
    ProtectedWait,
    SyncDisabled,
    SyncKnownCache,
    SyncProtected,
    CacheProbe,
    WarmQueueDelay,
    WarmSkipDrawn,
    WarmMissingShader,
    WarmProbe,
    WarmCompile,
    WarmTotal,
    Count
};
struct PipelineContext {
    uint64_t pipeline = 0, pair = 0;
    uint32_t reason = 0; // bit 0 async enabled; bit 1 known-cache; bit 2 protected draw
};
// Selected pipeline misses/compiles/waits, not ready-cache hits.
// Nested/concurrent spans overlap; elapsed duration is not CPU or GPU time.
class PipelineProfile {
public:
    static constexpr size_t record_limit = 65536, bucket_limit = 4096;
    struct Record {
        uint64_t sequence, start_qpc, end_qpc, pipeline, pair;
        int32_t result;
        uint32_t native_tid, kind, result_known, reason;
    };
    struct Stats {
        uint64_t count = 0, ticks = 0, max_ticks = 0, over_2ms = 0, over_10ms = 0, over_50ms = 0, known_results = 0, nonzero_results = 0;
    };
    struct Bucket {
        uint64_t index = 0;
        Stats stats;
    };
    explicit PipelineProfile(std::filesystem::path directory, std::string title,
        size_t capacity = record_limit);
    static uint64_t counter() noexcept;
    void append(PipelineKind kind, const PipelineContext &context, uint64_t start, uint64_t end,
        uint32_t tid, int32_t result, bool known) noexcept;
    bool finish() noexcept;
    const std::filesystem::path directory;
    const uint64_t frequency, origin_qpc;
    std::atomic<uint64_t> in_flight{ 0 };
    std::atomic<bool> accepting{ true };

private:
    std::mutex mutex;
    std::vector<Record> records;
    std::array<Stats, static_cast<size_t>(PipelineKind::Count)> kinds{};
    std::array<Bucket, bucket_limit> buckets{};
    uint64_t bucket_width, accepted = 0, detailed = 0;
    std::atomic<uint64_t> contention_drops{ 0 };
    bool finished = false, saved = false;
};
void publish_pipeline_profile(std::shared_ptr<PipelineProfile> profile) noexcept;
bool pipeline_profile_enabled() noexcept;
class PipelineScope {
public:
    explicit PipelineScope(PipelineKind kind, PipelineContext context = {}, uint64_t queued_at = 0) noexcept;
    ~PipelineScope() noexcept;
    void result(int32_t value) noexcept {
        code = value;
        known = true;
    }
    int32_t finish(int32_t value) noexcept {
        result(value);
        return value;
    }
    PipelineScope(const PipelineScope &) = delete;
    PipelineScope &operator=(const PipelineScope &) = delete;

private:
    std::shared_ptr<PipelineProfile> profile;
    PipelineKind kind;
    PipelineContext context;
    uint64_t start = 0;
    uint32_t tid = 0;
    int32_t code = 0;
    bool known = false;
};
} // namespace util
