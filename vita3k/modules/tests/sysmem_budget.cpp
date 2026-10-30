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

#include <modules/sysmem_budget.h>

#include <gtest/gtest.h>

#include <limits>

TEST(sysmem_budget, expansion_attributes) {
    constexpr uint32_t mib = 1024 * 1024;
    EXPECT_EQ(sysmem::user_memory_budget(0), 256 * mib);
    EXPECT_EQ(sysmem::user_memory_budget(0x4), 285 * mib);
    EXPECT_EQ(sysmem::user_memory_budget(0x8), 333 * mib);
    EXPECT_EQ(sysmem::user_memory_budget(0xC), 365 * mib);
    EXPECT_EQ(sysmem::user_memory_budget(0xFFFF'FFF3), 256 * mib);
    EXPECT_EQ(sysmem::user_memory_budget(0xFFFF'FFFF), 365 * mib);
}

TEST(sysmem_budget, exact_boundary_and_freed_capacity) {
    const uint32_t budget = sysmem::user_memory_budget(0);
    for (auto type : { SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE }) {
        EXPECT_FALSE(sysmem::exceeds_user_memory_budget(type, 0, budget, budget));
        EXPECT_TRUE(sysmem::exceeds_user_memory_budget(type, 0, budget + 1, budget));
        EXPECT_TRUE(sysmem::exceeds_user_memory_budget(type, budget, 1, budget));
        EXPECT_FALSE(sysmem::exceeds_user_memory_budget(type, budget - 4096, 4096, budget));
        EXPECT_TRUE(sysmem::exceeds_user_memory_budget(type, budget - 4096, 4097, budget));
    }
}

TEST(sysmem_budget, other_partitions_do_not_consume_user_budget) {
    const uint32_t budget = sysmem::user_memory_budget(0);
    for (auto type : { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW }) {
        EXPECT_FALSE(sysmem::exceeds_user_memory_budget(type, budget, 4096, budget));
    }
}

TEST(sysmem_budget, overflow_and_devkit_probe) {
    constexpr uint32_t mib = 1024 * 1024;
    const uint32_t budget = sysmem::user_memory_budget(0xC);
    EXPECT_TRUE(sysmem::exceeds_user_memory_budget(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 1, (std::numeric_limits<uint32_t>::max)(), budget));
    EXPECT_TRUE(sysmem::exceeds_user_memory_budget(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 0, 512 * mib, budget));
}
