"""Build-time provenance for the standalone benchmark executable."""

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess


def capture(args):
    return subprocess.check_output(args, universal_newlines=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "harness", "output", "c-compiler", "cxx-compiler", "c-flags",
                 "cxx-flags", "benchmark-version", "benchmark-dir", "benchmark-library",
                 "component-flags"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    root = Path(args.source)
    harness = Path(args.harness)
    definition_files = sorted(
        p for p in harness.rglob("*")
        if p.is_file() and "build" not in p.relative_to(harness).parts
        and (p.suffix in (".c", ".cpp", ".h", ".py") or p.name in ("CMakeLists.txt", "Makefile"))
        and "__pycache__" not in p.parts)
    digest = hashlib.sha256()
    for path in definition_files:
        digest.update(str(path.relative_to(harness)).encode() + b"\0" + path.read_bytes() + b"\0")
    output = Path(args.output).resolve()
    commands = json.loads((output / "compile_commands.json").read_text())
    recipe = []
    for command in commands:
        normalized = command["command"]
        for path, label in ((output, "<BUILD>"), (harness.resolve(), "<HARNESS>"),
                            (root.resolve(), "<PRODUCTION>")):
            normalized = normalized.replace(str(path), label)
        recipe.append(shlex.split(normalized))
    link = (output / "CMakeFiles/comdb2_microbenchmarks.dir/link.txt").read_text()
    for path, label in ((output, "<BUILD>"), (harness.resolve(), "<HARNESS>"),
                        (root.resolve(), "<PRODUCTION>")):
        link = link.replace(str(path), label)
    info = {
        "comdb2_sha": capture(["git", "-C", str(root), "rev-parse", "HEAD"]),
        "source_dirty": bool(capture(["git", "-C", str(root), "status", "--porcelain"])),
        "production_source_root": str(root.resolve()),
        "harness_sha": capture(["git", "-C", str(harness), "rev-parse", "HEAD"]),
        "harness_dirty": bool(capture(["git", "-C", str(harness), "status", "--porcelain"])),
        "benchmark_definition_sha256": digest.hexdigest(),
        "build_recipe_sha256": hashlib.sha256(
            json.dumps([recipe, shlex.split(link)], sort_keys=True).encode()).hexdigest(),
        "c_compiler": args.c_compiler,
        "c_compiler_version": capture([args.c_compiler, "--version"]).splitlines()[0],
        "cxx_compiler": args.cxx_compiler,
        "cxx_compiler_version": capture([args.cxx_compiler, "--version"]).splitlines()[0],
        "c_flags": args.c_flags,
        "cxx_flags": args.cxx_flags,
        "component_flags": args.component_flags,
        "google_benchmark_version": args.benchmark_version,
        "google_benchmark_cmake_dir": args.benchmark_dir,
        "google_benchmark_library_sha256": hashlib.sha256(Path(args.benchmark_library).read_bytes()).hexdigest(),
        "c_compiler_sha256": hashlib.sha256(Path(args.c_compiler).read_bytes()).hexdigest(),
        "cxx_compiler_sha256": hashlib.sha256(Path(args.cxx_compiler).read_bytes()).hexdigest(),
        "allocator": "BUILDING_TOOLS/system",
    }
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
