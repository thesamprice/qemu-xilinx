#!/usr/bin/env python3
"""Back-compat shim -> `rtqcov coverable` (rtqcov.coverable)."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rtqcov.coverable import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
