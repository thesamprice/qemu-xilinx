"""Show code an app executes that a baseline (e.g. the test suite) does NOT cover.

Computes the set difference: lines covered by --cov/--app (the app run) that are
NOT covered in --baseline (the suite aggregate). The output .info uses the
app-executed lines as the universe; a line is marked covered iff the baseline
also covers it, so in the HTML the **uncovered (red) lines are the gap** -- code
the app runs but the suite never tests -- and the percentage is "how much of the
app-executed code the baseline covers".

Both sides must share normalization, so symbolize the app .cov with the same
path flags as the baseline (default RTEMS-OS-only, cpukit/bsps/contrib relative);
pass the source root used to build the app so contrib/ paths line up.
"""

import argparse
import json
import os
import subprocess
import sys
from collections import defaultdict

from .format import read_cov
from .symbolize import iter_covered_lines
from .merge import parse_info
from .cliargs import add_symbolize_args


def load_app(args):
    """Return (app_count{(sf,line):count}, func_decl{(sf,name):line},
    func_lines{(sf,name):set(lines)} or None) for the app side."""
    app_count = {}
    func_decl = {}
    func_lines = defaultdict(set)

    if args.cov:
        addr2line = args.addr2line or (args.toolchain_prefix + "addr2line")
        meta, addrs, counts = read_cov(args.cov)
        for norm, line, func, _depth, addr in iter_covered_lines(
                addr2line, args.elf, addrs, args.source_root,
                args.include_testsuites, tuple(args.keep), args.all_paths):
            c = counts.get(addr, 0) if counts else 1
            k = (norm, line)
            if c > app_count.get(k, 0):
                app_count[k] = c
            fk = (norm, func)
            if func and (fk not in func_decl or line < func_decl[fk]):
                func_decl[fk] = line
            if func:
                func_lines[fk].add(line)
        return app_count, func_decl, func_lines

    # --app FILE: JSONL (symbolized) or LCOV .info.
    path = args.app
    if path.endswith(".jsonl"):
        with open(path) as f:
            for raw in f:
                raw = raw.strip()
                if not raw:
                    continue
                rec = json.loads(raw)
                sf, ln = rec["file"], int(rec["line"])
                cnt = int(rec.get("count", 1))
                k = (sf, ln)
                if cnt > app_count.get(k, 0):
                    app_count[k] = cnt
                fn = rec.get("function")
                if fn:
                    fk = (sf, fn)
                    if fk not in func_decl or ln < func_decl[fk]:
                        func_decl[fk] = ln
                    func_lines[fk].add(ln)
        return app_count, func_decl, func_lines

    # LCOV .info: covered lines only (DA hits > 0); no per-line function map.
    cov = defaultdict(set)
    lh = defaultdict(dict)
    fl = defaultdict(dict)
    fh = defaultdict(dict)
    parse_info(path, cov, lh, fl, fh)
    for sf, lines in lh.items():
        for ln, cnt in lines.items():
            app_count[(sf, ln)] = cnt
    for sf, fns in fl.items():
        for name, ln in fns.items():
            func_decl[(sf, name)] = ln
    return app_count, func_decl, None


def load_baseline_covered(path):
    """Return the set of (sf, line) covered (DA hits > 0) in a baseline .info."""
    cov = defaultdict(set)
    lh = defaultdict(dict)
    fl = defaultdict(dict)
    fh = defaultdict(dict)
    parse_info(path, cov, lh, fl, fh)
    return {(sf, ln) for sf, lines in lh.items() for ln in lines}


def add_arguments(parser):
    parser.add_argument("--baseline", required=True,
                        help="baseline LCOV .info to subtract (e.g. the suite "
                             "aggregate)")
    parser.add_argument("--cov", help="app .cov (RTQCov1); needs --elf")
    parser.add_argument("--elf", help="app ELF (with --cov)")
    parser.add_argument("--app", help="precomputed app coverage: .jsonl "
                        "(symbolized) or .info (alternative to --cov/--elf)")
    add_symbolize_args(parser)
    parser.add_argument("--name", default="gap",
                        help="LCOV test name (TN), default 'gap'")
    parser.add_argument("--html", metavar="DIR", help="also run genhtml into DIR")
    parser.add_argument("--out", required=True, help="output .info file")


def run(args):
    if args.cov and not args.elf:
        print("error: --cov requires --elf", file=sys.stderr)
        return 2
    if not args.cov and not args.app:
        print("error: provide --cov/--elf or --app", file=sys.stderr)
        return 2

    try:
        app_count, func_decl, func_lines = load_app(args)
        suite = load_baseline_covered(args.baseline)
    except (OSError, ValueError, RuntimeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    # Group app lines/functions by source file.
    lines_by_sf = defaultdict(set)
    for (sf, ln) in app_count:
        lines_by_sf[sf].add(ln)
    funcs_by_sf = defaultdict(dict)
    for (sf, name), ln in func_decl.items():
        funcs_by_sf[sf][name] = ln

    total = covered = 0
    with open(args.out, "w") as out:
        for sf in sorted(lines_by_sf):
            lines = lines_by_sf[sf]
            funcs = funcs_by_sf.get(sf, {})
            out.write(f"TN:{args.name}\n")
            out.write(f"SF:{sf}\n")
            ordered = sorted(funcs, key=lambda n: (funcs[n], n))

            def fn_covered(name):
                if func_lines is not None:
                    return any((sf, l) in suite for l in func_lines[(sf, name)])
                return (sf, funcs[name]) in suite

            for name in ordered:
                out.write(f"FN:{funcs[name]},{name}\n")
            fnh = 0
            for name in ordered:
                hit = 1 if fn_covered(name) else 0
                fnh += hit
                out.write(f"FNDA:{hit},{name}\n")
            if funcs:
                out.write(f"FNF:{len(funcs)}\n")
                out.write(f"FNH:{fnh}\n")
            lh = 0
            for ln in sorted(lines):
                if (sf, ln) in suite:
                    out.write(f"DA:{ln},{app_count[(sf, ln)]}\n")  # tested
                    lh += 1
                else:
                    out.write(f"DA:{ln},0\n")                      # gap (red)
            out.write(f"LF:{len(lines)}\n")
            out.write(f"LH:{lh}\n")
            out.write("end_of_record\n")
            total += len(lines)
            covered += lh

    gap = total - covered
    pct = (100.0 * covered / total) if total else 0.0
    print(f"gap: app executes {total} baseline-scoped lines, {covered} also "
          f"covered by baseline ({pct:.1f}%); {gap} GAP lines (app runs, "
          f"baseline does not) -> {args.out}", file=sys.stderr)

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
            print(f"gap: HTML -> {args.html}/index.html (red = the gap)",
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
