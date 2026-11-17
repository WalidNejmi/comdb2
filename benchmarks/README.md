# Comdb2 microbenchmarks

This optional Google Benchmark harness measures isolated Comdb2 primitives.
It does not run a database, replace sysbench, qualify results, or gate a PR.
Walle integration is a later milestone; scheduler and persistence behavior are
unchanged.

## Build

The current repository uses CMake, not a top-level recursive Makefile.
This standalone entry point deliberately leaves the production build graph
untouched:

```sh
make -C benchmarks CMAKE_ARGS='-DCMAKE_PREFIX_PATH=/path/to/benchmark/install'
make -C benchmarks check CMAKE_ARGS='-DCMAKE_PREFIX_PATH=/path/to/benchmark/install'
```

Use the **same C and C++ compiler as your Comdb2 build**, explicitly if needed:

```sh
make -C benchmarks CMAKE_ARGS='-DCMAKE_PREFIX_PATH=/path/to/benchmark/install -DCMAKE_C_COMPILER=/path/to/gcc -DCMAKE_CXX_COMPILER=/path/to/g++'
```

Prerequisites: Linux x86_64, CMake >= 3.16, Python >= 3.6, GCC or Clang, pthreads, and an
approved optimized Google Benchmark >= 1.6.1 installation exporting its CMake
package. `benchmark_DIR` may point directly at that package directory instead.
The build never downloads or vendors dependencies. Install the optional
dependency separately on developer/CI hosts; normal builds need none of it.
`pkg-config --modversion benchmark` can help inspect an installation.

An equivalent out-of-source invocation:

```sh
cmake -S benchmarks -B /tmp/comdb2-microbench-build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH=/path/to/benchmark/install
cmake --build /tmp/comdb2-microbench-build --target comdb2_microbenchmarks -j2
```

Only single-configuration Release/RelWithDebInfo builds are supported.
RelWithDebInfo uses the compiler's CMake defaults (normally `-O2 -g -DNDEBUG`);
both harness and primitives use `-fno-strict-aliasing`, as in Comdb2.
Do not use instrumentation/debug builds for timing. Configure fails explicitly
when the optional dependency or an optimized build configuration is absent.

## Run and retain JSON

```sh
./benchmarks/build/comdb2_microbenchmarks.tsk --benchmark_list_tests=true
./benchmarks/build/comdb2_microbenchmarks.tsk \
  --benchmark_filter='^PlHash_' \
  --benchmark_repetitions=10 \
  --benchmark_display_aggregates_only=true \
  --benchmark_out=/tmp/plhash.json --benchmark_out_format=json
```

These flags are tested against Google Benchmark 1.6.1. Display-only aggregates
keep raw repetition rows in the JSON. `--benchmark_report_aggregates_only=true`
also discards raw repetition rows from the file; avoid that for noise studies.
Use `--benchmark_min_time=0.001` only for functional smoke tests, not meaningful
performance conclusions. An empty filter match, setup exception, or reported
benchmark error returns nonzero. Console formatting is fixed by the checked
reporter; use `--benchmark_out_format=json`, not `--benchmark_format=json`.

Google Benchmark records timing, repetitions, host and CPU/cache context.
The executable also records source SHA/dirty state, definition hash, compiler
versions, flags, dependency version and allocator. `build/build-info.json`
contains compiler paths and dependency CMake location; `compile_commands.json`
contains the actual per-file commands. Provenance is regenerated at each build.
Retain these with the executable hash, raw JSON, CPU model and environment
observations. A dirty tree is explicitly marked and is not exact-SHA evidence.

## First family and fixture semantics

Stable names: `PlHash_{Insert,LookupHit,LookupMiss,Delete}/{1000,100000,1000000}`.
The parameter is populated entry count, not a date, SHA or host.

The harness links **real repository C sources**, not a hash-table surrogate:
`util/plhash.c`, `pool.c` and their logging/string utility dependencies.
It uses the established `BUILDING_TOOLS` system-allocator boundary, like
`tools/cdb2sockpool`, rather than Comdb2's server memory-accounting layer.
This is the repository's plhash implementation, not an external sysutil
substitution used by some internal production builds.

Commit-map was considered first. Its entry points share `berkdb/txn/txn_util.c`
with recovery/prepared-transaction code and depend on `DB_ENV`, Berkeley DB
allocators, LSN state and mutex wrappers. Rather than fabricate server globals
or extract a second implementation, this first layer uses plhash.

The existing generated `plhash.h` interface is C++ safe (`extern "C"`).
C sources remain compiled as C99; only the harness/fixture use C++11. No new C
adapter or production API change is necessary.

- Entries use deterministic consecutive 64-bit keys and preallocated payloads.
  Construction, population, full lookup validation and destruction are outside
  the timed loop. Lookups cycle through all populated keys; misses cycle through
  a disjoint range. They use `hash_find`, including its normal move-to-front
  behavior, not `hash_find_readonly`.
- Insert prewarms capacity, then inserts 256 spare entries per batch at the
  specified starting population. Removal/reset is paused. This is **steady-state
  insertion**, not cold allocation or table-growth throughput.
- Delete removes the first 256 entries per batch; reinsertion/reset is paused.
  It measures warmed deletion, not a full-table purge.
- `KeepRunningBatch(256)` reports per-operation time, not per-batch time.
  Timer pause/resume bookkeeping and loop/check overhead are not subtracted;
  batching amortizes that cost. Reset work can still affect cache state.
- Lookup results use `DoNotOptimize`; mutation batches use `ClobberMemory`.
  Failures are explicit, not represented as zero timings.

The fixture owns the table and payloads. It requires no network, database,
random seed, or file I/O in the measured operation.

## Expanded primitive families

There are 88 cases across PlHash (24), Pool (16), CRC32C (8), RLE (24) and
Buffer (16). All setup and correctness checks are outside timing. In addition
to the original warmed plhash mutations:

- `PlHash_Lookup{Hit,Miss}Readonly/{1000,100000,1000000}` uses the real
  readonly API. Default native query statistics stay enabled on both lookup
  paths; disabling them would also change `hash_find` semantics. Counters
  are read outside the timed loop. Native steps count skipped nodes, not the
  successful matching node; max steps includes setup.
- `PlHash_LookupHit_Chain{1,4,16}` and
  `PlHash_LookupReadonly_Chain{1,4,16}` are **synthetic**, constant-hash fixtures.
  Public statistics verify tail depth repeatedly without mutation and through
  a full move-to-front sweep. Mutable lookups rotate keys, keeping each next
  target at the tail; readonly always queries the fixed tail. Native
  steps/op must be depth minus one. They do not model production distributions.
- `Pool_{Get,GetZeroed,Return,GetReturn}/{32,128,512}` uses a prewarmed 256-entry
  pool. Get/return batches reset outside timing; zeroed gets validate all bytes
  and re-dirty payloads while paused. GetReturn immediately returns each block
  and measures that pair as one item. Native counts prove no steady-state growth.
- `Pool_Grow/entry:{32,128,512}/step:64` and
  `Pool_Grow/entry:128/step:512` measure first get from an empty initialized pool,
  including allocation/free-list initialization. Thirty-two independent pools
  amortize pause overhead. `pool_clear` and count checks are paused. System
  allocator reuse is possible; this is not cold OS-page allocation.
- `CRC32C_{Comdb2,Software}/{64,256,4096,65536}` uses deterministic buffers,
  the zero-seed/no-final-XOR Comdb2 convention, a fixed check vector and an
  independent bitwise reference. Initialization is outside timing. Comdb2
  dispatch may use CPU acceleration; Software is kept separate. The x86 CRC
  translation unit uses production's `-msse4.2 -mpclmul` flags. No unsupported
  ARM recipe is inferred.
- `RLE_{Compress,Decompress}/{Zeros,Repeated8,Mixed,Incompressible}/{256,4096,65536}`
  uses a fixed xorshift seed for literals; Mixed alternates 64-byte runs and
  literals. Preallocated output allows expansion on incompressible data.
  Round trips are validated before and after timing. Throughput counts original
  uncompressed bytes on both paths; bytes_in/out and compressed/original ratio
  show actual sizes. Descriptor capacity reset and return-code accumulation are
  part of the measured call path; decompression input is prepared beforehand.
- `Buffer_{Put,Get}/{4,8,16,64,256}` and
  `Buffer_{NoNetPut,NoNetGet}/{16,64,256}` use aligned buffers and validate bytes,
  returned pointers and too-short end pointers. Production's C-only header
  requires a thin **C batch adapter**: 256 calls per language boundary, with
  memory compiler barriers preventing redundant-copy elimination. Helpers are
  inlined inside that optimized C loop (not out-of-line replacement helpers).
  Boundary/loop/barrier costs are not subtracted. Network conversion flips 4/8
  byte values; the larger sizes are copies, not arrays of converted scalars.

Single-thread fixtures are warmed, not cold-cache models. No malloc interception,
fake DB_ENV, server, network or disk operation is introduced. Object pool,
commit-map, LC cache, prefault and contention are deferred to a separate phase.
Use `--filter='^(PlHash_|Pool_|CRC32C_|RLE_|Buffer_)'` on either Python runner
to select the full suite. Same-binary noise still uses five independent
processes and five repetitions; no family receives a universal threshold.

## Noise and comparison

### Frozen harness, different production revisions

Configure the same clean harness checkout twice with
`-DCOMDB2_SOURCE_ROOT=/absolute/clean/production/checkout` and separate build
directories. The default remains the harness's parent checkout. Do not copy
benchmark files into the baseline. Both source trees must be frozen at exact
commits; `build-info.json` records production identity separately from harness
identity. The definition hash covers benchmark C/C++/Python and build recipes,
not the production implementation.

```sh
cmake -S /frozen/harness/benchmarks -B /build/base \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCOMDB2_SOURCE_ROOT=/source/base \
  -DCMAKE_PREFIX_PATH=/dependency/install
cmake -S /frozen/harness/benchmarks -B /build/candidate \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCOMDB2_SOURCE_ROOT=/source/candidate \
  -DCMAKE_PREFIX_PATH=/dependency/install
cmake --build /build/base
cmake --build /build/candidate
python3 /frozen/harness/benchmarks/compare_revisions.py \
  /build/base/comdb2_microbenchmarks.tsk \
  /build/candidate/comdb2_microbenchmarks.tsk /new/comparison
```

The comparison refuses dirty provenance or different definitions, compiler
binaries/flags or dependency libraries. Four sequential pairs use AB/BA/AB/BA
order, five repetitions/process and min_time 0.05. Each pair uses process
medians; output retains every paired delta plus median/min/max. Timing delta is
`(candidate-base)/base*100`, negative means faster, not a verdict.
Raw repetitions, JSON context, command arrays and executable hashes are retained.
Use one admitted host and the same environment for both sides. No affinity is
imposed. This is report-only, not a replacement for workload qualification.
The Unix Makefiles generator is required: normalized compile commands and the
actual link recipe are hashed, including per-source flags, with only build,
harness and production root paths replaced. Ninja/multi-config generators are
rejected rather than silently omitting recipe provenance.

```sh
python3 benchmarks/noise_study.py \
  ./benchmarks/build/comdb2_microbenchmarks.tsk /tmp/plhash-noise
```

This runs five complete processes with five Google Benchmark repetitions each,
retaining all JSON. Add `--cpu N` on Linux to compare alternating unpinned and
CPU-N-pinned processes; pinning is opt-in and never changes kernel settings.
Only run on an approved idle host, at low priority; do not compete with another
benchmark. Observe host load, available memory, CPU model/governor/frequency,
NUMA, SMT and thermal sensor availability. Pinning to a logical CPU does not
reserve its SMT sibling.

The summary uses each process's repetition **median** for real/cpu time, then
reports the median, sample standard deviation, coefficient of variation, and
maximum relative difference across process medians (`(max/min - 1) * 100`).
No pooling of iterations or automatic 3% criterion is used. All results remain
`REPORT_ONLY / NOT_QUALIFIED`, regardless of variation.

For manual base/candidate analysis, Google Benchmark's source distribution
provides `tools/compare.py benchmarks base.json candidate.json` (with its
separate NumPy/SciPy requirements). It is not installed by all library packages.
Do not parse console output. Before comparing different revisions, require
matching `benchmark_definition_sha256`, build recipe and environment. The
future Walle adapter must enforce this and balanced AB/BA process ordering;
this foundation does not yet publish normalized comparisons.

## Extending and testing

Add one family in a separate C++ source, link the actual C component, and
register stable `Component_Operation/parameter` names at multiple sizes.
Benchmark C/C++/header/Python and CMake/Make files are included automatically
in the definition hash (excluding the conventional `build/` output directory).
Keep other build output directories outside the harness source.
Keep setup/teardown out of the timed region and document cache/allocation
semantics. Never benchmark server/network/SQL behavior in this harness.

`make -C benchmarks check` checks names, filtering, repetition/JSON shape,
missing-match/argument/setup/reporter failures and positive finite observations.
It deliberately has no absolute timing thresholds. The failure-only binary
is excluded from the default build and never linked into the real suite.
