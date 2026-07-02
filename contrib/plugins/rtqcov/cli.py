"""Unified `rtqcov` CLI dispatching to the subcommand modules."""

import argparse

from . import addr2line, coverable, lcov, merge, dump, restrict, gap
from . import __version__

# (subcommand name, module, short help). The module supplies add_arguments(p)
# and run(args); `symbolize`/`coverable` are the covered/coverable producers.
SUBCOMMANDS = [
    ("dump", dump, "inspect a .cov artifact (header/metadata/addresses)"),
    ("symbolize", addr2line, "covered .cov + ELF -> per-source-line JSONL"),
    ("coverable", coverable, "ELF -> coverable-line inventory JSONL"),
    ("lcov", lcov, "symbolized JSONL -> per-test LCOV .info"),
    ("merge", merge, "merge per-test .info -> aggregate (by source+line)"),
    ("restrict", restrict,
     "limit an aggregate .info to symbols present in a target ELF"),
    ("gap", gap,
     "lines an app executes that a baseline (suite) does not cover"),
]


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="rtqcov",
        description="Host-side tooling for RTQCov QEMU coverage artifacts.")
    parser.add_argument("--version", action="version",
                        version=f"rtqcov {__version__}")
    sub = parser.add_subparsers(dest="command", metavar="<command>")
    sub.required = True
    for name, mod, help_text in SUBCOMMANDS:
        sp = sub.add_parser(
            name, help=help_text, description=mod.__doc__,
            formatter_class=argparse.RawDescriptionHelpFormatter)
        mod.add_arguments(sp)
        sp.set_defaults(_run=mod.run)
    args = parser.parse_args(argv)
    return args._run(args)


if __name__ == "__main__":
    import sys
    sys.exit(main())
