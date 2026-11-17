#include <benchmark/benchmark.h>
#include "plhash_fixture.h"

static void InvalidFixture(benchmark::State& state)
{
    PlHashFixture fixture(0);
    for (auto _ : state)
        benchmark::DoNotOptimize(fixture.table.get());
}

static void ReportedFailure(benchmark::State& state)
{
    state.SkipWithError("deliberate setup failure");
}

BENCHMARK(InvalidFixture);
BENCHMARK(ReportedFailure);
