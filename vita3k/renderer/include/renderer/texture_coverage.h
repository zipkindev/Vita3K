// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace renderer {
// All mutation is serialized by DecodedTextureCache's mutex. Fingerprints are
// diagnostic candidates only, never authorization to reuse pixels. No payloads,
// guest addresses, hashes or handles are written. Allocations occur at enable.
class TextureCoverage {
public:
    static constexpr size_t identity_limit = 32768, table_size = 65536, probe_limit = 64, detail_limit = 8192;
    enum class Disposition : uint8_t { Absent,
        Pending,
        Prepared,
        Budget,
        Deadline,
        Ambiguous };
    enum class Reason : uint8_t { Absent,
        Budget,
        Deadline,
        NotPrepared,
        PreparedEvicted,
        PreparedCleared,
        PreparedOther,
        Ambiguous,
        Untracked,
        Count };
    struct Ticket {
        uint64_t miss_qpc = 0;
        uint32_t id = 0;
        Reason reason = Reason::Untracked;
    };
    struct Identity {
        uint64_t hash = 0, requests = 0, decodes = 0, decode_ns = 0, learning_evictions = 0;
        uint32_t width = 0, height = 0, format = 0;
        Disposition disposition = Disposition::Absent;
        bool saved = false;
    };
    struct Detail {
        uint64_t sequence, miss_qpc, end_qpc, decode_ns;
        uint32_t id, width, height, format;
        Reason reason;
    };

    static_assert(identity_limit * sizeof(Identity) + table_size * sizeof(uint32_t) + detail_limit * sizeof(Detail) < 4 * 1024 * 1024);

    TextureCoverage()
        : identities(identity_limit)
        , table(table_size)
        , details(detail_limit) {
#ifdef _WIN32
        LARGE_INTEGER hz;
        if (QueryPerformanceFrequency(&hz))
            frequency = hz.QuadPart;
#else
        frequency = 1000000000;
#endif
        origin = now();
    }
    static uint64_t now() {
#ifdef _WIN32
        LARGE_INTEGER q;
        return QueryPerformanceCounter(&q) ? uint64_t(q.QuadPart) : 0;
#else
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
    }
    template <typename Key>
    uint32_t identify(const Key &key) {
        uint64_t hash = key.hash ^ (uint64_t(key.width) << 32) ^ key.height ^ (uint64_t(key.format) << 48);
        hash ^= hash >> 33;
        hash *= 0xff51afd7ed558ccdULL;
        const size_t base = (hash ^ (hash >> 33)) & (table_size - 1);
        for (size_t probe = 0; probe < probe_limit; ++probe) {
            auto &id = table[(base + probe) & (table_size - 1)];
            if (id) {
                const auto &e = identities[id - 1];
                if (e.hash == key.hash && e.width == key.width && e.height == key.height && e.format == key.format)
                    return id;
            } else {
                if (count == identity_limit) {
                    ++capacity_drops;
                    return 0;
                }
                id = uint32_t(++count);
                auto &e = identities[id - 1];
                e.hash = key.hash;
                e.width = key.width;
                e.height = key.height;
                e.format = key.format;
                return id;
            }
        }
        ++probe_drops;
        return 0;
    }
    template <typename Key>
    void register_profile(const Key &key) {
        if (auto id = identify(key)) {
            auto &e = identities[id - 1];
            e.disposition = e.disposition == Disposition::Absent ? Disposition::Pending : Disposition::Ambiguous;
        }
    }
    template <typename Key>
    void prepare(const Key &key, bool inserted) {
        if (auto id = identify(key)) {
            auto &e = identities[id - 1];
            if (e.disposition != Disposition::Ambiguous)
                e.disposition = inserted ? Disposition::Prepared : Disposition::Budget;
        }
    }
    template <typename Key>
    void budget_skip(const Key &key) {
        if (auto id = identify(key)) {
            auto &e = identities[id - 1];
            if (e.disposition != Disposition::Ambiguous)
                e.disposition = Disposition::Budget;
        }
    }
    void finish_preparation(bool deadline) {
        for (size_t i = 0; i < count; ++i)
            if (deadline && identities[i].disposition == Disposition::Pending)
                identities[i].disposition = Disposition::Deadline;
        preparation_finished = true;
    }
    template <typename Key>
    Ticket request(const Key &key, bool hit, unsigned miss_class) {
        Ticket t{ hit ? 0 : now(), identify(key), Reason::Untracked };
        if (!t.id)
            return t;
        auto &e = identities[t.id - 1];
        ++e.requests;
        if (hit)
            return t;
        switch (e.disposition) {
        case Disposition::Absent: t.reason = Reason::Absent; break;
        case Disposition::Budget: t.reason = Reason::Budget; break;
        case Disposition::Deadline: t.reason = Reason::Deadline; break;
        case Disposition::Pending: t.reason = Reason::NotPrepared; break;
        case Disposition::Ambiguous: t.reason = Reason::Ambiguous; break;
        case Disposition::Prepared:
            t.reason = miss_class == 1 ? Reason::PreparedEvicted : miss_class == 2 ? Reason::PreparedCleared
                                                                                   : Reason::PreparedOther;
            break;
        }
        return t;
    }
    void decoded(Ticket ticket, uint64_t ns, uint32_t width, uint32_t height) {
        const auto reason = static_cast<size_t>(ticket.reason);
        ++decode_counts[reason];
        decode_times[reason] += ns;
        uint32_t format = 0;
        if (ticket.id && ticket.id <= count) {
            auto &e = identities[ticket.id - 1];
            ++e.decodes;
            e.decode_ns += ns;
            format = e.format;
        }
        if (ns >= 2000000) {
            const auto sequence = detail_count++;
            details[sequence % detail_limit] = { sequence, ticket.miss_qpc, now(), ns, ticket.id, width, height, format, ticket.reason };
        }
    }
    template <typename Key>
    void learning_evicted(const Key &key) {
        if (auto id = identify(key))
            ++identities[id - 1].learning_evictions;
    }
    template <typename Key>
    void saved(const Key &key) {
        if (auto id = identify(key))
            identities[id - 1].saved = true;
    }
    // Only after the writer is joined and ownership has left the live cache.
#ifdef _MSC_VER
    __declspec(noinline)
#else
    __attribute__((noinline))
#endif
    bool write(const std::filesystem::path &prefix) const {
        auto path = [&](const char *suffix) { auto p = prefix; p += suffix; return p; };
        if (std::filesystem::exists(path("-summary.json")) || std::filesystem::exists(path("-index.csv")) || std::filesystem::exists(path("-decodes.csv")))
            return false;
        std::ofstream index(path("-index.csv")), detail(path("-decodes.csv")), summary(path("-summary.json"));
        index << "id,width,height,format,disposition,requests,decodes,decodeNs,learningEvictions,saved\n";
        for (size_t i = 0; i < count; ++i) {
            const auto &e = identities[i];
            index << i + 1 << ',' << e.width << ',' << e.height << ',' << e.format << ',' << unsigned(e.disposition) << ',' << e.requests << ',' << e.decodes << ',' << e.decode_ns << ',' << e.learning_evictions << ',' << e.saved << '\n';
        }
        detail << "sequence,missQpc,endQpc,decodeNs,id,width,height,format,reason\n";
        for (uint64_t i = detail_count > detail_limit ? detail_count - detail_limit : 0; i < detail_count; ++i) {
            const auto &d = details[i % detail_limit];
            detail << d.sequence << ',' << d.miss_qpc << ',' << d.end_qpc << ',' << d.decode_ns << ',' << d.id << ',' << d.width << ',' << d.height << ',' << d.format << ',' << unsigned(d.reason) << '\n';
        }
        summary << "{\"schema\":1,\"originQpc\":" << origin << ",\"frequency\":" << frequency
                << ",\"identityLimit\":" << identity_limit << ",\"identities\":" << count << ",\"probeLimit\":" << probe_limit
                << ",\"capacityDrops\":" << capacity_drops << ",\"probeDrops\":" << probe_drops
                << ",\"detailLimit\":" << detail_limit << ",\"slowDecodes\":" << detail_count
                << ",\"retainedDetails\":" << std::min<uint64_t>(detail_count, detail_limit)
                << ",\"overwrittenDetails\":" << (detail_count > detail_limit ? detail_count - detail_limit : 0)
                << ",\"preparationFinished\":" << (preparation_finished ? "true" : "false")
                << ",\"allocatedPayloadBytes\":" << identities.capacity() * sizeof(Identity) + table.capacity() * sizeof(uint32_t) + details.capacity() * sizeof(Detail)
                << ",\"reasonCounts\":[";
        for (size_t i = 0; i < decode_counts.size(); ++i)
            summary << (i ? "," : "") << decode_counts[i];
        summary << "],\"reasonDecodeNs\":[";
        for (size_t i = 0; i < decode_times.size(); ++i)
            summary << (i ? "," : "") << decode_times[i];
        summary << "],\"reasons\":[\"absent-fingerprint\",\"budget-skipped\",\"deadline-skipped\",\"not-prepared\",\"prepared-evicted\",\"prepared-cleared\",\"prepared-other-miss\",\"ambiguous\",\"untracked\"],"
                   "\"dispositions\":[\"absent\",\"pending\",\"prepared\",\"budget\",\"deadline\",\"ambiguous\"],"
                   "\"limits\":\"Fingerprint candidates only, not byte identity; IDs session-local. All decodes counted; detail >=2ms rolling. Miss/end QPC bound decode plus lock/observer overhead; decodeNs times decoder separately. No texture content, hashes, pointers or asset names. Saved flags require successful normal-close profile replacement.\"}\n";
        index.flush();
        detail.flush();
        summary.flush();
        return bool(index) && bool(detail) && bool(summary);
    }

private:
    std::vector<Identity> identities;
    std::vector<uint32_t> table;
    std::vector<Detail> details;
    std::array<uint64_t, static_cast<size_t>(Reason::Count)> decode_counts{}, decode_times{};
    size_t count = 0;
    uint64_t origin = 0, frequency = 0, capacity_drops = 0, probe_drops = 0, detail_count = 0;
    bool preparation_finished = false;
};
} // namespace renderer
