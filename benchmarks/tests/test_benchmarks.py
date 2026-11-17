"""Functional contract only: never assert absolute timing thresholds."""

import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

EXECUTABLE, FAILURE = sys.argv[1:3]
del sys.argv[1:3]
SIZES = (1000, 100000, 1000000)
OPERATIONS = ("Insert", "LookupHit", "LookupMiss", "Delete")
NAMES = {"PlHash_{}/{}".format(op, size) for op in OPERATIONS for size in SIZES}
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from noise_study import process_medians


def run(executable, *args):
    return subprocess.run([executable] + list(args), stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, universal_newlines=True, timeout=120)


class BenchmarkContract(unittest.TestCase):
    def test_noise_uses_process_medians_not_pooled_repetitions(self):
        median = dict(run_name="PlHash_LookupHit/1000", aggregate_name="median",
                      time_unit="ns", real_time=20, cpu_time=10)
        raw = dict(run_name="PlHash_LookupHit/1000", real_time=9999, cpu_time=9999)
        self.assertEqual(process_medians({"benchmarks": [raw, median]}),
                         {"PlHash_LookupHit/1000": {"real_time": 20, "cpu_time": 10}})
        for rows in ([raw], [median, median], [dict(median, time_unit="ms")],
                     [dict(median, cpu_time=float("nan"))],
                     [dict(median, error_occurred=True)]):
            with self.assertRaises(ValueError):
                process_medians({"benchmarks": rows})

    def test_names_and_filter(self):
        result = run(EXECUTABLE, "--benchmark_list_tests=true")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(set(result.stdout.splitlines()), NAMES)
        result = run(EXECUTABLE, "--benchmark_list_tests=true", "--benchmark_filter=^PlHash_LookupHit/")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(set(result.stdout.splitlines()),
                         {"PlHash_LookupHit/{}".format(size) for size in SIZES})

    def test_json_and_repetitions(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result.json"
            result = run(EXECUTABLE, "--benchmark_min_time=0.001",
                         "--benchmark_repetitions=3", "--benchmark_out=" + str(path),
                         "--benchmark_out_format=json")
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(path.read_text())
        self.assertEqual(data["context"]["policy"], "REPORT_ONLY / NOT_QUALIFIED")
        self.assertEqual(len(data["context"]["comdb2_sha"]), 40)
        rows = data["benchmarks"]
        self.assertFalse(any(row.get("error_occurred") for row in rows))
        raw = [row for row in rows if row["run_type"] == "iteration"]
        self.assertEqual(len(raw), len(NAMES) * 3)
        self.assertEqual({row["name"] for row in raw}, NAMES)
        for row in raw:
            self.assertEqual(row["time_unit"], "ns")
            self.assertGreater(row["iterations"], 0)
            for key in ("real_time", "cpu_time", "items_per_second"):
                self.assertTrue(math.isfinite(row[key]) and row[key] > 0, row)
        medians = [row for row in rows if row.get("aggregate_name") == "median"]
        self.assertEqual(len(medians), len(NAMES))

    def test_failures_are_nonzero(self):
        for executable, args, message in (
            (EXECUTABLE, ["--not-a-real-flag"], "unrecognized"),
            (EXECUTABLE, ["--benchmark_filter=^NoSuchBenchmark$"], "match"),
            (FAILURE, ["--benchmark_filter=^InvalidFixture$"], "fixture size"),
            (FAILURE, ["--benchmark_filter=^ReportedFailure$"], "deliberate setup failure"),
        ):
            with self.subTest(args=args):
                result = run(executable, *args)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, (result.stdout + result.stderr).lower())


if __name__ == "__main__":
    unittest.main()
