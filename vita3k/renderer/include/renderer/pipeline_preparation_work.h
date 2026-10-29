// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/pipeline_recovery_policy.h>
#include <array>
#include <cstddef>
#include <iterator>
#include <list>
#include <map>
#include <unordered_map>

namespace renderer {
// Compact negative decisions outlive heavyweight queue owners. Caller locks.
class PipelineRecoveryCache {
    struct Entry { PipelineRecoveryPolicy policy; std::list<uint64_t>::iterator position; };
    std::list<uint64_t> order;
    std::unordered_map<uint64_t, Entry> entries;
public:
    static constexpr size_t limit = 2048;
    uint64_t evictions = 0;
    bool eligible(uint64_t key, uint64_t now, uint64_t history, uint64_t shaders) const {
        const auto it = entries.find(key);
        return it == entries.end() || it->second.policy.eligible(now, history, shaders);
    }
    void complete(uint64_t key, uint64_t now, uint64_t history, uint64_t shaders, uint32_t outcome) {
        auto it = entries.find(key);
        if (it == entries.end()) {
            if (entries.size() == limit) {
                entries.erase(order.front());
                order.pop_front();
                ++evictions;
            }
            order.push_back(key);
            it = entries.emplace(key, Entry{ {}, std::prev(order.end()) }).first;
        } else {
            order.splice(order.end(), order, it->second.position);
        }
        it->second.policy.complete(now, history, shaders, outcome);
    }
    size_t size() const { return entries.size(); }
    void clear() { entries.clear(); order.clear(); evictions = 0; }
};

// A real clock budget shared by all workers. Queue wakeups do not mint tokens.
class PipelinePreparationWorkBudget {
    uint64_t last_refill = 0, next_scan = 0;
    size_t tokens = 4;
    bool initialized = false, scanned = false;
public:
    static constexpr uint64_t interval_ns = 25000000ULL;
    static constexpr size_t burst = 4, scan_limit = 8;
    bool available(uint64_t now) {
        if (!initialized) { last_refill = now; initialized = true; }
        if (now < last_refill)
            return false;
        const auto count = (now - last_refill) / interval_ns;
        tokens += static_cast<size_t>(std::min<uint64_t>(burst - tokens, count));
        last_refill += count * interval_ns;
        return tokens != 0;
    }
    bool acquire(uint64_t now) {
        if (!available(now))
            return false;
        --tokens;
        return true;
    }
    // Keep one existing token available for newly announced actual pairs. This
    // does not mint tokens or increase the shared 40/s refill rate.
    bool background_available(uint64_t now) { return available(now) && tokens > 1; }
    bool acquire_background(uint64_t now) {
        if (!background_available(now)) return false;
        --tokens;
        return true;
    }
    bool scan(uint64_t now) {
        if (scanned && now < next_scan)
            return false;
        scanned = true;
        next_scan = now + interval_ns;
        return true;
    }
};

// Base-stage readiness, not a global shader revision. Caller locks.
class PipelineStageReadiness {
    std::map<std::array<uint8_t, 32>, uint64_t> revisions;
    uint64_t sequence = 0;
public:
    static constexpr size_t limit = 2048;
    void ready(const std::array<uint8_t, 32> &stage) {
        auto it = revisions.find(stage);
        if (it != revisions.end())
            it->second = ++sequence;
        else {
            if (revisions.size() == limit) {
                auto oldest = std::min_element(revisions.begin(), revisions.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
                revisions.erase(oldest);
            }
            revisions.emplace(stage, ++sequence);
        }
    }
    uint64_t generation(const std::array<uint8_t, 32> &vertex, const std::array<uint8_t, 32> &fragment) const {
        uint64_t result = 0;
        for (const auto *stage : { &vertex, &fragment }) {
            const auto it = revisions.find(*stage);
            if (it != revisions.end())
                result = std::max(result, it->second);
        }
        return result;
    }
    size_t size() const { return revisions.size(); }
    void clear() { revisions.clear(); sequence = 0; }
};
} // namespace renderer
