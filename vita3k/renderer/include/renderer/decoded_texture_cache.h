// Vita3K emulator project
// Copyright (C) 2026 Vita3K contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <list>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace renderer {

// CPU decoded pixels, independent of guest addresses and GPU texture lifetime.
// Hashes only locate candidates: source bytes must also match before reuse.
class DecodedTextureCache {
public:
    struct Key {
        uint64_t hash;
        uint32_t width, height, format;
        bool operator==(const Key &) const = default;
    };
    struct Stats {
        uint64_t requests = 0, hits = 0, misses = 0, bypasses = 0;
        uint64_t evictions = 0, decodes = 0, decode_ns = 0;
        // Exclusive causes: a victim can satisfy both bounds simultaneously.
        uint64_t byte_evictions = 0, entry_evictions = 0, both_evictions = 0, clear_evictions = 0;
        uint64_t decode_ge2ms = 0, decode_ge10ms = 0, decode_ge50ms = 0;
        uint64_t max_decode_ns = 0, interval_max_decode_ns = 0;
        uint32_t interval_max_width = 0, interval_max_height = 0;
        size_t resident_bytes = 0, entries = 0, budget_bytes = 0;
    };

    explicit DecodedTextureCache(size_t budget, size_t max_entries = 16384)
        : budget(budget)
        , max_entries(max_entries) {}

    bool eligible(size_t compressed, size_t decoded) const {
        std::lock_guard lock(mutex);
        return max_entries && compressed <= budget && decoded <= budget - compressed;
    }

    bool copy(const Key &key, std::span<const uint8_t> compressed, std::span<uint8_t> output, uint32_t &result) {
        std::lock_guard lock(mutex);
        ++counts.requests;
        const auto [begin, end] = lookup.equal_range(key);
        for (auto it = begin; it != end; ++it) {
            auto entry = it->second;
            if (entry->decoded.size() != output.size() || entry->compressed.size() != compressed.size()
                || !std::equal(compressed.begin(), compressed.end(), entry->compressed.begin()))
                continue;
            std::memcpy(output.data(), entry->decoded.data(), output.size());
            result = entry->result;
            lru.splice(lru.begin(), lru, entry);
            ++entry->uses;
            ++counts.hits;
            return true;
        }
        ++counts.misses;
        return false;
    }

    bool insert(const Key &key, std::vector<uint8_t> compressed, std::span<const uint8_t> decoded, uint32_t result, uint64_t decode_ns = 0, bool allow_eviction = true) {
        std::lock_guard lock(mutex);
        if (!max_entries || compressed.capacity() > budget || decoded.size() > budget - compressed.capacity())
            return false;
        const auto [begin, end] = lookup.equal_range(key);
        for (auto it = begin; it != end; ++it) {
            if (it->second->compressed == compressed && it->second->decoded.size() == decoded.size())
                return false; // Another decoder inserted this immutable content concurrently.
        }
        std::vector<uint8_t> pixels(decoded.begin(), decoded.end());
        const size_t cost = compressed.capacity() + pixels.capacity();
        if (cost > budget)
            return false;
        if (!allow_eviction && (resident > budget - cost || lru.size() >= max_entries))
            return false;
        while (!lru.empty() && (resident > budget - cost || lru.size() >= max_entries)) {
            const bool bytes_full = resident > budget - cost;
            const bool entries_full = lru.size() >= max_entries;
            if (bytes_full && entries_full)
                ++counts.both_evictions;
            else if (bytes_full)
                ++counts.byte_evictions;
            else
                ++counts.entry_evictions;
            const auto victim = std::prev(lru.end());
            const auto candidates = lookup.equal_range(victim->key);
            for (auto it = candidates.first; it != candidates.second; ++it) {
                if (it->second == victim) {
                    lookup.erase(it);
                    break;
                }
            }
            resident -= victim->compressed.capacity() + victim->decoded.capacity();
            lru.erase(victim);
            ++counts.evictions;
        }
        lru.push_front({ key, std::move(compressed), std::move(pixels), result, decode_ns, 0 });
        lookup.emplace(key, lru.begin());
        resident += cost;
        return true;
    }

    struct ProfileInput {
        Key key;
        std::vector<uint8_t> compressed;
        uint64_t decode_ns, uses;
    };

    // Session stop only: copies compressed bytes, never decoded pixels. The
    // renderer must be joined before taking a learning snapshot or clearing it.
    std::vector<ProfileInput> profile_snapshot(size_t file_budget, size_t resident_budget, size_t scratch_budget) const {
        std::lock_guard lock(mutex);
        std::vector<const Entry *> ranked;
        for (const auto &entry : lru)
            ranked.push_back(&entry);
        std::stable_sort(ranked.begin(), ranked.end(), [](const Entry *a, const Entry *b) {
            const auto score = [](const Entry *e) { return std::min<uint64_t>(e->decode_ns, 1000000000000ull) * (1 + std::min<uint64_t>(e->uses, 16)); };
            return score(a) > score(b);
        });
        std::vector<ProfileInput> result;
        size_t bytes = 0, pixels = 0;
        for (const auto *entry : ranked) {
            const size_t cost = entry->compressed.size() + entry->decoded.size();
            const size_t packed = entry->compressed.size() + 40;
            if (cost > scratch_budget || packed > file_budget - bytes || cost > resident_budget - pixels)
                continue;
            result.push_back({ entry->key, entry->compressed, entry->decode_ns, entry->uses });
            bytes += packed;
            pixels += cost;
        }
        return result;
    }

    void clear() {
        std::lock_guard lock(mutex);
        counts.evictions += lru.size();
        counts.clear_evictions += lru.size();
        lookup.clear();
        lru.clear();
        resident = 0;
    }

    void bypass() {
        std::lock_guard lock(mutex);
        ++counts.requests;
        ++counts.bypasses;
    }

    void decoded(uint64_t nanoseconds, uint32_t width = 0, uint32_t height = 0) {
        std::lock_guard lock(mutex);
        ++counts.decodes;
        counts.decode_ns += nanoseconds;
        counts.decode_ge2ms += nanoseconds >= 2000000;
        counts.decode_ge10ms += nanoseconds >= 10000000;
        counts.decode_ge50ms += nanoseconds >= 50000000;
        counts.max_decode_ns = std::max(counts.max_decode_ns, nanoseconds);
        if (nanoseconds > counts.interval_max_decode_ns) {
            counts.interval_max_decode_ns = nanoseconds;
            counts.interval_max_width = width;
            counts.interval_max_height = height;
        }
    }

    void set_budget(size_t bytes) {
        std::lock_guard lock(mutex);
        if (budget == bytes)
            return;
        counts.evictions += lru.size();
        counts.clear_evictions += lru.size();
        lookup.clear();
        lru.clear();
        resident = 0;
        budget = bytes;
    }

    Stats stats() const {
        std::lock_guard lock(mutex);
        return stats_unlocked();
    }

    // Only the optional telemetry writer consumes/resets the interval peak.
    Stats telemetry_stats() {
        std::lock_guard lock(mutex);
        const auto value = stats_unlocked();
        counts.interval_max_decode_ns = 0;
        counts.interval_max_width = counts.interval_max_height = 0;
        return value;
    }

private:
    Stats stats_unlocked() const {
        auto value = counts;
        value.resident_bytes = resident;
        value.entries = lru.size();
        value.budget_bytes = budget;
        return value;
    }

    struct Entry {
        Key key;
        std::vector<uint8_t> compressed, decoded;
        uint32_t result;
        uint64_t decode_ns, uses;
    };
    struct Hash {
        size_t operator()(const Key &key) const {
            return static_cast<size_t>(key.hash ^ (uint64_t(key.width) << 32) ^ key.height ^ (uint64_t(key.format) << 48));
        }
    };
    size_t budget;
    const size_t max_entries;
    mutable std::mutex mutex;
    std::list<Entry> lru;
    std::unordered_multimap<Key, std::list<Entry>::iterator, Hash> lookup;
    size_t resident = 0;
    Stats counts;
};
} // namespace renderer
