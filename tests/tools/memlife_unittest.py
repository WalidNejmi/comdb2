#!/usr/bin/env python3
"""Unit tests of memlife_compare.py (classifier, retention, snapshot validation).
Run: python3 tests/tools/memlife_unittest.py   (no database needed)"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
import memlife_compare as mc  # noqa: E402

K = 1024
BASE = 10 * 1024 * K


def rjson(path):
    with open(path) as fh:
        return json.load(fh)


def wjson(obj, path):
    with open(path, "w") as fh:
        json.dump(obj, fh)


def rtext(path):
    with open(path) as fh:
        return fh.read()


def cls(series, spread=0, base=BASE):
    return mc.classify(base, spread, series)[0]


class Classifier(unittest.TestCase):
    def test_flat(self):
        self.assertEqual(cls([BASE] * 8), "STABLE")

    def test_noisy_flat(self):
        noise = [20, -30, 25, -10, 30, -25, 15, -20]
        self.assertEqual(cls([BASE + x * K for x in noise], spread=20 * K), "STABLE")

    def test_one_early_jump_then_flat(self):
        self.assertEqual(cls([BASE + 500 * K] * 8), "ONE_TIME_GROWTH")

    def test_one_late_jump_then_flat(self):
        self.assertEqual(cls([BASE] * 5 + [BASE + 500 * K] * 3), "ONE_TIME_GROWTH")

    def test_short_ramp_then_plateau(self):
        s = [BASE + 100 * K * min(k, 3) for k in range(1, 9)]
        self.assertEqual(cls(s), "PLATEAU")

    def test_long_ramp_then_plateau(self):   # seqnum averager: 94 KiB steps for 14 iterations of 20
        s = [BASE + 94 * K * min(k, 15) for k in range(1, 21)]
        self.assertEqual(cls(s), "PLATEAU")

    def test_long_ramp_too_short_run(self):   # the same ramp seen in 8 iterations is growth
        s = [BASE + 94 * K * k for k in range(1, 9)]
        self.assertEqual(cls(s), "STRONG_GROWTH")

    def test_linear_growth(self):
        self.assertEqual(cls([BASE + 737 * K * k for k in range(1, 9)]), "STRONG_GROWTH")

    def test_slow_linear_growth(self):   # below band per step, consistent, beyond band overall
        self.assertEqual(cls([BASE + 20 * K * k for k in range(1, 9)]), "SUSPICIOUS_GROWTH")

    def test_two_late_jumps(self):
        self.assertEqual(cls([BASE] * 5 + [BASE + 300 * K, BASE + 600 * K, BASE + 900 * K]), "SUSPICIOUS_GROWTH")

    def test_decrease(self):
        self.assertEqual(cls([BASE - 100 * K * k for k in range(1, 9)]), "STABLE")

    def test_too_few_iterations(self):
        self.assertEqual(cls([BASE, BASE]), "INCONCLUSIVE")


class Retention(unittest.TestCase):
    def check(self, series, reclaims):
        c = cls(series)
        return mc.retention_check(c, BASE, mc.MIN_BAND, series, reclaims, "test")

    def test_reclaim_to_baseline(self):
        c, note, verified = self.check([BASE + 300 * K * k for k in range(1, 9)], [BASE - 200 * K])
        self.assertEqual(c, "RETENTION_BOUNDED")
        self.assertTrue(verified)

    def test_partial_reclaim_leaves_leak(self):   # +100 MB, reclaim gives back 50 MB
        c, note, _ = self.check([BASE + 12800 * K * k for k in range(1, 9)], [BASE + 51200 * K])
        self.assertEqual(c, "STRONG_GROWTH")
        self.assertIn("exceeds tolerance", note)

    def test_cycles_with_rising_floor(self):
        s = [BASE + 300 * K, BASE + 600 * K, BASE + 900 * K] * 3
        c, note, _ = self.check(s, [BASE + 10 * K, BASE + 60 * K, BASE + 200 * K])
        self.assertIn(c, ("SUSPICIOUS_GROWTH", "STRONG_GROWTH"))
        self.assertIn("floor rises", note)

    def test_cycles_bounded(self):
        s = [BASE + 300 * K, BASE + 600 * K, BASE + 900 * K] * 3
        c, _, _ = self.check(s, [BASE + 10 * K, BASE + 20 * K, BASE + 5 * K])
        self.assertEqual(c, "RETENTION_BOUNDED")

    def test_no_reclaim(self):
        c, note, verified = self.check([BASE + 300 * K * k for k in range(1, 9)], [])
        self.assertEqual(c, "STRONG_GROWTH")
        self.assertFalse(verified)
        self.assertIn("not verified", note)


def write_fixture(d, nodes, iters, values, reclaims=0, retention=None, skip=(), dup=(), drop_alloc=(), extra=(),
                  execution="WORKLOAD_PASS"):
    """values(host, label, alloc) -> used. skip/dup: (host, label); drop_alloc: (host, label, alloc);
    extra: labels written after the expected ones."""
    os.makedirs(os.path.join(d, "snapshots"))
    md = {"test": "fixture", "nodes": nodes, "iterations": iters, "reclaim_snapshots": reclaims,
          "retention": retention or {}, "cluster": nodes if len(nodes) > 1 else []}
    wjson(md, os.path.join(d, "metadata.json"))
    wjson({"execution_status": execution, "detail": ""}, os.path.join(d, "execution.json"))
    labels = ["A.1", "A.2", "A.3"] + ["I%d" % k for k in range(1, iters + 1)] + \
             ["R%d" % j for j in range(1, reclaims + 1)] + list(extra)
    ts = 1000.0
    with open(os.path.join(d, "snapshots", "snapshots.tsv"), "w") as fh:
        for label in labels:
            for host in nodes:
                if (host, label) in skip:
                    continue
                for _ in range(2 if (host, label) in dup else 1):
                    ts += 1
                    for alloc in ("berkdb", "commit_map", "util"):
                        if (host, label, alloc) in drop_alloc:
                            continue
                        u = values(host, label, alloc)
                        fh.write("\t".join(str(x) for x in (label, "%.3f" % ts, host, 123, 4567, "master", alloc,
                                                              u, u * 2, u, u)) + "\n")


def flat(host, label, alloc):
    return BASE


def leak_berkdb(host, label, alloc):
    if alloc == "berkdb" and label.startswith("I"):
        return BASE + 500 * K * int(label[1:])
    return BASE


def commit_map_reclaimed(host, label, alloc):
    if alloc == "commit_map" and label.startswith("I"):
        return BASE + 300 * K * int(label[1:])
    if alloc == "commit_map" and label.startswith("R"):
        return BASE - 100 * K
    return BASE


class Validation(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="memlife_ut")
        self.out = os.path.join(self.d, "art")

    def tearDown(self):
        shutil.rmtree(self.d)

    def run_compare(self):
        p = subprocess.run([sys.executable, os.path.join(HERE, "memlife_compare.py"), "--dir", self.out],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        summary = rtext(os.path.join(self.out, "summary.txt"))
        ex = rjson(os.path.join(self.out, "execution.json"))["execution_status"]
        return p.returncode, summary, ex

    def assertInvalid(self, needle, **kw):
        write_fixture(self.out, **kw)
        rc, summary, ex = self.run_compare()
        self.assertEqual(rc, 3, summary)
        self.assertIn("MEMORY: INCONCLUSIVE", summary)
        self.assertEqual(ex, "INCOMPLETE")
        self.assertIn(needle, summary)
        self.assertFalse(os.path.exists(os.path.join(self.out, "comparison.csv")))

    def test_valid_single_node(self):
        write_fixture(self.out, nodes=["n1"], iters=8, values=flat)
        rc, summary, ex = self.run_compare()
        self.assertEqual(rc, 0, summary)
        self.assertIn("EXECUTION: WORKLOAD_PASS", summary)
        self.assertIn("MEMORY: STABLE", summary)

    def test_valid_three_nodes_leak_on_one(self):
        write_fixture(self.out, nodes=["n1", "n2", "n3"], iters=8,
                      values=lambda h, l, a: leak_berkdb(h, l, a) if h == "n2" else BASE)
        rc, summary, _ = self.run_compare()
        self.assertEqual(rc, 0, summary)
        self.assertIn("MEMORY: STRONG_GROWTH", summary)
        self.assertIn("TOP ALLOCATOR: berkdb on n2", summary)
        self.assertIn("MEMLIFE_DEBUG_ALLOC=berkdb", summary)
        s = rjson(os.path.join(self.out, "summary.json"))
        self.assertEqual(s["nodes"]["n1"]["allocators"]["berkdb"]["classification"], "STABLE")

    def test_retention_verified(self):
        write_fixture(self.out, nodes=["n1"], iters=8, values=commit_map_reclaimed, reclaims=1,
                      retention={"commit_map": "log deletion"})
        rc, summary, _ = self.run_compare()
        self.assertEqual(rc, 0, summary)
        self.assertIn("MEMORY: RETENTION_BOUNDED", summary)

    def test_missing_A2(self):
        self.assertInvalid("missing snapshots A.2", nodes=["n1"], iters=8, values=flat, skip={("n1", "A.2")})

    def test_missing_I3(self):
        self.assertInvalid("missing snapshots I3", nodes=["n1"], iters=8, values=flat, skip={("n1", "I3")})

    def test_one_node_missing_I5(self):
        self.assertInvalid("n3: missing snapshots I5", nodes=["n1", "n2", "n3"], iters=8, values=flat,
                           skip={("n3", "I5")})

    def test_missing_reclaim(self):
        self.assertInvalid("missing snapshots R1", nodes=["n1"], iters=8, values=commit_map_reclaimed, reclaims=1,
                           retention={"commit_map": "x"}, skip={("n1", "R1")})

    def test_duplicate_snapshot(self):
        self.assertInvalid("snapshot I2 taken 2 times", nodes=["n1"], iters=8, values=flat, dup={("n1", "I2")})

    def test_missing_allocator(self):
        self.assertInvalid("allocators missing: util", nodes=["n1"], iters=8, values=flat,
                           drop_alloc={("n1", "I4", "util")})

    def test_wrong_node_set(self):
        write_fixture(self.out, nodes=["n1", "n2"], iters=8, values=flat)
        md = rjson(os.path.join(self.out, "metadata.json"))
        md["nodes"] = ["n1", "n3"]
        wjson(md, os.path.join(self.out, "metadata.json"))
        rc, summary, ex = self.run_compare()
        self.assertEqual(rc, 3)
        self.assertIn("unexpected node n2", summary)
        self.assertIn("n3: missing snapshots A.1", summary)

    def test_empty_memstats(self):
        write_fixture(self.out, nodes=["n1"], iters=8, values=flat)
        open(os.path.join(self.out, "snapshots", "snapshots.tsv"), "w").close()
        rc, summary, _ = self.run_compare()
        self.assertEqual(rc, 3)
        self.assertIn("missing snapshots A.1", summary)

    def test_server_restarted(self):
        write_fixture(self.out, nodes=["n1"], iters=8, values=flat)
        p = os.path.join(self.out, "snapshots", "snapshots.tsv")
        rows = [l.split("\t") for l in rtext(p).splitlines()]
        with open(p, "w") as fh:
            for r in rows:
                if r[0] in ("I5", "I6", "I7", "I8"):
                    r[3] = "999"
                fh.write("\t".join(r) + "\n")
        rc, summary, _ = self.run_compare()
        self.assertEqual(rc, 3)
        self.assertIn("server process changed", summary)

    def test_declared_restarted_node_not_classified(self):
        write_fixture(self.out, nodes=["n1", "n2"], iters=8,
                      values=lambda h, l, a: leak_berkdb(h, l, a) if h == "n2" else BASE)
        md = rjson(os.path.join(self.out, "metadata.json"))
        md["restarted_nodes"] = ["n2"]
        wjson(md, os.path.join(self.out, "metadata.json"))
        rc, summary, _ = self.run_compare()
        self.assertEqual(rc, 0, summary)
        self.assertIn("MEMORY: STABLE", summary)
        self.assertIn("restarted by the test", summary)

    def test_retention_without_reclaim_phase(self):
        self.assertInvalid("no reclaim phase", nodes=["n1"], iters=8, values=flat, retention={"commit_map": "x"})

    def test_workload_failure_has_no_verdict(self):
        write_fixture(self.out, nodes=["n1"], iters=8, values=leak_berkdb, execution="WORKLOAD_FAIL")
        rc, summary, ex = self.run_compare()
        self.assertEqual(rc, 3)
        self.assertEqual(ex, "WORKLOAD_FAIL")
        self.assertIn("MEMORY: INCONCLUSIVE", summary)
        self.assertNotIn("STRONG_GROWTH", summary)

    def test_labels_not_renumbered(self):   # I3 missing, I4..I9 present: never shifted into I3..I8
        self.assertInvalid("unexpected snapshots I9", nodes=["n1"], iters=8, values=flat, skip={("n1", "I3")},
                           extra=["I9"])



if __name__ == "__main__":
    unittest.main(verbosity=2)
