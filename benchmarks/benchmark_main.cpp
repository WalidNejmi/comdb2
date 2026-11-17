#include <benchmark/benchmark.h>
#include "build_info.h"

#include <exception>
#include <iostream>

#if !defined(__OPTIMIZE__) || !defined(NDEBUG)
#error "Microbenchmarks require optimization and NDEBUG"
#endif

class CheckedReporter : public benchmark::ConsoleReporter {
  public:
    bool failed = false;
    void ReportRuns(const std::vector<Run>& runs) override
    {
        for (const auto& run : runs)
            failed = failed || run.error_occurred;
        ConsoleReporter::ReportRuns(runs);
    }
};

int main(int argc, char** argv)
{
    try {
        benchmark::Initialize(&argc, argv);
        if (benchmark::ReportUnrecognizedArguments(argc, argv))
            return 1;
        benchmark::AddCustomContext("comdb2_sha", MICROBENCH_COMDB2_SHA);
        benchmark::AddCustomContext("source_dirty", MICROBENCH_SOURCE_DIRTY);
        benchmark::AddCustomContext("benchmark_definition_sha256", MICROBENCH_BENCHMARK_DEFINITION_SHA256);
        benchmark::AddCustomContext("c_compiler", MICROBENCH_C_COMPILER_VERSION);
        benchmark::AddCustomContext("cxx_compiler", MICROBENCH_CXX_COMPILER_VERSION);
        benchmark::AddCustomContext("c_flags", MICROBENCH_C_FLAGS);
        benchmark::AddCustomContext("cxx_flags", MICROBENCH_CXX_FLAGS);
        benchmark::AddCustomContext("google_benchmark_version", MICROBENCH_GOOGLE_BENCHMARK_VERSION);
        benchmark::AddCustomContext("allocator", MICROBENCH_ALLOCATOR);
        benchmark::AddCustomContext("policy", "REPORT_ONLY / NOT_QUALIFIED");
        CheckedReporter reporter;
        const auto matched = benchmark::RunSpecifiedBenchmarks(&reporter);
        benchmark::Shutdown();
        return matched == 0 || reporter.failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "microbenchmark failed: " << error.what() << '\n';
        return 1;
    }
}
