# State-aware pipeline preparation candidate (.41)

The .40 four-track WipEout 2048 tournament contained a Sebenco burst with 169–254 ms synchronous Vulkan pipeline creations. A worker simultaneously compiled generic pair variants. Pipeline telemetry fingerprints are session-local XOR identities: after translating them, all four effect pipelines were present in the newly saved profile and absent from the previous profile. They were first-use states, rather than lost learned entries.

This candidate replaces the generic zero-filled pair warm-up with bounded prediction from actual draw states. Eight recent complete states retain culling, depth/stencil, primitive, render-pass compatibility and texture hints. Matching fragment/vertex identities rank ahead of recency. Prediction uses only an already observed shader pair; it does not create a cross-product of programs.

At most one eligible state is compiled per pair request, with 128 dynamic jobs per session and the existing 256 live prepared-object ceiling. Cached shader availability or hint-independent code generation remains required. If drawing has already claimed a pair, speculative work is abandoned. Learned exact recipes keep queue priority. Prepared objects use the existing full-recipe ownership and reuse checks; a guessed state cannot replace a draw with a different recipe. Speculative recipes are not written into the learned profile.

Async draw skipping remains off in the comparison. Graphics pipeline libraries remain disabled. Texture budget stays 512 MiB, and .40 remains the rollback candidate. This does not guarantee every previously unseen effect can be prepared before its first draw, nor establish a performance improvement before measurement.

Validation: state-history tests cover hint/fixed-state preservation, stage preference, interlock separation, recency replacement and bounded behavior over 1,000 transitions. Native optimized build, full suite, matched EXE/PDB verification and short Go correctness checks are required before a targeted Sebenco Rapier driving comparison. Vulkan validation timings are excluded from performance results.
