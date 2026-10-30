// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <fstream>
#include <thread>
#include <util/sync_wait_profile.h>
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
    static_assert(sizeof(SyncWaitProfile::Record) == 80);
    assert(!sync_wait_profile_enabled());
    {
        SyncWaitScope off(WaitKind::GuestPrimitive);
        assert(off.finish(-7) == -7);
    }
    const auto root = std::filesystem::temp_directory_path() / ("vita3k-waits-" + std::to_string(SyncWaitProfile::counter()));
    std::filesystem::create_directories(root);
    auto p = std::make_shared<SyncWaitProfile>(root / "ring", "TEST", 4);
    bool rejected = false;
    try {
        SyncWaitProfile duplicate(root / "ring", "TEST");
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
    const auto tick = p->frequency / 100; // 10ms
    for (int n = 0; n < 12; ++n)
        p->append(WaitKind::GuestPrimitive, { 1, 2, 3, 4, 5, 6, 7 }, p->origin_qpc + n * tick, p->origin_qpc + (n + 1) * tick, 42, -8, true);
    // Short spans contribute summaries but not detail.
    p->append(WaitKind::RendererStatus, {}, p->origin_qpc, p->origin_qpc + 1, 43, 0, false);
    // Fold beyond the original 4096 seconds without sleeping or growing memory.
    p->append(WaitKind::VulkanFence, {}, p->origin_qpc + p->frequency * 5000, p->origin_qpc + p->frequency * 5001, 44, 0, true);
    assert(p->finish() && p->finish());
    auto report = read(p->directory / "summary.json");
    assert(field(report, "acceptedSpans") == 14 && field(report, "retainedRecords") == 4);
    assert(field(report, "detailedSpans") == 13 && field(report, "overwrittenRecords") == 9);
    assert(field(report, "bucketWidthQpc") == 2 * p->frequency);
    const auto detail = read(p->directory / "waits.csv");
    assert(detail.find("10,") != std::string::npos);
    auto concurrent = std::make_shared<SyncWaitProfile>(root / "concurrent", "TEST");
    publish_sync_wait_profile(concurrent);
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t)
        workers.emplace_back([] {
            for (int i = 0; i < 3000; ++i) {
                SyncWaitScope scope(WaitKind::RendererQueue);
                scope.result(i);
            }
        });
    for (auto &t : workers)
        t.join();
    publish_sync_wait_profile({});
    assert(concurrent->finish());
    report = read(concurrent->directory / "summary.json");
    assert(field(report, "acceptedSpans") + field(report, "contentionDrops") == 24000);
    assert(field(report, "inFlightAtStop") == 0);
    // An outstanding scope can safely outlive stop and publication reset.
    auto late = std::make_shared<SyncWaitProfile>(root / "late", "TEST");
    publish_sync_wait_profile(late);
    {
        SyncWaitScope active(WaitKind::VulkanPresent);
        publish_sync_wait_profile({});
        assert(late->finish());
        assert(field(read(late->directory / "summary.json"), "inFlightAtStop") == 1);
        late.reset();
    }
    assert(!sync_wait_profile_enabled());
    std::filesystem::remove_all(root);
}
