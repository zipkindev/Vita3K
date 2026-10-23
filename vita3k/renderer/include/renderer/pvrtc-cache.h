// Vita3K emulator project
// Copyright (C) 2026 Vita3K contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/decoded_texture_cache.h>

namespace pvr {
renderer::DecodedTextureCache::Stats PVRTCDecodeCacheStats();
// Apply the per-game setting; an explicit diagnostic environment override wins.
void PVRTCConfigureDecodeCache(int configured_mib);
// Unchanged reference path, also used for cache correctness comparisons.
uint32_t PVRTDecompressPVRTCUncached(const void *data, uint32_t two_bit, uint32_t width, uint32_t height, uint32_t type, uint8_t *output);
} // namespace pvr
