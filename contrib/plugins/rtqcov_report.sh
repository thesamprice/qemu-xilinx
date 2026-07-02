#!/usr/bin/env bash
#
# rtqcov_report.sh - drive the full RTQCov coverage pipeline:
#
#   coverage/raw/*.cov
#     -> coverage/symbolized/*.jsonl   (rtqcov_addr2line.py)
#     -> coverage/lcov/per-test/*.info (rtqcov_to_lcov.py)
#     -> coverage/lcov/aggregate-<arch>.info (rtqcov_merge.py)
#     -> coverage/html/index.html      (genhtml)
#
# The ELF for each .cov is read from the .cov's own embedded metadata, so no
# separate manifest is required. addr2line is taken from --toolchain-prefix.
#
# Usage:
#   rtqcov_report.sh --raw-dir DIR --source-root RTEMS_SRC --out-dir DIR \
#       [--toolchain-prefix microblaze-rtems6-] [--arch microblaze] \
#       [--include-testsuites]
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RAW_DIR=""
SOURCE_ROOT=""
OUT_DIR=""
PREFIX="microblaze-rtems6-"
ARCH="microblaze"
INCLUDE_TS=""
ALL_PATHS=""
KEEP_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --raw-dir)            RAW_DIR="$2"; shift 2 ;;
    --source-root)        SOURCE_ROOT="$2"; shift 2 ;;
    --out-dir)            OUT_DIR="$2"; shift 2 ;;
    --toolchain-prefix)   PREFIX="$2"; shift 2 ;;
    --arch)               ARCH="$2"; shift 2 ;;
    --include-testsuites) INCLUDE_TS="--include-testsuites"; shift ;;
    --all-paths)          ALL_PATHS="--all-paths"; shift ;;
    --keep)               KEEP_ARGS+=(--keep "$2"); shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

[[ -n "$RAW_DIR" && -n "$OUT_DIR" ]] || {
  echo "usage: rtqcov_report.sh --raw-dir DIR --out-dir DIR [--source-root SRC]" >&2
  echo "       [--all-paths] [--keep MARKER ...] [--include-testsuites]" >&2
  exit 2
}

# --source-root is optional; without it, --all-paths (absolute paths) or marker
# matching must supply usable SF paths.
SRC_OPT=()
[[ -n "$SOURCE_ROOT" ]] && SRC_OPT=(--source-root "$SOURCE_ROOT")

SYM_DIR="$OUT_DIR/symbolized"
CAB_DIR="$OUT_DIR/coverable"
PT_DIR="$OUT_DIR/lcov/per-test"
AGG="$OUT_DIR/lcov/aggregate-$ARCH.info"
HTML_DIR="$OUT_DIR/html"
mkdir -p "$SYM_DIR" "$CAB_DIR" "$PT_DIR" "$HTML_DIR"

read_meta() {  # read_meta <cov> <key>
  python3 -c "import json,struct,sys
d=open(sys.argv[1],'rb').read()
h=struct.unpack('<8sHHIIIQQQQQ',d[:struct.calcsize('<8sHHIIIQQQQQ')])
m=json.loads(d[h[7]:h[7]+h[8]].decode())
print(m.get(sys.argv[2],''))" "$1" "$2"
}

shopt -s nullglob
covs=("$RAW_DIR"/*.cov)
[[ ${#covs[@]} -gt 0 ]] || { echo "no .cov files in $RAW_DIR" >&2; exit 1; }

for cov in "${covs[@]}"; do
  base="$(basename "$cov" .cov)"
  elf="$(read_meta "$cov" elf)"
  if [[ ! -f "$elf" ]]; then
    echo "WARNING: ELF not found for $base ($elf); skipping" >&2
    continue
  fi
  # covered lines (what ran)
  python3 "$HERE/rtqcov_addr2line.py" --cov "$cov" --elf "$elf" \
    --toolchain-prefix "$PREFIX" "${SRC_OPT[@]}" \
    --arch "$ARCH" $INCLUDE_TS $ALL_PATHS "${KEEP_ARGS[@]}" \
    --out "$SYM_DIR/$base.jsonl"
  # coverable lines (DWARF inventory; cached per ELF since it is test-agnostic)
  cab="$CAB_DIR/$(echo "$elf" | sed 's#[/.]#_#g').jsonl"
  if [[ ! -s "$cab" ]]; then
    python3 "$HERE/rtqcov_dwarf_lines.py" --elf "$elf" \
      --toolchain-prefix "$PREFIX" "${SRC_OPT[@]}" \
      --arch "$ARCH" $INCLUDE_TS $ALL_PATHS "${KEEP_ARGS[@]}" --out "$cab"
  fi
  python3 "$HERE/rtqcov_to_lcov.py" "$SYM_DIR/$base.jsonl" \
    --coverable "$cab" --out "$PT_DIR/$base.info"
done

python3 "$HERE/rtqcov_merge.py" "$PT_DIR"/*.info --name "$ARCH" --out "$AGG"

# genhtml resolves relative SF paths against cwd; absolute paths (--all-paths)
# resolve anywhere, so fall back to '/' when no source root was given.
( cd "${SOURCE_ROOT:-/}" && genhtml "$AGG" --output-directory "$HTML_DIR" \
    --quiet --ignore-errors source )

echo "report: $HTML_DIR/index.html"
echo "aggregate: $AGG"
