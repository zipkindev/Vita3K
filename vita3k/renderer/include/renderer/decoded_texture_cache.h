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
        size_t resident_bytes = 0, entries = 0, budget_bytes = 0;
    };

    explicit DecodedTextureCache(size_t budget, size_t max_entries = 4096)
        : budget(budget)
        , max_entries(max_entries) {}

    bool eligible(size_t compressed, size_t decoded) const {
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
            ++counts.hits;
            return true;
        }
        ++counts.misses;
        return false;
    }

    void insert(const Key &key, std::vector<uint8_t> compressed, std::span<const uint8_t> decoded, uint32_t result) {
        if (!eligible(compressed.capacity(), decoded.size()))
            return;
        std::lock_guard lock(mutex);
        const auto [begin, end] = lookup.equal_range(key);
        for (auto it = begin; it != end; ++it) {
            if (it->second->compressed == compressed && it->second->decoded.size() == decoded.size())
                return; // Another decoder inserted this immutable content concurrently.
        }
        std::vector<uint8_t> pixels(decoded.begin(), decoded.end());
        const size_t cost = compressed.capacity() + pixels.capacity();
        if (cost > budget)
            return;
        while (!lru.empty() && (resident > budget - cost || lru.size() >= max_entries)) {
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
        lru.push_front({ key, std::move(compressed), std::move(pixels), result });
        lookup.emplace(key, lru.begin());
        resident += cost;
    }

    void bypass() {
        std::lock_guard lock(mutex);
        ++counts.requests;
        ++counts.bypasses;
    }

    void decoded(uint64_t nanoseconds) {
        std::lock_guard lock(mutex);
        ++counts.decodes;
        counts.decode_ns += nanoseconds;
    }

    Stats stats() const {
        std::lock_guard lock(mutex);
        auto value = counts;
        value.resident_bytes = resident;
        value.entries = lru.size();
        value.budget_bytes = budget;
        return value;
    }

private:
    struct Entry {
        Key key;
        std::vector<uint8_t> compressed, decoded;
        uint32_t result;
    };
    struct Hash {
        size_t operator()(const Key &key) const {
            return static_cast<size_t>(key.hash ^ (uint64_t(key.width) << 32) ^ key.height ^ (uint64_t(key.format) << 48));
        }
    };
    const size_t budget, max_entries;
    mutable std::mutex mutex;
    std::list<Entry> lru;
    std::unordered_multimap<Key, std::list<Entry>::iterator, Hash> lookup;
    size_t resident = 0;
    Stats counts;
};
} // namespace renderer
