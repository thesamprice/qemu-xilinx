"""Symbolize an RTQCov1 .cov into per-source-line JSONL (covered lines).

Reads the covered addresses, runs them through a batched
`<prefix>addr2line -a -f -C -i -e <elf>`, parses function/file:line frames
(including inlined frames), normalizes paths, and emits one JSON object per
unique covered source line. Keyed on (file, line, function) -- NOT address --
so the same source line merges across test binaries.

Output JSONL fields: file, line, function, count, inlined, test_id, bsp, arch,
address.
"""

import argparse
import json
import sys

from .format import read_cov
from .symbolize import iter_covered_lines
from .cliargs import add_symbolize_args


def add_arguments(parser):
    parser.add_argument("--cov", required=True, help="input .cov (RTQCov1) file")
    parser.add_argument("--elf", required=True, help="matching ELF")
    add_symbolize_args(parser)
    parser.add_argument("--test-id", help="override test_id from .cov metadata")
    parser.add_argument("--bsp", help="override bsp from .cov metadata")
    parser.add_argument("--out", required=True, help="output .jsonl file")


def run(args):
    addr2line = args.addr2line or (args.toolchain_prefix + "addr2line")

    try:
        meta, addrs, counts = read_cov(args.cov)
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    test_id = args.test_id or meta.get("test_id", "")
    bsp = args.bsp or meta.get("bsp", "")
    arch = args.arch or meta.get("target_name", "")

    if not addrs:
        open(args.out, "w").close()
        print(f"{args.cov}: 0 addresses, wrote empty {args.out}", file=sys.stderr)
        return 0

    # (file, line, function) -> min frame depth seen (0 == leaf/non-inlined)
    seen = {}
    sample_addr = {}
    # Per-line execution count = MAX over the line's instruction addresses.
    # Within a basic block all instructions share the block's count, so max
    # gives the block hit count without inflating by instructions-per-line.
    # Without counts mode, every address contributes 1 -> count stays 1.
    line_count = {}
    try:
        for norm, line, func, depth, addr in iter_covered_lines(
                addr2line, args.elf, addrs, args.source_root,
                args.include_testsuites, tuple(args.keep), args.all_paths):
            key = (norm, line, func)
            if key not in seen or depth < seen[key]:
                seen[key] = depth
            sample_addr.setdefault(key, addr)
            c = counts.get(addr, 0) if counts else 1
            if c > line_count.get(key, 0):
                line_count[key] = c
    except (OSError, RuntimeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    with open(args.out, "w") as out:
        for key in sorted(seen):
            norm, line, func = key
            out.write(json.dumps({
                "file": norm, "line": line, "function": func,
                "count": line_count.get(key, 1),
                "inlined": seen[key] > 0,
                "test_id": test_id, "bsp": bsp, "arch": arch,
                "address": "0x%x" % sample_addr[key],
            }) + "\n")

    print(f"{args.cov}: {len(addrs)} addrs -> {len(seen)} covered source lines "
          f"-> {args.out}", file=sys.stderr)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    add_arguments(ap)
    return run(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
