# Investigating unchanged-code A/B variability

## Result

**Large timing deltas were reproduced with the exact same executable, at the
same path, under two arbitrary labels.** A production-code or build difference
is therefore not necessary to produce changes of the magnitude seen in the
[initial expanded comparison](MAIN_VS_CANDIDATE_REPORT.md).

This investigation does not establish one unique physical cause. CPU placement,
NUMA placement and process-to-process timing varied. The observations support
treating the previous small-sample A/B changes as inconclusive, not as code
effects. They do not prove that every previous outlier had the same cause.

Pinning helped several selected cases in this sample but worsened CRC32C
repeatability. **No default affinity policy, universal threshold, qualification
or performance gate is adopted.** No new benchmark families were added.

## Evidence from the original campaign

- The original candidate changes benchmark infrastructure only. Production
  source is identical between main and candidate.
- Read-only ELF inspection confirms identical executable code sections, their
  addresses/sizes, relevant data-section layouts, and shared-library dependency
  names. These are non-PIE `EXEC` binaries, not differently relocated PIE code.
  Their differing full-file hashes include source provenance and debug paths.
- Family-wide medians do not show a uniform candidate slowdown.
- Some small cases are tightly clustered within a process yet move between
  processes. For example, original pool return/512 pair 2 baseline repetitions
  were about 4.77 ns and candidate repetitions about 5.55 ns; pair 4 reversed
  the direction. Buffer/64 remained nearly unchanged.
- Increasing internal repetitions alone would not eliminate those observed
  process-level differences. Iterations/repetitions must not be pooled as
  independent process-level evidence.

The earlier raw files did not contain CPU/NUMA telemetry, so their placements
cannot be reconstructed from current host state.

## Prespecified diagnostic

The diagnostic used the already-built, clean expanded baseline executable,
without recompiling or changing any measured code:

| Field | Value |
|---|---|
| Production SHA, both labels | `5a1af56b5286366a60a3e5a3932d877cc7c8accd` |
| Measured harness SHA | `d3d8d78cc0775157d0700ae939f08364886f3437` |
| Definition SHA256 | `5736f3584a8f2d9478de16f5a1fd5ccc6073a1d3e4c6e65ab3d6e06cdc3e0404` |
| Executable SHA256, both labels | `ce36c1e1c41aab31264bba8b606fdf8d5cfd0509c80304586013c69ae7d98053` |
| CPU | Xeon Gold 6240, 72 logical CPUs, two NUMA nodes, SMT2 |
| Toolchain | GCC/G++ 13.3.1, Google Benchmark 1.6.1, same installation as original campaign |
| UTC window | 2026-11-18 00:14:42-00:16:15 |
| Cases | PlHash miss/1M; pool return/512; software CRC32C/64; no-net buffer put/64 |
| Modes | Inherited affinity 0-71; CPU 2 only |
| Processes | Four balanced label pairs per case per mode: 64 processes total |
| Repetitions | Five per process, min_time 0.05: 320 retained raw repetitions |
| Scheduling | Sequential; AB/BA/AB/BA label order; mode order alternates by round |
| Case order | Rotated by round rather than always executing one case first |
| Host policy | Nice 19 / ionice class 3; no kernel settings changed or cores reserved |
| Monitoring | `perf stat` plus 100 ms read-only samples of the child process |

Both labels launch the **same absolute executable path** from the same working
directory; they do not select baseline versus candidate builds. Output filenames
identify the arbitrary labels. The diagnostic writes its design before starting
and checks executable/build-info hashes around every process.

Read-only admission found low load and approximately 1043 GiB available memory,
with no observed competing benchmark. CPU 2's SMT sibling was not reserved.
ASLR was enabled; no kernel or process ASLR policy was altered.

The first launch failed before benchmark execution because Python 3.6
`re.escape` escaped `/` in a way rejected by the benchmark regex engine.
The filter construction was corrected, and that failed launch is retained
separately. There was one completed diagnostic design, with no trimming or
selection of favorable processes.

## Same-executable results

The eight process medians per case/mode are used for CV (sample SD / mean).
Each row's four deltas compare paired **identical-executable labels**.
Delta convention remains `(second_label - first_label) / first_label * 100`.
These are not candidate-production deltas.

| Case | Mode | Median CPU ns | Process CV % | Max/min spread % | Four paired label deltas % |
|---|---|---:|---:|---:|---|
| PlHash_LookupMiss/1000000 | unpinned | 56.590 | 5.893 | 18.202 | +0.958, -1.756, +0.020, -11.205 |
| PlHash_LookupMiss/1000000 | CPU 2 | 57.470 | 2.577 | 8.192 | +1.437, -0.569, +2.866, +5.673 |
| Pool_Return/512 | unpinned | 4.733 | 3.603 | 11.219 | -6.885, -3.204, +7.778, +0.420 |
| Pool_Return/512 | CPU 2 | 4.577 | 0.419 | 1.239 | -0.267, +0.638, +0.060, -0.307 |
| CRC32C_Software/64 | unpinned | 27.003 | 0.655 | 1.898 | -0.077, -0.021, -0.004, +1.856 |
| CRC32C_Software/64 | CPU 2 | 27.013 | 5.333 | 15.584 | -8.889, -0.003, -0.046, +1.250 |
| Buffer_NoNetPut/64 | unpinned | 1.555 | 0.343 | 1.002 | -0.886, -0.008, -0.050, +0.024 |
| Buffer_NoNetPut/64 | CPU 2 | 1.555 | 0.043 | 0.140 | -0.003, +0.002, -0.120, -0.017 |

Pinning reduced CV in three selected cases but made the fourth worse, and
PlHash still had a +5.67% identical-executable pair on CPU 2. This is not a
general-purpose remedy.

This diagnostic runs one case per process and adds monitoring. Absolute
timings are therefore **not directly compared** to the earlier unmonitored
whole-suite timings as a methodology improvement. Changed preceding fixture
history, process layout and observer overhead are possible confounders.

## What telemetry does and does not establish

- Unpinned samples show different CPUs across processes and, in some processes,
  CPU changes. PlHash process memory was predominantly on different NUMA nodes
  depending on the process; one process changed sockets during its sampled
  lifetime. Those are observations, not proof of remote-memory causation.
- Slow cases do not require a sampled CPU change. The 66.301 ns unpinned
  PlHash process was observed on CPU 35; its same-executable paired process was
  58.872 ns on CPU 17. Other slow pool processes also stayed on one sampled CPU.
- Every pinned sample was on CPU 2. A small nonzero whole-process migration
  counter can include startup before `taskset`/exec; it does not demonstrate a
  timed-loop migration away from CPU 2.
- All eight selected perf events were available and reported 100% scheduled
  coverage: user cycles/instructions/cache misses/branches/branch misses,
  migrations, context switches and page faults.
- Perf covers the **whole child process**, including calibration, fixture
  setup/reset and teardown. It must not be divided by timed iterations and
  advertised as production allocations/op, misses/op or cycles/op.
- The slowest pinned software-CRC process had whole-process IPC about 2.91,
  versus about 3.31-3.36 in the later stable pinned processes. That corroborates
  different observed execution behavior, but does not identify contention,
  cache placement, frequency or another mechanism uniquely.
- NUMA page totals include shared libraries and other mappings. They are not
  a measurement of only the benchmark fixture's pages.
- Governor/current-frequency/turbo and thermal observations were unavailable.
  SMT occupancy, instantaneous clock behavior and physical-page/cache effects
  were not isolated. Do not assign a specific root cause without further
  discriminating evidence.

The diagnostic is useful as a bounded falsification: **neither different
production sources nor a different executable is necessary for these deltas**.
It is not a comprehensive machine-noise characterization.

## Reliability safeguards added after the investigation

The comparison runner now:

1. Checks both executable hashes and both build-info hashes **before and after
   every process**, stopping immediately on a detected change instead of
   spending the rest of the campaign on invalid evidence.
2. Writes an atomically replaced `campaign.json` progress journal before the
   first launch, then retains process order, timestamps, load, launch affinity,
   result JSON fingerprints and handled errors. Externally killed runs can
   leave RUNNING; that is explicitly not COMPLETE.
3. Checks every raw repetition's timing units, finite positive timings and
   five-repetition identity, then recomputes median CPU/real time before
   accepting the reported aggregate.
4. Writes the result summary only after successful complete aggregation and
   a final integrity check.

These are evidence-quality protections, not timing corrections. Nothing
subtracts observed variation, hides outliers, changes benchmark semantics,
automatically pins execution or turns a noisy result into a success.

Validation:

- All nine existing/extended Python benchmark contracts succeeded locally,
  including the 88-case functional run.
- Five targeted comparison contracts also succeeded with Python 3.6.8 on the
  measurement host, without rebuilding the measured executable.
- End-to-end fixture tests verify balanced successful completion and early
  failure/journal retention when an executable or metadata changes.
- Corrupt raw medians, units, nonfinite timings and repetition indexes fail.
- All 64 diagnostic process outputs, 320 raw repetitions, paired arithmetic,
  perf coverage and pinned CPU samples were independently checked.
- No production C/C++ code or fixture changed in this follow-up. Later runner
  changes alter the next build's definition hash; the diagnostic above remains
  attributed to the old frozen harness, not retroactively relabeled.

## Decision and next useful work

Keep all results report-only. Do not use the initial four-pair deltas to select
optimizations, qualify a family, or gate a PR. Do not increase repetitions or
retry until the aggregate happens to shrink.

Before a future real-code comparison, define a family-specific environmental
policy and an accompanying same-executable control. If small effects matter,
the next experiment should discriminate one remaining mechanism (for example
core/sibling occupancy versus allocation/NUMA placement), retaining unfavorable
results as well. This follow-up does not authorize an endless tuning sweep or
claim that a dedicated-host policy has already been validated.

Raw process JSON, perf CSV, sampled telemetry, design/commands and integrity
manifest are retained privately because they identify internal hosts/paths.
The local evidence handoff accompanying this report gives their exact locations.
No object_pool expansion, Walle integration, schema deployment, canary, UI
release or upstream PR was performed.
