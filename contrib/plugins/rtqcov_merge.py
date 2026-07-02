#!/usr/bin/env python3
"""Back-compat shim -> `rtqcov merge` (rtqcov.merge)."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rtqcov.merge import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
