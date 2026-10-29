# Pipeline retry work and useful prediction learning (.44)

Based on `.43` (4eb5120167ba0046e31a1c7f4949a2880549dfd8). The Sebenco session exposed 90,887 requeues, 39,356 parked-owner evictions and over eight million unrelated-state rejections. Those are whole-session counters, not race-only CPU measurements. Frame tails did not establish a net improvement over `.42`.

## Changes

- Share a monotonic work budget across prediction workers: four initial attempts, then one token per 25 ms, without accumulated catch-up bursts. Independently inspect at most eight parked requests per 25 ms. Learned exact recipe replay retains priority and bypasses the speculative budget.
- Keep 2,048 compact retry decisions independently of the 64 heavyweight parked owners. Owner eviction no longer immediately forgets cooldown. Decisions expire by bounded LRU replacement; this is not an unbounded negative cache.
- Require the 250 ms–2 s cooldown even when knowledge changes. Reopen work for related observed state, relevant shader-stage readiness, or temporary admission pressure. Unrelated shader/state activity does not reopen every pair. Related state templates remain bounded to 64 total/eight per pair; already-drawn key memory remains bounded to 4,096.
- Avoid repeated registry scans for an unchanged live binding epoch. A full epoch cache permits registration for subsequent levels. Readiness metadata is bounded to 2,048 stage identities and replaces the oldest revision when full.
- Reclaim an unused prepared pipeline only after a valid replacement candidate has passed state/shader checks. Eligibility scanning alone no longer destroys a prepared object. Existing 16 prediction slots/four hint slots, admission rate and exact ownership rules remain active.
- Save an expensive predicted recipe only after a draw successfully takes its exact prepared object. The threshold is 2 ms measured preparation elapsed time; it is not exclusive driver CPU time. Unused/failed guesses and cheap predictions are not persisted. The existing checksummed 1,024-recipe format and exact state identity remain unchanged. Saving is independent of diagnostic telemetry.
- Parked-owner recency uses an always-on monotonic timestamp, independent of profiling being enabled.

## Validation and limits

Focused C++ tests cover wakeup storms, elapsed-time budgets, bounded metadata replacement, cooldown after owner recreation, relevant versus unrelated knowledge, recipe round trips and exclusion of unused predictions. Native Windows build/test and separate menu correctness checks must pass before staging for performance comparison.

Comparison uses the same frozen qa41-sebenco-20261028 input, 512 MiB decoded texture cache, Vulkan 2×, async compilation off and validation off. Correctness checks use a separate validation-on process, followed by restoration of the frozen inputs. No measured `.44` performance improvement is claimed before a driving capture. New effect pipelines can still require synchronous compilation on first use; this change bounds wasted preparation and learns successful predictions for later launches, rather than skipping required draws.
