// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <vkutil/vkutil.h>
#include <array>
#include <bit>
#include <cstdint>
#include <vector>

namespace renderer::vulkan {
// Structural keys, never addresses of create-info payloads or padding bytes. Module/layout
// handles are stable for this cache lifetime. Compatibility describes the subpass formats,
// attachment references and flags; load/store operations do not distinguish compatible passes.
inline std::array<std::vector<uint64_t>, 4> pipeline_library_keys(const vk::GraphicsPipelineCreateInfo &p, uint64_t compatibility) {
    std::array<std::vector<uint64_t>, 4> keys;
    for (uint64_t i = 0; i < keys.size(); ++i)
        keys[i].push_back(i);
    auto &vi = keys[0];
    vi.insert(vi.end(), { uint64_t(p.pInputAssemblyState->topology), p.pInputAssemblyState->primitiveRestartEnable });
    for (uint32_t i = 0; i < p.pVertexInputState->vertexBindingDescriptionCount; ++i) {
        const auto &b = p.pVertexInputState->pVertexBindingDescriptions[i];
        vi.insert(vi.end(), { b.binding, b.stride, uint64_t(b.inputRate) });
    }
    // Include counts so different binding/attribute partitions cannot alias.
    vi.push_back(p.pVertexInputState->vertexBindingDescriptionCount);
    vi.push_back(p.pVertexInputState->vertexAttributeDescriptionCount);
    for (uint32_t i = 0; i < p.pVertexInputState->vertexAttributeDescriptionCount; ++i) {
        const auto &a = p.pVertexInputState->pVertexAttributeDescriptions[i];
        vi.insert(vi.end(), { a.location, a.binding, uint64_t(a.format), a.offset });
    }
    for (size_t i = 1; i < keys.size(); ++i)
        keys[i].insert(keys[i].end(), { compatibility, p.subpass });
    keys[1].push_back(std::bit_cast<uint64_t>(static_cast<VkPipelineLayout>(p.layout)));
    keys[2].push_back(std::bit_cast<uint64_t>(static_cast<VkPipelineLayout>(p.layout)));
    for (uint32_t i = 0; i < p.stageCount; ++i) {
        const auto &stage = p.pStages[i];
        auto &key = keys[stage.stage == vk::ShaderStageFlagBits::eVertex ? 1 : 2];
        key.insert(key.end(), { uint64_t(stage.stage), std::bit_cast<uint64_t>(static_cast<VkShaderModule>(stage.module)), uint64_t(VkPipelineShaderStageCreateFlags(stage.flags)) });
        for (const char *name = stage.pName; *name; ++name)
            key.push_back(static_cast<unsigned char>(*name));
        key.push_back(0);
        if (stage.pSpecializationInfo) {
            const auto &spec = *stage.pSpecializationInfo;
            key.insert(key.end(), { spec.mapEntryCount, spec.dataSize });
            for (uint32_t j = 0; j < spec.mapEntryCount; ++j) {
                const auto &entry = spec.pMapEntries[j];
                key.insert(key.end(), { entry.constantID, entry.offset, entry.size });
            }
            for (size_t j = 0; j < spec.dataSize; ++j)
                key.push_back(static_cast<const uint8_t *>(spec.pData)[j]);
        } else {
            key.insert(key.end(), { 0, 0 });
        }
    }
    const auto &r = *p.pRasterizationState;
    keys[1].insert(keys[1].end(), { r.depthClampEnable, r.rasterizerDiscardEnable, uint64_t(r.polygonMode), uint64_t(VkCullModeFlags(r.cullMode)), uint64_t(r.frontFace), r.depthBiasEnable, p.pViewportState->viewportCount, p.pViewportState->scissorCount });
    const auto &d = *p.pDepthStencilState;
    keys[2].insert(keys[2].end(), { d.depthTestEnable, d.depthWriteEnable, uint64_t(d.depthCompareOp), d.depthBoundsTestEnable, d.stencilTestEnable });
    for (const auto &face : { d.front, d.back })
        keys[2].insert(keys[2].end(), { uint64_t(face.failOp), uint64_t(face.passOp), uint64_t(face.depthFailOp), uint64_t(face.compareOp) });
    // Multisampling belongs to both fragment subsets. Sample mask is null in this renderer.
    const auto &m = *p.pMultisampleState;
    for (size_t i : { 2u, 3u })
        keys[i].insert(keys[i].end(), { uint64_t(m.rasterizationSamples), m.sampleShadingEnable, std::bit_cast<uint32_t>(m.minSampleShading), m.alphaToCoverageEnable, m.alphaToOneEnable });
    const auto &b = *p.pColorBlendState;
    keys[3].insert(keys[3].end(), { uint64_t(VkPipelineColorBlendStateCreateFlags(b.flags)), b.logicOpEnable, uint64_t(b.logicOp), b.attachmentCount });
    for (uint32_t i = 0; i < b.attachmentCount; ++i) {
        const auto &a = b.pAttachments[i];
        keys[3].insert(keys[3].end(), { a.blendEnable, uint64_t(a.srcColorBlendFactor), uint64_t(a.dstColorBlendFactor), uint64_t(a.colorBlendOp), uint64_t(a.srcAlphaBlendFactor), uint64_t(a.dstAlphaBlendFactor), uint64_t(a.alphaBlendOp), uint64_t(VkColorComponentFlags(a.colorWriteMask)) });
    }
    for (float f : b.blendConstants)
        keys[3].push_back(std::bit_cast<uint32_t>(f));
    // Key all dynamic-state declarations too. They are fixed today, but a later change must
    // not reuse libraries declaring a different set. Static values above are only omitted
    // where this renderer always declares and supplies the corresponding dynamic state.
    for (auto &key : keys)
        for (uint32_t i = 0; i < p.pDynamicState->dynamicStateCount; ++i)
            key.push_back(uint64_t(p.pDynamicState->pDynamicStates[i]));
    return keys;
}
} // namespace renderer::vulkan
