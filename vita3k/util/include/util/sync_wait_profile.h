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
enum class WaitKind : uint32_t {
    GuestPrimitive,
    RendererQueue,
    RendererStatus,
    RendererSync,
    VulkanAcquire,
    VulkanFence,
    VulkanPresent,
    VulkanFrameReady,
    Count
};
struct WaitContext {
    uint64_t object = 0, related = 0;
    int64_t timeout_us = -1; // -1: no finite API budget known
    int32_t guest_tid = 0;
    uint32_t guest_pc = 0, import_nid = 0, import_lr = 0;
};
// Completed API spans, including lock reacquisition and wake-up bookkeeping.
// Nested/concurrent spans overlap; elapsed duration is not OS sleep or CPU time.
class SyncWaitProfile {
public:
    static constexpr size_t record_limit = 65536, bucket_limit = 4096;
    struct Record {
        uint64_t sequence, start_qpc, end_qpc, object, related;
        int64_t timeout_us;
        int32_t result;
        uint32_t native_tid;
        int32_t guest_tid;
        uint32_t guest_pc, import_nid, import_lr, kind, result_known;
    };
    struct Stats {
        uint64_t count = 0, ticks = 0, max_ticks = 0, over_2ms = 0, over_10ms = 0, over_50ms = 0;
    };
    struct Bucket {
        uint64_t index = 0;
        Stats stats;
    };
    explicit SyncWaitProfile(std::filesystem::path directory, std::string title,
        size_t capacity = record_limit);
    static uint64_t counter() noexcept;
    void append(WaitKind kind, const WaitContext &context, uint64_t start, uint64_t end,
        uint32_t tid, int32_t result, bool known) noexcept;
    bool finish() noexcept;
    const std::filesystem::path directory;
    const uint64_t frequency, origin_qpc;
    std::atomic<uint64_t> in_flight{ 0 };
    std::atomic<bool> accepting{ true };

private:
    std::mutex mutex;
    std::vector<Record> records;
    std::array<Stats, static_cast<size_t>(WaitKind::Count)> kinds{};
    std::array<Bucket, bucket_limit> buckets{};
    uint64_t bucket_width, accepted = 0, detailed = 0;
    std::atomic<uint64_t> contention_drops{ 0 };
    bool finished = false, saved = false;
};
void publish_sync_wait_profile(std::shared_ptr<SyncWaitProfile> profile) noexcept;
bool sync_wait_profile_enabled() noexcept;
std::filesystem::path fresh_diagnostic_path(const std::filesystem::path &cache, const char *kind);
class SyncWaitScope {
public:
    explicit SyncWaitScope(WaitKind kind, WaitContext context = {}) noexcept;
    ~SyncWaitScope() noexcept;
    void result(int32_t value) noexcept {
        code = value;
        known = true;
    }
    int32_t finish(int32_t value) noexcept {
        result(value);
        return value;
    }
    SyncWaitScope(const SyncWaitScope &) = delete;
    SyncWaitScope &operator=(const SyncWaitScope &) = delete;

private:
    std::shared_ptr<SyncWaitProfile> profile;
    WaitKind kind;
    WaitContext context;
    uint64_t start = 0;
    uint32_t tid = 0;
    int32_t code = 0;
    bool known = false;
};
} // namespace util
