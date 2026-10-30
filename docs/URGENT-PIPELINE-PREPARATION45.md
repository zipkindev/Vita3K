# Required-path memory preparation and urgent pipeline scheduling (.45)

Based on `.44` (`4f19e476dff73f56ea966e250f68bcd05210384e`). Its Sebenco trace showed synchronous `PrefetchVirtualMemory` calls on the render-thread decoded-cache-hit path: about 17 ms in one frame gap and 43 ms in another. Three slow first-use pipelines were absent from the frozen 214-recipe input; a pair announcement only 6.3 ms before a 169 ms compile cannot hide that driver cost.

## Targeted changes

Remove OS page-prefetch and working-set inspection from the required cached-upload path. Loading still decodes/prepares learned pixels normally. Demand uploads retain source validation, staging preparation, post-wait revalidation and the direct cached consumer. No background pixel owner, detached pointer, extra decoded-byte cache, locked page allocation or cache-budget increase is introduced. A future proactive residency queue requires separate ownership, cancellation and utility evidence; moving the same synchronous hint immediately before a draw would not hide its cost.

Split actual-pair queue entries into fresh work and recovery work. Order is learned exact replay, fresh actual pairs (including queued hint promotion), actual-pair retries, then hints. Retries reserve eight pending/running slots for new work and learned replay. The shared four-token/40-per-second attempt budget reserves one existing token for fresh actual work; background requests cannot consume it. Refills, cooldown and compile-admission rates do not increase. FIFO ordering remains within tiers; already-running compiles are not preempted. Parked work continues honoring cooldown and metadata eligibility. A fresh actual pair still cannot synthesize shader/state information that has not arrived.

## Validation

Queue tests put fresh actual work behind a 48-request retry backlog and verify it runs first while learned replay remains first overall. Work-budget tests prove background wakeup storms cannot consume the urgent reserve or mint extra tokens. Existing ownership, cooldown, bounded-history, exact-recipe and decoded-cache tests remain relevant. Native Windows CTest and exact EXE/PDB identity verification are required before staging.

For a controlled dynamic comparison, restore the same original qa41-sebenco-20261028 frozen inputs as `.44`, at 512 MiB, async/validation off. Preserve `.44`'s newly learned 257-recipe output separately; do not silently blend it into that comparison. A later replay test may explicitly use the new profile. No performance gain is claimed until measured. First-use synchronous driver stalls can remain, especially when announcement-to-draw lead is shorter than compilation.
