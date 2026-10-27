// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#define VULKAN_HPP_NO_CONSTRUCTORS
#define VK_NO_PROTOTYPES
#include <renderer/vulkan/pipeline_library_keys.h>

#include <cstdlib>
#include <iostream>
using namespace renderer::vulkan;
static void check(bool good) {
    if (!good)
        std::abort();
}
int main() {
    vk::PipelineVertexInputStateCreateInfo vi{};
    vk::PipelineInputAssemblyStateCreateInfo ia{ .topology = vk::PrimitiveTopology::eTriangleList };
    vk::PipelineViewportStateCreateInfo viewport{ .viewportCount = 1, .scissorCount = 1 };
    vk::PipelineRasterizationStateCreateInfo raster{};
    vk::PipelineDepthStencilStateCreateInfo depth{};
    vk::PipelineMultisampleStateCreateInfo ms{ .rasterizationSamples = vk::SampleCountFlagBits::e1 };
    vk::PipelineColorBlendAttachmentState attachment{};
    vk::PipelineColorBlendStateCreateInfo blend{ .attachmentCount = 1, .pAttachments = &attachment };
    vk::PipelineDynamicStateCreateInfo dynamic{};
    const vk::SpecializationMapEntry map{ .constantID = 1, .size = 4 };
    uint32_t specialization = 0;
    vk::SpecializationInfo spec{ .mapEntryCount = 1, .pMapEntries = &map, .dataSize = 4, .pData = &specialization };
    vk::PipelineShaderStageCreateInfo stages[2] = {
        { .stage = vk::ShaderStageFlagBits::eVertex, .pName = "main_vs" },
        { .stage = vk::ShaderStageFlagBits::eFragment, .pName = "main_fs", .pSpecializationInfo = &spec },
    };
    vk::GraphicsPipelineCreateInfo p{ .stageCount = 2, .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &viewport, .pRasterizationState = &raster, .pMultisampleState = &ms, .pDepthStencilState = &depth, .pColorBlendState = &blend, .pDynamicState = &dynamic };
    const auto base = pipeline_library_keys(p, 7);
    // Blend/output changes must not recompile unchanged shader libraries.
    attachment.blendEnable = VK_TRUE;
    auto next = pipeline_library_keys(p, 7);
    check(base[0] == next[0] && base[1] == next[1] && base[2] == next[2] && base[3] != next[3]);
    attachment.blendEnable = VK_FALSE;
    depth.depthWriteEnable = VK_TRUE;
    next = pipeline_library_keys(p, 7);
    check(base[1] == next[1] && base[2] != next[2] && base[3] == next[3]);
    depth.depthWriteEnable = VK_FALSE;
    specialization = 1;
    next = pipeline_library_keys(p, 7);
    check(base[1] == next[1] && base[2] != next[2]);
    specialization = 0;
    ia.topology = vk::PrimitiveTopology::eLineList;
    next = pipeline_library_keys(p, 7);
    check(base[0] != next[0] && base[1] == next[1] && base[2] == next[2]);
    ia.topology = vk::PrimitiveTopology::eTriangleList;
    // Payload relocation must preserve identity; format/interlock/raw compatibility must not.
    auto copy = spec;
    uint32_t copied_data = 0;
    copy.pData = &copied_data;
    stages[1].pSpecializationInfo = &copy;
    check(base == pipeline_library_keys(p, 7));
    next = pipeline_library_keys(p, 8);
    check(base[0] == next[0] && base[1] != next[1] && base[2] != next[2] && base[3] != next[3]);
    p.stageCount = 1;
    check(base[2] != pipeline_library_keys(p, 7)[2]);
    std::cout << "pipeline library variant and reuse keys passed\n";
}
