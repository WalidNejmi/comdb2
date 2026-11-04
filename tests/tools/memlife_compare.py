#!/usr/bin/env python3
"""Memory-lifecycle suite: metadata, snapshot validation and classification.

memlife_compare.py --dir <artifact dir>
    Validate snapshots/snapshots.tsv against metadata.json and classify, per
    node and per allocator, the comdb2_memstats "used" values. Writes
    comparison.csv, summary.txt and summary.json; raw values are kept.
    Exit 0: classified. Exit 3: the snapshots are incomplete or inconsistent
    (execution status INCOMPLETE, no memory verdict).
memlife_compare.py --write-metadata <metadata.json>
    Write metadata.json from the environment set by memlife_common.sh.

Snapshots (labels are never renumbered): A.1 A.2 A.3 (baseline after the
warm-up; their spread is the noise), I1..IN (after each repetition),
R1..Rm (after each reclaim event, when the test has one). Every node in
metadata.json must have every label exactly once, every snapshot of a node
must contain the same allocators, and the server process of a node must not
change (a restart or crash would reset its memory) unless the test declares
the node in MEMLIFE_RESTARTED_NODES; such nodes are reported, not classified.

Per allocator, base = median(A), spread = max(A) - min(A),
band = max(64 KiB, 3 x spread), floor = max(4 KiB, 2 x spread),
d_k = I_k - I_(k-1) (I_0 = base), W = max(3, N // 4) (tail window),
tail_up = sum of the last W deltas > band, or every one of them > floor:
  INCONCLUSIVE       fewer than 3 iterations
  STRONG_GROWTH      at least 80% of d_2..d_N > band and I_N - I_1 > band
  ONE_TIME_GROWTH    exactly one d_k > band and the other steps together within band
  SUSPICIOUS_GROWTH  I_N - base > band, tail_up and at least two steps > floor
  PLATEAU            I_N - base > band and not tail_up
  STABLE             otherwise
Allocators declared in MEMLIFE_RETENTION ("allocator:reason") are checked
against the reclaim snapshots, never ignored: with peak = max(I) - base > band,
  RETENTION_BOUNDED  every R_j - base <= band (bounded residual) and, with
                     several reclaim cycles, R_m - R_1 <= band (floor not rising)
  rising floor       at least SUSPICIOUS_GROWTH
  residual > band    keeps its growth class, with a note.
"""
import argparse
import collections
import csv
import hashlib
import json
import os
import subprocess
import sys
import time

MIN_BAND = 64 * 1024
MIN_FLOOR = 4 * 1024
STRONG_FRACTION = 0.8
ORDER = ["STRONG_GROWTH", "SUSPICIOUS_GROWTH", "PLATEAU", "ONE_TIME_GROWTH", "RETENTION_BOUNDED", "STABLE",
         "INCONCLUSIVE"]
FIELDS = ["label", "ts", "host", "pid", "rss", "role", "name", "used", "total", "free", "peak"]


# ------------------------------------------------------------------ metadata
def sha256(path):
    try:
        h = hashlib.sha256()
        with open(path, "rb") as fh:
            for b in iter(lambda: fh.read(1 << 20), b""):
                h.update(b)
        return h.hexdigest()
    except (OSError, TypeError):
        return None


def run(cmd, cwd=None):
    try:
        return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                              universal_newlines=True, timeout=60).stdout.strip()
    except Exception:
        return ""


def cmake_cache(exe):
    """Selected entries of the CMakeCache.txt of the build that produced exe."""
    d = os.path.dirname(os.path.abspath(exe)) if exe else ""
    for _ in range(4):
        p = os.path.join(d, "CMakeCache.txt")
        if os.path.exists(p):
            want = ("COMDB2_PER_THREAD_MALLOC", "COMDB2_TEST", "CMAKE_BUILD_TYPE", "CMAKE_C_COMPILER",
                    "COMDB2_SANITIZER", "CMAKE_POSITION_INDEPENDENT_CODE")
            out = {"path": p}
            with open(p) as fh:
                for line in fh:
                    k = line.split(":", 1)[0]
                    if k in want and "=" in line:
                        out[k] = line.split("=", 1)[1].strip()
            return out
        d = os.path.dirname(d)
    return None


def write_metadata(path):
    e = os.environ
    exe = e.get("COMDB2_EXE", "")
    comment = run(["readelf", "-p", ".comment", exe]) if exe else ""
    elf = run(["readelf", "-h", exe]) if exe else ""
    src = e.get("MEMLIFE_SOURCE_SHA") or run(["git", "rev-parse", "HEAD"], cwd=e.get("TESTSROOTDIR") or None)
    iters, every = int(e["MEMLIFE_ITERS"]), int(e["MEMLIFE_RECLAIM_EVERY"])
    reclaims = (iters // every if every else 1) if e.get("MEMLIFE_RECLAIM") == "1" else 0
    lrl = e.get("MEMLIFE_LRL", "")
    lrl_lines = []
    if lrl and os.path.exists(lrl):
        with open(lrl) as fh:
            lrl_lines = [l.strip() for l in fh if l.strip()]
    md = {
        "format": 2,
        "test": e.get("TESTCASE"), "test_definition_sha256": sha256(e.get("MEMLIFE_RUNIT")),
        "database": e.get("MEMLIFE_DB"), "nodes": e.get("MEMLIFE_NODES", "").split(),
        "master_at_start": e.get("MEMLIFE_MASTER"), "cluster": e.get("CLUSTER", "").split(),
        "iterations": iters, "settle_seconds": float(e["MEMLIFE_SETTLE"]),
        "reclaim_every": every, "reclaim_snapshots": reclaims,
        "retention": parse_retention(e.get("MEMLIFE_RETENTION", "")),
        "restarted_nodes": e.get("MEMLIFE_RESTARTED_NODES", "").split(),
        "lrl_options": lrl_lines,
        "noise_controls": [l for l in lrl_lines if l.startswith(("timeseries_metrics", "sqlenginepool"))],
        "memstats_available": e.get("MEMLIFE_NO_MEMSTATS", "0") != "1",
        "debugger": e.get("DEBUGGER", ""), "debug_allocator": e.get("MEMLIFE_DEBUG_ALLOC", ""),
        "valgrind_hint": e.get("MEMLIFE_VALGRIND_HINT", ""),
        "build": {
            "comdb2_exe": exe, "comdb2_exe_sha256": sha256(exe),
            "source_sha": src or None, "bb_plugins_sha": e.get("MEMLIFE_BBPLUGINS_SHA"),
            "builddb_sha": e.get("MEMLIFE_BUILDDB_SHA"),
            "compiler": [l.split("]", 1)[1].strip() for l in comment.splitlines() if "]" in l][:3],
            "elf_type": next((l.split(":", 1)[1].strip() for l in elf.splitlines() if "Type:" in l), None),
            "cmake": cmake_cache(exe),
        },
        "hosts": {"runner": os.uname()[1], "kernels": e.get("MEMLIFE_KERNELS", "").split()},
        "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    with open(path, "w") as fh:
        json.dump(md, fh, indent=1, sort_keys=True)


def parse_retention(s):
    return dict(x.split(":", 1) if ":" in x else (x, "declared") for x in s.split())


# ------------------------------------------------------------------ validation
def expected_labels(md):
    return (["A.1", "A.2", "A.3"] + ["I%d" % k for k in range(1, md["iterations"] + 1)] +
            ["R%d" % j for j in range(1, md.get("reclaim_snapshots", 0) + 1)])


def load(path):
    """Rows of snapshots.tsv and the format errors found."""
    rows, errors = [], []
    if not os.path.exists(path):
        return rows, ["no snapshots file %s" % path]
    with open(path) as fh:
        for n, line in enumerate(fh, 1):
            f = line.rstrip("\n").split("\t")
            if len(f) != len(FIELDS):
                errors.append("line %d: %d fields, expected %d" % (n, len(f), len(FIELDS)))
                continue
            r = dict(zip(FIELDS, f))
            try:
                for k in ("used", "total", "free", "peak"):
                    r[k] = int(r[k])
                r["ts"] = float(r["ts"])
            except ValueError:
                errors.append("line %d: non-numeric value" % n)
                continue
            rows.append(r)
    return rows, errors


def validate(md, rows):
    """host -> label -> {allocator: row}; errors is empty when complete."""
    errors = []
    want = expected_labels(md)
    nodes = md["nodes"]
    snaps = collections.defaultdict(dict)
    stamps = collections.defaultdict(set)
    names = collections.defaultdict(set)
    for r in rows:
        stamps[(r["host"], r["label"])].add(r["ts"])
        key = (r["host"], r["label"], r["ts"])
        if r["name"] in names[key]:
            errors.append("%s %s: allocator %s listed twice" % (r["host"], r["label"], r["name"]))
        names[key].add(r["name"])
        snaps[r["host"]].setdefault(r["label"], {})[r["name"]] = r
    for (host, label), ts in sorted(stamps.items()):
        if len(ts) > 1:
            errors.append("%s: snapshot %s taken %d times" % (host, label, len(ts)))
    for host in sorted(set(snaps) - set(nodes)):
        errors.append("unexpected node %s (expected %s)" % (host, " ".join(nodes)))
    for host in nodes:
        got = snaps.get(host, {})
        missing = [l for l in want if l not in got]
        if missing:
            errors.append("%s: missing snapshots %s" % (host, " ".join(missing)))
        extra = [l for l in got if l not in want]
        if extra:
            errors.append("%s: unexpected snapshots %s" % (host, " ".join(sorted(extra))))
        pids = sorted({next(iter(got[l].values()))["pid"] for l in got} - {"-"})
        if len(pids) > 1 and host not in md.get("restarted_nodes", []):
            errors.append("%s: server process changed during the test (pids %s)" % (host, " ".join(pids)))
        if host in md.get("restarted_nodes", []):
            continue
        allocs = set().union(*[set(v) for v in got.values()]) if got else set()
        for l in want:
            if l in got:
                lost = sorted(allocs - set(got[l]))
                if lost:
                    errors.append("%s %s: allocators missing: %s" % (host, l, " ".join(lost)))
    for name in md.get("retention", {}):
        if md.get("reclaim_snapshots", 0) == 0:
            errors.append("retention declared for %s but the test has no reclaim phase" % name)
    return snaps, errors


# ------------------------------------------------------------------ classification
def median(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2]


def classify(base, spread, series):
    """(class, deltas) for the iteration values of one allocator; see the module doc."""
    band = max(MIN_BAND, 3 * spread)
    floor = max(MIN_FLOOR, 2 * spread)
    d = [b - a for a, b in zip([base] + series[:-1], series)]
    n = len(series)
    if n < 3:
        return "INCONCLUSIVE", d
    growth = series[-1] - base
    sig = [k for k, x in enumerate(d) if x > band]
    post = d[1:]
    if sum(1 for x in post if x > band) >= STRONG_FRACTION * len(post) and series[-1] - series[0] > band:
        return "STRONG_GROWTH", d
    if len(sig) == 1 and abs(sum(x for k, x in enumerate(d) if k != sig[0])) <= band:
        return "ONE_TIME_GROWTH", d
    tail = d[-max(3, n // 4):]
    tail_up = sum(tail) > band or all(x > floor for x in tail)
    if growth > band and tail_up and sum(1 for x in d if x > floor) >= 2:
        return "SUSPICIOUS_GROWTH", d
    if growth > band and not tail_up:
        return "PLATEAU", d
    return "STABLE", d


def worse(a, b):
    return a if ORDER.index(a) <= ORDER.index(b) else b


def retention_check(cls, base, band, series, reclaims, reason):
    """(class, note, verified) for an allocator declared retained by design."""
    peak = max(series) - base
    if not reclaims:
        return cls, "declared retained (%s); not verified: no reclaim snapshot" % reason, False
    if peak <= band:
        return cls, "declared retained (%s); growth %s within band, reclaim not exercised" % (reason, kib(peak)), False
    residual = [r - base for r in reclaims]
    if len(reclaims) > 1 and reclaims[-1] - reclaims[0] > band:
        return worse(cls, "SUSPICIOUS_GROWTH"), "declared retained (%s); post-reclaim floor rises %s over %d cycles" % (
            reason, kib(reclaims[-1] - reclaims[0]), len(reclaims)), True
    if max(residual) <= band:
        return "RETENTION_BOUNDED", "retained (%s); peak +%s, residual after reclaim %s (tolerance %s)" % (
            reason, kib(peak), "/".join(kib(x) for x in residual), kib(band)), True
    return cls, "declared retained (%s) but residual after reclaim %s exceeds tolerance %s" % (
        reason, "/".join(kib(x) for x in residual), kib(band)), True


def kib(x):
    return "%.0f KiB" % (x / 1024.0)


def analyze(md, snaps):
    iters = ["I%d" % k for k in range(1, md["iterations"] + 1)]
    recl = ["R%d" % j for j in range(1, md.get("reclaim_snapshots", 0) + 1)]
    retention = md.get("retention", {})
    res = {}
    for host in md["nodes"]:
        d = snaps[host]
        order = ["A.1", "A.2", "A.3"] + iters + recl
        node = {"labels": order,
                "roles": [next(iter(d[l].values()))["role"] for l in order],
                "pids": [next(iter(d[l].values()))["pid"] for l in order],
                "rss_kb": [int(x) if x.isdigit() else None for x in
                           (next(iter(d[l].values()))["rss"] for l in order)],
                "allocators": {}}
        if host in md.get("restarted_nodes", []):
            node["note"] = "restarted by the test: not classified"
            res[host] = node
            continue
        for name in sorted(d["A.1"]):
            a_vals = [d[l][name]["used"] for l in ("A.1", "A.2", "A.3")]
            series = [d[l][name]["used"] for l in iters]
            reclaims = [d[l][name]["used"] for l in recl]
            base, spread = median(a_vals), max(a_vals) - min(a_vals)
            band = max(MIN_BAND, 3 * spread)
            cls, deltas = classify(base, spread, series)
            note, verified = "", None
            if name in retention:
                cls, note, verified = retention_check(cls, base, band, series, reclaims, retention[name])
            node["allocators"][name] = {
                "classification": cls, "baseline": base, "baseline_spread": spread, "band": band,
                "values": series, "deltas": deltas, "reclaims": reclaims,
                "net_first_last": series[-1] - series[0],
                "per_iteration": (series[-1] - series[0]) / float(max(1, len(series) - 1)),
                "retention_verified": verified, "note": note}
        res[host] = node
    return res


# ------------------------------------------------------------------ output
def escalation(md, test, host, alloc):
    env = "CLUSTER=\"%s\" " % " ".join(md["cluster"]) if md.get("cluster") else ""
    hint = md.get("valgrind_hint") or ""
    return {
        "memstat_debug": "cd tests && %sMEMLIFE_DEBUG_ALLOC=%s make %s" % (env, alloc, test),
        "valgrind": ("build with -DCOMDB2_PER_THREAD_MALLOC=OFF, then: cd tests && %sMEMLIFE_ITERS=2 %s"
                     "make %s DEBUGGER=memcheck COMDB2_EXE=<that build>/db/comdb2" % (env, hint + " " if hint else "",
                                                                                     test)),
    }


def write_status(dirname, status, detail):
    with open(os.path.join(dirname, "execution.json"), "w") as fh:
        json.dump({"execution_status": status, "detail": detail}, fh)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir")
    ap.add_argument("--write-metadata")
    args = ap.parse_args()
    if args.write_metadata:
        write_metadata(args.write_metadata)
        return 0
    if not args.dir:
        ap.error("--dir or --write-metadata is required")
    with open(os.path.join(args.dir, "metadata.json")) as fh:
        md = json.load(fh)
    test = md["test"]
    ex_path = os.path.join(args.dir, "execution.json")
    execution = "INCOMPLETE"
    if os.path.exists(ex_path):
        with open(ex_path) as fh:
            execution = json.load(fh)["execution_status"]
    rows, errors = load(os.path.join(args.dir, "snapshots", "snapshots.tsv"))
    snaps, verrors = validate(md, rows)
    errors += verrors
    if execution != "WORKLOAD_PASS":
        errors.insert(0, "execution status is %s" % execution)
    if errors:
        if execution == "WORKLOAD_PASS":
            execution = "INCOMPLETE"
            write_status(args.dir, execution, "; ".join(errors[:5]))
        with open(os.path.join(args.dir, "summary.json"), "w") as fh:
            json.dump({"test": test, "execution_status": execution, "memory_status": "INCONCLUSIVE",
                       "validation_errors": errors}, fh, indent=1, sort_keys=True)
        with open(os.path.join(args.dir, "summary.txt"), "w") as fh:
            fh.write("TEST: %s\nEXECUTION: %s\nMEMORY: INCONCLUSIVE\n" % (test, execution))
            fh.write("".join("VALIDATION: %s\n" % e for e in errors))
        return 3

    res = analyze(md, snaps)
    classes = [a["classification"] for n in res.values() for a in n["allocators"].values()]
    present = [c for c in ORDER if c in classes]
    memory = present[0] if present else "INCONCLUSIVE"
    iters = md["iterations"]
    recl = md.get("reclaim_snapshots", 0)
    grow = sorted(((a["per_iteration"], host, name) for host, n in res.items() for name, a in n["allocators"].items()
                   if a["classification"] in ("STRONG_GROWTH", "SUSPICIOUS_GROWTH")), reverse=True)
    top = None
    if grow:
        rate, host, name = grow[0]
        top = {"allocator": name, "host": host, "per_iteration": rate, "classification": res[host]["allocators"][name][
            "classification"], "rerun": escalation(md, test, host, name)}
    summary = {"test": test, "execution_status": execution, "memory_status": memory, "iterations": iters,
               "reclaim_snapshots": recl, "nodes": res, "top_allocator": top, "build": md.get("build"),
               "noise_controls": md.get("noise_controls"), "retention": md.get("retention")}
    with open(os.path.join(args.dir, "summary.json"), "w") as fh:
        json.dump(summary, fh, indent=1, sort_keys=True)

    with open(os.path.join(args.dir, "comparison.csv"), "w") as fh:
        w = csv.writer(fh)
        w.writerow(["host", "allocator", "classification", "baseline", "spread", "band"] +
                   ["I%d" % k for k in range(1, iters + 1)] + ["d%d" % k for k in range(1, iters + 1)] +
                   ["R%d" % j for j in range(1, recl + 1)] + ["net_first_last", "note"])
        for host, n in sorted(res.items()):
            for name, a in sorted(n["allocators"].items()):
                w.writerow([host, name, a["classification"], a["baseline"], a["baseline_spread"], a["band"]] +
                           a["values"] + a["deltas"] + a["reclaims"] + [a["net_first_last"], a["note"]])

    lines = ["TEST: %s" % test, "EXECUTION: %s" % execution, "MEMORY: %s" % memory,
             "Iterations: %d  Reclaims: %d" % (iters, recl), ""]
    for host, n in sorted(res.items()):
        lines.append("Node %s (roles %s)" % (host, ",".join(sorted(set(n["roles"])))))
        lines.append("  RSS KiB: %s" % " ".join(str(x) if x is not None else "-" for x in n["rss_kb"]))
        if n.get("note"):
            lines.append("  [%s]" % n["note"])
        ranked = sorted(n["allocators"].items(),
                        key=lambda kv: (ORDER.index(kv[1]["classification"]), -kv[1]["net_first_last"]))
        for name, a in ranked:
            if a["classification"] == "STABLE" and abs(a["net_first_last"]) < 16 * 1024 and not a["note"]:
                continue
            k = lambda x: "%.0f" % (x / 1024.0)
            lines.append("  %-14s %-18s KiB A=%s used=%s%s%s" % (
                name, a["classification"], k(a["baseline"]), " ".join(k(x) for x in a["values"]),
                (" R=%s" % " ".join(k(x) for x in a["reclaims"])) if a["reclaims"] else "",
                ("  [%s]" % a["note"]) if a["note"] else ""))
    if top:
        lines += ["", "TOP ALLOCATOR: %s on %s (%s, %+.0f KiB/iteration)" % (
            top["allocator"], top["host"], top["classification"], top["per_iteration"] / 1024.0),
            "Escalation (memstat debug): %s" % top["rerun"]["memstat_debug"],
            "Targeted Valgrind: %s" % top["rerun"]["valgrind"]]
    with open(os.path.join(args.dir, "summary.txt"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
