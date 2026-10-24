// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <fstream>
#include <iostream>
#include <renderer/learned_texture_profile.h>
#include <thread>
using Cache = renderer::DecodedTextureCache;
using Profile = renderer::LearnedTextureProfile;
int main() {
    auto root = std::filesystem::temp_directory_path() / ("vita-learning-" + std::to_string(Profile::Clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{ root };
    Cache cache(600, 16384);
    Profile profile(cache);
    unsigned calls = 0;
    auto decode = [&](const void *, uint32_t, uint32_t w, uint32_t h, uint32_t, uint8_t *out) { ++calls; std::fill(out, out + w*h*4, 0); return 17u; };
    auto add = [&](uint8_t value, uint64_t ns) {
        std::vector<uint8_t> bytes(32, value), pixels(256, 0);
        Cache::Key key{ XXH3_64bits(bytes.data(), bytes.size()), 8, 8, 0 };
        assert(cache.insert(key, bytes, pixels, 17, ns));
        return key;
    };
    auto records = [&] {
        std::ifstream stream(root / "decoded-textures/TEST01/learned.pvrp", std::ios::binary);
        std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(stream), {});
        std::vector<Profile::Record> result;
        assert(Profile::validate(bytes, "TEST01|1.00|eboot", result));
        return result;
    };
    auto contains = [](const auto &items, const auto &key) { return std::any_of(items.begin(), items.end(), [&](const auto &v) { return v.key == key; }); };
    assert(profile.begin(root, "TEST01", "1.00|eboot", Profile::Mode::Learn, {}, decode).status == "learn-ready");
    const auto a = add(1, 1000000), b = add(2, 100), c = add(3, 100);
    assert(cache.stats().entries == 2); // A already evicted from decoded RAM.
    assert(profile.finish().saved_entries == 3 && contains(records(), a));
    auto report = profile.begin(root, "TEST01", "1.00|eboot", Profile::Mode::Automatic, {}, decode);
    assert(report.profile_entries == 3 && report.prepared_entries == 2 && report.status == "partial-budget");
    assert(report.preload_budget_skips == 1 && report.learning.merged == 3);
    // A hit preserves actual decoded output and result while tracking usage.
    std::vector<uint8_t> input(32, 1), pixels(256, 0xff);
    uint32_t result = 0;
    assert(cache.copy(a, input, pixels, result) && result == 17);
    assert(std::all_of(pixels.begin(), pixels.end(), [](auto v) { return v == 0; }));
    const auto d = add(4, 500);
    cache.clear(); // Decoded-cache clear does not erase the independent history.
    assert(cache.learning_stats().entries == 4);
    assert(profile.finish().saved_entries == 4);
    auto saved = records();
    assert(contains(saved, a) && contains(saved, b) && contains(saved, c) && contains(saved, d));
    const auto before = calls;
    report = profile.begin(root, "TEST01", "1.00|eboot", Profile::Mode::Learn, {}, decode);
    assert(report.status == "learn-ready" && calls == before && report.learning.merged == 4);
    assert(profile.finish().saved_entries == 4); // No-play session merges previous pack.
    assert(cache.learning_stats().byte_limit == 0);
    profile.begin(root, "TEST01", "1.01|eboot", Profile::Mode::Automatic, {}, decode);
    assert(cache.learning_stats().entries == 0); // Never merge another version.
    profile.finish();
    profile.begin(root, "OTHER01", "1.00|eboot", Profile::Mode::Automatic, {}, decode);
    assert(cache.learning_stats().entries == 0);
    profile.finish();
    profile.begin(root, "TEST01", "1.00|eboot", Profile::Mode::Preload, {}, decode);
    assert(cache.learning_stats().byte_limit == 0);
    profile.finish();
    profile.begin(root, "TEST01", "1.00|eboot", Profile::Mode::Off, {}, decode);
    add(5, 100);
    assert(cache.learning_stats().entries == 0);
    profile.finish();
    // Both bounds are independent of decoded residency; exact input equality
    // handles hash collisions. Oversize/invalid records cannot break the bound.
    cache.clear();
    cache.start_learning(144, 2);
    add(1, 100);
    add(2, 100);
    add(3, 100);
    assert(cache.learning_stats().entries == 2 && cache.learning_stats().bytes <= 144 && cache.learning_stats().evictions == 1);
    cache.start_learning(10000, 1);
    add(1, 100);
    add(2, 100);
    assert(cache.learning_stats().entries == 1 && cache.learning_stats().evictions == 1);
    cache.start_learning(10000, 8);
    const Cache::Key collision{ 123, 8, 8, 0 };
    std::vector<uint8_t> x(32, 1), y(32, 2);
    cache.merge_learning(collision, x, 1, 0);
    cache.merge_learning(collision, y, 2, 0);
    cache.merge_learning(collision, x, 3, 0);
    assert(cache.learning_stats().entries == 2);
    cache.merge_learning({ 1, 0xffffffffu, 0xffffffffu, 0 }, x, 1, 0);
    assert(cache.learning_stats().dropped == 1);
    cache.stop_learning();
    assert(cache.learning_stats().bytes == 0);
    // A large selected input must not prevent smaller later records fitting.
    Cache limited_cache(300, 16384);
    Profile limited(limited_cache);
    auto partial = limited.begin(root, "TEST01", "1.00|eboot", Profile::Mode::Preload, {}, decode);
    assert(partial.prepared_entries == 1 && partial.preload_budget_skips == 3);
    limited.finish();
    // Archive selection is independent of decoded size; skip an oversized
    // first record and still prepare a smaller later one.
    profile.begin(root, "TEST02", "1.00|eboot", Profile::Mode::Learn, {}, decode);
    std::vector<uint8_t> big_input(64, 9), big_pixels(512, 0);
    Cache::Key big{ XXH3_64bits(big_input.data(), big_input.size()), 16, 8, 0 };
    assert(cache.insert(big, big_input, big_pixels, 17, 2000000));
    add(7, 100);
    assert(profile.finish().saved_entries == 2);
    auto skipped = limited.begin(root, "TEST02", "1.00|eboot", Profile::Mode::Preload, {}, decode);
    assert(skipped.prepared_entries == 1 && skipped.preload_budget_skips == 1);
    limited.finish();
    cache.start_learning(576, 8);
    std::vector<std::thread> workers;
    for (unsigned n = 0; n < 4; ++n)
        workers.emplace_back([&, n] {
            for (unsigned i = 0; i < 100; ++i) {
                std::vector<uint8_t> bytes(32, uint8_t(n + 10)), output(256, 0);
                Cache::Key key{ XXH3_64bits(bytes.data(), bytes.size()), 8, 8, 0 };
                uint32_t value = 0;
                if (!cache.copy(key, bytes, output, value))
                    cache.insert(key, bytes, output, 17, 100);
            }
        });
    for (auto &worker : workers)
        worker.join();
    assert(cache.learning_stats().entries <= 8 && cache.learning_stats().bytes <= 576);
    cache.stop_learning();
    std::cout << "Learning survives eviction/clear; prior merge, identity, pixels, disabled modes, byte/entry bounds and exact-input dedup passed\n";
}
