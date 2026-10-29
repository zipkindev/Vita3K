// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace renderer {
// Caller locks. Knowledge changes reopen work; temporary pressure uses bounded
// backoff, never a session-lifetime attempt cap. No driver work in this policy.
struct PipelineRecoveryPolicy {
    static constexpr uint32_t transient = 128 | 512;
    uint64_t history = 0, registry = 0, retry_at = 0;
    uint32_t outcome = 0, attempts = 0;
    bool eligible(uint64_t now, uint64_t h, uint64_t r) const {
        return now >= retry_at && (h != history || r != registry || (outcome & transient));
    }
    void complete(uint64_t now, uint64_t h, uint64_t r, uint32_t result) {
        if (h != history || r != registry)
            attempts = 0;
        history = h;
        registry = r;
        outcome = result;
        attempts = std::min<uint32_t>(attempts + 1, 4);
        retry_at = now + (250000000ULL << (attempts - 1));
    }
};
} // namespace renderer
