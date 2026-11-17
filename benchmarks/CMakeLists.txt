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
get_filename_component(default_source "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(COMDB2_SOURCE_ROOT "${default_source}" CACHE PATH "Exact Comdb2 production checkout")
get_filename_component(COMDB2_SOURCE "${COMDB2_SOURCE_ROOT}" REALPATH)
if(NOT EXISTS "${COMDB2_SOURCE}/util/plhash.c")
  message(FATAL_ERROR "COMDB2_SOURCE_ROOT must name a Comdb2 production checkout")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include")
configure_file("${COMDB2_SOURCE}/bbinc/plhash.in" include/plhash.h COPYONLY)

# Match the existing standalone cdb2sockpool utility's allocator boundary.
add_library(microbench_primitives STATIC
  "${COMDB2_SOURCE}/util/plhash.c"
  "${COMDB2_SOURCE}/util/pool.c"
  "${COMDB2_SOURCE}/util/logmsg.c"
  "${COMDB2_SOURCE}/util/segstring.c"
  "${COMDB2_SOURCE}/crc32c/crc32c.c"
  "${COMDB2_SOURCE}/comdb2rle/comdb2rle.c"
)
target_compile_definitions(microbench_primitives PRIVATE BUILDING_TOOLS)
target_include_directories(microbench_primitives PUBLIC
  "${CMAKE_CURRENT_BINARY_DIR}/include"
  "${COMDB2_SOURCE}/bbinc"
  "${COMDB2_SOURCE}/util"
  "${COMDB2_SOURCE}/berkdb"
  "${COMDB2_SOURCE}/crc32c"
  "${COMDB2_SOURCE}/comdb2rle"
)
set(component_flags "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  set_source_files_properties("${COMDB2_SOURCE}/crc32c/crc32c.c"
    PROPERTIES COMPILE_OPTIONS "-msse4.2;-mpclmul")
  set(component_flags "crc32c: -msse4.2 -mpclmul")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|armv7l|aarch64")
  message(FATAL_ERROR "ARM CRC build flags require a separately reviewed production-equivalent recipe")
endif()
target_compile_options(microbench_primitives PRIVATE -fno-strict-aliasing)
target_link_libraries(microbench_primitives PUBLIC Threads::Threads)

add_custom_target(benchmark_build_info
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_info.py"
    --source "${COMDB2_SOURCE}" --harness "${CMAKE_CURRENT_SOURCE_DIR}"
    --output "${CMAKE_CURRENT_BINARY_DIR}"
    --c-compiler "${CMAKE_C_COMPILER}" --cxx-compiler "${CMAKE_CXX_COMPILER}"
    "--c-flags=${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${build_type}} -fPIC -std=gnu99 -fno-strict-aliasing -DBUILDING_TOOLS"
    "--cxx-flags=${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type}} -fPIE -std=gnu++11 -fno-strict-aliasing"
    --benchmark-version "${benchmark_VERSION}" --benchmark-dir "${benchmark_DIR}"
    --benchmark-library "$<TARGET_FILE:benchmark::benchmark>"
    "--component-flags=${component_flags}"
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/include/build_info.h"
    "${CMAKE_CURRENT_BINARY_DIR}/build-info.json"
  VERBATIM
)

function(add_microbench target source)
  add_executable(${target} ${source} benchmark_main.cpp)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  set_target_properties(${target} PROPERTIES SUFFIX ".tsk")
  target_compile_options(${target} PRIVATE -fno-strict-aliasing)
  target_link_libraries(${target} PRIVATE microbench_primitives benchmark::benchmark)
  add_dependencies(${target} benchmark_build_info)
endfunction()

add_microbench(comdb2_microbenchmarks primitives/plhash_bench.cpp)
target_sources(comdb2_microbenchmarks PRIVATE primitives/pool_bench.cpp)
target_sources(comdb2_microbenchmarks PRIVATE primitives/crc32c_bench.cpp primitives/rle_bench.cpp)
add_microbench(benchmark_failure_test tests/failure_bench.cpp)
set_target_properties(benchmark_failure_test PROPERTIES EXCLUDE_FROM_ALL ON)
enable_testing()
add_test(NAME microbenchmark_contract
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmarks.py"
    "$<TARGET_FILE:comdb2_microbenchmarks>" "$<TARGET_FILE:benchmark_failure_test>"
    "${COMDB2_SOURCE}")
