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

#include <blockingconcurrentqueue.h>
#include <condition_variable>
#include <optional>
#include <renderer/exact_pipeline_recipes.h>
#include <renderer/pipeline_binding_epochs.h>
#include <renderer/pipeline_preparation_queue.h>
#include <renderer/pipeline_prediction_admission.h>
#include <renderer/pipeline_state_history.h>
#include <renderer/protected_pipeline_pairs.h>
#include <util/containers.h>
#include <vkutil/vkutil.h>

#include <array>
#include <atomic>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

struct SceGxmProgram;
struct SceGxmFragmentProgram;
struct SceGxmVertexProgram;
enum SceGxmPrimitiveType : uint32_t;
struct MemState;

using Sha256Hash = std::array<uint8_t, 32>;

namespace shader {
struct Hints;
}

namespace renderer {

struct GxmRecordState;
struct ProgramBinding;

namespace vulkan {
struct VKState;
struct VKContext;
struct CompileRequest;

using PipelineCompileQueue = moodycamel::BlockingConcurrentQueue<CompileRequest *>;

struct VertexStreamLayout {
    static constexpr uint32_t MAX_ATTRIBUTES = 32;
    struct Copy {
        uint32_t src;
        uint32_t dst;
        uint32_t size;
    };
    bool repack = false;
    uint32_t stride = 0;
    uint32_t record_size = 0;
    std::array<uint32_t, MAX_ATTRIBUTES> offsets{};
    std::array<Copy, MAX_ATTRIBUTES> copies{};
    uint32_t copy_count = 0;
};

class PipelineCache {
    friend struct VKState;

private:
    VKState &state;

    // are we performing pipeline compilation on a parallel thread?
    bool use_async_compilation = false;
    // how many threads are used in case async compilation is enabled
    int nb_worker_threads = 0;

    // does the GPU support vertex attributes with 3 components (like R16G16B16_UNORM), some (like AMD GPUs) don't
    // this is needed when creating the input state
    std::set<vk::Format> unsupported_rgb_vertex_attribute_formats{};
    // does the GPU support SCALED vertex attribute formats (like R8G8_USCALED), some Android GPUs don't
    // we need this information both while creating the pipeline and generating the shader
    bool support_scaled_vertex_attribute;
    // does the GPU support rasterized color attachment (i.e coherent texture fetch)
    // this is needed to properly emulate frag color access in a way similar to opengl framebuffer fetch
    bool support_coherent_framebuffer_fetch;

    // how much time should we wait after the last shader compilation
    // to update the disk-saved shader cache (in seconds)
    static constexpr int pipeline_cache_save_delay = 15;

    vk::PipelineCache pipeline_cache;

    // first index: 0 if color is backed by memory, 1 otherwise
    // second index: 1 if the depth aspect is loaded at scene start, 0 if cleared
    // third index: 1 if the stencil aspect is loaded at scene start, 0 if cleared
    // fourth index: 1 if depth-stencil is force stored, 0 otherwise
    std::map<vk::Format, vk::RenderPass> render_passes[2][2][2][2];
    // variants carrying the raw u16 attachment
    std::map<vk::Format, vk::RenderPass> render_passes_with_raw[2][2][2];
    // render passes used along shader interlock
    std::map<vk::Format, vk::RenderPass> shader_interlock_pass;

    // only used when accessing the shaders map
    std::mutex shaders_mutex;
    std::condition_variable shaders_ready;
    // because of multithreading, we want the pointers to remain stable
    unordered_map_stable<Sha256Hash, vk::ShaderModule> shaders;
    unordered_map_stable<uint64_t, vk::Pipeline> pipelines;
    std::unordered_set<uint64_t> raw_pipeline_keys_seen;

    // the driver can hand over a pipeline from its cache without compiling it (VK_EXT_pipeline_creation_cache_control)
    bool support_cache_only_creation = false;

    // Libraries are kept until cleanup, after all compile workers stop. Full pipelines remain
    // owned by their existing caches; no handle replacement or GPU lifetime change is needed.
    struct LibraryEntry {
        vk::Pipeline pipeline;
        bool complete = false;
        uint32_t owner_tid = 0;
        std::condition_variable ready;
    };
    std::mutex libraries_mutex;
    std::map<std::vector<uint64_t>, std::shared_ptr<LibraryEntry>> libraries;
    static constexpr size_t library_limit = 2048;
    std::atomic<uint64_t> library_hits{ 0 }, library_compiles{ 0 }, library_waits{ 0 }, library_links{ 0 }, library_fallbacks{ 0 };
    vk::Pipeline retrieve_library(std::vector<uint64_t> key, const vk::GraphicsPipelineCreateInfo &info, bool cache_only);
    vk::Pipeline link_pipeline_libraries(const vk::GraphicsPipelineCreateInfo &info, uint64_t compatibility, bool cache_only);

    // background compiles of the vertex/fragment pairs a game creates, so the driver cache already holds their
    // shaders when the first draw needs a pipeline
    struct WarmRequest;
    struct ExactWarmJob {
        ExactPipelineRecipe recipe;
        vk::RenderPass render_pass;
        enum Status { Queued,
            Running,
            Ready,
            Taken,
            Failed } status
            = Queued;
        vk::Pipeline pipeline;
        uint32_t worker_id = 0;
        bool predicted = false, actual_pair = false, prediction_slot = false, draw_waiting = false;
        uint64_t completed_ns = 0, prediction_request_key = 0;
    };
    ExactPipelineRecipes exact_recipes;
    std::vector<std::pair<ExactPipelineRecipe, vk::RenderPass>> loaded_exact_recipes;
    std::map<ExactPipelineRecipe, std::shared_ptr<ExactWarmJob>> exact_jobs;
    std::condition_variable exact_ready;
    uint64_t exact_prepared_count = 0, exact_reused_count = 0, exact_draw_claimed_count = 0;
    uint64_t exact_miss_count = 0, exact_wait_count = 0, exact_refill_count = 0;
    size_t exact_live_jobs = 0;
    uint64_t warm_registry_generation = 0, exact_scan_generation = 0;
    bool exact_deferred = false;
    // Bounded identity coverage survives cheap skips and the rolling slow-event buffer.
    struct ExactCoverage {
        uint64_t pair = 0, first_qpc = 0, last_qpc = 0;
        uint32_t events = 0;
    };
    std::map<uint64_t, ExactCoverage> exact_coverage;
    enum CoverageEvent : uint32_t {
        MissingBinding = 1,
        MissingPass = 2,
        HashMismatch = 4,
        InvalidRecipe = 8,
        NoJob = 16,
        DrawClaimed = 32,
        DrawWaited = 64,
        NotReady = 128,
        Reused = 256,
        Queued = 512,
        Started = 1024,
        Prepared = 2048,
        Failed = 4096,
        Reclaimed = 8192
    };
    bool note_exact_coverage_locked(uint64_t key, uint64_t pair, uint32_t event);
    struct PairCoverage {
        uint64_t pair = 0, queued_qpc = 0, completed_qpc = 0;
        uint32_t outcome = 0;
    };
    std::map<uint64_t, PairCoverage> pair_coverage;
    void schedule_exact_locked();
    static constexpr size_t exact_ready_limit = 256;
    // Render-pass construction parameters, never persisted Vulkan handles.
    std::map<vk::RenderPass, std::array<uint32_t, 2>> render_pass_recipes;
    std::optional<ExactPipelineRecipe> describe_exact_pipeline(const VKContext &context, SceGxmPrimitiveType type, bool casts, uint64_t key);
    vk::Pipeline reuse_exact_pipeline(const ExactPipelineRecipe &recipe);
    bool warm_exact_pipeline(const WarmRequest &request, MemState &mem);
    struct WarmRequest {
        std::shared_ptr<ProgramBinding> vertex;
        std::shared_ptr<ProgramBinding> fragment;
        uint64_t pair_key;
        uint64_t telemetry_queued = 0;
        uint64_t request_key = 0;
        bool learned = false;
        std::shared_ptr<ExactWarmJob> exact;
        bool actual_pair = false;
    };
    PipelinePreparationQueue<WarmRequest *> warm_queue;
    std::condition_variable warm_work_ready;
    std::atomic<uint64_t> last_pipeline_draw_ns{ 0 };
    std::atomic<bool> pipeline_draw_busy{ false };
    uint32_t pipeline_draw_poll = 0;
    size_t warm_running = 0;
    std::vector<std::thread> warm_threads;
    std::atomic<bool> warm_enabled{ false };
    std::atomic<bool> warm_stopping{ false };
    std::mutex warm_mutex;
    std::unordered_set<uint64_t> warm_pairs_requested;
    std::map<uint64_t, WarmRequest *> pending_pair_requests;
    PipelineBindingEpochs<ProgramBinding> early_pairs;
    std::map<uint64_t, unsigned> warm_attempts;
    static constexpr size_t warm_pending_limit = 64, warm_program_limit = 2048;
    size_t warm_pending = 0, warm_registered = 0;
    std::map<Sha256Hash, std::vector<std::weak_ptr<ProgramBinding>>> warm_vertices, warm_fragments;
    ProtectedPipelinePairs protected_pairs;
    std::vector<ProtectedPipelinePair> loaded_protected_pairs;
    void queue_warm_pair(const std::shared_ptr<ProgramBinding> &vertex, const std::shared_ptr<ProgramBinding> &fragment, bool actual_pair);
    void learn_protected_pair(const std::shared_ptr<ProgramBinding> &vertex, const std::shared_ptr<ProgramBinding> &fragment);
    // pairs a draw created a pipeline for before their warm compile started
    std::unordered_set<uint64_t> pairs_drawn;
    vk::RenderPass warm_render_pass;
    vk::RenderPass warm_interlock_render_pass;

    // Recent complete draw states replace the old zero-filled speculative state.
    PipelineStateHistory<vk::RenderPass> warm_state_history;
    size_t dynamic_exact_jobs = 0;
    PipelinePredictionAdmission prediction_admission;
    std::vector<vk::Pipeline> retired_predictions;
    uint64_t prediction_rate_rejected = 0, prediction_full_rejected = 0, prediction_reclaimed = 0, prediction_unrelated = 0;
    void release_prediction_locked(ExactWarmJob &job);
    void prune_predictions_locked(uint64_t now, bool actual_pair);
    void warm_thread(MemState &mem);
    bool warm_pair(const WarmRequest &request, MemState &mem, uint32_t &outcome);
    void stop_warm_threads();
    bool codegen_ignores_draw_hints(const SceGxmProgram &program, bool is_vertex, const shader::Hints &hints);
    bool shader_available(const SceGxmProgram &program, const Sha256Hash &base_hash, bool is_vertex, const shader::Hints &hints);

    // in async mode, true when skipping the draw while its pipeline compiles could leave a glitch outlasting the frame
    bool must_not_skip_draw(const GxmRecordState &record, bool consider_for_async) const;

    // the periodic save runs on its own thread
    std::thread save_thread;
    std::atomic<bool> save_running{ false };
    void write_pipeline_cache(const std::vector<uint64_t> &keys);
    void wait_for_background_save();

    // --- diagnostics -------------------------------------------------------------------------
    // Everything below only exists so that a single run produces enough information to explain a
    // pipeline creation failure without having to rebuild and ask for another log.

    // human readable description of every render pass we have created, keyed by the handle, so a
    // failing pipeline can report the exact subpass it was being created against
    std::map<uint64_t, std::string> render_pass_descriptions;
    std::mutex diagnostics_mutex;
    std::atomic<uint32_t> pipelines_created{ 0 };
    std::atomic<uint32_t> pipelines_failed{ 0 };
    // the full state dump and the knockout bisect are expensive, only do them for the first few
    std::atomic<uint32_t> failures_dumped{ 0 };
    static constexpr uint32_t max_failures_dumped = 3;

    // reads the module back off disk and summarises it; only called when a pipeline is refused, so
    // that shader creation itself carries no diagnostic cost
    std::string describe_shader(const Sha256Hash &hash);
    // retries the failing pipeline with one piece of state neutralised at a time and logs which
    // variant the driver accepts. This is what turns "ErrorOutOfHostMemory" into an actual answer.
    void bisect_pipeline_failure(const vk::GraphicsPipelineCreateInfo &failing_info);

    // ready_only: return an empty stage instead of loading or translating a shader that has no module yet
    vk::ShaderModule load_cached_shader(const Sha256Hash &hash, const std::string &filename);
    vk::PipelineShaderStageCreateInfo retrieve_shader(const SceGxmProgram *program, const Sha256Hash &hash, bool is_vertex, bool maskupdate, MemState &mem, const shader::Hints &hints, bool is_srgb = false, bool has_casts = false, bool ready_only = false);
    vk::PipelineVertexInputStateCreateInfo get_vertex_input_state(const ProgramBinding &vertex_program);

    // queue containing request sent by the main thread to the compile threads
    PipelineCompileQueue pipeline_compile_queue;
    moodycamel::ProducerToken pipeline_compile_queue_token;
    std::vector<std::thread> worker_threads;
    std::atomic<bool> drain_compile_workers{ false };
    void stop_compile_workers(bool drain);

    // each pipeline compiler thread uses this function as its entrypoint
    void compiler_thread(MemState &mem);

    // cache_only: return the pipeline only if the driver has it cached
    vk::Pipeline compile_pipeline(SceGxmPrimitiveType type, vk::RenderPass render_pass, const ProgramBinding &vertex_program_binding, const ProgramBinding &fragment_program_binding, const GxmRecordState &record, bool has_color_surface_data, const shader::Hints &hints, bool has_casts, MemState &mem, bool cache_only = false);

public:
    size_t pipeline_key_count() const { return pipelines.size(); }

    // if not 0, next time the pipeline cache should be saved (in seconds since epoch)
    uint64_t next_pipeline_cache_save = std::numeric_limits<uint64_t>::max();

    // modified by the surface cache, estimates if it is safe to use async pipeline compilation
    // (i.e that it does not causes permanent graphical issues)
    bool can_use_deferred_compilation;

    vk::DescriptorSetLayout uniforms_layout;
    // used for the mask, color attachment
    vk::DescriptorSetLayout attachments_layout;
    std::array<vk::DescriptorSetLayout, 17> vertex_textures_layout;
    std::array<vk::DescriptorSetLayout, 17> fragment_textures_layout;

    // there are at most 16 different textures in the fragment shader, and 16 in the vertex shader
    // first index is vertex, second is fragment
    vk::PipelineLayout pipeline_layouts[17][17] = {};

    PipelineCache(VKState &state);
    void init(bool support_rasterized_order_access);
    void cleanup();

    void read_pipeline_cache();
    void save_pipeline_cache();
    // the periodic save written by a background thread
    void save_pipeline_cache_async();
    // queue a background compile of a vertex/fragment pair the game has just created
    void warm_program_pair(const std::shared_ptr<ProgramBinding> &vertex, const std::shared_ptr<ProgramBinding> &fragment, bool actual_pair = false);

    vk::RenderPass retrieve_render_pass(vk::Format format, bool depth_load, bool stencil_load, bool force_store, bool is_color_transient, bool no_color = false, bool has_raw_attachment = false);
    vk::Pipeline retrieve_pipeline(VKContext &context, SceGxmPrimitiveType &type, bool consider_for_async, MemState &mem);

    vk::ShaderModule precompile_shader(const Sha256Hash &hash, bool search_first = true);

    // true when an attribute offset is past maxVertexInputAttributeOffset
    bool needs_attribute_bindings(const ProgramBinding &vertex_program) const;
    // the binding stride and attribute offsets of a stream, repacked when an attribute address would not be a multiple of its component size
    VertexStreamLayout vertex_stream_layout(const ProgramBinding &vertex_program, uint32_t stream_index) const;

    void set_async_compilation(bool enable);
};
} // namespace vulkan
} // namespace renderer
