// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <fstream>
#include <sstream>
#include <thread>
#include <util/pipeline_profile.h>
#include <vector>
using namespace util;
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
int main() {
    static_assert(sizeof(PipelineProfile::Record) == 64);
    assert(!pipeline_profile_enabled());
    {
        PipelineScope off(PipelineKind::AsyncCompile);
        assert(off.finish(-7) == -7);
    }
    const auto root = std::filesystem::temp_directory_path() / ("vita3k-pipelines-" + std::to_string(PipelineProfile::counter()));
    std::filesystem::create_directories(root);
    auto p = std::make_shared<PipelineProfile>(root / "ring", "TEST", 4);
    bool rejected = false;
    try {
        PipelineProfile duplicate(root / "ring", "TEST");
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
    const auto tick = p->frequency / 100; // 10ms
    for (int n = 0; n < 12; ++n)
        p->append(PipelineKind::AsyncCompile, { 1, 2, 3 }, p->origin_qpc + n * tick, p->origin_qpc + (n + 1) * tick, 42, -8, true);
    // Short spans contribute summaries but not detail.
    p->append(PipelineKind::AsyncQueued, {}, p->origin_qpc, p->origin_qpc + 1, 43, 0, false);
    // Fold beyond the original 4096 seconds without sleeping or growing memory.
    p->append(PipelineKind::ProtectedWait, {}, p->origin_qpc + p->frequency * 5000, p->origin_qpc + p->frequency * 5001, 44, 0, true);
    assert(p->finish() && p->finish());
    auto report = read(p->directory / "summary.json");
    assert(field(report, "acceptedSpans") == 14 && field(report, "retainedRecords") == 4);
    assert(field(report, "detailedSpans") == 13 && field(report, "overwrittenRecords") == 9);
    assert(field(report, "bucketWidthQpc") == 2 * p->frequency);
    const auto detail = read(p->directory / "pipelines.csv");
    assert(detail.find("10,") != std::string::npos);
    // Persist session-local identifiers, never the original cache key/handle.
    assert(detail.find("sequence,startQpc,endQpc,pipeline,pair") == 0);
    assert(field(read(p->directory / "armed.json"), "recordBytes") == 64);
    assert(read(p->directory / "kinds.csv").find("protected_wait") != std::string::npos);
    std::istringstream lines(detail);
    std::string line, token;
    std::getline(lines, line); // CSV header
    std::getline(lines, line);
    std::istringstream row(line);
    for (int column = 0; column < 4; ++column)
        std::getline(row, token, ',');
    assert(std::stoull(token) == (uint64_t(1) ^ p->origin_qpc));
    assert(read(p->directory / "kinds.csv").find("knownResults,nonzeroResults") != std::string::npos);
    auto concurrent = std::make_shared<PipelineProfile>(root / "concurrent", "TEST");
    publish_pipeline_profile(concurrent);
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t)
        workers.emplace_back([] {
            for (int i = 0; i < 3000; ++i) {
                PipelineScope scope(PipelineKind::WarmCompile);
                scope.result(i);
            }
        });
    for (auto &t : workers)
        t.join();
    publish_pipeline_profile({});
    assert(concurrent->finish());
    report = read(concurrent->directory / "summary.json");
    assert(field(report, "acceptedSpans") + field(report, "contentionDrops") == 24000);
    assert(field(report, "inFlightAtStop") == 0);
    // An outstanding scope can safely outlive stop and publication reset.
    auto late = std::make_shared<PipelineProfile>(root / "late", "TEST");
    publish_pipeline_profile(late);
    {
        PipelineScope active(PipelineKind::CacheProbe);
        publish_pipeline_profile({});
        assert(late->finish());
        assert(field(read(late->directory / "summary.json"), "inFlightAtStop") == 1);
        late.reset();
    }
    assert(!pipeline_profile_enabled());
    std::filesystem::remove_all(root);
}
