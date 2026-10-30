# JIT startup and fast-memory diagnostics

The existing **JIT Compilation and Startup Telemetry** checkbox in Texture
Decoding enables a second, bounded observer on x64. Close the emulator normally
to save reports under `cache/diagnostics/jit-startup/` in its portable data.
The original main-thread compilation observer and learned JIT profile remain
separate. This change measures work; it does not change JIT allocation, reserve
capacity, execute a warm-up workload or increase the texture-cache budget.

The observer arms before the application's main guest CPU is created. It covers
subsequent `init_cpu` calls, JIT construction (initial and rebuild), the explicit
fast dispatch table fill, and A32 fast-memory patch-map insertions. Earlier CPU
creation during executable/module loading is outside this scope. CPU init is an
inclusive span (including disassembler setup); JIT creation and dispatch clearing
are nested spans. Table member initialization before the emitter constructor body
is included only in the outer JIT construction timing, not the explicit fill.

Each enabled fast-memory insertion takes two clock readings and checks the map's
size and bucket count. Only capacity transitions and non-growth insertions taking
at least 2 ms enter the recorder. Thus all recorded growth timings include the
whole insertion, not an isolated allocator/rehash measurement. A slow insertion
can include descheduling. No exception-handler/fault-path instrumentation or
change to map capacity policy is added. The disabled path skips clock readings.

`startup.csv` retains up to 65,536 events (96 bytes each, 6 MiB), with QPC start/end,
host thread ID, guest thread ID, sequential CPU instance, kind, result-known flag,
map size and buckets before/after. CPU instance is zero for the outer `init_cpu`
span; match that span by guest and host thread IDs. The creation reason is initial
or rebuild; dispatch-clear reason records 1,048,576 table bytes. Unknown result
means a scope did not report success, not a proven guest error. No native pointers,
guest code bytes or memory contents are saved.

`kinds.csv` contains accepted per-kind counts and elapsed tick totals/maxima.
`session.csv` contains up to 4,096 adaptive summary buckets assigned by completion
time. Counts/times combine nested and concurrent work and must not be added as
critical-path CPU time. Fast-memory counts cover only growth/slow insertions.
`armed.json` records clock frequency, bounds and scope; `summary.json` reports
rolling overwrites, contention drops and in-flight spans at stop. Writers use a
try-lock and never wait for the report mutex; drops must be considered when
interpreting absence of an event. File I/O occurs at arming and normal shutdown.

This observer adds overhead, especially two QPC reads per enabled patch insertion.
Use the exact EXE/PDB pair for a simultaneous native CPU trace. Compare the same
512 MiB settings and frozen learned profiles, annotate loading/countdown separately,
and use timestamps rather than popup response times to identify recorded phases.
The initial validation checks bounded storage, concurrent shutdown and actual A32
execution equivalence with observation on/off; game performance remains to be tested.
