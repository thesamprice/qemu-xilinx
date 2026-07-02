"""Back-compat shim. The implementation moved into the `rtqcov` package.

Existing code that does `from rtqcov_common import read_cov, normalize_path,
run_addr2line, iter_covered_lines` keeps working. Prefer importing from the
`rtqcov` package directly in new code.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from rtqcov.format import (  # noqa: E402,F401
    read_cov, MAGIC, HEADER_FMT, HEADER_SIZE, FLAG_HAS_COUNTS)
from rtqcov.paths import (  # noqa: E402,F401
    normalize_path, DEFAULT_MARKERS, TESTSUITES_MARKER)
from rtqcov.symbolize import (  # noqa: E402,F401
    run_addr2line, iter_covered_lines, LINE_RE)
