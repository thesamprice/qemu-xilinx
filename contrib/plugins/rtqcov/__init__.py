"""rtqcov -- host-side tooling for RTQCov QEMU code-coverage artifacts.

The QEMU TCG plugin (qemu_rtems_cov.c) writes compact RTQCov1 .cov files; this
package turns them into symbolized, source-line LCOV/HTML coverage reports.

Public API:
    read_cov, normalize_path, run_addr2line, iter_covered_lines

CLI (also runnable as `python3 -m rtqcov`):
    rtqcov dump|symbolize|coverable|lcov|merge
"""

from .format import read_cov, MAGIC, FLAG_HAS_COUNTS
from .paths import normalize_path
from .symbolize import run_addr2line, iter_covered_lines

__version__ = "1.0.0"

__all__ = [
    "read_cov", "normalize_path", "run_addr2line", "iter_covered_lines",
    "MAGIC", "FLAG_HAS_COUNTS", "__version__",
]
