# Repository consolidation: .46 development baseline

The 2026-10-29 consolidation brings the tested .46 development line into `master`,
while retaining the existing self-hosted Windows release runner, storage safeguards,
workflow definitions and public documentation. It does not promote .46 to stable,
rebuild its published binaries or import divergent experiments.

## Resolution and verification

The former master and Plus-derived candidate had divergent emulator histories.
Automatic code merging would combine untested runtime changes. The consolidation
therefore takes runtime source, build configuration and submodule references exactly
from candidate commit `5848a17c64f44749d8d4e15dda8ad63bc26e640a` (documentation follows
built source `f5fdfb0b96db383e355e3a2a99ac64d5dfb8fd85`). The previous master is
`09e284150e`; its exact full identity is retained in the merge parent.

Existing `.github/` and `tools/windows-build/` files are preserved byte-for-byte.
Historical docs missing from the candidate are retained, current candidate menu-path
corrections are retained, and prior release .6 spacing is retained. Both patch-file
attribute rules and useful ignore entries remain. Master-only auxiliary CI/autotest
scripts remain available. No new upstream emulator changes are selected here.

Checks: runtime/build/submodule index equality with .46; workflow/runner equality
with previous master; no unresolved merge entries; 17 runner retention/dependency
patch tests passed. The already tested .46 binary passed 38 native Windows tests;
no new binary performance result or additional gameplay validation is implied.

## Superseded continuation reviews

These head commits are ancestors of the selected .46 candidate and will be retained
in master history by the merge. Their reviews can be closed as superseded and their
remote branches removed after verifying inclusion. Release tags remain unchanged.

| PR | Branch | Head before cleanup |
|---|---|---|
| #1 | `experiment/pvrtc-decoded-cache` | `182271a18180285b131aab980d07d3330afebe58` |
| #2 | `feature/decoded-texture-cache-ui` | `53abf6cf0e8e4c4f5749c8cd46ae3302c7988b1c` |
| #3 | `candidate/curated-upstream-1` | `5c87451a1681ac1abc1e07a22fd761ea5537f6fe` |
| #5 | `candidate/parallel-pvrtc-1` | `1ff11ff2a7278f8d118d0d5e0683f20bc8fad5d8` |
| #6 | `candidate/decoded-cache-entries-1` | `da8c5342b14b15152e2079d1ce79905fe4ab7e0d` |
| #7 | `candidate/cache-telemetry-1` | `d1770a9ae662f7148d4910304f7c1a141446688a` |
| #8 | `candidate/texture-preload-1` | `b23d14eb789371efb4645fd94f51493c01d454bb` |
| #10 | `candidate/automatic-textures-1` | `507e0d78a65031c229d8d3d5ab793b6363efc180` |
| #15 | `candidate/jit-profile-1` | `20961a66400cde078dcc4f5855d0fb47a920f7ca` |
| #16 | `candidate/jit-profile-rolling-1` | `ebc2589a82f303cfb07bca3f3b579ad18b738fd5` |
| #17 | `candidate/sync-waits-ui-1` | `3553cbf2552a8f7aa8bea5c109ce65083ca42e6e` |
| #18 | `candidate/texture-reuse-1` | `215f584c9be5eff44160671a741238e82ed0c213` |
| #20 | `candidate/texture-learning-history-1` | `5a88690e5b1584f5b1e2be452f3f9a29cdcd869a` |
| #21 | `candidate/cache-diagnostics-ui-1` | `7a296b309a1c87ee61faab066a40940ca7c924d9` |
| #23 | `candidate/compact-texture-settings-1` | `126a994fc6551a457110099becdbe7de6a12f636` |
| #27 | `candidate/pipeline-telemetry-19` | `03889bf951332c6c02722aaa7b17d27d01135b17` |
| #28 | `candidate/jit-preparation-20` | `7a6ac1cd883b773680af3917b92f2fb863bf39d0` |
| #29 | `candidate/texture-coverage-21` | `cf0565299ecee4ab9bb45bf15eb021de4ec6c367` |
| #30 | `candidate/decoded-retention-22` | `be89c46c39dfeb14007f5e4c6f787474adc29ce2` |
| #31 | `candidate/jit-startup-23` | `266784d985d10e3f307bba9f8b46617149ec1d8e` |
| #32 | `candidate/texture-work-24` | `63321659f77cefe9f66be16d9a3b489444b47296` |
| #33 | `candidate/retention-events-25` | `e029382f8079db4fe41450d79d949963e9e3bb36` |
| #34 | `candidate/preload-retention-26` | `f0cd056549403bd21490d7c71bedcbfdf36b47c4` |
| #36 | `candidate/admission-retention-27` | `0c181b9e4badd538919809a83b3da0816e6380c4` |
| #37 | `candidate/pvrtc-vulkan-28` | `fa2ec56b0e92ea330eeaf60070ba225bd7379b11` |
| #38 | `candidate/pipeline-prewarm-29` | `eb313adbecc92b6487f5661fc7f4a67ff65d0155` |
| #39 | `candidate/exact-pipeline-30` | `5714d6d7c456fc4813a8b89ac15ee43c53675a84` |
| #40 | `candidate/dynamic-pipeline-31` | `7ff6b128a1814e2157333cf7170537c81fa866c4` |
| #41 | `candidate/fast-linking-32` | `e157850acf42ef45d97d9ef9dfad58181ea110b8` |
| #42 | `candidate/recent-demand-preparation-37` | `76b90476adce69db3a248fd4eb7ad040f680d116` |
| #43 | `candidate/vulkan-queue-sync-38` | `c12e5b48a2eca44a8c0c93813b078dc825cb90f8` |
| #44 | `candidate/progress-driven-pipeline-recovery-46` | `5848a17c64f44749d8d4e15dda8ad63bc26e640a` |

## Divergent reviews retained

These branches are not ancestors of .46. Their PRs and branch refs stay available;
no new adoption or rejection of their changes is made by consolidation.

| PR | Branch |
|---|---|
| #4 | `proposal/upstream-candidate-automation` |
| #11 | `candidate/reviewed-upstream-1` |
| #19 | `candidate/preload-retention-1` |
| #24 | `candidate/upstream-correctness-16` |
| #25 | `candidate/allocator-17` |
| #26 | `candidate/invalid-code-18` |

Other remote branches without superseded open PRs are outside this cleanup. Local
working copies and dirty dependency checkouts are untouched. Future features should
branch from master and return through focused PRs. Existing test/release tags remain
the authoritative historical rollback points.
