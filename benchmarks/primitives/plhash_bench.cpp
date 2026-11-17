#include <benchmark/benchmark.h>
#include "plhash_fixture.h"

static void counters(benchmark::State& state, hash_t* table, const HashStats& before)
{
    const HashStats after(table);
    const auto steps = static_cast<unsigned>(after.steps) - static_cast<unsigned>(before.steps);
    state.counters["entries"] = after.entries;
    state.counters["buckets"] = after.buckets;
    state.counters["hits"] = static_cast<unsigned>(after.hits) - static_cast<unsigned>(before.hits);
    state.counters["misses"] = static_cast<unsigned>(after.misses) - static_cast<unsigned>(before.misses);
    state.counters["steps_per_op"] = steps / static_cast<double>(state.iterations());
    state.counters["max_steps_including_setup"] = after.max_steps;
    state.SetItemsProcessed(state.iterations());
}

template <bool Readonly>
static void lookup_hit(benchmark::State& state)
{
    const size_t count = state.range(0);
    PlHashFixture fixture(count);
    const HashStats before(fixture.table.get());
    size_t index = 0;
    for (auto _ : state) {
        auto* result = Readonly ? hash_find_readonly(fixture.table.get(), &fixture.entries[index].key)
                                : hash_find(fixture.table.get(), &fixture.entries[index].key);
        benchmark::DoNotOptimize(result);
        if (++index == count)
            index = 0;
    }
    counters(state, fixture.table.get(), before);
}

template <bool Readonly>
static void lookup_miss(benchmark::State& state)
{
    const size_t count = state.range(0);
    PlHashFixture fixture(count);
    const HashStats before(fixture.table.get());
    uint64_t key = count + PlHashFixture::batch + 1;
    const uint64_t first = key;
    for (auto _ : state) {
        auto* result = Readonly ? hash_find_readonly(fixture.table.get(), &key)
                                : hash_find(fixture.table.get(), &key);
        benchmark::DoNotOptimize(result);
        if (++key == first + count)
            key = first;
    }
    counters(state, fixture.table.get(), before);
}

static void PlHash_LookupHit(benchmark::State& s) { lookup_hit<false>(s); }
static void PlHash_LookupMiss(benchmark::State& s) { lookup_miss<false>(s); }
static void PlHash_LookupHitReadonly(benchmark::State& s) { lookup_hit<true>(s); }
static void PlHash_LookupMissReadonly(benchmark::State& s) { lookup_miss<true>(s); }

template <int Depth, bool Readonly>
static void collision(benchmark::State& state)
{
    CollisionFixture fixture(Depth);
    const HashStats before(fixture.table.get());
    size_t index = 0;
    for (auto _ : state) {
        auto* result = Readonly ? hash_find_readonly(fixture.table.get(), &fixture.keys[0])
                               : hash_find(fixture.table.get(), &fixture.keys[index]);
        benchmark::DoNotOptimize(result);
        if (++index == Depth)
            index = 0;
    }
    counters(state, fixture.table.get(), before);
    state.counters["synthetic_chain_depth"] = Depth;
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
BENCHMARK(PlHash_LookupHitReadonly)->Arg(1000)->Arg(100000)->Arg(1000000);
BENCHMARK(PlHash_LookupMissReadonly)->Arg(1000)->Arg(100000)->Arg(1000000);
BENCHMARK_TEMPLATE(collision, 1, false)->Name("PlHash_LookupHit_Chain1");
BENCHMARK_TEMPLATE(collision, 4, false)->Name("PlHash_LookupHit_Chain4");
BENCHMARK_TEMPLATE(collision, 16, false)->Name("PlHash_LookupHit_Chain16");
BENCHMARK_TEMPLATE(collision, 1, true)->Name("PlHash_LookupReadonly_Chain1");
BENCHMARK_TEMPLATE(collision, 4, true)->Name("PlHash_LookupReadonly_Chain4");
BENCHMARK_TEMPLATE(collision, 16, true)->Name("PlHash_LookupReadonly_Chain16");
