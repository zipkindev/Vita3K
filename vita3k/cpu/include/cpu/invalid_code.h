// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace cpu {

// This is the Vita's first 4 KiB page, independent of the host page size.
constexpr bool is_guest_first_page(uint32_t address) {
    return address < 4096;
}

} // namespace cpu
