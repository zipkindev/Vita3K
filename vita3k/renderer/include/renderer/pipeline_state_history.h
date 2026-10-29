// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/exact_pipeline_recipes.h>
#include <deque>
#include <unordered_set>

namespace renderer {
// Caller owns synchronization. Keep complete observed states, not a shader cross-product.
template <typename Pass>
class PipelineStateHistory {
public:
    struct Entry {
        ExactPipelineRecipe recipe;
        Pass pass;
        bool interlock;
        uint64_t revision;
    };
    static constexpr size_t limit = 64, pair_limit = 8, drawn_limit = 4096;
    uint64_t generation() const { return revision; }
    void clear() { states.clear(); drawn_keys.clear(); drawn_order.clear(); revision = 0; }
    size_t size() const { return states.size(); }
    void observe(const ExactPipelineRecipe &recipe, Pass pass, bool interlock) {
        // Mirrors the render cache's pipeline-key identity. Remember more drawn keys
        // than full templates to avoid recompiling an old pipeline after history eviction.
        if (drawn_keys.insert(recipe.pipeline_key).second) {
            if (drawn_order.size() == drawn_limit) {
                drawn_keys.erase(drawn_order.front());
                drawn_order.pop_front();
            }
            drawn_order.push_back(recipe.pipeline_key);
        }
        const auto same = [&](const auto &e) { return e.interlock == interlock && e.recipe == recipe; };
        const auto old = std::find_if(states.begin(), states.end(), same);
        const uint64_t state_revision = old == states.end() ? ++revision : old->revision;
        std::erase_if(states, same);
        // Preserve different shader associations for identical fixed state. Bound each
        // pair as well as total history so a busy effect cannot consume every slot.
        size_t count = 0;
        std::erase_if(states, [&](const auto &e) {
            return e.interlock == interlock && e.recipe.vertex == recipe.vertex && e.recipe.fragment == recipe.fragment
                && ++count >= pair_limit;
        });
        if (states.size() == limit)
            states.pop_back();
        states.insert(states.begin(), { recipe, pass, interlock, state_revision });
    }
    bool observed(const ExactPipelineRecipe &recipe) const {
        return drawn_keys.contains(recipe.pipeline_key);
    }
    uint64_t relevant_generation(const std::array<uint8_t, 32> &vertex, const std::array<uint8_t, 32> &fragment, bool interlock = false) const {
        uint64_t result = 0;
        for (const auto &e : states)
            if (e.interlock == interlock && (e.recipe.vertex == vertex || e.recipe.fragment == fragment))
                result = std::max(result, e.revision);
        return result;
    }
    auto candidates(const std::array<uint8_t, 32> &vertex, const std::array<uint8_t, 32> &fragment, bool interlock) const {
        std::vector<Entry> result;
        for (const auto &e : states)
            if (e.interlock == interlock && (e.recipe.vertex == vertex || e.recipe.fragment == fragment))
                result.push_back(e);
        const auto score = [&](const auto &e) { return (e.recipe.fragment == fragment ? 4 : 0) + (e.recipe.vertex == vertex ? 2 : 0); };
        std::stable_sort(result.begin(), result.end(), [&](const auto &a, const auto &b) { return score(a) > score(b); });
        return result;
    }

private:
    std::vector<Entry> states;
    uint64_t revision = 0;
    std::unordered_set<uint64_t> drawn_keys;
    std::deque<uint64_t> drawn_order;
};
} // namespace renderer
