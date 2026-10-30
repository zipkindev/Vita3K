// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <renderer/texture_profile_storage.h>
using Storage = renderer::TextureProfileStorage;
static void put(const std::filesystem::path &path, uint64_t size) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    if (size) {
        stream.seekp(size - 1);
        stream.put('x');
    }
    stream.close();
    assert(!stream.fail());
}
int main() {
    auto base = std::filesystem::temp_directory_path() / ("vita-storage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{ base };
    auto root = base / "profiles";
    auto a = root / "AAA/learned.pvrp", b = root / "BBB/learned.pvrp", c = root / "CCC/learned.pvrp";
    put(a, 200);
    put(b, 200);
    put(c, 200);
    auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(a, now - std::chrono::hours(2));
    std::filesystem::last_write_time(b, now - std::chrono::hours(3));
    std::filesystem::last_write_time(c, now - std::chrono::hours(1));
    put(root / "AAA/notes.txt", 100);
    put(root / "AAA/learned.pvrp.tmp-123", 100);
    put(root / "AAA/learned.pvrp.tmp-unrelated", 100);
    {
        Storage storage(root);
        bool busy = false;
        try {
            Storage conflict(root);
        } catch (const std::runtime_error &) {
            busy = true;
        }
        assert(busy); // Works across processes too: OS lock/share denial.
        storage.reserve(c, 0, 300);
        assert(!std::filesystem::exists(a) && !std::filesystem::exists(b) && std::filesystem::exists(c));
        assert(!std::filesystem::exists(root / "AAA/learned.pvrp.tmp-123"));
        assert(std::filesystem::exists(root / "AAA/notes.txt"));
        assert(std::filesystem::exists(root / "AAA/learned.pvrp.tmp-unrelated"));
        storage.reserve(c, 300, 300); // Replacing200 by300 requires only300 final bytes.
        put(c, 300);
        bool over = false;
        try {
            storage.reserve(c, 301, 300);
        } catch (const std::runtime_error &) {
            over = true;
        }
        assert(over && std::filesystem::file_size(c) == 300);
    }
    {
        Storage storage(root);
        storage.reserve(c, 0, 300);
    } // Lock released.
    put(a, Storage::per_title_limit + 1);
    {
        Storage storage(root);
        storage.reserve(c);
    }
    assert(!std::filesystem::exists(a) && std::filesystem::exists(c));
    const auto outside = base / "outside/learned.pvrp";
    put(outside, 900);
    std::error_code ec;
    std::filesystem::create_directory_symlink(outside.parent_path(), root / "LINK", ec);
    if (!ec) {
        {
            Storage storage(root);
            storage.reserve(c, 0, 300);
        }
        assert(std::filesystem::file_size(outside) == 900);
    }
    {
        Storage storage(root);
        assert(storage.clear("CCC") == 1);
        assert(!std::filesystem::exists(c));
        assert(std::filesystem::exists(root / "AAA/notes.txt"));
        assert(std::filesystem::exists(root / "AAA/learned.pvrp.tmp-unrelated"));
        assert(std::filesystem::file_size(outside) == 900);
        put(a, 20);
        put(b, 20);
        assert(storage.clear() == 2);
        assert(storage.clear() == 0);
    }
    assert(!Storage::title_name("../outside") && Storage::title_name("PCSA00015"));
    std::cout << "Profile quota/LRU/replacement/orphan/lock/symlink checks passed\n";
}
