// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cpu/jit_profile.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cpu {
namespace {
uint64_t counter() noexcept {
#ifdef _WIN32
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<uint64_t>(value.QuadPart);
#else
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
#endif
}
uint64_t counter_frequency() noexcept {
#ifdef _WIN32
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return static_cast<uint64_t>(value.QuadPart);
#else
    return 1000000000;
#endif
}
std::string quote(const std::string &s) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += static_cast<char>(c);
    }
    return out + '"';
}
} // namespace

JitCompileProfile::JitCompileProfile(JitProfileConfig config_, JitProfileClock clock)
    : config(std::move(config_))
    , read_counter(clock.read ? clock.read : counter)
    , clock_frequency(clock.read ? clock.frequency : counter_frequency())
    , origin_qpc(read_counter()) {
    if ((!clock.read && clock.frequency) || (clock.read && !clock.frequency))
        throw std::runtime_error("Invalid JIT diagnostic clock");
    if (!clock_frequency || !config.max_records || config.max_records > record_limit
        || config.regions.size() > 256 || config.title.size() > 128
        || config.version.size() > 128 || config.executable.size() > 256)
        throw std::runtime_error("Invalid JIT diagnostic bounds");
    for (const auto &r : config.regions) {
        if (r.module_path.size() > 1024 || uint64_t(r.base) + r.size > (uint64_t(1) << 32))
            throw std::runtime_error("Invalid JIT startup region");
    }
    entries.reserve(config.max_records);
    buckets.resize(bucket_limit);
    bucket_width_qpc = clock_frequency;
    // Never overwrite an earlier learning run. Launcher chooses a fresh directory.
    if (!std::filesystem::create_directory(config.directory))
        throw std::runtime_error("JIT diagnostic directory must be fresh");
    std::ofstream armed(config.directory / "armed.json", std::ios::binary);
    armed << "{\"schema\":2,\"status\":\"armed\",\"role\":\"main\",\"backend\":\"x64\","
          << "\"title\":" << quote(config.title) << ",\"version\":" << quote(config.version)
          << ",\"executable\":" << quote(config.executable) << ",\"guestThread\":" << config.guest_thread
          << ",\"cpuOptimizations\":" << (config.cpu_optimizations ? "true" : "false")
          << ",\"dynarmicPin\":\"86458a0bd369d63ba4c2ef812cacbb6c9080c065\",\"observerSchema\":1"
          << ",\"clockFrequency\":" << clock_frequency << ",\"originQpc\":" << origin_qpc
          << ",\"maxRecords\":" << config.max_records << ",\"recordBytes\":" << sizeof(Record)
          << ",\"bufferBytes\":" << config.max_records * sizeof(Record)
          << ",\"retention\":\"rolling\",\"maxSessionBuckets\":" << bucket_limit
          << ",\"sessionBucketBytes\":" << sizeof(Bucket) * bucket_limit
          << ",\"initialBucketWidthQpc\":" << bucket_width_qpc
          << ",\"maxReadWordsPerRecord\":" << read_limit << ",\"startupRegions\":[";
    for (size_t i = 0; i < config.regions.size(); ++i) {
        const auto &r = config.regions[i];
        if (i)
            armed << ',';
        armed << "{\"index\":" << i << ",\"module\":" << quote(r.module_path)
              << ",\"base\":" << r.base << ",\"size\":" << r.size << ",\"permissions\":" << r.permissions << '}';
    }
    armed << "],\"scope\":\"compile misses only; no prewarming; read fingerprints are diagnostic, not replay validation\"}\n";
    armed.close();
    if (!armed)
        throw std::runtime_error("Cannot write JIT diagnostic header");
}
JitCompileProfile::~JitCompileProfile() { finish(); }
bool JitCompileProfile::enabled() const noexcept { return !finished; }
const JitCompileProfile::Record &JitCompileProfile::chronological_record(size_t index) const {
    if (index >= entries.size())
        throw std::out_of_range("JIT detail index");
    return entries[(entries.size() == config.max_records ? next_record + index : index) % entries.size()];
}
JitCompileProfile::Bucket &JitCompileProfile::bucket_for(uint64_t qpc) noexcept {
    const uint64_t distance = qpc >= origin_qpc ? qpc - origin_qpc : 0;
    while (distance / bucket_width_qpc >= bucket_limit) {
        for (size_t i = 0; i < bucket_limit / 2; ++i) {
            const auto left = buckets[i * 2], right = buckets[i * 2 + 1];
            buckets[i] = { left.count + right.count, left.compile_ns + right.compile_ns,
                (std::max)(left.max_ns, right.max_ns), left.emitted_bytes + right.emitted_bytes,
                left.aborted + right.aborted, left.invalidations + right.invalidations,
                left.flagged + right.flagged, left.read_limited + right.read_limited };
        }
        std::fill(buckets.begin() + bucket_limit / 2, buckets.end(), Bucket{});
        bucket_count = (bucket_count + 1) / 2;
        bucket_width_qpc *= 2;
        ++bucket_folds;
    }
    const auto index = static_cast<size_t>(distance / bucket_width_qpc);
    bucket_count = (std::max)(bucket_count, index + 1);
    return buckets[index];
}
void JitCompileProfile::begin(uint64_t descriptor) noexcept {
    if (active)
        abort();
    if (!enabled())
        return;
    current = {};
    current.descriptor = descriptor;
    current.epoch = epoch;
    const auto pc = static_cast<uint32_t>(descriptor);
    for (size_t i = 0; i < config.regions.size(); ++i) {
        const auto &r = config.regions[i];
        if (pc >= r.base && uint64_t(pc) < uint64_t(r.base) + r.size) {
            current.region = static_cast<uint32_t>(i);
            current.relative_pc = pc - r.base;
            break;
        }
    }
    current.start_qpc = read_counter();
    active = true;
}
void JitCompileProfile::code_read(uint32_t address, uint32_t value, bool hook) noexcept {
    if (!active)
        return;
    if (current.reads >= read_limit) {
        current.flags |= ReadLimit;
        return;
    }
    ++current.reads;
    if (hook)
        ++current.hook_reads;
    current.min_read = (std::min)(current.min_read, address);
    current.max_read = (std::max)(current.max_read, address);
    if (current.region == UINT32_MAX)
        current.flags |= OutsideStartupRegion;
    else {
        const auto &r = config.regions[current.region];
        if (address < r.base || uint64_t(address) + 4 > uint64_t(r.base) + r.size)
            current.flags |= OutsideStartupRegion;
    }
    // Bounded read-trace fingerprint. Normalize offsets to block PC; never retain bytes.
    const uint32_t offset = address - static_cast<uint32_t>(current.descriptor);
    for (uint32_t word : { offset, value, uint32_t(hook) }) {
        for (unsigned i = 0; i < 4; ++i) {
            current.read_hash ^= (word >> (i * 8)) & 255;
            current.read_hash *= 1099511628211ull;
        }
    }
}
void JitCompileProfile::invalid_read() noexcept {
    if (active)
        current.flags |= InvalidRead;
}
void JitCompileProfile::complete(uint32_t end_pc, size_t native_bytes) noexcept {
    if (!active)
        return;
    current.end_qpc = read_counter();
    current.end_epoch = epoch;
    if (current.epoch != epoch)
        current.flags |= InvalidatedDuringCompile;
    current.end_pc = end_pc;
    current.native_bytes = native_bytes;
    current.sequence = completed++;
    const auto ns = static_cast<uint64_t>((current.end_qpc - current.start_qpc) * (1000000000.0L / clock_frequency));
    total_compile_ns += ns;
    total_emitted_bytes += native_bytes;
    // Attribute inclusive compilation time to the bucket in which it began.
    auto &bucket = bucket_for(current.start_qpc);
    ++bucket.count;
    bucket.compile_ns += ns;
    bucket.max_ns = (std::max)(bucket.max_ns, ns);
    bucket.emitted_bytes += native_bytes;
    bucket.flagged += current.flags != 0;
    bucket.read_limited += (current.flags & ReadLimit) != 0;
    if (entries.size() < config.max_records)
        entries.push_back(current); // Capacity reserved before the guest CPU starts.
    else
        entries[next_record] = current;
    next_record = (next_record + 1) % config.max_records;
    active = false;
}
void JitCompileProfile::abort() noexcept {
    if (active) {
        ++abandoned;
        ++bucket_for(read_counter()).aborted;
        active = false;
    }
}
void JitCompileProfile::invalidated() noexcept {
    if (!finished) {
        ++epoch;
        ++bucket_for(read_counter()).invalidations;
    }
}
bool JitCompileProfile::finish() noexcept {
    if (finished)
        return write_ok;
    abort();
    finish_qpc = read_counter();
    bucket_for(finish_qpc); // Include idle time through normal shutdown.
    finished = true;
    try {
        std::ofstream rows(config.directory / "compilations.csv", std::ios::binary);
        rows << "Schema,Sequence,Descriptor,Pc,EndPc,Epoch,EndEpoch,StartQpc,EndQpc,CompileNs,NativeBytes,StartupRegion,StartupRelativePc,ReadWords,HookReadWords,MinReadPc,MaxReadPc,ReadTraceHash,Flags\n";
        uint64_t total_ns = 0, total_native = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto &r = chronological_record(i);
            const auto ns = static_cast<uint64_t>((r.end_qpc - r.start_qpc) * (1000000000.0L / clock_frequency));
            total_ns += ns;
            total_native += r.native_bytes;
            rows << "2," << r.sequence << ',' << r.descriptor << ',' << uint32_t(r.descriptor) << ',' << r.end_pc
                 << ',' << r.epoch << ',' << r.end_epoch << ',' << r.start_qpc << ',' << r.end_qpc
                 << ',' << ns << ',' << r.native_bytes << ',' << r.region << ',' << r.relative_pc
                 << ',' << r.reads << ',' << r.hook_reads << ',' << r.min_read << ',' << r.max_read
                 << ',' << r.read_hash << ',' << r.flags << '\n';
        }
        rows.close();
        if (!rows)
            return false;
        std::ofstream session(config.directory / "session.csv", std::ios::binary);
        session << "Schema,Bucket,StartQpc,EndQpc,CompilationCount,CompileNs,MaxCompileNs,EmittedBytes,Aborted,Invalidations,Flagged,ReadLimited\n";
        for (size_t i = 0; i < bucket_count; ++i) {
            const auto &b = buckets[i];
            const auto start = origin_qpc + i * bucket_width_qpc;
            const auto end = start + (std::min)(bucket_width_qpc, finish_qpc - start);
            session << "2," << i << ',' << start << ',' << end << ',' << b.count << ',' << b.compile_ns
                    << ',' << b.max_ns << ',' << b.emitted_bytes << ',' << b.aborted << ',' << b.invalidations
                    << ',' << b.flagged << ',' << b.read_limited << '\n';
        }
        session.close();
        if (!session)
            return false;
        std::ofstream summary(config.directory / "summary.json", std::ios::binary);
        summary << "{\"schema\":2,\"status\":\"saved\",\"role\":\"main\",\"retention\":\"rolling\",\"records\":" << entries.size()
                << ",\"maxRecords\":" << config.max_records << ",\"totalCompilations\":" << completed
                << ",\"overwrittenRecords\":" << completed - entries.size()
                << ",\"firstRetainedSequence\":" << completed - entries.size()
                << ",\"nextSequence\":" << completed
                << ",\"detailFirstQpc\":" << (entries.empty() ? 0 : chronological_record(0).start_qpc)
                << ",\"detailLastQpc\":" << (entries.empty() ? 0 : chronological_record(entries.size() - 1).end_qpc)
                << ",\"originQpc\":" << origin_qpc << ",\"finishQpc\":" << finish_qpc
                << ",\"aborted\":" << abandoned << ",\"invalidationEpoch\":" << epoch
                << ",\"totalCompileNs\":" << total_compile_ns << ",\"totalEmittedBytes\":" << total_emitted_bytes
                << ",\"retainedCompileNs\":" << total_ns << ",\"retainedEmittedBytes\":" << total_native
                << ",\"sessionBuckets\":" << bucket_count << ",\"maxSessionBuckets\":" << bucket_limit
                << ",\"bucketWidthQpc\":" << bucket_width_qpc << ",\"bucketFolds\":" << bucket_folds
                << ",\"sessionComplete\":true,\"clockFrequency\":" << clock_frequency
                << ",\"limits\":\"main thread only; inclusive compile time assigned to start bucket; adaptive summaries cover session; detail is rolling and may overwrite race events; timings include observer overhead; not a replay profile\"}\n";
        summary.close();
        write_ok = bool(summary);
    } catch (...) {
        write_ok = false;
    }
    return write_ok;
}
} // namespace cpu
