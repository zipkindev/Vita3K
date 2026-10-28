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

#include <renderer/vulkan/pipeline_cache.h>
#include <renderer/vulkan/pipeline_library_keys.h>

#include <renderer/functions.h>
#include <renderer/vulkan/functions.h>
#include <renderer/vulkan/gxm_to_vulkan.h>
#include <renderer/vulkan/state.h>
#include <renderer/vulkan/types.h>

#include <gxm/functions.h>
#include <gxm/types.h>
#include <renderer/shaders.h>
#include <shader/spirv_recompiler.h>

#include <util/fs.h>
#include <util/hash.h>
#include <util/log.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>

#include <SDL3/SDL_cpuinfo.h>
#include <util/pipeline_profile.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// don't use the dispatch version, because we always hash a small amount
// with a known size
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace renderer::vulkan {

static constexpr bool probe_before_defer = true;
static constexpr bool warm_at_program_creation = true;
static constexpr bool sync_new_surface_draws = false; // the surface was not drawn to in the last 60 frames (off: the main scene counts as new after a loading screen so a level's first frame froze for seconds on a cold cache)
static constexpr bool sync_depth_stencil_writers = false; // the draw writes depth or stencil (off: nearly every new pipeline of a 3D game does so async would almost never defer)
static constexpr bool save_in_background = true;

namespace {
thread_local bool pipeline_preparation_worker = false;

uint64_t pipeline_clock_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct PipelineDrawActivity {
    std::atomic<bool> &busy;
    std::atomic<uint64_t> &last_draw;
    PipelineDrawActivity(std::atomic<bool> &busy, std::atomic<uint64_t> &last_draw)
        : busy(busy)
        , last_draw(last_draw) { busy.store(true, std::memory_order_release); }
    ~PipelineDrawActivity() {
        last_draw.store(pipeline_clock_ns(), std::memory_order_relaxed);
        busy.store(false, std::memory_order_release);
    }
};

uint64_t now_s() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

uint64_t warm_pair_key(const Sha256Hash &vertex, const Sha256Hash &fragment, SceGxmOutputRegisterFormat output_format) {
    std::array<uint8_t, 68> data;
    memcpy(data.data(), vertex.data(), 32);
    memcpy(data.data() + 32, fragment.data(), 32);
    const uint32_t format = static_cast<uint32_t>(output_format);
    memcpy(data.data() + 64, &format, sizeof(format));
    return XXH3_64bits(data.data(), data.size());
}

// a skipped draw that writes depth or stencil changes what the later draws of the frame show
bool writes_depth_or_stencil(const GxmRecordState &record) {
    const bool two_sided = record.two_sided == SCE_GXM_TWO_SIDED_ENABLED;
    const auto stencil_writes = [](const GxmStencilStateOp &op) {
        return op.stencil_fail != SCE_GXM_STENCIL_OP_KEEP || op.depth_fail != SCE_GXM_STENCIL_OP_KEEP || op.depth_pass != SCE_GXM_STENCIL_OP_KEEP;
    };
    return record.front_depth_write_mode == SCE_GXM_DEPTH_WRITE_ENABLED || (two_sided && record.back_depth_write_mode == SCE_GXM_DEPTH_WRITE_ENABLED)
        || stencil_writes(record.front_stencil_state_op) || (two_sided && stencil_writes(record.back_stencil_state_op));
}
} // namespace

// Size of the record containing what is needed for the pipeline construction (what is after is dynamic state)
constexpr size_t record_pipeline_len = offsetof(GxmRecordState, vertex_streams);

static uint64_t hash_pipeline_record(const GxmRecordState &record) {
    alignas(8) uint8_t block[record_pipeline_len];
    memcpy(block, &record, record_pipeline_len);
    const auto zero = [&](const auto &field) {
        const size_t off = reinterpret_cast<const uint8_t *>(&field) - reinterpret_cast<const uint8_t *>(&record);
        memset(block + off, 0, sizeof(field));
    };
    zero(record.region_clip_mode);
    zero(record.back_polygon_mode);
    zero(record.back_depth_func);
    zero(record.back_depth_write_mode);
    zero(record.back_side_fragment_program_mode);
    if (record.two_sided != SCE_GXM_TWO_SIDED_ENABLED)
        zero(record.back_stencil_state_op);
    return XXH3_64bits(block, record_pipeline_len);
}

namespace {
GxmRecordState exact_record(const ExactPipelineRecipe &r) {
    GxmRecordState out{};
    out.vertex_program_hash = r.vertex;
    out.fragment_program_hash = r.fragment;
    const auto &w = r.words;
    out.color_base_format = static_cast<decltype(out.color_base_format)>(w[6]);
    out.cull_mode = static_cast<decltype(out.cull_mode)>(w[7]);
    out.two_sided = static_cast<decltype(out.two_sided)>(w[8]);
    out.region_clip_mode = static_cast<decltype(out.region_clip_mode)>(w[9]);
    out.front_polygon_mode = static_cast<decltype(out.front_polygon_mode)>(w[10]);
    out.back_polygon_mode = static_cast<decltype(out.back_polygon_mode)>(w[11]);
    out.front_stencil_state_op.func = static_cast<decltype(out.front_stencil_state_op.func)>(w[12]);
    out.front_stencil_state_op.stencil_fail = static_cast<decltype(out.front_stencil_state_op.stencil_fail)>(w[13]);
    out.front_stencil_state_op.depth_fail = static_cast<decltype(out.front_stencil_state_op.depth_fail)>(w[14]);
    out.front_stencil_state_op.depth_pass = static_cast<decltype(out.front_stencil_state_op.depth_pass)>(w[15]);
    out.back_stencil_state_op.func = static_cast<decltype(out.back_stencil_state_op.func)>(w[16]);
    out.back_stencil_state_op.stencil_fail = static_cast<decltype(out.back_stencil_state_op.stencil_fail)>(w[17]);
    out.back_stencil_state_op.depth_fail = static_cast<decltype(out.back_stencil_state_op.depth_fail)>(w[18]);
    out.back_stencil_state_op.depth_pass = static_cast<decltype(out.back_stencil_state_op.depth_pass)>(w[19]);
    out.front_depth_func = static_cast<decltype(out.front_depth_func)>(w[20]);
    out.back_depth_func = static_cast<decltype(out.back_depth_func)>(w[21]);
    out.front_depth_write_mode = static_cast<decltype(out.front_depth_write_mode)>(w[22]);
    out.back_depth_write_mode = static_cast<decltype(out.back_depth_write_mode)>(w[23]);
    out.front_side_fragment_program_mode = static_cast<decltype(out.front_side_fragment_program_mode)>(w[24]);
    out.back_side_fragment_program_mode = static_cast<decltype(out.back_side_fragment_program_mode)>(w[25]);
    out.is_maskupdate = static_cast<decltype(out.is_maskupdate)>(w[26]);
    out.is_gamma_corrected = static_cast<decltype(out.is_gamma_corrected)>(w[27]);
    return out;
}
bool valid_exact_recipe(const ExactPipelineRecipe &r) {
    const auto &w = r.words;
    // Reject malformed cached scalar state before it can reach translation switches.
    if (w[0] > 0x14000000 || (w[0] & 0x03FFFFFF) || w[2] > 1 || w[3] > 1 || w[5] > 63 || w[7] > 2 || (w[8] != 0 && w[8] != 0x800)
        || (w[9] & 0x3FFFFFFF) || w[10] > 0x30000 || (w[10] & 0x7FFF)
        || w[11] > 0x30000 || (w[11] & 0x7FFF) || w[26] != 0 || w[27] > 1
        || w[28] > 8 || w[29] > 1 || w[62] || w[63])
        return false;
    for (auto i : { 12, 16 })
        if (w[i] > 0x0E000000 || (w[i] & 0x01FFFFFF))
            return false;
    for (auto i : { 13, 14, 15, 17, 18, 19 })
        if (w[i] > 7)
            return false;
    for (auto i : { 20, 21 })
        if (w[i] > 0x01C00000 || (w[i] & 0x003FFFFF))
            return false;
    for (auto i : { 22, 23 })
        if (w[i] != 0 && w[i] != 0x00100000)
            return false;
    for (auto i : { 24, 25 })
        if (w[i] != 0 && w[i] != 0x00200000)
            return false;
    const auto record = exact_record(r);
    if (record.color_base_format != gxm::get_base_format(static_cast<SceGxmColorFormat>(w[1])))
        return false;
    uint64_t key = hash_pipeline_record(record) ^ r.layout ^ r.blend ^ w[0];
    if (w[28] != SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED)
        key ^= uint64_t(w[28]) << 56;
    const int channel = gxm::one_channel_source_component(static_cast<SceGxmColorFormat>(w[1]));
    if (channel > 0)
        key ^= uint64_t(channel) << 48;
    if (w[3])
        key ^= 0xC2B2AE3D27D4EB4FULL;
    // Output-register bits are only in the key for native-colour shaders, checked with live bindings instead.
    return key == r.pipeline_key || (w[28] != SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED && (key ^ (uint64_t(w[28]) << 56)) == r.pipeline_key);
}
} // namespace

// structure containing everything needed to compile a pipeline
struct CompileRequest {
    // iterator to the pipeline location
    vk::Pipeline *pipeline;

    // this is everything we need to compile the shader on another thread (as the original data will change)
    SceGxmPrimitiveType type;
    vk::RenderPass render_pass;
    std::shared_ptr<ProgramBinding> vertex_program_binding;
    std::shared_ptr<ProgramBinding> fragment_program_binding;
    bool has_color_surface_data;
    shader::Hints hints;
    bool has_casts;
    uint64_t telemetry_key = 0, telemetry_pair = 0, telemetry_queued = 0;
    std::optional<ExactPipelineRecipe> exact_recipe;

    // the content of the record useful for the pipeline creation
    alignas(8) uint8_t record_data[record_pipeline_len];

    const GxmRecordState *get_record() {
        // note: this object is only half defined, but we are only looking at the part that's defined
        return reinterpret_cast<const GxmRecordState *>(record_data);
    }
};

PipelineCache::PipelineCache(VKState &state)
    : state(state)
    , pipeline_compile_queue_token(pipeline_compile_queue) {
}

void PipelineCache::init(bool support_rasterized_order_access) {
    vk::PipelineCacheCreateInfo pipeline_info{};
    pipeline_cache = state.device.createPipelineCache(pipeline_info);

    // the layout for uniforms buffer can be made here as it will always be the same
    {
        std::array<vk::DescriptorSetLayoutBinding, 4> layout_bindings;
        // Our vertex uniform (GXMRenderVertUniformBlock)
        layout_bindings[0] = vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = vk::DescriptorType::eUniformBufferDynamic,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eVertex,
        };
        // Our fragment uniform (GXMRenderFragUniformBlock)
        layout_bindings[1] = vk::DescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = vk::DescriptorType::eUniformBufferDynamic,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        };
        // GXM vertex uniform (if no memory mapping)
        layout_bindings[2] = vk::DescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = vk::DescriptorType::eStorageBufferDynamic,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eVertex,
        };
        // GXM Fragment uniform (if no memory mapping)
        layout_bindings[3] = vk::DescriptorSetLayoutBinding{
            .binding = 3,
            .descriptorType = vk::DescriptorType::eStorageBufferDynamic,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        };

        vk::DescriptorSetLayoutCreateInfo descriptor_info{
            .bindingCount = state.features.enable_memory_mapping ? 2U : 4U,
            .pBindings = layout_bindings.data()
        };
        uniforms_layout = state.device.createDescriptorSetLayout(descriptor_info);
    }

    {
        // layout for the mask, color attachment as input, being an input attachment or a storage image
        // depending on whether or not we are using shader interlock
        std::array<vk::DescriptorSetLayoutBinding, 3> layout_binding;
        const vk::DescriptorType intput_image_descriptor = state.features.support_shader_interlock
            ? vk::DescriptorType::eStorageImage
            : vk::DescriptorType::eInputAttachment;
        uint32_t binding_count = 0;
        layout_binding[binding_count++] = vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = intput_image_descriptor,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment
        };
        if (state.features.use_mask_bit)
            layout_binding[binding_count++] = vk::DescriptorSetLayoutBinding{
                .binding = 1,
                .descriptorType = vk::DescriptorType::eStorageImage,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eFragment
            };
        if (state.features.preserve_f16_nan_as_u16)
            // raw u16x4 alias of the color attachment (set 1, binding 2 in the shaders)
            layout_binding[binding_count++] = vk::DescriptorSetLayoutBinding{
                .binding = 2,
                .descriptorType = intput_image_descriptor,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eFragment
            };

        vk::DescriptorSetLayoutCreateInfo descriptor_info{
            .bindingCount = binding_count,
            .pBindings = layout_binding.data()
        };
        attachments_layout = state.device.createDescriptorSetLayout(descriptor_info);
    }

    {
        // texture layout

        // empty descriptor
        {
            vk::DescriptorSetLayoutCreateInfo empty_info{};
            vertex_textures_layout[0] = state.device.createDescriptorSetLayout(empty_info);
            fragment_textures_layout[0] = vertex_textures_layout[0];
        }

        // first vertex
        std::array<vk::DescriptorSetLayoutBinding, 16> layout_bindings;
        for (uint32_t i = 0; i < 16; i++) {
            layout_bindings[i] = {
                .binding = i,
                .descriptorType = vk::DescriptorType::eCombinedImageSampler,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eVertex
            };
        }
        for (uint32_t i = 1; i <= 16; i++) {
            vk::DescriptorSetLayoutCreateInfo descriptor_info{
                .bindingCount = i,
                .pBindings = layout_bindings.data()
            };
            vertex_textures_layout[i] = state.device.createDescriptorSetLayout(descriptor_info);
        }

        // then fragment
        for (uint32_t i = 0; i < 16; i++) {
            layout_bindings[i].stageFlags = vk::ShaderStageFlagBits::eFragment;
        }
        for (uint32_t i = 1; i <= 16; i++) {
            vk::DescriptorSetLayoutCreateInfo descriptor_info{
                .bindingCount = i,
                .pBindings = layout_bindings.data()
            };
            fragment_textures_layout[i] = state.device.createDescriptorSetLayout(descriptor_info);
        }
    }

    // compute all possible pipeline layouts
    for (uint32_t vert_texture_count = 0; vert_texture_count <= 16; vert_texture_count++) {
        for (uint32_t frag_texture_count = 0; frag_texture_count <= 16; frag_texture_count++) {
            vk::PipelineLayoutCreateInfo layout_info{};
            vk::DescriptorSetLayout set_layouts[] = { uniforms_layout, attachments_layout, vertex_textures_layout[vert_texture_count], fragment_textures_layout[frag_texture_count] };
            layout_info.setSetLayouts(set_layouts);
            pipeline_layouts[vert_texture_count][frag_texture_count] = state.device.createPipelineLayout(layout_info);
        }
    }

    {
        // look for rgb vertex attribute support
        // we need to look at each format because it is not the same for all usual 3-component formats (checked on AMD Radeon HD 7800)
        // no need to test for 32-bit types, they are always supported
        vk::Format formats[] = {
            vk::Format::eR16G16B16Unorm, vk::Format::eR16G16B16Snorm,
            vk::Format::eR16G16B16Uscaled, vk::Format::eR16G16B16Sscaled,
            vk::Format::eR16G16B16Uint, vk::Format::eR16G16B16Sint,
            vk::Format::eR16G16B16Sfloat,
            vk::Format::eR8G8B8Unorm, vk::Format::eR8G8B8Snorm,
            vk::Format::eR8G8B8Uscaled, vk::Format::eR8G8B8Sscaled,
            vk::Format::eR8G8B8Uint, vk::Format::eR8G8B8Sint
        };
        for (auto fmt : formats) {
            vk::FormatProperties rgb_property = state.physical_device.getFormatProperties(fmt);
            if (!(rgb_property.bufferFeatures & vk::FormatFeatureFlagBits::eVertexBuffer)) {
                unsupported_rgb_vertex_attribute_formats.emplace(fmt);
            }
        }

        // same for scaled formats
        const vk::FormatProperties scaled_property = state.physical_device.getFormatProperties(vk::Format::eR8G8B8A8Uscaled);
        support_scaled_vertex_attribute = static_cast<bool>(scaled_property.bufferFeatures & vk::FormatFeatureFlagBits::eVertexBuffer);
        state.features.support_scaled_attribute_formats = support_scaled_vertex_attribute;

        // handle interactions between these 2 properties, the scaled support will be applied before the 3-component support
        if (!support_scaled_vertex_attribute) {
            vk::Format scaled_fmt[] = { vk::Format::eR16G16B16Uscaled, vk::Format::eR16G16B16Sscaled, vk::Format::eR8G8B8Uscaled, vk::Format::eR8G8B8Sscaled };
            for (auto fmt : scaled_fmt)
                unsupported_rgb_vertex_attribute_formats.erase(fmt);
        }

        state.features.support_rgb_attributes = unsupported_rgb_vertex_attribute_formats.empty();
    }

    support_coherent_framebuffer_fetch = support_rasterized_order_access;

    const int nb_logical_threads = SDL_GetNumLogicalCPUCores();
    // took this from RPCS3 (slightly modified)
    if (nb_logical_threads > 12)
        nb_worker_threads = 6;
    else if (nb_logical_threads > 8)
        nb_worker_threads = 4;
    else if (nb_logical_threads >= 8)
        nb_worker_threads = 3;
    else if (nb_logical_threads >= 6)
        nb_worker_threads = 2;
    else
        nb_worker_threads = 1;

    if (use_async_compilation) {
        // we could not initialize the worker threads previously
        use_async_compilation = false;
        set_async_compilation(true);
    }

    support_cache_only_creation = state.support_pipeline_cache_control;
    if (warm_at_program_creation) {
        warm_stopping = false;
        warm_render_pass = retrieve_render_pass(vk::Format::eR8G8B8A8Unorm, true, true, true, false);
        if (state.features.support_shader_interlock)
            warm_interlock_render_pass = retrieve_render_pass(vk::Format::eR8G8B8A8Unorm, true, true, true, false, true);
        const int warm_thread_count = std::clamp(nb_worker_threads, 1, 2);
        warm_threads.reserve(warm_thread_count);
        for (int i = 0; i < warm_thread_count; i++)
            warm_threads.emplace_back(&PipelineCache::warm_thread, this, std::ref(*state.mem));
        warm_enabled = true;
    }
}

void PipelineCache::set_async_compilation(bool enable) {
    if (enable == use_async_compilation)
        return;

    use_async_compilation = enable;
    if (nb_worker_threads == 0)
        // not ingame yet
        return;

    if (enable) {
        LOG_INFO("Enabling asynchronous pipeline compilation with {} threads", nb_worker_threads);
        worker_threads.reserve(nb_worker_threads);
        for (int i = 0; i < nb_worker_threads; i++) {
            worker_threads.emplace_back(&PipelineCache::compiler_thread, this, std::ref(*state.mem));
        }
    } else {
        LOG_INFO("Asynchronous pipeline compilation is now disabled");
        stop_compile_workers(true);
    }
}

void PipelineCache::stop_compile_workers(bool drain) {
    if (drain) {
        drain_compile_workers = true;
    } else {
        for (size_t i = 0; i < worker_threads.size(); i++)
            // if a thread receives nullptr, it exits
            pipeline_compile_queue.enqueue(nullptr);
    }

    for (auto &thread : worker_threads) {
        if (thread.joinable())
            thread.join();
    }
    worker_threads.clear();
    drain_compile_workers = false;
}

// magic number put at the beginning of the pipeline cache file
constexpr uint32_t pipeline_cache_magic = 0xBEEF4321;

void PipelineCache::read_pipeline_cache() {
    // Read independently of the opaque driver cache: stage identities contain no Vulkan handles.
    {
        fs::ifstream learned(state.shaders_path / fmt::format("protected-pairs-vk{}.bin", shader::CURRENT_VERSION), std::ios::binary);
        ProtectedPipelinePairs loaded;
        if (learned && loaded.read(learned)) {
            std::lock_guard<std::mutex> guard(warm_mutex);
            protected_pairs = loaded;
            loaded_protected_pairs = loaded.entries();
            LOG_INFO("[PIPEWARM] loaded {} protected program pairs", loaded_protected_pairs.size());
        }
    }
    {
        fs::ifstream file(state.shaders_path / fmt::format("exact-pipelines-vk{}-f{}.bin", shader::CURRENT_VERSION, state.get_features_mask()), std::ios::binary);
        ExactPipelineRecipes loaded;
        if (file && loaded.read(file)) {
            for (auto recipe : loaded.entries()) {
                if (!valid_exact_recipe(recipe))
                    continue;
                canonicalize_exact_pipeline(recipe, 0xFFFF, 0xFFFF, 0);
                const auto &w = recipe.words;
                try {
                    const auto format = static_cast<vk::Format>(w[4]);
                    const auto props = state.physical_device.getFormatProperties(format);
                    if (!(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eColorAttachment))
                        continue;
                    const auto pass = retrieve_render_pass(format, w[5] & 1, w[5] & 2, w[5] & 4, w[5] & 8, w[5] & 16, w[5] & 32);
                    loaded_exact_recipes.emplace_back(recipe, pass);
                    exact_recipes.learn(recipe);
                } catch (const std::exception &error) {
                    LOG_WARN("[PIPEEXACT] ignored invalid render-pass recipe: {}", error.what());
                }
            }
            LOG_INFO("[PIPEEXACT] loaded {} exact pipeline recipes", loaded_exact_recipes.size());
        }
    }
    const std::string pipeline_cache_name = fmt::format("pipeline-cache-vk{}.dat", shader::CURRENT_VERSION);
    const fs::path path = state.shaders_path / pipeline_cache_name;

    fs::ifstream pipeline_cache_file(path, std::ios::in | std::ios::binary);
    if (!pipeline_cache_file.is_open())
        return;

    LOG_INFO("Found pipeline cache, reading...");

    pipeline_cache_file.seekg(0, fs::ifstream::end);
    const auto file_size = pipeline_cache_file.tellg();
    if (file_size < 0)
        return;
    size_t pipeline_size = static_cast<size_t>(file_size);
    pipeline_cache_file.seekg(0);

    if (pipeline_size < sizeof(uint32_t) + sizeof(size_t))
        return;

    // read the hashes
    auto read_integer = [&]<typename T>(T &val) {
        pipeline_cache_file.read(reinterpret_cast<char *>(&val), sizeof(T));
    };
    uint32_t magic_number = 0;
    read_integer(magic_number);
    size_t nb_hashes = 0;
    read_integer(nb_hashes);
    // safety check
    const size_t header_size = sizeof(magic_number) + sizeof(nb_hashes);
    if (!pipeline_cache_file || magic_number != pipeline_cache_magic || nb_hashes > (pipeline_size - header_size) / sizeof(uint64_t)) {
        LOG_WARN("Pipeline cache is corrupted, ignoring it.");
        pipeline_cache_file.close();
        return;
    }
    pipeline_size -= header_size + nb_hashes * sizeof(uint64_t);

    std::vector<uint64_t> hashes(nb_hashes);
    for (uint64_t &hash : hashes)
        read_integer(hash);

    std::vector<char> pipeline_data(pipeline_size);
    if (!pipeline_cache_file.read(pipeline_data.data(), pipeline_size))
        return;
    pipeline_cache_file.close();

    VkPipelineCacheHeaderVersionOne header{};
    if (pipeline_size >= sizeof(header))
        std::memcpy(&header, pipeline_data.data(), sizeof(header));
    const vk::PhysicalDeviceProperties &props = state.physical_device_properties;
    if (pipeline_size < sizeof(header) || header.headerSize < sizeof(header) || header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE
        || header.vendorID != props.vendorID || header.deviceID != props.deviceID
        || std::memcmp(header.pipelineCacheUUID, props.pipelineCacheUUID.data(), VK_UUID_SIZE) != 0) {
        LOG_WARN("Pipeline cache was written for another GPU or driver, ignoring it");
        return;
    }

    // insert hashes with null pipeline
    for (const uint64_t hash : hashes)
        pipelines[hash] = nullptr;

    vk::PipelineCacheCreateInfo cache_info{
        .initialDataSize = pipeline_size,
        .pInitialData = pipeline_data.data()
    };

    state.device.destroyPipelineCache(pipeline_cache);
    pipeline_cache = state.device.createPipelineCache(cache_info);
    LOG_INFO("Pipeline cache read and loaded");
}

void PipelineCache::save_pipeline_cache() {
    wait_for_background_save();
    std::vector<uint64_t> keys;
    keys.reserve(pipelines.size());
    for (const auto &[hash, _] : pipelines)
        keys.push_back(hash);
    write_pipeline_cache(keys);
}

void PipelineCache::save_pipeline_cache_async() {
    if (!save_in_background) {
        save_pipeline_cache();
        return;
    }
    if (save_running) {
        // the previous save is still writing, try again shortly
        next_pipeline_cache_save = now_s() + 2;
        return;
    }
    wait_for_background_save();
    std::vector<uint64_t> keys;
    keys.reserve(pipelines.size());
    for (const auto &[hash, _] : pipelines)
        keys.push_back(hash);
    save_running = true;
    save_thread = std::thread([this, keys = std::move(keys)]() {
        try {
            write_pipeline_cache(keys);
        } catch (const std::exception &error) {
            LOG_ERROR("Could not save the pipeline cache: {}", error.what());
        }
        save_running = false;
    });
}

void PipelineCache::wait_for_background_save() {
    if (save_thread.joinable())
        save_thread.join();
}

void PipelineCache::write_pipeline_cache(const std::vector<uint64_t> &keys) {
    // first save the shader hashes
    // do a copy for thread safety
    std::vector<ShadersHash> shader_cache_copy;
    {
        std::lock_guard<std::mutex> guard(shaders_mutex);
        shader_cache_copy = state.shaders_cache_hashs;
    }
    renderer::save_shaders_cache_hashs(state, shader_cache_copy);

    const std::vector<uint8_t> pipeline_data = state.device.getPipelineCacheData(pipeline_cache);
    if (pipeline_data.empty())
        // No pipeline was created
        return;

    const std::string pipeline_cache_name = fmt::format("pipeline-cache-vk{}.dat", shader::CURRENT_VERSION);
    const fs::path path = state.shaders_path / pipeline_cache_name;

    fs::ofstream pipeline_cache_file(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!pipeline_cache_file.is_open())
        return;

    LOG_INFO("Saving pipeline cache...");

    // The learned set is independently bounded and checksummed. A partial write is ignored on load.
    {
        ProtectedPipelinePairs copy;
        {
            std::lock_guard<std::mutex> guard(warm_mutex);
            copy = protected_pairs;
        }
        fs::ofstream learned(state.shaders_path / fmt::format("protected-pairs-vk{}.bin", shader::CURRENT_VERSION), std::ios::binary | std::ios::trunc);
        if (learned && !copy.write(learned))
            LOG_WARN("[PIPEWARM] could not save protected program pairs");
    }

    {
        ExactPipelineRecipes copy;
        {
            std::lock_guard<std::mutex> guard(warm_mutex);
            copy = exact_recipes;
        }
        fs::ofstream file(state.shaders_path / fmt::format("exact-pipelines-vk{}-f{}.bin", shader::CURRENT_VERSION, state.get_features_mask()), std::ios::binary | std::ios::trunc);
        if (file && !copy.write(file))
            LOG_WARN("[PIPEEXACT] could not save exact recipes");
    }
    // first save the hashes of all pipelines
    auto write_integer = [&]<typename T>(T val) {
        pipeline_cache_file.write(reinterpret_cast<const char *>(&val), sizeof(T));
    };
    write_integer(pipeline_cache_magic);
    write_integer(keys.size());
    for (const uint64_t hash : keys) {
        write_integer(hash);
    }

    // then save the cache
    pipeline_cache_file.write(reinterpret_cast<const char *>(pipeline_data.data()), pipeline_data.size());
    pipeline_cache_file.close();
    LOG_INFO("Pipeline cache saved");
}

void PipelineCache::cleanup() {
    stop_warm_threads();
    wait_for_background_save();
    warm_render_pass = nullptr;
    warm_interlock_render_pass = nullptr;
    {
        std::lock_guard<std::mutex> guard(warm_mutex);
        warm_pairs_requested.clear();
        early_pairs.clear();
        warm_attempts.clear();
        warm_vertices.clear();
        warm_fragments.clear();
        warm_registered = warm_pending = warm_running = exact_live_jobs = 0;
        last_pipeline_draw_ns = 0;
        pipeline_draw_busy = false;
        pipeline_draw_poll = 0;
        warm_registry_generation = exact_scan_generation = 0;
        exact_deferred = false;
        for (const auto &[key, row] : exact_coverage)
            LOG_INFO("[PIPECOVER] exact key={} pair={} events={} first_qpc={} last_qpc={}", key, row.pair, row.events, row.first_qpc, row.last_qpc);
        for (const auto &[variant, row] : pair_coverage)
            LOG_INFO("[PIPECOVER] pair variant={} pair={} queued_qpc={} completed_qpc={} outcome={}", variant, row.pair, row.queued_qpc, row.completed_qpc, row.outcome);
        exact_coverage.clear();
        pair_coverage.clear();
        LOG_INFO("[PIPEEXACT] session recipes={} queued={} prepared={} reused={} draw_claimed={}",
            exact_recipes.entries().size(), exact_jobs.size(), exact_prepared_count, exact_reused_count, exact_draw_claimed_count);
        LOG_INFO("[PIPESCHED] misses={} waits={} refills={} active_limit=1 quiet_limit=2", exact_miss_count, exact_wait_count, exact_refill_count);
        exact_prepared_count = exact_reused_count = exact_draw_claimed_count = 0;
        exact_miss_count = exact_wait_count = exact_refill_count = 0;
        for (auto &[recipe, job] : exact_jobs)
            if (job->pipeline)
                state.device.destroyPipeline(job->pipeline);
        exact_jobs.clear();
        loaded_exact_recipes.clear();
        exact_recipes = {};
        protected_pairs = {};
        loaded_protected_pairs.clear();
        pairs_drawn.clear();
        warm_states.clear();
    }
    render_pass_recipes.clear();
    draw_states.clear();
    draw_states_total = 0;

    // stop threads (queued requests are abandoned at shutdown)
    if (use_async_compilation) {
        use_async_compilation = false;
        stop_compile_workers(false);
    }

    for (auto &[hash, pipeline] : pipelines)
        state.device.destroy(pipeline);
    pipelines.clear();

    {
        std::lock_guard<std::mutex> lock(libraries_mutex);
        LOG_INFO("[PIPEGPL] libraries={} hits={} compiled={} waits={} links={} fallback={}", libraries.size(), library_hits.load(), library_compiles.load(), library_waits.load(), library_links.load(), library_fallbacks.load());
        for (const auto &[key, entry] : libraries)
            if (entry->pipeline)
                state.device.destroyPipeline(entry->pipeline);
        libraries.clear();
        library_hits = library_compiles = library_waits = library_links = library_fallbacks = 0;
    }

    {
        std::lock_guard<std::mutex> guard(shaders_mutex);
        for (auto &[hash, shader] : shaders)
            state.device.destroy(shader);
        shaders.clear();
    }

    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            for (int k = 0; k < 2; k++)
                for (int l = 0; l < 2; l++) {
                    for (auto &[fmt, pass] : render_passes[i][j][k][l])
                        state.device.destroy(pass);
                    render_passes[i][j][k][l].clear();
                }

    for (auto &[fmt, pass] : shader_interlock_pass)
        state.device.destroy(pass);
    shader_interlock_pass.clear();

    for (int i = 0; i < 17; i++)
        for (int j = 0; j < 17; j++) {
            state.device.destroy(pipeline_layouts[i][j]);
            pipeline_layouts[i][j] = nullptr;
        }

    state.device.destroy(uniforms_layout);
    uniforms_layout = nullptr;
    state.device.destroy(attachments_layout);
    attachments_layout = nullptr;

    state.device.destroy(vertex_textures_layout[0]);
    vertex_textures_layout[0] = nullptr;
    fragment_textures_layout[0] = nullptr;

    for (int i = 1; i <= 16; i++) {
        state.device.destroy(vertex_textures_layout[i]);
        vertex_textures_layout[i] = nullptr;
        state.device.destroy(fragment_textures_layout[i]);
        fragment_textures_layout[i] = nullptr;
    }

    state.device.destroy(pipeline_cache);
    pipeline_cache = nullptr;

    next_pipeline_cache_save = std::numeric_limits<uint64_t>::max();
    nb_worker_threads = 0;
}

// Vulkan structs used to specify the fragment specialization constants
// Also, booleans in SPIRV are 32bit wide
static const vk::SpecializationMapEntry frag_spec_entries[] = {
    { .constantID = shader::GAMMA_CORRECTION_SPECIALIZATION_ID, .offset = 0, .size = sizeof(uint32_t) },
    { .constantID = shader::SURFACE_CAST_SPECIALIZATION_ID, .offset = sizeof(uint32_t), .size = sizeof(uint32_t) },
};

// indexed by (is_srgb << 1) | has_casts
static const uint32_t frag_spec_data[4][2] = {
    { vk::False, vk::False },
    { vk::False, vk::True },
    { vk::True, vk::False },
    { vk::True, vk::True },
};

static const vk::SpecializationInfo frag_spec_infos[4] = {
    { .mapEntryCount = 2, .pMapEntries = frag_spec_entries, .dataSize = sizeof(frag_spec_data[0]), .pData = frag_spec_data[0] },
    { .mapEntryCount = 2, .pMapEntries = frag_spec_entries, .dataSize = sizeof(frag_spec_data[1]), .pData = frag_spec_data[1] },
    { .mapEntryCount = 2, .pMapEntries = frag_spec_entries, .dataSize = sizeof(frag_spec_data[2]), .pData = frag_spec_data[2] },
    { .mapEntryCount = 2, .pMapEntries = frag_spec_entries, .dataSize = sizeof(frag_spec_data[3]), .pData = frag_spec_data[3] },
};

static Sha256Hash shader_cache_key(const SceGxmProgram *program, const Sha256Hash &base_hash, bool is_vertex, const shader::Hints &hints, std::string &version_suffix) {
    // A one-channel surface moves a different component into the stored channel, so it needs its own cache entry
    const int one_channel_source = is_vertex ? -1 : gxm::one_channel_source_component(hints.color_format);
    Sha256Hash hash = base_hash;
    if (one_channel_source > 0) {
        hash[0] ^= static_cast<uint8_t>(0xC0 + one_channel_source);
        version_suffix = fmt::format("c{}", one_channel_source);
    }
    if (!is_vertex && program->is_native_color() && hints.output_register_format != SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED) {
        hash[1] ^= static_cast<uint8_t>(0x80 + hints.output_register_format);
        version_suffix += fmt::format("r{}", static_cast<int>(hints.output_register_format));
    }
    const SceGxmColorBaseFormat surface_base = gxm::get_base_format(hints.color_format);
    if (!is_vertex && (program->is_frag_color_used() || surface_base == SCE_GXM_COLOR_BASE_FORMAT_F32F32)) {
        for (int i = 0; i < 4; i++)
            hash[2 + i] ^= static_cast<uint8_t>(static_cast<uint32_t>(surface_base) >> (8 * i));
        version_suffix += fmt::format("s{:X}", static_cast<uint32_t>(surface_base));
    }
    if (!is_vertex && hints.raw_color_attachment_input) {
        hash[6] ^= 0x75;
        version_suffix += "u";
    }
    return hash;
}

vk::PipelineShaderStageCreateInfo PipelineCache::retrieve_shader(const SceGxmProgram *program, const Sha256Hash &base_hash, bool is_vertex, bool maskupdate, MemState &mem, const shader::Hints &hints, bool is_srgb, bool has_casts, bool ready_only) {
    if (maskupdate)
        LOG_WARN_ONCE("Mask not implemented in the vulkan renderer!");

    std::string version_suffix;
    const Sha256Hash hash = shader_cache_key(program, base_hash, is_vertex, hints, version_suffix);

    const vk::ShaderModule shader_compiling = std::bit_cast<vk::ShaderModule>(~0ULL);

    // entries for constants a module does not declare are ignored, and is_srgb is only declared along shader interlock
    const bool srgb = is_srgb && state.features.should_use_shader_interlock() && program->is_frag_color_used();
    const vk::SpecializationInfo *spec_info = is_vertex ? nullptr : &frag_spec_infos[(srgb ? 2 : 0) | (has_casts ? 1 : 0)];

    if (ready_only) {
        std::lock_guard<std::mutex> guard(shaders_mutex);
        const auto found = shaders.find(hash);
        if (found == shaders.end() || found->second == nullptr || found->second == shader_compiling)
            return {};
        return vk::PipelineShaderStageCreateInfo{
            .stage = is_vertex ? vk::ShaderStageFlagBits::eVertex : vk::ShaderStageFlagBits::eFragment,
            .module = found->second,
            .pName = is_vertex ? "main_vs" : "main_fs",
            .pSpecializationInfo = spec_info,
        };
    }

    vk::ShaderModule *shader_module;
    vk::ShaderModule module;
    {
        std::unique_lock<std::mutex> lock(shaders_mutex);
        shader_module = &shaders.insert({ hash, nullptr }).first->second;
        if (*shader_module == shader_compiling) {
            util::PipelineScope waiting(util::PipelineKind::ShaderWait, { 0, 0, is_vertex ? 1u : 0u });
            shaders_ready.wait(lock, [&] { return *shader_module != shader_compiling; });
        }
        module = *shader_module;
        if (!module)
            *shader_module = shader_compiling;
    }

    if (!module) {
        // This thread owns the slot. A variant load publishes under shaders_mutex.
        module = load_cached_shader(hash, fmt::format("vk{}{}-{}.spv", shader::CURRENT_VERSION, version_suffix, hex_string(base_hash)));
    }
    if (module) {
        return vk::PipelineShaderStageCreateInfo{
            .stage = is_vertex ? vk::ShaderStageFlagBits::eVertex : vk::ShaderStageFlagBits::eFragment,
            .module = module,
            .pName = is_vertex ? "main_vs" : "main_fs",
            .pSpecializationInfo = spec_info,
        };
    }

    const std::string hash_text = hex_string(hash);

    LOG_DEBUG("Generating vulkan spv shader {}", hash_text);
    const std::string shader_version = fmt::format("vk{}{}", shader::CURRENT_VERSION, version_suffix);

    // Release failed ownership and wake waiters as well as publishing successful modules.
    try {
        shader::usse::SpirvCode source = load_spirv_shader(*program, state.features, true, hints, maskupdate, state.shaders_path, state.shaders_log_path, shader_version, true);
        if (source.empty())
            throw std::runtime_error("shader translation produced no SPIR-V");

        vk::ShaderModuleCreateInfo shader_info{
            .codeSize = sizeof(uint32_t) * source.size(),
            .pCode = source.data()
        };

        module = state.device.createShaderModule(shader_info);
        {
            std::lock_guard<std::mutex> guard(shaders_mutex);
            *shader_module = module;
        }
        shaders_ready.notify_all();
    } catch (...) {
        {
            std::lock_guard<std::mutex> guard(shaders_mutex);
            *shader_module = nullptr;
        }
        shaders_ready.notify_all();
        throw;
    }
    {
        std::lock_guard<std::mutex> guard(shaders_mutex);
        // Save shader cache hashes
        // vertex and fragment shaders are not linked together so no need to associate them
        Sha256Hash empty_hash{};
        if (is_vertex) {
            state.shaders_cache_hashs.push_back({ hash, empty_hash });
        } else {
            state.shaders_cache_hashs.push_back({ empty_hash, hash });
        }
    }

    vk::PipelineShaderStageCreateInfo shader_stage_info{
        .stage = is_vertex ? vk::ShaderStageFlagBits::eVertex : vk::ShaderStageFlagBits::eFragment,
        .module = module,
        .pName = is_vertex ? "main_vs" : "main_fs",
        .pSpecializationInfo = spec_info,
    };

    return shader_stage_info;
}

vk::RenderPass PipelineCache::retrieve_render_pass(vk::Format format, bool depth_load, bool stencil_load, bool force_store, bool is_color_transient, bool no_color, bool has_raw_attachment) {
    const bool with_raw_attachment = has_raw_attachment && !no_color && !is_color_transient && state.features.preserve_f16_nan_as_u16 && format == vk::Format::eR16G16B16A16Sfloat;

    auto &render_passes_map = no_color
        ? shader_interlock_pass
        : (with_raw_attachment
                  ? render_passes_with_raw[depth_load][stencil_load][force_store]
                  : render_passes[is_color_transient][depth_load][stencil_load][force_store]);

    auto it = render_passes_map.find(format);

    if (it != render_passes_map.end())
        return it->second;

    // create a new render pass for this format

    vk::AttachmentReference color_refs[2] = {
        { .attachment = 0, .layout = vk::ImageLayout::eGeneral },
        { .attachment = 1, .layout = vk::ImageLayout::eGeneral }
    };
    vk::AttachmentReference ds_ref{
        .attachment = no_color ? 0U : (with_raw_attachment ? 2U : 1U),
        .layout = vk::ImageLayout::eDepthStencilAttachmentOptimal
    };
    vk::SubpassDescription subpass{
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics
    };

    subpass.setPDepthStencilAttachment(&ds_ref);
    if (!no_color) {
        if (support_coherent_framebuffer_fetch)
            subpass.flags = vk::SubpassDescriptionFlagBits::eRasterizationOrderAttachmentColorAccessEXT;

        subpass.colorAttachmentCount = with_raw_attachment ? 2 : 1;
        subpass.pColorAttachments = color_refs;
        subpass.inputAttachmentCount = (with_raw_attachment && state.features.direct_fragcolor) ? 2 : 1;
        subpass.pInputAttachments = color_refs;
    }

    vk::AttachmentDescription color_attachment{
        .format = format,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = is_color_transient ? vk::AttachmentLoadOp::eDontCare : vk::AttachmentLoadOp::eLoad,
        .storeOp = is_color_transient ? vk::AttachmentStoreOp::eDontCare : vk::AttachmentStoreOp::eStore,
        .initialLayout = is_color_transient ? vk::ImageLayout::eUndefined : vk::ImageLayout::eGeneral,
        .finalLayout = vk::ImageLayout::eGeneral
    };

    vk::AttachmentDescription raw_attachment{
        .format = vk::Format::eR16G16B16A16Uint,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .initialLayout = vk::ImageLayout::eGeneral,
        .finalLayout = vk::ImageLayout::eGeneral
    };

    vk::AttachmentStoreOp store_op = force_store ? vk::AttachmentStoreOp::eStore : vk::AttachmentStoreOp::eDontCare;
    vk::AttachmentDescription ds_attachment{
        .format = state.deep_stencil_use,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = depth_load ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eClear,
        .storeOp = store_op,
        .stencilLoadOp = stencil_load ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eClear,
        .stencilStoreOp = store_op,
        // eUndefined would allow discarding a loaded aspect, so use it only when neither aspect is loaded
        .initialLayout = (depth_load || stencil_load) ? vk::ImageLayout::eDepthStencilReadOnlyOptimal : vk::ImageLayout::eUndefined,
        .finalLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal
    };

    std::array<vk::SubpassDependency, 4> dependencies;

    // external dependency
    // we want the previous render pass to be done when we reach the fragment stage / stencil*depth testing
    dependencies[0] = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
        .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
        .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite
            | vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eInputAttachmentRead
    };

    // BUG: This is dead code (as immediately overwritten below)
    if (state.features.support_shader_interlock && no_color) {
        // we must wait for the previous shaders to be done
        dependencies[1].dstStageMask = vk::PipelineStageFlagBits::eFragmentShader;
        dependencies[1].dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
    }

    // if an attachment is sampled from, we want it to be done before the next render pass fragment shader
    dependencies[1] = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        // Surface readback can blit the attachment before the next scene.
        // Include that transfer read in the dependency before rendering writes it.
        .srcStageMask = vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eTransfer,
        .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eLateFragmentTests,
        .srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead,
        .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite
    };

    if (state.features.support_shader_interlock && !no_color) {
        // we must wait for the shader interlock shader to be done
        dependencies[1].srcAccessMask |= vk::AccessFlagBits::eShaderWrite;
    }

    // self-dependency
    // this allows us to use a pipeline barrier in the render pass for programmable blending
    dependencies[2] = {
        .srcSubpass = 0,
        .dstSubpass = 0,
        .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
        .dstStageMask = vk::PipelineStageFlagBits::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        .dstAccessMask = vk::AccessFlagBits::eInputAttachmentRead,
        .dependencyFlags = vk::DependencyFlagBits::eByRegion
    };

    // mid-scene flush
    // unity games use it to write to a buffer in a vertex shader then use it as the vertex input in the next draw
    dependencies[3] = {
        .srcSubpass = 0,
        .dstSubpass = 0,
        .srcStageMask = vk::PipelineStageFlagBits::eVertexShader,
        .dstStageMask = vk::PipelineStageFlagBits::eVertexInput,
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eVertexAttributeRead
    };

    vk::RenderPassCreateInfo pass_info{};
    vk::AttachmentDescription attachments[] = { color_attachment, with_raw_attachment ? raw_attachment : ds_attachment, ds_attachment };
    pass_info.setAttachments(attachments);
    pass_info.attachmentCount = with_raw_attachment ? 3 : 2;
    pass_info.setSubpasses(subpass);
    pass_info.setDependencies(dependencies);
    if (no_color) {
        // only add the ds attachment
        pass_info.pAttachments = &attachments[2];
        pass_info.attachmentCount = 1;
        // no need for the self-dependency
        pass_info.setDependencyCount(2);
    }

    render_passes_map[format] = state.device.createRenderPass(pass_info);

    {
        // Remember exactly how this render pass was built. A pipeline is only ever rejected
        // *relative to* its subpass, so a failure report is close to useless without this.
        std::string desc = fmt::format(
            "format={} attachments={} no_color={} raw={} transient={} depth_load={} stencil_load={} force_store={}"
            " subpass_flags=0x{:X} color_att={} input_att={} ds_att={}",
            vk::to_string(format), pass_info.attachmentCount, no_color, with_raw_attachment, is_color_transient,
            depth_load, stencil_load, force_store, static_cast<uint32_t>(static_cast<VkSubpassDescriptionFlags>(subpass.flags)),
            subpass.colorAttachmentCount, subpass.inputAttachmentCount, ds_ref.attachment);
        std::lock_guard<std::mutex> guard(diagnostics_mutex);
        render_pass_descriptions[std::bit_cast<uint64_t>(static_cast<VkRenderPass>(render_passes_map[format]))] = std::move(desc);
    }

    render_pass_recipes[render_passes_map[format]] = {
        static_cast<uint32_t>(format), uint32_t(depth_load) | (uint32_t(stencil_load) << 1) | (uint32_t(force_store) << 2) | (uint32_t(is_color_transient) << 3) | (uint32_t(no_color) << 4) | (uint32_t(has_raw_attachment) << 5)
    };
    return render_passes_map[format];
}

// --- SPIR-V introspection ---------------------------------------------------------------------
// Just enough of a walker to answer the questions a rejected pipeline raises: what capabilities
// does the module need, and what does its interface look like? A driver that refuses a module
// nearly always does so because of one of those two things.
static const char *spirv_capability_name(uint32_t cap) {
    switch (cap) {
    case 0: return "Matrix";
    case 1: return "Shader";
    case 9: return "Float16";
    case 10: return "Float64";
    case 11: return "Int64";
    case 22: return "Int16";
    case 25: return "ImageGatherExtended";
    case 32: return "ClipDistance";
    case 33: return "CullDistance";
    case 34: return "ImageCubeArray";
    case 35: return "SampleRateShading";
    case 39: return "Int8";
    case 40: return "InputAttachment";
    case 42: return "MinLod";
    case 43: return "Sampled1D";
    case 44: return "Image1D";
    case 45: return "SampledCubeArray";
    case 46: return "SampledBuffer";
    case 47: return "ImageBuffer";
    case 49: return "StorageImageExtendedFormats";
    case 50: return "ImageQuery";
    case 51: return "DerivativeControl";
    case 55: return "StorageImageReadWithoutFormat";
    case 56: return "StorageImageWriteWithoutFormat";
    case 61: return "GroupNonUniform";
    case 4427: return "DrawParameters";
    case 4433: return "StorageBuffer16BitAccess";
    case 4434: return "UniformAndStorageBuffer16BitAccess";
    case 4436: return "StorageInputOutput16";
    case 4448: return "StorageBuffer8BitAccess";
    case 4449: return "UniformAndStorageBuffer8BitAccess";
    case 5301: return "ShaderNonUniform";
    case 5302: return "RuntimeDescriptorArray";
    case 5345: return "VulkanMemoryModel";
    case 5347: return "PhysicalStorageBufferAddresses";
    case 5363: return "FragmentShaderSampleInterlockEXT";
    case 5372: return "FragmentShaderShadingRateInterlockEXT";
    case 5378: return "FragmentShaderPixelInterlockEXT";
    case 5379: return "DemoteToHelperInvocation";
    default: return nullptr;
    }
}

static std::string spirv_literal_string(const uint32_t *words, size_t max_words) {
    std::string out;
    for (size_t i = 0; i < max_words; i++) {
        for (int b = 0; b < 4; b++) {
            const char c = static_cast<char>((words[i] >> (8 * b)) & 0xFF);
            if (c == '\0')
                return out;
            out.push_back(c);
        }
    }
    return out;
}

static std::string describe_spirv_module(const uint32_t *code, size_t word_count) {
    if (word_count < 5 || code[0] != 0x07230203)
        return "<not a valid SPIR-V module>";

    std::vector<std::string> capabilities;
    std::vector<std::string> extensions;
    std::string entry_point;
    uint32_t interface_count = 0;
    std::map<uint32_t, uint32_t> var_storage_class; // variable id -> storage class
    std::map<uint32_t, uint32_t> var_location; // variable/target id -> Location decoration
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> var_set_binding; // id -> (set, binding)
    std::map<uint32_t, uint32_t> var_builtin;
    std::map<uint32_t, std::string> var_name;
    std::vector<uint32_t> spec_const_ids;

    size_t i = 5;
    while (i < word_count) {
        const uint32_t instr = code[i];
        const uint16_t op = instr & 0xFFFF;
        const uint16_t len = instr >> 16;
        if (len == 0 || i + len > word_count)
            break;
        const uint32_t *w = &code[i];

        switch (op) {
        case 17: // OpCapability
            if (len >= 2) {
                const char *name = spirv_capability_name(w[1]);
                capabilities.push_back(name ? name : fmt::format("Cap{}", w[1]));
            }
            break;
        case 10: // OpExtension
            if (len >= 2)
                extensions.push_back(spirv_literal_string(&w[1], len - 1));
            break;
        case 15: // OpEntryPoint: model, id, name..., interface ids
            if (len >= 4) {
                const std::string name = spirv_literal_string(&w[3], len - 3);
                const size_t name_words = (name.size() / 4) + 1;
                interface_count = static_cast<uint32_t>(len - 3 - name_words);
                entry_point = fmt::format("{} (model {}) interface_vars={}", name, w[1], interface_count);
            }
            break;
        case 5: // OpName
            if (len >= 3)
                var_name[w[1]] = spirv_literal_string(&w[2], len - 2);
            break;
        case 71: // OpDecorate: target, decoration, operands...
            if (len >= 3) {
                const uint32_t target = w[1];
                const uint32_t decoration = w[2];
                if (decoration == 30 && len >= 4) // Location
                    var_location[target] = w[3];
                else if (decoration == 11 && len >= 4) // BuiltIn
                    var_builtin[target] = w[3];
                else if (decoration == 34 && len >= 4) // DescriptorSet
                    var_set_binding[target].first = w[3];
                else if (decoration == 33 && len >= 4) // Binding
                    var_set_binding[target].second = w[3];
                else if (decoration == 1 && len >= 4) // SpecId
                    spec_const_ids.push_back(w[3]);
            }
            break;
        case 59: // OpVariable: result type, result id, storage class
            if (len >= 4)
                var_storage_class[w[2]] = w[3];
            break;
        default:
            break;
        }
        i += len;
    }

    auto list_vars = [&](uint32_t storage_class) {
        std::string out;
        for (const auto &[id, sc] : var_storage_class) {
            if (sc != storage_class)
                continue;
            const auto loc = var_location.find(id);
            const auto bi = var_builtin.find(id);
            if (loc == var_location.end() && bi == var_builtin.end())
                continue; // unlocated / unnamed globals are not interesting here
            const auto nm = var_name.find(id);
            if (!out.empty())
                out += ", ";
            if (bi != var_builtin.end())
                out += fmt::format("{}=builtin{}", nm != var_name.end() ? nm->second : fmt::format("%{}", id), bi->second);
            else
                out += fmt::format("{}@loc{}", nm != var_name.end() ? nm->second : fmt::format("%{}", id), loc->second);
        }
        return out.empty() ? std::string("none") : out;
    };

    std::string descriptors;
    for (const auto &[id, sb] : var_set_binding) {
        const auto nm = var_name.find(id);
        if (!descriptors.empty())
            descriptors += ", ";
        descriptors += fmt::format("{}=set{}.binding{}", nm != var_name.end() ? nm->second : fmt::format("%{}", id), sb.first, sb.second);
    }

    auto join = [](const std::vector<std::string> &items) {
        std::string out;
        for (const std::string &item : items)
            out += (out.empty() ? "" : ", ") + item;
        return out.empty() ? std::string("none") : out;
    };
    std::vector<std::string> spec_ids;
    for (uint32_t id : spec_const_ids)
        spec_ids.push_back(std::to_string(id));

    return fmt::format(
        "words={} entry=[{}]\n"
        "      capabilities: {}\n"
        "      extensions: {}\n"
        "      inputs: {}\n"
        "      outputs: {}\n"
        "      uniform_constants: {}\n"
        "      spec_constant_ids: {}",
        word_count, entry_point,
        join(capabilities),
        join(extensions),
        list_vars(1), // Input
        list_vars(3), // Output
        descriptors.empty() ? std::string("none") : descriptors,
        join(spec_ids));
}

std::string PipelineCache::describe_shader(const Sha256Hash &hash) {
    const std::string file_name = fmt::format("vk{}-{}.spv", shader::CURRENT_VERSION, hex_string(hash));
    const std::vector<uint32_t> source = renderer::pre_load_shader_spirv(state.shaders_path / file_name);
    if (source.empty())
        return fmt::format("<could not read {} back from the shader cache>", file_name);

    return describe_spirv_module(source.data(), source.size());
}

namespace {
struct AttributeFetch {
    vk::Format format;
    // TODO: add support for matrix input for non-regformated attributes
    uint32_t array_size = 1;
    uint32_t array_element_size = 0;
    uint32_t component_size = 0;
    uint32_t size = 0;
};

AttributeFetch attribute_fetch(const SceGxmVertexAttribute &attribute, shader::usse::AttributeInformation info, const bool support_scaled_vertex_attribute, const std::set<vk::Format> &unsupported_rgb_vertex_attribute_formats) {
    AttributeFetch fetch{};
    SceGxmAttributeFormat attribute_format = attribute.format;
    uint8_t component_count = attribute.componentCount;
    if (info.regformat) {
        component_count = info.component_count;
        switch (info.gxm_type) {
        case SCE_GXM_PARAMETER_TYPE_U8:
        case SCE_GXM_PARAMETER_TYPE_S8:
        case SCE_GXM_PARAMETER_TYPE_C10:
            attribute_format = SCE_GXM_ATTRIBUTE_FORMAT_U8;
            break;
        case SCE_GXM_PARAMETER_TYPE_U16:
        case SCE_GXM_PARAMETER_TYPE_S16:
        case SCE_GXM_PARAMETER_TYPE_F16:
            attribute_format = SCE_GXM_ATTRIBUTE_FORMAT_U16;
            break;
        default:
            // U32 format
            attribute_format = SCE_GXM_ATTRIBUTE_FORMAT_UNTYPED;
            break;
        }

        if (info.gxm_type == SCE_GXM_PARAMETER_TYPE_C10)
            component_count = (component_count * 10 + 7) / 8;

        if (component_count > 4) {
            fetch.array_size = (component_count + 3) / 4;
            fetch.array_element_size = 4 * gxm::attribute_format_size(attribute_format);
            component_count = 4;
        }

        fetch.format = translate_attribute_format(attribute_format, component_count, true, false);
        if (component_count == 3 && unsupported_rgb_vertex_attribute_formats.contains(fetch.format)) {
            component_count = 4;
            fetch.format = translate_attribute_format(attribute_format, component_count, true, false);
        }
    } else {
        // some Android GPUs do not support scaled attributes (so do the conversion in the GPU instead)
        if (!support_scaled_vertex_attribute)
            info.is_integer = true;

        // some AMD GPUs do not seem to support rgb vertex attributes
        fetch.format = translate_attribute_format(attribute_format, component_count, info.is_integer, info.is_signed);
        if (component_count == 3 && unsupported_rgb_vertex_attribute_formats.contains(fetch.format)) {
            component_count = 4;
            fetch.format = translate_attribute_format(attribute_format, component_count, info.is_integer, info.is_signed);
        }
    }

    fetch.component_size = gxm::attribute_format_size(attribute_format);
    fetch.size = (fetch.array_size - 1) * fetch.array_element_size + component_count * fetch.component_size;
    return fetch;
}
} // namespace

vk::PipelineVertexInputStateCreateInfo PipelineCache::get_vertex_input_state(const ProgramBinding &vertex_program) {
    // pointer to these objects are returned (so it needs to be static)
    // and each thread needs one (hence the thread_local)
    static thread_local std::vector<vk::VertexInputBindingDescription> binding_descr;
    static thread_local std::vector<vk::VertexInputAttributeDescription> attr_descr;
    binding_descr.clear();
    attr_descr.clear();

    // Vertex attributes.
    VertexProgram *vkvert = vertex_program.vertex_program.get();

    uint32_t used_streams = 0;
    for (const SceGxmVertexAttribute &attribute : vertex_program.attributes) {
        if (vkvert->attribute_infos.contains(attribute.regIndex))
            used_streams |= (1 << attribute.streamIndex);
    }
    std::array<VertexStreamLayout, SCE_GXM_MAX_VERTEX_STREAMS> layouts;
    for (uint32_t stream_index = 0; stream_index < SCE_GXM_MAX_VERTEX_STREAMS; stream_index++) {
        if (used_streams & (1 << stream_index))
            layouts[stream_index] = vertex_stream_layout(vertex_program, stream_index);
    }

    const bool attribute_bindings = needs_attribute_bindings(vertex_program);
    const auto add_binding = [&](uint32_t binding, uint32_t stream_index) {
        const SceGxmVertexStream &stream = vertex_program.streams[stream_index];
        const bool is_instanced = gxm::is_stream_instancing(static_cast<SceGxmIndexSource>(stream.indexSource));
        binding_descr.push_back(vk::VertexInputBindingDescription{
            .binding = binding,
            .stride = layouts[stream_index].stride,
            .inputRate = is_instanced ? vk::VertexInputRate::eInstance : vk::VertexInputRate::eVertex });
    };

    for (size_t a = 0; a < vertex_program.attributes.size(); a++) {
        const SceGxmVertexAttribute &attribute = vertex_program.attributes[a];
        if (!vkvert->attribute_infos.contains(attribute.regIndex))
            continue;

        const shader::usse::AttributeInformation &info = vkvert->attribute_infos.at(attribute.regIndex);
        const AttributeFetch fetch = attribute_fetch(attribute, info, support_scaled_vertex_attribute, unsupported_rgb_vertex_attribute_formats);

        uint32_t binding = attribute.streamIndex;
        if (attribute_bindings) {
            binding = static_cast<uint32_t>(binding_descr.size());
            add_binding(binding, attribute.streamIndex);
        }

        const uint32_t offset = a < VertexStreamLayout::MAX_ATTRIBUTES ? layouts[attribute.streamIndex].offsets[a] : attribute.offset;
        for (uint32_t i = 0; i < fetch.array_size; i++) {
            attr_descr.push_back(vk::VertexInputAttributeDescription{
                .location = info.location + i,
                .binding = binding,
                .format = fetch.format,
                .offset = (attribute_bindings ? 0 : offset) + i * fetch.array_element_size });
        }
    }

    if (!attribute_bindings) {
        for (unsigned int stream_index = 0; stream_index < SCE_GXM_MAX_VERTEX_STREAMS; stream_index++) {
            if (used_streams & (1 << stream_index))
                add_binding(stream_index, stream_index);
        }
    }

    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(binding_descr);
    vertex_input.setVertexAttributeDescriptions(attr_descr);
    return vertex_input;
}

bool PipelineCache::needs_attribute_bindings(const ProgramBinding &vertex_program) const {
    const VertexProgram *vkvert = vertex_program.vertex_program.get();
    const uint32_t max_offset = state.physical_device_properties.limits.maxVertexInputAttributeOffset;
    for (const SceGxmVertexAttribute &attribute : vertex_program.attributes) {
        const auto it = vkvert->attribute_infos.find(attribute.regIndex);
        if (it == vkvert->attribute_infos.end())
            continue;

        const shader::usse::AttributeInformation &info = it->second;
        // a matrix attribute is an array of vec4, the last one furthest along
        const uint32_t array_offset = info.regformat && info.component_count > 4 ? ((info.component_count - 1) / 4) * 16 : 0;
        if (attribute.offset + array_offset > max_offset)
            return true;
    }
    return false;
}

VertexStreamLayout PipelineCache::vertex_stream_layout(const ProgramBinding &vertex_program, const uint32_t stream_index) const {
    VertexStreamLayout layout;
    const uint32_t stride = vertex_program.streams[stream_index].stride;
    layout.stride = stride;

    struct Item {
        uint32_t index;
        uint32_t offset;
        uint32_t size;
        uint32_t align;
    };
    std::array<Item, VertexStreamLayout::MAX_ATTRIBUTES> items;
    uint32_t count = 0;
#ifdef __APPLE__
    // Metal only takes a stride that is a multiple of 4
    bool unaligned = stride % 4 != 0;
#else
    bool unaligned = false;
#endif
    const VertexProgram *vkvert = vertex_program.vertex_program.get();
    for (size_t a = 0; a < vertex_program.attributes.size() && a < VertexStreamLayout::MAX_ATTRIBUTES; a++) {
        const SceGxmVertexAttribute &attribute = vertex_program.attributes[a];
        layout.offsets[a] = attribute.offset;
        if (attribute.streamIndex != stream_index)
            continue;

        const auto it = vkvert->attribute_infos.find(attribute.regIndex);
        if (it == vkvert->attribute_infos.end())
            continue;

        const AttributeFetch fetch = attribute_fetch(attribute, it->second, support_scaled_vertex_attribute, unsupported_rgb_vertex_attribute_formats);
        items[count++] = { static_cast<uint32_t>(a), attribute.offset, fetch.size, fetch.component_size };
        unaligned |= attribute.offset % fetch.component_size != 0 || stride % fetch.component_size != 0;
    }
    if (!unaligned)
        return layout;

    std::sort(items.begin(), items.begin() + count, [](const Item &l, const Item &r) { return l.offset != r.offset ? l.offset < r.offset : l.size > r.size; });
    VertexStreamLayout::Copy block{};
    uint32_t delta = 0;
    for (uint32_t k = 0; k < count; k++) {
        const Item &item = items[k];
        if (k > 0 && (item.offset + delta) % item.align == 0) {
            block.size = std::max(block.size, item.offset + item.size - block.src);
        } else {
            if (k > 0)
                layout.copies[layout.copy_count++] = block;
            const uint32_t dst = align(std::max(layout.record_size, static_cast<uint32_t>(item.offset)), item.align);
            delta = dst - item.offset;
            block = { item.offset, dst, item.size };
        }
        layout.offsets[item.index] = item.offset + delta;
        layout.record_size = std::max(layout.record_size, block.dst + block.size);
    }
    if (count > 0)
        layout.copies[layout.copy_count++] = block;
    layout.stride = stride == 0 ? 0 : align(std::max(layout.record_size, stride), 4);
    layout.repack = true;
    return layout;
}

void PipelineCache::compiler_thread(MemState &mem) {
    pipeline_preparation_worker = true;
    moodycamel::ConsumerToken consumer_token(pipeline_compile_queue);

    // just a single loop, waiting for a pipeline compile request and compiling it
    CompileRequest *request;
    while (true) {
        if (!pipeline_compile_queue.wait_dequeue_timed(consumer_token, request, std::chrono::milliseconds(100))) {
            if (drain_compile_workers)
                break;
            continue;
        }

        if (request == nullptr)
            // use this as an instruction to stop the thread
            break;

        if (drain_compile_workers) {
            util::PipelineScope cancelled(util::PipelineKind::QueueCancelled, { request->telemetry_key, request->telemetry_pair, 1 });
            *request->pipeline = nullptr;
            delete request;
            continue;
        }

        if (request->telemetry_queued) {
            util::PipelineScope queue_delay(util::PipelineKind::QueueDelay, { request->telemetry_key, request->telemetry_pair, 1 }, request->telemetry_queued);
        }
        vk::Pipeline pipeline;
        const auto compile_start = std::chrono::steady_clock::now();
        {
            util::PipelineScope compiling(util::PipelineKind::AsyncCompile, { request->telemetry_key, request->telemetry_pair, 1 });
            pipeline = compile_pipeline(request->type, request->render_pass, *request->vertex_program_binding, *request->fragment_program_binding, *request->get_record(), request->has_color_surface_data, request->hints, request->has_casts, mem);
            compiling.result(bool(pipeline));
        }
        if (pipeline && request->exact_recipe && std::chrono::steady_clock::now() - compile_start >= std::chrono::milliseconds(2)) {
            std::lock_guard<std::mutex> guard(warm_mutex);
            exact_recipes.learn(*request->exact_recipe);
        }
        // mark a refused pipeline as failed rather than leaving it null, which would make every
        // later draw queue the same doomed compilation again
        *request->pipeline = pipeline ? pipeline : std::bit_cast<vk::Pipeline, uint64_t>(~1ULL);

        const auto time_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        next_pipeline_cache_save = time_s + pipeline_cache_save_delay;

        state.shaders_count_compiled++;

        delete request;
    }
}

static vk::StencilOpState convert_op_state(const GxmStencilStateOp &state) {
    return vk::StencilOpState{
        .failOp = translate_stencil_op(state.stencil_fail),
        .passOp = translate_stencil_op(state.depth_pass),
        .depthFailOp = translate_stencil_op(state.depth_fail),
        .compareOp = translate_stencil_func(state.func)
    };
}

vk::Pipeline PipelineCache::retrieve_library(std::vector<uint64_t> key, const vk::GraphicsPipelineCreateInfo &info, bool cache_only) {
    std::shared_ptr<LibraryEntry> entry;
    {
        std::unique_lock<std::mutex> lock(libraries_mutex);
        auto found = libraries.find(key);
        if (found != libraries.end()) {
            entry = found->second;
            if (!entry->complete) {
                if (cache_only)
                    return nullptr;
                ++library_waits;
                util::PipelineScope wait(util::PipelineKind::LibraryWait, { 0, 0, static_cast<uint32_t>(key[0]) });
#if defined(_WIN32)
                // Promote only the compiler whose result this required draw actually needs.
                if (!pipeline_preparation_worker && entry->owner_tid) {
                    HANDLE worker = OpenThread(THREAD_SET_INFORMATION, FALSE, entry->owner_tid);
                    if (worker) {
                        SetThreadPriority(worker, THREAD_PRIORITY_NORMAL);
                        CloseHandle(worker);
                    }
                }
#endif
                entry->ready.wait(lock, [&] { return entry->complete; });
            }
            ++library_hits;
            return entry->pipeline;
        }
        if (cache_only || libraries.size() >= library_limit)
            return nullptr;
        entry = std::make_shared<LibraryEntry>();
#if defined(_WIN32)
        entry->owner_tid = GetCurrentThreadId();
#endif
        libraries.emplace(std::move(key), entry);
    }
    vk::Pipeline pipeline;
    try {
        util::PipelineScope compile(util::PipelineKind::LibraryCompile, { 0, 0, static_cast<uint32_t>(VkGraphicsPipelineLibraryFlagsEXT(static_cast<const vk::GraphicsPipelineLibraryCreateInfoEXT *>(info.pNext)->flags)) });
        const auto result = state.device.createGraphicsPipeline(pipeline_cache, info);
        pipeline = result.result == vk::Result::eSuccess ? result.value : vk::Pipeline{};
        compile.result(bool(pipeline));
        ++library_compiles;
    } catch (const std::exception &error) {
        LOG_WARN("[PIPEGPL] library creation failed; synchronous fallback remains available: {}", error.what());
    }
    {
        std::lock_guard<std::mutex> lock(libraries_mutex);
        entry->pipeline = pipeline;
        entry->owner_tid = 0;
        entry->complete = true;
    }
    entry->ready.notify_all();
    return pipeline;
}

vk::Pipeline PipelineCache::link_pipeline_libraries(const vk::GraphicsPipelineCreateInfo &info, uint64_t compatibility, bool cache_only) {
    auto keys = pipeline_library_keys(info, compatibility);
    std::array<vk::Pipeline, 4> parts;
    const std::array<vk::GraphicsPipelineLibraryFlagsEXT, 4> subsets = {
        vk::GraphicsPipelineLibraryFlagBitsEXT::eVertexInputInterface,
        vk::GraphicsPipelineLibraryFlagBitsEXT::ePreRasterizationShaders,
        vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentShader,
        vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentOutputInterface,
    };
    const std::array pre_dynamic = { vk::DynamicState::eViewport, vk::DynamicState::eScissor, vk::DynamicState::eDepthBias, vk::DynamicState::eLineWidth };
    const std::array frag_dynamic = { vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilReference, vk::DynamicState::eStencilWriteMask };
    vk::PipelineDynamicStateCreateInfo pre_dynamic_info{}, frag_dynamic_info{};
    pre_dynamic_info.setDynamicStates(pre_dynamic);
    if (!state.physical_device_features.wideLines)
        --pre_dynamic_info.dynamicStateCount;
    frag_dynamic_info.setDynamicStates(frag_dynamic);
    for (size_t i = 0; i < parts.size(); ++i) {
        vk::GraphicsPipelineLibraryCreateInfoEXT subset{ .flags = subsets[i] };
        vk::GraphicsPipelineCreateInfo part{
            .pNext = &subset,
            .flags = vk::PipelineCreateFlagBits::eLibraryKHR,
        };
        if (i == 0) {
            part.pVertexInputState = info.pVertexInputState;
            part.pInputAssemblyState = info.pInputAssemblyState;
        } else {
            part.renderPass = info.renderPass;
            part.subpass = info.subpass;
            if (i == 1 || i == 2) {
                part.layout = info.layout;
                part.stageCount = (i == 1 || info.stageCount == 2) ? 1 : 0;
                part.pStages = part.stageCount ? &info.pStages[i - 1] : nullptr;
            }
            if (i == 1) {
                part.pMultisampleState = info.pMultisampleState;
                part.pViewportState = info.pViewportState;
                part.pRasterizationState = info.pRasterizationState;
                part.pDynamicState = &pre_dynamic_info;
            } else {
                part.pMultisampleState = info.pMultisampleState;
                if (i == 2) {
                    part.pDepthStencilState = info.pDepthStencilState;
                    part.pDynamicState = &frag_dynamic_info;
                } else {
                    part.pColorBlendState = info.pColorBlendState;
                }
            }
        }
        parts[i] = retrieve_library(std::move(keys[i]), part, cache_only);
        if (!parts[i])
            return nullptr;
    }
    vk::PipelineLibraryCreateInfoKHR libraries_info{};
    libraries_info.setLibraries(parts);
    // No LINK_TIME_OPTIMIZATION: fast linking is explicitly queried before enabling this path.
    vk::GraphicsPipelineCreateInfo link_info{
        .pNext = &libraries_info,
        .layout = info.layout,
        .renderPass = info.renderPass,
        .subpass = info.subpass,
    };
    if (cache_only)
        link_info.flags |= vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequiredEXT;
    try {
        util::PipelineScope link(util::PipelineKind::LibraryLink);
        const auto result = state.device.createGraphicsPipeline(pipeline_cache, link_info);
        link.result(result.result == vk::Result::eSuccess);
        if (result.result == vk::Result::eSuccess) {
            ++library_links;
            return result.value;
        }
    } catch (const std::exception &error) {
        LOG_WARN("[PIPEGPL] link failed; using synchronous fallback: {}", error.what());
    }
    return nullptr;
}

vk::Pipeline PipelineCache::compile_pipeline(SceGxmPrimitiveType type, vk::RenderPass render_pass, const ProgramBinding &vertex_program_binding, const ProgramBinding &fragment_program_binding, const GxmRecordState &record, bool has_color_surface_data, const shader::Hints &hints, bool has_casts, MemState &mem, bool cache_only) try {
    const VertexProgram &vertex_program = *vertex_program_binding.vertex_program;
    const SceGxmProgram *gxm_fragment_shader = fragment_program_binding.program();
    const VKFragmentProgram &fragment_program = *reinterpret_cast<VKFragmentProgram *>(
        fragment_program_binding.fragment_program.get());

    // the vertex input state must be computed before shader are retrieved in case symbols are stripped
    const vk::PipelineVertexInputStateCreateInfo vertex_input = get_vertex_input_state(vertex_program_binding);

    const bool use_shader_interlock = state.features.support_shader_interlock && gxm_fragment_shader->is_frag_color_used();
    const bool with_raw_attachment = state.features.preserve_f16_nan_as_u16 && !use_shader_interlock && record.color_base_format == SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16 && has_color_surface_data;

    shader::Hints fragment_hints = hints;
    fragment_hints.raw_color_attachment_input = with_raw_attachment && state.features.direct_fragcolor && gxm_fragment_shader->is_frag_color_used();

    const vk::PipelineShaderStageCreateInfo vertex_shader = retrieve_shader(vertex_program_binding.program(), vertex_program.hash, true, fragment_program_binding.is_maskupdate, mem, hints, false, false, cache_only);
    const vk::PipelineShaderStageCreateInfo fragment_shader = retrieve_shader(gxm_fragment_shader, fragment_program.hash, false, fragment_program_binding.is_maskupdate, mem, fragment_hints, record.is_gamma_corrected, has_casts, cache_only);
    if (cache_only && (!vertex_shader.module || !fragment_shader.module))
        // a cache-only request never translates a shader on the calling thread
        return nullptr;
    const vk::PipelineShaderStageCreateInfo shader_stages[] = { vertex_shader, fragment_shader };
    // disable the fragment shader if gxm asks us to
    const bool is_fragment_disabled = record.front_side_fragment_program_mode == SCE_GXM_FRAGMENT_PROGRAM_DISABLED || gxm_fragment_shader->has_no_effect();
    const uint32_t shader_stage_count = is_fragment_disabled ? 1U : 2U;

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = translate_primitive(type)
    };

    const bool two_sided = (record.two_sided == SCE_GXM_TWO_SIDED_ENABLED);

    const vk::PipelineRasterizationStateCreateInfo rasterizer{
        .depthClampEnable = (enable_depth_clamp && state.physical_device_features.depthClamp) ? VK_TRUE : VK_FALSE,
        .polygonMode = translate_polygon_mode(record.front_polygon_mode),
        .cullMode = translate_cull_mode(record.cull_mode),
        // front face is always counter clockwise
        .frontFace = vk::FrontFace::eCounterClockwise,
        .depthBiasEnable = VK_TRUE,
        .lineWidth = 1.0f
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1
    };
    // depth and stencil tests are always enabled on the ps vita as there is almost no cost in doing so
    // on a tiled renderer
    vk::PipelineDepthStencilStateCreateInfo ds_info{
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = (record.front_depth_write_mode == SCE_GXM_DEPTH_WRITE_ENABLED),
        .depthCompareOp = translate_depth_func(record.front_depth_func),
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_TRUE,
        .front = convert_op_state(record.front_stencil_state_op),
        .back = convert_op_state(two_sided ? record.back_stencil_state_op : record.front_stencil_state_op)
    };
    if (fetch_draw_stops_lrz_write(state, record, *gxm_fragment_shader)) {
        ds_info.front.compareOp = vk::CompareOp::eEqual;
        ds_info.front.failOp = vk::StencilOp::eKeep;
    }

    vk::PipelineColorBlendStateCreateInfo color_blending{};
    if (support_coherent_framebuffer_fetch)
        color_blending.flags = vk::PipelineColorBlendStateCreateFlagBits::eRasterizationOrderAttachmentAccessEXT;

    const bool frag_has_no_output = static_cast<bool>(gxm_fragment_shader->program_flags & SCE_GXM_PROGRAM_FLAG_OUTPUT_UNDEFINED);
    std::array<vk::PipelineColorBlendAttachmentState, 2> blend_attachments;
    blend_attachments[1] = vk::PipelineColorBlendAttachmentState{
        .blendEnable = VK_FALSE,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
    };
    if (is_fragment_disabled || frag_has_no_output || use_shader_interlock) {
        // The write mask must be empty as the lack of a fragment shader results in undefined values
        blend_attachments[0] = vk::PipelineColorBlendAttachmentState{
            .blendEnable = VK_FALSE,
            .colorWriteMask = vk::ColorComponentFlags()
        };
        blend_attachments[1].colorWriteMask = vk::ColorComponentFlags();
    } else {
        blend_attachments[0] = fragment_program.blending;
    }
    color_blending.attachmentCount = with_raw_attachment ? 2 : 1;
    color_blending.pAttachments = blend_attachments.data();

    vk::PipelineLayout pipeline_layout = pipeline_layouts[vertex_program.texture_count][fragment_program.texture_count];

    // all of these can be changed at any time using the vita graphics api (like opengl)
    // Because each one can take a lot of different values, it's better to set them as dynamic
    const std::array dynamic_states = {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
        vk::DynamicState::eStencilCompareMask,
        vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eDepthBias,
        vk::DynamicState::eLineWidth,
    };
    vk::PipelineDynamicStateCreateInfo dynamic_info{};
    dynamic_info.setDynamicStates(dynamic_states);
    if (!state.physical_device_features.wideLines)
        dynamic_info.dynamicStateCount--;

    // we still need to specify the viewport and scissor count even though they are dynamic
    vk::PipelineViewportStateCreateInfo viewport{
        .viewportCount = 1,
        .scissorCount = 1
    };

    vk::GraphicsPipelineCreateInfo pipeline_info{
        .stageCount = shader_stage_count,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &ds_info,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = pipeline_layout,
        .renderPass = render_pass,
        .subpass = 0
    };

    std::optional<util::PipelineScope> library_fallback_scope;
    // A fragment-disabled pipeline has no fragment stage. Keep that established monolithic
    // path: an independent FRAGMENT_SHADER library requires a nonzero stageCount.
    if (state.support_fast_pipeline_libraries && shader_stage_count == 2) {
        // Keep the guest-format/raw/interlock token as an extra discriminator only.
        // The library key also includes the actual native render-pass handle: gamma
        // and fallback surface formats are not fully described by these guest hints.
        const uint64_t compatibility = uint64_t(hints.color_format) | (uint64_t(with_raw_attachment) << 32) | (uint64_t(use_shader_interlock) << 33);
        if (auto linked = link_pipeline_libraries(pipeline_info, compatibility, cache_only)) {
            ++pipelines_created;
            return linked;
        }
        if (cache_only)
            return nullptr;
        ++library_fallbacks;
        library_fallback_scope.emplace(util::PipelineKind::LibraryFallback);
    }

    // The stock Adreno driver can SIGSEGV inside createGraphicsPipeline
    // If the log ends on this line this exact vert/frag pair killed the driver's compiler
    if (state.is_adreno_stock && !cache_only)
        LOG_INFO("[PIPE] stock-Adreno: compiling pipeline vert={} frag={} ({})", hex_string(vertex_program.hash),
            hex_string(fragment_program.hash), is_fragment_disabled ? "frag-disabled" : "frag-enabled");

    // vulkan.hpp THROWS on an error result here, so a driver refusing the pipeline used to take
    // the whole emulator down before the check below could run. Some drivers (notably Mesa/Turnip)
    // also report an internal shader-compiler rejection as ErrorOutOfHostMemory rather than a
    // specific error.
    if (cache_only)
        pipeline_info.flags |= vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequiredEXT;
    try {
        const auto result = state.device.createGraphicsPipeline(pipeline_cache, pipeline_info);
        if (result.result == vk::Result::ePipelineCompileRequiredEXT)
            // not in the driver cache
            return nullptr;
        if (result.result != vk::Result::eSuccess) {
            LOG_CRITICAL("Failed to create pipeline: {}", vk::to_string(result.result));
            return nullptr;
        }

        pipelines_created++;
        {
            constexpr bool log_pipeline_keys = false; // ~700 lines per run
            static std::atomic<uint32_t> pipekey_lines{ 0 };
            const uint32_t n = pipekey_lines.fetch_add(1, std::memory_order_relaxed);
            if (log_pipeline_keys && n < 4000) {
                const auto &sf = record.front_stencil_state_op;
                const auto &sb = record.back_stencil_state_op;
                LOG_INFO("[PIPEKEY] #{} vert={} frag={} blend={:016x} layout={:016x} prim={} fmt=0x{:x} cull={} two={} poly={} dfunc={} dwrite={} stF={}/{}/{}/{} stB={}/{}/{}/{} fmode={} fdis={} mask={} gamma={} rp=0x{:x}",
                    n, hex_string(vertex_program.hash).substr(0, 10), hex_string(fragment_program.hash).substr(0, 10),
                    fragment_program.blending_hash, vertex_program_binding.key_hash, static_cast<int>(type),
                    static_cast<uint32_t>(record.color_base_format), static_cast<int>(record.cull_mode), static_cast<int>(record.two_sided),
                    static_cast<int>(record.front_polygon_mode), static_cast<int>(record.front_depth_func), static_cast<int>(record.front_depth_write_mode),
                    static_cast<int>(sf.func), static_cast<int>(sf.stencil_fail), static_cast<int>(sf.depth_fail), static_cast<int>(sf.depth_pass),
                    static_cast<int>(sb.func), static_cast<int>(sb.stencil_fail), static_cast<int>(sb.depth_fail), static_cast<int>(sb.depth_pass),
                    static_cast<int>(record.front_side_fragment_program_mode), is_fragment_disabled ? 1 : 0,
                    record.is_maskupdate ? 1 : 0, record.is_gamma_corrected ? 1 : 0,
                    std::bit_cast<uint64_t>(static_cast<VkRenderPass>(render_pass)));
            }
        }
        return result.value;
    } catch (const vk::SystemError &err) {
        const uint32_t failure_index = pipelines_failed++;
        LOG_CRITICAL("Failed to create pipeline #{} ({} succeeded so far): {}", failure_index,
            pipelines_created.load(), err.what());
        LOG_CRITICAL("  vert shader: {}", hex_string(vertex_program.hash));
        LOG_CRITICAL("  frag shader: {} (fragment {})", hex_string(fragment_program.hash),
            is_fragment_disabled ? "disabled" : "enabled");
        LOG_CRITICAL("  colour base format: 0x{:X} raw_attachment: {} raw_input: {} shader_interlock: {} blend: {} attachments: {}",
            static_cast<uint32_t>(record.color_base_format), with_raw_attachment, fragment_hints.raw_color_attachment_input, use_shader_interlock,
            static_cast<bool>(fragment_program.blending.blendEnable), color_blending.attachmentCount);

        // Dump absolutely everything for the first few failures. Anything less means another
        // round trip, and the state that gets left out is always the state that mattered.
        if (failures_dumped++ < max_failures_dumped) {
            std::string render_pass_desc;
            {
                std::lock_guard<std::mutex> guard(diagnostics_mutex);
                auto it = render_pass_descriptions.find(std::bit_cast<uint64_t>(static_cast<VkRenderPass>(render_pass)));
                render_pass_desc = (it == render_pass_descriptions.end()) ? "<unknown render pass>" : it->second;
            }

            LOG_CRITICAL("=== FULL PIPELINE DUMP (failure #{}) ===", failure_index);
            LOG_CRITICAL("  render pass: {}", render_pass_desc);
            LOG_CRITICAL("  support_coherent_framebuffer_fetch: {} colour blend flags: 0x{:X}",
                support_coherent_framebuffer_fetch,
                static_cast<uint32_t>(static_cast<VkPipelineColorBlendStateCreateFlags>(color_blending.flags)));
            LOG_CRITICAL("  stages: {} (vertex entry main_vs, fragment entry main_fs) vert_spec_info: {} frag_spec_info: {}",
                shader_stage_count, static_cast<bool>(vertex_shader.pSpecializationInfo),
                static_cast<bool>(fragment_shader.pSpecializationInfo));
            LOG_CRITICAL("  pipeline layout: vertex_textures={} fragment_textures={}",
                vertex_program.texture_count, fragment_program.texture_count);
            LOG_CRITICAL("  input assembly: topology={} primitive_restart={}",
                vk::to_string(input_assembly.topology), static_cast<bool>(input_assembly.primitiveRestartEnable));
            LOG_CRITICAL("  rasterizer: depth_clamp={} discard={} polygon={} cull={} front_face={} depth_bias={} line_width={}",
                static_cast<bool>(rasterizer.depthClampEnable), static_cast<bool>(rasterizer.rasterizerDiscardEnable),
                vk::to_string(rasterizer.polygonMode), vk::to_string(rasterizer.cullMode),
                vk::to_string(rasterizer.frontFace), static_cast<bool>(rasterizer.depthBiasEnable), rasterizer.lineWidth);
            LOG_CRITICAL("  multisample: samples={} sample_shading={}",
                vk::to_string(multisampling.rasterizationSamples), static_cast<bool>(multisampling.sampleShadingEnable));
            LOG_CRITICAL("  depth/stencil: test={} write={} compare={} stencil_test={} front(fail={} pass={} depth_fail={} cmp={}) back(fail={} pass={} depth_fail={} cmp={})",
                static_cast<bool>(ds_info.depthTestEnable), static_cast<bool>(ds_info.depthWriteEnable),
                vk::to_string(ds_info.depthCompareOp), static_cast<bool>(ds_info.stencilTestEnable),
                vk::to_string(ds_info.front.failOp), vk::to_string(ds_info.front.passOp),
                vk::to_string(ds_info.front.depthFailOp), vk::to_string(ds_info.front.compareOp),
                vk::to_string(ds_info.back.failOp), vk::to_string(ds_info.back.passOp),
                vk::to_string(ds_info.back.depthFailOp), vk::to_string(ds_info.back.compareOp));
            for (uint32_t att = 0; att < color_blending.attachmentCount; att++) {
                const auto &b = blend_attachments[att];
                LOG_CRITICAL("  blend[{}]: enable={} colour(src={} dst={} op={}) alpha(src={} dst={} op={}) write_mask=0x{:X}",
                    att, static_cast<bool>(b.blendEnable), vk::to_string(b.srcColorBlendFactor),
                    vk::to_string(b.dstColorBlendFactor), vk::to_string(b.colorBlendOp),
                    vk::to_string(b.srcAlphaBlendFactor), vk::to_string(b.dstAlphaBlendFactor),
                    vk::to_string(b.alphaBlendOp), static_cast<uint32_t>(static_cast<VkColorComponentFlags>(b.colorWriteMask)));
            }
            std::string dyn;
            for (uint32_t d = 0; d < dynamic_info.dynamicStateCount; d++)
                dyn += (dyn.empty() ? "" : ", ") + vk::to_string(dynamic_info.pDynamicStates[d]);
            LOG_CRITICAL("  dynamic states ({}): {}", dynamic_info.dynamicStateCount, dyn);
            LOG_CRITICAL("  vertex input: {} bindings, {} attributes",
                vertex_input.vertexBindingDescriptionCount, vertex_input.vertexAttributeDescriptionCount);
            for (uint32_t b = 0; b < vertex_input.vertexBindingDescriptionCount; b++) {
                const auto &bd = vertex_input.pVertexBindingDescriptions[b];
                LOG_CRITICAL("    binding {}: stride={} rate={}", bd.binding, bd.stride, vk::to_string(bd.inputRate));
            }
            for (uint32_t a = 0; a < vertex_input.vertexAttributeDescriptionCount; a++) {
                const auto &ad = vertex_input.pVertexAttributeDescriptions[a];
                LOG_CRITICAL("    attribute loc={} binding={} format={} offset={}", ad.location, ad.binding,
                    vk::to_string(ad.format), ad.offset);
            }
            LOG_CRITICAL("  VERTEX SHADER {}\n      {}", hex_string(vertex_program.hash), describe_shader(vertex_program.hash));
            LOG_CRITICAL("  FRAGMENT SHADER {}\n      {}", hex_string(fragment_program.hash), describe_shader(fragment_program.hash));
            LOG_CRITICAL("  spv files live in: {}", state.shaders_path.string());
            LOG_CRITICAL("=== END PIPELINE DUMP ===");

            // and now find out which single piece of state the driver is objecting to
            bisect_pipeline_failure(pipeline_info);
        }
        return nullptr;
    }
} catch (const std::exception &error) {
    LOG_ERROR("Failed to compile pipeline: {}", error.what());
    return nullptr;
}

// Recreate the same pipeline several times, each time with one aspect neutralised, and report
// which variants the driver accepts. Whatever change makes it succeed is the cause.
void PipelineCache::bisect_pipeline_failure(const vk::GraphicsPipelineCreateInfo &failing_info) {
    LOG_CRITICAL("=== PIPELINE KNOCKOUT BISECT: retrying with individual state neutralised ===");

    auto attempt = [&](const char *what, const vk::GraphicsPipelineCreateInfo &info) {
        try {
            const auto res = state.device.createGraphicsPipeline(pipeline_cache, info);
            if (res.result == vk::Result::eSuccess) {
                LOG_CRITICAL("  [OK  ] {}", what);
                state.device.destroyPipeline(res.value);
            } else {
                LOG_CRITICAL("  [FAIL] {} -> {}", what, vk::to_string(res.result));
            }
        } catch (const vk::SystemError &e) {
            LOG_CRITICAL("  [FAIL] {} -> {}", what, e.what());
        }
    };

    // 1. vertex stage only: isolates "is the vertex shader itself acceptable?"
    {
        vk::GraphicsPipelineCreateInfo info = failing_info;
        vk::PipelineRasterizationStateCreateInfo raster = *failing_info.pRasterizationState;
        raster.rasterizerDiscardEnable = VK_TRUE;
        info.stageCount = 1;
        info.pRasterizationState = &raster;
        attempt("vertex stage only (rasterizer discard)", info);
    }

    // 2. colour blend flags flipped: tests the rasterization-order-attachment-access match
    {
        vk::PipelineColorBlendStateCreateInfo blend = *failing_info.pColorBlendState;
        blend.flags ^= vk::PipelineColorBlendStateCreateFlagBits::eRasterizationOrderAttachmentAccessEXT;
        vk::GraphicsPipelineCreateInfo info = failing_info;
        info.pColorBlendState = &blend;
        attempt(fmt::format("colour blend flags toggled to 0x{:X}",
                    static_cast<uint32_t>(static_cast<VkPipelineColorBlendStateCreateFlags>(blend.flags)))
                    .c_str(),
            info);
    }

    // 3. no blending at all, full write mask
    {
        vk::PipelineColorBlendAttachmentState plain{
            .blendEnable = VK_FALSE,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
                | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
        };
        std::array<vk::PipelineColorBlendAttachmentState, 2> plains{ plain, plain };
        vk::PipelineColorBlendStateCreateInfo blend = *failing_info.pColorBlendState;
        blend.pAttachments = plains.data();
        vk::GraphicsPipelineCreateInfo info = failing_info;
        info.pColorBlendState = &blend;
        attempt("blending disabled", info);
    }

    // 4. default rasterization state
    {
        const vk::PipelineRasterizationStateCreateInfo raster{ .lineWidth = 1.0f };
        vk::GraphicsPipelineCreateInfo info = failing_info;
        info.pRasterizationState = &raster;
        attempt("default rasterization state", info);
    }

    // 5. depth and stencil tests off
    {
        const vk::PipelineDepthStencilStateCreateInfo ds{};
        vk::GraphicsPipelineCreateInfo info = failing_info;
        info.pDepthStencilState = &ds;
        attempt("depth/stencil tests disabled", info);
    }

    // 6. no dynamic state (static viewport and scissor instead)
    {
        const vk::Viewport vp{ .width = 1.0f, .height = 1.0f, .maxDepth = 1.0f };
        const vk::Rect2D sc{ .extent = { 1, 1 } };
        vk::PipelineViewportStateCreateInfo viewport{};
        viewport.setViewports(vp);
        viewport.setScissors(sc);
        vk::PipelineRasterizationStateCreateInfo raster = *failing_info.pRasterizationState;
        raster.depthBiasEnable = VK_FALSE;
        vk::GraphicsPipelineCreateInfo info = failing_info;
        info.pDynamicState = nullptr;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        attempt("no dynamic state (static viewport/scissor, no depth bias)", info);
    }

    // 7. empty vertex input
    {
        const vk::PipelineVertexInputStateCreateInfo vertex_input{};
        vk::GraphicsPipelineCreateInfo info = failing_info;
        info.pVertexInputState = &vertex_input;
        attempt("empty vertex input state", info);
    }

    LOG_CRITICAL("=== END KNOCKOUT BISECT ===");
}

vk::Pipeline PipelineCache::retrieve_pipeline(VKContext &context, SceGxmPrimitiveType &type, bool consider_for_async, MemState &mem) {
    // Sample activity once per 64 draws, including ready hits; avoid a clock call on every draw.
    if ((pipeline_draw_poll++ & 63u) == 0)
        last_pipeline_draw_ns.store(pipeline_clock_ns(), std::memory_order_relaxed);
    const GxmRecordState &record = context.record;
    uint64_t key = hash_pipeline_record(record);
    uint64_t raw_key = XXH3_64bits(&record, record_pipeline_len);

    // add the hash of the blending
    const std::shared_ptr<ProgramBinding> &fragment_program_binding = record.fragment_program_binding;
    const VKFragmentProgram &fragment_program = *reinterpret_cast<VKFragmentProgram *>(
        fragment_program_binding->fragment_program.get());
    key ^= fragment_program.blending_hash;
    raw_key ^= fragment_program.blending_hash;

    // add the hash of the attribute and stream layout
    const std::shared_ptr<ProgramBinding> &vertex_program_binding = record.vertex_program_binding;
    if (must_not_skip_draw(record, consider_for_async))
        learn_protected_pair(vertex_program_binding, fragment_program_binding);
    key ^= vertex_program_binding->key_hash;
    raw_key ^= vertex_program_binding->key_hash;

    // and also add the primitive type
    key ^= static_cast<uint64_t>(type);
    raw_key ^= static_cast<uint64_t>(type);

    uint64_t variant_bits = 0;
    const SceGxmProgram *key_fragment_gxp = fragment_program_binding->program();
    if (key_fragment_gxp && key_fragment_gxp->is_native_color() && fragment_program.output_register_format != SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED)
        variant_bits |= static_cast<uint64_t>(fragment_program.output_register_format) << 56;
    const int key_one_channel = gxm::one_channel_source_component(record.color_surface.colorFormat);
    if (key_one_channel > 0)
        variant_bits |= static_cast<uint64_t>(key_one_channel) << 48;
    key ^= variant_bits;
    raw_key ^= variant_bits;

    const uint32_t cast_units = context.curr_frag_ublock.cast_sampler_bits | context.curr_frag_ublock.cast_phase_bits | context.curr_frag_ublock.raw_cast_bits;
    const bool has_casts = (cast_units & fragment_program_binding->fragment_program->textures_used.to_ulong()) != 0;
    if (has_casts) {
        key ^= 0xC2B2AE3D27D4EB4Full;
        raw_key ^= 0xC2B2AE3D27D4EB4Full;
    }

    // can't use constexpr because of apple clang...
    const vk::Pipeline pipeline_compiling = std::bit_cast<vk::Pipeline, uint64_t>(~0ULL);
    // a pipeline the driver refused: remembered so we don't recompile (and re-log) it on every
    // single draw, which turns one rejected pipeline into a freeze and a gigabyte of log
    const vk::Pipeline pipeline_failed = std::bit_cast<vk::Pipeline, uint64_t>(~1ULL);
    // if the pipeline is in the pipeline cache, we can expect its creation time to be almost instantaneous
    bool already_in_cache = false;

    // Exact learned objects are published under warm_mutex; workers never mutate pipelines.
    auto it = pipelines.find(key);
    std::optional<PipelineDrawActivity> draw_activity;
    if (it == pipelines.end() || it->second == nullptr || it->second == pipeline_compiling)
        draw_activity.emplace(pipeline_draw_busy, last_pipeline_draw_ns);
    const auto recipe = it == pipelines.end() || it->second == nullptr
        ? describe_exact_pipeline(context, type, has_casts, key)
        : std::optional<ExactPipelineRecipe>{};
    if (recipe) {
        if (auto prepared = reuse_exact_pipeline(*recipe)) {
            pipelines[key] = prepared;
            raw_pipeline_keys_seen.insert(raw_key);
            next_pipeline_cache_save = now_s() + pipeline_cache_save_delay;
            ++state.shaders_count_compiled;
            return prepared;
        }
    }
    if (it != pipelines.end()) {
        if (it->second != nullptr) {
            if (it->second == pipeline_failed) {
                util::PipelineScope failed(util::PipelineKind::FailedCached, { key });
                return nullptr;
            }
            if (raw_pipeline_keys_seen.insert(raw_key).second)
                state.pipelines_redundant_avoided++;
            if (it->second == pipeline_compiling) {
                // a worker is compiling it for an earlier draw (a draw that must not be skipped waits for it)
                if (!use_async_compilation || !must_not_skip_draw(record, consider_for_async)) {
                    util::PipelineScope skipped(util::PipelineKind::AsyncSkipped, { key, 0, use_async_compilation ? 1u : 0u });
                    return nullptr;
                }
                util::PipelineScope waiting(util::PipelineKind::ProtectedWait, { key, 0, 5 });
                while (it->second == pipeline_compiling)
                    std::this_thread::yield();
                waiting.result(it->second != pipeline_failed);
                return it->second == pipeline_failed ? nullptr : it->second;
            }
            return it->second;
        }
        already_in_cache = true;
    } else {
        // the pipeline hash was not in the cache;
        it = pipelines.insert({ key, pipeline_compiling }).first;
    }
    raw_pipeline_keys_seen.insert(raw_key);

    // get the correct renderpass here
    const SceGxmProgram *gxm_fragment_shader = fragment_program_binding->program();
    const bool use_shader_interlock = state.features.support_shader_interlock && gxm_fragment_shader->is_frag_color_used();
    const vk::RenderPass render_pass = use_shader_interlock ? context.current_shader_interlock_pass : context.current_render_pass;

    // the warm threads learn from it which draw states new pipelines use
    const WarmState draw_state{
        .color_format = static_cast<uint32_t>(record.color_surface.colorFormat),
        .raw = state.features.preserve_f16_nan_as_u16 && !use_shader_interlock && record.color_base_format == SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16 && static_cast<bool>(record.color_surface.data),
        .casts = has_casts,
        .render_pass = context.current_render_pass,
        .interlock_render_pass = context.current_shader_interlock_pass,
    };
    note_draw_state(draw_state);
    const uint64_t draw_pair = warm_pair_key(vertex_program_binding->vertex_program->hash, fragment_program.hash, fragment_program.output_register_format);
    {
        std::lock_guard<std::mutex> guard(warm_mutex);
        pairs_drawn.insert(draw_pair);
    }
    // update the shader hints
    context.shader_hints.color_format = record.color_surface.colorFormat;
    context.shader_hints.attributes = &vertex_program_binding->attributes;
    context.shader_hints.output_register_format = fragment_program_binding->fragment_program
        ? fragment_program_binding->fragment_program->output_register_format
        : SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED;
    // a native-colour program's register format has to agree with the surface it is drawn to
    if (gxm::LOG_FRAGOUT && context.shader_hints.output_register_format != SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED) {
        static std::mutex fragout_mutex;
        static std::set<std::tuple<const SceGxmProgram *, uint32_t, int>> fragout_seen;
        const std::lock_guard<std::mutex> fragout_lock(fragout_mutex);
        if (fragout_seen.emplace(gxm_fragment_shader, static_cast<uint32_t>(record.color_surface.colorFormat), static_cast<int>(context.shader_hints.output_register_format)).second)
            LOG_INFO("[FRAGOUT] draw: fragment program {} register format {} on colour surface format 0x{:X} (msaa mode {}, native_color={})",
                hex_string(fragment_program_binding->fragment_program->hash).substr(0, 12), static_cast<int>(context.shader_hints.output_register_format),
                static_cast<uint32_t>(record.color_surface.colorFormat), static_cast<int>(fragment_program_binding->fragment_program->multisample_mode), gxm_fragment_shader->is_native_color());
    }

    constexpr bool log_texture_hint_changes = false; // costs a mutex + map lookup per draw
    if constexpr (log_texture_hint_changes) {
        static std::mutex texhint_mutex;
        static std::unordered_map<const SceGxmProgram *, std::array<SceGxmTextureFormat, SCE_GXM_MAX_TEXTURE_UNITS>> texhint_seen;
        static uint32_t texhint_stale_count = 0;
        std::array<SceGxmTextureFormat, SCE_GXM_MAX_TEXTURE_UNITS> now{};
        for (uint32_t i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++)
            now[i] = context.shader_hints.fragment_textures[i];

        std::lock_guard<std::mutex> texhint_lock(texhint_mutex);
        auto [texhint_it, texhint_inserted] = texhint_seen.try_emplace(gxm_fragment_shader, now);
        if (texhint_inserted) {
            std::string fmts;
            for (uint32_t i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++)
                fmts += fmt::format(" [{}]=0x{:08X}", i, static_cast<uint32_t>(now[i]));
            LOG_INFO("[TEXHINT] first draw of fragment program {}:{}", fmt::ptr(gxm_fragment_shader), fmts);
        } else if (texhint_it->second != now) {
            texhint_stale_count++;
            if (texhint_stale_count <= 500) {
                std::string diff;
                for (uint32_t i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++)
                    if (texhint_it->second[i] != now[i])
                        diff += fmt::format(" [{}] 0x{:08X} -> 0x{:08X}", i,
                            static_cast<uint32_t>(texhint_it->second[i]), static_cast<uint32_t>(now[i]));
                LOG_WARN("[TEXHINT] STALE #{}: fragment program {} is being drawn with texture formats "
                         "different from the ones its cached shader was generated for:{}",
                    texhint_stale_count, fmt::ptr(gxm_fragment_shader), diff);
                if (texhint_stale_count == 500)
                    LOG_WARN("[TEXHINT] (further STALE lines suppressed)");
            }
            texhint_it->second = now;
        }
    }

    const bool compile_pipeline_async = !already_in_cache && use_async_compilation && !must_not_skip_draw(record, consider_for_async);
    const uint32_t telemetry_reason = (use_async_compilation ? 1u : 0u) | (already_in_cache ? 2u : 0u) | (must_not_skip_draw(record, consider_for_async) ? 4u : 0u);

    if (compile_pipeline_async && probe_before_defer && support_cache_only_creation) {
        // the driver can already hold this pipeline, for example the same shaders with other render state, and then
        // hands it over without compiling
        vk::Pipeline probed;
        {
            util::PipelineScope probe(util::PipelineKind::CacheProbe, { key, draw_pair, telemetry_reason });
            probed = compile_pipeline(type, render_pass, *vertex_program_binding, *fragment_program_binding, record, static_cast<bool>(record.color_surface.data), context.shader_hints, has_casts, mem, true);
            probe.result(bool(probed));
        }
        if (probed) {
            next_pipeline_cache_save = now_s() + pipeline_cache_save_delay;
            state.shaders_count_compiled++;
            it->second = probed;
            return probed;
        }
    }

    if (compile_pipeline_async) {
        // create the pipeline compile request
        CompileRequest *request = new CompileRequest;
        *request = {
            .pipeline = &it->second,
            .type = type,
            .render_pass = render_pass,
            .vertex_program_binding = vertex_program_binding,
            .fragment_program_binding = fragment_program_binding,
            .has_color_surface_data = static_cast<bool>(record.color_surface.data),
            .hints = context.shader_hints,
            .has_casts = has_casts
        };
        memcpy(request->record_data, &record, record_pipeline_len);
        it->second = pipeline_compiling;

        request->exact_recipe = recipe;
        request->telemetry_key = key;
        request->telemetry_pair = draw_pair;
        request->telemetry_queued = util::pipeline_profile_enabled() ? util::PipelineProfile::counter() : 0;
        pipeline_compile_queue.enqueue(pipeline_compile_queue_token, request);
        util::PipelineScope queued(util::PipelineKind::AsyncQueued, { key, draw_pair, telemetry_reason });

        return nullptr;
    } else {
        // can't wait, compile it right now
        vk::Pipeline result;
        const auto compile_start = std::chrono::steady_clock::now();
        {
            const auto reason = !use_async_compilation ? util::PipelineKind::SyncDisabled
                : already_in_cache                     ? util::PipelineKind::SyncKnownCache
                                                       : util::PipelineKind::SyncProtected;
            util::PipelineScope synchronous(reason, { key, draw_pair, telemetry_reason });
            result = compile_pipeline(type, render_pass, *vertex_program_binding, *fragment_program_binding, record, static_cast<bool>(record.color_surface.data), context.shader_hints, has_casts, mem);
            synchronous.result(bool(result));
        }
        // Do not fill the bounded replay set with cheap cache hits and loading-screen variants.
        if (result && recipe && std::chrono::steady_clock::now() - compile_start >= std::chrono::milliseconds(2)) {
            std::lock_guard<std::mutex> guard(warm_mutex);
            exact_recipes.learn(*recipe);
        }

        const auto time_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        next_pipeline_cache_save = time_s + pipeline_cache_save_delay;

        if (!already_in_cache)
            state.shaders_count_compiled++;

        it->second = result ? result : pipeline_failed;

        return result;
    }
}

vk::ShaderModule PipelineCache::precompile_shader(const Sha256Hash &hash, bool search_first) try {
    if (search_first) {
        std::lock_guard<std::mutex> guard(shaders_mutex);
        auto it = shaders.find(hash);
        if (it != shaders.end())
            return it->second;
    }

    if (!fs::exists(state.shaders_path) || fs::is_empty(state.shaders_path))
        return nullptr;

    return load_cached_shader(hash, fmt::format("vk{}-{}.spv", shader::CURRENT_VERSION, hex_string(hash)));
} catch (const std::exception &error) {
    LOG_ERROR("Could not load cached shader {}: {}", hex_string(hash), error.what());
    return nullptr;
}

vk::ShaderModule PipelineCache::load_cached_shader(const Sha256Hash &hash, const std::string &shader_file_name) try {
    const std::vector<uint32_t> source = renderer::pre_load_shader_spirv(state.shaders_path / shader_file_name);

    if (source.empty())
        return nullptr;

    vk::ShaderModuleCreateInfo shader_info{
        .codeSize = sizeof(uint32_t) * source.size(),
        .pCode = source.data()
    };

    vk::ShaderModule shader = state.device.createShaderModule(shader_info);
    {
        std::lock_guard<std::mutex> guard(shaders_mutex);
        shaders[hash] = shader;
    }
    shaders_ready.notify_all();

    return shader;
} catch (const std::exception &error) {
    LOG_ERROR("Could not load cached shader {}: {}", hex_string(hash), error.what());
    return nullptr;
}

bool PipelineCache::must_not_skip_draw(const GxmRecordState &record, bool consider_for_async) const {
    return !consider_for_async || (sync_new_surface_draws && !can_use_deferred_compilation) || (sync_depth_stencil_writers && writes_depth_or_stencil(record));
}

void PipelineCache::learn_protected_pair(const std::shared_ptr<ProgramBinding> &vertex, const std::shared_ptr<ProgramBinding> &fragment) {
    if (!vertex || !fragment || !vertex->vertex_program || !fragment->fragment_program || fragment->is_maskupdate)
        return;
    const ProtectedPipelinePair pair{ vertex->vertex_program->hash, fragment->fragment_program->hash,
        static_cast<uint32_t>(fragment->fragment_program->output_register_format) };
    std::lock_guard<std::mutex> guard(warm_mutex);
    if (protected_pairs.learn(pair)) {
        util::PipelineScope learned(util::PipelineKind::WarmLearnedPair);
    }
}

void PipelineCache::schedule_exact_locked() {
    if (!warm_enabled || warm_stopping)
        return;
    if (!exact_deferred && exact_scan_generation == warm_registry_generation)
        return;
    exact_scan_generation = warm_registry_generation;
    exact_deferred = false;
    for (const auto &[stored_recipe, pass] : loaded_exact_recipes) {
        if (exact_live_jobs >= exact_ready_limit || warm_pending >= warm_pending_limit) {
            exact_deferred = true;
            break;
        }
        auto recipe = stored_recipe;
        const auto vs = warm_vertices.find(recipe.vertex), fs = warm_fragments.find(recipe.fragment);
        if (vs == warm_vertices.end() || fs == warm_fragments.end())
            continue;
        std::shared_ptr<ProgramBinding> vp, fp;
        for (const auto &v : vs->second) {
            auto binding = v.lock();
            if (binding && binding->key_hash == recipe.layout) {
                vp = std::move(binding);
                break;
            }
        }
        for (const auto &f : fs->second) {
            auto binding = f.lock();
            if (!binding || !binding->fragment_program)
                continue;
            const auto &program = static_cast<const VKFragmentProgram &>(*binding->fragment_program);
            if (program.blending_hash == recipe.blend && static_cast<uint32_t>(program.output_register_format) == recipe.words[28]) {
                fp = std::move(binding);
                break;
            }
        }
        if (!vp || !fp)
            continue;
        canonicalize_exact_pipeline(recipe, vp->vertex_program->textures_used.to_ulong(), fp->fragment_program->textures_used.to_ulong(), SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
        if (exact_jobs.contains(recipe))
            continue;
        auto job = std::make_shared<ExactWarmJob>();
        job->recipe = recipe;
        job->render_pass = pass;
        auto *request = new WarmRequest{ vp, fp, warm_pair_key(recipe.vertex, recipe.fragment, fp->fragment_program->output_register_format),
            util::pipeline_profile_enabled() ? util::PipelineProfile::counter() : 0, 0, true, job };
        if (!warm_queue.push(request, true)) {
            delete request;
            continue;
        }
        exact_jobs.emplace(recipe, job);
        note_exact_coverage_locked(recipe.pipeline_key, request->pair_key, Queued);
        ++warm_pending;
        ++exact_live_jobs;
        ++exact_refill_count;
    }
    warm_work_ready.notify_all();
}

void PipelineCache::warm_program_pair(const std::shared_ptr<ProgramBinding> &vertex, const std::shared_ptr<ProgramBinding> &fragment, bool actual_pair) {
    if (actual_pair && vertex && fragment && vertex->vertex_program && fragment->fragment_program) {
        std::lock_guard<std::mutex> guard(warm_mutex);
        if (!warm_enabled || warm_stopping)
            return;
        const uint64_t identity = warm_pair_key(vertex->vertex_program->hash, fragment->fragment_program->hash, fragment->fragment_program->output_register_format);
        const uint64_t variant = identity ^ std::rotl(vertex->key_hash, 17) ^ std::rotl(static_cast<const VKFragmentProgram &>(*fragment->fragment_program).blending_hash, 31);
        if (!early_pairs.observe(variant, vertex, fragment))
            return;
        if (early_pairs.size() <= 128)
            LOG_INFO("[PIPEEARLY] actual_pair={} variant={} vertex={} fragment={} qpc={}", identity, variant, hex_string(vertex->vertex_program->hash), hex_string(fragment->fragment_program->hash), util::PipelineProfile::counter());
    }
    if (!warm_enabled)
        return;
    std::vector<std::pair<std::shared_ptr<ProgramBinding>, std::shared_ptr<ProgramBinding>>> ready;
    {
        std::lock_guard<std::mutex> guard(warm_mutex);
        const auto register_stage = [&](const auto &binding, bool is_vertex) {
            if (!binding || (is_vertex ? !binding->vertex_program : !binding->fragment_program) || binding->is_maskupdate)
                return;
            auto &registry = is_vertex ? warm_vertices : warm_fragments;
            const auto &hash = is_vertex ? binding->vertex_program->hash : binding->fragment_program->hash;
            if (warm_registered >= warm_program_limit) {
                for (auto *map : { &warm_vertices, &warm_fragments }) {
                    for (auto it = map->begin(); it != map->end();) {
                        auto &items = it->second;
                        const auto old = items.size();
                        std::erase_if(items, [](const auto &item) { return item.expired(); });
                        warm_registered -= old - items.size();
                        if (items.empty())
                            it = map->erase(it);
                        else
                            ++it;
                    }
                }
            }
            auto found = registry.find(hash);
            if (found != registry.end()) {
                // Recreated patcher bindings must not exhaust a program's variant slots.
                auto &items = found->second;
                const auto old = items.size();
                std::erase_if(items, [](const auto &item) { return item.expired(); });
                warm_registered -= old - items.size();
                for (const auto &item : items)
                    if (item.lock() == binding)
                        return;
            }
            if (warm_registered >= warm_program_limit || (found != registry.end() && found->second.size() >= 8)) {
                util::PipelineScope bounded(util::PipelineKind::WarmBounded);
                return;
            }
            registry[hash].push_back(binding);
            ++warm_registered;
            ++warm_registry_generation;
            if (warm_registered <= 256)
                LOG_INFO("[PIPESTAGE] vertex={} hash={} qpc={}", is_vertex, hex_string(hash), util::PipelineProfile::counter());
            util::PipelineScope registered(util::PipelineKind::WarmStageRegistered, { 0, 0, is_vertex ? 1u : 0u });
        };
        register_stage(vertex, true);
        register_stage(fragment, false);
        schedule_exact_locked();
        // Match only previously observed protected pairs, never a guessed stage cross-product.
        for (const auto &pair : loaded_protected_pairs) {
            if ((!vertex || !vertex->vertex_program || vertex->vertex_program->hash != pair.vertex)
                && (!fragment || !fragment->fragment_program || fragment->fragment_program->hash != pair.fragment))
                continue;
            const auto vs = warm_vertices.find(pair.vertex), fs = warm_fragments.find(pair.fragment);
            if (vs == warm_vertices.end() || fs == warm_fragments.end())
                continue;
            for (const auto &v : vs->second) {
                auto vp = v.lock();
                if (!vp)
                    continue;
                for (const auto &f : fs->second) {
                    auto fp = f.lock();
                    if (fp && fp->fragment_program && static_cast<uint32_t>(fp->fragment_program->output_register_format) == pair.output_format) {
                        if (ready.size() < warm_pending_limit)
                            ready.emplace_back(vp, fp);
                        else {
                            util::PipelineScope bounded(util::PipelineKind::WarmBounded);
                        }
                    }
                }
            }
        }
    }
    for (const auto &[vp, fp] : ready)
        queue_warm_pair(vp, fp, true);
    if (vertex && fragment)
        queue_warm_pair(vertex, fragment, false);
}

void PipelineCache::queue_warm_pair(const std::shared_ptr<ProgramBinding> &vertex, const std::shared_ptr<ProgramBinding> &fragment, bool learned) {
    if (!vertex || !fragment || !vertex->vertex_program || !fragment->fragment_program || fragment->is_maskupdate)
        return;
    const uint64_t pair = warm_pair_key(vertex->vertex_program->hash, fragment->fragment_program->hash, fragment->fragment_program->output_register_format);
    const auto &fp = static_cast<const VKFragmentProgram &>(*fragment->fragment_program);
    const uint64_t request_key = pair ^ std::rotl(vertex->key_hash, 17) ^ std::rotl(fp.blending_hash, 31);
    std::lock_guard<std::mutex> guard(warm_mutex);
    if (!warm_enabled)
        return;
    for (const auto &[recipe, job] : exact_jobs) {
        if (job->status != ExactWarmJob::Failed && recipe.vertex == vertex->vertex_program->hash
            && recipe.fragment == fragment->fragment_program->hash && recipe.layout == vertex->key_hash
            && recipe.blend == fp.blending_hash && recipe.words[28] == static_cast<uint32_t>(fp.output_register_format))
            return;
    }
    if (warm_pairs_requested.contains(request_key)) {
        util::PipelineScope duplicate(util::PipelineKind::WarmDuplicate, { 0, pair });
        return;
    }
    const auto attempt = warm_attempts.find(request_key);
    if (warm_pending >= warm_pending_limit
        || (attempt == warm_attempts.end() && warm_attempts.size() >= ProtectedPipelinePairs::limit)
        || (attempt != warm_attempts.end() && attempt->second >= 2)) {
        util::PipelineScope bounded(util::PipelineKind::WarmBounded, { 0, pair });
        return;
    }
    auto *request = new WarmRequest{ vertex, fragment, pair,
        util::pipeline_profile_enabled() ? util::PipelineProfile::counter() : 0, request_key, learned };
    if (!warm_queue.push(request, false)) {
        delete request;
        return;
    }
    warm_pairs_requested.insert(request_key);
    ++warm_attempts[request_key];
    ++warm_pending;
    warm_work_ready.notify_all();
}

void PipelineCache::warm_thread(MemState &mem) {
    pipeline_preparation_worker = true;
#if defined(__linux__)
    // below the guest and render threads: this work only pays off later
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    while (true) {
        WarmRequest *request = nullptr;
        {
            std::unique_lock<std::mutex> lock(warm_mutex);
            // Completion and registration wake this loop. The timeout also reopens quiet-time capacity.
            warm_work_ready.wait_for(lock, std::chrono::milliseconds(25), [&] {
                return warm_stopping || (!warm_queue.empty() && warm_running < PipelinePreparationQueue<WarmRequest *>::concurrency(pipeline_clock_ns(), last_pipeline_draw_ns.load(std::memory_order_relaxed), pipeline_draw_busy.load(std::memory_order_acquire)));
            });
            if (warm_stopping)
                break;
            if (warm_queue.empty() || warm_running >= PipelinePreparationQueue<WarmRequest *>::concurrency(pipeline_clock_ns(), last_pipeline_draw_ns.load(std::memory_order_relaxed), pipeline_draw_busy.load(std::memory_order_acquire)))
                continue;
            request = warm_queue.pop();
            ++warm_running;
        }
        if (request->telemetry_queued) {
            util::PipelineScope delay(util::PipelineKind::WarmQueueDelay, { 0, request->pair_key }, request->telemetry_queued);
        }
        uint32_t outcome = 0;
        const bool ready = !warm_stopping && (request->exact ? warm_exact_pipeline(*request, mem) : warm_pair(*request, mem, outcome));
#if defined(_WIN32)
        // A render-thread wait may have promoted this job. Only this worker restores its priority.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        {
            std::lock_guard<std::mutex> guard(warm_mutex);
            if (!request->exact) {
                if (!ready)
                    warm_pairs_requested.erase(request->request_key);
                if (util::pipeline_profile_enabled() && (pair_coverage.contains(request->request_key) || pair_coverage.size() < warm_program_limit))
                    pair_coverage[request->request_key] = { request->pair_key, request->telemetry_queued, util::PipelineProfile::counter(), outcome };
            }
            --warm_pending;
            --warm_running;
            schedule_exact_locked();
        }
        delete request;
    }
}

void PipelineCache::stop_warm_threads() {
    {
        std::lock_guard<std::mutex> guard(warm_mutex);
        warm_enabled = false;
        warm_stopping = true;
    }
    warm_work_ready.notify_all();
    for (auto &thread : warm_threads) {
        if (thread.joinable())
            thread.join();
    }
    warm_threads.clear();
    std::lock_guard<std::mutex> guard(warm_mutex);
    while (!warm_queue.empty())
        delete warm_queue.pop();
    warm_pending = warm_running = 0;
    warm_stopping = false;
}

bool PipelineCache::codegen_ignores_draw_hints(const SceGxmProgram &program, bool is_vertex, const shader::Hints &hints) {
    struct Variant {
        SceGxmTextureFormat texture;
        SceGxmColorFormat color;
        bool no_attributes;
    };
    static constexpr Variant variants[] = {
        { SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR, false },
        { SCE_GXM_TEXTURE_FORMAT_F32_R, SCE_GXM_COLOR_FORMAT_F16F16F16F16_ABGR, true },
        { SCE_GXM_TEXTURE_FORMAT_F16F16_GR, SCE_GXM_COLOR_FORMAT_U5U6U5_BGR, true },
        { SCE_GXM_TEXTURE_FORMAT_S16S16S16S16_ABGR, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR, false },
    };
    static const std::vector<SceGxmVertexAttribute> no_attributes;
    const bool vary_color = !is_vertex && !program.is_frag_color_used();
    shader::usse::SpirvCode reference;
    for (const Variant &variant : variants) {
        shader::Hints variant_hints = hints;
        std::fill_n(is_vertex ? variant_hints.vertex_textures : variant_hints.fragment_textures, SCE_GXM_MAX_TEXTURE_UNITS, variant.texture);
        if (vary_color)
            variant_hints.color_format = variant.color;
        if (is_vertex && variant.no_attributes)
            variant_hints.attributes = &no_attributes;
        const shader::GeneratedShader generated = shader::convert_gxp(program, "warm", state.features, shader::Target::SpirVVulkan, variant_hints, false, false);
        if (generated.spirv.empty())
            return false;
        if (reference.empty())
            reference = generated.spirv;
        else if (generated.spirv != reference)
            return false;
    }
    return true;
}

bool PipelineCache::shader_available(const SceGxmProgram &program, const Sha256Hash &base_hash, bool is_vertex, const shader::Hints &hints) {
    std::string version_suffix;
    const Sha256Hash hash = shader_cache_key(&program, base_hash, is_vertex, hints, version_suffix);
    {
        std::lock_guard<std::mutex> guard(shaders_mutex);
        const auto found = shaders.find(hash);
        if (found != shaders.end() && found->second != nullptr)
            return true;
    }
    return fs::exists(state.shaders_path / fmt::format("vk{}{}-{}.spv", shader::CURRENT_VERSION, version_suffix, hex_string(base_hash)));
}

bool PipelineCache::warm_pair(const WarmRequest &request, MemState &mem, uint32_t &outcome) try {
    // Outcome bits: drawn=1, missing vertex=2, missing fragment=4,
    // missing pass=8, compile failed=16, prepared=32, exception=64.
    outcome = 0;
    util::PipelineScope total(util::PipelineKind::WarmTotal, { 0, request.pair_key });
    const ProgramBinding &vertex_binding = *request.vertex;
    const ProgramBinding &fragment_binding = *request.fragment;
    const SceGxmProgram &vertex_gxp = *vertex_binding.program();
    const SceGxmProgram &fragment_gxp = *fragment_binding.program();

    std::vector<WarmState> states;
    {
        std::lock_guard<std::mutex> guard(warm_mutex);
        // a draw already created a pipeline for it, so the driver cache holds that draw's state
        if (!request.learned && pairs_drawn.contains(request.pair_key)) {
            outcome |= 1;
            util::PipelineScope skipped(util::PipelineKind::WarmSkipDrawn, { 0, request.pair_key });
            return false;
        }
        states = warm_states;
    }
    if (states.empty()) {
        // nothing drawn yet: an RGBA8 surface without casts
        states.push_back(WarmState{
            .color_format = static_cast<uint32_t>(SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR),
            .render_pass = warm_render_pass,
            .interlock_render_pass = warm_interlock_render_pass,
        });
    }

    shader::Hints hints{
        .attributes = &vertex_binding.attributes,
        .color_format = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
    };
    std::fill_n(hints.vertex_textures, SCE_GXM_MAX_TEXTURE_UNITS, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    std::fill_n(hints.fragment_textures, SCE_GXM_MAX_TEXTURE_UNITS, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    hints.output_register_format = fragment_binding.fragment_program->output_register_format;

    // a stage with no cached module is only generated here when the draw would generate the same module
    if (!shader_available(vertex_gxp, vertex_binding.vertex_program->hash, true, hints) && !codegen_ignores_draw_hints(vertex_gxp, true, hints)) {
        outcome |= 2;
        util::PipelineScope skipped(util::PipelineKind::WarmMissingShader, { 0, request.pair_key });
        return false;
    }

    bool prepared = false;
    const bool interlock = state.features.support_shader_interlock && fragment_gxp.is_frag_color_used();
    for (const WarmState &warm_state : states) {
        hints.color_format = static_cast<SceGxmColorFormat>(warm_state.color_format);
        if (!shader_available(fragment_gxp, fragment_binding.fragment_program->hash, false, hints) && !codegen_ignores_draw_hints(fragment_gxp, false, hints)) {
            outcome |= 4;
            util::PipelineScope skipped(util::PipelineKind::WarmMissingShader, { 0, request.pair_key });
            continue;
        }
        const vk::RenderPass render_pass = interlock ? warm_state.interlock_render_pass : warm_state.render_pass;
        if (!render_pass) {
            outcome |= 8;
            continue;
        }
        GxmRecordState record{};
        record.color_base_format = gxm::get_base_format(hints.color_format);
        const bool has_color_surface_data = warm_state.raw || record.color_base_format != SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16;

        vk::Pipeline pipeline;
        if (support_cache_only_creation) {
            util::PipelineScope probe(util::PipelineKind::WarmProbe, { 0, request.pair_key });
            pipeline = compile_pipeline(SCE_GXM_PRIMITIVE_TRIANGLES, render_pass, vertex_binding, fragment_binding, record, has_color_surface_data, hints, warm_state.casts, mem, true);
            probe.result(bool(pipeline));
        }
        if (!pipeline) {
            util::PipelineScope compile(util::PipelineKind::WarmCompile, { 0, request.pair_key });
            pipeline = compile_pipeline(SCE_GXM_PRIMITIVE_TRIANGLES, render_pass, vertex_binding, fragment_binding, record, has_color_surface_data, hints, warm_state.casts, mem);
            compile.result(bool(pipeline));
        }
        if (!pipeline) {
            outcome |= 16;
            continue;
        }
        outcome |= 32;
        // only the driver cache entry is wanted: the draw creates its own pipeline from it
        state.device.destroyPipeline(pipeline);
        prepared = true;
    }
    return prepared;
} catch (const std::exception &error) {
    outcome |= 64;
    LOG_WARN("[PIPEWARM] background compile failed: {}", error.what());
    return false;
}

void PipelineCache::note_draw_state(const WarmState &draw_state) {
    auto &entry = draw_states[draw_state.key()];
    if (entry.second == 0)
        entry.first = draw_state;
    entry.second++;
    draw_states_total++;

    // the most common state
    std::vector<std::pair<uint32_t, const WarmState *>> ranked;
    for (const auto &[state_key, value] : draw_states)
        ranked.push_back({ value.second, &value.first });
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<WarmState> top;
    for (size_t i = 0; i < ranked.size() && top.size() < 2; i++) {
        if (i > 0 && ranked[i].first * 4 < draw_states_total)
            break;
        top.push_back(*ranked[i].second);
    }
    std::lock_guard<std::mutex> guard(warm_mutex);
    if (top.size() != warm_states.size() || !std::equal(top.begin(), top.end(), warm_states.begin(), [](const WarmState &a, const WarmState &b) { return a.key() == b.key(); }))
        warm_states = top;
}

bool PipelineCache::note_exact_coverage_locked(uint64_t key, uint64_t pair, uint32_t event) {
    if (!util::pipeline_profile_enabled())
        return false;
    auto found = exact_coverage.find(key);
    if (found == exact_coverage.end()) {
        if (exact_coverage.size() >= ExactPipelineRecipes::limit)
            return false;
        found = exact_coverage.emplace(key, ExactCoverage{}).first;
    }
    auto &row = found->second;
    const bool first_event = !(row.events & event);
    row.events |= event;
    if (pair)
        row.pair = pair;
    row.last_qpc = util::PipelineProfile::counter();
    if (!row.first_qpc)
        row.first_qpc = row.last_qpc;
    return first_event;
}

std::optional<ExactPipelineRecipe> PipelineCache::describe_exact_pipeline(const VKContext &context, SceGxmPrimitiveType type, bool casts, uint64_t key) {
    const auto reject = [&](uint32_t event) -> std::optional<ExactPipelineRecipe> {
        std::lock_guard<std::mutex> guard(warm_mutex);
        note_exact_coverage_locked(key, 0, event);
        return {};
    };
    const auto &record = context.record;
    const auto &vp = record.vertex_program_binding;
    const auto &fp = record.fragment_program_binding;
    if (!vp || !fp || !vp->vertex_program || !fp->fragment_program || fp->is_maskupdate)
        return reject(MissingBinding);
    const bool interlock = state.features.support_shader_interlock && fp->program()->is_frag_color_used();
    const auto pass = interlock ? context.current_shader_interlock_pass : context.current_render_pass;
    const auto found = render_pass_recipes.find(pass);
    if (found == render_pass_recipes.end())
        return reject(MissingPass);
    ExactPipelineRecipe r;
    r.vertex = record.vertex_program_hash;
    r.fragment = record.fragment_program_hash;
    if (r.vertex != vp->vertex_program->hash || r.fragment != fp->fragment_program->hash)
        return reject(HashMismatch);
    r.layout = vp->key_hash;
    r.blend = static_cast<const VKFragmentProgram &>(*fp->fragment_program).blending_hash;
    r.pipeline_key = key;
    auto &w = r.words;
    w[0] = static_cast<uint32_t>(type);
    w[1] = static_cast<uint32_t>(record.color_surface.colorFormat);
    w[2] = bool(record.color_surface.data);
    w[3] = casts;
    w[4] = found->second[0];
    // Load/store operations do not change Vulkan render-pass compatibility.
    w[5] = (found->second[1] & 0x38u) | 7u;
    w[6] = static_cast<uint32_t>(record.color_base_format);
    w[7] = static_cast<uint32_t>(record.cull_mode);
    w[8] = static_cast<uint32_t>(record.two_sided);
    w[9] = static_cast<uint32_t>(record.region_clip_mode);
    w[10] = static_cast<uint32_t>(record.front_polygon_mode);
    w[11] = static_cast<uint32_t>(record.back_polygon_mode);
    w[12] = static_cast<uint32_t>(record.front_stencil_state_op.func);
    w[13] = static_cast<uint32_t>(record.front_stencil_state_op.stencil_fail);
    w[14] = static_cast<uint32_t>(record.front_stencil_state_op.depth_fail);
    w[15] = static_cast<uint32_t>(record.front_stencil_state_op.depth_pass);
    w[16] = static_cast<uint32_t>(record.back_stencil_state_op.func);
    w[17] = static_cast<uint32_t>(record.back_stencil_state_op.stencil_fail);
    w[18] = static_cast<uint32_t>(record.back_stencil_state_op.depth_fail);
    w[19] = static_cast<uint32_t>(record.back_stencil_state_op.depth_pass);
    w[20] = static_cast<uint32_t>(record.front_depth_func);
    w[21] = static_cast<uint32_t>(record.back_depth_func);
    w[22] = static_cast<uint32_t>(record.front_depth_write_mode);
    w[23] = static_cast<uint32_t>(record.back_depth_write_mode);
    w[24] = static_cast<uint32_t>(record.front_side_fragment_program_mode);
    w[25] = static_cast<uint32_t>(record.back_side_fragment_program_mode);
    w[26] = static_cast<uint32_t>(record.is_maskupdate);
    w[27] = static_cast<uint32_t>(record.is_gamma_corrected);
    w[28] = static_cast<uint32_t>(fp->fragment_program->output_register_format);
    const bool raw_attachment = state.features.preserve_f16_nan_as_u16 && !interlock && record.color_base_format == SCE_GXM_COLOR_BASE_FORMAT_F16F16F16F16 && bool(record.color_surface.data);
    w[29] = raw_attachment && state.features.direct_fragcolor && fp->program()->is_frag_color_used();
    for (size_t i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; ++i) {
        w[30 + i] = static_cast<uint32_t>(context.shader_hints.vertex_textures[i]);
        w[46 + i] = static_cast<uint32_t>(context.shader_hints.fragment_textures[i]);
    }
    canonicalize_exact_pipeline(r, vp->vertex_program->textures_used.to_ulong(), fp->fragment_program->textures_used.to_ulong(), SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    if (!valid_exact_recipe(r)) {
        std::lock_guard<std::mutex> guard(warm_mutex);
        if (note_exact_coverage_locked(key, 0, InvalidRecipe)) {
            std::string words;
            for (auto value : r.words)
                words += fmt::format("{},", value);
            LOG_INFO("[PIPECOVER] rejected key={} vertex={} fragment={} layout={} blend={} words={}", key, hex_string(r.vertex), hex_string(r.fragment), r.layout, r.blend, words);
        }
        return {};
    }
    return r;
}

vk::Pipeline PipelineCache::reuse_exact_pipeline(const ExactPipelineRecipe &recipe) {
    std::unique_lock<std::mutex> lock(warm_mutex);
    const auto found = exact_jobs.find(recipe);
    if (found == exact_jobs.end()) {
        note_exact_coverage_locked(recipe.pipeline_key, 0, NoJob);
        ++exact_miss_count;
        return nullptr;
    }
    const auto &job = found->second;
    if (job->status == ExactWarmJob::Queued) {
        // A draw must not wait behind speculative queue work. It claims this recipe instead.
        job->status = ExactWarmJob::Taken;
        note_exact_coverage_locked(recipe.pipeline_key, 0, DrawClaimed);
        ++exact_draw_claimed_count;
        --exact_live_jobs;
        warm_work_ready.notify_all();
        return nullptr;
    }
    if (job->status == ExactWarmJob::Running) {
        note_exact_coverage_locked(recipe.pipeline_key, 0, DrawWaited);
        ++exact_wait_count;
#if defined(_WIN32)
        // Resolve priority inversion for required work, without changing guest scheduling or affinity.
        if (const auto worker = OpenThread(THREAD_SET_INFORMATION, FALSE, job->worker_id)) {
            SetThreadPriority(worker, THREAD_PRIORITY_NORMAL);
            CloseHandle(worker);
        }
#endif
        util::PipelineScope wait(util::PipelineKind::ProtectedWait, { recipe.pipeline_key, 0, 0x30 });
        exact_ready.wait(lock, [&] { return job->status != ExactWarmJob::Running; });
    }
    if (job->status != ExactWarmJob::Ready) {
        note_exact_coverage_locked(recipe.pipeline_key, 0, NotReady);
        return nullptr;
    }
    const auto pipeline = job->pipeline;
    job->pipeline = nullptr;
    job->status = ExactWarmJob::Taken;
    --exact_live_jobs;
    schedule_exact_locked();
    note_exact_coverage_locked(recipe.pipeline_key, 0, Reused);
    ++exact_reused_count;
    if (exact_reused_count <= 16)
        LOG_INFO("[PIPEEXACT] reused exact prepared pipeline {}", recipe.pipeline_key);
    return pipeline;
}

bool PipelineCache::warm_exact_pipeline(const WarmRequest &request, MemState &mem) {
    const auto &job = request.exact;
    {
        std::lock_guard<std::mutex> guard(warm_mutex);
        if (job->status != ExactWarmJob::Queued)
            return false;
        job->status = ExactWarmJob::Running;
        note_exact_coverage_locked(job->recipe.pipeline_key, request.pair_key, Started);
#if defined(_WIN32)
        job->worker_id = GetCurrentThreadId();
#endif
    }
    vk::Pipeline pipeline;
    try {
        const auto &r = job->recipe;
        const auto &w = r.words;
        auto record = exact_record(r);
        shader::Hints hints{};
        hints.attributes = &request.vertex->attributes;
        hints.color_format = static_cast<SceGxmColorFormat>(w[1]);
        hints.output_register_format = static_cast<SceGxmOutputRegisterFormat>(w[28]);
        hints.raw_color_attachment_input = w[29];
        for (size_t i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; ++i) {
            hints.vertex_textures[i] = static_cast<SceGxmTextureFormat>(w[30 + i]);
            hints.fragment_textures[i] = static_cast<SceGxmTextureFormat>(w[46 + i]);
        }
        util::PipelineScope compile(util::PipelineKind::WarmCompile, { r.pipeline_key, request.pair_key, 0x30 });
        pipeline = compile_pipeline(static_cast<SceGxmPrimitiveType>(w[0]), job->render_pass, *request.vertex, *request.fragment, record, w[2], hints, w[3], mem);
        compile.result(bool(pipeline));
    } catch (const std::exception &error) {
        LOG_WARN("[PIPEEXACT] exact preparation failed: {}", error.what());
    }
    {
        std::lock_guard<std::mutex> guard(warm_mutex);
        job->pipeline = pipeline;
        if (pipeline)
            ++exact_prepared_count;
        job->status = pipeline ? ExactWarmJob::Ready : ExactWarmJob::Failed;
        note_exact_coverage_locked(job->recipe.pipeline_key, request.pair_key, pipeline ? Prepared : Failed);
        if (!pipeline)
            --exact_live_jobs;
    }
    exact_ready.notify_all();
    return bool(pipeline);
}

} // namespace renderer::vulkan
