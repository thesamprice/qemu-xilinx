"""Shared argparse options used by the symbolize (addr2line) and coverable
(dwarf_lines) subcommands."""


def add_symbolize_args(parser):
    """Add the path/toolchain options common to symbolize and coverable."""
    parser.add_argument("--toolchain-prefix", default="microblaze-rtems6-",
                        help="toolchain prefix for objdump/addr2line "
                             "(default: microblaze-rtems6-)")
    parser.add_argument("--addr2line",
                        help="explicit addr2line path (overrides "
                             "--toolchain-prefix)")
    parser.add_argument("--source-root",
                        help="source root for relative path normalization")
    parser.add_argument("--include-testsuites", action="store_true",
                        help="keep testsuites/** lines (excluded by default)")
    parser.add_argument("--keep", action="append", default=[], metavar="MARKER",
                        help="extra 'keep from here' path substring (repeatable),"
                             " e.g. /myproject/ to keep non-RTEMS project code")
    parser.add_argument("--all-paths", action="store_true",
                        help="keep every source file by absolute path (for app "
                             "ELFs mixing RTEMS and project code in different "
                             "trees)")
    parser.add_argument("--arch", default="",
                        help="arch tag for output records (covered: defaults to "
                             "the .cov target_name when empty)")
