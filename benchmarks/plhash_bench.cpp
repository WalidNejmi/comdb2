#include <benchmark/benchmark.h>
#include "plhash_fixture.h"

static void PlHash_LookupHit(benchmark::State& state)
{
    const size_t count = state.range(0);
    PlHashFixture fixture(count);
    size_t index = 0;
    for (auto _ : state) {
        auto* result = hash_find(fixture.table.get(), &fixture.entries[index].key);
        benchmark::DoNotOptimize(result);
        if (++index == count)
            index = 0;
    }
    state.SetItemsProcessed(state.iterations());
}

static void PlHash_LookupMiss(benchmark::State& state)
{
    const size_t count = state.range(0);
    PlHashFixture fixture(count);
    uint64_t key = count + PlHashFixture::batch + 1;
    const uint64_t first = key;
    for (auto _ : state) {
        auto* result = hash_find(fixture.table.get(), &key);
        benchmark::DoNotOptimize(result);
        if (++key == first + count)
            key = first;
    }
    state.SetItemsProcessed(state.iterations());
}

static void PlHash_Insert(benchmark::State& state)
{
    const size_t count = state.range(0);
    PlHashFixture fixture(count);
    // Reserve table/pool capacity once; this case measures steady-state insertion.
    for (size_t i = 0; i < PlHashFixture::batch; ++i)
        fixture.add(count + i);
    for (size_t i = 0; i < PlHashFixture::batch; ++i)
        fixture.remove(count + i);
    while (state.KeepRunningBatch(PlHashFixture::batch)) {
        for (size_t i = 0; i < PlHashFixture::batch; ++i)
            fixture.add(count + i);
        benchmark::ClobberMemory();
        state.PauseTiming();
        for (size_t i = 0; i < PlHashFixture::batch; ++i)
            fixture.remove(count + i);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}

static void PlHash_Delete(benchmark::State& state)
{
    const size_t count = state.range(0);
    PlHashFixture fixture(count);
    while (state.KeepRunningBatch(PlHashFixture::batch)) {
        for (size_t i = 0; i < PlHashFixture::batch; ++i)
            fixture.remove(i);
        benchmark::ClobberMemory();
        state.PauseTiming();
        for (size_t i = 0; i < PlHashFixture::batch; ++i)
            fixture.add(i);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(PlHash_Insert)->Arg(1000)->Arg(100000)->Arg(1000000);
BENCHMARK(PlHash_LookupHit)->Arg(1000)->Arg(100000)->Arg(1000000);
BENCHMARK(PlHash_LookupMiss)->Arg(1000)->Arg(100000)->Arg(1000000);
BENCHMARK(PlHash_Delete)->Arg(1000)->Arg(100000)->Arg(1000000);
