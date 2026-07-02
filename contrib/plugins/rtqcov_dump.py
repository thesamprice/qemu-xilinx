#!/usr/bin/env python3
"""Back-compat shim -> `rtqcov dump` (rtqcov.dump)."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rtqcov.dump import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
