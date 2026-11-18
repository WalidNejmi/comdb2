# Compile production translation units, discarding unrelated database functions.
# Generate Berkeley DB headers with the same scripts used by its normal build.
set(berk "${COMDB2_SOURCE}/berkdb")
find_package(OpenSSL REQUIRED)
find_library(COMMIT_MAP_UNWIND_LIBRARY NAMES unwind)
if(NOT COMMIT_MAP_UNWIND_LIBRARY)
  message(FATAL_ERROR "Commit-map benchmarks require the production allocator's libunwind dependency")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dbinc_auto")
foreach(module berkdb util)
  string(TOUPPER "${module}" MODULE)
  configure_file("${COMDB2_SOURCE}/mem/mem.h.in" "include/mem_${module}.h" @ONLY)
endforeach()
set(commit_map_headers "")
foreach(directory mutex env common xa dbreg log lock mp btree db rep os qam hash fileops hmac crypto)
  file(GLOB sources CONFIGURE_DEPENDS "${berk}/${directory}/*.c")
  if(directory STREQUAL "db")
    list(APPEND sources "${CMAKE_CURRENT_BINARY_DIR}/db_auto.c")
  endif()
  set(header "${CMAKE_CURRENT_BINARY_DIR}/dbinc_auto/${directory}_ext.h")
  add_custom_command(OUTPUT "${header}"
    COMMAND bash dist/geninc.sh "${directory}" "${header}" ${sources}
    WORKING_DIRECTORY "${berk}"
    DEPENDS ${sources} "${berk}/dist/geninc.sh" "${berk}/dist/gen_inc.awk"
    VERBATIM)
  list(APPEND commit_map_headers "${header}")
endforeach()
foreach(record dbreg/dbreg db/db btree/btree fileops/fileops db/crdel qam/qam hash/hash)
  get_filename_component(name "${record}" NAME)
  set(header "${CMAKE_CURRENT_BINARY_DIR}/dbinc_auto/${name}_auto.h")
  add_custom_command(OUTPUT "${header}" "${CMAKE_CURRENT_BINARY_DIR}/${name}_auto.c"
    COMMAND bash dist/genrec.sh "${berk}/${record}.src"
      "${CMAKE_CURRENT_BINARY_DIR}/${name}_auto.c" "${header}"
      "${CMAKE_CURRENT_BINARY_DIR}/${name}_template"
    WORKING_DIRECTORY "${berk}"
    DEPENDS "${berk}/${record}.src" "${berk}/dist/genrec.sh" "${berk}/dist/gen_rec.awk"
    VERBATIM)
  list(APPEND commit_map_headers "${header}")
endforeach()
add_custom_target(commit_map_headers DEPENDS ${commit_map_headers})

add_library(microbench_commit_map STATIC
  fixtures/commit_map_adapter.c
  "${berk}/txn/txn_util.c"
  "${berk}/os/os_alloc.c"
  "${berk}/os/os_errno.c"
  "${berk}/common/db_err.c"
  "${berk}/env/env_method.c"
  "${COMDB2_SOURCE}/mem/mem.c"
  "${COMDB2_SOURCE}/dlmalloc/dlmalloc.c"
  "${COMDB2_SOURCE}/util/list.c"
  "${COMDB2_SOURCE}/util/comdb2_walkback.c"
)
add_dependencies(microbench_commit_map commit_map_headers)
target_include_directories(microbench_commit_map PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures" "${CMAKE_CURRENT_BINARY_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}/include"
  "${OPENSSL_INCLUDE_DIR}"
  "${berk}/build" "${berk}" "${COMDB2_SOURCE}/bdb" "${COMDB2_SOURCE}/bbinc"
  "${COMDB2_SOURCE}/mem" "${COMDB2_SOURCE}/dlmalloc" "${COMDB2_SOURCE}/util")
target_compile_definitions(microbench_commit_map PRIVATE _LINUX_SOURCE _GNU_SOURCE PER_THREAD_MALLOC)
target_compile_options(microbench_commit_map PRIVATE -fno-strict-aliasing -ffunction-sections -fdata-sections)
target_link_libraries(microbench_commit_map PUBLIC microbench_primitives Threads::Threads "${COMMIT_MAP_UNWIND_LIBRARY}")
target_link_options(microbench_commit_map INTERFACE -Wl,--gc-sections)
