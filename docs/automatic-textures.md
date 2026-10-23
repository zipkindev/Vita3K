# Automatic texture preparation (experimental)

Enable **Automatic Texture Preparation** under Graphics alongside a nonzero
**Decoded Texture Cache** budget. The option can be set globally or per game;
legacy profiles inherit the global value. Changes apply on the next game launch.
Default is disabled, preserving existing behavior. No manual Learn/Preload switching
is needed when using the saved setting.

On the first launch, normal PVRTC demand decoding fills the RAM cache. Normal game
shutdown saves a bounded, ranked snapshot of retained compressed inputs. On later
launches a compatible profile is validated and prepared before guest execution,
then normal gameplay updates the working set. Normal shutdown replaces the profile
with the latest retained content. Missing/corrupt/incompatible profiles fall back
to normal decoding and can be relearned. A crash does not save a new profile.

The RAM cache uses the selected budget (up to512MiB) and16384entries. Saved profiles
are a separate disk cache: **64MiB per game,1GiB total**. Least recently used saved
profiles are removed as space is needed, with the current game's profile protected
when possible. An OS lock serializes writers/pruning/clearing across processes;
a busy/unavailable store safely falls back to normal demand decoding. Owned orphan
temporary files are cleaned while holding the lock. Atomic replacement needs up
to64MiB temporary headroom above the1GiB steady-state limit. Unknown files and
symlinks are never followed/deleted by storage management; these limits apply to
this feature's managed profiles, not arbitrary files manually put in the directory.

**Clear This Game's Texture Profile** and **Clear All Saved Texture Profiles** are
available while no game is active. They remove only managed saved texture profiles,
with an explicit confirmation. They do not remove saves, game files or shader/
pipeline caches. A future Automatic session can learn the removed content again.

Preparation retains .5's five-second soft deadline checked between jobs, bounded
64MiB input file and32MiB per-job compressed+decoded scratch, additional to the RAM
payload budget and allocator/index overhead. Decoder work uses at most one helper
plus the caller; no speculative work remains running after preparation. Profiles
contain game-derived compressed bytes: keep them local/private. No GPU uploads,
JIT warming, race detection or runtime prediction is added in this version.

Diagnostic VITA3K_DIAG_PVRTC_PROFILE_MODE overrides the UI setting when explicitly
set to off/learn/preload/auto. Normal launchers must clear this override to test the
UI. Optional per-launch report includes mode/status, entry counts and preparation
time; normal decode telemetry keeps demand and preparation counts separate.

This shared renderer path covers eligible PVRTC I/II2/4bpp textures, not every
texture format or bottleneck. Legion Go on Windows is the validated hardware;
other handhelds/platforms are potential targets requiring their own validation.
Current game-derived profiles can be imported from the previous isolated .5 copy
for continuity, but keep .5's frozen comparison pack separately: Automatic mode
intentionally updates its own pack after each normal shutdown.
