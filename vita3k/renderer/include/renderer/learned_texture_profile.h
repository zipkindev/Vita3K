// Vita3K emulator project
// Copyright (C) 2026 Vita3K contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/decoded_texture_cache.h>
#include <renderer/texture_profile_storage.h>
#include <xxhash.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace renderer {
// Private game-derived data. This format stores compressed inputs, not native
// pointers or decoded pixels. Change the version when decoder semantics change.
class LearnedTextureProfile {
public:
    enum class Mode { Off,
        Learn,
        Preload,
        Automatic };
    static constexpr size_t file_limit = 64 * 1024 * 1024;
    static constexpr size_t job_limit = 32 * 1024 * 1024;
    using Clock = std::chrono::steady_clock;
    using Decode = std::function<uint32_t(const void *, uint32_t, uint32_t, uint32_t, uint32_t, uint8_t *)>;
    struct Record {
        DecodedTextureCache::Key key;
        uint64_t decode_ns, uses;
        size_t offset, size;
    };
    struct Report {
        std::string status = "off";
        std::string mode = "off";
        uint64_t profile_entries = 0, prepared_entries = 0, prepared_bytes = 0, prepare_ns = 0, saved_entries = 0, profile_hash = 0;
        uint64_t preload_budget_skips = 0, saved_decoded_bytes = 0;
        DecodedTextureCache::LearningStats learning;
    };

    explicit LearnedTextureProfile(DecodedTextureCache &cache)
        : cache(cache) {}

    static bool layout(const DecodedTextureCache::Key &key, size_t compressed, size_t &pixels) {
        const uint64_t w = std::max(key.width, key.format & 1 ? 16u : 8u), h = std::max(key.height, 8u);
        if (!key.width || !key.height || key.format > 3 || (w & (w - 1)) || (h & (h - 1)))
            return false;
        const uint64_t input = w * h / (key.format & 1 ? 4 : 2);
        // Divide first: malicious uint32 dimensions must never overflow output.
        if (input != compressed || input > job_limit || uint64_t(key.width) * key.height > (job_limit - input) / 4)
            return false;
        pixels = size_t(key.width) * key.height * 4;
        return true;
    }

    // Validate the entire container before any decoder or cache insertion runs.
    static bool validate(const std::vector<uint8_t> &bytes, const std::string &identity, std::vector<Record> &records) {
        records.clear();
        if (bytes.size() < 28 || bytes.size() > file_limit || std::string(reinterpret_cast<const char *>(bytes.data()), 8) != std::string("V3KPVR1\0", 8))
            return false;
        size_t cursor = 8;
        auto get = [&](unsigned n, uint64_t &value) {
            if (cursor > bytes.size() - 8 || n > bytes.size() - 8 - cursor)
                return false;
            value = 0;
            for (unsigned i = 0; i < n; ++i)
                value |= uint64_t(bytes[cursor++]) << (i * 8);
            return true;
        };
        uint64_t version, length, count;
        if (!get(4, version) || version != 1 || !get(4, length) || length > 256 || length != identity.size() || cursor + length > bytes.size() - 8)
            return false;
        if (std::memcmp(identity.data(), bytes.data() + cursor, identity.size()) != 0)
            return false;
        cursor += size_t(length);
        if (!get(4, count) || count > 16384)
            return false;
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t hash, w, h, format, size, cost, uses;
            if (!get(8, hash) || !get(4, w) || !get(4, h) || !get(4, format) || !get(4, size) || !get(8, cost) || !get(8, uses) || size > bytes.size() - 8 - cursor)
                return false;
            Record record{ { hash, uint32_t(w), uint32_t(h), uint32_t(format) }, cost, uses, cursor, size_t(size) };
            size_t pixels;
            if (!layout(record.key, size_t(size), pixels) || XXH3_64bits(bytes.data() + cursor, size_t(size)) != hash)
                return false;
            records.push_back(record);
            cursor += size_t(size);
        }
        if (cursor != bytes.size() - 8)
            return false;
        uint64_t checksum = 0;
        for (unsigned i = 0; i < 8; ++i)
            checksum |= uint64_t(bytes[cursor + i]) << (8 * i);
        return checksum == XXH3_64bits(bytes.data(), cursor);
    }

    Report begin(const std::filesystem::path &root, const std::string &title, const std::string &id, Mode requested, const std::filesystem::path &report_path, const Decode &decode, std::chrono::milliseconds deadline = std::chrono::seconds(5)) {
        cache.stop_learning();
        mode = Mode::Off;
        report = {};
        output = report_path;
        report.mode = requested == Mode::Automatic ? "auto" : requested == Mode::Learn ? "learn"
            : requested == Mode::Preload                                               ? "preload"
                                                                                       : "off";
        identity = title + "|" + id;
        if (requested == Mode::Off) {
            write_report();
            return report;
        }
        // App lifecycle calls are serialized; all work is synchronous and each
        // decode joins its helper before returning. No worker survives begin().
        cache.clear();
        if (title.empty() || title.size() > 32 || identity.size() > 256 || title.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) {
            report.status = "invalid-identity";
            write_report();
            return report;
        }
        mode = requested;
        profile = root / "decoded-textures" / title / "learned.pvrp";
        if (!cache.stats().budget_bytes) {
            mode = Mode::Off;
            report.status = "cache-disabled";
        } else {
            if (mode == Mode::Learn || mode == Mode::Automatic)
                cache.start_learning(file_limit - 28 - identity.size());
            if (std::filesystem::is_symlink(profile.parent_path().parent_path()) || std::filesystem::is_symlink(profile.parent_path()) || std::filesystem::is_symlink(profile))
                throw std::runtime_error("profile path is a symlink");
            if (mode == Mode::Automatic || mode == Mode::Learn) {
                TextureProfileStorage storage(profile.parent_path().parent_path());
                storage.reserve(profile);
            }
            const auto start = Clock::now();
            std::error_code ec;
            const auto size = std::filesystem::file_size(profile, ec);
            report.status = mode == Mode::Learn ? "learn-ready" : "no-profile";
            if (!ec) {
                report.status = "invalid-profile";
                if (size <= file_limit) {
                    std::vector<uint8_t> bytes(static_cast<size_t>(size));
                    std::ifstream input(profile, std::ios::binary);
                    std::vector<Record> records;
                    if (input.read(reinterpret_cast<char *>(bytes.data()), bytes.size()) && validate(bytes, identity, records)) {
                        if (mode == Mode::Automatic || mode == Mode::Learn)
                            std::filesystem::last_write_time(profile, std::filesystem::file_time_type::clock::now());
                        report.profile_entries = records.size();
                        report.profile_hash = XXH3_64bits(bytes.data(), bytes.size());
                        // Merge every validated prior record, including records that
                        // cannot fit the decoded cache. No decoded pixels retained here.
                        if (mode == Mode::Automatic || mode == Mode::Learn)
                            for (const auto &record : records)
                                cache.merge_learning(record.key, std::span(bytes.data() + record.offset, record.size), record.decode_ns, record.uses);
                        report.status = mode == Mode::Learn ? "learn-ready" : "preload-ready";
                        for (const auto &record : records) {
                            if (mode == Mode::Learn)
                                break;
                            if (Clock::now() - start >= deadline) {
                                report.status = "partial-deadline";
                                break;
                            }
                            size_t pixels;
                            layout(record.key, record.size, pixels);
                            auto stats = cache.stats();
                            if (pixels + record.size > stats.budget_bytes - stats.resident_bytes || stats.entries >= 16384) {
                                report.status = "partial-budget";
                                ++report.preload_budget_skips;
                                continue;
                            }
                            // At most 64 MiB input container plus one <=32 MiB
                            // decoded+compressed job; insertion copies pixels into
                            // the resident budget. Index/allocator overhead extra.
                            std::vector<uint8_t> decoded(pixels);
                            // Copy first to preserve the decoder's input alignment.
                            std::vector<uint8_t> compressed(bytes.begin() + record.offset, bytes.begin() + record.offset + record.size);
                            const auto result = decode(compressed.data(), record.key.format & 1, record.key.width, record.key.height, record.key.format >> 1, decoded.data());
                            if (cache.insert(record.key, std::move(compressed), decoded, result, record.decode_ns, false))
                                ++report.prepared_entries;
                        }
                        report.prepared_bytes = cache.stats().resident_bytes;
                    }
                }
            }
            report.prepare_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        }
        report.learning = cache.learning_stats();
        write_report();
        return report;
    }

    Report finish() {
        if (mode == Mode::Learn || mode == Mode::Automatic) {
            // Called only after guest/render threads joined. Learning retains
            // compressed inputs independently of decoded-cache eviction.
            report.learning = cache.learning_stats();
            auto entries = cache.learning_snapshot();
            for (const auto &entry : entries) {
                size_t pixels = 0;
                if (!layout(entry.key, entry.compressed.size(), pixels))
                    throw std::runtime_error("invalid learned texture layout");
                report.saved_decoded_bytes += pixels + entry.compressed.size();
            }
            if (entries.empty()) {
                report.status = "empty-learning"; // Preserve an older good pack.
            } else {
                TextureProfileStorage storage(profile.parent_path().parent_path());
                uint64_t incoming = 28 + identity.size();
                for (const auto &entry : entries)
                    incoming += 40 + entry.compressed.size();
                if (std::filesystem::is_symlink(profile.parent_path()) || std::filesystem::is_symlink(profile))
                    throw std::runtime_error("profile path is a symlink");
                storage.reserve(profile, incoming);
                std::filesystem::create_directories(profile.parent_path());
                auto temporary = profile;
                temporary += ".tmp-" + std::to_string(Clock::now().time_since_epoch().count());
                struct Cleanup {
                    std::filesystem::path path;
                    ~Cleanup() {
                        std::error_code ec;
                        std::filesystem::remove(path, ec);
                    }
                } cleanup{ temporary };
                std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
                auto *hash = XXH3_createState();
                if (!hash)
                    throw std::bad_alloc();
                struct Free {
                    XXH3_state_t *state;
                    ~Free() { XXH3_freeState(state); }
                } free{ hash };
                XXH3_64bits_reset(hash);
                auto put_bytes = [&](const void *data, size_t n) {
                    stream.write(static_cast<const char *>(data), n);
                    XXH3_64bits_update(hash, data, n);
                };
                auto put = [&](uint64_t n, unsigned size) {
                    uint8_t bytes[8];
                    for (unsigned i = 0; i < size; ++i)
                        bytes[i] = uint8_t(n >> (8 * i));
                    put_bytes(bytes, size);
                };
                put_bytes("V3KPVR1\0", 8);
                put(1, 4);
                put(identity.size(), 4);
                put_bytes(identity.data(), identity.size());
                put(entries.size(), 4);
                for (const auto &entry : entries) {
                    put(entry.key.hash, 8);
                    put(entry.key.width, 4);
                    put(entry.key.height, 4);
                    put(entry.key.format, 4);
                    put(entry.compressed.size(), 4);
                    put(entry.decode_ns, 8);
                    put(entry.uses, 8);
                    put_bytes(entry.compressed.data(), entry.compressed.size());
                }
                const auto checksum = XXH3_64bits_digest(hash);
                put(checksum, 8);
                stream.flush();
                const bool good = bool(stream);
                stream.close();
                if (!good || stream.fail())
                    throw std::runtime_error("profile write failed");
#ifdef _WIN32
                if (!MoveFileExW(temporary.c_str(), profile.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    throw std::runtime_error("profile replace failed");
#else
                std::filesystem::rename(temporary, profile);
#endif
                report.saved_entries = entries.size();
                report.profile_entries = entries.size();
                report.profile_hash = XXH3_64bits_digest(hash);
                report.status = "saved";
            }
        }
        if (mode != Mode::Off)
            cache.clear();
        cache.stop_learning();
        mode = Mode::Off;
        write_report();
        return report;
    }

    bool active() const { return mode != Mode::Off; }

    void fail() noexcept {
        cache.stop_learning();
        if (mode != Mode::Off)
            cache.clear();
        mode = Mode::Off;
        report.status = "error";
        try {
            write_report();
        } catch (...) {
        }
    }

private:
    void write_report() const {
        if (output.empty())
            return;
        std::ofstream stream(output, std::ios::trunc);
        stream << "{\"mode\":\"" << report.mode << "\",\"status\":\"" << report.status << "\",\"profileEntries\":" << report.profile_entries
               << ",\"preparedEntries\":" << report.prepared_entries << ",\"preparedResidentBytes\":" << report.prepared_bytes
               << ",\"prepareNs\":" << report.prepare_ns << ",\"savedEntries\":" << report.saved_entries
               << ",\"profileHash\":" << report.profile_hash
               << ",\"learningEntries\":" << report.learning.entries << ",\"learningBytes\":" << report.learning.bytes
               << ",\"learningByteLimit\":" << report.learning.byte_limit << ",\"learningEntryLimit\":" << report.learning.entry_limit
               << ",\"learningObservations\":" << report.learning.observations << ",\"mergedEntries\":" << report.learning.merged
               << ",\"learningEvictions\":" << report.learning.evictions << ",\"learningDrops\":" << report.learning.dropped
               << ",\"preloadBudgetSkips\":" << report.preload_budget_skips << ",\"savedDecodedBytes\":" << report.saved_decoded_bytes << "}\n";
    }
    DecodedTextureCache &cache;
    Mode mode = Mode::Off;
    std::filesystem::path profile, output;
    std::string identity;
    Report report;
};
} // namespace renderer
