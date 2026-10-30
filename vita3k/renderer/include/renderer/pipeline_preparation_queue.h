// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>

namespace renderer {
// Caller owns synchronization. Exact learned work takes priority over guessed pairs.
template <typename T>
class PipelinePreparationQueue {
    std::deque<T> exact, actual, recovery, speculative;

public:
    static constexpr size_t limit = 64;
    size_t size() const { return exact.size() + actual.size() + recovery.size() + speculative.size(); }
    bool empty() const { return size() == 0; }
    bool has_exact() const { return !exact.empty(); }
    bool has_urgent() const { return !actual.empty(); }
    bool push(T item, bool learned_exact, bool actual_pair = false, bool retry = false) {
        if (size() >= limit)
            return false;
        (learned_exact ? exact : actual_pair ? retry ? recovery : actual
                                             : speculative)
            .push_back(item);
        return true;
    }
    bool promote(T item) {
        for (auto it = speculative.begin(); it != speculative.end(); ++it) {
            if (*it == item) {
                speculative.erase(it);
                actual.push_back(item);
                return true;
            }
        }
        return false;
    }
    T pop() {
        auto &queue = !exact.empty() ? exact : !actual.empty() ? actual
            : !recovery.empty()                                ? recovery
                                                               : speculative;
        auto item = queue.front();
        queue.pop_front();
        return item;
    }
    // Nanoseconds from the same monotonic clock; no recent draws permits both workers.
    static size_t concurrency(uint64_t now, uint64_t last_draw, bool draw_work_in_progress = false) {
        return draw_work_in_progress || (last_draw && now >= last_draw && now - last_draw < 250000000ULL) ? 1 : 2;
    }
};
} // namespace renderer
