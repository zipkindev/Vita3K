# Chenghou Project native pause analysis — October 23, 2026

Two 45-second races used the separately staged optimized Plus v1.3.3-win11diag.1 EXE/PDB, identical graphics profile, BelowNormal priority, all16 logical CPUs, ExternalHost and async compilation. Owner selected Chenghou Project and initially reported FEISAR HD. They later clarified that the repeat used Harimau. Both were driven with working graphics/controls; the first had confirmed noticeable pauses. The repeat used the same process with retained caches, but the ship changed, so these are not a strict controlled ship-matched comparison; complete texture residency is not established. Both foreground screenshots show active lap1 racing. Owner also confirmed continuous racing without pause/results throughout the repeat.

| Capture | Presents | Median / p99 / maximum ms | Gaps >50 / >100 ms |
| --- | --- | --- | --- |
| Initial | 1348 | 33.320 / 35.822 / 115.343 | 6 / 2 |
| Repeat | 1342 | 33.337 / 35.687 / 300.069 | 6 / 2 |

Both ETLs report zero lost events, no truncation, 1ms sampling, and the exact executable's PDB GUID3b4f6a08-f7ce-47ec-bf34-79c2453cb079 / age1. PresentMon raw QPC and ETW raw QPC share a verified10MHz clock. Present coverage ~99.8%. CDB private symbols verified and debugger detached before sampling.

The unnamed native command-processing thread5944 runs:

`renderer::render_loop -> process_batches -> process_batch -> cmd_set_state_texture -> vulkan::sync_texture -> TextureCache::cache_and_bind_texture -> upload_texture -> texture::decompress_compressed_texture -> pvr::PVRTDecompressPVRTC -> pvrtcDecompress / interpolateColors / pvrtcGetDecompressedPixels`.

Every >50ms gap overlaps CPU PVRTC decoding on that thread. The following counts are 1ms sampled stacks, not exact function durations. Off-CPU times are intersections of matched switch-out/in intervals with the same QPC present-gap window. Guest Graphics::RenderThread is3972; SceGxmDisplayQueue is5948; guest main4204.

| Run | Gap offset s | Gap ms | Worker samples / PVRTC samples | Worker off CPU ms | Guest render off CPU ms | Display off CPU ms |
| --- | --- | --- | --- | --- | --- | --- |
| Initial | 12.2571 | 58.224 | 49 / 37 | 7.223 | 50.767 | 25.953 |
| Initial | 12.9813 | 115.343 | 115 / 103 | 0.448 | 104.877 | 84.840 |
| Initial | 15.4371 | 52.942 | 50 / 37 | 1.203 | 43.727 | 23.300 |
| Initial | 15.5769 | 61.384 | 60 / 46 | 0.210 | 51.303 | 29.157 |
| Initial | 19.7821 | 100.122 | 100 / 84 | 0.097 | 86.053 | 68.894 |
| Initial | 34.4800 | 64.419 | 63 / 48 | 0.510 | 53.820 | 34.786 |
| Repeat | 1.9563 | 90.005 | 86 / 73 | 0.650 | 76.728 | 56.389 |
| Repeat | 2.5989 | 300.069 | 298 / 277 | 1.495 | 281.264 | 266.455 |
| Repeat | 2.7301 | 81.003 | 81 / 69 | 0.809 | 68.198 | 63.947 |
| Repeat | 20.7854 | 86.974 | 87 / 73 | 0.314 | 75.009 | 54.025 |
| Repeat | 29.8557 | 57.031 | 55 / 37 | 0.531 | 43.652 | 25.972 |
| Repeat | 44.1471 | 115.892 | 109 / 95 | 3.152 | 105.788 | 84.583 |

In the repeat's300.069ms gap the worker stays on CPU for~298.574ms,277of298samples include PVRTC decoding,282include texture upload. Guest render is off CPU281.264ms, including one227.629ms interval; display is off CPU266.455ms, including234.178ms continuous overlap. This reproduces the broad busy-worker/downstream-wait pattern from the earlier unsymbolized trace, now with resolved operations. It does not retroactively prove the older worker's function identity.

First capture GPU activity median10.689ms/max13.486ms; at its115.343ms gap GPU activity12.989ms and Present API0.554ms. These support a CPU texture-upload/decode bottleneck in this capture rather than the GPU executing115ms of work. No OS scheduler starvation of worker5944 is shown; it is predominantly running. JIT work is present on other threads and guest dynamic stacks can remain incomplete. Sampled calls do not identify texture dimensions/content, unique textures versus reuploads, cache eviction causes or lock ownership. Several shorter gaps include other CPU work too. Correlation plus the synchronous source path gives a strong current lead; no performance fix has yet been measured.

## Source comparison and next change

Pinned Plus a86b5f93, mainline5844e21f and nxdb030b5a have identical `vita3k/renderer/src/texture/pvrt-dec.cpp` blob6c7c9c843b5cf577c4c72eb32fd6aee6d7e894b6. Plus already has full-mip cache sizing and GPU-write hashing behavior analogous to nx131a9f879/29e541d0c. Nx1881b9adb concerns shader/pipeline-cache persistence, not this decoded PVRTC pixel path. Do not cherry-pick scheduler or shader-cache PRs as a demonstrated fix for these gaps, or remove hashing/coherency to hide uploads.

Prioritize an optional bounded CPU decoded-content cache and telemetry before bulk integration. Existing GPU texture cache has1024entries; its residency is not the same as persistent decoded CPU pixels. A CPU cache must compare actual source bytes plus dimensions and format, preserve decode output exactly, bound pixels and source snapshots, track hits/misses/evictions, and keep the unmodified decoder available. Source texture addresses alone are insufficient because games rewrite/reuse them. Cache hits cannot remove genuinely new-content decoding; first misses still decode synchronously.

A separate experimental branch now implements an opt-in cache, disabled by default, with1–512MiB configuration limit, maximum4096entries and exact compressed-byte collision checks. It snapshots the bytes it decodes, so its stored key matches its output even if guest memory later changes. Local576 output comparisons match uncached results (PVRTC-I/II,2/4bpp, small mip sizes, moved/changed content, oversize bypass). Hash collisions, LRU, byte/entry bounds, duplicates and concurrency tested; cache container passes ASAN+UBSAN (leak detection unavailable under sandbox ptrace). A local synthetic1024x1024 PVRTC4 test gives~29.806ms uncached versus0.361ms warmed hits including output comparison; this is not a Go gameplay benchmark or evidence of actual game cache-hit rates. Game graphics, Windows build and repeat performance remain to be tested before proposing a fix upstream.

Raw ETLs, full stacks, absolute machine paths and images remain private/local. Reviewed source provenance and aggregate results can be published in the user's fork. Production binaries, graphics, controls, save and launcher mapping have not been replaced; current diagnostic progress is stored separately.

## Additional capture and context correction

Owner clarified Harimau was used for both the45s repeat and the additional60s sample (not FEISAR HD for the repeat). Original capture metadata is preserved alongside corrected labels. The finding is based on CPU operations within each gap, not an inferred performance change between ships. Keep Harimau fixed for the cache on/off test.

Additional60s sample, exact track not reconfirmed:1798presents,median33.309ms,p9935.643ms,max116.051ms,6gaps>50ms/1>100ms,zero lost events,not truncated. All six gaps include PVRTC decoding on the same command worker; the116.051ms gap has99PVRTC samples of114worker samples. Do not pool this sample as a controlled Chenghou comparison; foreground context was checked before recording but full-window race context was not reconfirmed.

The diagnostic portable log records at02:33:34.386 Eastern: `Texture cache is full. Starting to replace textures`. This occurs before the first capture's observed gaps and supports GPU-cache replacement as one possible reason decoded content gets uploaded again. It does not identify which PVRTC textures were evicted or prove that eviction explains each pause.

## Diagnostic audio regression and required control baseline

Owner reports music stopped since diagnostic.1. Its portable log confirms repeated ATRAC9 ERR_BUFFER_OVERRUN0x81000001 with `size0`, from02:32:26 onward. Diagnostic and production audio settings match:SDL,volume100,NGS enabled. This was introduced by our diagnostic input-span patch: NGS called `send(input,0)` using the historical unused-size placeholder, but the newly enforced bound interpreted it as no input. This is not a demonstrated regression in the installed Plus release or an effect of the not-yet-running pixel cache.

Correct the NGS caller to pass the actual remaining bytes in its already assembled complete superframe and decrement by consumed bytes; keep strict decoder bounds. Remove the misleading default zero argument on the typed ATRAC9 override. A new codec-level test rejects the zero span, decodes two synthetic four-frame superframes with positive decreasing spans, verifies PCM and final padding/reset. Preliminary C-library test validates this synthetic sequence. Actual game music must be verified after rebuilding. The first cache build39914747666 was canceled because it inherited the caller bug; no.2installation was launched.

The existing captures still identify PVRTC work during their recorded pauses, but.1has a functional audio regression and is not a fully equivalent production-performance baseline. Use the corrected.2binary with caching disabled to establish music and timing first, then enable caching in that same binary for a fair on/off comparison. Do not compare cache-on.2against silent-audio.1and attribute every change to caching.
