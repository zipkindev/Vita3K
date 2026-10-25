// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <fstream>
#include <renderer/decoded_texture_cache.h>
#include <string>
#include <tuple>

using Cache = renderer::DecodedTextureCache;
using Coverage = renderer::TextureCoverage;
static Cache::Key key(uint64_t value) { return { value, 8, 8, 0 }; }
static std::string read(const std::filesystem::path &path) {
    std::ifstream file(path);
    return { std::istreambuf_iterator<char>(file), {} };
}
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("vita-coverage-" + std::to_string(Coverage::now()));
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{ root };
    Coverage coverage;
    coverage.register_profile(key(1));
    coverage.prepare(key(1), true);
    coverage.register_profile(key(2));
    coverage.budget_skip(key(2));
    coverage.register_profile(key(3));
    auto pending = coverage.request(key(3), false, 0);
    assert(pending.reason == Coverage::Reason::NotPrepared);
    coverage.decoded(pending, 3000000, 8, 8);
    coverage.finish_preparation(true);
    coverage.register_profile(key(4));
    coverage.register_profile(key(4));
    for (auto [k, miss, expected] : {
             std::tuple{ 5, 0, Coverage::Reason::Absent },
             std::tuple{ 2, 0, Coverage::Reason::Budget },
             std::tuple{ 3, 0, Coverage::Reason::Deadline },
             std::tuple{ 1, 1, Coverage::Reason::PreparedEvicted },
             std::tuple{ 1, 2, Coverage::Reason::PreparedCleared },
             std::tuple{ 1, 0, Coverage::Reason::PreparedOther },
             std::tuple{ 4, 0, Coverage::Reason::Ambiguous } }) {
        auto ticket = coverage.request(key(k), false, miss);
        assert(ticket.reason == expected && ticket.id);
        coverage.decoded(ticket, 3000000, 8, 8);
    }
    coverage.decoded({}, 3000000, 8, 8);
    assert(coverage.write(root / "reasons"));
    const auto summary = read(root / "reasons-summary.json");
    assert(summary.find("\"reasonCounts\":[1,1,1,1,1,1,1,1,1]") != std::string::npos);
    assert(!coverage.write(root / "reasons")); // Do not overwrite evidence.
    for (size_t i = 0; i < Coverage::detail_limit + 10; ++i)
        coverage.decoded({}, 3000000, 8, 8);
    for (size_t i = 0; i < Coverage::identity_limit * 2; ++i)
        coverage.request(key(i + 100), true, 0);
    assert(coverage.write(root / "bounded"));
    assert(read(root / "bounded-summary.json").find("\"retainedDetails\":8192") != std::string::npos);
    assert(read(root / "bounded-summary.json").find("\"overwrittenDetails\":19") != std::string::npos);
    auto bounded = coverage.request(key(99999999), false, 0);
    assert(bounded.id == 0 && bounded.reason == Coverage::Reason::Untracked);

    Cache cache(384);
    cache.enable_coverage();
    cache.acquire_reuse_tracking();
    cache.coverage_profile(key(1));
    const std::vector<uint8_t> compressed(32, 17), pixels(256, 29);
    assert(cache.insert(key(1), compressed, pixels, 42, 3000000, false));
    cache.coverage_prepared(key(1), true);
    cache.coverage_finish_preparation(false);
    auto output = pixels;
    uint32_t result = 0;
    Cache::Observation observed;
    assert(cache.copy(key(1), compressed, output, result, &observed) && output == pixels && result == 42);
    assert(cache.insert(key(2), compressed, pixels, 42, 3000000));
    assert(!cache.copy(key(1), compressed, output, result, &observed));
    assert(observed.coverage.reason == Coverage::Reason::PreparedEvicted);
    cache.decoded(3000000, 8, 8, observed);
    cache.coverage_saved(key(1));
    auto report = cache.take_coverage();
    assert(report && !cache.coverage_active() && report->write(root / "integrated"));
    assert(read(root / "integrated-index.csv").find("1,8,8,0,2,2,1,3000000,0,1") != std::string::npos);
    // Disabled instrumentation leaves decoding/results unchanged and allocates nothing.
    assert(cache.copy(key(2), compressed, output, result) && output == pixels && result == 42);
}
