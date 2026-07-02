"""Tests for restricting an aggregate .info to a target ELF (rtqcov.restrict)."""

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from rtqcov.restrict import main as restrict_main  # noqa: E402

# Aggregate covers foo (cpukit/x.c) and bar (cpukit/y.c). The target ELF only
# contains foo, so bar (and its file) must be dropped entirely.
AGGREGATE = """\
TN:agg
SF:cpukit/x.c
FN:10,foo
FNDA:5,foo
DA:10,5
DA:11,3
DA:12,0
end_of_record
TN:agg
SF:cpukit/y.c
FN:40,bar
FNDA:2,bar
DA:40,2
DA:41,0
end_of_record
"""

# Target coverable: only foo's lines in cpukit/x.c (note line 12 is NOT in the
# target -> must be dropped even though the aggregate listed it).
TARGET_COVERABLE = [
    {"file": "cpukit/x.c", "line": 10, "function": "foo"},
    {"file": "cpukit/x.c", "line": 11, "function": "foo"},
]


class TestRestrict(unittest.TestCase):
    def test_drops_non_target_symbols(self):
        d = tempfile.mkdtemp()
        agg = os.path.join(d, "agg.info")
        cab = os.path.join(d, "target.jsonl")
        out = os.path.join(d, "restricted.info")
        with open(agg, "w") as f:
            f.write(AGGREGATE)
        with open(cab, "w") as f:
            for r in TARGET_COVERABLE:
                f.write(json.dumps(r) + "\n")

        rc = restrict_main(["--aggregate", agg, "--coverable", cab,
                            "--out", out, "--name", "qual"])
        self.assertEqual(rc, 0)

        sf = None
        da = {}
        fns = []
        for line in open(out):
            line = line.strip()
            if line.startswith("SF:"):
                sf = line[3:]
            elif line.startswith("DA:"):
                ln, _, hits = line[3:].partition(",")
                da[(sf, int(ln))] = int(hits)
            elif line.startswith("FN:"):
                fns.append(line[3:].partition(",")[2])

        # bar / cpukit/y.c dropped; foo kept with its counts.
        self.assertEqual(fns, ["foo"])
        self.assertEqual(da, {("cpukit/x.c", 10): 5, ("cpukit/x.c", 11): 3})
        # line 12 (in aggregate but not in target) must be gone.
        self.assertNotIn(("cpukit/x.c", 12), da)
        # nothing from y.c survives.
        self.assertFalse(any(k[0] == "cpukit/y.c" for k in da))


if __name__ == "__main__":
    unittest.main()
