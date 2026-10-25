// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <renderer/decoded_texture_cache.h>

int main() {
    using Cache = renderer::DecodedTextureCache;
    const std::vector<uint8_t> input{ 1, 2, 3, 4 }, pixels{ 9, 8, 7, 6 };
    std::array<uint8_t, 4> output{};
    uint32_t result = 0;
    const auto key = [](uint64_t id) { return Cache::Key{ id, 8, 8, 0 }; };
    const auto bound = [](const Cache &cache) {
        const auto s = cache.stats();
        assert(s.resident_bytes <= s.budget_bytes);
        assert(s.retained_bytes <= s.budget_bytes - s.budget_bytes / 4);
        assert(s.retained_entries <= s.entries);
    };
    Cache cache(64, 8);
    cache.acquire_reuse_tracking();
    // Ranked preload retains only a bounded group. No-eviction preparation
    // still fills remaining space, and must reject rather than evict when full.
    for (uint64_t id = 0; id < 6; ++id) {
        assert(cache.insert(key(id), input, pixels, 32, 10000000 - id, false));
        bound(cache);
    }
    assert(cache.stats().retained_bytes == 48 && cache.stats().retained_entries == 6);
    assert(!cache.insert(key(6), input, pixels, 32, 20000000, false));
    assert(cache.stats().evictions == 0);
    // One-pass cheap loading textures churn in probation instead of flushing
    // every expensive startup texture. Byte-identical content/result retained.
    for (uint64_t id = 100; id < 200; ++id) {
        assert(cache.insert(key(id), input, pixels, 32, 1000));
        bound(cache);
    }
    for (uint64_t id = 0; id < 6; ++id)
        assert(cache.copy(key(id), input, output, result) && result == 32 && std::equal(output.begin(), output.end(), pixels.begin()));
    // A cheap one-pass hit cannot displace a more valuable prepared tail.
    const auto demotions = cache.stats().retention_demotions;
    assert(cache.copy(key(199), input, output, result));
    assert(cache.stats().retention_demotions == demotions);
    // Map transition: expensive new demand content takes priority, bounded by
    // the same byte/entry caps. Old-map content eventually yields space.
    for (uint64_t id = 200; id < 220; ++id) {
        bool admitted = false;
        for (unsigned attempt = 0; attempt < 3 && !admitted; ++attempt)
            admitted = cache.insert(key(id), input, pixels, 64, 12000000);
        assert(admitted);
        assert(cache.copy(key(id), input, output, result));
        assert(cache.copy(key(id), input, output, result));
        bound(cache);
    }
    for (uint64_t id = 0; id < 6; ++id)
        assert(!cache.copy(key(id), input, output, result));
    for (uint64_t id = 214; id < 220; ++id)
        assert(cache.copy(key(id), input, output, result) && result == 64);
    // A texture larger than the retained segment can still be admitted using
    // normal total bounds; it cannot occupy the retained group indefinitely.
    bool oversized_admitted = false;
    for (unsigned attempt = 0; attempt < 3 && !oversized_admitted; ++attempt)
        oversized_admitted = cache.insert(key(300), std::vector<uint8_t>(4), std::vector<uint8_t>(52), 1, 50000000);
    assert(oversized_admitted);
    bound(cache);
    assert(cache.stats().resident_bytes <= 64);
    cache.clear();
    assert(cache.stats().retained_bytes == 0 && cache.stats().retained_entries == 0);
    for (uint64_t id = 0; id < 1000; ++id) {
        if (id % 37 == 0)
            cache.set_budget(id % 74 ? 80 : 64);
        cache.insert(key(id % 20), input, pixels, 32, id % 3 ? 500 : 5000000);
        cache.copy(key((id + 3) % 20), input, output, result);
        assert(cache.stats().entries <= 8 && cache.stats().retained_entries <= 6);
        bound(cache);
    }
    cache.set_budget(0);
    assert(cache.stats().resident_bytes == 0 && cache.stats().retained_entries == 0);
    cache.set_budget(64);
    assert(cache.insert(key(0), input, pixels, 32, 5000000));
    assert(cache.copy(key(0), input, output, result));
    // Independent entry cap constrains retained small textures too.
    Cache entry_limited(1024, 4);
    for (uint64_t id = 0; id < 100; ++id) {
        entry_limited.insert(key(id), input, pixels, 32, 5000000);
        assert(entry_limited.stats().entries <= 4 && entry_limited.stats().retained_entries <= 3);
        bound(entry_limited);
    }
}
