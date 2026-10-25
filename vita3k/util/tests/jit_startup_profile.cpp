// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <fstream>
#include <sstream>
#include <thread>
#include <util/jit_startup_profile.h>
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
    static_assert(sizeof(JitStartupProfile::Record) == 96);
    assert(!jit_startup_profile_enabled());
    {
        JitStartupScope off(JitStartupKind::JitCreate);
        assert(off.finish(-7) == -7);
    }
    const auto root = std::filesystem::temp_directory_path() / ("vita3k-jit-startup-" + std::to_string(JitStartupProfile::counter()));
    std::filesystem::create_directories(root);
    auto p = std::make_shared<JitStartupProfile>(root / "ring", "TEST", 4);
    bool rejected = false;
    try {
        JitStartupProfile duplicate(root / "ring", "TEST");
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
    const auto tick = p->frequency / 100; // 10ms
    for (int n = 0; n < 12; ++n)
        p->append(JitStartupKind::JitCreate, { 1, 2, 3 }, p->origin_qpc + n * tick, p->origin_qpc + (n + 1) * tick, 42, -8, true);
    // Keep short startup events too; the bounded ring rolls.
    p->append(JitStartupKind::CpuInit, {}, p->origin_qpc, p->origin_qpc + 1, 43, 0, false);
    // Fold beyond the original 4096 seconds without sleeping or growing memory.
    p->append(JitStartupKind::DispatchClear, {}, p->origin_qpc + p->frequency * 5000, p->origin_qpc + p->frequency * 5001, 44, 0, true);
    assert(p->finish() && p->finish());
    auto report = read(p->directory / "summary.json");
    assert(field(report, "acceptedSpans") == 14 && field(report, "retainedRecords") == 4);
    assert(field(report, "detailedSpans") == 14 && field(report, "overwrittenRecords") == 10);
    assert(field(report, "bucketWidthQpc") == 2 * p->frequency);
    const auto detail = read(p->directory / "startup.csv");
    assert(detail.find("11,") != std::string::npos);
    // Guest ID and sequential CPU instance are retained, no native pointer.
    assert(detail.find("sequence,startQpc,endQpc,guestThread,cpuInstance") == 0);
    assert(field(read(p->directory / "armed.json"), "recordBytes") == 96);
    assert(read(p->directory / "kinds.csv").find("dispatch_clear") != std::string::npos);
    std::istringstream lines(detail);
    std::string line, token;
    std::getline(lines, line); // CSV header
    std::getline(lines, line);
    std::istringstream row(line);
    for (int column = 0; column < 4; ++column)
        std::getline(row, token, ',');
    assert(std::stoull(token) == uint64_t(1));
    assert(read(p->directory / "kinds.csv").find("knownResults,nonzeroResults") != std::string::npos);
    auto concurrent = std::make_shared<JitStartupProfile>(root / "concurrent", "TEST");
    publish_jit_startup_profile(concurrent);
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t)
        workers.emplace_back([] {
            for (int i = 0; i < 3000; ++i) {
                JitStartupScope scope(JitStartupKind::JitCreate);
                scope.result(i);
            }
        });
    for (auto &t : workers)
        t.join();
    publish_jit_startup_profile({});
    assert(concurrent->finish());
    report = read(concurrent->directory / "summary.json");
    assert(field(report, "acceptedSpans") + field(report, "contentionDrops") == 24000);
    assert(field(report, "inFlightAtStop") == 0);
    // An outstanding scope can safely outlive stop and publication reset.
    auto late = std::make_shared<JitStartupProfile>(root / "late", "TEST");
    publish_jit_startup_profile(late);
    {
        JitStartupScope active(JitStartupKind::CpuInit);
        publish_jit_startup_profile({});
        assert(late->finish());
        assert(field(read(late->directory / "summary.json"), "inFlightAtStop") == 1);
        late.reset();
    }
    assert(!jit_startup_profile_enabled());
    std::filesystem::remove_all(root);
}
