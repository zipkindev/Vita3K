# Early shader preparation and fast pipeline linking

Candidate .32 extends .31 with Vulkan graphics pipeline libraries. This path requires both extension dependencies, the graphicsPipelineLibrary feature and the graphicsPipelineLibraryFastLinking property. Unsupported devices keep the existing monolithic pipeline path.

Actual vertex/fragment bindings are offered to the bounded preparation queue while HLE draw commands are being recorded, including precomputed draws. Consecutive identical pairs are filtered at the context and previously offered variants are bounded and deduplicated for the session. Program-creation preparation and saved exact recipes remain available. No guest draw is executed early.

The pipeline cache separates vertex input, pre-rasterization, fragment shader and fragment output libraries. Shader-module/layout handles and structural state keys retain specialization and interface differences. Compatible render-pass load/store variants can share libraries. Missing required libraries compile synchronously; completed libraries link without link-time optimization. Async compilation remains an independent setting and can stay off. This path does not skip required draws or replace shaders with approximations.

The session library cache has a 2,048-entry cap, including failed requests. Duplicate in-flight requests wait for their own result rather than compiling a second copy. Failed/capped library requests fall back to the existing full pipeline creation. Worker scheduling limits from .31 remain. Libraries are destroyed during existing pipeline-cache cleanup after workers stop. No background optimized-handle replacement is implemented.

Pipeline telemetry appends library_compile, library_wait, library_link, library_fallback and early_pair categories. Library compile reasons contain Vulkan subset flags; wait reasons contain subset index (0–3). Existing category numbers are preserved. Aggregate PIPEGPL logs report hits, compilations, waits, links and fallbacks; bounded PIPEEARLY logs include actual-pair identity and QPC time. Elapsed scopes may overlap and must not be added as independent costs.

This is a test candidate. First-use shaders can still stall when their valid specialization/state becomes known too late. Fast linking support alone does not establish a measured improvement, and linked pipelines can have different GPU efficiency. Validate image correctness and worst-frame latency in a short matched run before recommending this path generally.

References: [Khronos graphics pipeline libraries](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_graphics_pipeline_library.html), [fast-linking property](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT.html), [ANGLE implementation design](https://chromium.googlesource.com/angle/angle/+/refs/heads/main/src/libANGLE/renderer/vulkan/doc/PipelineCreation.md).

## .33 correctness isolation and diagnostic control

The `.32` loading attempt lost its Vulkan device before racing. A cache-key audit found that guest format hints did not fully encode actual native render-pass compatibility. `.33` conservatively includes the native render-pass handle in all pass-dependent library keys; cross-handle sharing is deferred until a complete compatibility signature is available. This fixes a demonstrated keying gap, but does not establish the cause of the captured device loss.

For a controlled comparison, launch with `VITA3K_DISABLE_PIPELINE_LIBRARIES=1` to use monolithic pipelines while retaining early preparation. Unset it or use `0` to allow capability-gated libraries. The value is read at device creation, so restart between comparisons. Startup logs report `capable`, `enabled` and `disabled_by_control` independently. Async draw compilation remains a separate setting.

## .34 full device-contract isolation

Libraries are now disabled by default following loading failures. `VITA3K_ENABLE_PIPELINE_LIBRARIES=1` explicitly opts in; `VITA3K_DISABLE_PIPELINE_LIBRARIES=1` always wins. When disabled, both library extension names and the feature structure are removed before device creation. The .33 control removed only the feature structure and library creation path.

`VITA3K_DISABLE_EARLY_PIPELINE_PREPARATION=1` independently bypasses only actual-pair offers from HLE draws. Existing program-creation preparation and exact recipes remain available. Restart between modes. No improvement or GPU fault cause is established by these controls alone.
