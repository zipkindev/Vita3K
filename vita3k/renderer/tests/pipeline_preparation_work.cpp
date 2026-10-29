// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <renderer/pipeline_preparation_work.h>
int main() {
    using namespace renderer;
    PipelineRecoveryCache cache;
    cache.complete(7, 100, 1, 1, 1024);
    // Recreating or evicting a queue owner does not reset this compact decision.
    for (unsigned i = 0; i < 10000; ++i)
        assert(!cache.eligible(7, 101 + i, 1 + i, 1));
    assert(cache.eligible(7, 250000100, 2, 1));
    assert(!cache.eligible(7, 10000000000ULL, 1, 1));
    for (unsigned i = 0; i < 1000; ++i)
        cache.complete(100 + i, i, 1, 1, 256);
    assert(!cache.eligible(7, 200, 2, 2));
    for (unsigned i = 0; i < 10000; ++i) {
        cache.complete(10000 + i, i, 1, 1, 1024);
        assert(cache.size() <= cache.limit);
    }
    assert(cache.evictions > 0);
    cache.clear(); assert(cache.size() == 0 && cache.evictions == 0);

    PipelinePreparationWorkBudget budget;
    unsigned admitted = 0, scans = 0;
    for (unsigned i = 0; i < 100000; ++i) {
        admitted += budget.acquire(100);
        scans += budget.scan(100);
    }
    assert(admitted == budget.burst && scans == 1); // wakeup storm cannot bypass real time
    assert(!budget.available(99));
    assert(!budget.scan(99));
    assert(budget.acquire(100 + budget.interval_ns));
    assert(!budget.acquire(100 + budget.interval_ns));
    // One second allows only 40 refills plus the original burst, regardless of wakeups.
    budget = {};
    admitted = scans = 0;
    for (uint64_t t = 0; t < 1000000000ULL; t += 10000) {
        admitted += budget.acquire(t);
        scans += budget.scan(t);
    }
    assert(admitted <= budget.burst + 39 && scans == 40);
    budget = {};
    for (unsigned i = 0; i < budget.burst; ++i) assert(budget.acquire(0));
    admitted = 0;
    for (unsigned i = 0; i < 1000; ++i) admitted += budget.acquire(100000000000ULL);
    assert(admitted == budget.burst); // long idle time never creates a catch-up burst

    budget = {};
    for (unsigned i = 0; i < budget.burst - 1; ++i) assert(budget.acquire_background(0));
    for (unsigned i = 0; i < 10000; ++i) assert(!budget.acquire_background(0));
    assert(budget.acquire(0)); // reserved existing token serves a fresh actual pair
    assert(!budget.acquire(0));
    assert(!budget.acquire_background(budget.interval_ns));
    assert(budget.acquire(budget.interval_ns)); // no extra refill/catch-up credit

    PipelineStageReadiness ready;
    std::array<uint8_t, 32> a{}, b{}, unrelated{};
    a[0] = 1; b[0] = 2; unrelated[0] = 3;
    ready.ready(a); auto revision = ready.generation(a, b);
    for (unsigned i = 0; i < 10000; ++i) ready.ready(unrelated);
    assert(ready.generation(a, b) == revision);
    ready.ready(b); assert(ready.generation(a, b) > revision);
    for (unsigned i = 0; i < 10000; ++i) {
        unrelated[1] = i & 255; unrelated[2] = i >> 8;
        ready.ready(unrelated); assert(ready.size() <= ready.limit);
    }
    ready.clear(); assert(ready.size() == 0);
}
