// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/exact_pipeline_recipes.h>

namespace renderer {
// Caller owns synchronization. Keep complete observed states, not a shader cross-product.
template <typename Pass>
class PipelineStateHistory {
public:
    struct Entry {
        ExactPipelineRecipe recipe;
        Pass pass;
        bool interlock;
    };
    static constexpr size_t limit = 8;
    void clear() { states.clear(); }
    size_t size() const { return states.size(); }
    void observe(const ExactPipelineRecipe &recipe, Pass pass, bool interlock) {
        std::erase_if(states, [&](const auto &e) { return e.interlock == interlock && e.recipe.words == recipe.words; });
        if (states.size() == limit)
            states.pop_back();
        states.insert(states.begin(), { recipe, pass, interlock });
    }
    auto candidates(const std::array<uint8_t, 32> &vertex, const std::array<uint8_t, 32> &fragment, bool interlock) const {
        std::vector<Entry> result;
        for (const auto &e : states)
            if (e.interlock == interlock)
                result.push_back(e);
        const auto score = [&](const auto &e) { return (e.recipe.fragment == fragment ? 4 : 0) + (e.recipe.vertex == vertex ? 2 : 0); };
        std::stable_sort(result.begin(), result.end(), [&](const auto &a, const auto &b) { return score(a) > score(b); });
        return result;
    }

private:
    std::vector<Entry> states;
};
} // namespace renderer
