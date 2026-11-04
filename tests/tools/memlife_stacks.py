#!/usr/bin/env python3
"""memlife_stacks.py --dir <artifact dir> --exe <comdb2> --first rep1 --last repN

Aggregate the memstat debug dumps written by memlife_common.sh
(debug.<label>.<host>.dump: one line of frame addresses per live tracked
chunk) into stacks.txt: identical stacks are grouped and counted after the
first and the last tracked repetition, and sorted by growth. Frames are
symbolized with addr2line. Non-PIE executables use the addresses as they
are; PIE executables are relocated with the load address recorded per node
in debug.base.<host> (from /proc/<pid>/maps). A PIE node without a load
address is not symbolized: it is reported as unsupported and the exit code
is 2, never a list of wrong symbols.
"""
import argparse
import collections
import glob
import os
import subprocess
import sys


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
    for line in out.splitlines():
        if "Type:" in line:
            return "DYN" in line
    raise SystemExit("memlife_stacks: cannot read the ELF header of %s" % exe)


def load_base(dirname, host):
    try:
        with open(os.path.join(dirname, "debug.base.%s" % host)) as fh:
            return int(fh.read().strip(), 16)
    except (OSError, ValueError):
        return None


def symbolize(exe, addrs, base):
    sym = {}
    for i in range(0, len(addrs), 500):
        chunk = addrs[i:i + 500]
        res = subprocess.run(["addr2line", "-fsp", "-e", exe] + ["0x%x" % (int(a, 16) - base) for a in chunk],
                             stdout=subprocess.PIPE, universal_newlines=True).stdout.splitlines()
        sym.update({a: r.replace(" at ", " ") for a, r in zip(chunk, res)})
    return sym


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--first", required=True)
    ap.add_argument("--last", required=True)
    ap.add_argument("--frames", type=int, default=10)
    args = ap.parse_args()
    pie = is_pie(args.exe)
    lines, rc = [], 0
    lines.append("executable %s (%s)" % (args.exe, "PIE: relocated per node" if pie else "non-PIE"))
    for first in sorted(glob.glob(os.path.join(args.dir, "debug.%s.*.dump" % args.first))):
        host = os.path.basename(first)[len("debug.%s." % args.first):-len(".dump")]
        last = os.path.join(args.dir, "debug.%s.%s.dump" % (args.last, host))
        if not os.path.exists(last):
            lines.append("== node %s: no dump after %s" % (host, args.last))
            rc = 2
            continue
        base = 0
        if pie:
            base = load_base(args.dir, host)
            if base is None:
                lines.append("== node %s: PIE executable and no load address (debug.base.%s): not symbolized" % (
                    host, host))
                rc = 2
                continue
        s1, s2 = stacks(first), stacks(last)
        addrs = sorted({a for st in list(s1) + list(s2) for a in st[:args.frames]})
        sym = symbolize(args.exe, addrs, base)
        lines.append("== node %s: %d tracked chunks after %s, %d after %s (load address 0x%x)" % (
            host, sum(s1.values()), args.first, sum(s2.values()), args.last, base))
        lines.append("count_%s\tcount_%s\tdelta\tstack" % (args.first, args.last))
        for st in sorted(set(s1) | set(s2), key=lambda st: -(s2[st] - s1[st]))[:25]:
            lines.append("%d\t%d\t%d\t%s" % (s1[st], s2[st], s2[st] - s1[st],
                                             " < ".join(sym.get(a, a) for a in st[:args.frames])))
    with open(os.path.join(args.dir, "stacks.txt"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    return rc


if __name__ == "__main__":
    sys.exit(main())
