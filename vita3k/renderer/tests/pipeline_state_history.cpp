// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <renderer/pipeline_state_history.h>
int main() {
    renderer::PipelineStateHistory<unsigned> h;
    renderer::ExactPipelineRecipe a;
    a.pipeline_key = 1; a.vertex[0] = 1; a.fragment[0] = 2; a.words[7] = 1;
    a.words[20] = 12582912; a.words[46] = 2197815296;
    h.observe(a, 10, false);
    auto b = a; b.pipeline_key = 2; b.vertex[0] = 3; b.fragment[0] = 4;
    h.observe(b, 20, false);
    assert(h.size() == 2); // identical state must retain both shader associations
    auto found = h.candidates(a.vertex, a.fragment, false);
    assert(found[0].recipe == a && found[0].pass == 10);
    auto generation = h.generation();
    h.observe(a, 30, false);
    assert(h.generation() == generation && h.size() == 2);
    found = h.candidates(a.vertex, a.fragment, false);
    assert(found[0].pass == 30 && h.observed(a));
    h.observe(a, 40, true);
    assert(h.candidates(a.vertex, a.fragment, true).size() == 1);
    for (unsigned i = 0; i < 1000; ++i) {
        b.pipeline_key = i + 10;
        b.words[46] = i;
        h.observe(b, i, false);
    }
    found = h.candidates(b.vertex, b.fragment, false);
    size_t same_pair = 0;
    for (auto &e : found)
        if (e.recipe.vertex == b.vertex && e.recipe.fragment == b.fragment)
            ++same_pair;
    assert(same_pair == h.pair_limit && found.front().pass == 999);
    assert(h.observed(a)); // a busy unrelated pair didn't evict this association
    for (unsigned i = 0; i < 1000; ++i) {
        b.pipeline_key = i + 2000;
        b.vertex[0] = i & 255; b.vertex[1] = i >> 8;
        b.fragment = b.vertex;
        h.observe(b, i, false);
        assert(h.size() <= h.limit);
    }
    assert(h.size() == h.limit);
    found = h.candidates(b.vertex, b.fragment, false);
    assert(found.size() == 1 && found.front().pass == 999);
    assert(h.observed(a)); // template eviction must not forget a recently drawn pipeline
    for (unsigned i = 0; i < h.drawn_limit + 1; ++i) {
        b.pipeline_key = 10000 + i;
        h.observe(b, i, false);
    }
    assert(!h.observed(a) && h.observed(b)); // key history is independently bounded
    h.clear(); assert(h.size() == 0 && h.generation() == 0);
    h.observe(a, 1, false);
    const auto available = h.relevant_generation(a.vertex, a.fragment);
    for (unsigned i = 0; i < 65; ++i) {
        auto different = b; different.vertex = {}; different.fragment = {};
        different.vertex[0] = 50; different.vertex[1] = i;
        different.fragment[0] = 51; different.fragment[1] = i;
        different.pipeline_key = 60000 + i;
        h.observe(different, i, false);
    }
    assert(h.candidates(a.vertex, a.fragment, false).empty());
    assert(h.relevant_generation(a.vertex, a.fragment) == available); // template loss isn't new work
    h.observe(a, 2, false);
    assert(h.relevant_generation(a.vertex, a.fragment) > available); // a returning candidate can unblock an untried pair
    auto restored = h.relevant_generation(a.vertex, a.fragment);
    h.observe(a, 3, false);
    assert(h.relevant_generation(a.vertex, a.fragment) == restored);
    h.clear();
    h.observe(a, 1, false);
    const auto relevant = h.relevant_generation(a.vertex, a.fragment);
    assert(relevant != 0);
    unsigned changes = 0;
    auto previous = relevant;
    for (unsigned i = 0; i < 10000; ++i) {
        b.vertex = {}; b.fragment = {};
        b.vertex[0] = 99; b.fragment[0] = 98;
        b.vertex[1] = i & 255; b.vertex[2] = i >> 8;
        b.fragment[1] = i & 255; b.fragment[2] = i >> 8;
        b.pipeline_key = i + 50000;
        h.observe(b, i, false);
        auto g = h.relevant_generation(a.vertex, a.fragment);
        changes += g != previous;
        previous = g;
        assert(h.candidates(a.vertex, a.fragment, false).size() <= 1);
    }
    assert(changes == 1 && previous == 0); // only bounded dependency-registry eviction changes it

}
