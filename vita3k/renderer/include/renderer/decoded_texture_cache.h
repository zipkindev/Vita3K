// Vita3K emulator project
// Copyright (C) 2026 Vita3K contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
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
    enum class MissClass : uint8_t { FirstObserved,
        CapacityEvicted,
        Cleared,
        SeenOther,
        Untracked };
    struct Observation {
        MissClass classification;
        Observation(MissClass value = MissClass::Untracked)
            : classification(value) {}
    };
    struct ReuseStats {
        std::array<uint64_t, 4> misses{}, decodes{}, decode_ns{};
        uint64_t history_replacements = 0, history_entries = 0, history_capacity = 0;
        uint64_t epochs = 0, preloaded_hits = 0, demand_hits = 0, untracked_decodes = 0;
        bool enabled = false;
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
        ReuseStats reuse;
    };

    explicit DecodedTextureCache(size_t budget, size_t max_entries = 16384)
        : budget(budget)
        , max_entries(max_entries) {}

    bool eligible(size_t compressed, size_t decoded) const {
        std::lock_guard lock(mutex);
        return max_entries && compressed <= budget && decoded <= budget - compressed;
    }

    bool copy(const Key &key, std::span<const uint8_t> compressed, std::span<uint8_t> output, uint32_t &result, Observation *observation = nullptr) {
        std::lock_guard lock(mutex);
        ++counts.requests;
        if (observation)
            *observation = {};
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
            if (reuse) {
                auto &slot = reuse_slot(key, compressed.size(), output.size());
                if (slot.state == ReuseState::Preloaded)
                    ++counts.reuse.preloaded_hits;
                else
                    ++counts.reuse.demand_hits;
                slot.state = slot.state == ReuseState::Preloaded ? ReuseState::Preloaded : ReuseState::Resident;
            }
            return true;
        }
        ++counts.misses;
        if (reuse) {
            auto &slot = reuse_slot(key, compressed.size(), output.size());
            const auto classification = slot.state == ReuseState::New ? MissClass::FirstObserved
                : slot.state == ReuseState::CapacityEvicted           ? MissClass::CapacityEvicted
                : slot.state == ReuseState::Cleared                   ? MissClass::Cleared
                                                                      : MissClass::SeenOther;
            ++counts.reuse.misses[static_cast<size_t>(classification)];
            slot.state = ReuseState::Pending;
            if (observation)
                observation->classification = classification;
        }
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
            if (reuse)
                reuse_slot(victim->key, victim->compressed.size(), victim->decoded.size()).state = ReuseState::CapacityEvicted;
            resident -= victim->compressed.capacity() + victim->decoded.capacity();
            lru.erase(victim);
            ++counts.evictions;
        }
        // Preparation submits records in descending profile priority. Append
        // each prepared entry so earlier, higher-ranked entries stay newer.
        // Demand inserts/hits retain ordinary most-recently-used promotion.
        const auto entry = lru.insert(allow_eviction ? lru.begin() : lru.end(),
            { key, std::move(compressed), std::move(pixels), result, decode_ns, 0 });
        lookup.emplace(key, entry);
        resident += cost;
        if (reuse)
            reuse_slot(key, entry->compressed.size(), decoded.size()).state = allow_eviction ? ReuseState::Resident : ReuseState::Preloaded;
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
        if (reuse)
            for (const auto &entry : lru)
                reuse_slot(entry.key, entry.compressed.size(), entry.decoded.size()).state = ReuseState::Cleared;
        lookup.clear();
        lru.clear();
        resident = 0;
    }

    void bypass() {
        std::lock_guard lock(mutex);
        ++counts.requests;
        ++counts.bypasses;
    }

    void decoded(uint64_t nanoseconds, uint32_t width = 0, uint32_t height = 0, Observation observation = {}) {
        std::lock_guard lock(mutex);
        ++counts.decodes;
        if (reuse) {
            const auto index = static_cast<size_t>(observation.classification);
            if (index < counts.reuse.decodes.size()) {
                ++counts.reuse.decodes[index];
                counts.reuse.decode_ns[index] += nanoseconds;
            } else {
                ++counts.reuse.untracked_decodes;
            }
        }
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
        if (reuse)
            for (const auto &entry : lru)
                reuse_slot(entry.key, entry.compressed.size(), entry.decoded.size()).state = ReuseState::Cleared;
        lookup.clear();
        lru.clear();
        resident = 0;
        budget = bytes;
    }

    // Enable only for optional telemetry. Fixed four-way history stores no
    // texture bytes. Classification is fingerprint-based and has bounded recall.
    void acquire_reuse_tracking() {
        std::lock_guard lock(mutex);
        if (reuse_users++ == 0) {
            try {
                reuse = std::make_unique<std::array<ReuseSlot, reuse_capacity>>();
            } catch (...) {
                --reuse_users;
                throw;
            }
            counts.reuse.enabled = true;
            counts.reuse.history_capacity = reuse_capacity;
            counts.reuse.history_entries = 0;
            ++counts.reuse.epochs;
            for (const auto &entry : lru)
                reuse_slot(entry.key, entry.compressed.size(), entry.decoded.size()).state = ReuseState::Resident;
        }
    }

    void release_reuse_tracking() {
        std::lock_guard lock(mutex);
        if (reuse_users && --reuse_users == 0) {
            reuse.reset();
            counts.reuse.enabled = false;
        }
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
    enum class ReuseState : uint8_t { Empty,
        New,
        Pending,
        Resident,
        Preloaded,
        CapacityEvicted,
        Cleared };
    struct ReuseSlot {
        Key key{};
        size_t compressed = 0, decoded = 0;
        uint64_t sequence = 0;
        ReuseState state = ReuseState::Empty;
    };
    static_assert(sizeof(ReuseSlot) <= 64);
    static constexpr size_t reuse_capacity = 65536;
    static constexpr size_t reuse_ways = 4;

    ReuseSlot &reuse_slot(const Key &key, size_t compressed, size_t decoded) {
        // Mix all identity fields, retaining the original fingerprint for full
        // slot comparison. A 64-bit collision can affect diagnostics only;
        // actual cache hits still require byte-for-byte source equality.
        uint64_t mixed = key.hash ^ (uint64_t(key.width) << 32) ^ key.height
            ^ (uint64_t(key.format) << 48) ^ compressed ^ (uint64_t(decoded) << 17);
        mixed ^= mixed >> 33;
        mixed *= 0xff51afd7ed558ccdULL;
        mixed ^= mixed >> 33;
        const size_t base = (mixed % (reuse_capacity / reuse_ways)) * reuse_ways;
        auto *oldest = &(*reuse)[base];
        for (size_t i = base; i < base + reuse_ways; ++i) {
            auto &slot = (*reuse)[i];
            if (slot.state != ReuseState::Empty && slot.key == key && slot.compressed == compressed && slot.decoded == decoded) {
                slot.sequence = ++reuse_sequence;
                return slot;
            }
            if (slot.sequence < oldest->sequence)
                oldest = &slot;
        }
        if (oldest->state == ReuseState::Empty)
            ++counts.reuse.history_entries;
        else
            ++counts.reuse.history_replacements;
        *oldest = { key, compressed, decoded, ++reuse_sequence, ReuseState::New };
        return *oldest;
    }

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
    std::unique_ptr<std::array<ReuseSlot, reuse_capacity>> reuse;
    uint64_t reuse_sequence = 0;
    size_t reuse_users = 0;
};
} // namespace renderer
