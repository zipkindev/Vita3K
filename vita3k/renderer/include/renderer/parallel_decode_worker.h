// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace renderer {
// One persistent helper, at most one borrowed task, no queued backlog.
// Concurrent callers fall back to inline decoding. Every dispatched task finishes
// before try_run returns or propagates an exception, so no input/output escapes.
class ParallelDecodeWorker {
public:
    ParallelDecodeWorker()
        : worker([this] { loop(); }) {}
    ParallelDecodeWorker(const ParallelDecodeWorker &) = delete;
    ParallelDecodeWorker &operator=(const ParallelDecodeWorker &) = delete;
    ~ParallelDecodeWorker() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_one();
        worker.join();
    }

    bool try_run(std::function<void()> helper, const std::function<void()> &caller) {
        std::unique_lock admission(gate, std::try_to_lock);
        if (!admission.owns_lock())
            return false;
        {
            std::lock_guard lock(mutex);
            task = std::move(helper);
            failure = nullptr;
            complete = false;
        }
        changed.notify_one();
        std::exception_ptr caller_failure;
        try {
            caller();
        } catch (...) {
            caller_failure = std::current_exception();
        }
        std::unique_lock lock(mutex);
        finished.wait(lock, [this] { return complete; });
        if (caller_failure)
            std::rethrow_exception(caller_failure);
        if (failure)
            std::rethrow_exception(failure);
        return true;
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> work;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [this] { return stopping || bool(task); });
                if (stopping)
                    return;
                work = std::move(task);
                task = nullptr;
            }
            std::exception_ptr error;
            try {
                work();
            } catch (...) {
                error = std::current_exception();
            }
            // Release any borrowed captures before reporting completion.
            work = nullptr;
            {
                std::lock_guard lock(mutex);
                failure = error;
                complete = true;
            }
            finished.notify_one();
        }
    }
    std::mutex gate, mutex;
    std::condition_variable changed, finished;
    std::function<void()> task;
    std::exception_ptr failure;
    bool complete = true, stopping = false;
    std::thread worker;
};
} // namespace renderer
