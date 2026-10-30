// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <renderer/pipeline_prediction_admission.h>

int main() {
    using Admission = renderer::PipelinePredictionAdmission;
    Admission a;
    // Hints cannot consume the actual-pair burst or its reserved object capacity.
    assert(a.acquire(0, false));
    assert(a.acquire(0, false));
    assert(!a.acquire(0, false));
    for (int i = 0; i < 8; ++i)
        assert(a.acquire(0, true));
    assert(!a.acquire(0, true));
    assert(!a.available(249999999, true));
    assert(a.acquire(250000000, true));
    // Releasing an object does not refund compile-rate tokens.
    assert(a.release(true));
    assert(!a.acquire(250000000, true));
    assert(a.acquire(500000000, true));
    assert(a.acquire(2000000000, false));
    assert(a.acquire(4000000000, false));
    assert(a.hints() == Admission::hint_limit);
    assert(!a.acquire(6000000000, false));
    while (a.live() < Admission::live_limit)
        assert(a.acquire(6000000000, true));
    assert(!a.acquire(9000000000, true));
    assert(a.release(false));
    assert(a.acquire(9000000000, true));
    // A long session can admit later effects, without expanding its live-object pool.
    for (uint64_t i = 10; i < 10000; ++i) {
        assert(a.release(true));
        assert(a.acquire(i * 1000000000ULL, true));
        assert(a.live() == Admission::live_limit);
    }
    // Reclamation never takes a compiling object or an object awaited by a draw.
    assert(!Admission::reclaimable(3000000000ULL, 1000000000ULL, false, false));
    assert(!Admission::reclaimable(3000000000ULL, 1000000000ULL, true, true));
    assert(!Admission::reclaimable(2999999999ULL, 1000000000ULL, true, false));
    assert(Admission::reclaimable(3000000000ULL, 1000000000ULL, true, false));
    assert(!Admission::reclaimable(0, 1000000000ULL, true, false));
    Admission clean;
    assert(!clean.release(true) && !clean.release(false));
    assert(clean.acquire(1000000000ULL, true));
    assert(clean.available(0, true)); // clock regression cannot create new tokens
    for (int i = 0; i < 7; ++i)
        assert(clean.acquire(0, true));
    assert(!clean.acquire(0, true));
}
