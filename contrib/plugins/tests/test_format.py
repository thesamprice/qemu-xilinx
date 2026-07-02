"""Tests for the RTQCov1 reader (rtqcov.format.read_cov)."""

import json
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from rtqcov.format import read_cov, MAGIC, HEADER_FMT, HEADER_SIZE  # noqa: E402


def build_cov(meta, records, has_counts):
    """Serialize an RTQCov1 buffer. records is [addr] or [(addr,count)]."""
    meta_bytes = json.dumps(meta).encode("utf-8")
    if has_counts:
        rec_bytes = b"".join(struct.pack("<QQ", a, c) for a, c in records)
        flags = 0x1
    else:
        rec_bytes = b"".join(struct.pack("<Q", a) for a in records)
        flags = 0x0
    meta_off = HEADER_SIZE
    rec_off = meta_off + len(meta_bytes)
    header = struct.pack(
        HEADER_FMT, MAGIC, 1, 1, HEADER_SIZE, 2, flags, len(records),
        meta_off, len(meta_bytes), rec_off, len(rec_bytes))
    return header + meta_bytes + rec_bytes


class TestReadCov(unittest.TestCase):
    def _roundtrip(self, buf):
        with tempfile.NamedTemporaryFile(suffix=".cov", delete=False) as f:
            f.write(buf)
            path = f.name
        try:
            return read_cov(path)
        finally:
            os.unlink(path)

    def test_plain_addresses(self):
        meta = {"mode": "tb-insn", "test_id": "t", "target_name": "microblazeel"}
        addrs = [0x80000000, 0x80000004, 0x80001000]
        m, a, c = self._roundtrip(build_cov(meta, addrs, has_counts=False))
        self.assertEqual(m["test_id"], "t")
        self.assertEqual(a, addrs)
        self.assertIsNone(c)

    def test_counts(self):
        meta = {"mode": "tb-insn", "counts_enabled": True}
        records = [(0x80000000, 5), (0x80000004, 12253), (0x8000a1f8, 1620147382)]
        m, a, c = self._roundtrip(build_cov(meta, records, has_counts=True))
        self.assertEqual(a, [r[0] for r in records])
        self.assertEqual(c, {a_: c_ for a_, c_ in records})
        self.assertEqual(c[0x8000a1f8], 1620147382)  # > 2**31, needs 64-bit

    def test_bad_magic(self):
        with self.assertRaises(ValueError):
            self._roundtrip(b"NOTRTQCV" + b"\0" * 100)


if __name__ == "__main__":
    unittest.main()
