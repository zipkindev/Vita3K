// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <renderer/learned_texture_profile.h>
#include <renderer/pvrt-dec.h>
#include <renderer/pvrtc-cache.h>

using Profile = renderer::LearnedTextureProfile;
using Cache = renderer::DecodedTextureCache;
static auto read(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), {});
}
static void write(const std::filesystem::path &path, const std::vector<uint8_t> &bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}
static void seal(std::vector<uint8_t> &bytes) {
    auto checksum = XXH3_64bits(bytes.data(), bytes.size() - 8);
    for (unsigned i = 0; i < 8; ++i)
        bytes[bytes.size() - 8 + i] = uint8_t(checksum >> (8 * i));
}
int main() {
    auto root = std::filesystem::temp_directory_path() / ("vita-profile-tests-" + std::to_string(Profile::Clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{ root };
    std::filesystem::create_directories(root);
    const auto file = root / "decoded-textures" / "TEST01" / "learned.pvrp";
    Cache cache(16 * 1024 * 1024, 16384);
    Profile profile(cache);
    unsigned calls = 0;
    auto decode = [&](const void *input, uint32_t bits, uint32_t w, uint32_t h, uint32_t type, uint8_t *output) {
        ++calls;
        return pvr::PVRTDecompressPVRTCParallel(input, bits, w, h, type, output);
    };
    assert(profile.begin(root, "TEST01", "1.04|eboot", Profile::Mode::Learn, {}, decode).status == "learn-ready");
    struct Test {
        Cache::Key key;
        std::vector<uint8_t> input, expected;
        uint32_t result;
    };
    std::vector<Test> tests;
    unsigned seed = 97;
    for (uint32_t bits : { 0u, 1u })
        for (uint32_t type : { 0u, 1u })
            for (uint32_t width : { 1u, 16u, 128u, 512u })
                for (uint32_t height : { 1u, 8u, 32u, 128u }) {
                    Test t{ { 0, width, height, bits | (type << 1) }, std::vector<uint8_t>(std::max(width, bits ? 16u : 8u) * std::max(height, 8u) / (bits ? 4 : 2)), std::vector<uint8_t>(width * height * 4), 0 };
                    for (auto &byte : t.input) {
                        seed = seed * 1664525 + 1013904223;
                        byte = uint8_t(seed >> 24);
                    }
                    t.key.hash = XXH3_64bits(t.input.data(), t.input.size());
                    t.result = pvr::PVRTDecompressPVRTCUncached(t.input.data(), bits, width, height, type, t.expected.data());
                    cache.insert(t.key, t.input, t.expected, t.result, width * height);
                    tests.push_back(std::move(t));
                }
    assert(!std::filesystem::exists(file)); // No learning IO during demand decoding.
    assert(profile.finish().saved_entries == tests.size());
    assert(cache.stats().entries == 0);
    const auto valid = read(file);
    assert(valid.size() < Profile::file_limit);
    std::vector<Profile::Record> records;
    assert(Profile::validate(valid, "TEST01|1.04|eboot", records) && records.size() == tests.size());
    assert(!Profile::validate(valid, "OTHER|1.04|eboot", records));
    assert(!Profile::validate(valid, "TEST01|1.05|eboot", records));
    auto report = profile.begin(root, "TEST01", "1.04|eboot", Profile::Mode::Preload, {}, decode);
    assert(report.status == "preload-ready" && report.prepared_entries == tests.size() && calls == tests.size());
    assert(cache.stats().decodes == 0); // Prewarm must not distort demand counters.
    for (const auto &t : tests) {
        std::vector<uint8_t> pixels(t.expected.size());
        uint32_t result = 0;
        assert(cache.copy(t.key, t.input, pixels, result) && pixels == t.expected && result == t.result);
        auto changed = t.input;
        changed[0] ^= 1;
        assert(!cache.copy(t.key, changed, pixels, result)); // Exact bytes, even same hash/key.
    }
    profile.finish();
    assert(read(file) == valid); // Preload never changes profile.
    report = profile.begin(root, "TEST01", "1.04|eboot", Profile::Mode::Preload, {}, decode, std::chrono::milliseconds(0));
    assert(report.status == "partial-deadline" && !report.prepared_entries);
    profile.finish();
    cache.set_budget(64);
    report = profile.begin(root, "TEST01", "1.04|eboot", Profile::Mode::Preload, {}, decode);
    assert(report.status == "partial-budget" && report.prepared_entries > 0 && cache.stats().resident_bytes <= 64);
    profile.finish();
    cache.set_budget(0);
    assert(profile.begin(root, "TEST01", "1.04|eboot", Profile::Mode::Learn, {}, decode).status == "cache-disabled");
    profile.finish();
    assert(read(file) == valid);
    cache.set_budget(16 * 1024 * 1024);
    assert(profile.begin(root, "../TEST01", "1", Profile::Mode::Learn, {}, decode).status == "invalid-identity");
    assert(profile.begin(root, "TEST01", "1.04|eboot", Profile::Mode::Off, {}, decode).status == "off");
    profile.finish();
    assert(read(file) == valid);
    profile.begin(root, "TEST01", "1.04|eboot", Profile::Mode::Learn, {}, decode);
    assert(profile.finish().saved_entries == tests.size() && read(file) == valid);
    // Unknown versions, truncation, lengths, dimensions, per-record hash, and
    // checksum. Re-seal mutations to exercise validation beyond checksum alone.
    const size_t first = 20 + std::string("TEST01|1.04|eboot").size();
    for (size_t offset : { size_t(0), size_t(8), size_t(12), first - 4, first, first + 8, first + 12, first + 16, first + 20 }) {
        auto bad = valid;
        bad[offset] ^= 0xff;
        seal(bad);
        assert(!Profile::validate(bad, "TEST01|1.04|eboot", records));
        write(file, bad);
        const auto before = calls;
        auto invalid = profile.begin(root, "TEST01", "1.04|eboot", Profile::Mode::Preload, {}, decode);
        assert(invalid.status == "invalid-profile" && calls == before && !cache.stats().entries);
        profile.finish();
    }
    for (size_t length : { size_t(0), size_t(20), valid.size() - 1 }) {
        std::vector<uint8_t> bad(valid.begin(), valid.begin() + length);
        assert(!Profile::validate(bad, "TEST01|1.04|eboot", records));
    }
    size_t pixels;
    assert(!Profile::layout({ 0, 0xffffffffu, 0xffffffffu, 0 }, 32, pixels));
    assert(!Profile::layout({ 0, 8192, 8192, 0 }, 32 * 1024 * 1024, pixels));
    std::vector<uint8_t> oversized(Profile::file_limit + 1);
    assert(!Profile::validate(oversized, "TEST01|1.04|eboot", records));
    write(file, valid);
    // Atomic replacement and ranked snapshot, no-eviction admission.
    profile.begin(root, "TEST01", "1.04|eboot", Profile::Mode::Learn, {}, decode);
    const auto &t = tests.back();
    cache.insert(t.key, t.input, t.expected, t.result, 999);
    assert(profile.finish().saved_entries == tests.size() && read(file) != valid);
    Cache bounded_cache(1024, 1);
    const auto &a = tests.front();
    assert(bounded_cache.insert(a.key, a.input, a.expected, a.result, 1, false));
    const auto before = bounded_cache.stats();
    assert(!bounded_cache.insert(t.key, t.input, t.expected, t.result, 1, false));
    assert(bounded_cache.stats().entries == before.entries && bounded_cache.stats().evictions == before.evictions);
    std::cout << tests.size() << " profile reloads byte-identical; bounds, corruption, lifecycle passed\n";
}
