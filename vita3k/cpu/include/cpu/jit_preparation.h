// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace cpu {
// Main-thread owner only. No stored machine code and no guest execution.
class JitPreparation {
public:
    static constexpr size_t capacity = 2048, read_limit = 256;
    static constexpr uint64_t native_limit = 8 * 1024 * 1024, time_limit_ms = 1500;
    struct Region {
        uint32_t base, size;
    };
    struct Read {
        uint32_t address = 0, value = 0, hook = 0;
    };
    struct Block {
        uint64_t descriptor = 0, cost_ns = 0;
        uint32_t end_pc = 0, count = 0;
        std::array<Read, read_limit> reads{};
    };
    struct Stats {
        uint64_t observed = 0, learned = 0, rejected = 0, loaded = 0;
        uint64_t attempted = 0, prepared = 0, skipped = 0, elapsed_ms = 0, native_bytes = 0;
        bool profile_valid = false, deadline = false, native_cap = false, saved = false;
    } stats;
    JitPreparation(std::filesystem::path profile, std::array<uint8_t, 32> identity, std::vector<Region> regions);
    ~JitPreparation();
    void begin(uint64_t descriptor) noexcept;
    void read(uint32_t address, uint32_t value, bool hook) noexcept;
    void invalid_read() noexcept { bad = true; }
    void complete(uint32_t end_pc, uint64_t cost_ns) noexcept;
    void abort() noexcept { active = false; }
    void invalidated() noexcept { ++epoch; }
    // Replays must match the entire ordered code/hook read sequence and end PC.
    void replay(const Block &block) noexcept;
    std::optional<uint32_t> replay_read(uint32_t address, uint32_t actual, bool hook) noexcept;
    bool expects_read(uint32_t address, bool hook) const noexcept;
    const Block *replay_block() const noexcept { return replaying; }
    bool allow_emit(uint64_t descriptor, uint32_t end_pc) noexcept;
    void end_replay() noexcept { replaying = nullptr; }
    bool is_replaying() const noexcept { return replaying != nullptr; }
    std::vector<Block> ranked() const;
    bool finish() noexcept;
    void report() const noexcept;

private:
    bool address_ok(uint32_t address) const noexcept;
    bool block_ok(const Block &block) const noexcept;
    void load();
    std::filesystem::path path;
    std::array<uint8_t, 32> identity;
    std::vector<Region> regions;
    std::vector<Block> slots;
    Block current;
    const Block *replaying = nullptr;
    size_t replay_index = 0;
    uint64_t epoch = 0, begin_epoch = 0;
    bool active = false, bad = false, finished = false;
};
} // namespace cpu
