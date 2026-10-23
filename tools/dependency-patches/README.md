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
