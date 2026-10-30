// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <renderer/pvrt-dec.h>
#include <renderer/pvrtc-cache.h>
#include <vector>
static void env(const char *key, const char *value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}
int main() {
    auto root = std::filesystem::temp_directory_path() / ("vita-lifecycle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    auto utf8 = root.u8string();
    std::string path(utf8.begin(), utf8.end());
    env("VITA3K_DIAG_PVRTC_CACHE_MB", "16");
    env("VITA3K_DIAG_PVRTC_PROFILE_MODE", "learn");
    pvr::PVRTCConfigureDecodeCache(16);
    assert(pvr::PVRTCStartTextureProfile(path, "TEST01", "1.04|eboot").find("learn-ready") != std::string::npos);
    std::vector<uint8_t> input(128 * 128 / 2, 0x7b), pixels(128 * 128 * 4), expected(pixels.size());
    const auto result = pvr::PVRTDecompressPVRTCUncached(input.data(), 0, 128, 128, 0, expected.data());
    assert(pvr::PVRTDecompressPVRTC(input.data(), 0, 128, 128, 0, pixels.data()) == result && pixels == expected);
    const auto file = root / "decoded-textures/TEST01/learned.pvrp";
    assert(!std::filesystem::exists(file));
    std::string clear_message;
    assert(!pvr::PVRTCClearTextureProfiles(path, "TEST01", clear_message));
    assert(pvr::PVRTCFinishTextureProfile() == "saved, saved=1");
    assert(std::filesystem::exists(file));
    env("VITA3K_DIAG_PVRTC_PROFILE_MODE", "preload");
    const auto before = pvr::PVRTCDecodeCacheStats();
    assert(pvr::PVRTCStartTextureProfile(path, "TEST01", "1.04|eboot").find("prepared=1") != std::string::npos);
    const auto ready = pvr::PVRTCDecodeCacheStats();
    assert(ready.decodes == before.decodes && ready.requests == before.requests);
    assert(pvr::PVRTDecompressPVRTC(input.data(), 0, 128, 128, 0, pixels.data()) == result && pixels == expected);
    assert(pvr::PVRTCDecodeCacheStats().hits == ready.hits + 1);
    input[0] ^= 0xff;
    pvr::PVRTDecompressPVRTCUncached(input.data(), 0, 128, 128, 0, expected.data());
    pvr::PVRTDecompressPVRTC(input.data(), 0, 128, 128, 0, pixels.data());
    assert(pixels == expected && pvr::PVRTCDecodeCacheStats().misses == ready.misses + 1);
    pvr::PVRTCFinishTextureProfile();
    assert(pvr::PVRTCDecodeCacheStats().entries == 0);
    env("VITA3K_DIAG_PVRTC_PROFILE_MODE", "off");
    assert(pvr::PVRTCStartTextureProfile(path, "TEST01", "1.04|eboot").find("off: off") == 0);
    assert(pvr::PVRTCDecodeCacheStats().entries == 0);
    pvr::PVRTCFinishTextureProfile();
    env("VITA3K_DIAG_PVRTC_PROFILE_MODE", "preload");
    assert(pvr::PVRTCStartTextureProfile(path, "TEST02", "1.04|eboot").find("no-profile") != std::string::npos);
    assert(pvr::PVRTCDecodeCacheStats().entries == 0);
    pvr::PVRTCFinishTextureProfile();

    // Saved UI setting combines learning and preparation, diagnostic override wins.
    env("VITA3K_DIAG_PVRTC_PROFILE_MODE", "");
    assert(pvr::PVRTCStartTextureProfile(path, "AUTO01", "1.04|module\xc3\xa9", true).find("auto: no-profile") == 0);
    pvr::PVRTDecompressPVRTC(input.data(), 0, 128, 128, 0, pixels.data());
    assert(pvr::PVRTCFinishTextureProfile() == "saved, saved=1");
    auto prepared = pvr::PVRTCStartTextureProfile(path, "AUTO01", "1.04|module\xc3\xa9", true);
    assert(prepared.find("auto: preload-ready, prepared=1") == 0);
    const auto auto_ready = pvr::PVRTCDecodeCacheStats();
    pvr::PVRTDecompressPVRTC(input.data(), 0, 128, 128, 0, pixels.data());
    assert(pvr::PVRTCDecodeCacheStats().hits == auto_ready.hits + 1);
    input[0] ^= 0x71;
    pvr::PVRTDecompressPVRTCUncached(input.data(), 0, 128, 128, 0, expected.data());
    pvr::PVRTDecompressPVRTC(input.data(), 0, 128, 128, 0, pixels.data());
    assert(pixels == expected);
    assert(pvr::PVRTCFinishTextureProfile() == "saved, saved=2");
    assert(pvr::PVRTCStartTextureProfile(path, "AUTO01", "1.04|module\xc3\xa9", true).find("prepared=2") != std::string::npos);
    pvr::PVRTCFinishTextureProfile();
    env("VITA3K_DIAG_PVRTC_PROFILE_MODE", "off");
    assert(pvr::PVRTCStartTextureProfile(path, "AUTO01", "1.04|module\xc3\xa9", true).find("off: off") == 0);
    assert(!pvr::PVRTCDecodeCacheStats().entries);
    pvr::PVRTCFinishTextureProfile();
    env("VITA3K_DIAG_PVRTC_PROFILE_MODE", "");
    assert(pvr::PVRTCStartTextureProfile(path, "AUTO01", "1.04|module\xc3\xa9", false).find("off: off") == 0);
    pvr::PVRTCFinishTextureProfile();
    env("VITA3K_DIAG_PVRTC_CACHE_MB", "0");
    pvr::PVRTCConfigureDecodeCache(0);
    assert(pvr::PVRTCStartTextureProfile(path, "AUTO01", "1.04|module\xc3\xa9", true).find("cache-disabled") != std::string::npos);
    pvr::PVRTCFinishTextureProfile();
    assert(pvr::PVRTCClearTextureProfiles(path, "AUTO01", clear_message));
    assert(!std::filesystem::exists(root / "decoded-textures/AUTO01/learned.pvrp"));
    assert(std::filesystem::exists(file));
    std::filesystem::remove_all(root);
    std::cout << "Real decoder learning/preload/off lifecycle and demand counters passed\n";
}
