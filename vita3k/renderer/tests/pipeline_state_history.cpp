// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <renderer/pipeline_state_history.h>

int main() {
    renderer::PipelineStateHistory<unsigned> history;
    renderer::ExactPipelineRecipe a;
    a.vertex[0] = 1;
    a.fragment[0] = 2;
    a.words[7] = 1; // culling
    a.words[20] = 12582912; // depth function
    a.words[46] = 2197815296; // a non-default texture hint
    history.observe(a, 10, false);
    auto b = a;
    b.fragment[0] = 3;
    b.words[46] = 2264924160;
    history.observe(b, 20, false);
    // Prefer matching fragment state over recency; preserve fixed state and hints.
    auto found = history.candidates(a.vertex, a.fragment, false);
    assert(found.size() == 2 && found[0].pass == 10);
    assert(found[0].recipe.words == a.words);
    // Refresh an existing scalar state, replacing its binding identities and pass.
    a.vertex[0] = 9;
    history.observe(a, 30, false);
    assert(history.size() == 2);
    found = history.candidates({}, {}, false);
    assert(found[0].pass == 30 && found[0].recipe.vertex[0] == 9);
    // Interlock passes must never be predicted for a non-interlock shader, or vice versa.
    history.observe(a, 40, true);
    assert(history.candidates({}, {}, true).size() == 1);
    assert(history.candidates({}, {}, true)[0].pass == 40);
    assert(history.candidates({}, {}, false).size() == 2);
    // Bounded recency across arbitrary later maps; no growth with session length.
    for (unsigned i = 0; i < 1000; ++i) {
        b.words[46] = i;
        history.observe(b, i, false);
        assert(history.size() <= history.limit);
    }
    found = history.candidates({}, {}, false);
    assert(found.size() == history.limit && found.front().pass == 999 && found.back().pass == 992);
    history.clear();
    assert(history.size() == 0 && history.candidates({}, {}, false).empty());
}
