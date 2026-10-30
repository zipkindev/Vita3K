# Progress-driven pipeline recovery (.46 candidate)

Based on `.45`; async pipeline compilation remains optional and is OFF for the comparison. The decoded-texture cache stays at 512 MiB. No additional decoder threads, speculative workers, prediction slots or compile-rate budget are introduced.

Changes:

- Filter repeated actual binding-pair notifications until bindings or preparation knowledge change. Aggregate notification counts instead of timing every HLE pair switch. At the bounded binding-cache limit, registration retains its existing fallback.
- Retain per-stage progress beyond full-template eviction. Losing an old template does not reopen completed work. A returning template still announces availability, allowing a deferred pair that never tried it to recover.
- Let genuinely newer relevant state/shader readiness reopen work immediately under the existing shared admission budget. Keep bounded backoff for unchanged transient pressure; lower generations from registry eviction are not progress.
- Remember up to 2,048 successful speculative recipes independently of their live Vulkan objects. Suppress recreating the same prediction after reclamation. Complete identities distinguish fixed state/variant differences. Required draws and loaded exact recipes retain their normal reuse/compile paths; failure does not create a success record.
- Stop waking workers for unchanged observed states and unchanged parked binding notifications. Periodic bounded recovery, knowledge publication, hint promotion and saturated-queue capacity recovery remain active.

`[PIPEPROGRESS]` reports actual-pair calls, unchanged suppression, completed-candidate count/skips/evictions and progress generation once at cleanup. Pair outcome bit `16384` means an already completed prediction. Existing bounded work/detail reports remain available; `early_pair` per-call counts are intentionally no longer comparable with .45.

Limits and tradeoffs:

- A reclaimed prediction is not a retained ready pipeline. Required use can still recreate it through the normal cache path. This avoids repeated unused speculation; it does not promise every later draw is already prepared.
- Returning template availability is deliberately not permanently suppressed. Completed recipe memory is bounded and can eventually evict old decisions; it does not disable a game after a lifetime attempt cap.
- Missing full recipes and genuinely new 200 ms driver compiles are not eliminated by this change. Independent native preparation of complete learned descriptions is separate work.
- Metadata remains bounded independently of decoded texture pixels: at most 8,192 stage progress entries and 2,048 full completed-recipe identities. The 64-template history and 16 live prediction-object limit remain unchanged.

Validation: event-oriented policy tests cover unchanged pressure, useful knowledge during cooldown, template eviction/return, recreated bindings, saturation, exact-identity distinctions and bounded completed-recipe eviction. A fresh-process performance comparison must use the same frozen learned bundle, graphics and validation/async lane; loading and results intervals require separate qualification. No performance improvement is claimed until measured.
