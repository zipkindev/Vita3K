# Curated integration candidate 1

Experimental proposal, not a validated replacement for the tested `.2` reference or `.4` UI trial. This combines the tested Plus-derived patch line and two narrow imports; it is not a complete merge of all upstream histories.

Base: `53abf6cf0` (Plus all-enhancements, corrected ATRAC9 span, opt-in decoded texture cache, global/per-game cache UI and tests).

| Import | Origin | Adaptation |
|---|---|---|
| Stored NGS output patch validity | Vita3K/Vita3K `e9225f90f04cdf0d6d7c6fe617c0c70a5a0af84a` | Reject invalid non-null handles before dereference; retain Plus route behavior. |
| Failed/truncated Vulkan pipeline-cache reads | NaGaa95/Vita3K-nx `1881b9adb608bbb7e532c2d66e3a12aa0e97ae9e` | Shared read/bounds checks only; retain Plus GPU/driver header checks. |

The nx shader-key change requires additional native-color variant plumbing absent in this Plus tree. It is deferred; forwarding the current hash alone would not complete that fix. The mainline scheduler/sync-object series and RGBA4444 fallback also remain separate investigations.

Observed sources on 2026-10-23: mainline advanced from `5844e21fb` to `c4c90df7c` (ffmpeg-core submodule update); Plus and nx pins in the maintenance proposal are unchanged. No ffmpeg dependency change is included here.

## Validation before promotion

Run the optimized Windows package workflow on the dedicated server, retain the full source/support-patch manifest and exact EXE/PDB pair, and require all CTests. Compile success is not gameplay validation. Dedicated invalid-handle guest tests and malformed-cache integration fixtures remain outstanding.

Stage in a new portable directory only after normal closure, with a new consistent save/config/cache copy. Test three Chenghou loads, music throughout races, Ubermall, controls, save/reload and repeated active race pacing. Compare to `.4` at the same cache budget without changing graphics, scheduler, affinity or priority. Cold shader-cache tests use an expendable copy; preserve the owner's working caches. Do not advance the tested release on CI results alone.

## Continued maintenance

Use primary mainline as the long-term reference, Plus for selected compatibility/scheduler work, and nx for applicable shared-code fixes. The companion automation proposal watches metadata, then builds only explicitly reviewed candidate SHAs. It never blindly combines fork heads or promotes/deploys a release. Preserve per-import authorship and tests; retire a local patch only when an upstream equivalent passes those tests.
