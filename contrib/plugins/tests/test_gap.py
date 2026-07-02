"""Tests for the app-minus-baseline gap report (rtqcov.gap)."""

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from rtqcov.gap import main as gap_main  # noqa: E402

# App executes lines 10,11 (foo) and 20 (baz) in cpukit/x.c.
APP = [
    {"file": "cpukit/x.c", "line": 10, "function": "foo", "count": 4},
    {"file": "cpukit/x.c", "line": 11, "function": "foo", "count": 4},
    {"file": "cpukit/x.c", "line": 20, "function": "baz", "count": 9},
]

# The suite (baseline) covers line 10 but NOT 11 or 20 -> 11 and 20 are the gap.
BASELINE = """\
TN:suite
SF:cpukit/x.c
FN:10,foo
FNDA:1,foo
DA:10,1
DA:11,0
end_of_record
"""


class TestGap(unittest.TestCase):
    def test_app_minus_baseline(self):
        d = tempfile.mkdtemp()
        app = os.path.join(d, "app.jsonl")
        base = os.path.join(d, "suite.info")
        out = os.path.join(d, "gap.info")
        with open(app, "w") as f:
            for r in APP:
                f.write(json.dumps(r) + "\n")
        with open(base, "w") as f:
            f.write(BASELINE)

        rc = gap_main(["--app", app, "--baseline", base, "--out", out])
        self.assertEqual(rc, 0)

        da = {}
        with open(out) as f:
            for line in f:
                if line.startswith("DA:"):
                    ln, _, hits = line[3:].strip().partition(",")
                    da[int(ln)] = int(hits)

        # Universe = app-executed lines {10,11,20}.
        self.assertEqual(set(da), {10, 11, 20})
        # Line 10 covered by baseline -> hit (app count 4). 11 and 20 are the
        # gap -> 0 (red).
        self.assertEqual(da[10], 4)
        self.assertEqual(da[11], 0)
        self.assertEqual(da[20], 0)


if __name__ == "__main__":
    unittest.main()
