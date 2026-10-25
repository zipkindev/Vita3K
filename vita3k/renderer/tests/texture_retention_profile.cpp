// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <map>
#include <renderer/decoded_texture_cache.h>
#include <sstream>
#include <string>
#include <thread>

using Cache = renderer::DecodedTextureCache;
using Profile = renderer::TextureRetentionProfile;
using Row = std::map<std::string, uint64_t>;
static std::vector<Row> read_csv(const std::filesystem::path &path) {
    std::ifstream file(path);
    std::string line, cell;
    std::getline(file, line);
    std::vector<std::string> keys;
    std::istringstream header(line);
    while (std::getline(header, cell, ','))
        keys.push_back(cell);
    std::vector<Row> rows;
    while (std::getline(file, line)) {
        std::istringstream values(line);
        Row row;
        size_t i = 0;
        while (std::getline(values, cell, ','))
            row[keys.at(i++)] = std::stoull(cell);
        assert(i == keys.size());
        rows.push_back(row);
    }
    return rows;
}
static std::string read(const std::filesystem::path &path) {
    std::ifstream f(path);
    return { std::istreambuf_iterator<char>(f), {} };
}
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("vita-retention-" + std::to_string(renderer::TextureCoverage::now()));
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() { std::filesystem::remove_all(p); }
    } cleanup{ root };
    const auto key = [](uint64_t i) { return Cache::Key{ i, 8, 8, 0 }; };
    const std::vector<uint8_t> source{ 1, 2, 3, 4 }, pixels{ 9, 8, 7, 6 };
    Cache plain(64, 8), observed(64, 8);
    observed.enable_coverage();
    for (auto *c : { &plain, &observed }) {
        for (uint64_t i = 0; i < 8; ++i) {
            c->coverage_profile(key(i));
            assert(c->insert(key(i), source, pixels, 42, 10000000 - i, false));
            c->coverage_prepared(key(i), true);
        }
        c->coverage_finish_preparation(false);
        assert(c->insert(key(8), source, pixels, 42, 1000));
        auto output = pixels;
        uint32_t result = 0;
        assert(c->copy(key(8), source, output, result) && output == pixels && result == 42);
        assert(c->insert(key(9), source, pixels, 42, 1000));
        assert(c->insert(key(10), source, pixels, 42, 1000));
        Cache::Observation miss;
        assert(!c->copy(key(5), source, output, result, &miss));
        auto collision = source;
        collision[0] ^= 1;
        assert(!c->copy(key(0), collision, output, result, &miss) && output == pixels);
        std::vector<uint8_t> wrong_size(5, 17);
        assert(!c->copy(key(0), source, wrong_size, result, &miss) && wrong_size == std::vector<uint8_t>(5, 17));
    }
    const auto a = plain.stats(), b = observed.stats();
    assert(a.requests == b.requests && a.hits == b.hits && a.misses == b.misses && a.evictions == b.evictions);
    assert(a.retained_bytes == b.retained_bytes && a.retention_promotions == b.retention_promotions && a.retention_demotions == b.retention_demotions);
    observed.set_budget(72);
    assert(observed.insert(key(20), source, pixels, 1, 1000));
    observed.clear();
    auto coverage = observed.take_coverage();
    assert(coverage->write(root / "cache"));
    auto events = read_csv(root / "cache-retention-events.csv");
    uint64_t demote_serial = 0, evict_serial = 0;
    bool demote = false, evict = false, collision = false, size = false, budget = false, clear = false;
    for (const auto &e : events) {
        const auto k = static_cast<Profile::Kind>(e.at("kind"));
        if (e.at("id") == 6 && k == Profile::Kind::Demote) {
            assert(e.at("triggerId") == 9 && e.at("cause") == static_cast<unsigned>(Profile::Cause::RetainedBoth));
            assert((e.at("flags") & 3) == 3 && !(e.at("flags") & 4));
            demote_serial = e.at("serial");
            demote = true;
        }
        if (e.at("id") == 6 && k == Profile::Kind::Evict) {
            assert(e.at("triggerId") == 11 && e.at("cause") == static_cast<unsigned>(Profile::Cause::Both));
            assert((e.at("flags") & 3) == 1);
            evict_serial = e.at("serial");
            evict = true;
        }
        if (k == Profile::Kind::Miss && (e.at("flags") & 8)) {
            assert(e.at("candidates") == 1);
            collision = true;
        }
        if (k == Profile::Kind::Miss && (e.at("flags") & 16)) {
            assert(e.at("candidates") == 1);
            size = true;
        }
        if (k == Profile::Kind::Clear && e.at("cause") == static_cast<unsigned>(Profile::Cause::BudgetChange))
            budget = true;
        if (k == Profile::Kind::Clear && e.at("cause") == static_cast<unsigned>(Profile::Cause::Clear))
            clear = true;
    }
    assert(demote && evict && demote_serial == evict_serial && collision && size && budget && clear);
    const auto index = read_csv(root / "cache-retention-index.csv");
    const auto it = std::find_if(index.begin(), index.end(), [](const auto &e) { return e.at("id") == 6; });
    assert(it != index.end() && it->at("preloads") == 1 && it->at("hits") == 0 && it->at("misses") == 1);
    assert(it->at("lastDemoteTriggerId") == 9 && it->at("lastEvictTriggerId") == 11);
    assert(!coverage->retention.write(root / "cache", 1));
    // Concurrent cache writers still serialize observer mutation under its
    // existing mutex. Every successful read must return validated pixels.
    Cache concurrent(1024, 16);
    concurrent.enable_coverage();
    assert(concurrent.insert(key(0), source, pixels, 42, 1000));
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) {
                auto out = pixels;
                uint32_t value = 0;
                assert(concurrent.copy(key(0), source, out, value) && out == pixels && value == 42);
            }
        });
    for (auto &thread : threads)
        thread.join();
    auto concurrent_report = concurrent.take_coverage();
    assert(concurrent_report->write(root / "concurrent"));
    const auto concurrent_index = read_csv(root / "concurrent-retention-index.csv");
    assert(concurrent_index.size() == 1 && concurrent_index[0].at("hits") == 4000);
    Cache attached(64, 8);
    assert(attached.insert(key(42), source, pixels, 42, 3000000));
    attached.enable_coverage();
    auto attached_output = pixels;
    uint32_t attached_result = 0;
    assert(attached.copy(key(42), source, attached_output, attached_result));
    assert(attached_result == 42 && attached_output == pixels);
    auto attached_report = attached.take_coverage();
    assert(attached_report->write(root / "attached"));
    const auto attached_events = read_csv(root / "attached-retention-events.csv");
    assert(attached_events[0].at("kind") == static_cast<unsigned>(Profile::Kind::Attach));
    assert(attached_events[0].at("id") == 1 && attached_events[0].at("serial") == 1 && !(attached_events[0].at("flags") & 1));
    // Ring overwrite must not discard bounded per-identity causal summaries.
    Profile profile;
    Profile::Record record;
    record.id = 1;
    record.kind = static_cast<unsigned>(Profile::Kind::Demote);
    record.trigger_id = 2;
    record.trigger_serial = 99;
    record.qpc = 10;
    profile.append(record);
    record.id = 0;
    record.kind = static_cast<unsigned>(Profile::Kind::Miss);
    for (size_t i = 0; i < Profile::record_limit + 10; ++i)
        profile.append(record);
    assert(profile.write(root / "bounded", 1000000000));
    assert(read_csv(root / "bounded-retention-events.csv").size() == Profile::record_limit);
    const auto latest = read_csv(root / "bounded-retention-index.csv");
    assert(latest.size() == 1 && latest[0].at("demotions") == 1 && latest[0].at("lastDemoteTrigger") == 99);
    const auto summary = read(root / "bounded-retention-summary.json");
    assert(summary.find("\"overwrittenRecords\":11") != std::string::npos);
    assert(summary.find("\"untrackedEvents\":65546") != std::string::npos);
}
