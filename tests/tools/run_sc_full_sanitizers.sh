#!/usr/bin/env bash
set -euo pipefail

root=$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)
build=${SC_SANITIZER_BUILDDIR:-$root/build.sc-sanitizers}
jobs=${SC_BUILD_JOBS:-8}

san_flags='-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all'
cmake -S "$root" -B "$build" \
	-DCMAKE_BUILD_TYPE=Debug \
	-DCOMDB2_TEST=ON \
	-DCMAKE_C_FLAGS="$san_flags" \
	-DCMAKE_CXX_FLAGS="$san_flags" \
	-DCMAKE_EXE_LINKER_FLAGS="$san_flags" \
	-DCMAKE_SHARED_LINKER_FLAGS="$san_flags"
cmake --build "$build" --target comdb2 cdb2sql comdb2ar \
	txn_commit_flags_sanitizer sc_publication_fence_codec_sanitizer -j"$jobs"

ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} \
UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1} \
"$build/tests/tools/txn_commit_flags_sanitizer"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} \
UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1} \
"$build/tests/tools/sc_publication_fence_codec_sanitizer"

echo "Full sanitizer build and focused decoder tests passed: $build"