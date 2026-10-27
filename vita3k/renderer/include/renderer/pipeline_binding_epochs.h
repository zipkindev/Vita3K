// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

namespace renderer {
// A program identity can outlive its patcher bindings. Only suppress a repeated
// observation while both actual binding objects are still the same. Caller locks.
template <typename Binding>
class PipelineBindingEpochs {
    std::unordered_map<uint64_t, std::pair<std::weak_ptr<Binding>, std::weak_ptr<Binding>>> entries;

public:
    static constexpr size_t limit = 2048;
    bool observe(uint64_t variant, const std::shared_ptr<Binding> &vertex, const std::shared_ptr<Binding> &fragment) {
        auto found = entries.find(variant);
        if (found != entries.end()) {
            if (found->second.first.lock() == vertex && found->second.second.lock() == fragment)
                return false;
            found->second = { vertex, fragment };
            return true;
        }
        if (entries.size() >= limit)
            return false;
        entries.emplace(variant, std::pair{ vertex, fragment });
        return true;
    }
    size_t size() const { return entries.size(); }
    void clear() { entries.clear(); }
};
} // namespace renderer
