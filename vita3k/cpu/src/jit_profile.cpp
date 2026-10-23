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

JitCompileProfile::JitCompileProfile(JitProfileConfig config_)
    : config(std::move(config_))
    , clock_frequency(counter_frequency())
    , origin_qpc(counter()) {
    if (!clock_frequency || !config.max_records || config.max_records > record_limit
        || config.regions.size() > 256 || config.title.size() > 128
        || config.version.size() > 128 || config.executable.size() > 256)
        throw std::runtime_error("Invalid JIT diagnostic bounds");
    for (const auto &r : config.regions) {
        if (r.module_path.size() > 1024 || uint64_t(r.base) + r.size > (uint64_t(1) << 32))
            throw std::runtime_error("Invalid JIT startup region");
    }
    entries.reserve(config.max_records);
    // Never overwrite an earlier learning run. Launcher chooses a fresh directory.
    if (!std::filesystem::create_directory(config.directory))
        throw std::runtime_error("JIT diagnostic directory must be fresh");
    std::ofstream armed(config.directory / "armed.json", std::ios::binary);
    armed << "{\"schema\":1,\"status\":\"armed\",\"role\":\"main\",\"backend\":\"x64\","
          << "\"title\":" << quote(config.title) << ",\"version\":" << quote(config.version)
          << ",\"executable\":" << quote(config.executable) << ",\"guestThread\":" << config.guest_thread
          << ",\"cpuOptimizations\":" << (config.cpu_optimizations ? "true" : "false")
          << ",\"dynarmicPin\":\"86458a0bd369d63ba4c2ef812cacbb6c9080c065\",\"observerSchema\":1"
          << ",\"clockFrequency\":" << clock_frequency << ",\"originQpc\":" << origin_qpc
          << ",\"maxRecords\":" << config.max_records << ",\"recordBytes\":" << sizeof(Record)
          << ",\"bufferBytes\":" << config.max_records * sizeof(Record)
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
bool JitCompileProfile::enabled() const noexcept { return !finished && entries.size() < config.max_records; }
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
    current.start_qpc = counter();
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
    current.end_qpc = counter();
    current.end_epoch = epoch;
    if (current.epoch != epoch)
        current.flags |= InvalidatedDuringCompile;
    current.end_pc = end_pc;
    current.native_bytes = native_bytes;
    entries.push_back(current); // Capacity reserved before the guest CPU starts.
    active = false;
}
void JitCompileProfile::abort() noexcept {
    if (active) {
        ++abandoned;
        active = false;
    }
}
void JitCompileProfile::invalidated() noexcept { ++epoch; }
bool JitCompileProfile::finish() noexcept {
    if (finished)
        return write_ok;
    abort();
    finished = true;
    try {
        std::ofstream rows(config.directory / "compilations.csv", std::ios::binary);
        rows << "Schema,Sequence,Descriptor,Pc,EndPc,Epoch,EndEpoch,StartQpc,EndQpc,CompileNs,NativeBytes,StartupRegion,StartupRelativePc,ReadWords,HookReadWords,MinReadPc,MaxReadPc,ReadTraceHash,Flags\n";
        uint64_t total_ns = 0, total_native = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto &r = entries[i];
            const auto ns = static_cast<uint64_t>((r.end_qpc - r.start_qpc) * (1000000000.0L / clock_frequency));
            total_ns += ns;
            total_native += r.native_bytes;
            rows << "1," << i << ',' << r.descriptor << ',' << uint32_t(r.descriptor) << ',' << r.end_pc
                 << ',' << r.epoch << ',' << r.end_epoch << ',' << r.start_qpc << ',' << r.end_qpc
                 << ',' << ns << ',' << r.native_bytes << ',' << r.region << ',' << r.relative_pc
                 << ',' << r.reads << ',' << r.hook_reads << ',' << r.min_read << ',' << r.max_read
                 << ',' << r.read_hash << ',' << r.flags << '\n';
        }
        rows.close();
        if (!rows)
            return false;
        std::ofstream summary(config.directory / "summary.json", std::ios::binary);
        summary << "{\"schema\":1,\"status\":\"saved\",\"role\":\"main\",\"records\":" << entries.size()
                << ",\"maxRecords\":" << config.max_records << ",\"truncated\":" << (entries.size() == config.max_records ? "true" : "false")
                << ",\"aborted\":" << abandoned << ",\"invalidationEpoch\":" << epoch
                << ",\"totalCompileNs\":" << total_ns << ",\"totalEmittedBytes\":" << total_native
                << ",\"clockFrequency\":" << clock_frequency
                << ",\"limits\":\"main thread only; timings include observer overhead; capped chronological records; startup region matches are not live module identity proofs; not a replay profile\"}\n";
        summary.close();
        write_ok = bool(summary);
    } catch (...) {
        write_ok = false;
    }
    return write_ok;
}
} // namespace cpu
