// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace renderer {
// Caller locks. Bounds live objects and compile rate, not the lifetime of a game session.
class PipelinePredictionAdmission {
    struct Bucket {
        uint64_t last = 0;
        size_t tokens;
        bool initialized = false;
        bool available(uint64_t now, size_t burst, uint64_t interval) {
            if (!initialized) {
                last = now;
                initialized = true;
            } else if (now >= last) {
                const auto refill = (now - last) / interval;
                tokens += static_cast<size_t>(std::min<uint64_t>(burst - tokens, refill));
                last += refill * interval;
            }
            return tokens != 0;
        }
    } actual{ 0, 8 }, hint{ 0, 2 };
    size_t live_actual = 0, live_hint = 0;

public:
    static constexpr size_t live_limit = 16, hint_limit = 4;
    static constexpr uint64_t reclaim_age_ns = 2000000000ULL;
    size_t live() const { return live_actual + live_hint; }
    size_t hints() const { return live_hint; }
    bool full(bool is_actual) const { return live() >= live_limit || (!is_actual && live_hint >= hint_limit); }
    bool has_token(uint64_t now, bool is_actual) {
        return is_actual ? actual.available(now, 8, 250000000ULL) : hint.available(now, 2, 2000000000ULL);
    }
    bool available(uint64_t now, bool is_actual) { return has_token(now, is_actual) && !full(is_actual); }
    bool acquire(uint64_t now, bool is_actual) {
        if (!available(now, is_actual))
            return false;
        --(is_actual ? actual.tokens : hint.tokens);
        ++(is_actual ? live_actual : live_hint);
        return true;
    }
    bool release(bool is_actual) {
        auto &count = is_actual ? live_actual : live_hint;
        if (!count)
            return false;
        --count;
        return true;
    }
    static bool reclaimable(uint64_t now, uint64_t completed, bool ready, bool draw_waiting) {
        return ready && !draw_waiting && now >= completed && now - completed >= reclaim_age_ns;
    }
};
} // namespace renderer
