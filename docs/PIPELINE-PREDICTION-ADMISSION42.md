# Pipeline prediction admission candidate (.42)

The .41 Sebenco Phantom recording consumed all 128 lifetime prediction jobs before recording began. Seven predictions were reused; three later slow states were never predicted. A fixed lifetime allowance cannot adapt across later effects or maps.

This candidate removes speculative combinations reconstructed from historical pair profiles. Exact saved recipes still prepare normally. Prediction accepts direct program-creation hints and actual game-bound vertex/fragment pairs, using recent observed full state only when at least one shader identity matches. Full-recipe matching, shader guards and correct synchronous drawing remain required.

Admission has two rate limits: actual bindings receive an eight-job burst and four tokens per second; creation hints receive a two-job burst and one token per two seconds. At most 16 predicted objects are live, including at most four creation hints. Hint activity cannot consume the actual-binding token budget. These limits bound prediction work rather than ending it permanently after 128 jobs. Existing attempt and identity bounds still apply.

Queue priority is learned exact recipes, actual bindings, then creation hints. An actual-binding announcement promotes a queued hint without duplicating ownership. At most one predicted state is compiled per request. Known required render-thread work prevents starting speculative compilation. Two workers remain the ceiling; active rendering retains the single-preparation-worker policy.

Reuse, draw claims and compile failures release predicted-object capacity. When needed for a new request, the oldest ready predicted object unused for at least two seconds can be reclaimed. Running jobs, learned exact objects and predictions awaited by a draw are excluded. Draw waits retain shared ownership even if a failed entry is pruned. Vulkan destruction runs on preparation workers outside the object mutex, not on guest threads. The driver's opaque cache entry remains after an unused object is reclaimed.

No texture-budget or graphics change is included. Async draw skipping stays off, graphics pipeline libraries remain disabled, and the test budget stays 512 MiB. A newly encountered state can still require synchronous compilation if insufficient preparation lead time, matching state or budget is available. Performance benefit remains subject to measurement.

Tests cover token isolation/refill, reserved capacity, bounded live objects over 10,000 admissions, non-refundable rate tokens, clock regression, safe reclamation conditions, queue ordering and hint promotion. Native optimized build, matching full PDB, full tests and an isolated Go loading check precede a short comparison. Use the same frozen pre-.41 tournament checkpoint for a strict comparison; separately preserve .41's newly learned profile rather than silently incorporating its new states into only one build.
