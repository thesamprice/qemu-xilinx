"""Restrict an aggregate LCOV .info to the symbols present in a target ELF.

Drops coverage for any source line / function that is not part of the target
binary, keeping the suite's covered counts and denominator for what remains
(filter-only: the result never adds target lines the suite never had). A common
use is qualification -- point --elf at a deliverable binary to see how well the
test campaign covered just the code that ships.

Membership oracle: the target's coverable inventory (same `(file, line)`
normalization as the aggregate), built via objdump+addr2line like
`rtqcov coverable`. Build it with the source root used to build the target so
`contrib/` paths are preserved.
"""

import argparse
import json
import os
import subprocess
import sys
from collections import defaultdict

from .symbolize import iter_covered_lines
from .coverable import disassemble_addresses
from .merge import parse_info
from .cliargs import add_symbolize_args


def load_target_inventory(args):
    """Return (keep_lines{(sf,line)}, keep_funcs{(sf,function)}) for the target.

    From a precomputed --coverable JSONL, or built from --elf via objdump +
    addr2line using the same normalization as the aggregate.
    """
    keep_lines = set()
    keep_funcs = set()

    if args.coverable:
        with open(args.coverable) as f:
            for raw in f:
                raw = raw.strip()
                if not raw:
                    continue
                rec = json.loads(raw)
                sf = rec["file"]
                keep_lines.add((sf, int(rec["line"])))
                fn = rec.get("function")
                if fn:
                    keep_funcs.add((sf, fn))
        return keep_lines, keep_funcs

    objdump = args.objdump or (args.toolchain_prefix + "objdump")
    addr2line = args.addr2line or (args.toolchain_prefix + "addr2line")
    addrs = disassemble_addresses(objdump, args.elf)
    for norm, line, func, _depth, _addr in iter_covered_lines(
            addr2line, args.elf, addrs, args.source_root,
            args.include_testsuites, tuple(args.keep), args.all_paths):
        keep_lines.add((norm, line))
        keep_funcs.add((norm, func))
    return keep_lines, keep_funcs


def add_arguments(parser):
    parser.add_argument("--aggregate", required=True,
                        help="aggregate LCOV .info to restrict")
    parser.add_argument("--elf", help="target ELF (its symbols define what is "
                        "kept); or use --coverable")
    parser.add_argument("--coverable",
                        help="precomputed target coverable JSONL "
                             "(skips objdump/addr2line)")
    add_symbolize_args(parser)
    parser.add_argument("--objdump", help="explicit objdump path")
    parser.add_argument("--name", default="restricted",
                        help="LCOV test name (TN), default 'restricted'")
    parser.add_argument("--html", metavar="DIR",
                        help="also run genhtml into DIR")
    parser.add_argument("--out", required=True, help="output .info file")


def run(args):
    if not args.elf and not args.coverable:
        print("error: --elf or --coverable is required", file=sys.stderr)
        return 2

    try:
        keep_lines, keep_funcs = load_target_inventory(args)
    except (OSError, RuntimeError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    coverable = defaultdict(set)
    line_hits = defaultdict(dict)
    func_line = defaultdict(dict)
    func_hits = defaultdict(dict)
    try:
        parse_info(args.aggregate, coverable, line_hits, func_line, func_hits)
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    in_lf = sum(len(v) for v in coverable.values())
    out_lf = out_lh = 0
    with open(args.out, "w") as out:
        for sf in sorted(coverable):
            cab = {ln for ln in coverable[sf] if (sf, ln) in keep_lines}
            if not cab:
                continue
            lh = line_hits.get(sf, {})
            funcs = {fn: ln for fn, ln in func_line.get(sf, {}).items()
                     if (sf, fn) in keep_funcs}
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
            covered = 0
            for ln in sorted(cab):
                hits = lh.get(ln, 0)
                if hits > 0:
                    covered += 1
                out.write(f"DA:{ln},{hits}\n")
            out.write(f"LF:{len(cab)}\n")
            out.write(f"LH:{covered}\n")
            out.write("end_of_record\n")
            out_lf += len(cab)
            out_lh += covered

    pct = (100.0 * out_lh / out_lf) if out_lf else 0.0
    print(f"restrict: {in_lf} -> {out_lf} coverable lines kept, "
          f"{out_lh} covered ({pct:.1f}%) -> {args.out}", file=sys.stderr)

    if args.html:
        cwd = args.source_root or "/"
        rc = subprocess.run(
            ["genhtml", os.path.abspath(args.out),
             "--output-directory", os.path.abspath(args.html),
             "--quiet", "--ignore-errors", "source"],
            cwd=cwd).returncode
        if rc != 0:
            print(f"warning: genhtml exited {rc}", file=sys.stderr)
        else:
            print(f"restrict: HTML -> {args.html}/index.html", file=sys.stderr)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    add_arguments(ap)
    return run(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
