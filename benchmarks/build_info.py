"""Build-time provenance for the standalone benchmark executable."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def capture(args):
    return subprocess.check_output(args, universal_newlines=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "output", "c-compiler", "cxx-compiler", "c-flags",
                 "cxx-flags", "benchmark-version", "benchmark-dir"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    root = Path(args.source)
    definition_files = [
        "benchmarks/CMakeLists.txt", "benchmarks/Makefile",
        "benchmarks/benchmark_main.cpp", "benchmarks/plhash_bench.cpp",
        "benchmarks/plhash_fixture.h", "benchmarks/build_info.py",
    ]
    digest = hashlib.sha256()
    for name in definition_files:
        digest.update(name.encode() + b"\0" + (root / name).read_bytes() + b"\0")
    info = {
        "comdb2_sha": capture(["git", "-C", str(root), "rev-parse", "HEAD"]),
        "source_dirty": bool(capture(["git", "-C", str(root), "status", "--porcelain"])),
        "benchmark_definition_sha256": digest.hexdigest(),
        "c_compiler": args.c_compiler,
        "c_compiler_version": capture([args.c_compiler, "--version"]).splitlines()[0],
        "cxx_compiler": args.cxx_compiler,
        "cxx_compiler_version": capture([args.cxx_compiler, "--version"]).splitlines()[0],
        "c_flags": args.c_flags,
        "cxx_flags": args.cxx_flags,
        "google_benchmark_version": args.benchmark_version,
        "google_benchmark_cmake_dir": args.benchmark_dir,
        "allocator": "BUILDING_TOOLS/system",
    }
    output = Path(args.output)
    header = "#pragma once\n"
    for key, value in sorted(info.items()):
        text = str(value).lower() if isinstance(value, bool) else value
        header += "#define MICROBENCH_{} {}\n".format(key.upper(), json.dumps(text))
    for path, text in ((output / "include/build_info.h", header),
                       (output / "build-info.json", json.dumps(info, indent=2) + "\n")):
        if not path.exists() or path.read_text() != text:
            path.write_text(text)


if __name__ == "__main__":
    main()
