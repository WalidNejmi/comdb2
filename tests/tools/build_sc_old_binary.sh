#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(git -C "$script_dir" rev-parse --show-toplevel)
ref=${1:-upstream/main}
output=${2:-$root/build.sc-old/db/comdb2}
worktree=${SC_OLD_WORKTREE:-$root/.sc-old-source}
builddir=${SC_OLD_BUILDDIR:-$root/build.sc-old}

if git -C "$root" show "$ref:berkdb/dbinc_auto/txn_auto.h" |
   grep -q 'DB___txn_regop_flags'; then
	echo "FAIL: $ref already contains flag commit records" >&2
	exit 1
fi

rm -rf "$worktree"
mkdir -p "$worktree"
git -C "$root" archive "$ref" | tar -x -C "$worktree"
cmake -S "$worktree" -B "$builddir" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$builddir" --target comdb2 -j"${SC_BUILD_JOBS:-8}"

[[ -x "$output" ]] || {
	echo "FAIL: old comdb2 binary was not produced at $output" >&2
	exit 1
}

rm -rf "$worktree"
echo "$output"