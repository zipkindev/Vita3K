# Community performance findings

These findings summarize Windows Legion Go testing through October 24, 2026. WipEout 2048 is the primary development workload; DOA5 Plus and DOA Xtreme 3 Venus provide additional compatibility and bottleneck checks. Improvements to shared code can help other compatible workloads, but testing two or three titles does not validate every game or handheld.

## Measurement method and limits

We capture PresentMon presentation intervals and Windows CPU traces, usually for 90 seconds after the player confirms active gameplay. Early Ubermall comparisons used 45 seconds. The player reports deaths, pauses, restarts and finishes; interrupted attempts are kept separate from clean comparisons. Some initial traces started too late or included uncertain transitions and are not treated as clean races.

A fresh process has a cold decoded RAM cache, but retains disk shader/pipeline caches and the explicitly identified learned texture profile. It is not a completely uncached installation. Most WipEout comparisons use Vulkan at 2×, AF4, async pipeline compilation, 512 MiB decoded cache, two decode workers and the same saved input settings. Older 256 MiB and Off tests are labeled separately. Ship, traffic, weapons and fight stages were not deterministic; sequential tests can also benefit from cache warming. Medians/p99 describe presentation, not guest simulation rate or controller-to-display latency.

CPU attribution uses exact EXE/PDB identities and sampled native stacks. Sample counts are not exact function duration. JIT elapsed spans and synchronization spans can overlap; adding them does not give the duration of a pause. Missing detail can reflect rolling-buffer limits. Texture “first observed” means observed in this process's bounded reuse history, not first ever seen or necessarily absent from the saved archive.

## WipEout: repeated texture decoding

The initial Plus-derived diagnostic runs identified CPU-heavy PVRTC decompression during long renderer stalls. Early silent-audio traces helped identify the path but are excluded from the corrected-audio comparison. Bounded decoder input handling and the corrected NGS audio caller were retained in the working build recipe; audio was subsequently confirmed in gameplay. The audio correction is not a texture-cache performance result.

In the corrected-audio Ubermall pair, the same EXE/PDB and graphics settings were used:

| 45-second capture | Cache Off | 256 MiB cache |
|---|---:|---:|
| Median interval | 33.318 ms | 33.360 ms |
| p99 interval | 39.426 ms | 35.433 ms |
| Maximum interval | 242.361 ms | 63.845 ms |
| Intervals above 50 ms | 10 | 1 |
| Intervals above 100 ms | 2 | 0 |

All ten Off gaps contained PVRTC samples. The largest had a CPU-busy command worker with 218 PVRTC samples while guest rendering waited. The remaining On gap contained mixed decode, upload/hash and off-CPU work. A later clean Ubermall repeat had maximum 65.365 ms and no intervals above 100 ms. This supports reusing decoded textures for this workload, without establishing a universal fix or host scheduler defect. [Detailed earlier comparison](ubermall-cache-20261023.md).

## Parallel decoding, entry capacity and preparation

Parallel decoding divides eligible PVRTC work between the caller and one helper. It is bounded parallel work, not speculative asynchronous decoding of unknown future inputs. Small textures remain serial. The cache entry ceiling was raised to 16,384 while retaining the selected byte budget. Owner testing reported smoother repeats; cache size, entry pressure, changing effects and warm-up require separate telemetry rather than attributing every change to parallelism alone. [Parallel decoding](parallel-pvrtc-trial.md) and [entry-limit trial](cache-entry-limit-trial.md).

The `.5` preparation prototype loaded 3,151 learned textures in 2.23 seconds, using about 314 MiB of a 512 MiB decoded budget. One clean sequential Chenghou Rapier pair compared fresh processes using the same learned input:

| 90-second race | Preparation Off | Preparation On |
|---|---:|---:|
| Maximum interval | 161.75 ms | 36.62 ms |
| Intervals above 50 ms | 6 | 0 |
| Demand decodes, about 89 seconds of telemetry | 1,776 | 2 |

This moved substantial known decoding work before guest execution. It does not prepare genuinely unseen inputs, upload textures to the GPU, detect every race loading screen or warm JIT code. [Preparation design](texture-preloading.md).

## An unsuccessful retention experiment

The `.12` experiment changed how prepared entries were retained. Two clean fresh-process Chenghou Rapier four-lap races used the same frozen 7,118-record profile, the same 512 MiB budget and two decode workers. Both prepared all records before racing.

| Approximately 89 seconds of texture telemetry | `.11` baseline | `.12` candidate |
|---|---:|---:|
| Cache hits | 98.66% | 63.23% |
| Demand decodes | 45 | 1,346 |
| Decodes after known capacity eviction | 2 | 1,284 |
| Byte-capacity evictions | 3 | 219 |
| Longest presentation interval | 162.66 ms | 162.70 ms |

The changed retention order was not promoted. Unit tests confirmed its intended ordering, but gameplay measurements showed worse reuse. Large texture size, ranking and startup working-set turnover remain candidates for admission-policy research, not proven causes from this pair alone. `.13` and the UI candidate use `.11` cache behavior and exclude the `.12` change.

## Why bounded learning and profile merging were added

The earlier learning snapshot saved only compressed inputs still represented in the decoded cache at shutdown. Eviction could therefore lose learned inputs. Previous compatible profile records were not fully merged. Controlled tests sometimes restored an older frozen profile intentionally, which must not be mistaken for an ordinary learning failure: the latest `.11` output did contain newly learned inputs.

`.13` keeps an independent compressed-input history, capped at 64 MiB including packed-record accounting and 16,384 records, with bounded container overhead extra. It validates and merges previous learning, deduplicates exact inputs and atomically saves the bounded selection on normal close. It adds learning RAM separate from the decoded budget. Profiles remain at most 64 MiB per game and 1 GiB total; oldest observations/profiles can be removed at their limits. Archive size can exceed decoded capacity, so preparation admits fitting entries within the existing budget and five-second soft deadline.

On the Go, `.13` imported all 7,257 records from the latest `.11` profile and prepared them in 4.456 seconds. After learning and a clean warm capture, normal close saved 7,281 records: exact comparison retained every previous input and added 24, with no history drops or evictions. A fresh process then imported that merged profile; 3,493 entries filled exactly 512 MiB, with 3,788 skipped by the decoded budget. All 7,281 remained in learning history.

| Clean 90-second Chenghou Rapier, four laps | `.13` warm | `.13` fresh process with merged profile |
|---|---:|---:|
| Median interval | 33.339 ms | 33.359 ms |
| p99 interval | 35.196 ms | 35.296 ms |
| Maximum interval | 37.948 ms | 97.012 ms |
| Intervals above 50 ms | 0 | 1 |
| Intervals above 100 ms | 0 | 0 |

The owner described the fresh-process run as “felt flawless.” Its isolated 97 ms interval ended about 1.27 seconds into recording and overlapped 52.532 ms of main-thread JIT compilation. The surrounding one-second texture bucket totaled only 1.485 ms of decoding. This makes JIT a follow-up candidate; it does not establish the full cause without further native-stack analysis.

After this run, normal close saved 7,349 records, retaining all 7,281 prior exact inputs and adding 68. Full container and per-input hashes were checked, followed by byte-for-byte identity comparison. There were no history drops or evictions. The profile was 64,347,612 bytes, below its cap; later sessions may reach that bound and legitimately evict older observations. This verifies learning retention at current bounds, not that every future profile will fit decoded RAM.

## DOA5 Plus: pipeline compilation was the main lead

DOA5 Plus was tested in `.11`, at 512 MiB, initially with Vulkan 1× and async pipeline compilation disabled. The owner reported early stutters followed by smoother play. Seven measured intervals above 50 ms were distributed throughout the recording, so the trace does not support a simple claim that only the first fight stuttered.

All seven gaps were dominated by renderer-side Vulkan pipeline compilation. The largest was 298.198 ms, with 287 of 292 renderer CPU samples in the compilation path and only 6.37 ms of renderer off-CPU overlap. This did not support PVRTC decoding or scheduler waits as the dominant source of that gap.

| 90-second gameplay capture | 1× synchronous | 1× async | 2× async |
|---|---:|---:|---:|
| Median interval | 16.664 ms | 16.677 ms | 16.643 ms |
| p99 interval | 18.629 ms | 18.573 ms | 18.793 ms |
| Maximum interval | 298.198 ms | 36.351 ms | 45.391 ms |
| Intervals above 50 ms | 7 | 0 | 0 |
| Intervals above 100 ms | 5 | 0 | 0 |

The async run moved substantial sampled compilation work off the renderer onto existing compiler workers. The owner confirmed smoother fighting and correct graphics; the subsequent 2× run was also reported smooth. Async pipeline compilation is an existing emulator feature, not an invention of this fork. Disk caches were retained, order was sequential and exact fighters/stages were not controlled, so this is corroborated practical evidence rather than an isolated universal speedup estimate.

Runtime reports saved no learned PVRTC entries for these DOA5 sessions; sampled windows did not show PVRTC decoding. These results must not be advertised as proof that the learned-texture cache improved DOA5.

## DOA Xtreme 3 Venus and other limitations

A clean 90-second `.11` gameplay capture at Vulkan 3×, AF8, async compilation Off and a 512 MiB decoded budget was reported smooth. Median presentation interval was 33.354 ms, p99 38.388 ms and maximum 145.833 ms, with 14 intervals above 50 ms and two above 100 ms. Several gaps included JIT work; the largest also included synchronous pipeline compilation and guest/renderer waiting. Other gaps had I/O or UI work. The sampled window did not show PVRTC decoding and the saved learned profile was empty.

Async compilation was subsequently enabled for that title at the owner's request, but its controlled follow-up capture was deferred. Do not infer a measured async gain or a texture-cache gain for this title. Likewise, no validated Steam Deck, ROG Ally or other-platform measurements are available from this investigation.

## Diagnostic and provenance lessons

Early bounded JIT detail could fill before useful race activity. Later versions added rolling detail and bounded full-session summaries. Synchronization telemetry records completed API spans, not pure OS sleep; nested spans overlap. Texture reuse telemetry distinguishes capacity/clear-related redecoding within bounded recall. Optional diagnostics add overhead and should be disabled for routine play. Missing or replaced history must remain visible in reports.

Native Windows tests cover cache/decoder correctness, learning bounds and merging, profile lifecycle/storage, settings layout, JIT summaries, synchronization reports and ATRAC9 input bounds. `.13` passed 19 native tests, plus independent package hashes, dependency patch provenance and EXE/PDB identity verification. Decoder comparisons and no-play profile merge checks complement gameplay testing; tests alone do not prove better frame pacing.

| Build | Exact source | Purpose |
|---|---|---|
| `.11` | `215f584c9be5eff44160671a741238e82ed0c213` | Working reuse-telemetry baseline; DOA tests |
| `.13` | `5a88690e5b1584f5b1e2be452f3f9a29cdcd869a` | Bounded learning and profile merging |
| `.14` | Recorded in the release manifest | UI-label and documentation follow-up; build/staging results tracked in release notes |

`.13` EXE SHA-256: `647344142a59a623189511dd5072f8eb4dfff3d279e16ccdf08412b2fd288cc2`. Its PDB SHA-256 is `fea199909354d4e1bbcb2c27a6f8365a15bbd46ab76f1cd07f27c1dccbf8d7fb`, GUID `0f40747a-ac7c-48be-a107-8b372aea0d3a`, age 1. The [Windows runner job](https://github.com/zipkindev/Vita3K-Go/actions/runs/40077345873) identifies the build. Manifests additionally record the recipe and applied support/dependency patches; the source commit alone is not the full build recipe. [Symbol instructions](windows-symbols.md).

## Next questions and reproducing results

Useful follow-ups are controlled fresh-process repeats, new tracks/games, profile admission when archives exceed decoded RAM, mixed size/cost/frequency ranking, long sessions near storage/history limits and attribution of the remaining initial JIT gap. JIT preparation has not been implemented; diagnostic read fingerprints are not a safe replay-validation scheme. More CPU threads alone do not make arbitrary guest compilation or texture prediction safe.

To contribute, report hardware/OS/driver, game and version, exact build/EXE identity, graphics settings, cache budget and preparation report. State whether it is the first race/fight after a fresh launch, a same-session repeat, or a new disk-cache state. Keep the event and settings comparable, confirm continuous gameplay and separate interrupted captures. Reproduce both improvements and regressions. Submit numeric summaries and reviewed traces where appropriate, with game-derived data and personal information removed. No game files, saves, learned texture payloads or credentials are published in this report.
