# Bounded decoded texture retention diagnostics

**Texture Cache and Upload Telemetry**, in **Texture Decoding → Diagnostics**,
now also records why decoded textures leave the cache. Restart after changing
this optional switch and close normally to save reports. This diagnostic does
not change the 512 MiB setting, retention policy, preload ranking, pixel validation
or decoder concurrency, and does not itself improve performance.

The total decoded cache is bounded by both its configured payload budget and
entry limit. Its retained segment is limited to three quarters of each bound.
Expensive admissions and hits move to the retained front; ranked preparation
appends to its back. When the retained segment exceeds a bound, its tail is
demoted into probation. Total-cache pressure evicts probation first. Consequently,
a texture can have been prepared successfully and still need decoding later.
Preparation is not a permanent pin, and compressed profiles on disk are separate
from decoded pixels in RAM.

Normal-close coverage output gains three files alongside the existing files:

- `.coverage-retention-events.csv`: chronological admission, first demand hit,
  promotion, demotion, eviction, miss, rejection and clear events. Each admission
  receives a session-local serial. A demotion/eviction records the triggering
  admission or hit serial, identity candidate, cost, segment, uses, decode cost,
  dimensions and QPC. Misses count rejected matching-fingerprint candidates,
  distinguish size/byte mismatches, and link the first rejected resident serial.
- `.coverage-retention-index.csv`: bounded per-identity counts, first preload and
  first/last demand times, and latest demotion/eviction trigger and cause. These
  totals and latest outcomes survive detail-ring overwrite. All hits count in
  this index; only the first hit of a resident entry is a detailed hit event.
- `.coverage-retention-summary.json`: schema, clock frequency, bounds, payload
  allocation, kind totals, overwrites and events/hits without a tracked identity.

The detail ring has 65,536 records of 128 bytes (8 MiB). A preallocated index of
32,768 × 144 bytes (4.5 MiB) shares the existing coverage identity namespace:
12.5 MiB payload plus vector/object overhead. This is additional to existing work,
coverage and reuse telemetry, and outside the configured decoded-pixel budget.
Small serial/identity fields are also added to cache-entry metadata. Allocation
happens when telemetry is enabled; hot-path recording takes no extra lock or
allocation and uses the existing cache mutex. Report I/O happens at normal close.

Event state is sampled before eviction/demotion; the demotion snapshot can
therefore show a temporarily over-limit retained segment before its tail is
trimmed. Admission events follow making room, before retention placement. Hit
uses increment before first-hit and promotion events. Reject events distinguish
whole-cache ineligibility, duplicate insertion, no-eviction preparation pressure
and inability to join the retained segment. Clear/budget-change events identify
cache-wide invalidation. Attach events identify content already resident when a
new observer starts; its prior preparation provenance is unknown.

Coverage IDs are fingerprint candidates, not byte identities. Admission serials
distinguish actual resident entries but are not persistent content IDs. No hashes,
texture bytes, pointers, guest addresses or asset names are written. Identity
capacity/probe drops and ring overwrites limit reconstruction; no missing event
should be treated as proof that a transition did not happen. Latest per-identity
summaries can combine colliding candidates and are not a complete lifetime log.
QPC data can be compared with exact-symbol CPU and presentation traces. It does
not measure GPU execution time or establish speedups by itself.

Tests check causal trigger serials for demotion and eviction of an unused preload,
byte/size mismatch reporting without stale output, both pressure bounds, clear
and budget-change attribution, bounded ring rollover with persistent latest
outcomes, concurrent cache hits, and identical cache results/accounting with the
observer enabled and disabled. Existing real-decoder and cache tests also run.
