// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <kernel/types.h>

#include <cstdint>

namespace sysmem {

constexpr uint32_t user_memory_budget(uint32_t attribute2) {
    constexpr uint32_t mib = 1024 * 1024;
    switch (attribute2 & 0x0C) {
    case 0x4: return (256 + 29) * mib;
    case 0x8: return (256 + 77) * mib;
    case 0xC: return (256 + 109) * mib;
    default: return 256 * mib;
    }
}

// Match this branch's allocation/free accounting. Newer mainline has additional
// partition types and masks which this Plus-derived branch does not implement.
constexpr bool takes_from_user(SceKernelMemBlockType type) {
    switch (type) {
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW:
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW:
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW:
        return false;
    default:
        return true;
    }
}

constexpr bool exceeds_user_memory_budget(SceKernelMemBlockType type, uint32_t allocated, uint32_t size, uint32_t budget) {
    return takes_from_user(type) && static_cast<uint64_t>(allocated) + size > budget;
}

} // namespace sysmem
