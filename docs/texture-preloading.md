# Experimental learned PVRTC texture preparation

Opt-in diagnostic experiment on top of the 512 MiB CPU decoded-texture cache,
16,384-entry bound, two-way decoder and lightweight telemetry. No CPU JIT changes.
Default is off. Set `VITA3K_DIAG_PVRTC_PROFILE_MODE` before starting the emulator:

* `learn`: retain normal demand decoding; on normal app shutdown save a ranked
  snapshot of compressed inputs still retained in RAM. No profile file IO during
  the race. Close normally after the learning race; crashes do not save a pack.
* `preload`: validate the learned pack and decode selected inputs synchronously
  before `run_app` starts guest execution. Each existing decoder helper joins
  before return; no speculative work remains running during gameplay.
* `off` (or unset): ordinary demand cache. Does not read or overwrite a pack.

The learning run must use this version. Packs live under the emulator cache root
`decoded-textures/<title>/learned.pvrp`. They contain game-derived compressed
texture data: keep them private and local, outside source/releases/issues.
A pack covers retained content from the learned session, not all possible tracks,
weapons or game assets. Start with the event being compared. Ranking favors
expensive/reused entries; cache pressure can omit earlier content. Learning again
atomically replaces the previous pack; Preload and Off preserve it.

Bounds: 64 MiB entire pack, 16,384 entries, <=32 MiB compressed+decoded bytes per
speculative job, five-second soft preparation deadline checked between jobs.
One job/file read may overrun that deadline. The bounded whole input container
(up to64 MiB) and one job's scratch (up to32 MiB) are additional to the UI's
resident cache payload budget; index/allocator overhead is additional too.
Preparation cannot evict existing entries. It stops at cache/deadline limits.
Demand requests still compare exact compressed bytes after content/layout lookup,
so changed source content misses normally. Preload uses the same decoder and
never adds to demand request/decode counters.

The versioned little-endian file binds title/update/executable identity and
validates the complete file, lengths, layouts, per-entry hashes and final checksum
before decoding. Unsupported/corrupt/missing/oversize profiles fall back to demand
decoding. Checksums detect corruption, not authenticity. Title/session teardown
joins guest/render threads before learning and clears feature-owned cache state;
no background insertion, pointer persistence, guest instruction execution or save
format change is involved.

`VITA3K_DIAG_PVRTC_PRELOAD_REPORT` optionally names a per-launch JSON status file
(parent directory must exist). It reports prepared entries/resident bytes/time,
profile identity hash and learning result; log messages also state readiness.
Diagnostic status output is separate from demand telemetry. A pack read/write or
allocation error falls back to demand decoding; an older completed pack survives
a failed save. The first prototype may hold app launch for bounded preparation;
no progress UI or per-game settings change is included yet.

Validation: synthetic PVRTC I/II, 2/4bpp, cropped/rectangular inputs round-trip
through learning and preloading with byte-identical reference output. Tests cover
source changes/collisions, malformed/re-sealed metadata, bounds, lifecycle,
disabled/off modes, no-eviction admission and atomic replacement. No game assets
are test fixtures. Runtime performance benefit is unproven until measurement.

Compare fresh processes of the same exact EXE/PDB, same learned pack and 512 MiB,
Off versus Preload, preserving disk shader caches and matching event/class/laps.
Record preparation time/status, first22s and full90s presentation gaps and demand
telemetry. Repeat to control traffic/randomness and OS disk warming. Fresh starts
also reset JIT/other state; texture preparation cannot eliminate those costs.
