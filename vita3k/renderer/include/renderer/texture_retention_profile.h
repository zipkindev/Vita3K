// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace renderer {
// Serialized by the cache mutex; allocated at optional coverage activation.
// Identity IDs refer to coverage fingerprints; serials distinguish actual cache
// admissions even if their fingerprints collide. Neither authorizes pixel reuse.
class TextureRetentionProfile {
public:
    static constexpr size_t record_limit = 65536, identity_limit = 32768;
    enum class Kind : uint32_t { Preload,
        Demand,
        FirstHit,
        Promote,
        Demote,
        Evict,
        Miss,
        Reject,
        Clear,
        Attach,
        Count };
    enum class Cause : uint32_t { None,
        Bytes,
        Entries,
        Both,
        RetainedBytes,
        RetainedEntries,
        RetainedBoth,
        Oversized,
        Clear,
        BudgetChange,
        Duplicate,
        Ineligible,
        PreparationFull,
        AdmissionValue,
        AdmissionCapacity,
        AdmissionScanLimit };
    struct Record {
        uint64_t sequence = 0, qpc = 0, serial = 0, trigger_serial = 0, bytes = 0, trigger_bytes = 0;
        uint64_t decode_ns = 0, uses = 0, resident = 0, retained = 0, candidates = 0;
        uint32_t id = 0, trigger_id = 0, kind = 0, cause = 0, flags = 0, entries = 0, retained_entries = 0;
        uint32_t width = 0, height = 0, format = 0;
    };
    static_assert(sizeof(Record) == 128);
    struct Identity {
        uint64_t preloads = 0, demands = 0, hits = 0, misses = 0, promotions = 0, demotions = 0, evictions = 0, clears = 0;
        uint64_t first_preload_qpc = 0, first_preload_serial = 0, first_demand_qpc = 0, last_demand_qpc = 0;
        uint64_t last_demote_qpc = 0, last_evict_qpc = 0, last_demote_trigger = 0, last_evict_trigger = 0;
        uint32_t last_demote_trigger_id = 0, last_evict_trigger_id = 0, last_demote_cause = 0, last_evict_cause = 0;
    };
    static_assert(sizeof(Identity) == 144);
    TextureRetentionProfile()
        : records(record_limit)
        , identities(identity_limit) {}
    uint64_t next_serial() { return ++serial; }
    void hit(uint32_t id, uint64_t qpc) {
        if (id && id <= identity_limit) {
            auto &s = identities[id - 1];
            ++s.hits;
            if (!s.first_demand_qpc)
                s.first_demand_qpc = qpc;
            s.last_demand_qpc = qpc;
        } else
            ++untracked_hits;
    }
    void append(Record r) {
        r.sequence = count++;
        records[r.sequence % record_limit] = r;
        ++kinds[r.kind];
        if (!r.id || r.id > identity_limit) {
            ++untracked;
            return;
        }
        auto &s = identities[r.id - 1];
        switch (static_cast<Kind>(r.kind)) {
        case Kind::Preload:
            ++s.preloads;
            if (!s.first_preload_qpc) {
                s.first_preload_qpc = r.qpc;
                s.first_preload_serial = r.serial;
            }
            break;
        case Kind::Demand: ++s.demands; break;
        case Kind::Promote: ++s.promotions; break;
        case Kind::Demote:
            ++s.demotions;
            s.last_demote_qpc = r.qpc;
            s.last_demote_trigger = r.trigger_serial;
            s.last_demote_trigger_id = r.trigger_id;
            s.last_demote_cause = r.cause;
            break;
        case Kind::Evict:
            ++s.evictions;
            s.last_evict_qpc = r.qpc;
            s.last_evict_trigger = r.trigger_serial;
            s.last_evict_trigger_id = r.trigger_id;
            s.last_evict_cause = r.cause;
            break;
        case Kind::Miss:
            ++s.misses;
            if (!s.first_demand_qpc)
                s.first_demand_qpc = r.qpc;
            s.last_demand_qpc = r.qpc;
            break;
        case Kind::Clear: ++s.clears; break;
        default: break;
        }
    }
#ifdef _MSC_VER
    __declspec(noinline)
#else
    __attribute__((noinline))
#endif
    bool
    write(const std::filesystem::path &prefix, uint64_t frequency) const {
        auto path = [&](const char *suffix) { auto p = prefix; p += suffix; return p; };
        if (std::filesystem::exists(path("-retention-events.csv")) || std::filesystem::exists(path("-retention-index.csv")) || std::filesystem::exists(path("-retention-summary.json")))
            return false;
        std::ofstream events(path("-retention-events.csv")), index(path("-retention-index.csv")), summary(path("-retention-summary.json"));
        events << "sequence,qpc,serial,triggerSerial,bytes,triggerBytes,decodeNs,uses,residentBytes,retainedBytes,candidates,id,triggerId,kind,cause,flags,entries,retainedEntries,width,height,format\n";
        for (uint64_t i = count > record_limit ? count - record_limit : 0; i < count; ++i) {
            const auto &r = records[i % record_limit];
            events << r.sequence << ',' << r.qpc << ',' << r.serial << ',' << r.trigger_serial << ',' << r.bytes << ',' << r.trigger_bytes << ',' << r.decode_ns << ',' << r.uses << ',' << r.resident << ',' << r.retained << ',' << r.candidates << ',' << r.id << ',' << r.trigger_id << ',' << r.kind << ',' << r.cause << ',' << r.flags << ',' << r.entries << ',' << r.retained_entries << ',' << r.width << ',' << r.height << ',' << r.format << '\n';
        }
        index << "id,preloads,demands,hits,misses,promotions,demotions,evictions,clears,firstPreloadQpc,firstPreloadSerial,firstDemandQpc,lastDemandQpc,lastDemoteQpc,lastEvictQpc,lastDemoteTrigger,lastEvictTrigger,lastDemoteTriggerId,lastEvictTriggerId,lastDemoteCause,lastEvictCause\n";
        for (size_t i = 0; i < identities.size(); ++i) {
            const auto &s = identities[i];
            if (!(s.preloads || s.demands || s.hits || s.misses || s.promotions || s.demotions || s.evictions || s.clears))
                continue;
            index << i + 1 << ',' << s.preloads << ',' << s.demands << ',' << s.hits << ',' << s.misses << ',' << s.promotions << ',' << s.demotions << ',' << s.evictions << ',' << s.clears << ',' << s.first_preload_qpc << ',' << s.first_preload_serial << ',' << s.first_demand_qpc << ',' << s.last_demand_qpc << ',' << s.last_demote_qpc << ',' << s.last_evict_qpc << ',' << s.last_demote_trigger << ',' << s.last_evict_trigger << ',' << s.last_demote_trigger_id << ',' << s.last_evict_trigger_id << ',' << s.last_demote_cause << ',' << s.last_evict_cause << '\n';
        }
        summary << "{\"schema\":1,\"frequency\":" << frequency << ",\"recordLimit\":" << record_limit << ",\"recordBytes\":" << sizeof(Record)
                << ",\"identityLimit\":" << identity_limit << ",\"identityBytes\":" << sizeof(Identity)
                << ",\"allocatedPayloadBytes\":" << records.capacity() * sizeof(Record) + identities.capacity() * sizeof(Identity)
                << ",\"events\":" << count << ",\"retainedRecords\":" << std::min<uint64_t>(count, record_limit)
                << ",\"overwrittenRecords\":" << (count > record_limit ? count - record_limit : 0)
                << ",\"admissionSerials\":" << serial << ",\"untrackedEvents\":" << untracked << ",\"untrackedHits\":" << untracked_hits
                << ",\"kindCounts\":[";
        for (size_t i = 0; i < kinds.size(); ++i)
            summary << (i ? "," : "") << kinds[i];
        summary << "],\"kinds\":[\"preload\",\"demand\",\"first-hit\",\"promote\",\"demote\",\"evict\",\"miss\",\"reject\",\"clear\",\"attach\"],"
                   "\"causes\":[\"none\",\"total-bytes\",\"total-entries\",\"total-both\",\"retained-bytes\",\"retained-entries\",\"retained-both\",\"oversized\",\"clear\",\"budget-change\",\"duplicate\",\"ineligible\",\"preparation-full\",\"admission-value\",\"admission-capacity\",\"admission-scan-limit\"],"
                   "\"flags\":\"1 preloaded,2 retained,4 used by demand,8 rejected source bytes,16 rejected output/source size\","
                   "\"limits\":\"Cache mutex serialized, no extra lock or allocation per event. State sampled before transition; hit/promotion after uses increment. Admission event follows room-making and precedes retention. IDs share coverage fingerprint namespace and its drops/collisions. Serials identify admissions, not content; trigger links connect demotion/eviction to admission or hit. First hit only in detail; all hits update bounded identity totals. Latest demotion/eviction summaries survive ring overwrite. No texture bytes, hashes, pointers or guest addresses.\"}\n";
        events.flush();
        index.flush();
        summary.flush();
        return bool(events) && bool(index) && bool(summary);
    }

private:
    std::vector<Record> records;
    std::vector<Identity> identities;
    std::array<uint64_t, static_cast<size_t>(Kind::Count)> kinds{};
    uint64_t count = 0, serial = 0, untracked = 0, untracked_hits = 0;
};
} // namespace renderer
