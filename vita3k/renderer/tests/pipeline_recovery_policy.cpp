// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <renderer/pipeline_recovery_policy.h>
int main() {
    renderer::PipelineRecoveryPolicy p;
    p.complete(100, 1, 1, 1024); // no related state: don't poll or spin
    assert(!p.eligible(10000000000ULL, 1, 1));
    assert(!p.eligible(101, 2, 1)); // new knowledge must not bypass the cooldown
    assert(p.eligible(p.retry_at, 2, 1));
    p.complete(101, 2, 1, 256); // exact object already owned
    assert(!p.eligible(10000000000ULL, 2, 1));
    assert(!p.eligible(102, 2, 2));
    assert(p.eligible(p.retry_at, 2, 2));
    p.complete(102, 2, 2, 512); // temporary busy pressure
    assert(!p.eligible(101, 2, 2)); // clock regression must not bypass backoff
    assert(!p.eligible(p.retry_at - 1, 2, 2));
    assert(p.eligible(p.retry_at, 2, 2));
    uint64_t now = p.retry_at;
    for (unsigned i = 0; i < 10000; ++i) {
        p.complete(now, 2, 2, 128);
        assert(p.retry_at > now && p.retry_at - now <= 2000000000ULL);
        assert(!p.eligible(now, 2, 2));
        now = p.retry_at;
        assert(p.eligible(now, 2, 2)); // no lifetime shutdown during a long session
    }
    p.complete(now, 3, 3, 32);
    assert(!p.eligible(now + 10000000000ULL, 3, 3)); // ready stays quiet
    assert(!p.eligible(now + 1, 4, 3));
    assert(p.eligible(p.retry_at, 4, 3)); // new scenes recover after bounded backoff
}
