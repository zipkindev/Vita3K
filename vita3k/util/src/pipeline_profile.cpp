// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <util/pipeline_profile.h>
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
std::atomic<std::shared_ptr<PipelineProfile>> current;
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
void add(PipelineProfile::Stats &s, uint64_t ticks, uint64_t f, bool known, int32_t result) noexcept {
    ++s.count;
    s.known_results += known;
    s.nonzero_results += known && result != 0;
    s.ticks += ticks;
    s.max_ticks = std::max(s.max_ticks, ticks);
    s.over_2ms += ticks >= f / 500;
    s.over_10ms += ticks >= f / 100;
    s.over_50ms += ticks >= f / 20;
}
void merge(PipelineProfile::Stats &a, const PipelineProfile::Stats &b) noexcept {
    a.count += b.count;
    a.known_results += b.known_results;
    a.nonzero_results += b.nonzero_results;
    a.ticks += b.ticks;
    a.max_ticks = std::max(a.max_ticks, b.max_ticks);
    a.over_2ms += b.over_2ms;
    a.over_10ms += b.over_10ms;
    a.over_50ms += b.over_50ms;
}
void csv_stats(std::ostream &out, const PipelineProfile::Stats &s) {
    out << s.count << ',' << s.ticks << ',' << s.max_ticks << ',' << s.over_2ms << ',' << s.over_10ms << ',' << s.over_50ms << ',' << s.known_results << ',' << s.nonzero_results;
}
constexpr const char *names[] = { "async_queued", "async_skipped", "failed_cached", "queue_delay", "async_compile", "queue_cancelled", "protected_wait", "sync_disabled", "sync_known_cache", "sync_protected", "cache_probe", "warm_queue_delay", "warm_skip_drawn", "warm_missing_shader", "warm_probe", "warm_compile", "warm_total" };
static_assert(std::size(names) == static_cast<size_t>(PipelineKind::Count));
} // namespace
uint64_t PipelineProfile::counter() noexcept {
#ifdef _WIN32
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return q.QuadPart;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
PipelineProfile::PipelineProfile(std::filesystem::path output, std::string title, size_t capacity)
    : directory(std::move(output))
    , frequency(clock_frequency())
    , origin_qpc(counter())
    , bucket_width(frequency) {
    if (!capacity || capacity > record_limit || title.size() > 32
        || title.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
        throw std::runtime_error("Invalid pipeline profile bounds/title");
    records.resize(capacity);
    if (!std::filesystem::create_directory(directory))
        throw std::runtime_error("Pipeline diagnostic directory must be fresh");
    std::ofstream out(directory / "armed.json", std::ios::binary);
    out << "{\"schema\":1,\"status\":\"armed\",\"title\":\"" << title
        << "\",\"clockFrequency\":" << frequency << ",\"originQpc\":" << origin_qpc
        << ",\"maxRecords\":" << capacity << ",\"recordBytes\":" << sizeof(Record)
        << ",\"bufferBytes\":" << capacity * sizeof(Record) << ",\"maxSessionBuckets\":" << bucket_limit
        << ",\"sessionBucketBytes\":" << sizeof(buckets) << ",\"detailThresholdUs\":2000,\"retention\":\"rolling\""
        << ",\"scope\":\"selected pipeline work; excludes ready hits; elapsed includes contention; nested spans overlap; session-local opaque IDs; no shaders or handles\",\"kinds\":[";
    for (size_t i = 0; i < std::size(names); ++i) {
        if (i)
            out << ',';
        out << '"' << names[i] << '"';
    }
    out << "]}\n";
    out.close();
    if (!out)
        throw std::runtime_error("Cannot write pipeline profile header");
}
void PipelineProfile::append(PipelineKind kind, const PipelineContext &c, uint64_t start, uint64_t end,
    uint32_t tid, int32_t code, bool known) noexcept {
    const std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock) {
        contention_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!accepting.load(std::memory_order_relaxed) || end < start || start < origin_qpc || kind >= PipelineKind::Count)
        return;
    const auto duration = end - start;
    add(kinds[static_cast<size_t>(kind)], duration, frequency, known, code);
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
    add(buckets[index].stats, duration, frequency, known, code);
    ++accepted;
    if (duration >= frequency / 500) {
        records[detailed % records.size()] = { detailed + 1, start, end, c.pipeline ? c.pipeline ^ origin_qpc : 0, c.pair ? c.pair ^ origin_qpc : 0,
            code, tid, static_cast<uint32_t>(kind), known ? 1u : 0u, c.reason };
        ++detailed;
    }
}
bool PipelineProfile::finish() noexcept {
    const std::lock_guard<std::mutex> lock(mutex);
    if (finished)
        return saved;
    accepting.store(false, std::memory_order_release);
    finished = true;
    const auto stopped = counter(), outstanding = in_flight.load(std::memory_order_acquire);
    try {
        std::ofstream detail(directory / "pipelines.csv", std::ios::binary);
        detail << "sequence,startQpc,endQpc,pipeline,pair,result,nativeTid,kind,resultKnown,reason\n";
        const auto retained = std::min<uint64_t>(detailed, records.size());
        for (uint64_t n = detailed - retained; n < detailed; ++n) {
            const auto &r = records[n % records.size()];
            detail << r.sequence << ',' << r.start_qpc << ',' << r.end_qpc << ',' << r.pipeline << ',' << r.pair << ',' << r.result << ',' << r.native_tid << ',' << r.kind << ',' << r.result_known << ',' << r.reason << '\n';
        }
        std::ofstream session(directory / "session.csv", std::ios::binary);
        session << "startQpc,endQpc,count,ticks,maxTicks,over2ms,over10ms,over50ms,knownResults,nonzeroResults\n";
        for (size_t i = 0; i < bucket_limit; ++i) {
            const auto &b = buckets[i];
            if (!b.stats.count)
                continue;
            session << origin_qpc + i * bucket_width << ',' << origin_qpc + (i + 1) * bucket_width << ',';
            csv_stats(session, b.stats);
            session << '\n';
        }
        std::ofstream totals(directory / "kinds.csv", std::ios::binary);
        totals << "kind,name,count,ticks,maxTicks,over2ms,over10ms,over50ms,knownResults,nonzeroResults\n";
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
void publish_pipeline_profile(std::shared_ptr<PipelineProfile> profile) noexcept {
    enabled.store(false, std::memory_order_release);
    current.store(std::move(profile), std::memory_order_release);
    enabled.store(bool(current.load(std::memory_order_acquire)), std::memory_order_release);
}
bool pipeline_profile_enabled() noexcept { return enabled.load(std::memory_order_relaxed); }
PipelineScope::PipelineScope(PipelineKind k, PipelineContext c, uint64_t queued_at) noexcept
    : kind(k)
    , context(c) {
    if (!pipeline_profile_enabled())
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
    start = queued_at >= profile->origin_qpc && queued_at ? queued_at : PipelineProfile::counter();
}
PipelineScope::~PipelineScope() noexcept {
    if (!profile)
        return;
    const auto end = PipelineProfile::counter();
    profile->append(kind, context, start, end, tid, code, known);
    profile->in_flight.fetch_sub(1, std::memory_order_release);
}
} // namespace util
