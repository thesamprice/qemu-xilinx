"""ELF -> coverable-line inventory JSONL (the denominator for real coverage).

A source line is "coverable" iff at least one EXECUTABLE instruction address in
the ELF maps to it via the DWARF line table -- a conservative definition that
avoids DWARF artifacts. Implementation: disassemble the ELF (`<prefix>objdump
-d`, which emits only executable sections) to enumerate every instruction
address, then resolve them through the DWARF line table with the SAME batched
addr2line and SAME path normalization as the covered side, so coverable and
covered keys are guaranteed comparable.

Output JSONL fields: file, line, function, arch, address.
"""

import argparse
import json
import re
import subprocess
import sys

from .symbolize import iter_covered_lines
from .cliargs import add_symbolize_args

# objdump disassembly line: optional leading space, hex address, ':', tab/space.
OBJDUMP_ADDR_RE = re.compile(r"^[ ]*([0-9a-fA-F]+):[ \t]")


def disassemble_addresses(objdump, elf):
    """Return a sorted list of unique instruction addresses in exec sections."""
    proc = subprocess.run([objdump, "-d", elf],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"{objdump} failed: {proc.stderr.strip()}")
    addrs = set()
    for line in proc.stdout.splitlines():
        m = OBJDUMP_ADDR_RE.match(line)
        if m:
            addrs.add(int(m.group(1), 16))
    return sorted(addrs)


def add_arguments(parser):
    parser.add_argument("--elf", required=True, help="ELF to inventory")
    add_symbolize_args(parser)
    parser.add_argument("--objdump", help="explicit objdump path")
    parser.add_argument("--out", required=True, help="output coverable .jsonl")


def run(args):
    objdump = args.objdump or (args.toolchain_prefix + "objdump")
    addr2line = args.addr2line or (args.toolchain_prefix + "addr2line")

    try:
        addrs = disassemble_addresses(objdump, args.elf)
    except (OSError, RuntimeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if not addrs:
        open(args.out, "w").close()
        print(f"{args.elf}: no instruction addresses", file=sys.stderr)
        return 0

    # (file, line, function) -> representative address. A line is coverable if
    # ANY executable address maps to it.
    seen = {}
    try:
        for norm, line, func, _depth, addr in iter_covered_lines(
                addr2line, args.elf, addrs, args.source_root,
                args.include_testsuites, tuple(args.keep), args.all_paths):
            seen.setdefault((norm, line, func), addr)
    except (OSError, RuntimeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    with open(args.out, "w") as out:
        for key in sorted(seen):
            norm, line, func = key
            out.write(json.dumps({
                "file": norm, "line": line, "function": func,
                "arch": args.arch, "address": "0x%x" % seen[key],
            }) + "\n")

    files = len({k[0] for k in seen})
    print(f"{args.elf}: {len(addrs)} instr addrs -> {len(seen)} coverable "
          f"lines across {files} files -> {args.out}", file=sys.stderr)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    add_arguments(ap)
    return run(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
