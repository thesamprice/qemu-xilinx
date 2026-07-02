"""Tests for source-path normalization (rtqcov.paths.normalize_path)."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from rtqcov.paths import normalize_path  # noqa: E402

SRC = "/src/rtems"


class TestNormalizePath(unittest.TestCase):
    def test_contrib_prefix_preserved(self):
        # The bug this guards: '/cpukit/' marker must not eat the 'contrib/'
        # prefix. Build path with .. that resolves under contrib/cpukit.
        raw = "/src/rtems/build/mb/x/../../../contrib/cpukit/jffs2/nodelist.c"
        self.assertEqual(
            normalize_path(raw, source_root=SRC),
            "contrib/cpukit/jffs2/nodelist.c")

    def test_cpukit_kept(self):
        self.assertEqual(
            normalize_path("/src/rtems/cpukit/score/src/x.c", source_root=SRC),
            "cpukit/score/src/x.c")

    def test_bsps_kept(self):
        self.assertEqual(
            normalize_path("/src/rtems/bsps/microblaze/x.c", source_root=SRC),
            "bsps/microblaze/x.c")

    def test_testsuites_excluded_by_default(self):
        p = "/src/rtems/testsuites/sptests/sp01/init.c"
        self.assertIsNone(normalize_path(p, source_root=SRC))
        self.assertEqual(
            normalize_path(p, source_root=SRC, include_testsuites=True),
            "testsuites/sptests/sp01/init.c")

    def test_newlib_excluded(self):
        # Not under the source root and no cpukit/bsps marker -> dropped.
        self.assertIsNone(
            normalize_path("/opt/toolchain/newlib/libc/foo.c", source_root=SRC))

    def test_unknown_under_root_excluded(self):
        # Under the source root but not an OS tree -> dropped (no marker).
        self.assertIsNone(
            normalize_path("/src/rtems/spec/build/x.yml", source_root=SRC))

    def test_marker_fallback_without_source_root(self):
        raw = "/anywhere/cpukit/rtems/src/taskcreate.c"
        self.assertEqual(normalize_path(raw),
                         "cpukit/rtems/src/taskcreate.c")

    def test_extra_marker_keeps_project_code(self):
        raw = "/work/navcube/src/app.c"
        self.assertEqual(
            normalize_path(raw, extra_markers=("/navcube/",)),
            "navcube/src/app.c")

    def test_all_paths_keeps_absolute(self):
        raw = "/work/navcube/src/app.c"
        out = normalize_path(raw, all_paths=True)
        self.assertTrue(out.startswith("/"))
        self.assertTrue(out.endswith("navcube/src/app.c"))

    def test_all_paths_excludes_testsuites(self):
        self.assertIsNone(
            normalize_path("/x/testsuites/y/z.c", all_paths=True))


if __name__ == "__main__":
    unittest.main()
