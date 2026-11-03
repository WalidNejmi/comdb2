#!/usr/bin/env python3
"""memlife_stacks.py --dir <artifact dir> --exe <comdb2> --first rep1 --last repN

Aggregate the memstat debug dumps written by memlife_common.sh
(debug.<label>.<host>.dump: one line of frame addresses per live tracked
chunk) into stacks.txt: identical stacks are grouped and counted after the
first and the last tracked repetition, and sorted by growth. Frames are
symbolized with addr2line; non-PIE binaries use the addresses as they are,
PIE binaries need --base (the load address from pmap).
"""
import argparse
import collections
import glob
import os
import subprocess


def stacks(path):
    c = collections.Counter()
    with open(path) as fh:
        for line in fh:
            fr = tuple(line.split())
            if fr:
                c[fr] += 1
    return c


def is_pie(exe):
    out = subprocess.run(["readelf", "-h", exe], stdout=subprocess.PIPE, universal_newlines=True).stdout
    return "DYN" in out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--first", required=True)
    ap.add_argument("--last", required=True)
    ap.add_argument("--base", default="0")
    ap.add_argument("--frames", type=int, default=10)
    args = ap.parse_args()
    base = int(args.base, 16) if is_pie(args.exe) else 0
    lines = []
    for first in sorted(glob.glob(os.path.join(args.dir, "debug.%s.*.dump" % args.first))):
        host = first.rsplit(".", 2)[-2]
        last = os.path.join(args.dir, "debug.%s.%s.dump" % (args.last, host))
        if not os.path.exists(last):
            continue
        s1, s2 = stacks(first), stacks(last)
        addrs = sorted({a for st in list(s1) + list(s2) for a in st[:args.frames]})
        sym = {}
        for i in range(0, len(addrs), 500):
            chunk = addrs[i:i + 500]
            res = subprocess.run(["addr2line", "-fsp", "-e", args.exe] + ["0x%x" % (int(a, 16) - base) for a in chunk],
                                 stdout=subprocess.PIPE, universal_newlines=True).stdout.splitlines()
            sym.update({a: r.replace(" at ", " ") for a, r in zip(chunk, res)})
        lines.append("== node %s: %d tracked chunks after %s, %d after %s" % (
            host, sum(s1.values()), args.first, sum(s2.values()), args.last))
        lines.append("count_%s\tcount_%s\tdelta\tstack" % (args.first, args.last))
        for st in sorted(set(s1) | set(s2), key=lambda st: -(s2[st] - s1[st]))[:25]:
            lines.append("%d\t%d\t%d\t%s" % (s1[st], s2[st], s2[st] - s1[st],
                                             " < ".join(sym.get(a, a) for a in st[:args.frames])))
    with open(os.path.join(args.dir, "stacks.txt"), "w") as fh:
        fh.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
