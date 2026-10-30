// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <renderer/learned_texture_profile.h>

using Cache = renderer::DecodedTextureCache;
using Profile = renderer::LearnedTextureProfile;
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("vita-preparation-" + std::to_string(Profile::Clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{ root };
    Cache cache(32768, 64);
    Profile profile(cache);
    unsigned calls = 0;
    auto decode = [&](const void *p, uint32_t, uint32_t w, uint32_t h, uint32_t, uint8_t *out) {
        ++calls;
        std::fill(out, out + w * h * 4, *static_cast<const uint8_t *>(p));
        return 17u;
    };
    assert(profile.begin(root, "TEST01", "1|eboot", Profile::Mode::Learn, {}, decode).status == "learn-ready");
    std::vector<Cache::Key> keys;
    for (uint8_t i = 0; i < 34; ++i) {
        const uint32_t dim = i < 2 ? 64 : 8;
        const std::vector<uint8_t> bytes(dim * dim / 2, i);
        Cache::Key key{ XXH3_64bits(bytes.data(), bytes.size()), dim, dim, 0 };
        keys.push_back(key);
        // Absolute-cost ranking favors the two larger inputs. Cost per byte
        // favors more useful smaller inputs that all fit the retained budget.
        cache.merge_learning(key, bytes, i < 2 ? 20000000 : 5000000, 0);
    }
    assert(profile.finish().saved_entries == 34);
    auto report = profile.begin(root, "TEST01", "1|eboot", Profile::Mode::Preload, {}, decode);
    assert(report.prepared_entries == 32 && calls == 32 && report.preload_budget_skips == 2);
    auto stats = cache.stats();
    assert(stats.resident_bytes == 9216 && stats.resident_bytes == stats.retained_bytes);
    assert(stats.evictions == 0 && stats.retention_demotions == 0);
    assert(stats.resident_bytes <= report.preparation_budget_bytes && report.preparation_budget_bytes == 24576);
    uint32_t result = 0;
    std::vector<uint8_t> output(256, 99);
    assert(!cache.copy(keys[0], std::vector<uint8_t>(2048, 0), output, result));
    for (uint8_t i = 2; i < 34; ++i) {
        assert(cache.copy(keys[i], std::vector<uint8_t>(32, i), output, result) && result == 17);
        assert(std::all_of(output.begin(), output.end(), [&](auto b) { return b == i; }));
    }
    profile.finish();
    // Mixed recent demand and a rare costly effect both receive preparation.
    std::vector<Profile::Record> plan;
    for (uint64_t i = 0; i < 30; ++i)
        plan.push_back({ { i, 8, 8, 0 }, 100000, 100, 0, 32, 64, 0 });
    plan.push_back({ { 99, 16, 16, 0 }, 30000000, 1, 0, 128, 0, 777 });
    plan.push_back({ { 100, 8, 8, 0 }, 20000, 1, 0, 32, 0, 777 });
    Profile::order_preparation(plan, 8192);
    size_t selected_bytes = 0;
    bool effect = false, companion = false;
    for (const auto &record : plan) {
        const size_t bytes = record.size + record.key.width * record.key.height * 4;
        if (bytes > 8192 - selected_bytes)
            continue;
        selected_bytes += bytes;
        effect |= record.key.hash == 99;
        companion |= record.key.hash == 100;
    }
    assert(effect && companion && selected_bytes <= 8192);
    // Bounds, no startup demand, actual-use aging, and no-play preservation.
    Cache history(10000);
    std::vector<uint8_t> source(32), decoded(256);
    const Cache::Key learned{ 1, 8, 8, 0 };
    history.start_learning(1024);
    history.merge_learning(learned, source, 100, 8, 16, 777);
    assert(history.learning_snapshot().front().recent == 16);
    history.start_learning(1024);
    history.merge_learning(learned, source, 100, 8, 16, 777);
    assert(history.insert(learned, source, decoded, 42, 100, false));
    uint32_t learned_result = 0;
    assert(history.copy(learned, source, decoded, learned_result));
    const auto learned_entries = history.learning_snapshot();
    assert(learned_entries.front().recent == 9 && learned_entries.front().cohort != 0);
    // Optional observer must not be required for the admission/aging policy.
    Cache aged(64, 8);
    const auto key = [](uint64_t i) { return Cache::Key{ i, 8, 8, 0 }; };
    const std::vector<uint8_t> input(4, 1), pixels(4, 2);
    for (uint64_t i = 0; i < 6; ++i)
        assert(aged.insert(key(i), input, pixels, 42, 10000000, false, 8));
    assert(aged.insert(key(99), input, pixels, 42, 1000));
    assert(aged.insert(key(100), input, pixels, 42, 5000000));
    std::vector<uint8_t> small(4);
    for (int i = 0; i < 8; ++i)
        assert(aged.copy(key(100), input, small, result));
    assert(aged.stats().retention_demotions == 0); // Historical warm-up still useful.
    for (int i = 0; i < 140000; ++i)
        assert(aged.copy(key(99), input, small, result));
    const auto aged_demotions = aged.stats().retention_demotions;
    assert(aged_demotions <= 1); // One actually hot small entry can earn protection.
    assert(aged.copy(key(100), input, small, result));
    assert(aged.stats().retention_demotions > aged_demotions); // Old unused preparation ages out.
    assert(aged.stats().retained_bytes <= 48 && aged.stats().resident_bytes <= 64);
    aged.set_budget(0);
    assert(aged.stats().entries == 0 && !aged.preparation_fits(1, 1));
    // Independent entry pressure also prevents unretainable startup work.
    Cache entries(1024, 4);
    for (uint64_t i = 0; i < 3; ++i)
        assert(entries.insert(key(i), input, pixels, 42, 1, false));
    assert(!entries.preparation_fits(4, 4));
    assert(!entries.insert(key(3), input, pixels, 42, 1, false));
    assert(entries.stats().retained_entries == 3 && entries.stats().evictions == 0);
    // Admission runs before *any* eviction: a one-use large upload must not
    // destroy the prepared working set just to discover promotion was declined.
    Cache admission(64, 8);
    for (uint64_t i = 0; i < 6; ++i)
        assert(admission.insert(key(i), input, pixels, 42, 30000000, false, 8));
    const std::vector<uint8_t> large(52, 7);
    const auto before = admission.stats();
    for (int i = 0; i < 2; ++i) {
        assert(!admission.insert(key(900), input, large, 73, 1000000));
        assert(admission.stats().evictions == before.evictions);
        for (uint64_t k = 0; k < 6; ++k)
            assert(admission.copy(key(k), input, small, result) && result == 42);
    }
    // Repeated misses let a different scene replace old preparation. Declining
    // caching never changes returned decode pixels; the third attempt admits.
    assert(admission.insert(key(900), input, large, 73, 1000000));
    std::vector<uint8_t> large_out(52);
    assert(admission.copy(key(900), input, large_out, result) && result == 73 && large_out == large);
    assert(!admission.copy(key(900), std::vector<uint8_t>(4, 9), large_out, result));
    assert(admission.stats().resident_bytes <= 64);
    // Bounded victim inspection declines before removing even a cheap entry.
    Cache scan(1024, 128);
    for (uint64_t i = 0; i < 128; ++i)
        assert(scan.insert(key(i), input, pixels, 42, 0));
    const std::vector<uint8_t> huge(800, 8);
    assert(!scan.insert(key(999), input, huge, 81, 100000000));
    assert(scan.stats().evictions == 0 && scan.stats().entries == 128);
    assert(!scan.insert(key(999), input, huge, 81, 100000000));
    assert(scan.insert(key(999), input, huge, 81, 100000000));
    assert(scan.stats().resident_bytes <= 1024);
    scan.clear();
    // Clearing must discard stale admission attempts, too.
    for (uint64_t i = 0; i < 128; ++i)
        assert(scan.insert(key(i), input, pixels, 42, 0));
    assert(!scan.insert(key(999), input, huge, 81, 100000000));
}
