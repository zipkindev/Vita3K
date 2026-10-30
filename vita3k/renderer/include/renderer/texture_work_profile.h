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

namespace renderer {
enum class TextureWorkKind : uint32_t {
    SourceSnapshotHash,
    CacheTotal,
    CacheLockWait,
    CacheValidate,
    CachePixelCopy,
    CacheBookkeeping,
    Decode,
    CacheInsert,
    UploadTotal,
    BackendUpload,
    StagingPrepare,
    StagingFenceWait,
    StagingAllocate,
    StagingCopy,
    UploadCommands,
    CacheDirectUpload,
    CachePrefetch,
    CacheResidency,
    Count
};
struct TextureWorkContext {
    uint64_t source_bytes = 0, decoded_bytes = 0;
    uint32_t width = 0, height = 0, format = 0, mip = 0, face = 0;
    uint32_t flags = 0; // CacheTotal: 1 hit, 2 miss, 4 bypass; staging wait: 1 current scene, 2 prior scene
};
// CPU texture work. Byte fields are stage-specific requested sizes, not resident memory.
// Nested/concurrent spans overlap; elapsed duration is not CPU or GPU time.
class TextureWorkProfile {
public:
    static constexpr size_t record_limit = 65536, bucket_limit = 4096;
    struct Record {
        uint64_t sequence, start_qpc, end_qpc, source_bytes, decoded_bytes;
        int32_t result;
        uint32_t native_tid, kind, result_known, width, height, format, mip, face, flags;
    };
    struct Stats {
        uint64_t count = 0, ticks = 0, max_ticks = 0, over_2ms = 0, over_10ms = 0, over_50ms = 0, known_results = 0, nonzero_results = 0, source_bytes = 0, decoded_bytes = 0;
    };
    struct Bucket {
        uint64_t index = 0;
        std::array<Stats, static_cast<size_t>(TextureWorkKind::Count)> stats{};
    };
    explicit TextureWorkProfile(std::filesystem::path directory, std::string title,
        size_t capacity = record_limit);
    static uint64_t counter() noexcept;
    void append(TextureWorkKind kind, const TextureWorkContext &context, uint64_t start, uint64_t end,
        uint32_t tid, int32_t result, bool known) noexcept;
    bool finish() noexcept;
    const std::filesystem::path directory;
    const uint64_t frequency, origin_qpc;
    std::atomic<uint64_t> in_flight{ 0 };
    std::atomic<bool> accepting{ true };

private:
    std::mutex mutex;
    std::vector<Record> records;
    std::array<Stats, static_cast<size_t>(TextureWorkKind::Count)> kinds{};
    std::vector<Bucket> buckets;
    uint64_t bucket_width, accepted = 0, detailed = 0;
    std::atomic<uint64_t> contention_drops{ 0 };
    bool finished = false, saved = false;
};
void publish_texture_work_profile(std::shared_ptr<TextureWorkProfile> profile) noexcept;
bool texture_work_profile_enabled() noexcept;
class TextureWorkScope {
public:
    explicit TextureWorkScope(TextureWorkKind kind, TextureWorkContext context = {}, uint64_t queued_at = 0) noexcept;
    ~TextureWorkScope() noexcept;
    bool active() const noexcept { return bool(profile); }
    void bytes(uint64_t source, uint64_t decoded) noexcept {
        context.source_bytes = source;
        context.decoded_bytes = decoded;
    }
    void flags(uint32_t value) noexcept { context.flags = value; }
    void end_now() noexcept {
        if (profile)
            measured_end = TextureWorkProfile::counter();
    }
    void bounds(uint64_t begin, uint64_t end) noexcept {
        start = begin;
        measured_end = end;
    }
    void record(TextureWorkKind stage, uint64_t begin, uint64_t end) noexcept {
        if (profile)
            profile->append(stage, context, begin, end, tid, code, known);
    }
    void result(int32_t value) noexcept {
        code = value;
        known = true;
    }
    int32_t finish(int32_t value) noexcept {
        result(value);
        return value;
    }
    TextureWorkScope(const TextureWorkScope &) = delete;
    TextureWorkScope &operator=(const TextureWorkScope &) = delete;

private:
    std::shared_ptr<TextureWorkProfile> profile;
    TextureWorkKind kind;
    TextureWorkContext context;
    uint64_t start = 0, measured_end = 0;
    uint32_t tid = 0;
    int32_t code = 0;
    bool known = false;
};
} // namespace renderer
