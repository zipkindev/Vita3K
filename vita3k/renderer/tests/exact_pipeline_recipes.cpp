// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <renderer/exact_pipeline_recipes.h>
#include <sstream>
int main() {
    renderer::ExactPipelineRecipes p;
    renderer::ExactPipelineRecipe a;
    a.vertex[31] = 255;
    a.layout = 0x123456789ABCDEF0ULL;
    assert(p.learn(a));
    assert(!p.learn(a));
    for (size_t i = 0; i < a.words.size(); ++i) {
        auto b = a;
        b.words[i] = 1;
        assert(p.learn(b));
    }
    auto b = a;
    b.blend = 7;
    assert(p.learn(b));
    b = a;
    b.fragment[0] = 1;
    assert(p.learn(b));
    std::ostringstream out(std::ios::binary);
    assert(p.write(out));
    const auto good = out.str();
    assert(good.size() == 20 + p.entries().size() * p.entry_bytes);
    renderer::ExactPipelineRecipes restored;
    std::istringstream in(good, std::ios::binary);
    assert(restored.read(in));
    assert(restored.entries() == p.entries());
    for (auto bad : { good.substr(0, 19), good.substr(0, good.size() - 1), good + "x" }) {
        std::istringstream s(bad, std::ios::binary);
        assert(!restored.read(s));
        assert(restored.entries() == p.entries());
    }
    auto bad = good;
    bad.back() ^= 1;
    std::istringstream corrupt(bad, std::ios::binary);
    assert(!restored.read(corrupt));
    bad = good;
    bad[8] = bad[9] = bad[10] = bad[11] = char(255);
    std::istringstream huge(bad, std::ios::binary);
    assert(!restored.read(huge));
    while (p.entries().size() < p.limit) {
        a.pipeline_key++;
        assert(p.learn(a));
    }
    a.pipeline_key++;
    assert(!p.learn(a));
}
