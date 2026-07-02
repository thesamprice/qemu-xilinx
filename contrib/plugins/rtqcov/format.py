"""RTQCov1 binary artifact format: constants and reader.

The on-disk layout (all little-endian):

    struct rtqcov_header {
        char     magic[8];       # "RTQCov1\0"
        uint16_t version;
        uint16_t endian;         # 1 = little, 2 = big
        uint32_t header_size;
        uint32_t record_type;    # 1=TB_ADDR, 2=INSN_ADDR, 3=EDGE
        uint32_t flags;          # bit0 = HAS_COUNTS
        uint64_t record_count;
        uint64_t metadata_offset;
        uint64_t metadata_size;
        uint64_t records_offset;
        uint64_t records_size;
    };
    <UTF-8 JSON metadata>
    <records>                    # 8-byte addr, or 16-byte {addr,count} if HAS_COUNTS
"""

import json
import struct

MAGIC = b"RTQCov1\0"
HEADER_FMT = "<8sHHIIIQQQQQ"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
FLAG_HAS_COUNTS = 0x1
REC_TYPE = {1: "TB_ADDR", 2: "INSN_ADDR", 3: "EDGE"}


def read_cov(path):
    """Return (metadata_dict, [addresses], counts) from an RTQCov1 file.

    counts is None for plain coverage files, or a dict {addr: exec_count} when
    the file was written with counts mode (header flag FLAG_HAS_COUNTS, 16-byte
    {addr, count} records).
    """
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < HEADER_SIZE or data[:8] != MAGIC:
        raise ValueError(f"{path}: not an RTQCov1 file")
    fields = struct.unpack(HEADER_FMT, data[:HEADER_SIZE])
    flags = fields[5]
    meta_off, meta_size, rec_off, rec_size = fields[7], fields[8], fields[9], fields[10]
    meta = json.loads(data[meta_off:meta_off + meta_size].decode("utf-8"))

    if flags & FLAG_HAS_COUNTS:
        n = rec_size // 16
        flat = struct.unpack_from("<%dQ" % (2 * n), data, rec_off) if n else ()
        addrs = list(flat[0::2])
        counts = dict(zip(flat[0::2], flat[1::2]))
        return meta, addrs, counts

    n = rec_size // 8
    addrs = list(struct.unpack_from("<%dQ" % n, data, rec_off)) if n else []
    return meta, addrs, None
