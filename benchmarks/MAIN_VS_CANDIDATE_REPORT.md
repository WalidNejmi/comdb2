# Main versus candidate: primitive microbenchmarks

Follow-up: [same-executable reliability investigation](RELIABILITY_INVESTIGATION.md).
The historical measurements below are unchanged; later diagnostics reproduce
large null deltas without a different executable and do not establish a
universal affinity or qualification policy.

## Provenance

| Field | Measured value |
|---|---|
| Upstream main production SHA | `5a1af56b5286366a60a3e5a3932d877cc7c8accd` |
| Candidate production SHA | `e8e2d1b0f9c4f87b385467f1f0fb72b3bd5bd5b3` |
| Candidate identity | `WalidNejmi/comdb2:google-benchmark-foundation` at task start; no PR |
| Frozen expanded harness SHA | `d3d8d78cc0775157d0700ae939f08364886f3437` |
| Benchmark definition SHA256 | `5736f3584a8f2d9478de16f5a1fd5ccc6073a1d3e4c6e65ab3d6e06cdc3e0404` |
| Normalized build recipe SHA256 | `746a723731f5c50bde9dee22024c5dcc141d19f7351fe4b25ed8e245ce49e541` |
| Compiler | GCC/G++ 13.3.1, 20240611, Red Hat 13.3.1-2 |
| Build | RelWithDebInfo, `-O2 -g -DNDEBUG`, `-fno-strict-aliasing`, C99/C++11 |
| Component flags | CRC32C: `-msse4.2 -mpclmul`; C buffer adapter: `-D_LINUX_SOURCE` |
| Google Benchmark | 1.6.1, optimized library, identical library binary on both sides |
| Allocator | Existing `BUILDING_TOOLS` / system allocator |
| Host CPU | Intel Xeon Gold 6240 @ 2.60 GHz, 72 logical CPUs, two sockets, SMT2, two NUMA nodes |
| Pinning | None; inherited affinity 0-71, no NUMA binding or SMT reservation |
| Noise window (UTC, 2026-11-17) | 23:30:34-23:36:19 |
| Accepted A/B window (UTC, 2026-11-17) | 23:43:55-23:53:11 |
| A/B design | Four sequential pairs: AB, BA, AB, BA; five repetitions/process |
| Noise design | Five independent baseline processes; five repetitions/process |
| Minimum time | 0.05 seconds per Google Benchmark repetition |
| Process policy | Sequential, nice 19 / ionice class 3, 16 GiB script memory limit |
| A/B load1/5/15 | Before 1.54/2.01/1.72; after 1.59/1.75/1.76 |
| Available memory | Approximately 1043 GiB throughout |
| Result policy | `REPORT_ONLY / NOT_QUALIFIED` |

Both production checkouts and the harness were clean. Production source files
are identical between these two revisions: only the original benchmark
foundation differs. The expanded harness was compiled against both production
roots, without copying files into either production checkout.

The base and candidate executable `.text` sections are identical, verified by
read-only section inspection. Full binary hashes differ because provenance and
build/debug paths differ:

```text
base:      ce36c1e1c41aab31264bba8b606fdf8d5cfd0509c80304586013c69ae7d98053
candidate: 9b9fb5bc66d433831cfcdec3620b91b8d2cc689af2a9eb9d9376f26e3a1b33be
```

Frequency/governor/turbo-control and thermal observations were unavailable
during host inspection. Reported nominal frequency is not proof of fixed
clocks. This shared-host study does not establish a stable noise envelope for
future runs.

## Outcome

The suite now has **88 cases**, 76 more than the original 12. It covers real
PlHash, pool, CRC32C, Comdb2RLE and endian/buffer code. Normal production build
files and all production source files are unchanged.

**No candidate production optimization was tested.** This is a null comparison
of unchanged production code and an infrastructure validation. Some null
movements are large enough to investigate measurement/environment effects:

- PlHash miss/1M: median paired CPU delta **+11.03%**, individual pairs
  **-0.08% to +13.66%**.
- Pool return/512: median **+9.63%**, pairs **-11.46% to +16.36%**.
- RLE repeated8 decompression/64KiB: median **+3.88%**, pairs
  **-2.98% to +8.52%**, same-binary CV **7.43%**.

These are **clear movements worth investigation as measurement effects**, not
evidence of a code regression. A specific cause (cache placement, allocation,
host interference, frequency, etc.) was not established. The remaining small
movements are **inconclusive**. There is no reviewed family-specific threshold,
significance claim, performance gate, or universal 3% criterion.

No repetitions were increased or selected for favorable results. One campaign
was invalidated for an agent-caused executable-integrity failure; its replacement
is disclosed below and uses the same protocol.

## CONTROL RESULT: existing PlHash before expansion

The required control ran first, before new families were added:

- Harness `8610587fd2ef702cbc61875017e2773547a8c56c`.
- Definition `f144a521bd13222a4a420f19d3c0163507ff3119554724e2c6723710392512ba`.
- Same production SHAs as above, four balanced pairs, five repetitions.
- UTC 2026-11-17 23:19:11-23:21:15.

| Original case | Median paired CPU delta % | Pair min..max % |
|---|---:|---:|
| PlHash_Insert/1000 | -0.20 | -3.17..+0.17 |
| PlHash_Insert/100000 | -0.11 | -0.42..+2.47 |
| PlHash_Insert/1000000 | +0.01 | -0.46..+1.72 |
| PlHash_LookupHit/1000 | +0.00 | -4.71..+0.16 |
| PlHash_LookupHit/100000 | +1.35 | -0.41..+1.94 |
| PlHash_LookupHit/1000000 | -2.39 | -3.57..+1.43 |
| PlHash_LookupMiss/1000 | +0.23 | -2.21..+0.61 |
| PlHash_LookupMiss/100000 | +0.53 | -4.60..+1.45 |
| PlHash_LookupMiss/1000000 | -0.98 | -5.06..+11.22 |
| PlHash_Delete/1000 | -0.18 | -0.99..+0.33 |
| PlHash_Delete/100000 | -0.80 | -2.26..-0.14 |
| PlHash_Delete/1000000 | -0.09 | -0.26..+0.10 |

The control did not show a broad uniform shift and established that both
production revisions can be measured with one harness. It did **not** establish
qualification: the expanded run later exhibited larger null changes. Absolute
timings from different harness definitions are not pooled or directly treated
as an A/B result.

## Expanded A/B CPU-time results

Times are ns per operation (GetReturn counts one get/return pair; pool growth
counts one block-growth-triggering get). Columns "base" and "candidate" are
medians of their four process medians. Delta is the **median of four paired
deltas**, not the delta between those two overall medians:

```text
100 * (candidate_process_median - base_process_median) / base_process_median
negative = faster timing; positive = slower timing; neither is a verdict
```

Representative cases span every new family and include the largest null
movements; full per-name process medians and paired deltas are retained in the
private machine-readable evidence for all 88 cases.

| Benchmark | Base ns | Candidate ns | Paired delta % | Pair min..max % |
|---|---:|---:|---:|---:|
| PlHash_LookupHit/1000000 | 64.558 | 67.756 | +4.33 | -2.24..+10.54 |
| PlHash_LookupHitReadonly/1000000 | 64.664 | 65.571 | +2.10 | -2.38..+5.48 |
| PlHash_LookupMiss/1000000 | 56.481 | 62.397 | +11.03 | -0.08..+13.66 |
| PlHash_LookupMissReadonly/1000000 | 55.587 | 55.729 | +0.25 | -6.19..+2.37 |
| PlHash_LookupHit_Chain16 | 58.725 | 58.824 | +0.17 | -0.16..+0.35 |
| PlHash_LookupReadonly_Chain16 | 60.873 | 60.871 | -0.02 | -0.10..+0.03 |
| Pool_Get/128 | 4.481 | 4.480 | -0.01 | -2.79..+0.55 |
| Pool_GetZeroed/128 | 7.625 | 7.650 | +0.36 | -1.03..+0.85 |
| Pool_Return/512 | 5.085 | 5.553 | +9.63 | -11.46..+16.36 |
| Pool_GetReturn/128 | 6.161 | 6.156 | -0.14 | -6.94..+0.05 |
| Pool_Grow/entry:128/step:64 | 129.206 | 130.254 | +0.81 | +0.41..+1.10 |
| Pool_Grow/entry:128/step:512 | 1609.451 | 1621.233 | +0.92 | -7.69..+10.54 |
| CRC32C_Comdb2/4096 | 157.237 | 157.239 | +0.00 | -0.05..+0.05 |
| CRC32C_Software/4096 | 2075.737 | 2076.973 | +0.06 | -0.00..+0.08 |
| RLE_Compress/Zeros/4096 | 214.089 | 210.892 | -1.42 | -9.04..-0.14 |
| RLE_Compress/Repeated8/4096 | 538916.252 | 538876.158 | -0.03 | -0.12..+0.18 |
| RLE_Compress/Mixed/4096 | 268003.722 | 267891.705 | -0.08 | -0.13..+0.06 |
| RLE_Compress/Incompressible/4096 | 533865.847 | 533770.466 | -0.03 | -0.14..+0.08 |
| RLE_Decompress/Repeated8/65536 | 1905.645 | 1971.659 | +3.88 | -2.98..+8.52 |
| Buffer_Put/4 | 0.950 | 0.950 | -0.01 | -0.65..+0.05 |
| Buffer_Get/8 | 0.650 | 0.650 | -0.02 | -0.05..+0.08 |
| Buffer_NoNetPut/64 | 1.554 | 1.554 | +0.00 | -0.03..+0.02 |
| Buffer_NoNetGet/256 | 4.872 | 4.872 | -0.00 | -0.01..+0.00 |

Representative real-time medians, also ns:

| Benchmark | Base | Candidate |
|---|---:|---:|
| PlHash_LookupMiss/1000000 | 56.691 | 62.621 |
| Pool_Return/512 | 5.082 | 5.556 |
| CRC32C_Comdb2/4096 | 157.546 | 157.559 |
| CRC32C_Software/4096 | 2079.601 | 2080.628 |
| RLE_Compress/Mixed/4096 | 268570.322 | 268423.611 |
| RLE_Decompress/Repeated8/65536 | 1909.076 | 1977.494 |
| Buffer_NoNetPut/64 | 1.557 | 1.557 |

CPU and real time have separate per-pair output, as do items/s and bytes/s where
available. Throughput deltas use the same arithmetic, but their direction is
the opposite of timing; no automated interpretation is attached.

### Mutable versus readonly lookup: different semantics

Baseline process medians, ns; this is a side-by-side API observation, not an
optimization comparison or a recommendation to substitute one API for another:

| Entries | Hit `hash_find` | Hit readonly | Miss `hash_find` | Miss readonly |
|---|---:|---:|---:|---:|
| 1000 | 26.100 | 24.744 | 24.024 | 23.217 |
| 100000 | 46.530 | 38.737 | 43.941 | 42.435 |
| 1000000 | 64.558 | 64.664 | 56.481 | 55.587 |

`hash_find` can move hits to the chain head. Readonly does not. Controlled
collision fixtures deliberately keep the mutable next target at the tail by
rotating keys, whereas readonly repeatedly queries one fixed tail.

## Same-binary noise, by family

Each case has five process medians. CV uses sample SD / arithmetic mean;
maximum spread is `(max/min - 1) * 100`. These are observations, not thresholds.

| Family | Cases | CPU CV range % | Largest CPU spread % | Real-time CV range % | Largest real-time spread % |
|---|---:|---:|---:|---:|---:|
| PlHash | 24 | 0.013-4.958 | 13.178 | 0.040-4.966 | 13.190 |
| Pool | 16 | 0.024-4.723 | 11.115 | 0.036-4.712 | 11.106 |
| CRC32C | 8 | 0.002-0.787 | 1.874 | 0.005-0.786 | 1.887 |
| RLE | 24 | 0.015-7.433 | 20.384 | 0.042-7.437 | 20.397 |
| Buffer | 16 | 0.004-0.991 | 2.237 | 0.040-1.083 | 2.457 |

The same-binary study is compact, and some subsequent null A/B deltas exceed its
observed spread. Do not treat its maximum as a guaranteed future error bound.
Pinning was not revisited or adopted based on the previous mixed study.

## Domain counters and fixture limits

- The 1M-entry natural PlHash fixture reported 4,194,319 buckets and zero
  skipped-node steps for these deterministic lookups. Its size curve should
  not be interpreted as measured collision-chain cost.
- Synthetic chain16 reported 16 entries, 257 buckets and 15 skipped nodes/op on
  both lookup paths. Setup verifies exact depth through the public stats API.
- Steady-state pool cases reported 256 entries / one block / no growth;
  GetReturn and Return leave zero entries used at the measured batch boundary.
  Growth cases verify exactly one new block and one used entry per operation.
- CRC32C throughput uses input bytes. In the first baseline process, 4KiB
  Comdb2 dispatch was about 26.06 GB/s versus 1.97 GB/s software. Different
  algorithms are separate families, not a main-versus-candidate speedup.
- Mixed RLE/4096 produced 2208 bytes (ratio 0.5390625). Repeated8/4096 produced
  **4100 bytes**, not compression: this production codec does not automatically
  exploit every repeating pattern. Input class names describe inputs, not an
  assumption about output. No incompressible case was dropped.
- Buffer/64 counts 64 bytes/op and uses a 256-operation C adapter batch.
  Optimized assembly shows inline byte swap for 4-byte conversions; helper-call
  indirection is not introduced per operation. Loop/barrier/boundary costs
  remain, and these tiny warmed-buffer timings are not protocol latency.
- No private allocator structs, global malloc interception, fake server state,
  database, network or disk operation was added.

## Integrity incident and exclusion

The first expanded A/B attempt was interrupted and excluded in full. An
additional `objcopy --dump-section` inspection omitted its output-object
argument, rewriting the baseline executable in place. Its full binary hash
changed even though `.text` did not. The attempted candidate rewrite was
rejected by the OS because it was executing.

The process tree was stopped. Relinking the unmodified baseline objects restored
the **exact original binary SHA256** listed above. The candidate hash was
unchanged. The accepted four-pair campaign then ran once in a new directory;
pre/post binary hashes matched. The completed five-process noise study predates
the incident and remains valid. Interrupted raw files and a written invalidation
record were retained; none contributed to the results above. This restart was
required by integrity, not a search for a favorable result.

## Build, run and validation

Portable command forms below deliberately omit private machine paths.
The retained evidence contains the exact executed argv arrays and paths.
Use **the same compiler binaries and dependency installation** on both sides.

```sh
make -C "$HARNESS/benchmarks" check BUILD_DIR="$BASE_BUILD" \
  CMAKE_ARGS="-DCOMDB2_SOURCE_ROOT=$BASE_TREE -DCMAKE_PREFIX_PATH=$BENCHMARK_PREFIX -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX"
make -C "$HARNESS/benchmarks" check BUILD_DIR="$CANDIDATE_BUILD" \
  CMAKE_ARGS="-DCOMDB2_SOURCE_ROOT=$CANDIDATE_TREE -DCMAKE_PREFIX_PATH=$BENCHMARK_PREFIX -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX"

nice -n 19 ionice -c3 python3 "$HARNESS/benchmarks/noise_study.py" \
  "$BASE_BUILD/comdb2_microbenchmarks.tsk" "$NEW_NOISE_DIR" \
  --filter='^(PlHash_|Pool_|CRC32C_|RLE_|Buffer_)'
nice -n 19 ionice -c3 python3 "$HARNESS/benchmarks/compare_revisions.py" \
  "$BASE_BUILD/comdb2_microbenchmarks.tsk" \
  "$CANDIDATE_BUILD/comdb2_microbenchmarks.tsk" "$NEW_AB_DIR" \
  --filter='^(PlHash_|Pool_|CRC32C_|RLE_|Buffer_)'
```

- Functional contracts succeeded locally and for both external production roots
  on the measurement host. Six Python tests exercise all 88 names, filters,
  repetition/JSON shape, actual production-root/SHA provenance, failure exits,
  comparison identity and the process/pair hierarchy.
- A deliberately different definition hash refuses comparison **before**
  execution/output-directory creation. Compiler/recipe/library mismatches and
  dirty provenance are rejected too.
- Fixture checks verify readonly lookups, exact chains, pool counts/zeroing,
  fixed/reference CRCs, all RLE round trips and buffer bytes/end pointers.
- All 3520 accepted A/B raw repetition rows and 2200 noise rows were retained.
  Their aggregate medians and per-pair deltas were recomputed and checked;
  evidence has a separate SHA256 manifest.
- Normal production files/build graph are unchanged; no full server build,
  performance CI gate, Walle integration, schema deployment or UI/RR change
  is claimed.

Detailed private artifacts: control JSON/logs; five noise JSON/logs plus summary;
eight accepted A/B JSON/logs plus summary; both build-info/compile-command files;
environment observations; interrupted attempt plus invalidation note.
They are deliberately not committed to public history because they identify
internal hosts and paths. A local handoff delivered with this report records
their locations.

See [README](README.md) for all stable names and precise fixture semantics.
Implementation is limited to `benchmarks/`: standalone build/provenance,
comparison/noise scripts, five primitive source files, fixtures and tests.
`object_pool`, commit-map, LC cache, prefault, contention and hardware counters
remain deferred; no server-dependent component was fabricated to enlarge the
suite. Resolve the observed null variability before interpreting small
cross-revision movements as production-code effects.
