# Texture cache copy and upload diagnostics

Enable **Texture Cache and Upload Telemetry** in Texture Decoding, then launch a
game. The existing texture diagnostic switch now also arms a bounded CPU-work
observer before preparation. Close normally to save reports beneath
`cache/diagnostics/texture-work/`. Existing coverage/reuse reports stay separate.
This is observation: cache policy, byte validation, decoder concurrency, learned
profiles and Vulkan submission/fence behavior are unchanged.

The cache observer separates immutable compressed-input snapshot/hash work, cache
lookup total, cache mutex acquisition, lookup/byte validation, decoded pixel copy,
bookkeeping, decoding and insertion. The validation phase includes candidate
lookup and all byte comparisons. Pixel-copy events occur only on hits. The cache
records clock marks under its existing mutex, then appends diagnostic records
after releasing it. The original decoder cost measurement excludes observer
publication and continues to drive existing retention/learning decisions.

The upload observer covers the CPU texture upload path (all its mip/face work),
Vulkan backend upload, staging preparation, explicit fence waits, staging-buffer
allocation, mapped-buffer copying and command recording. Preparation can occur
outside backend upload during texture configuration. Fence wait result codes are
recorded; no fence is added and no extra GPU submission/query is made. CPU copy,
queue submission and driver elapsed time are **not GPU execution timings**.

Reports include host thread ID, QPC endpoints, dimensions, format, mip/face and
stage-specific byte sizes. Cache-format values encode two-bit/type flags;
upload-format values use the GXM base format, so these are different namespaces.
Cache stages use compressed input and decoded output sizes (requested sizes on
misses); pixel-copy/staging-copy bytes describe the copy length. Upload total
has unknown byte count zero; Vulkan backend/copy/commands record upload length.
Staging allocation records previous and requested buffer capacities; preparation
records required capacity. These sizes are not additional resident-memory totals.
No pointers, image handles, texture hashes, pixels or guest addresses are saved.

`work.csv` retains spans taking at least 2 ms in a 65,536-record ring (80 bytes per
record, 5 MiB). `session.csv` retains summaries **per kind**, including short work,
in up to 4,096 adaptive time buckets. `kinds.csv` contains session totals and byte
counts. All storage is allocated before recording; records and buckets together
use about 9.72 MiB plus small object/header overhead. Summaries count accepted
spans; the recorder uses a try-lock and reports contention drops, overwritten
detail and in-flight work at stop. File I/O happens at arming and normal close.

Cache flags distinguish hits/misses; decode bypasses carry flag4. Staging waits
use flag1 for a current-scene fence and flag2 for a prior-scene fence. Result0
usually means normal return; insertion result1 means admission skipped, not a
rendering failure. Early returns/exceptions can have unknown results. The
observer does not validate GPU output. Enabled cache copies add several clock
reads; normal operations still incur the disabled observer branch when off.

Nested stages overlap. Do not sum total upload, decoding, cache total and its
subphases as independent work. Byte counts across stages likewise repeat the same
data. Bucket time is completion-assigned elapsed time, not CPU utilization or an
exact frame critical path. Use retained QPC spans and the exact EXE/PDB alongside
frame/CPU traces; account for detail thresholds, dropped records and initial
loading/countdown. Keep the same RAM budget and comparison profiles when testing.

Validation includes bounded rings, per-kind/byte summary compaction, concurrent
writers and shutdown, cache collision/miss behavior, identical copied pixels and
720 real-decoder comparisons per mode, including an observer-enabled mode whose
cache/decode counts agree with existing accounting. GPU upload behavior still
requires gameplay testing on the target device.
