# Retention-aware texture preparation candidate

This candidate uses the existing **Texture Decoding → Automatic Texture
Preparation** option and decoded-cache budget. At 512 MiB, startup preparation
uses at most the 384 MiB retained segment and 12,288 retained entries, leaving
room for new loading content within the unchanged 512 MiB/16,384-entry total.
The profile is still validated and merged completely before selection; lower
priority inputs remain learned even when their pixels are not prepared.

Startup selection ranks historical avoided decode work per payload byte, using
bounded decode-cost and reuse hints. It skips entries without protected space
before decoding them. Successful preparation enters the retained segment even
when the individual decode was cheap. This reduces wasted startup work on pixels
that would otherwise be immediately demoted. It does not detect a game's loading
screens or predict the next race: entries outside the set decode on demand, with
the existing parallel decoder. The startup deadline remains five seconds. There
is no new background decoder, worker lifetime, GPU upload, submission or wait.

New retained admissions compare their value with the least recently used
retained entry when space is tight. Cheap one-pass demand does not automatically
displace more valuable warm-up. Existing retained hits still refresh recency.
Measured decode cost is capped at 250 ms, reuse weight at nine, and the payload
denominator has a 4 KiB floor. Historical reuse is a hint, not a demand hit.
Unaccessed entries lose priority for each 4,096 subsequent texture requests;
waiting in a menu alone does not age them. There is no permanent pin. Actual
reused content and later maps can replace old preparation as their value rises
or the old entries age. This is a constant-work admission check against the
retained LRU tail, not a global optimal eviction search. Normal total-capacity
admission may still need to evict retained content for oversized demand jobs.

The decoded byte and entry caps, compressed-input equality, collision rejection,
normal fallback decoding, saved profile format, bounded learning storage and
clear-cache controls are preserved. RAM limits exclude allocator/bookkeeping,
learning history, diagnostic buffers and decode scratch as before. Small cache
entry fields retain historical hints and the request generation. Neither
selection scores nor historical costs are measured performance savings.

Preparation reports add `preparationPolicy: 1` and `preparationBudgetBytes`.
The existing bounded retention observer adds an `admission-value` rejection cause;
its fixed memory and file bounds are unchanged. This cause means an entry remains
available in probation, not that its decode or rendering failed. Existing clear,
demotion and eviction causes remain distinct. Diagnostics are optional and do
not determine admission policy.

Tests cover retained-fit preparation without self-demotion, useful work per byte,
deferral without decoding, exact returned pixels, unchanged learning/profile
identity, expensive preload survival through cheap loading churn, request-based
aging, new-map reuse, independent entry/byte caps, clear/budget changes, disabled
and observed operation, and real-decoder byte identity. Gameplay validation on
the target device is required; no particular game or speedup is guaranteed.

The `.27` candidate adds [admission-aware retention](admission-aware-retention.md)
before total-capacity eviction while keeping this startup selection unchanged.
