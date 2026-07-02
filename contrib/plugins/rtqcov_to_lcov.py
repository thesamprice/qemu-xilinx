#!/usr/bin/env python3
"""Back-compat shim -> `rtqcov lcov` (rtqcov.lcov)."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rtqcov.lcov import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
