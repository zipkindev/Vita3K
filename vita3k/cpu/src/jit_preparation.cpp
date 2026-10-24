// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cpu/jit_preparation.h>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace cpu {
namespace {
constexpr uint64_t magic = 0x3032504552504a56ull; // VJPREP20, little endian
constexpr uint64_t file_limit = 16 * 1024 * 1024, store_limit = 64 * 1024 * 1024;
void write(std::ostream &s, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        s.put(static_cast<char>(value >> (i * 8)));
}
uint64_t read_word(std::istream &s) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        const int c = s.get();
        if (c < 0)
            throw std::runtime_error("Truncated JIT preparation profile");
        value |= uint64_t(static_cast<uint8_t>(c)) << (i * 8);
    }
    return value;
}
uint32_t read32(std::istream &s) {
    const auto value = read_word(s);
    if (value > UINT32_MAX)
        throw std::runtime_error("Invalid JIT preparation word");
    return static_cast<uint32_t>(value);
}
uint64_t fingerprint(std::istream &s, uint64_t size) {
    uint64_t value = 14695981039346656037ull;
    for (uint64_t i = 0; i < size; ++i) {
        const int c = s.get();
        if (c < 0)
            throw std::runtime_error("Truncated JIT preparation checksum");
        value = (value ^ uint8_t(c)) * 1099511628211ull;
    }
    return value;
}
size_t group(uint64_t descriptor) { return ((descriptor ^ (descriptor >> 32)) * 11400714819323198485ull >> 32) % (JitPreparation::capacity / 4) * 4; }
} // namespace

JitPreparation::JitPreparation(std::filesystem::path profile, std::array<uint8_t, 32> identity_, std::vector<Region> regions_)
    : path(std::move(profile))
    , identity(identity_)
    , regions(std::move(regions_))
    , slots(capacity) {
    if (regions.empty() || regions.size() > 8)
        throw std::runtime_error("Invalid JIT preparation regions");
    for (const auto &r : regions) {
        if (!r.size || uint64_t(r.base) + r.size > (uint64_t(1) << 32))
            throw std::runtime_error("Invalid JIT preparation range");
    }
    try {
        load();
    } catch (...) {
        std::fill(slots.begin(), slots.end(), Block{});
        stats.profile_valid = false;
        stats.loaded = 0;
    }
}
JitPreparation::~JitPreparation() = default; // Finish explicitly after the CPU exits.
bool JitPreparation::address_ok(uint32_t address) const noexcept {
    if (address & 3)
        return false;
    for (const auto &r : regions)
        if (address >= r.base && uint64_t(address) + 4 <= uint64_t(r.base) + r.size)
            return true;
    return false;
}
bool JitPreparation::block_ok(const Block &b) const noexcept {
    // Full A32 location descriptor: PC, Thumb/endian, FPSCR, IT state. No stepping/reserved bits.
    constexpr uint64_t mask = 0x07f7ff03ffffffffull;
    const auto pc = uint32_t(b.descriptor);
    if ((b.descriptor & ~mask) || (pc & ((b.descriptor >> 32) & 1 ? 1 : 3)) || !address_ok(pc & ~3u)
        || !b.count || b.count > read_limit || !b.cost_ns || !address_ok((b.end_pc - 1) & ~3u))
        return false;
    for (size_t i = 0; i < b.count; ++i)
        if (!address_ok(b.reads[i].address) || b.reads[i].hook > 1)
            return false;
    return true;
}
void JitPreparation::begin(uint64_t descriptor) noexcept {
    current = {};
    current.descriptor = descriptor;
    begin_epoch = epoch;
    bad = false;
    active = true;
}
void JitPreparation::read(uint32_t address, uint32_t value, bool hook) noexcept {
    if (!active)
        return;
    if (!address_ok(address) || current.count == read_limit) {
        bad = true;
        return;
    }
    current.reads[current.count++] = { address, value, uint32_t(hook) };
}
void JitPreparation::complete(uint32_t end_pc, uint64_t cost_ns) noexcept {
    if (!active)
        return;
    active = false;
    ++stats.observed;
    current.end_pc = end_pc;
    current.cost_ns = cost_ns;
    if (bad || begin_epoch != epoch || cost_ns < 50000 || !block_ok(current)) {
        ++stats.rejected;
        return;
    }
    const size_t first = group(current.descriptor);
    size_t victim = first;
    for (size_t i = first; i < first + 4; ++i) {
        if (!slots[i].count || slots[i].descriptor == current.descriptor) {
            victim = i;
            break;
        }
        if (slots[i].cost_ns < slots[victim].cost_ns)
            victim = i;
    }
    if (slots[victim].count && slots[victim].descriptor != current.descriptor && slots[victim].cost_ns > cost_ns)
        return;
    slots[victim] = current;
    ++stats.learned;
}
void JitPreparation::replay(const Block &block) noexcept {
    replaying = &block;
    replay_index = 0;
    bad = !block_ok(block);
}
bool JitPreparation::expects_read(uint32_t address, bool hook) const noexcept {
    return replaying && !bad && replay_index < replaying->count
        && replaying->reads[replay_index].address == address && replaying->reads[replay_index].hook == uint32_t(hook);
}
std::optional<uint32_t> JitPreparation::replay_read(uint32_t address, uint32_t actual, bool hook) noexcept {
    if (!replaying || bad || replay_index >= replaying->count) {
        bad = true;
        return std::nullopt;
    }
    const auto &r = replaying->reads[replay_index++];
    if (r.address != address || r.value != actual || r.hook != uint32_t(hook)) {
        bad = true;
        return std::nullopt;
    }
    return actual;
}
bool JitPreparation::allow_emit(uint64_t descriptor, uint32_t end_pc) noexcept {
    return replaying && !bad && descriptor == replaying->descriptor && end_pc == replaying->end_pc && replay_index == replaying->count;
}
std::vector<JitPreparation::Block> JitPreparation::ranked() const {
    std::vector<Block> result;
    for (const auto &b : slots)
        if (b.count)
            result.push_back(b);
    std::sort(result.begin(), result.end(), [](const Block &a, const Block &b) { return a.cost_ns > b.cost_ns; });
    return result;
}
void JitPreparation::load() {
    if (!std::filesystem::is_regular_file(path) || std::filesystem::is_symlink(path) || std::filesystem::file_size(path) > file_limit)
        return;
    const auto length = std::filesystem::file_size(path);
    if (length < 56)
        return;
    std::ifstream s(path, std::ios::binary);
    const auto checksum = fingerprint(s, length - 8);
    if (read_word(s) != checksum)
        return;
    s.clear();
    s.seekg(0);
    if (read_word(s) != magic)
        return;
    std::array<uint8_t, 32> stored{};
    s.read(reinterpret_cast<char *>(stored.data()), stored.size());
    if (!s || stored != identity)
        return;
    const auto count = read_word(s);
    if (count > capacity)
        return;
    for (uint64_t i = 0; i < count; ++i) {
        Block b;
        b.descriptor = read_word(s);
        b.cost_ns = read_word(s);
        b.end_pc = read32(s);
        b.count = read32(s);
        if (!b.count || b.count > read_limit)
            throw std::runtime_error("Invalid JIT preparation read count");
        for (size_t j = 0; j < b.count; ++j)
            b.reads[j] = { read32(s), read32(s), read32(s) };
        if (!block_ok(b))
            throw std::runtime_error("Invalid JIT preparation block");
        const size_t first = group(b.descriptor);
        bool placed = false;
        for (size_t j = first; j < first + 4; ++j) {
            if (slots[j].count && slots[j].descriptor == b.descriptor)
                throw std::runtime_error("Duplicate JIT preparation block");
            if (!slots[j].count) {
                slots[j] = b;
                placed = true;
                break;
            }
        }
        if (!placed)
            throw std::runtime_error("JIT preparation bucket overflow");
    }
    if (s.tellg() != std::streampos(length - 8))
        throw std::runtime_error("Trailing JIT preparation data");
    stats.loaded = count;
    stats.profile_valid = true;
}
bool JitPreparation::finish() noexcept {
    if (finished)
        return stats.saved;
    finished = true;
    try {
        std::filesystem::create_directories(path.parent_path());
        const auto blocks = ranked();
        auto pending = path;
        pending += ".pending";
        std::ofstream s(pending, std::ios::binary | std::ios::trunc);
        write(s, magic);
        s.write(reinterpret_cast<const char *>(identity.data()), identity.size());
        write(s, blocks.size());
        for (const auto &b : blocks) {
            write(s, b.descriptor);
            write(s, b.cost_ns);
            write(s, b.end_pc);
            write(s, b.count);
            for (size_t i = 0; i < b.count; ++i) {
                write(s, b.reads[i].address);
                write(s, b.reads[i].value);
                write(s, b.reads[i].hook);
            }
        }
        s.close();
        if (!s)
            throw std::runtime_error("JIT preparation profile write failed");
        std::ifstream check(pending, std::ios::binary);
        const auto checksum = fingerprint(check, std::filesystem::file_size(pending));
        check.close();
        std::ofstream footer(pending, std::ios::binary | std::ios::app);
        write(footer, checksum);
        footer.close();
        if (!footer || std::filesystem::file_size(pending) > file_limit)
            throw std::runtime_error("JIT preparation profile write failed");
        // Profile lives only in this bounded cache directory. Preserve unrelated files.
        std::vector<std::filesystem::directory_entry> profiles;
        uint64_t bytes = std::filesystem::file_size(pending);
        for (const auto &p : std::filesystem::directory_iterator(path.parent_path())) {
            if (p.is_symlink() || !p.is_regular_file() || p.path().extension() != ".pj20" || p.path() == path)
                continue;
            bytes += p.file_size();
            profiles.push_back(p);
        }
        std::sort(profiles.begin(), profiles.end(), [](const auto &a, const auto &b) { return a.last_write_time() < b.last_write_time(); });
        for (const auto &p : profiles) {
            if (bytes <= store_limit)
                break;
            bytes -= p.file_size();
            std::filesystem::remove(p.path());
        }
        // Windows rename does not replace an existing file. Backup the old profile first.
        auto backup = path;
        backup += ".previous";
        std::error_code ec;
        std::filesystem::remove(backup, ec);
        const bool existed = std::filesystem::exists(path);
        if (existed)
            std::filesystem::rename(path, backup);
        try {
            std::filesystem::rename(pending, path);
        } catch (...) {
            if (existed)
                std::filesystem::rename(backup, path);
            throw;
        }
        std::filesystem::remove(backup, ec);
        stats.saved = true;
    } catch (...) {
        stats.saved = false;
    }
    report();
    return stats.saved;
}
void JitPreparation::report() const noexcept {
    try {
        std::filesystem::create_directories(path.parent_path());
        auto report_path = path;
        report_path.replace_extension(".json");
        std::ofstream s(report_path, std::ios::trunc);
        s << "{\"schema\":1,\"mode\":\"validated-main-thread-compile-only\",\"profileValid\":" << (stats.profile_valid ? "true" : "false")
          << ",\"loaded\":" << stats.loaded << ",\"attempted\":" << stats.attempted << ",\"prepared\":" << stats.prepared << ",\"skipped\":" << stats.skipped
          << ",\"elapsedMs\":" << stats.elapsed_ms << ",\"nativeBytes\":" << stats.native_bytes << ",\"deadline\":" << (stats.deadline ? "true" : "false")
          << ",\"nativeCap\":" << (stats.native_cap ? "true" : "false") << ",\"observed\":" << stats.observed << ",\"learned\":" << stats.learned << ",\"rejected\":" << stats.rejected
          << ",\"saved\":" << (stats.saved ? "true" : "false") << ",\"blockLimit\":" << capacity << ",\"readLimit\":" << read_limit
          << ",\"timeLimitMs\":" << time_limit_ms << ",\"nativeLimit\":" << native_limit << ",\"profileLimitBytes\":" << file_limit << ",\"storeLimitBytes\":" << store_limit << "}\n";
    } catch (...) {
    }
}
} // namespace cpu
