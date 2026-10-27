// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string_view>

namespace renderer::vulkan {
// Diagnostic, process-local opt-out. This never forces support on an unsupported device.
inline bool pipeline_libraries_enabled(bool capable, const char *disable) {
    return capable && (!disable || std::string_view(disable) != "1");
}
} // namespace renderer::vulkan
