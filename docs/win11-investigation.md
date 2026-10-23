# Windows diagnostic integration, 2026-10-23

This is an independent diagnostic fork, not an official Vita3K release. No performance fix is established yet. Mainline, Vita3K-Plus and Vita3K-nx retain their original authorship and licensing.

## Pinned source history

| Source | Full commit | Mainline-only / fork-only commits |
| --- | --- | --- |
| Vita3K master | 5844e21fb6d7259a31ba1f13b98ed1a648170052 | 0 / 0 |
| Vita3K-Plus master | 28dd8fb231c6a90dc810b41d608a9b422fc7a60c | 162 / 35 |
| Vita3K-Plus all-enhancements | a86b5f93db43e4c4640ad1527e2a4695aad0f35b | 162 / 213 |
| Vita3K-nx master | db030b5ab121b37d3e1c12fd78562b60c5ef86ff | 125 / 236 |

Counts are reachable commits from `git rev-list --left-right --count mainline...fork`, not a count of independent features. Plus's scheduler source lives on all-enhancements. Its merge base with mainline is 5b040dc4a6373ccecfaeeaaf817ba0edb8241edd. The nx merge base is 257464af2fea1e3497d6df75ca3d3bb276e909a8.

A non-mutating `git merge-tree --write-tree` preview of pinned Plus all-enhancements and mainline reports 71 conflicting paths, including kernel scheduling, synchronization, Vulkan and UI code. This branch starts from pinned Plus; it does not yet contain a resolved mainline merge. Keep the baseline for controlled comparisons. A wholesale combination is not evidence of better behavior.

## Third fork to track

[Vita3K-nx](https://github.com/NaGaa95/Vita3K-nx) targets Switch, but also modifies shared emulator code. Candidate revisions for review:

- a0505b91d74f8461ee7c2040ded8485fb5aa6985: accurate thread scheduling. The implementation also uses guest scheduler gates, so compare its ancestry and behavior with Plus rather than treating it as an independent scheduler-free control.
- 9919d3fd384c8a7f2438245b80ff965f23cb41ab: reacquire a condition variable's mutex on timeout. Inspected Plus already has equivalent untimed reacquisition plus its own scheduler integration; do not apply a duplicate patch.
- 1881b9adb608bbb7e532c2d66e3a12aa0e97ae9e: shader cache keys and filesystem lookup changes.
- 9a6b470084bf9891da1c4283101c726525cd6042: submission of command buffers in split Vulkan scenes.

These are review candidates, not confirmed explanations for WipEout pauses. Switch-only platform, Mesa and ARM64 tuning cannot be assumed to improve Windows/AMD Vulkan behavior.

## Diagnostic build identity

Mainline's vita3k/CMakeLists.txt generates the numeric version, Git commit count, short hash and fork/branch suffix. config/src/version.cpp.in uses those in the title and exposes official-update eligibility. Use an explicit diagnostic version such as v1.3.3-win11diag.1 for the separately sourced Plus build, and explicitly disable official update eligibility.

Record full source and submodule commits, all patches (including submodule contents), recipe commit, toolchain and SDK, optimization/debug flags, and EXE/PDB SHA256. Verify PE RSDS versus PDB GUID and age and resolve representative native functions before recording a race. A matching short version string alone does not establish binary provenance.

## Build reproducibility finding

Pinned all-enhancements compilation fails because logging.cpp includes absent util/fork_build.h, and codec/src/atrac9.cpp calls Atrac9DecodeBounded although pinned LibAtrac9 (82767fe38823c32536726ea798f392b0b49e66b9) does not declare or implement it. This matches [Plus issue 99](https://github.com/nckstwrt/Vita3K-Plus/issues/99). A separately reconstructed bounded decoder must be labeled as such; it is not proof of the unpublished release's implementation.

## Current runtime evidence

A retained mainline Windows package repeatedly fails loading WipEout 2048 Chenghou Project on an AMD Z1 Extreme handheld, while the installed Plus package completes that event. Mainline attempts varied ExternalHost/DoubleBuffer, async compilation and an isolated fresh shader cache. All four failures report amdvlk64.dll 32.0.23044.19, c0000005 at offset 0x2bd6d34. Menus, intro video, graphics and controls work before loading. The driver fault location does not establish root cause. No valid mainline race performance comparison exists.

The separate Plus pause investigation previously found render/display threads waiting while another native emulator worker remained busy. Its functions were unresolved, so a fresh optimized executable and matching PDB are required before attributing the pauses to a scheduler, shader compiler or driver.

Production installation, saves, graphics, controls, caches and launcher mapping remain separate from diagnostic experiments. Public reports exclude saves, licenses, private access details and unreviewed traces.
