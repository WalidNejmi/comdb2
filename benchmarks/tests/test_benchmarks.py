"""Functional contract only: never assert absolute timing thresholds."""

import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

EXECUTABLE, FAILURE, SOURCE = sys.argv[1:4]
del sys.argv[1:4]
SIZES = (1000, 100000, 1000000)
OPERATIONS = ("Insert", "LookupHit", "LookupMiss", "Delete", "LookupHitReadonly", "LookupMissReadonly")
NAMES = {"PlHash_{}/{}".format(op, size) for op in OPERATIONS for size in SIZES}
NAMES |= {"PlHash_{}_Chain{}".format(op, depth) for op in ("LookupHit", "LookupReadonly")
          for depth in (1, 4, 16)}
NAMES |= {"Pool_{}/{}".format(op, size) for op in ("Get", "GetZeroed", "Return", "GetReturn")
          for size in (32, 128, 512)}
NAMES |= {"Pool_Grow/entry:{}/step:{}".format(size, step)
          for size, step in ((32, 64), (128, 64), (512, 64), (128, 512))}
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from noise_study import process_medians
from compare_revisions import validate_builds, summarize_pairs


def run(executable, *args):
    return subprocess.run([executable] + list(args), stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, universal_newlines=True, timeout=120)


class BenchmarkContract(unittest.TestCase):
    def test_comparison_identity_and_pair_hierarchy(self):
        info = json.loads((Path(EXECUTABLE).parent / "build-info.json").read_text())
        clean = dict(info, source_dirty=False, harness_dirty=False)
        validate_builds(clean, dict(clean, comdb2_sha="b" * 40))
        for field in ("benchmark_definition_sha256", "c_flags",
                      "google_benchmark_library_sha256", "c_compiler_sha256"):
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_builds(clean, dict(clean, **{field: "different"}))
        with self.assertRaises(ValueError):
            validate_builds(clean, dict(clean, source_dirty=True))
        pairs = [
            {"base": {"x": {"cpu_time": 10, "real_time": 20}},
             "candidate": {"x": {"cpu_time": 11, "real_time": 18}}},
            {"base": {"x": {"cpu_time": 100, "real_time": 200}},
             "candidate": {"x": {"cpu_time": 90, "real_time": 220}}},
        ]
        result = summarize_pairs(pairs)["x"]
        self.assertAlmostEqual(result["cpu_time"]["median_delta_percent"], 0)
        self.assertAlmostEqual(result["real_time"]["median_delta_percent"], 0)
        self.assertEqual(result["cpu_time"]["base_process_medians"], [10, 100])
        pairs[1]["candidate"] = {}
        with self.assertRaises(ValueError):
            summarize_pairs(pairs)

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
        expected = subprocess.check_output(["git", "-C", SOURCE, "rev-parse", "HEAD"],
                                           universal_newlines=True).strip()
        self.assertEqual(data["context"]["comdb2_sha"], expected)
        info = json.loads((Path(EXECUTABLE).parent / "build-info.json").read_text())
        self.assertEqual(Path(info["production_source_root"]).resolve(), Path(SOURCE).resolve())
        self.assertEqual(data["context"]["benchmark_definition_sha256"],
                         info["benchmark_definition_sha256"])
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
            if "_Chain" in row["name"]:
                self.assertEqual(row["steps_per_op"], row["synthetic_chain_depth"] - 1)
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
