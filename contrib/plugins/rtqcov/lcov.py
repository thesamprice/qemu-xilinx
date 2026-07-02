"""Convert symbolized RTQCov JSONL into a per-test LCOV .info file.

With --coverable, combines the coverable-line inventory with the covered lines
and emits DA:<line>,<count> for hit lines and DA:<line>,0 for coverable-but-not-
hit lines, so genhtml reports true percentages (covered / coverable). Without
it, covered-only (every covered line DA:,1).
"""

import argparse
import json
import sys
from collections import defaultdict


def load(path):
    """Parse symbolized JSONL.

    Returns (lines_by_file{set}, funcs_by_file{name:min_line}, test_id, arch,
    line_count{(sf,line):max_count}). line_count is the per-line execution count
    (max across the line's records); it is 1 for plain coverage files.
    """
    lines_by_file = defaultdict(set)
    funcs_by_file = defaultdict(dict)
    line_count = {}
    test_id = arch = ""
    with open(path) as f:
        for raw in f:
            raw = raw.strip()
            if not raw:
                continue
            rec = json.loads(raw)
            sf = rec["file"]
            ln = int(rec["line"])
            fn = rec.get("function") or ""
            cnt = int(rec.get("count", 1))
            lines_by_file[sf].add(ln)
            k = (sf, ln)
            if cnt > line_count.get(k, 0):
                line_count[k] = cnt
            test_id = test_id or rec.get("test_id", "")
            arch = arch or rec.get("arch", "")
            if fn:
                cur = funcs_by_file[sf].get(fn)
                if cur is None or ln < cur:
                    funcs_by_file[sf][fn] = ln
    return lines_by_file, funcs_by_file, test_id, arch, line_count


def add_arguments(parser):
    parser.add_argument("jsonl", help="covered .jsonl from `rtqcov symbolize`")
    parser.add_argument("--coverable", help="coverable .jsonl from "
                        "`rtqcov coverable` (enables real percentages)")
    parser.add_argument("--out", required=True, help="output .info file")
    parser.add_argument("--test-name", help="LCOV test name (TN); "
                        "default derived from test_id+arch")


def run(args):
    try:
        cov_lines, cov_funcs, test_id, arch, cov_count = load(args.jsonl)
        if args.coverable:
            cab_lines, cab_funcs, _, _, _ = load(args.coverable)
        else:
            cab_lines, cab_funcs = cov_lines, cov_funcs
    except (OSError, ValueError, KeyError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    tn = args.test_name or "-".join(x for x in (test_id, arch) if x) or "rtqcov"

    # The universe of source files is the coverable set, plus any covered file
    # not present in coverable (shouldn't happen, but never drop a hit).
    all_files = set(cab_lines) | set(cov_lines)

    total_lf = total_lh = 0
    with open(args.out, "w") as out:
        for sf in sorted(all_files):
            covered = cov_lines.get(sf, set())
            coverable = cab_lines.get(sf, set()) | covered  # never lose a hit
            # Functions: union; declaration line preferred from coverable.
            funcs = dict(cab_funcs.get(sf, {}))
            for fn, ln in cov_funcs.get(sf, {}).items():
                if fn not in funcs or ln < funcs[fn]:
                    funcs[fn] = ln
            covered_fn_lines = cov_funcs.get(sf, {})
            covered_fns = set(covered_fn_lines)

            out.write(f"TN:{tn}\n")
            out.write(f"SF:{sf}\n")
            ordered = sorted(funcs, key=lambda n: (funcs[n], n))
            for fn in ordered:
                out.write(f"FN:{funcs[fn]},{fn}\n")
            for fn in ordered:
                # Function hit count = the count at its (covered) declaration
                # line; an executed function is always at least 1.
                if fn in covered_fns:
                    fc = max(1, cov_count.get((sf, covered_fn_lines[fn]), 1))
                else:
                    fc = 0
                out.write(f"FNDA:{fc},{fn}\n")
            if funcs:
                out.write(f"FNF:{len(funcs)}\n")
                out.write(f"FNH:{len(covered_fns)}\n")
            for ln in sorted(coverable):
                hits = cov_count.get((sf, ln), 0) if ln in covered else 0
                out.write(f"DA:{ln},{hits}\n")
            out.write(f"LF:{len(coverable)}\n")
            out.write(f"LH:{len(covered)}\n")
            out.write("end_of_record\n")
            total_lf += len(coverable)
            total_lh += len(covered)

    pct = (100.0 * total_lh / total_lf) if total_lf else 0.0
    print(f"{args.jsonl}: {len(all_files)} files, "
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
