"""Four balanced process pairs with one frozen harness; no performance verdict."""

import argparse
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess

from noise_study import process_medians


def validate_builds(base, candidate):
    if base.get("support_libraries", {}) != candidate.get("support_libraries", {}):
        raise ValueError("incomparable build field: support_libraries")
    for info in (base, candidate):
        if info["source_dirty"] is not False or info["harness_dirty"] is not False:
            raise ValueError("comparison requires clean production and harness checkouts")
    for key in ("benchmark_definition_sha256", "build_recipe_sha256", "harness_sha", "c_compiler",
                "cxx_compiler", "c_compiler_version", "cxx_compiler_version",
                "c_compiler_sha256", "cxx_compiler_sha256", "c_flags", "cxx_flags",
                "component_flags", "google_benchmark_version", "google_benchmark_library_sha256", "allocator"):
        if base[key] != candidate[key]:
            raise ValueError("incomparable build field: " + key)


def summarize_pairs(pairs):
    names = set(pairs[0]["base"])
    if not names or any(set(pair[side]) != names for pair in pairs
                        for side in ("base", "candidate")):
        raise ValueError("benchmark set changed between paired processes")
    summary = {}
    for name in sorted(names):
        summary[name] = {}
        keys = set(pairs[0]["base"][name])
        if any(set(pair[side][name]) != keys for pair in pairs for side in ("base", "candidate")):
            raise ValueError("metric set changed between paired processes: " + name)
        for metric in pairs[0]["base"][name]:
            base = [pair["base"][name][metric] for pair in pairs]
            candidate = [pair["candidate"][name][metric] for pair in pairs]
            if not all(math.isfinite(v) and v > 0 for v in base + candidate):
                raise ValueError("nonpositive/nonfinite comparison metric")
            deltas = [(b - a) / a * 100 for a, b in zip(base, candidate)]
            summary[name][metric] = {
                "base_process_medians": base, "candidate_process_medians": candidate,
                "base_median": statistics.median(base),
                "candidate_median": statistics.median(candidate),
                "paired_delta_percent": deltas,
                "median_delta_percent": statistics.median(deltas),
                "min_delta_percent": min(deltas), "max_delta_percent": max(deltas),
            }
    return summary


def observation(document, info):
    context = document["context"]
    for key in ("comdb2_sha", "harness_sha", "benchmark_definition_sha256",
                "c_flags", "cxx_flags", "component_flags", "google_benchmark_version"):
        if context[key] != info[key]:
            raise ValueError("binary/build metadata mismatch: " + key)
    if context["source_dirty"] != "false" or context["harness_dirty"] != "false":
        raise ValueError("dirty executable provenance")
    medians = process_medians(document)
    raw = {}
    for row in document["benchmarks"]:
        if row.get("run_type") == "iteration":
            raw.setdefault(row["run_name"], []).append(row)
    if set(raw) != set(medians):
        raise ValueError("raw/median benchmark sets differ")
    for name, rows in raw.items():
        if (len(rows) != 5 or {r["repetition_index"] for r in rows} != set(range(5))
                or any(r["repetitions"] != 5 or r["time_unit"] != "ns" for r in rows)):
            raise ValueError("expected five raw repetitions: " + name)
        for key in ("cpu_time", "real_time"):
            values = [row[key] for row in rows]
            if not all(math.isfinite(value) and value > 0 for value in values):
                raise ValueError("invalid raw repetition: " + name)
            if not math.isclose(statistics.median(values), medians[name][key], rel_tol=1e-9):
                raise ValueError("reported median differs from raw repetitions: " + name)
    for row in document["benchmarks"]:
        if row.get("aggregate_name") == "median":
            for key in ("items_per_second", "bytes_per_second"):
                if key in row:
                    medians[row["run_name"]][key] = row[key]
    return medians


def fingerprint(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_artifacts(executables, metadata, fingerprints, metadata_hashes):
    for side in executables:
        if fingerprint(executables[side]) != fingerprints[side]:
            raise ValueError("executable changed during comparison: " + side)
        if fingerprint(metadata[side]) != metadata_hashes[side]:
            raise ValueError("build metadata changed during comparison: " + side)


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--filter", default="^PlHash_")
    args = parser.parse_args()
    executables = {side: getattr(args, side).resolve(strict=True)
                   for side in ("base", "candidate")}
    metadata = {side: exe.parent / "build-info.json" for side, exe in executables.items()}
    infos = {side: json.loads(path.read_text()) for side, path in metadata.items()}
    validate_builds(infos["base"], infos["candidate"])
    fingerprints = {side: fingerprint(exe) for side, exe in executables.items()}
    metadata_hashes = {side: fingerprint(path) for side, path in metadata.items()}
    args.output.mkdir(parents=True, exist_ok=False)
    summary = {"policy": "REPORT_ONLY / NOT_QUALIFIED", "builds": infos,
               "binary_sha256": fingerprints, "pairs": 4, "repetitions": 5,
               "build_info_sha256": metadata_hashes,
               "filter": args.filter, "time_unit": "ns",
               "affinity": sorted(os.sched_getaffinity(0)),
               "started_at": utc(), "status": "RUNNING",
               "commands": [], "processes": []}
    journal = args.output / "campaign.json"

    def save_journal():
        temporary = journal.with_suffix(".tmp")
        temporary.write_text(json.dumps(summary, indent=2) + "\n")
        temporary.replace(journal)

    save_journal()
    pairs = []
    host = None
    for number in range(4):
        pair = {}
        for side in (("base", "candidate") if number % 2 == 0 else ("candidate", "base")):
            path = args.output.resolve() / "{}-{}.json".format(side, number + 1)
            command = [str(executables[side]), "--benchmark_filter=" + args.filter,
                       "--benchmark_min_time=0.05", "--benchmark_repetitions=5",
                       "--benchmark_display_aggregates_only=true",
                       "--benchmark_out=" + str(path), "--benchmark_out_format=json"]
            summary["commands"].append(command)
            record = {"pair": number + 1, "side": side, "started_at": utc(),
                      "status": "RUNNING", "load_before": os.getloadavg(),
                      "affinity": sorted(os.sched_getaffinity(0))}
            summary["processes"].append(record)
            save_journal()
            try:
                verify_artifacts(executables, metadata, fingerprints, metadata_hashes)
                if record["affinity"] != summary["affinity"]:
                    raise ValueError("process launch affinity changed")
                with path.with_suffix(".log").open("w") as log:
                    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                   check=True, timeout=1800)
                verify_artifacts(executables, metadata, fingerprints, metadata_hashes)
                document = json.loads(path.read_text())
                machine = {k: document["context"][k] for k in ("host_name", "num_cpus", "caches")}
                if host is not None and host != machine:
                    raise ValueError("host/CPU context changed")
                host = machine
                pair[side] = observation(document, infos[side])
                record["result_sha256"] = fingerprint(path)
                record["status"] = "COMPLETE"
            except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
                record["status"] = summary["status"] = "FAILED"
                record["error"] = str(error)
                raise
            finally:
                record["finished_at"] = utc()
                record["load_after"] = os.getloadavg()
                save_journal()
        pairs.append(pair)
    try:
        summary["host"] = host
        summary["benchmarks"] = summarize_pairs(pairs)
        verify_artifacts(executables, metadata, fingerprints, metadata_hashes)
        summary["status"] = "COMPLETE"
    except (OSError, ValueError, KeyError, TypeError) as error:
        summary["status"] = "FAILED"
        summary["error"] = str(error)
        raise
    finally:
        summary["finished_at"] = utc()
        save_journal()
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(args.output / "summary.json")


if __name__ == "__main__":
    main()
