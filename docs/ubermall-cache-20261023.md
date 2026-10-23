# Ubermall decoded-texture cache comparison — 2026-10-23

Both captures use the same optimized Windows EXE/PDB (`v1.3.3-win11diag.2`), corrected NGS audio, existing graphics profile, BelowNormal priority and all 16 logical processors. Music worked in both runs; the owner confirmed continuous driving throughout both 45-second measurements. Controls and graphics worked with caching enabled. Ship was not independently reconfirmed. Production installation and Playnite mapping remain unchanged.

| CPU present intervals | Cache off (0 MiB) | Cache on (256 MiB) |
|---|---:|---:|
| Presents | 1,344 | 1,349 |
| Median | 33.3181 ms | 33.3603 ms |
| p99 | 39.4260 ms | 35.4326 ms |
| Maximum | 242.3608 ms | 63.8445 ms |
| Gaps >50 ms | 10 | 1 |
| Gaps >100 ms | 2 | 0 |

Stems: `OptimizedPlus-combat-20261023-052118-c3c3a6f3` (off), `OptimizedPlus-combat-20261023-052741-ef904cd0` (on). Raw ETLs and per-frame/QPC/scheduling exports retained privately; aggregate results are shared here. Both ETLs have zero lost events, no truncation and 1 ms sampling. All target native frames resolve against GUID `9d33eab1-1c5a-4493-9a9e-a3e05e2d82d5`, age 1.

The off trace has 992 sampled PVRTC decoder frames across its complete trace; the on trace has 19. These are full ETL totals, including recorder rundown beyond the 45-second present window, and are not exact decoding durations. Off worker 27640 is CPU-busy through the 242.3608 ms gap (218 PVRTC samples, 1.461 ms off CPU); guest render 24376 is off CPU for 220.768 ms and display 15604 for 211.228 ms. All ten off gaps include PVRTC samples. On worker 26368 has six PVRTC samples in the single 63.8445 ms gap, mixed with texture upload/hash work and 33.429 ms off CPU. This remaining gap does not show the earlier hundreds-of-milliseconds sustained decode pattern.

Session log counters later progress from requests 7168 / hits 4028 / misses 3140 at 05:29:00.830 to requests 9216 / hits 6076 / misses 3140 at 05:29:48.068: 2,048 consecutive requests hit without another decode. Payload residency is 266,265,600 bytes (compressed snapshots plus decoded pixels), within the 268,435,456-byte budget; 3,079 entries, 61 evictions. These are cumulative/session log observations outside part of the measured interval, not a precise 45-second hit rate. Container/hash bookkeeping adds overhead beyond the payload budget.

Owner reports fewer or no pauses in the on measurement, then reports another race is “way smoother” with barely noticeable hitches. An additional warm capture is being collected; do not include it in these numbers until its context is verified.

This is one sequential A/B pair, with race events, ship and cache warming not fully controlled. It supports repeated decoded-texture reuse as a practical improvement for this workload; it does not establish a universal fix or a Windows scheduler defect. Median pacing remains approximately 30 presents/second. These are CPU present intervals, not display latency. Earlier silent-audio .1 traces establish the decode lead but are excluded from this corrected-audio A/B comparison.

Exact measured binary provenance: source `82e12040cdad72b8509ff19d0aa90f98d73a2787` plus diagnostic support diff SHA256 `75fdad3bec4910de2205012e3593cccf240a541a09f65df4433368c79a9f5ffb` and LibAtrac9 diff SHA256 `7026c979268a8edec2fe256a5f04866c1d63e9b31249230bc32dfb229b7f7371`; EXE SHA256 `0640cdbc387120baa6f20ea78208aef95c7e622ea7b2eaaef3a0f3d714120f79`; PDB SHA256 `bf75551daa2f19d662fcc3a004952bfafb9f5a4050a6f02f4b65d89d8d7798cb`. This is a separately sourced Plus diagnostic baseline, not an established exact rebuild of the published Plus package. MSVC 19.44.35229.0, SDK26100, Qt6.11.0, RelWithDebInfo /O2 + LTO + full symbols. All six Windows CTests passed, including audio-span and enabled/disabled cache validation.
