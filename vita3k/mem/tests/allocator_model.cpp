// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <mem/allocator.h>
#include <random>
#include <vector>

// Reference model enumerates individual slots; it does not use word/bit scans.
static int expected(const std::vector<bool> &free, unsigned start, unsigned size, bool best) {
    int result = -1;
    size_t best_length = free.size() + 1;
    for (size_t i = start & ~31u; i < free.size();) {
        if (!free[i]) {
            ++i;
            continue;
        }
        const auto begin = i;
        while (i < free.size() && free[i])
            ++i;
        const auto length = i - begin;
        if (length >= size && (!best || length < best_length)) {
            result = static_cast<int>(begin);
            best_length = length;
            if (!best)
                break;
        }
    }
    return result;
}
int main() {
    std::mt19937 random(0x3a23);
    unsigned operations = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (unsigned slots : { 1u, 7u, 31u, 32u, 33u, 63u, 64u, 65u, 127u, 512u, 1025u }) {
        BitmapAllocator allocator(slots);
        std::vector<bool> free(slots, true);
        std::vector<std::pair<unsigned, unsigned>> blocks;
        for (unsigned step = 0; step < 20000; ++step) {
            if (!blocks.empty() && random() % 3 == 0) {
                const auto index = random() % blocks.size();
                const auto [offset, size] = blocks[index];
                allocator.free(offset, size);
                for (unsigned i = offset; i < offset + size; ++i) {
                    assert(!free[i]);
                    free[i] = true;
                }
                blocks.erase(blocks.begin() + index);
            } else {
                unsigned size = 1 + random() % (slots + 1);
                const auto requested = size;
                const auto start = random() % (slots + 32);
                const bool best = random() % 2;
                const auto want = expected(free, start, size, best);
                const auto got = allocator.allocate_from(start, size, best);
                if (got != want)
                    std::cerr << "slots=" << slots << " step=" << step << " start=" << start << " size=" << requested << " best=" << best << " expected=" << want << " got=" << got << "\n";
                assert(got == want);
                assert(size == requested);
                if (got >= 0) {
                    assert(unsigned(got) + size <= slots);
                    for (unsigned i = got; i < unsigned(got) + size; ++i) {
                        assert(free[i]);
                        free[i] = false;
                    }
                    blocks.emplace_back(got, size);
                }
            }
            // Every visible slot and random free-count range must match.
            for (unsigned i = 0; i < slots; ++i)
                assert(bool(allocator.words[i / 32] & (uint32_t(1) << (31 - i % 32))) == free[i]);
            const auto first = random() % slots;
            const auto end = first + 1 + random() % (slots - first);
            assert(allocator.free_slot_count(first, end) == std::count(free.begin() + first, free.begin() + end, true));
            ++operations;
        }
        // Newly appended words/partial-word tail are free, existing bits retained.
        allocator.set_maximum(slots + 65);
        for (unsigned i = slots; i < slots + 65; ++i)
            assert(allocator.words[i / 32] & (uint32_t(1) << (31 - i % 32)));
    }
    // Unused tail bits past max_offset must not inflate the last free run.
    BitmapAllocator tail(7);
    assert(tail.allocate_at(2, 4) == 0);
    unsigned single = 1;
    assert(tail.allocate_from(0, single, true) == 6);
    BitmapAllocator empty(0);
    unsigned size = 1;
    assert(empty.allocate_from(0, size) == -1);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    std::cout << operations << " randomized allocator operations match slot model; elapsed " << ms << " ms (host test, not Go benchmark)\n";
}
