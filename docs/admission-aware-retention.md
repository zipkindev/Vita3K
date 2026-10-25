# Admission-aware decoded texture retention (.27 candidate)

This candidate uses the existing Automatic Texture Preparation and decoded-cache
controls. It keeps the total byte/entry caps and the density-ranked startup set:
at 512 MiB, preparation still fits within 384 MiB / 12,288 entries, leaving a
quarter for loading within the 512 MiB / 16,384-entry total. No new worker,
background prefetch, loading-screen detector, GPU upload or synchronization is added.

Demand insertion checks prospective victims before removing any resident pixels.
It examines at most 64 nodes in the same probation-first order used by eviction,
comparing the incoming measured decode cost with the bounded avoided work of
retained or actually reused victims. A failed check declines cache insertion;
the already decoded result still reaches the normal renderer. It never reuses
pixels without the existing compressed-byte and layout checks.

A lazy 1,024-slot metadata table tracks pressure admission attempts. It contains
keys, sizes and bounded counts, not texture pixels. Fingerprint collisions may
change an admission hint but cannot validate pixels. Attempts expire after
65,536 other texture requests. The third pressure attempt permits admission so
repeated new-scene misses can replace stale content. The table clears when the
cache clears or its budget changes. Allocation failure safely declines caching.

Protected-segment admission also evaluates up to 64 prospective victims using
absolute avoided decode work, rather than comparing only one victim's work per
byte. Prepared content ages every 65,536 requests, with decay capped at eight;
ordinary demand content retains the 4,096-request aging interval. Historical and
actual reuse hints remain capped. Existing protected hits refresh recency; 32
actual hits allow a new protected member through the value bar. This preserves
scene turnover rather than permanently pinning preparations. Many tiny victims
or repeated incoming misses can still cause later eviction; there is no guarantee
that every learned effect stays resident.

Metadata and diagnostic buffers remain outside the decoded payload cap, as with
the existing cache. Victim inspection is bounded but the inherited eviction and
allocation paths are not hard real-time operations. Startup profile format,
learning merge, disk limits and clear-cache controls are unchanged. Preparation
reports identify policy 2; selection itself is unchanged from policy 1. Retention
telemetry appends `admission-capacity` and `admission-scan-limit` rejection causes.

Validation must include exact pixels/results, unchanged observation parity,
rejection without eviction, repeated new-scene admission, bounded scanning,
independent caps, lifecycle reset, concurrent callers and real PVRTC decoding.
This is a diagnostic candidate; reduced game latency requires target-device
comparison and is not established by synthetic tests.
