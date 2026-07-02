"""Inspect an RTQCov1 coverage artifact (header, metadata, addresses, counts)."""

import argparse
import json
import struct
import sys

from .format import MAGIC, HEADER_FMT, HEADER_SIZE, REC_TYPE, FLAG_HAS_COUNTS


def load(path):
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < HEADER_SIZE:
        raise ValueError(f"file too small ({len(data)} bytes) to be RTQCov")

    (magic, version, endian, header_size, record_type, flags, record_count,
     meta_off, meta_size, rec_off, rec_size) = struct.unpack(
        HEADER_FMT, data[:HEADER_SIZE])

    if magic != MAGIC:
        raise ValueError(f"bad magic {magic!r}, expected {MAGIC!r}")

    meta_raw = data[meta_off:meta_off + meta_size]
    try:
        meta = json.loads(meta_raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        meta = {"_parse_error": str(e), "_raw": meta_raw.decode("latin1")}

    has_counts = bool(flags & FLAG_HAS_COUNTS)
    if has_counts:
        n = rec_size // 16
        flat = struct.unpack_from("<%dQ" % (2 * n), data, rec_off) if n else ()
        addrs = list(flat[0::2])
        counts = list(flat[1::2])
    else:
        n = rec_size // 8
        addrs = list(struct.unpack_from("<%dQ" % n, data, rec_off)) if n else []
        counts = None

    header = {
        "version": version, "endian": endian, "header_size": header_size,
        "record_type": record_type,
        "record_type_name": REC_TYPE.get(record_type, "?"),
        "flags": flags, "has_counts": has_counts,
        "record_count": record_count,
        "metadata_size": meta_size, "records_size": rec_size,
    }
    return header, meta, addrs, counts


def add_arguments(parser):
    parser.add_argument("file")
    parser.add_argument("--all", action="store_true",
                        help="print every address")
    parser.add_argument("-n", type=int, default=10,
                        help="head/tail address count (default 10)")


def run(args):
    try:
        header, meta, addrs, counts = load(args.file)
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    print("== header ==")
    for k, v in header.items():
        print(f"  {k}: {v}")

    print("== metadata ==")
    print(json.dumps(meta, indent=2))

    print("== addresses ==")
    print(f"  count: {len(addrs)}")
    if addrs:
        print(f"  min: 0x{min(addrs):x}")
        print(f"  max: 0x{max(addrs):x}")
    if counts is not None:
        print(f"  counts: present (max={max(counts) if counts else 0}, "
              f"total={sum(counts)})")
        hottest = sorted(range(len(addrs)), key=lambda i: counts[i],
                         reverse=True)[:args.n]
        print("  hottest:")
        for i in hottest:
            print(f"    0x{addrs[i]:x}  count={counts[i]}")

    if header["record_count"] != len(addrs):
        print(f"  WARNING: header record_count={header['record_count']} "
              f"!= decoded {len(addrs)}")

    def fmt(i):
        return f"  0x{addrs[i]:x}" + (f"  count={counts[i]}" if counts else "")

    idxs = range(len(addrs))
    if args.all:
        for i in idxs:
            print(fmt(i))
    elif addrs:
        n = args.n
        for i in list(idxs)[:n]:
            print(fmt(i))
        if len(addrs) > 2 * n:
            print(f"  ... ({len(addrs) - 2 * n} more) ...")
            for i in list(idxs)[-n:]:
                print(fmt(i))
        elif len(addrs) > n:
            for i in list(idxs)[n:]:
                print(fmt(i))

    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    add_arguments(ap)
    return run(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
