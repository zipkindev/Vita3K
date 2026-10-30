// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string_view>

namespace renderer::vulkan {
// Diagnostic, process-local opt-out. This never forces support on an unsupported device.
inline bool pipeline_libraries_enabled(bool capable, const char *enable, const char *disable) {
    return capable && enable && std::string_view(enable) == "1"
        && (!disable || std::string_view(disable) != "1");
}
inline bool pipeline_library_extension_enabled(std::string_view name, bool enabled) {
    return enabled || (name != "VK_KHR_pipeline_library" && name != "VK_EXT_graphics_pipeline_library");
}
} // namespace renderer::vulkan
