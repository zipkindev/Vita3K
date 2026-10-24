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
    assert(bounded_entries.stats().entry_evictions == 1 && bounded_entries.stats().byte_evictions == 0);
    // Many small textures must fit beyond the old4096-entry limit while the
    // byte budget remains512MiB. The new default still has a hard entry bound.
    Cache many_small(size_t(512) * 1024 * 1024);
    for (uint64_t n = 0; n < 16384; ++n)
        many_small.insert({ n, 8, 8, 0 }, input, pixels, 32);
    assert(many_small.stats().entries == 16384 && many_small.stats().evictions == 0);
    assert(many_small.stats().budget_bytes == size_t(512) * 1024 * 1024);
    assert(many_small.stats().resident_bytes <= many_small.stats().budget_bytes);
    assert(many_small.copy({ 0, 8, 8, 0 }, input, output, result));
    many_small.insert({ 16384, 8, 8, 0 }, input, pixels, 32);
    assert(many_small.stats().entries == 16384 && many_small.stats().evictions == 1);
    assert(!many_small.copy({ 1, 8, 8, 0 }, input, output, result));
    assert(many_small.copy({ 0, 8, 8, 0 }, input, output, result));
    assert(many_small.copy({ 16384, 8, 8, 0 }, input, output, result));
    // Raising the entry cap must not relax the independent payload byte bound.
    Cache byte_limited(16);
    byte_limited.insert(a, input, pixels, 32);
    byte_limited.insert(b, input, pixels, 32);
    byte_limited.insert(c, input, pixels, 32);
    assert(byte_limited.stats().entries == 2 && byte_limited.stats().evictions == 1);
    assert(byte_limited.stats().resident_bytes <= 16);
    assert(!byte_limited.copy(a, input, output, result));
    assert(byte_limited.copy(c, input, output, result));
    assert(byte_limited.stats().byte_evictions == 1 && byte_limited.stats().entry_evictions == 0);
    Cache both_limited(8, 1);
    both_limited.insert(a, input, pixels, 1);
    both_limited.insert(b, input, pixels, 1);
    assert(both_limited.stats().both_evictions == 1);
    both_limited.set_budget(0);
    assert(both_limited.stats().clear_evictions == 1 && both_limited.stats().evictions == 2);
    both_limited.decoded(1000000, 8, 8);
    both_limited.decoded(12000000, 16, 16);
    both_limited.decoded(60000000, 32, 32);
    const auto peak = both_limited.telemetry_stats();
    assert(peak.decodes == 3 && peak.decode_ns == 73000000);
    assert(peak.decode_ge2ms == 2 && peak.decode_ge10ms == 2 && peak.decode_ge50ms == 1);
    assert(peak.interval_max_decode_ns == 60000000 && peak.max_decode_ns == 60000000);
    assert(peak.interval_max_width == 32 && peak.interval_max_height == 32);
    const auto reset = both_limited.telemetry_stats();
    assert(reset.interval_max_decode_ns == 0 && reset.interval_max_width == 0);
    assert(reset.decode_ns == peak.decode_ns && reset.max_decode_ns == peak.max_decode_ns);
    // Reuse diagnostics must distinguish capacity removal from explicit clear,
    // preserve actual decoded pixels, and carry miss classification to decode.
    Cache measured(8, 1);
    Cache::Observation observation;
    assert(!measured.stats().reuse.enabled);
    measured.acquire_reuse_tracking();
    measured.acquire_reuse_tracking(); // Two writers share one history epoch.
    assert(!measured.copy(a, input, output, result, &observation));
    assert(observation.classification == Cache::MissClass::FirstObserved);
    measured.decoded(100, 8, 8, observation);
    assert(measured.insert(a, input, pixels, 32));
    assert(measured.copy(a, input, output, result, &observation));
    assert(observation.classification == Cache::MissClass::Untracked);
    assert(result == 32 && std::equal(output.begin(), output.end(), pixels.begin()));
    assert(measured.insert(b, input, pixels, 32));
    assert(!measured.copy(a, input, output, result, &observation));
    assert(observation.classification == Cache::MissClass::CapacityEvicted);
    measured.decoded(200, 8, 8, observation);
    // A second outstanding miss is seen but cannot be attributed to a second eviction.
    assert(!measured.copy(a, input, output, result, &observation));
    assert(observation.classification == Cache::MissClass::SeenOther);
    measured.decoded(300, 8, 8, observation);
    assert(measured.insert(a, input, pixels, 32));
    measured.clear();
    assert(!measured.copy(a, input, output, result, &observation));
    assert(observation.classification == Cache::MissClass::Cleared);
    measured.decoded(400, 8, 8, observation);
    assert(measured.insert(a, input, pixels, 32, 100, false));
    assert(measured.copy(a, input, output, result));
    measured.set_budget(16);
    assert(!measured.copy(a, input, output, result, &observation));
    assert(observation.classification == Cache::MissClass::Cleared);
    measured.decoded(500, 8, 8, observation);
    measured.decoded(600); // Bypassed decoding has no eligible identity.
    auto reuse = measured.stats().reuse;
    assert((reuse.misses == std::array<uint64_t, 4>{ 1, 1, 2, 1 }));
    assert((reuse.decodes == std::array<uint64_t, 4>{ 1, 1, 2, 1 }));
    assert((reuse.decode_ns == std::array<uint64_t, 4>{ 100, 200, 900, 300 }));
    assert(reuse.preloaded_hits == 1 && reuse.demand_hits == 1 && reuse.untracked_decodes == 1);
    assert(reuse.epochs == 1 && reuse.history_entries == 2 && reuse.history_capacity == 65536);
    measured.release_reuse_tracking();
    assert(measured.stats().reuse.enabled);
    // Exercise bounded history replacement; forgotten identities become first
    // observed again, not first-ever. Telemetry never changes hit validation.
    for (uint64_t n = 100; n < 100100; ++n)
        measured.copy({ n, 8, 8, 0 }, input, output, result, &observation);
    reuse = measured.stats().reuse;
    assert(reuse.history_entries <= reuse.history_capacity && reuse.history_replacements > 0);
    measured.release_reuse_tracking();
    assert(!measured.stats().reuse.enabled);
    measured.acquire_reuse_tracking();
    assert(measured.stats().reuse.epochs == 2 && measured.stats().reuse.history_entries == 0);
    measured.release_reuse_tracking();
    Cache shared(4096, 16);
    shared.acquire_reuse_tracking();
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
    assert(shared.stats().reuse.history_entries == 8);
    shared.release_reuse_tracking();
}
