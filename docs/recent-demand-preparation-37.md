# Recent-demand texture preparation (.37)

This candidate changes selection of learned PVRTC inputs at startup. It retains
.36's direct CPU-cache-to-staging copy and bounded Windows page hints. Vulkan
pipeline compilation and the runtime decoded-cache replacement policy are unchanged.

## Policy

Automatic Texture Preparation uses the existing per-game control and identity.
The preparation ceiling remains 75% of the decoded-cache budget (384 MiB with a
512 MiB cache), leaving 128 MiB for demand. The existing entry ceiling and
five-second startup deadline remain independent limits.

A deterministic planner first selects recent-demand records within two thirds of
the preparation ceiling. Up to one sixth of the ceiling is then available to
historically expensive inputs (at least 2 ms recorded decode cost), ranked by
absolute avoided cost. Remaining space favors learned companions of selected
records, then existing cost-per-byte ranking. Unused reservations are available
to later passes. Large records never prevent smaller later records fitting.

Recent demand is bounded at 64 per input. Actual CPU cache use or demand decoding
counts; importing a profile and preparing textures do not. After a session with
demand, the new score is min(64, previous / 2 + session demand). A no-demand
session preserves history. Each group of 32 distinct inputs first demanded in a
session shares a co-use hint. It is a coarse demand-order cohort, not a map,
weapon identifier, GPU visibility measurement, or proof of temporal overlap.
Cohort selection uses a snapshot of selected seeds; it cannot recursively expand.
No per-hit allocations or unbounded graph are added by the metadata.

## Storage and compatibility

Version 2 records add 16 bytes for recent-demand and co-use metadata. The existing
64 MiB per-game file limit, 16,384 entry limit, 32 MiB decode-job limit, aggregate
storage budget and clear-cache UI still apply. Packed header accounting increases
from 40 to 56 bytes. Runtime list/index overhead remains separately entry-bounded.
Version 1 packs are read without invented recent history. Normal close writes
version 2 atomically with the existing checksum and exact compressed-input checks.
Older builds do not read version 2: keep separate installations and frozen v1
profiles for comparisons and rollback. Profiles contain private game-derived data.

## Validation and limits

Tests cover v1 migration, no-demand preservation, demand aging, seed neutrality,
byte and entry bounds, costly effects alongside recent inputs, companion selection,
concurrent cache lifecycle, decoded pixel equivalence and malformed profiles.
No gameplay improvement is established for .37 yet. One learning session followed
by a fresh-process short combat run is needed. Unknown first-use assets cannot be
prepared from history. Pipeline compilation stalls remain a separate limitation;
this candidate neither adds asynchronous rendering nor omits protected draws.
