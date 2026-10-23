// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <renderer/decoded_texture_cache.h>
#include <thread>

int main() {
    using Cache = renderer::DecodedTextureCache;
    const Cache::Key a{ 1, 8, 8, 0 }, b{ 2, 8, 8, 0 }, c{ 3, 8, 8, 0 };
    const std::vector<uint8_t> input{ 1, 2, 3, 4 }, changed{ 4, 3, 2, 1 }, pixels{ 9, 8, 7, 6 }, other{ 5, 4, 3, 2 };
    std::array<uint8_t, 4> output{};
    uint32_t result = 0;
    Cache cache(16, 2);
    cache.insert(a, input, pixels, 32);
    cache.insert(b, input, other, 64);
    assert(cache.copy(a, input, output, result) && result == 32 && std::equal(output.begin(), output.end(), pixels.begin()));
    cache.insert(c, input, other, 64);
    assert(!cache.copy(b, input, output, result)); // Least recent entry evicted.
    assert(cache.copy(a, input, output, result));
    assert(!cache.copy(a, changed, output, result)); // Same hash/key, changed bytes.
    assert(!cache.copy({ 1, 16, 8, 0 }, input, output, result));
    assert(!cache.copy({ 1, 8, 8, 1 }, input, output, result));
    cache.insert(a, changed, other, 96); // Deliberate hash collision.
    assert(cache.copy(a, changed, output, result) && result == 96 && std::equal(output.begin(), output.end(), other.begin()));
    assert(cache.copy(a, input, output, result) && result == 32);
    cache.insert(a, input, pixels, 32); // Concurrent duplicate insert is harmless.
    assert(cache.stats().entries == 2 && cache.stats().resident_bytes <= 16);
    cache.insert(b, std::vector<uint8_t>(17), pixels, 0);
    assert(cache.stats().resident_bytes <= 16);
    cache.set_budget(0);
    assert(cache.stats().entries == 0 && cache.stats().resident_bytes == 0);
    cache.insert(a, input, pixels, 1);
    assert(!cache.copy(a, input, output, result));
    cache.set_budget(16);
    cache.insert(a, input, pixels, 32);
    assert(cache.copy(a, input, output, result) && result == 32);
    Cache disabled(0);
    assert(!disabled.eligible(1, 1));
    disabled.insert(a, input, pixels, 1);
    assert(!disabled.copy(a, input, output, result) && disabled.stats().resident_bytes == 0);
    Cache bounded_entries(1024, 1);
    bounded_entries.insert(a, input, pixels, 1);
    bounded_entries.insert(b, input, pixels, 1);
    assert(bounded_entries.stats().entries == 1);
    Cache shared(4096, 16);
    std::vector<std::thread> workers;
    for (unsigned n = 0; n < 8; ++n)
        workers.emplace_back([&, n] {
            Cache::Key key{ n, 8, 8, 0 };
            for (int i = 0; i < 1000; ++i) {
                std::array<uint8_t, 4> out{};
                uint32_t used = 0;
                if (!shared.copy(key, input, out, used))
                    shared.insert(key, input, pixels, 32);
                else
                    assert(used == 32 && std::equal(out.begin(), out.end(), pixels.begin()));
            }
        });
    for (auto &worker : workers)
        worker.join();
    assert(shared.stats().entries == 8 && shared.stats().resident_bytes <= 4096);
}
