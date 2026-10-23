// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <renderer/pvrt-dec.h>
#include <renderer/pvrtc-cache.h>
#include <string_view>
#include <vector>
int main(int argc, char **argv) {
    const bool configured = argc > 1 && std::string_view(argv[1]) == "configured";
    const bool disabled = argc > 1 && !configured;
#ifdef _WIN32
    _putenv_s("VITA3K_DIAG_PVRTC_CACHE_MB", disabled ? "0" : "1");
#else
    setenv("VITA3K_DIAG_PVRTC_CACHE_MB", disabled ? "0" : "1", 1);
#endif
    if (configured) {
#ifdef _WIN32
        _putenv_s("VITA3K_DIAG_PVRTC_CACHE_MB", "");
#else
        unsetenv("VITA3K_DIAG_PVRTC_CACHE_MB");
#endif
        pvr::PVRTCConfigureDecodeCache(-1);
        assert(pvr::PVRTCDecodeCacheStats().budget_bytes == 0);
        pvr::PVRTCConfigureDecodeCache(513);
        assert(pvr::PVRTCDecodeCacheStats().budget_bytes == size_t(512) * 1024 * 1024);
        pvr::PVRTCConfigureDecodeCache(1);
    } else {
        // Explicit diagnostic override wins even when saved config requests off.
        pvr::PVRTCConfigureDecodeCache(0);
    }
    unsigned seed = 17, cases = 0;
    for (unsigned bits : { 0u, 1u })
        for (unsigned type : { 0u, 1u }) {
            for (unsigned w : { 1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 512u })
                for (unsigned h : { 1u, 8u, 32u, 512u }) {
                    auto bytes = std::max(w, bits ? 16u : 8u) * std::max(h, 8u) / (bits ? 4 : 2);
                    std::vector<uint8_t> source(bytes), expected(w * h * 4), actual(w * h * 4);
                    for (auto &v : source) {
                        seed = seed * 1664525 + 1013904223;
                        v = seed >> 24;
                    }
                    auto used = pvr::PVRTDecompressPVRTCUncached(source.data(), bits, w, h, type, expected.data());
                    for (int repeat = 0; repeat < 2; ++repeat) {
                        assert(pvr::PVRTDecompressPVRTC(source.data(), bits, w, h, type, actual.data()) == used);
                        assert(actual == expected);
                        ++cases;
                    }
                    auto relocated = source;
                    assert(pvr::PVRTDecompressPVRTC(relocated.data(), bits, w, h, type, actual.data()) == used && actual == expected);
                    ++cases;
                    source[0] ^= 0xff;
                    used = pvr::PVRTDecompressPVRTCUncached(source.data(), bits, w, h, type, expected.data());
                    assert(pvr::PVRTDecompressPVRTC(source.data(), bits, w, h, type, actual.data()) == used && actual == expected);
                    ++cases;
                }
        }
    auto stats = pvr::PVRTCDecodeCacheStats();
    assert(stats.resident_bytes <= stats.budget_bytes);
    if (disabled)
        assert(stats.hits == 0 && stats.entries == 0 && stats.decodes == cases);
    else
        assert(stats.hits > 0 && stats.misses > 0 && stats.evictions > 0 && stats.bypasses > 0);
    assert(stats.decodes == stats.requests - stats.hits);
    printf("%u byte-identical decode comparisons; requests=%llu hits=%llu misses=%llu evictions=%llu resident=%zu budget=%zu\n", cases, (unsigned long long)stats.requests, (unsigned long long)stats.hits, (unsigned long long)stats.misses, (unsigned long long)stats.evictions, stats.resident_bytes, stats.budget_bytes);
}
