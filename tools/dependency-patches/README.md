# Source-pinned diagnostic dependency patch

manifest.json pins the clean Vita3K/dynarmic base and SHA256 of the observer
patch. The Windows runner recipe verifies and applies it before configuration
and records the full dependency diff in build-evidence/dynarmic.patch. A source
parent diff alone does not contain submodule edits.

For other builds, initialize the pinned submodules and apply the checked patch
to external/dynarmic before configuration. Never update the submodule gitlink
to a different base or silently build without the observer patch. The observer
is default-off and implemented only in the x64 A32 backend. Original Dynarmic
license and notices are retained.

The .20 candidate extends this cumulative patch with an x64 A32 compile-only
PrepareBlock API and a default-deny pre-emission validation callback. It never
runs generated code, requires a quiescent JIT, preserves pending invalidations,
and skips preparation rather than flushing for low code-cache headroom.
Normal compilation and profiling keep their existing paths. See the .20 release
notes for exact read validation and the experimental preparation limits.
