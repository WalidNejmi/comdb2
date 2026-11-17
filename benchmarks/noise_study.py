"""Characterize same-binary process variation without assigning qualification."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess


def process_medians(document):
    rows = document["benchmarks"]
    if any(row.get("error_occurred") for row in rows):
        raise ValueError("benchmark process reported an error")
    result = {}
    for row in rows:
        if row.get("aggregate_name") != "median":
            continue
        if row["time_unit"] != "ns":
            raise ValueError("noise study expects nanoseconds")
        name = row["run_name"]
        if name in result:
            raise ValueError("duplicate process median: " + name)
        values = {key: row[key] for key in ("real_time", "cpu_time")}
        if not all(math.isfinite(value) and value > 0 for value in values.values()):
            raise ValueError("invalid process median: " + name)
        result[name] = values
    if not result:
        raise ValueError("no process medians")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--cpu", type=int)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    if args.cpu is not None and (not hasattr(os, "sched_getaffinity") or
                                 args.cpu not in os.sched_getaffinity(0)):
        parser.error("--cpu must be in this process's Linux affinity mask")
    args.output.mkdir(parents=True, exist_ok=False)
    binary_hash = hashlib.sha256(executable.read_bytes()).hexdigest()
    modes = ["unpinned"] if args.cpu is None else ["unpinned", "pinned"]
    observations = {mode: [] for mode in modes}
    commands = []
    for number in range(5):
        # Alternate mode order to avoid always placing one mode first.
        for mode in modes if number % 2 == 0 else reversed(modes):
            path = args.output / "{}-{}.json".format(mode, number + 1)
            command = [str(executable), "--benchmark_filter=^PlHash_",
                       "--benchmark_min_time=0.05", "--benchmark_repetitions=5",
                       "--benchmark_display_aggregates_only=true",
                       "--benchmark_out=" + str(path.resolve()), "--benchmark_out_format=json"]
            if mode == "pinned":
                command = ["taskset", "-c", str(args.cpu)] + command
            commands.append(command)
            with path.with_suffix(".log").open("w") as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                               check=True, timeout=300)
            observations[mode].append(process_medians(json.loads(path.read_text())))
    if hashlib.sha256(executable.read_bytes()).hexdigest() != binary_hash:
        raise ValueError("executable changed during study")
    names = set(observations["unpinned"][0])
    summary = {"policy": "REPORT_ONLY / NOT_QUALIFIED",
               "executable_sha256": binary_hash, "cpu": args.cpu,
               "processes_per_mode": 5, "repetitions_per_process": 5,
               "commands": commands, "modes": {}}
    for mode, processes in observations.items():
        if any(set(process) != names for process in processes):
            raise ValueError("benchmark set changed between processes")
        summary["modes"][mode] = {}
        for name in sorted(names):
            metrics = {}
            for metric in ("real_time", "cpu_time"):
                values = [process[name][metric] for process in processes]
                metrics[metric + "_ns"] = {
                    "process_medians": values,
                    "median": statistics.median(values),
                    "sample_stddev": statistics.stdev(values),
                    "cv_percent": statistics.stdev(values) / statistics.mean(values) * 100,
                    "max_relative_difference_percent": (max(values) / min(values) - 1) * 100,
                }
            summary["modes"][mode][name] = metrics
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(args.output / "summary.json")


if __name__ == "__main__":
    main()
