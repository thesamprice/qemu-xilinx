"""addr2line driver and the shared covered/coverable symbolization core."""

import re
import subprocess

from .paths import normalize_path

LINE_RE = re.compile(r"^(\d+)")  # leading integer of "123 (discriminator 1)"


def run_addr2line(addr2line, elf, addrs):
    """Yield (address, [(function, file, line_str), ...]) per input address.

    Uses a single batched `addr2line -a -f -C -i` call. The -a flag prints the
    queried address (0x...) as a group delimiter; -i emits inlined frames
    (innermost first). '??'/missing frames are passed through for the caller
    to filter.
    """
    cmd = [addr2line, "-a", "-f", "-C", "-i", "-e", elf]
    stdin = "".join("0x%x\n" % a for a in addrs)
    proc = subprocess.run(cmd, input=stdin, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"{addr2line} failed: {proc.stderr.strip()}")

    lines = proc.stdout.splitlines()
    cur_addr = None
    frames = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("0x"):
            if cur_addr is not None:
                yield cur_addr, frames
            cur_addr = int(line, 16)
            frames = []
            i += 1
            continue
        func = line
        fileline = lines[i + 1] if i + 1 < len(lines) else "??:?"
        fpath, _, lno = fileline.rpartition(":") if ":" in fileline else (fileline, "", "?")
        frames.append((func, fpath, lno))
        i += 2
    if cur_addr is not None:
        yield cur_addr, frames


def iter_covered_lines(addr2line, elf, addrs, source_root, include_testsuites,
                       extra_markers=(), all_paths=False):
    """Yield (norm_file, line:int, function, depth:int, address) for covered/coverable.

    Shared core used by both the covered and coverable producers: normalize,
    drop '??' and non-source frames, parse the integer line number.
    """
    for addr, frames in run_addr2line(addr2line, elf, addrs):
        for depth, (func, fpath, lno) in enumerate(frames):
            if func == "??":
                continue
            m = LINE_RE.match(lno)
            if not m:
                continue
            norm = normalize_path(fpath, source_root, include_testsuites,
                                  extra_markers, all_paths)
            if norm is None:
                continue
            yield norm, int(m.group(1)), func, depth, addr
