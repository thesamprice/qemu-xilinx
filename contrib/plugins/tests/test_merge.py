"""Tests for aggregate merge-by-source (rtqcov.merge)."""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from rtqcov.merge import main as merge_main  # noqa: E402

INFO_A = """\
TN:a
SF:cpukit/x.c
FN:10,foo
FNDA:3,foo
DA:10,3
DA:11,0
end_of_record
"""

INFO_B = """\
TN:b
SF:cpukit/x.c
FN:10,foo
FNDA:2,foo
DA:10,2
DA:11,5
DA:12,0
end_of_record
"""


class TestMerge(unittest.TestCase):
    def test_sum_and_union(self):
        d = tempfile.mkdtemp()
        a = os.path.join(d, "a.info")
        b = os.path.join(d, "b.info")
        out = os.path.join(d, "agg.info")
        with open(a, "w") as f:
            f.write(INFO_A)
        with open(b, "w") as f:
            f.write(INFO_B)

        rc = merge_main([a, b, "--out", out, "--name", "agg"])
        self.assertEqual(rc, 0)

        da = {}
        with open(out) as f:
            for line in f:
                if line.startswith("DA:"):
                    ln, _, hits = line[3:].strip().partition(",")
                    da[int(ln)] = int(hits)
                elif line.startswith("FNDA:"):
                    hits, _, name = line[5:].strip().partition(",")
                    self.assertEqual((name, int(hits)), ("foo", 5))  # 3 + 2

        # coverable union = {10,11,12}; counts summed; line 12 coverable-not-hit.
        self.assertEqual(da, {10: 5, 11: 5, 12: 0})


if __name__ == "__main__":
    unittest.main()
