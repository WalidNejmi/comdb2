#include <benchmark/benchmark.h>
#include "buffer_adapter.h"
#include <algorithm>
#include <array>
#include <stdexcept>

using BufferFunction = const uint8_t* (*)(uint8_t*, const uint8_t*, const uint8_t*, size_t);

template <size_t Size, bool Get, bool Network, BufferFunction Function>
static void buffer(benchmark::State& state)
{
    alignas(64) std::array<uint8_t, Size> source, destination, expected;
    for (size_t i = 0; i < Size; ++i)
        source[i] = i * 7 + 1;
    expected = source;
    if (Network && (Size == 4 || Size == 8))
        std::reverse(expected.begin(), expected.end());
    const auto* end = Get ? source.data() + Size : destination.data() + Size;
    if (Function(destination.data(), source.data(), end, 1) != end || destination != expected ||
        Function(destination.data(), source.data(), end - 1, 1) != nullptr)
        throw std::runtime_error("buffer fixture pointer/byte validation failed");
    const uint8_t* result = nullptr;
    while (state.KeepRunningBatch(256)) {
        result = Function(destination.data(), source.data(), end, 256);
        benchmark::DoNotOptimize(result);
        benchmark::ClobberMemory();
    }
    if (result != end || destination != expected)
        throw std::runtime_error("buffer final pointer/byte validation failed");
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * Size);
    state.counters["bytes_per_op"] = Size;
    state.counters["adapter_batch"] = 256;
}

#define BUFFER_CASE(op, size, get, net) \
    BENCHMARK_TEMPLATE(buffer, size, get, net, buffer_##op##_##size)->Name("Buffer_" #op "/" #size)
#define BUFFER_PAIR(size) \
    BUFFER_CASE(Put, size, false, true); BUFFER_CASE(Get, size, true, true)
#define BUFFER_NO_NET_PAIR(size) \
    BUFFER_CASE(NoNetPut, size, false, false); BUFFER_CASE(NoNetGet, size, true, false)
BUFFER_PAIR(4);
BUFFER_PAIR(8);
BUFFER_PAIR(16);
BUFFER_PAIR(64);
BUFFER_PAIR(256);
BUFFER_NO_NET_PAIR(16);
BUFFER_NO_NET_PAIR(64);
BUFFER_NO_NET_PAIR(256);
