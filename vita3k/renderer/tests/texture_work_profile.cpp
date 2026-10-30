// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <fstream>
#include <renderer/decoded_texture_cache.h>
#include <renderer/texture_work_profile.h>
#include <sstream>
#include <thread>
#include <vector>
using namespace renderer;
std::string read(const std::filesystem::path &p) {
    std::ifstream f(p);
    assert(f);
    return { std::istreambuf_iterator<char>(f), {} };
}
uint64_t field(const std::string &s, const char *name) {
    const auto key = std::string("\"") + name + "\":";
    const auto at = s.find(key);
    assert(at != std::string::npos);
    return std::stoull(s.substr(at + key.size()));
}
uint64_t fake_qpc = 100;
uint64_t fake_counter() noexcept { return ++fake_qpc; }
void cache_marks_test() {
    DecodedTextureCache cache(4096);
    const DecodedTextureCache::Key key{ 42, 8, 8, 0 };
    const std::vector<uint8_t> source(8, 3), decoded(256, 7);
    assert(cache.insert(key, source, decoded, 99));
    std::vector<uint8_t> a(256), b(256);
    uint32_t ra = 0, rb = 0;
    DecodedTextureCache::CopyTiming t{ fake_counter };
    assert(cache.copy(key, source, a, ra, nullptr, &t));
    assert(t.start < t.locked && t.locked < t.validated && t.validated < t.copied && t.copied < t.done);
    assert(cache.copy(key, source, b, rb));
    assert(a == decoded && b == decoded && ra == rb && ra == 99);
    auto collision = source;
    collision[0] ^= 1;
    assert(!cache.copy(key, collision, a, ra, nullptr, &t));
    assert(t.copied == 0 && t.start < t.locked && t.locked < t.validated && t.validated < t.done);
    assert(a == decoded && ra == 99); // Mismatch never copies stale pixels.
    const auto stats = cache.stats();
    assert(stats.requests == 3 && stats.hits == 2 && stats.misses == 1);
}
int main() {
    cache_marks_test();
    static_assert(sizeof(TextureWorkProfile::Record) == 80);
    assert(!texture_work_profile_enabled());
    {
        TextureWorkScope off(TextureWorkKind::UploadTotal);
        assert(off.finish(-7) == -7);
    }
    const auto root = std::filesystem::temp_directory_path() / ("vita3k-texture-work-" + std::to_string(TextureWorkProfile::counter()));
    std::filesystem::create_directories(root);
    auto p = std::make_shared<TextureWorkProfile>(root / "ring", "TEST", 4);
    bool rejected = false;
    try {
        TextureWorkProfile duplicate(root / "ring", "TEST");
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
    const auto tick = p->frequency / 100; // 10ms
    for (int n = 0; n < 12; ++n)
        p->append(TextureWorkKind::UploadTotal, { 1, 2, 3 }, p->origin_qpc + n * tick, p->origin_qpc + (n + 1) * tick, 42, -8, true);
    // Short spans contribute summaries but not detail.
    p->append(TextureWorkKind::CacheTotal, {}, p->origin_qpc, p->origin_qpc + 1, 43, 0, false);
    // Fold beyond the original 4096 seconds without sleeping or growing memory.
    p->append(TextureWorkKind::StagingFenceWait, {}, p->origin_qpc + p->frequency * 5000, p->origin_qpc + p->frequency * 5001, 44, 0, true);
    assert(p->finish() && p->finish());
    auto report = read(p->directory / "summary.json");
    assert(field(report, "acceptedSpans") == 14 && field(report, "retainedRecords") == 4);
    assert(field(report, "detailedSpans") == 13 && field(report, "overwrittenRecords") == 9);
    assert(field(report, "bucketWidthQpc") == 2 * p->frequency);
    const auto detail = read(p->directory / "work.csv");
    assert(detail.find("10,") != std::string::npos);
    // Persist byte sizes, never the cache key or handle.
    assert(detail.find("sequence,startQpc,endQpc,sourceBytes,decodedBytes") == 0);
    assert(field(read(p->directory / "armed.json"), "recordBytes") == 80);
    assert(read(p->directory / "kinds.csv").find("staging_fence_wait") != std::string::npos);
    std::istringstream lines(detail);
    std::string line, token;
    std::getline(lines, line); // CSV header
    std::getline(lines, line);
    std::istringstream row(line);
    for (int column = 0; column < 4; ++column)
        std::getline(row, token, ',');
    assert(std::stoull(token) == uint64_t(1));
    assert(read(p->directory / "kinds.csv").find("knownResults,nonzeroResults") != std::string::npos);
    // Per-kind summaries retain short spans and byte counts across compaction.
    std::istringstream totals(read(p->directory / "kinds.csv"));
    std::getline(totals, line);
    uint64_t accepted_count = 0, source_bytes = 0, decoded_bytes = 0;
    while (std::getline(totals, line)) {
        std::istringstream row(line);
        std::vector<std::string> columns;
        while (std::getline(row, token, ','))
            columns.push_back(token);
        accepted_count += std::stoull(columns[2]);
        source_bytes += std::stoull(columns[10]);
        decoded_bytes += std::stoull(columns[11]);
    }
    assert(accepted_count == 14 && source_bytes == 12 && decoded_bytes == 24);
    std::istringstream buckets(read(p->directory / "session.csv"));
    std::getline(buckets, line);
    uint64_t bucket_count = 0, bucket_source = 0, bucket_decoded = 0;
    while (std::getline(buckets, line)) {
        std::istringstream row(line);
        std::vector<std::string> columns;
        while (std::getline(row, token, ','))
            columns.push_back(token);
        bucket_count += std::stoull(columns[3]);
        bucket_source += std::stoull(columns[11]);
        bucket_decoded += std::stoull(columns[12]);
    }
    assert(bucket_count == accepted_count && bucket_source == source_bytes && bucket_decoded == decoded_bytes);
    auto concurrent = std::make_shared<TextureWorkProfile>(root / "concurrent", "TEST");
    publish_texture_work_profile(concurrent);
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t)
        workers.emplace_back([] {
            for (int i = 0; i < 3000; ++i) {
                TextureWorkScope scope(TextureWorkKind::BackendUpload);
                scope.result(i);
            }
        });
    for (auto &t : workers)
        t.join();
    publish_texture_work_profile({});
    assert(concurrent->finish());
    report = read(concurrent->directory / "summary.json");
    assert(field(report, "acceptedSpans") + field(report, "contentionDrops") == 24000);
    assert(field(report, "inFlightAtStop") == 0);
    // An outstanding scope can safely outlive stop and publication reset.
    auto late = std::make_shared<TextureWorkProfile>(root / "late", "TEST");
    publish_texture_work_profile(late);
    {
        TextureWorkScope active(TextureWorkKind::CacheValidate);
        publish_texture_work_profile({});
        assert(late->finish());
        assert(field(read(late->directory / "summary.json"), "inFlightAtStop") == 1);
        late.reset();
    }
    assert(!texture_work_profile_enabled());
    std::filesystem::remove_all(root);
}
