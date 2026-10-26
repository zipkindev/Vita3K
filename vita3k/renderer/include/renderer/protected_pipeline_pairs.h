// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <istream>
#include <ostream>
#include <vector>

namespace renderer {
// Program identities only: no pointers, shader bytes, Vulkan handles or guessed cross-products.
struct ProtectedPipelinePair {
    std::array<uint8_t, 32> vertex{}, fragment{};
    uint32_t output_format = 0;
    bool operator==(const ProtectedPipelinePair &) const = default;
};
class ProtectedPipelinePairs {
public:
    static constexpr size_t limit = 1024;
    const std::vector<ProtectedPipelinePair> &entries() const { return pairs; }
    bool learn(const ProtectedPipelinePair &pair) {
        if (pair.output_format > 8 || pairs.size() >= limit || std::find(pairs.begin(), pairs.end(), pair) != pairs.end())
            return false;
        pairs.push_back(pair);
        return true;
    }
    bool read(std::istream &in) {
        std::array<uint8_t, 20> header{};
        in.read(reinterpret_cast<char *>(header.data()), header.size());
        constexpr std::array<uint8_t, 8> magic{ 'V', '3', 'P', 'P', 'A', 'I', 'R', 1 };
        if (!in || !std::equal(magic.begin(), magic.end(), header.begin()))
            return false;
        uint32_t count = 0;
        uint64_t expected = 0;
        for (unsigned i = 0; i < 4; ++i)
            count |= uint32_t(header[8 + i]) << (8 * i);
        for (unsigned i = 0; i < 8; ++i)
            expected |= uint64_t(header[12 + i]) << (8 * i);
        if (count > limit)
            return false;
        std::vector<uint8_t> payload(size_t(count) * 68);
        in.read(reinterpret_cast<char *>(payload.data()), payload.size());
        if (!in || in.peek() != std::char_traits<char>::eof() || checksum(payload) != expected)
            return false;
        ProtectedPipelinePairs parsed;
        for (size_t i = 0; i < count; ++i) {
            ProtectedPipelinePair pair;
            const auto p = payload.begin() + i * 68;
            std::copy_n(p, 32, pair.vertex.begin());
            std::copy_n(p + 32, 32, pair.fragment.begin());
            for (unsigned b = 0; b < 4; ++b)
                pair.output_format |= uint32_t(p[64 + b]) << (8 * b);
            if (!parsed.learn(pair))
                return false;
        }
        pairs = std::move(parsed.pairs);
        return true;
    }
    bool write(std::ostream &out) const {
        std::vector<uint8_t> payload;
        payload.reserve(pairs.size() * 68);
        for (const auto &pair : pairs) {
            payload.insert(payload.end(), pair.vertex.begin(), pair.vertex.end());
            payload.insert(payload.end(), pair.fragment.begin(), pair.fragment.end());
            for (unsigned i = 0; i < 4; ++i)
                payload.push_back(uint8_t(pair.output_format >> (8 * i)));
        }
        std::array<uint8_t, 20> header{ 'V', '3', 'P', 'P', 'A', 'I', 'R', 1 };
        for (unsigned i = 0; i < 4; ++i)
            header[8 + i] = uint8_t(pairs.size() >> (8 * i));
        const auto sum = checksum(payload);
        for (unsigned i = 0; i < 8; ++i)
            header[12 + i] = uint8_t(sum >> (8 * i));
        out.write(reinterpret_cast<const char *>(header.data()), header.size());
        out.write(reinterpret_cast<const char *>(payload.data()), payload.size());
        return bool(out);
    }

private:
    static uint64_t checksum(const std::vector<uint8_t> &data) {
        uint64_t h = 14695981039346656037ULL;
        for (auto b : data)
            h = (h ^ b) * 1099511628211ULL;
        return h;
    }
    std::vector<ProtectedPipelinePair> pairs;
};
} // namespace renderer
