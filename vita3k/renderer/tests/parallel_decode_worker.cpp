// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <future>
#include <renderer/parallel_decode_worker.h>
#include <stdexcept>

int main() {
    renderer::ParallelDecodeWorker worker;
    std::atomic<int> completed = 0;
    for (int i = 0; i < 100; ++i)
        assert(worker.try_run([&] { ++completed; }, [&] { ++completed; }));
    assert(completed == 200);
    // Keep an admitted job alive, then prove concurrent admission never queues.
    std::promise<void> started, release;
    auto released = release.get_future().share();
    std::thread owner([&] {
        assert(worker.try_run([&] { started.set_value(); released.wait(); }, [] {}));
    });
    started.get_future().wait();
    assert(!worker.try_run([] { assert(false); }, [] { assert(false); }));
    release.set_value();
    owner.join();
    // Both failure paths complete borrowed work before propagating, and recover.
    for (bool helper_fails : { false, true }) {
        std::atomic<bool> other_done = false;
        bool caught = false;
        try {
            worker.try_run([&] {
                if (helper_fails)
                    throw std::runtime_error("helper");
                other_done = true; }, [&] {
                if (!helper_fails)
                    throw std::runtime_error("caller");
                other_done = true; });
        } catch (const std::runtime_error &) {
            caught = true;
        }
        assert(caught && other_done);
        assert(worker.try_run([] {}, [] {}));
    }
    // Repeated idle shutdown after completed borrowed stack work.
    for (int i = 0; i < 20; ++i) {
        renderer::ParallelDecodeWorker transient;
        int a = 0, b = 0;
        assert(transient.try_run([&] { a = 1; }, [&] { b = 2; }));
        assert(a == 1 && b == 2);
    }
}
