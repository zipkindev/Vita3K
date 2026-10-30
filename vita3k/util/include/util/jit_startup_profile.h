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
enum class JitStartupKind : uint32_t {
    CpuInit,
    JitCreate,
    DispatchClear,
    FastmemGrowth,
    FastmemSlowInsert,
    Count
};
struct JitStartupContext {
    uint64_t guest = 0, cpu = 0, size_before = 0, size_after = 0, buckets_before = 0, buckets_after = 0;
    uint32_t reason = 0; // 0 initial JIT, 1 rebuild; dispatch clear: table byte count
};
// Selected CPU/JIT startup and fast-memory table insertion spans.
// Nested/concurrent spans overlap; elapsed duration is not CPU or GPU time.
class JitStartupProfile {
public:
    static constexpr size_t record_limit = 65536, bucket_limit = 4096;
    struct Record {
        uint64_t sequence, start_qpc, end_qpc, guest, cpu, size_before, size_after, buckets_before, buckets_after;
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
    explicit JitStartupProfile(std::filesystem::path directory, std::string title,
        size_t capacity = record_limit);
    static uint64_t counter() noexcept;
    static uint64_t counter_frequency() noexcept;
    void append(JitStartupKind kind, const JitStartupContext &context, uint64_t start, uint64_t end,
        uint32_t tid, int32_t result, bool known) noexcept;
    bool finish() noexcept;
    const std::filesystem::path directory;
    const uint64_t frequency, origin_qpc;
    std::atomic<uint64_t> in_flight{ 0 };
    std::atomic<bool> accepting{ true };

private:
    std::mutex mutex;
    std::vector<Record> records;
    std::array<Stats, static_cast<size_t>(JitStartupKind::Count)> kinds{};
    std::array<Bucket, bucket_limit> buckets{};
    uint64_t bucket_width, accepted = 0, detailed = 0;
    std::atomic<uint64_t> contention_drops{ 0 };
    bool finished = false, saved = false;
};
void publish_jit_startup_profile(std::shared_ptr<JitStartupProfile> profile) noexcept;
bool jit_startup_profile_enabled() noexcept;
void record_jit_fastmem(JitStartupContext context, uint64_t start, uint64_t end) noexcept;
class JitStartupScope {
public:
    explicit JitStartupScope(JitStartupKind kind, JitStartupContext context = {}, uint64_t queued_at = 0) noexcept;
    ~JitStartupScope() noexcept;
    void metadata(uint64_t size_before, uint64_t size_after, uint64_t buckets_before, uint64_t buckets_after) noexcept {
        context.size_before = size_before;
        context.size_after = size_after;
        context.buckets_before = buckets_before;
        context.buckets_after = buckets_after;
    }
    void result(int32_t value) noexcept {
        code = value;
        known = true;
    }
    int32_t finish(int32_t value) noexcept {
        result(value);
        return value;
    }
    JitStartupScope(const JitStartupScope &) = delete;
    JitStartupScope &operator=(const JitStartupScope &) = delete;

private:
    std::shared_ptr<JitStartupProfile> profile;
    JitStartupKind kind;
    JitStartupContext context;
    uint64_t start = 0;
    uint32_t tid = 0;
    int32_t code = 0;
    bool known = false;
};
} // namespace util
