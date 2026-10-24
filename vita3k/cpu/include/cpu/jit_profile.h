// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cpu {

struct JitStartupRegion {
    std::string module_path;
    uint32_t base = 0;
    uint32_t size = 0;
    uint32_t permissions = 0;
};

struct JitProfileClock {
    uint64_t (*read)() noexcept = nullptr;
    uint64_t frequency = 0;
};

struct JitProfileConfig {
    std::filesystem::path directory;
    std::string title, version, executable;
    uint32_t guest_thread = 0;
    bool cpu_optimizations = true;
    std::vector<JitStartupRegion> regions;
    size_t max_records = 65536;
};

// One writer: the owning guest CPU, after configuration and until it has exited.
// All allocation and file I/O occur before recording or after that CPU exits.
// This is diagnostic observation, not a validated code-replay manifest.
class JitCompileProfile {
public:
    static constexpr size_t record_limit = 65536;
    static constexpr size_t read_limit = 4096;
    static constexpr size_t bucket_limit = 4096;
    enum Flags : uint32_t {
        OutsideStartupRegion = 1,
        ReadLimit = 2,
        InvalidRead = 4,
        InvalidatedDuringCompile = 8
    };
    struct Record {
        uint64_t sequence = 0, descriptor = 0, start_qpc = 0, end_qpc = 0;
        uint64_t epoch = 0, end_epoch = 0, native_bytes = 0;
        uint64_t read_hash = 14695981039346656037ull;
        uint32_t end_pc = 0, reads = 0, hook_reads = 0;
        uint32_t min_read = UINT32_MAX, max_read = 0;
        uint32_t region = UINT32_MAX, relative_pc = 0, flags = 0;
    };

    struct Bucket {
        uint64_t count = 0, compile_ns = 0, max_ns = 0, emitted_bytes = 0;
        uint64_t aborted = 0, invalidations = 0, flagged = 0, read_limited = 0;
    };

    explicit JitCompileProfile(JitProfileConfig config, JitProfileClock clock = {});
    ~JitCompileProfile();
    JitCompileProfile(const JitCompileProfile &) = delete;
    JitCompileProfile &operator=(const JitCompileProfile &) = delete;
    bool enabled() const noexcept;
    void begin(uint64_t descriptor) noexcept;
    void code_read(uint32_t address, uint32_t value, bool hook = false) noexcept;
    void invalid_read() noexcept;
    void complete(uint32_t end_pc, size_t native_bytes) noexcept;
    void abort() noexcept;
    void invalidated() noexcept;
    // Only after the owning CPU is quiescent. Idempotent; failure is diagnostic.
    bool finish() noexcept;
    // Physical ring storage. Use chronological_record for ordered access.
    const std::vector<Record> &records() const noexcept { return entries; }
    const Record &chronological_record(size_t index) const;
    const std::vector<Bucket> &session_buckets() const noexcept { return buckets; }
    size_t used_buckets() const noexcept { return bucket_count; }
    uint64_t bucket_width() const noexcept { return bucket_width_qpc; }
    uint64_t frequency() const noexcept { return clock_frequency; }

private:
    JitProfileConfig config;
    std::vector<Record> entries;
    std::vector<Bucket> buckets;
    Bucket &bucket_for(uint64_t qpc) noexcept;
    size_t next_record = 0, bucket_count = 0;
    uint64_t bucket_width_qpc = 0, bucket_folds = 0;
    uint64_t completed = 0, total_compile_ns = 0, total_emitted_bytes = 0;
    uint64_t finish_qpc = 0;
    uint64_t (*read_counter)() noexcept;
    Record current;
    uint64_t epoch = 0, abandoned = 0, clock_frequency = 0, origin_qpc = 0;
    bool active = false, finished = false, write_ok = false;
};

} // namespace cpu
