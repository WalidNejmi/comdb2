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
NAMES |= {"CRC32C_{}/{}".format(op, size) for op in ("Comdb2", "Software")
          for size in (64, 256, 4096, 65536)}
NAMES |= {"RLE_{}/{}/{}".format(op, shape, size) for op in ("Compress", "Decompress")
          for shape in ("Zeros", "Repeated8", "Mixed", "Incompressible")
          for size in (256, 4096, 65536)}
NAMES |= {"Buffer_{}/{}".format(op, size) for op in ("Put", "Get") for size in (4, 8, 16, 64, 256)}
NAMES |= {"Buffer_{}/{}".format(op, size) for op in ("NoNetPut", "NoNetGet") for size in (16, 64, 256)}
COMMIT_MAP = "commit_map:" in json.loads((Path(EXECUTABLE).parent / "build-info.json").read_text())["component_flags"]
COMMIT_NAMES = {"CommitMap_LookupHit/1024", "CommitMap_LookupMiss/1024", "CommitMap_InsertBatch",
                "CommitMap_InsertGrow", "CommitMap_PurgeLogfile", "CommitMap_PurgeShrink"}
if COMMIT_MAP:
    NAMES |= COMMIT_NAMES
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from noise_study import process_medians
from compare_revisions import validate_builds, summarize_pairs, observation


def run(executable, *args):
    return subprocess.run([executable] + list(args), stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, universal_newlines=True, timeout=120)


class BenchmarkContract(unittest.TestCase):
    @unittest.skipUnless(COMMIT_MAP, "optional commit-map target disabled")
    def test_commit_map_production_paths_and_invariants(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "commit-map.json"
            result = run(EXECUTABLE, "--benchmark_filter=^CommitMap_", "--benchmark_min_time=0.001",
                         "--benchmark_repetitions=2", "--benchmark_out=" + str(path), "--benchmark_out_format=json")
            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(path.read_text())
        rows = [row for row in data["benchmarks"] if row["run_type"] == "iteration"]
        self.assertEqual({row["run_name"] for row in rows}, COMMIT_NAMES)
        self.assertEqual(len(rows), 12)
        self.assertIn("commit_map: comdb2ma/dlmalloc/MT_SAFE", data["context"]["allocator"])
        for row in rows:
            self.assertFalse(row.get("error_occurred"))
            self.assertGreater(row["items_per_second"], 0)
            name = row["run_name"]
            if "Lookup" in name:
                self.assertEqual((row["entries"], row["buckets"]), (1024, 1024))
            elif name == "CommitMap_InsertBatch":
                self.assertEqual(row["inserts_per_iteration"], 128)
            elif name == "CommitMap_InsertGrow":
                self.assertEqual((row["entries_rehashed"], row["resizes_per_op"]), (1025, 1))
            else:
                shrink = name == "CommitMap_PurgeShrink"
                self.assertEqual(row["entries_purged_per_iteration"], 1024)
                self.assertEqual(row["survivors"], 1 if shrink else 1024)
                self.assertEqual(row["resizes_per_op"], 1 if shrink else 0)

    def test_comparison_recomputes_median_from_raw_repetitions(self):
        info = json.loads((Path(EXECUTABLE).parent / "build-info.json").read_text())
        context = dict(info, source_dirty="false", harness_dirty="false")
        rows = [dict(run_name="case", run_type="iteration", repetition_index=i,
                     repetitions=5, cpu_time=i + 1, real_time=2 * (i + 1), time_unit="ns")
                for i in range(5)]
        aggregate = dict(run_name="case", run_type="aggregate", aggregate_name="median",
                         time_unit="ns", cpu_time=3, real_time=6)
        document = {"context": context, "benchmarks": rows + [aggregate]}
        self.assertEqual(observation(document, info)["case"], {"cpu_time": 3, "real_time": 6})
        for field, value in (("cpu_time", 99), ("cpu_time", float("nan")),
                             ("time_unit", "ms"), ("repetition_index", 4)):
            corrupt = json.loads(json.dumps(document))
            corrupt["benchmarks"][2][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                observation(corrupt, info)

    def test_comparison_stops_and_journals_artifact_mutation(self):
        info = json.loads((Path(EXECUTABLE).parent / "build-info.json").read_text())
        script = Path(__file__).resolve().parents[1] / "compare_revisions.py"
        for target in ("executable", "build-info.json"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                for side in ("base", "candidate"):
                    folder = root / side
                    folder.mkdir()
                    executable = folder / "executable"
                    executable.write_text(
                        "#!" + sys.executable + "\n"
                        "from pathlib import Path\n"
                        "Path(__file__).with_name(" + repr(target) + ").write_text('mutated')\n")
                    executable.chmod(0o700)
                    (folder / "build-info.json").write_text(json.dumps(
                        dict(info, source_dirty=False, harness_dirty=False)))
                result = subprocess.run(
                    [sys.executable, str(script), str(root / "base/executable"),
                     str(root / "candidate/executable"), str(root / "output")],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
                self.assertNotEqual(result.returncode, 0)
                message = "executable changed" if target == "executable" else "build metadata changed"
                self.assertIn(message, result.stderr)
                journal = json.loads((root / "output/campaign.json").read_text())
                self.assertEqual(journal["status"], "FAILED")
                self.assertEqual(len(journal["processes"]), 1)
                self.assertEqual(journal["processes"][0]["status"], "FAILED")
                self.assertFalse((root / "output/summary.json").exists())

    def test_comparison_journals_complete_balanced_campaign(self):
        info = json.loads((Path(EXECUTABLE).parent / "build-info.json").read_text())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "executable"
            executable.write_text(
                "#!" + sys.executable + "\n"
                "import json, sys\n"
                "from pathlib import Path\n"
                "context = json.loads(Path(__file__).with_name('build-info.json').read_text())\n"
                "context.update(source_dirty='false', harness_dirty='false')\n"
                "rows = [dict(run_name='case', run_type='iteration', repetition_index=i,\n"
                "             repetitions=5, cpu_time=10, real_time=20, time_unit='ns') for i in range(5)]\n"
                "rows.append(dict(run_name='case', run_type='aggregate', aggregate_name='median',\n"
                "                 cpu_time=10, real_time=20, time_unit='ns'))\n"
                "context.update(host_name='fixture', num_cpus=1, caches=[])\n"
                "output = next(a.split('=',1)[1] for a in sys.argv if a.startswith('--benchmark_out='))\n"
                "Path(output).write_text(json.dumps(dict(context=context, benchmarks=rows)))\n")
            executable.chmod(0o700)
            (root / "build-info.json").write_text(json.dumps(
                dict(info, source_dirty=False, harness_dirty=False)))
            result = subprocess.run(
                [sys.executable, str(Path(__file__).resolve().parents[1] / "compare_revisions.py"),
                 str(executable), str(executable), str(root / "output")],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads((root / "output/summary.json").read_text())
            self.assertEqual(summary, json.loads((root / "output/campaign.json").read_text()))
            self.assertEqual(summary["status"], "COMPLETE")
            self.assertEqual([p["side"] for p in summary["processes"]],
                             ["base", "candidate", "candidate", "base"] * 2)
            self.assertTrue(all(p["status"] == "COMPLETE" and len(p["result_sha256"]) == 64
                                for p in summary["processes"]))
            self.assertEqual(summary["benchmarks"]["case"]["cpu_time"]["paired_delta_percent"], [0] * 4)

    def test_comparison_identity_and_pair_hierarchy(self):
        info = json.loads((Path(EXECUTABLE).parent / "build-info.json").read_text())
        clean = dict(info, source_dirty=False, harness_dirty=False)
        validate_builds(clean, dict(clean, comdb2_sha="b" * 40))
        for field in ("benchmark_definition_sha256", "build_recipe_sha256", "c_flags",
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

    def test_comparison_refuses_definition_change_before_execution(self):
        info = json.loads((Path(EXECUTABLE).parent / "build-info.json").read_text())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for side in ("base", "candidate"):
                folder = root / side
                folder.mkdir()
                (folder / "executable").write_text("must not execute")
                metadata = dict(info, source_dirty=False, harness_dirty=False)
                if side == "candidate":
                    metadata["benchmark_definition_sha256"] = "0" * 64
                (folder / "build-info.json").write_text(json.dumps(metadata))
            result = subprocess.run(
                [sys.executable, str(Path(__file__).resolve().parents[1] / "compare_revisions.py"),
                 str(root / "base/executable"), str(root / "candidate/executable"), str(root / "output")],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("incomparable build field: benchmark_definition_sha256", result.stderr)
            self.assertFalse((root / "output").exists())

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
        for family in ("Pool_", "CRC32C_", "RLE_", "Buffer_"):
            result = run(EXECUTABLE, "--benchmark_list_tests=true", "--benchmark_filter=^" + family)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(set(result.stdout.splitlines()), {n for n in NAMES if n.startswith(family)})

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
            if row["name"].startswith(("CRC32C_", "RLE_", "Buffer_")):
                self.assertGreater(row["bytes_per_second"], 0)
            if row["name"].startswith("RLE_"):
                self.assertGreater(row["bytes_out"], 0)
                self.assertGreater(row["compressed_over_original"], 0)
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
