# Texture reuse telemetry

Enable **Settings → Texture Decoding (Perf. Tweaks) → Texture Cache and Reuse Telemetry** before launching a game. This extends the existing saved `decoded-texture-telemetry` switch; it remains off by default and works globally or per game. Reports use `cache/diagnostics/texture-cache/` (or the existing explicit `VITA3K_DIAG_PVRTC_TELEMETRY` path). Close normally for a final sample. This instrumentation measures behavior; it does not change cache admission, preload ranking, eviction policy, budget or worker count.

Schema 2 retains the original 26 CSV columns and appends cumulative reuse counters. One snapshot per second, up to 14,401 rows / less than 16 MiB per writer. No per-request file IO, unbounded queue, texture contents, addresses or fingerprints are exported. Existing scripts consuming named original columns continue to work; consumers that validate the schema or column count must accept the new version deliberately. Old diagnostic files are not pruned automatically.

A fixed 65,536-slot, four-way set-associative history tracks fingerprints within the existing cache mutex. It allocates at activation, uses at most 4 MiB additional slot storage and performs at most four slot comparisons per event, without per-event allocation or another content-hash pass. Disabling telemetry frees the history. Multiple writers share a reference-counted history; a later activation begins a new history epoch. Aggregate counters are cumulative for the cache lifetime, just like the original counters. This adds CPU/lock work when enabled; no zero-overhead claim is made.

Identity consists of the existing 64-bit compressed-content hash, format, dimensions and compressed/decoded lengths. This is probabilistic attribution: a hash collision can affect telemetry, but actual cache hits still require byte-for-byte compressed-source equality and output-size validation. No diagnostic identity controls reuse of pixels. The history retains neither compressed nor decoded bytes. It can forget identities before filling globally because each set has four slots; `ReuseHistoryReplacements` explicitly counts replacements. `ReuseHistoryEntries`, `ReuseHistoryCapacity`, `ReuseEnabled` and `ReuseEpochs` expose scope/recall limits.

Miss categories (all eligible lookup misses partition exactly while enabled):

- `MissFirstObserved`: identity absent from current bounded history. This does **not** mean first-ever; it can follow history replacement or a new epoch.
- `MissAfterCapacityEviction`: the most recent tracked state is eviction for bytes, entry count or both. The first subsequent miss consumes that attribution.
- `MissAfterClear`: most recent tracked state is removal by explicit cache clear or a budget change.
- `MissSeenOther`: identity remains known but is not in an eviction/clear state; examples include another outstanding miss or unsuccessful admission.

Corresponding `Decode*` and `DecodeNs*` columns count demand decodes and elapsed decoding nanoseconds using the classification captured at lookup, rather than reclassifying after decoding. Elapsed time includes preemption and helper coordination; it is not summed CPU execution. Bypass/unclassified decode calls enter `UntrackedDecodes`. Startup preparation uses the existing direct decoder and is not part of demand-decode timing. A concurrent insertion can make a demand decode redundant; it remains a real decode with its original miss classification.

`PreloadedHits` counts cache hits whose most recent tracked insertion was preparation (`allow_eviction=false`); `DemandHits` counts other hits, including history rediscovery of resident entries. History replacement can lose preload origin, so this is bounded recall, not exhaustive coverage. These counters do not alter preparation behavior. Shutdown clear events must be excluded by bracketing the gameplay interval.

For comparison, keep 512 MiB, the same game/event/class, settings and caller-plus-helper mode; distinguish fresh-process and repeat races. Use QPC snapshot brackets with PresentMon and native stacks. A one-second counter interval cannot attribute every decode to one frame. Compare `DecodeNsAfterCapacityEviction` with `DecodeNsFirstObserved`, checking history replacements and untracked decodes, before changing preload selection or cache policy.
