// Vita3K emulator project
// Copyright (C) 2026 Vita3K contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace renderer {
// Serializes all profile writers/pruners across emulator processes. Read-only
// diagnostic Preload does not acquire this lock or modify profile timestamps.
class TextureProfileStorage {
public:
    static constexpr uint64_t per_title_limit = 64ull * 1024 * 1024;
    static constexpr uint64_t total_limit = 1024ull * 1024 * 1024;
    static bool title_name(const std::string &name) {
        return !name.empty() && name.size() <= 32 && name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
    }
    explicit TextureProfileStorage(const std::filesystem::path &directory)
        : root(directory) {
        if (std::filesystem::is_symlink(root))
            throw std::runtime_error("profile storage is a symlink");
        std::filesystem::create_directories(root);
        const auto path = root / ".storage-lock";
        if (std::filesystem::is_symlink(path))
            throw std::runtime_error("profile lock is a symlink");
#ifdef _WIN32
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            throw std::runtime_error("profile storage busy");
#else
        handle = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (handle < 0)
            throw std::runtime_error("profile lock unavailable");
        if (flock(handle, LOCK_EX | LOCK_NB)) {
            close(handle);
            handle = -1;
            throw std::runtime_error("profile storage busy");
        }
#endif
    }
    TextureProfileStorage(const TextureProfileStorage &) = delete;
    TextureProfileStorage &operator=(const TextureProfileStorage &) = delete;
    ~TextureProfileStorage() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
#else
        if (handle >= 0)
            close(handle);
#endif
    }
    // Account only this feature's owned files in direct safe-title directories.
    // Unknown files/directories and symlinks are never followed or deleted.
    // While a replacement is written, at most one extra <=64MiB temp is present.
    void reserve(const std::filesystem::path &replacement = {}, uint64_t incoming = 0, uint64_t limit = total_limit) {
        if (incoming > per_title_limit || incoming > limit)
            throw std::runtime_error("profile exceeds storage budget");
        struct File {
            std::filesystem::path path;
            uint64_t bytes;
            std::filesystem::file_time_type time;
        };
        std::vector<File> files;
        uint64_t bytes = 0;
        for (const auto &directory : std::filesystem::directory_iterator(root)) {
            if (directory.is_symlink() || !directory.is_directory() || !title_name(directory.path().filename().string()))
                continue;
            for (const auto &entry : std::filesystem::directory_iterator(directory.path())) {
                if (entry.is_symlink() || !entry.is_regular_file())
                    continue;
                const auto name = entry.path().filename().string();
                constexpr char prefix[] = "learned.pvrp.tmp-";
                const bool temporary = name.starts_with(prefix) && name.size() > sizeof(prefix) - 1 && name.find_first_not_of("0123456789", sizeof(prefix) - 1) == std::string::npos;
                if (temporary) {
                    // All writers hold our cross-process lock; these are orphans.
                    if (!std::filesystem::remove(entry.path()))
                        throw std::runtime_error("cannot remove orphan profile");
                } else if (name == "learned.pvrp") {
                    const auto size = entry.file_size();
                    if (size > per_title_limit) {
                        if (!std::filesystem::remove(entry.path()))
                            throw std::runtime_error("cannot remove oversized profile");
                        continue;
                    }
                    if (entry.path() == replacement && incoming)
                        continue; // Atomically replaced; temporary headroom documented.
                    bytes += size;
                    files.push_back({ entry.path(), size, entry.last_write_time() });
                }
            }
        }
        std::sort(files.begin(), files.end(), [](const File &a, const File &b) { return a.time < b.time; });
        for (const auto &file : files) {
            if (bytes <= limit - incoming)
                break;
            // On startup prefer retaining the active title if it fits by itself.
            if (!incoming && file.path == replacement)
                continue;
            if (!std::filesystem::remove(file.path))
                throw std::runtime_error("cannot prune profile");
            bytes -= file.bytes;
        }
        if (bytes > limit - incoming)
            throw std::runtime_error("profile quota unavailable");
    }

    uint64_t clear(const std::string &title = {}) {
        if (!title.empty() && !title_name(title))
            throw std::runtime_error("invalid profile title");
        uint64_t removed = 0;
        for (const auto &directory : std::filesystem::directory_iterator(root)) {
            if (directory.is_symlink() || !directory.is_directory())
                continue;
            const auto name = directory.path().filename().string();
            if (!title_name(name) || (!title.empty() && name != title))
                continue;
            for (const auto &entry : std::filesystem::directory_iterator(directory.path())) {
                if (entry.is_symlink() || !entry.is_regular_file())
                    continue;
                const auto file = entry.path().filename().string();
                constexpr char prefix[] = "learned.pvrp.tmp-";
                const bool temporary = file.starts_with(prefix) && file.size() > sizeof(prefix) - 1 && file.find_first_not_of("0123456789", sizeof(prefix) - 1) == std::string::npos;
                if (file == "learned.pvrp" || temporary) {
                    if (!std::filesystem::remove(entry.path()))
                        throw std::runtime_error("cannot clear saved profile");
                    ++removed;
                }
            }
        }
        return removed;
    }

private:
    std::filesystem::path root;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int handle = -1;
#endif
};
} // namespace renderer
