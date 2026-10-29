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
    struct Entry {
        std::weak_ptr<Binding> vertex, fragment;
        uint64_t notified = 0;
        bool has_notification = false;
    };
    std::unordered_map<uint64_t, Entry> entries;

public:
    static constexpr size_t limit = 2048;
    bool observe(uint64_t variant, const std::shared_ptr<Binding> &vertex, const std::shared_ptr<Binding> &fragment) {
        auto found = entries.find(variant);
        if (found != entries.end()) {
            if (found->second.vertex.lock() == vertex && found->second.fragment.lock() == fragment)
                return false;
            found->second = { vertex, fragment };
            return true;
        }
        if (entries.size() >= limit)
            return false;
        entries.emplace(variant, Entry{ vertex, fragment });
        return true;
    }
    bool notify(uint64_t variant, uint64_t knowledge) {
        const auto found = entries.find(variant);
        // Saturation must not suppress a new binding that wasn't remembered.
        if (found == entries.end())
            return true;
        auto &entry = found->second;
        if (entry.has_notification && entry.notified == knowledge)
            return false;
        entry.has_notification = true;
        entry.notified = knowledge;
        return true;
    }
    size_t size() const { return entries.size(); }
    void clear() { entries.clear(); }
};
} // namespace renderer
