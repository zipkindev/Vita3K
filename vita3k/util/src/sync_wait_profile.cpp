// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <util/sync_wait_profile.h>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif
namespace util {
namespace {
std::atomic<std::shared_ptr<SyncWaitProfile>> current;
std::atomic<bool> enabled{ false };
uint64_t clock_frequency() noexcept {
#ifdef _WIN32
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
#else
    return 1000000000;
#endif
}
uint32_t native_tid() noexcept {
#ifdef _WIN32
    return GetCurrentThreadId();
#elif defined(__linux__)
    return static_cast<uint32_t>(syscall(SYS_gettid));
#else
    return static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}
void add(SyncWaitProfile::Stats &s, uint64_t ticks, uint64_t f) noexcept {
    ++s.count;
    s.ticks += ticks;
    s.max_ticks = std::max(s.max_ticks, ticks);
    s.over_2ms += ticks >= f / 500;
    s.over_10ms += ticks >= f / 100;
    s.over_50ms += ticks >= f / 20;
}
void merge(SyncWaitProfile::Stats &a, const SyncWaitProfile::Stats &b) noexcept {
    a.count += b.count;
    a.ticks += b.ticks;
    a.max_ticks = std::max(a.max_ticks, b.max_ticks);
    a.over_2ms += b.over_2ms;
    a.over_10ms += b.over_10ms;
    a.over_50ms += b.over_50ms;
}
void csv_stats(std::ostream &out, const SyncWaitProfile::Stats &s) {
    out << s.count << ',' << s.ticks << ',' << s.max_ticks << ',' << s.over_2ms << ',' << s.over_10ms << ',' << s.over_50ms;
}
constexpr const char *names[] = { "guest_primitive", "renderer_queue", "renderer_status", "renderer_sync", "vulkan_acquire", "vulkan_fence", "vulkan_present", "vulkan_frame_ready" };
} // namespace
uint64_t SyncWaitProfile::counter() noexcept {
#ifdef _WIN32
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return q.QuadPart;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
std::filesystem::path fresh_diagnostic_path(const std::filesystem::path &cache, const char *kind) {
    static std::atomic<uint64_t> sequence{ 0 };
    const auto parent = cache / "diagnostics" / kind;
    std::filesystem::create_directories(parent);
    return parent / (std::to_string(SyncWaitProfile::counter()) + "-" + std::to_string(sequence.fetch_add(1)));
}
SyncWaitProfile::SyncWaitProfile(std::filesystem::path output, std::string title, size_t capacity)
    : directory(std::move(output))
    , frequency(clock_frequency())
    , origin_qpc(counter())
    , bucket_width(frequency) {
    if (!capacity || capacity > record_limit || title.size() > 32
        || title.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
        throw std::runtime_error("Invalid synchronization profile bounds/title");
    records.resize(capacity);
    if (!std::filesystem::create_directory(directory))
        throw std::runtime_error("Synchronization diagnostic directory must be fresh");
    std::ofstream out(directory / "armed.json", std::ios::binary);
    out << "{\"schema\":1,\"status\":\"armed\",\"title\":\"" << title
        << "\",\"clockFrequency\":" << frequency << ",\"originQpc\":" << origin_qpc
        << ",\"maxRecords\":" << capacity << ",\"recordBytes\":" << sizeof(Record)
        << ",\"bufferBytes\":" << capacity * sizeof(Record) << ",\"maxSessionBuckets\":" << bucket_limit
        << ",\"sessionBucketBytes\":" << sizeof(buckets) << ",\"detailThresholdUs\":2000,\"retention\":\"rolling\""
        << ",\"scope\":\"completed API spans; elapsed includes wake-up/relock; nested spans overlap; not OS sleep time\",\"kinds\":[";
    for (size_t i = 0; i < std::size(names); ++i) {
        if (i)
            out << ',';
        out << '"' << names[i] << '"';
    }
    out << "]}\n";
    out.close();
    if (!out)
        throw std::runtime_error("Cannot write synchronization profile header");
}
void SyncWaitProfile::append(WaitKind kind, const WaitContext &c, uint64_t start, uint64_t end,
    uint32_t tid, int32_t code, bool known) noexcept {
    const std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock) {
        contention_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!accepting.load(std::memory_order_relaxed) || end < start || start < origin_qpc || kind >= WaitKind::Count)
        return;
    const auto duration = end - start;
    add(kinds[static_cast<size_t>(kind)], duration, frequency);
    // Summaries assign spans to their completion time; detail retains both endpoints.
    while ((end - origin_qpc) / bucket_width >= bucket_limit) {
        for (size_t i = 0; i < bucket_limit / 2; ++i) {
            buckets[i] = buckets[2 * i];
            merge(buckets[i].stats, buckets[2 * i + 1].stats);
            buckets[i].index = i;
        }
        for (size_t i = bucket_limit / 2; i < bucket_limit; ++i)
            buckets[i] = {};
        bucket_width *= 2;
    }
    const auto index = (end - origin_qpc) / bucket_width;
    buckets[index].index = index;
    add(buckets[index].stats, duration, frequency);
    ++accepted;
    if (duration >= frequency / 500) {
        records[detailed % records.size()] = { detailed + 1, start, end, c.object, c.related, c.timeout_us,
            code, tid, c.guest_tid, c.guest_pc, c.import_nid, c.import_lr, static_cast<uint32_t>(kind), known ? 1u : 0u };
        ++detailed;
    }
}
bool SyncWaitProfile::finish() noexcept {
    const std::lock_guard<std::mutex> lock(mutex);
    if (finished)
        return saved;
    accepting.store(false, std::memory_order_release);
    finished = true;
    const auto stopped = counter(), outstanding = in_flight.load(std::memory_order_acquire);
    try {
        std::ofstream detail(directory / "waits.csv", std::ios::binary);
        detail << "sequence,startQpc,endQpc,object,related,timeoutUs,result,nativeTid,guestTid,guestPc,importNid,importLr,kind,resultKnown\n";
        const auto retained = std::min<uint64_t>(detailed, records.size());
        for (uint64_t n = detailed - retained; n < detailed; ++n) {
            const auto &r = records[n % records.size()];
            detail << r.sequence << ',' << r.start_qpc << ',' << r.end_qpc << ',' << r.object << ',' << r.related << ',' << r.timeout_us << ',' << r.result << ',' << r.native_tid << ',' << r.guest_tid << ',' << r.guest_pc << ',' << r.import_nid << ',' << r.import_lr << ',' << r.kind << ',' << r.result_known << '\n';
        }
        std::ofstream session(directory / "session.csv", std::ios::binary);
        session << "startQpc,endQpc,count,ticks,maxTicks,over2ms,over10ms,over50ms\n";
        for (size_t i = 0; i < bucket_limit; ++i) {
            const auto &b = buckets[i];
            if (!b.stats.count)
                continue;
            session << origin_qpc + i * bucket_width << ',' << origin_qpc + (i + 1) * bucket_width << ',';
            csv_stats(session, b.stats);
            session << '\n';
        }
        std::ofstream totals(directory / "kinds.csv", std::ios::binary);
        totals << "kind,name,count,ticks,maxTicks,over2ms,over10ms,over50ms\n";
        for (size_t i = 0; i < kinds.size(); ++i) {
            totals << i << ',' << names[i] << ',';
            csv_stats(totals, kinds[i]);
            totals << '\n';
        }
        std::ofstream summary(directory / "summary.json", std::ios::binary);
        summary << "{\"schema\":1,\"acceptedSpans\":" << accepted << ",\"detailedSpans\":" << detailed
                << ",\"retainedRecords\":" << retained << ",\"overwrittenRecords\":" << detailed - retained
                << ",\"contentionDrops\":" << contention_drops.load() << ",\"inFlightAtStop\":" << outstanding
                << ",\"stopQpc\":" << stopped << ",\"bucketWidthQpc\":" << bucket_width
                << ",\"detailThresholdUs\":2000,\"summaryAssignment\":\"completion time\",\"coverage\":\"selected APIs only; drops and in-flight spans excluded; nested/concurrent spans overlap\"}\n";
        detail.close();
        session.close();
        totals.close();
        summary.close();
        saved = detail && session && totals && summary;
    } catch (...) {
        saved = false;
    }
    return saved;
}
void publish_sync_wait_profile(std::shared_ptr<SyncWaitProfile> profile) noexcept {
    enabled.store(false, std::memory_order_release);
    current.store(std::move(profile), std::memory_order_release);
    enabled.store(bool(current.load(std::memory_order_acquire)), std::memory_order_release);
}
bool sync_wait_profile_enabled() noexcept { return enabled.load(std::memory_order_relaxed); }
SyncWaitScope::SyncWaitScope(WaitKind k, WaitContext c) noexcept
    : kind(k)
    , context(c) {
    if (!sync_wait_profile_enabled())
        return;
    auto candidate = current.load(std::memory_order_acquire);
    if (!candidate)
        return;
    candidate->in_flight.fetch_add(1, std::memory_order_acq_rel);
    if (!candidate->accepting.load(std::memory_order_acquire)) {
        candidate->in_flight.fetch_sub(1, std::memory_order_release);
        return;
    }
    profile = std::move(candidate);
    tid = native_tid();
    start = SyncWaitProfile::counter();
}
SyncWaitScope::~SyncWaitScope() noexcept {
    if (!profile)
        return;
    const auto end = SyncWaitProfile::counter();
    profile->append(kind, context, start, end, tid, code, known);
    profile->in_flight.fetch_sub(1, std::memory_order_release);
}
} // namespace util
