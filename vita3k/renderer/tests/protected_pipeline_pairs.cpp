// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <renderer/protected_pipeline_pairs.h>
#include <sstream>

int main() {
    using namespace renderer;
    ProtectedPipelinePairs pairs;
    ProtectedPipelinePair p;
    p.vertex[0] = 7;
    p.fragment[0] = 9;
    assert(pairs.learn(p));
    assert(!pairs.learn(p));
    auto other = p;
    other.output_format = 5;
    assert(pairs.learn(other)); // Same programs can have distinct output-register variants.
    std::ostringstream encoded(std::ios::binary);
    assert(pairs.write(encoded));
    const auto bytes = encoded.str();
    ProtectedPipelinePairs restored;
    std::istringstream valid(bytes, std::ios::binary);
    assert(restored.read(valid) && restored.entries() == pairs.entries());
    // Partial, over-limit, corrupt and trailing inputs must not destroy an existing learned set.
    for (auto bad : { bytes.substr(0, 19), bytes.substr(0, bytes.size() - 1), bytes + "x" }) {
        std::istringstream in(bad, std::ios::binary);
        assert(!restored.read(in) && restored.entries() == pairs.entries());
    }
    auto corrupt = bytes;
    corrupt[20] ^= 1;
    std::istringstream bad(corrupt, std::ios::binary);
    assert(!restored.read(bad));
    auto huge = bytes;
    huge[8] = huge[9] = huge[10] = huge[11] = char(255);
    std::istringstream oversized(huge, std::ios::binary);
    assert(!restored.read(oversized));
    p.output_format = 9;
    assert(!pairs.learn(p));
    for (size_t i = 0; i < ProtectedPipelinePairs::limit + 10; ++i) {
        p.output_format = 0;
        p.vertex[0] = uint8_t(i);
        p.vertex[1] = uint8_t(i >> 8);
        pairs.learn(p);
    }
    assert(pairs.entries().size() == ProtectedPipelinePairs::limit);
    std::ostringstream bounded(std::ios::binary);
    assert(pairs.write(bounded));
    assert(bounded.str().size() == 20 + 68 * ProtectedPipelinePairs::limit);
}
