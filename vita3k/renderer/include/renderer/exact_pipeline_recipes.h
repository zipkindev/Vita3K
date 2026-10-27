// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <istream>
#include <ostream>
#include <vector>

namespace renderer {
// Versioned scalar state only. No guest addresses, Vulkan handles, pointers or shader bytes.
struct ExactPipelineRecipe {
    std::array<uint8_t, 32> vertex{}, fragment{};
    uint64_t layout = 0, blend = 0, pipeline_key = 0;
    std::array<uint32_t, 64> words{};
    auto operator<=>(const ExactPipelineRecipe &) const = default;
};
class ExactPipelineRecipes {
    std::vector<ExactPipelineRecipe> recipes;
    static uint64_t checksum(const std::vector<uint8_t> &bytes) {
        uint64_t h = 14695981039346656037ULL;
        for (auto b : bytes)
            h = (h ^ b) * 1099511628211ULL;
        return h;
    }

public:
    static constexpr size_t limit = 1024, entry_bytes = 344;
    const auto &entries() const { return recipes; }
    bool learn(const ExactPipelineRecipe &recipe) {
        if (recipes.size() >= limit || std::find(recipes.begin(), recipes.end(), recipe) != recipes.end())
            return false;
        recipes.push_back(recipe);
        return true;
    }
    bool write(std::ostream &out) const {
        std::vector<uint8_t> bytes;
        const auto append = [&](uint64_t v, unsigned n) { for (unsigned i = 0; i < n; ++i) bytes.push_back(uint8_t(v >> (8 * i))); };
        for (const auto &r : recipes) {
            bytes.insert(bytes.end(), r.vertex.begin(), r.vertex.end());
            bytes.insert(bytes.end(), r.fragment.begin(), r.fragment.end());
            append(r.layout, 8);
            append(r.blend, 8);
            append(r.pipeline_key, 8);
            for (auto w : r.words)
                append(w, 4);
        }
        const auto sum = checksum(bytes);
        std::vector<uint8_t> payload = std::move(bytes);
        bytes = { 'V', '3', 'E', 'X', 'A', 'C', 'T', 1 };
        append(recipes.size(), 4);
        append(sum, 8);
        out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        out.write(reinterpret_cast<const char *>(payload.data()), payload.size());
        return bool(out);
    }
    bool read(std::istream &in) {
        std::array<uint8_t, 20> header{};
        in.read(reinterpret_cast<char *>(header.data()), header.size());
        constexpr std::array<uint8_t, 8> magic{ 'V', '3', 'E', 'X', 'A', 'C', 'T', 1 };
        if (!in || !std::equal(magic.begin(), magic.end(), header.begin()))
            return false;
        const auto number = [](const uint8_t *p, unsigned n) { uint64_t v = 0; for (unsigned i = 0; i < n; ++i) v |= uint64_t(p[i]) << (8 * i); return v; };
        const auto count = number(header.data() + 8, 4);
        if (count > limit)
            return false;
        std::vector<uint8_t> bytes(count * entry_bytes);
        in.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
        if (!in || in.peek() != std::char_traits<char>::eof() || checksum(bytes) != number(header.data() + 12, 8))
            return false;
        ExactPipelineRecipes parsed;
        for (size_t i = 0; i < count; ++i) {
            const auto *p = bytes.data() + i * entry_bytes;
            ExactPipelineRecipe r;
            std::copy_n(p, 32, r.vertex.begin());
            std::copy_n(p + 32, 32, r.fragment.begin());
            r.layout = number(p + 64, 8);
            r.blend = number(p + 72, 8);
            r.pipeline_key = number(p + 80, 8);
            for (size_t w = 0; w < r.words.size(); ++w)
                r.words[w] = uint32_t(number(p + 88 + 4 * w, 4));
            if (!parsed.learn(r))
                return false;
        }
        recipes = std::move(parsed.recipes);
        return true;
    }
};
} // namespace renderer
