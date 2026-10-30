# Reviewed upstream changes — 25 October 2026

Reference: tested `.27` source `0c181b9e4badd538919809a83b3da0816e6380c4`. Mainline refreshed to `92bda548f3aa0691102471a72725587e00676383`; Plus all-enhancements remains `a86b5f93db43e4c4640ad1527e2a4695aad0f35b`, nx remains `db030b5ab121b37d3e1c12fd78562b60c5ef86ff`. No new Plus/nx commits were fetched compared with the previous review.

## All four new mainline commits since the previous mainline pin

| Commit | Scope and source review | Decision |
|---|---|---|
| [`ba189e234`](https://github.com/Vita3K/Vita3K/commit/ba189e234886b5d7c74ea615bfe6cc3a62cdf000) | Advance an empty NGS player slot to a populated next buffer. The Plus-derived player already uses bounded `first_buffer_with_data`, index validation, callbacks and an initial-buffer retry protocol. | No duplicate patch. Preserve the corrected audio path; no performance candidate from this diff. |
| [`eeea7918a`](https://github.com/Vita3K/Vita3K/commit/eeea7918a3f867911fdab21667a80fac28dfc37e) | Vulkan portability enumeration negotiation is conditional on `__APPLE__`; only added standard includes affect Windows compilation. | Defer from Windows performance builds. No Legion Go performance claim. |
| [`c4712764e`](https://github.com/Vita3K/Vita3K/commit/c4712764e3cf78852b5f6e2177866505db61a497) | Force software PVRTC decoding because the Vulkan texture mapping uses RGBA8 rather than native PVRTC formats. Existing capability detection could otherwise bypass the required conversion. | Isolated `.28` correctness candidate on `.27`, preserving cache/learning/telemetry. This is not a decoder algorithm or decoding-order speed optimization. On devices already reporting no PVRTC support, the effective path is unchanged; a Go speedup is not established. |
| [`92bda548f`](https://github.com/Vita3K/Vita3K/commit/92bda548f3aa0691102471a72725587e00676383) | Optional OpenGL mask-bit workaround, configuration persistence/restart plumbing and broad settings-tab relocation. Final upstream code uses an explicit setting, not automatic AMD detection. | Hold for a separate OpenGL compatibility task. It does not optimize Vulkan, and copying the UI relocation would need reconciliation with Texture Decoding. Keep mask behavior unchanged for current tests. |

## Rechecked fork performance candidates

- nx `5b5d17e40` unused shader register reads: the same highest-word selection, indexed-register handling and packed-width calculation are already present in the Plus-derived source. Do not reapply or bump shader-cache version just to claim an import.
- nx `0bb8672fc` Vulkan U2F10F10F10 conversion: already present (`!is_vulkan` guard; Vulkan conversion to F16 RGBA). No duplicate candidate.
- nx `c50c97ad7` main-thread JIT cache increase is `__SWITCH__`/Oaknut shared-pool-specific. It does not enlarge or optimize the Windows x64 cache by direct import.
- nx `db45d86f6` / `020262526` synchronous GL-query avoidance / alternating GL surface reuse affect OpenGL, not the current Vulkan workload. Defer.
- nx `1881b9adb` shader keys/filesystem lookup changes: failed/truncated pipeline-cache reads were already adapted. Directory indexing is Switch-only; full shader-key changes require native-color variant plumbing missing from this branch. Do not import only hash forwarding and claim equivalent behavior.
- nx `48f1e430c` broad shader/pipeline persistence work overlaps the Plus-derived worker/warm/save architecture. It is not a narrow drop-in fix for protected-draw stalls; requires a separate design and cache-lifecycle validation.
- nx `1b983f2fe` transfer-subrectangle synchronization and `5fba7c3ff` overlapping surface layout selection are correctness candidates requiring mapped-memory/surface-range prerequisites and focused tests. They are not established remedies for the measured decoded-cache/pipeline stalls; hold separately.
- Mainline kernel wait/callback/timer series remains dependent on the object-owned wait architecture documented in the earlier review. Directly copying the final commits would bypass Plus scheduler gates. `.18` first-page behavior remains a separate prior candidate, not part of `.27` or `.28`.

Commit-list divergence is not a count of missing fixes: Plus has adapted many nx/mainline changes under different commit identities. No claim is made that every historical divergent commit has received a fresh semantic audit.

## Test order

1. Keep `.27` as the published release candidate and reference. Repeat the same four-map Rapier tournament at 512 MiB using the newly learned multi-map profile. This distinguishes learning coverage from policy changes without a rebuild.
2. Test `.28` in its own installation, seeded from the same immutable learned profile and saves/settings snapshot as the `.27` comparison. Keep Vulkan 2×/AF4, Bilinear, VSync, shader cache, async pipelines, surface synchronization, mapping, decoder workers and guest scheduling unchanged.
3. Record fresh-process and warm measurements separately. Start recording before race start; use only click prompts. Preserve map loading/results in continuous capture and avoid presenting them as pure racing frame times. Check graphics as well as smoothness.

No new algorithmic retention adjustment is justified solely by a tournament with a changed map/profile workload. New-texture decoding and protected pipeline compilation/waits remain distinct follow-up investigations. Upstream imports are candidates until gameplay validation; they are not automatically part of the released `.27` binary.
