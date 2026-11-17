cmake_minimum_required(VERSION 3.16)
project(Comdb2Microbenchmarks LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$" OR CMAKE_CONFIGURATION_TYPES)
  message(FATAL_ERROR "Microbenchmarks require a single-config Release or RelWithDebInfo build.")
endif()
if(NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$" OR
   NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  message(FATAL_ERROR "Microbenchmarks currently support GCC and Clang toolchains.")
endif()

find_package(benchmark 1.6.1 CONFIG QUIET)
if(NOT benchmark_FOUND)
  message(FATAL_ERROR
    "Optional Google Benchmark >= 1.6.1 is missing. Install an approved release and "
    "set CMAKE_PREFIX_PATH or benchmark_DIR; see benchmarks/README.md. "
    "Normal Comdb2 builds do not need this dependency.")
endif()
find_package(Threads REQUIRED)
find_package(Python3 3.6 REQUIRED COMPONENTS Interpreter)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type)
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "${CMAKE_C_FLAGS_${build_type}}")
unset(MICROBENCH_OPTIMIZED_C CACHE)
check_c_source_compiles("
#if !defined(__OPTIMIZE__) || !defined(NDEBUG)
#error Microbenchmarks require optimization and NDEBUG
#endif
int main(void) { return 0; }
" MICROBENCH_OPTIMIZED_C)
if(NOT MICROBENCH_OPTIMIZED_C)
  message(FATAL_ERROR "The configured C flags must enable optimization and NDEBUG.")
endif()
get_filename_component(COMDB2_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include")
configure_file("${COMDB2_SOURCE}/bbinc/plhash.in" include/plhash.h COPYONLY)

# Match the existing standalone cdb2sockpool utility's allocator boundary.
add_library(microbench_primitives STATIC
  "${COMDB2_SOURCE}/util/plhash.c"
  "${COMDB2_SOURCE}/util/pool.c"
  "${COMDB2_SOURCE}/util/logmsg.c"
  "${COMDB2_SOURCE}/util/segstring.c"
)
target_compile_definitions(microbench_primitives PRIVATE BUILDING_TOOLS)
target_include_directories(microbench_primitives PUBLIC
  "${CMAKE_CURRENT_BINARY_DIR}/include"
  "${COMDB2_SOURCE}/bbinc"
  "${COMDB2_SOURCE}/util"
  "${COMDB2_SOURCE}/berkdb"
)
target_compile_options(microbench_primitives PRIVATE -fno-strict-aliasing)
target_link_libraries(microbench_primitives PUBLIC Threads::Threads)

add_custom_target(benchmark_build_info
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_info.py"
    --source "${COMDB2_SOURCE}" --output "${CMAKE_CURRENT_BINARY_DIR}"
    --c-compiler "${CMAKE_C_COMPILER}" --cxx-compiler "${CMAKE_CXX_COMPILER}"
    "--c-flags=${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${build_type}} -fPIC -std=gnu99 -fno-strict-aliasing -DBUILDING_TOOLS"
    "--cxx-flags=${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type}} -fPIE -std=gnu++11 -fno-strict-aliasing"
    --benchmark-version "${benchmark_VERSION}" --benchmark-dir "${benchmark_DIR}"
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/include/build_info.h"
    "${CMAKE_CURRENT_BINARY_DIR}/build-info.json"
  VERBATIM
)

function(add_microbench target source)
  add_executable(${target} ${source} benchmark_main.cpp)
  set_target_properties(${target} PROPERTIES SUFFIX ".tsk")
  target_compile_options(${target} PRIVATE -fno-strict-aliasing)
  target_link_libraries(${target} PRIVATE microbench_primitives benchmark::benchmark)
  add_dependencies(${target} benchmark_build_info)
endfunction()

add_microbench(comdb2_microbenchmarks plhash_bench.cpp)
add_microbench(benchmark_failure_test tests/failure_bench.cpp)
set_target_properties(benchmark_failure_test PROPERTIES EXCLUDE_FROM_ALL ON)
enable_testing()
add_test(NAME microbenchmark_contract
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmarks.py"
    "$<TARGET_FILE:comdb2_microbenchmarks>" "$<TARGET_FILE:benchmark_failure_test>")
