// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <renderer/pipeline_binding_epochs.h>

int main() {
    renderer::PipelineBindingEpochs<int> epochs;
    auto vertex = std::make_shared<int>(1), fragment = std::make_shared<int>(2);
    assert(epochs.observe(1, vertex, fragment));
    assert(epochs.notify(1, 0));
    assert(!epochs.observe(1, vertex, fragment));
    for (unsigned i = 0; i < 10000; ++i)
        assert(!epochs.notify(1, 0));
    assert(epochs.notify(1, 1)); // new knowledge reopens unchanged bindings
    assert(!epochs.notify(1, 1));
    // Replacement must register even while the old object is still retained by a worker.
    auto replacement = std::make_shared<int>(1);
    assert(epochs.observe(1, replacement, fragment));
    assert(epochs.notify(1, 1)); // recreated bindings reset the notification
    assert(!epochs.observe(1, replacement, fragment));
    std::weak_ptr<int> old = fragment;
    fragment.reset();
    assert(old.expired()); // the registry must not keep guest bindings alive
    fragment = std::make_shared<int>(2);
    assert(epochs.observe(1, replacement, fragment));
    for (size_t i = 2; i <= epochs.limit; ++i)
        assert(epochs.observe(i, replacement, fragment));
    assert(!epochs.observe(epochs.limit + 1, replacement, fragment));
    assert(epochs.notify(epochs.limit + 1, 1)); // saturation cannot block an uncached pair
    assert(epochs.size() == epochs.limit);
    assert(epochs.observe(1, vertex, fragment)); // refresh still works at capacity
    epochs.clear();
    assert(epochs.observe(1, vertex, fragment));
}
