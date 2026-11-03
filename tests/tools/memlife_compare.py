#!/usr/bin/env python3
"""memlife_compare.py --dir <artifact dir> --test <name> [--retention "alloc:reason ..."]

Classify per node and per allocator the comdb2_memstats "used" values written
by memlife_snapshot.sh (snapshots/snapshots.tsv). Writes comparison.csv,
summary.txt and summary.json into --dir; raw values are always kept.

Snapshots: A.1-A.3 (baseline after warm-up; their spread is the noise),
I1..In (after each lifecycle repetition), optional RECLAIM (after the release
event of a structure retained by design).

Per allocator, with band = max(64 KiB, 3 x spread(A.1..A.3)) and
d_k = I_k - I_(k-1) (I_0 = median A):
  STRONG_GROWTH      every step after the first grows by more than band
  SUSPICIOUS_GROWTH  grows on every repetition by more than the step floor
                     (max(4 KiB, 2 x spread)) and by more than band overall, or
                     net growth I1->In > band with the second half still growing
  PLATEAU            grew by more than band in total, flat over the second half
  ONE_TIME_GROWTH    a single step beyond band
  STABLE             no step beyond band
  RETENTION_BOUNDED  declared retained (--retention) and RECLAIM gave back at
                     least half of its growth
  INCONCLUSIVE       not enough snapshots
A declared allocator whose RECLAIM did not give memory back keeps its growth
class, with a note: retention is tested, never just ignored.
"""
import argparse
import collections
import csv
import json
import os

MIN_BAND = 64 * 1024
ORDER = ["STRONG_GROWTH", "SUSPICIOUS_GROWTH", "PLATEAU", "ONE_TIME_GROWTH", "RETENTION_BOUNDED", "STABLE",
         "INCONCLUSIVE"]


def load(path):
    """host -> label -> {alloc: row}; iterations renumbered I1..In by time."""
    raw = collections.defaultdict(lambda: collections.OrderedDict())
    with open(path) as fh:
        for line in fh:
            f = line.rstrip("\n").split("\t")
            if len(f) < 11:
                continue
            label, ts, host, pid, rss, role, name = f[:7]
            used, total, free, peak = (int(x) for x in f[7:11])
            raw[host].setdefault((label, ts), {})[name] = dict(
                ts=float(ts), pid=pid, rss=rss, role=role, used=used, total=total, free=free, peak=peak)
    out = {}
    for host, snaps in raw.items():
        d = collections.OrderedDict()
        iters = []
        for (label, ts), rows in snaps.items():
            if label.startswith("I"):
                iters.append((float(ts), rows))
            else:
                d[label] = rows
        for k, (_, rows) in enumerate(sorted(iters, key=lambda x: x[0]), 1):
            d["I%d" % k] = rows
        out[host] = d
    return out


def median(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2]


def classify(base, band, series, floor):
    d = [b - a for a, b in zip([base] + series[:-1], series)]
    if len(series) < 3:
        return "INCONCLUSIVE", d
    if all(x > band for x in d[1:]):
        return "STRONG_GROWTH", d
    net = series[-1] - series[0]
    tail = d[len(d) // 2:]
    if net > band and (all(x > floor for x in d[1:]) or sum(tail) > band):
        return "SUSPICIOUS_GROWTH", d
    if series[-1] - base > band and sum(tail) <= band / 2.0:
        return "PLATEAU", d
    if any(x > band for x in d):
        return "ONE_TIME_GROWTH", d
    return "STABLE", d


def analyze(snaps, retention):
    res = {}
    for host, d in snaps.items():
        a_labels = [l for l in d if l.startswith("A.")]
        iters = sorted((l for l in d if l.startswith("I")), key=lambda l: int(l[1:]))
        allocs = sorted(set().union(*[set(v) for v in d.values()])) if d else []
        node = {"role": [], "rss_kb": [], "allocators": {}}
        for l in a_labels[1:2] + iters + (["RECLAIM"] if "RECLAIM" in d else []):
            first = next(iter(d[l].values()))
            node["role"].append(first["role"])
            node["rss_kb"].append(int(first["rss"]) if first["rss"].isdigit() else None)
        for name in allocs:
            a_vals = [d[l][name]["used"] for l in a_labels if name in d[l]]
            series = [d[l][name]["used"] for l in iters if name in d[l]]
            if not a_vals or not series:
                node["allocators"][name] = {"classification": "INCONCLUSIVE", "values": series}
                continue
            base = median(a_vals)
            band = max(MIN_BAND, 3 * (max(a_vals) - min(a_vals)))
            floor = max(4 * 1024, 2 * (max(a_vals) - min(a_vals)))
            cls, deltas = classify(base, band, series, floor)
            note = ""
            reclaim = d.get("RECLAIM", {}).get(name, {}).get("used")
            if name in retention:
                grown = max(series) - base
                if reclaim is not None and grown > band:
                    if max(series) - reclaim >= grown / 2.0:
                        cls = "RETENTION_BOUNDED"
                        note = "retained (%s); RECLAIM gave back %d of %d bytes" % (
                            retention[name], max(series) - reclaim, grown)
                    else:
                        note = "declared retained (%s) but RECLAIM gave back only %d of %d bytes" % (
                            retention[name], max(series) - reclaim, grown)
                elif reclaim is None:
                    note = "declared retained (%s); no reclamation event tested" % retention[name]
            node["allocators"][name] = {
                "classification": cls, "baseline": base, "baseline_spread": max(a_vals) - min(a_vals), "band": band,
                "values": series, "deltas": deltas, "net_first_last": series[-1] - series[0], "reclaim": reclaim,
                "note": note}
        res[host] = node
    return res


def worst(classes):
    present = [c for c in ORDER if c in classes]
    return present[0] if present else "INCONCLUSIVE"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--test", required=True)
    ap.add_argument("--retention", default="")
    args = ap.parse_args()
    retention = dict(x.split(":", 1) if ":" in x else (x, "declared") for x in args.retention.split())
    snaps = load(os.path.join(args.dir, "snapshots", "snapshots.tsv"))
    res = analyze(snaps, retention)
    allc = [a["classification"] for n in res.values() for a in n["allocators"].values()]
    status = worst(allc)
    iters = max((len(a.get("values", [])) for n in res.values() for a in n["allocators"].values()), default=0)
    summary = {"test": args.test, "iterations": iters, "status": status, "nodes": res}
    with open(os.path.join(args.dir, "summary.json"), "w") as fh:
        json.dump(summary, fh, indent=1, sort_keys=True)
    with open(os.path.join(args.dir, "comparison.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["host", "allocator", "classification", "baseline", "band"] +
                   ["I%d" % (k + 1) for k in range(iters)] + ["d%d" % (k + 1) for k in range(iters)] +
                   ["net_first_last", "reclaim", "note"])
        for host, n in sorted(res.items()):
            for name, a in sorted(n["allocators"].items()):
                v, dl = a.get("values", []), a.get("deltas", [])
                w.writerow([host, name, a["classification"], a.get("baseline"), a.get("band")] +
                           v + [""] * (iters - len(v)) + dl + [""] * (iters - len(dl)) +
                           [a.get("net_first_last"), a.get("reclaim"), a.get("note", "")])
    lines = ["TEST: %s" % args.test, "Iterations: %d" % iters, "STATUS: %s" % status, ""]
    for host, n in sorted(res.items()):
        lines.append("Node %s (roles %s)" % (host, ",".join(sorted(set(n["role"])))))
        lines.append("  RSS KiB: %s" % " ".join(str(x) for x in n["rss_kb"]))
        ranked = sorted(n["allocators"].items(), key=lambda kv: (ORDER.index(kv[1]["classification"]),
                                                                  -kv[1].get("net_first_last", 0) or 0))
        for name, a in ranked:
            if a["classification"] in ("STABLE",) and a.get("net_first_last", 0) < 16 * 1024:
                continue
            kib = lambda x: "%.0f" % (x / 1024.0)
            lines.append("  %-14s %-18s KiB A=%s used=%s%s%s" % (
                name, a["classification"], kib(a.get("baseline", 0)), " ".join(kib(x) for x in a.get("values", [])),
                (" RECLAIM=%s" % kib(a["reclaim"])) if a.get("reclaim") is not None else "",
                ("  [%s]" % a["note"]) if a.get("note") else ""))
    grow = sorted(((a.get("net_first_last", 0), name) for n in res.values() for name, a in n["allocators"].items()
                   if a["classification"] in ("STRONG_GROWTH", "SUSPICIOUS_GROWTH")), reverse=True)
    if grow:
        lines += ["", "Escalation: memstat debug recommended: MEMLIFE_DEBUG_ALLOC=%s" % grow[0][1]]
    with open(os.path.join(args.dir, "summary.txt"), "w") as fh:
        fh.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
