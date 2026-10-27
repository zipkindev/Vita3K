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
    std::deque<T> exact, speculative;

public:
    static constexpr size_t limit = 64;
    size_t size() const { return exact.size() + speculative.size(); }
    bool empty() const { return size() == 0; }
    bool push(T item, bool learned_exact) {
        if (size() >= limit)
            return false;
        (learned_exact ? exact : speculative).push_back(item);
        return true;
    }
    T pop() {
        auto &queue = exact.empty() ? speculative : exact;
        auto item = queue.front();
        queue.pop_front();
        return item;
    }
    // Nanoseconds from the same monotonic clock; no recent draws permits both workers.
    static size_t concurrency(uint64_t now, uint64_t last_draw) {
        return last_draw && now >= last_draw && now - last_draw < 250000000ULL ? 1 : 2;
    }
};
} // namespace renderer
