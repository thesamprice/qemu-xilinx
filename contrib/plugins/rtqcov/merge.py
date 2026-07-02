"""Merge many per-test LCOV .info files into one aggregate .info.

Merging is by SOURCE IDENTITY (source path + line) -- the same cpukit/bsps line
lives at different addresses in different test binaries, so address-based
merging would be wrong. Coverable vs covered are tracked separately so real
percentages survive: a line is covered in the aggregate if any input covers it;
coverable if any input lists it. Execution counts are SUMMED across tests.
"""

import argparse
import glob
import sys
from collections import defaultdict


def parse_info(path, coverable, line_hits, func_line, func_hits):
    """Accumulate one .info. line_hits/func_hits SUM execution counts across
    tests (total executions for counts mode; number of covering tests for plain
    coverage). coverable is the union of all DA lines."""
    cur_sf = None
    with open(path) as f:
        for raw in f:
            raw = raw.strip()
            if raw.startswith("SF:"):
                cur_sf = raw[3:]
            elif raw == "end_of_record":
                cur_sf = None
            elif cur_sf is None:
                continue
            elif raw.startswith("DA:"):
                lno_s, _, hits_s = raw[3:].partition(",")
                try:
                    lno = int(lno_s)
                    hits = int(hits_s) if hits_s else 0
                except ValueError:
                    continue
                coverable[cur_sf].add(lno)
                if hits > 0:
                    lh = line_hits[cur_sf]
                    lh[lno] = lh.get(lno, 0) + hits
            elif raw.startswith("FN:"):
                lno_s, _, name = raw[3:].partition(",")
                try:
                    lno = int(lno_s)
                except ValueError:
                    continue
                if name:
                    cur = func_line[cur_sf].get(name)
                    if cur is None or lno < cur:
                        func_line[cur_sf][name] = lno
            elif raw.startswith("FNDA:"):
                hits_s, _, name = raw[5:].partition(",")
                try:
                    hits = int(hits_s)
                except ValueError:
                    continue
                if name and hits > 0:
                    fh = func_hits[cur_sf]
                    fh[name] = fh.get(name, 0) + hits


def add_arguments(parser):
    parser.add_argument("inputs", nargs="+",
                        help="per-test .info files (globs allowed)")
    parser.add_argument("--out", required=True, help="aggregate .info output")
    parser.add_argument("--name", default="aggregate",
                        help="aggregate test name (TN), default 'aggregate'")


def run(args):
    paths = []
    for pat in args.inputs:
        expanded = glob.glob(pat)
        paths.extend(expanded if expanded else [pat])
    if not paths:
        print("error: no input files", file=sys.stderr)
        return 1

    coverable = defaultdict(set)
    line_hits = defaultdict(dict)     # sf -> {line: summed count}
    func_line = defaultdict(dict)
    func_hits = defaultdict(dict)     # sf -> {name: summed count}
    for p in paths:
        try:
            parse_info(p, coverable, line_hits, func_line, func_hits)
        except OSError as e:
            print(f"warning: skipping {p}: {e}", file=sys.stderr)

    total_lf = total_lh = 0
    with open(args.out, "w") as out:
        for sf in sorted(coverable):
            cab = coverable[sf]
            lh = line_hits.get(sf, {})
            funcs = func_line.get(sf, {})
            fh = func_hits.get(sf, {})
            out.write(f"TN:{args.name}\n")
            out.write(f"SF:{sf}\n")
            ordered = sorted(funcs, key=lambda n: (funcs[n], n))
            for fn in ordered:
                out.write(f"FN:{funcs[fn]},{fn}\n")
            for fn in ordered:
                out.write(f"FNDA:{fh.get(fn, 0)},{fn}\n")
            if funcs:
                out.write(f"FNF:{len(funcs)}\n")
                out.write(f"FNH:{sum(1 for fn in funcs if fh.get(fn, 0) > 0)}\n")
            for ln in sorted(cab):
                out.write(f"DA:{ln},{lh.get(ln, 0)}\n")
            out.write(f"LF:{len(cab)}\n")
            out.write(f"LH:{len(lh)}\n")
            out.write("end_of_record\n")
            total_lf += len(cab)
            total_lh += len(lh)

    pct = (100.0 * total_lh / total_lf) if total_lf else 0.0
    print(f"merged {len(paths)} files -> {len(coverable)} source files, "
          f"{total_lh}/{total_lf} lines ({pct:.1f}%) -> {args.out}",
          file=sys.stderr)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    add_arguments(ap)
    return run(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
