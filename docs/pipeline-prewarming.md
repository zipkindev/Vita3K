# Protected-draw pipeline preparation candidate (.29)

Vulkan async compilation deliberately protects draws that cannot safely be skipped. Those draws can still stall when the required pipeline is unavailable. Preparation must improve coverage without removing that rendering safeguard.

This candidate registers vertex and fragment bindings independently, including shader-patcher cache hits. Fragment creation no longer needs an optional vertex-link hint to participate in learned-pair matching. The previous explicit hint path remains available. Cached SPIR-V variant lookup now uses the base program identity and variant suffix used when writing the file, rather than treating the derived in-memory key as a filename.

Actual protected draws teach a per-game set of vertex/fragment/output-register identities. On subsequent launches, matching registered bindings can schedule background preparation even when the game supplies no link hint. Only observed pairs are matched; arbitrary vertex/fragment combinations are not generated. The metadata file, `protected-pairs-vk19.bin`, lives beside that game's Vulkan shader cache. It contains no guest memory, shader bytecode or Vulkan handles. Deleting it resets this additional learning.

Storage is limited to 1,024 pairs (69,652 bytes including header), with format validation and checksum rejection of damaged files. Registration holds weak references to at most 2,048 bindings and eight variants per program identity. At most 64 requests are pending, with two attempts per request identity and 1,024 request identities per session. Failed or capacity-rejected requests do not permanently suppress a later registration retry. Attribute/stream and blend variants have distinct request identities.

Preparation still uses compatible cached shader variants and common render-surface states. It warms shader modules and the driver cache; it does **not** reconstruct or guarantee every exact draw-state pipeline. A first encounter may still compile synchronously, and incomplete stage registration, unsafe generation hints, queue limits or driver work can still prevent early completion. No protected-draw rule is relaxed and no reduction in latency is claimed before comparison testing.

The existing pipeline telemetry adds stage-registration, learned-pair, bounded-request, duplicate-request and missing-link-hint counters. Existing kind numbers remain unchanged. These counters and warm/protected timings distinguish absent coverage from late or incomplete preparation. Pipeline telemetry remains controlled by its existing diagnostic setting; the texture RAM budget is unchanged.

## Validation

The standalone persistence test checks round trips, output-register variants, duplicate suppression, capacity and rejection of truncated, corrupt, oversized or trailing data without destroying an existing learned set. Windows validation uses the optimized self-hosted build and full matching PDB. A short, matched-settings test should compare a fresh process before and after learning, with both preparation timings and protected-draw waits inspected. Background work can contend with rendering, so frame-time tails and graphical correctness must be checked rather than assuming that more queued work improves performance.
