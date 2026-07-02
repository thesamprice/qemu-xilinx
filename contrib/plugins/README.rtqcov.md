# RTQCov — Non-Intrusive RTEMS Code Coverage via a QEMU TCG Plugin

RTQCov collects code coverage for RTEMS test binaries running under QEMU system
emulation **without** instrumenting the guest, recompiling with `--coverage`, or
producing huge instruction traces. A small QEMU TCG plugin records which guest
code addresses execute and writes one compact binary artifact per run; host-side
Python tools then symbolize against DWARF, build a coverable-line denominator,
and merge everything into per-test and aggregate LCOV/HTML reports.

```
RTEMS test ELF
  → QEMU system emulator + TCG plugin   →  compact .cov  (covered addresses)
  → addr2line symbolization (DWARF)      →  covered .jsonl
  → objdump + addr2line                  →  coverable .jsonl  (the denominator)
  → LCOV emit + merge by source+line     →  per-test / aggregate .info
  → genhtml                              →  browsable HTML
```

Everything lives in `contrib/plugins/`.

---

## Installation

This tooling ships **inside this QEMU fork** and is **not published to PyPI** —
you install it from a local clone.

**Prerequisites**
- This QEMU fork, configured with `--enable-plugins` and built (gives
  `build/qemu-system-microblazeel`).
- A cross toolchain providing `<prefix>addr2line` and `<prefix>objdump` on
  `PATH` (e.g. `microblaze-rtems6-`, typically under
  `~/local/opt/rtems/6/bin`).
- `genhtml` (from the `lcov` package) for the HTML report.
- Python ≥ 3.8 (the package is pure standard library — no third-party deps).

**1. Clone and build the coverage plugin**
```bash
git clone <your-qemu-xilinx-remote> qemu-xilinx
cd qemu-xilinx
# ...configure with --enable-plugins and build QEMU once...
make -C contrib/plugins BUILD_DIR="$PWD/build" libqemu_rtems_cov.so
```

**2. Install the `rtqcov` host tools** — pick one:

- **Editable install (recommended)** — puts an `rtqcov` command on your `PATH`;
  edits to the clone take effect immediately:
  ```bash
  pip install -e contrib/plugins        # or: pipx install -e ./contrib/plugins
  rtqcov --help
  ```
- **Plain local install**: `pip install ./contrib/plugins`
- **No install at all** — run it in place:
  ```bash
  cd contrib/plugins && python3 -m rtqcov --help
  # or from anywhere:
  PYTHONPATH=/path/to/qemu-xilinx/contrib/plugins python3 -m rtqcov --help
  ```

If your system Python is externally managed (PEP 668 error), use a venv or
`pipx`:
```bash
python3 -m venv ~/.venvs/rtqcov
~/.venvs/rtqcov/bin/pip install -e contrib/plugins
```

> The driver `rtqcov_report.sh` and the integration scripts (`run_qemu.sh`,
> the rtems_builder2 tester) call the in-tree scripts by path via `COV_TOOLS`,
> so **they need no `pip install`** — installing is only for the convenience
> `rtqcov` command.

**3. Verify**
```bash
python3 -m unittest discover -s contrib/plugins/tests   # 14 tests
rtqcov dump some.cov                                     # or: python3 -m rtqcov dump some.cov
```

---

## 1. Why merge by source+line, not address

Each RTEMS test statically links the OS, so the *same* `cpukit`/`bsps` source
line lands at *different* addresses in different test ELFs (and different BSPs).
Address-based merging would therefore be wrong. RTQCov normalizes every covered
and coverable address to a repo-relative **(source path, line)** key and merges
on that identity. The covered and coverable sides share one symbolizer and one
path normalizer (the `rtqcov` package), so their keys are guaranteed comparable.

---

## 2. The QEMU plugin

`qemu_rtems_cov.c` → `libqemu_rtems_cov.so` (built alongside the other example
plugins; it is listed in `contrib/plugins/Makefile`).

**Build:**
```bash
make -C contrib/plugins \
     BUILD_DIR=/local/home/sprice5/src/qemu-xilinx/build \
     libqemu_rtems_cov.so
```
(The `BUILD_DIR=` override lets the standalone Makefile find `config-host.mak`.)

**Use:**
```bash
qemu-system-microblazeel ... \
  -plugin ./libqemu_rtems_cov.so,out=test.cov,mode=tb-insn,\
test_id=sp01,bsp=sc3m_navcube,elf=/path/sp01.exe,verbose=1
```

**Plugin arguments** (`key=value`, comma-separated):

| arg | default | meaning |
|---|---|---|
| `out=` | `qemu-rtems.cov` | output artifact path (written atomically via `.tmp`+rename) |
| `mode=` | `tb-insn` | `tb-insn` = emit every in-range instruction address of each executed TB; `tb` = only TB start addresses |
| `filter=` | *(none)* | `0xSTART-0xEND[,…]` address ranges to record; empty = everything |
| `test_id=`,`bsp=`,`elf=` | "" | metadata strings copied into the file |
| `counts=1` | off | record 64-bit per-block execution counts (hotspots) |
| `verbose=1` | off | log a one-line summary at exit |

### Hit counts (hotspot mode)

With `counts=1` the plugin records how many times each block executed and the
whole pipeline carries it through to `DA:<line>,<count>` in LCOV, so the same
HTML report doubles as a hotspot view (genhtml colors/sorts by execution
count). The tester enables this by default (`run_qemu_bsp.sh --coverage`)
because counts are a strict superset of hit/miss coverage: a covered line is
just `count > 0`, so coverage and percentages are unchanged. Notes:

- Granularity is the translation block: every instruction in a block shares
  the block's count, and a line's count is the **max** over its instruction
  addresses (the block hit count) — not inflated by instructions-per-line.
- The plugin counter is 64-bit (a spin/idle loop easily exceeds 2^32).
- On `rtqcov_merge.py`, counts are **summed** across tests, so the aggregate
  shows total executions per line across the suite. (Without `counts`, the
  merge sums the per-test `1`s, i.e. the number of tests covering each line;
  coverage percentages are identical either way.)
- The on-disk format sets header flag `HAS_COUNTS` and uses 16-byte
  `{addr, count}` records; plain coverage files are unchanged (8-byte addrs).
  `rtqcov_dump.py` prints the hottest addresses when counts are present.

**How it works.** A TB-translation callback records each block (and, in
`tb-insn` mode, its in-range instruction addresses); a minimal TB-execution
callback atomically marks the block executed. At QEMU exit, executed blocks are
expanded to addresses, sorted, de-duplicated, and written. Output size tracks
*unique code covered*, not runtime length. It mirrors the structure of the
in-tree `drcov.c`; note this QEMU's `tb_trans` callback carries **no userdata**,
so the plugin uses a single global state object.

**Flush on signal.** The RTEMS tests don't exit QEMU cleanly — the runner sends
`SIGTERM` after `*** END OF TEST`. QEMU runs the plugin's atexit/exit-notifier on
that shutdown, so the `.cov` is still written (verified). No special handler is
needed.

### File format (`RTQCov1`)

`rtqcov_dump.py` inspects it. Layout (all little-endian):

```
struct rtqcov_header {            # 64 bytes
  char     magic[8];              # "RTQCov1\0"
  uint16_t version; uint16_t endian;
  uint32_t header_size; uint32_t record_type;   # 1=TB_ADDR, 2=INSN_ADDR
  uint32_t flags; uint64_t record_count;
  uint64_t metadata_offset, metadata_size;
  uint64_t records_offset, records_size;
};
<UTF-8 JSON metadata>            # mode, target_name, test_id, bsp, elf, filters…
<record_count × uint64>          # sorted, unique addresses
```

```bash
python3 rtqcov_dump.py test.cov          # header + metadata + address sample
python3 rtqcov_dump.py --all test.cov    # every address
```

---

## 3. Host-side tools

The host-side logic is the **`rtqcov` Python package** (pure stdlib, no deps).
Use it three ways:

```bash
python3 -m rtqcov <command> ...                 # no install, from contrib/plugins/
pip install -e contrib/plugins && rtqcov ...    # installs a `rtqcov` command
python3 rtqcov_dump.py ...                       # legacy script names still work (thin shims)
```

Commands: `dump`, `symbolize` (covered), `coverable`, `lcov`, `merge`. The old
`rtqcov_*.py` scripts remain as thin shims, and `rtqcov_report.sh` (unchanged)
still drives the whole pipeline, so existing callers keep working.

Package layout (`contrib/plugins/`): `rtqcov/{format,paths,symbolize}.py` hold
the shared `.cov` reader, `normalize_path`, and the `addr2line` driver;
`rtqcov/{addr2line,coverable,lcov,merge,dump}.py` are the subcommands; `cli.py`
is the dispatcher. Path normalization keeps `cpukit/**`, `bsps/**`,
`contrib/**` (e.g. `contrib/cpukit/jffs2`, `cpukit/posix-newlib/*` are real
RTEMS files) and **excludes** `testsuites/**` (unless `--include-testsuites`),
toolchain newlib, crt objects, and unknown system paths. See §3.6 for
`--all-paths` / `--keep` (application ELFs).

The sections below describe each tool by its legacy script name; the same logic
is `rtqcov <command>` / `python3 -m rtqcov <command>`.

### 3.1 `rtqcov_addr2line.py` — covered lines
`.cov` + ELF → JSONL, one object per unique covered `(file, line, function)`.
Runs all addresses through a single batched
`addr2line -a -f -C -i` (the `-a` flag prints each `0x…` as a group delimiter;
`-i` preserves inlined frames).
```bash
python3 rtqcov_addr2line.py --cov sp01.cov --elf sp01.exe \
  --toolchain-prefix microblaze-rtems6- --source-root /path/to/rtems \
  --out sp01.covered.jsonl
```

### 3.2 `rtqcov_dwarf_lines.py` — coverable lines (the denominator)
ELF → JSONL of every **coverable** source line. Conservative definition: a line
is coverable iff ≥1 executable instruction address maps to it. Implementation is
dependency-free — disassemble executable sections (`objdump -d`) to enumerate
instruction addresses, then resolve them through `.debug_line` with the **same**
`addr2line` + normalizer as covered lines, guaranteeing `covered ⊆ coverable`
with no path drift. (`pyelftools` is the upgrade path only if discriminator /
`is_stmt` precision is ever needed.)
```bash
python3 rtqcov_dwarf_lines.py --elf sp01.exe \
  --toolchain-prefix microblaze-rtems6- --source-root /path/to/rtems \
  --arch microblaze --out sp01.coverable.jsonl
```

### 3.3 `rtqcov_to_lcov.py` — per-test `.info`
Covered JSONL (+ `--coverable` JSONL) → LCOV. With `--coverable` it emits
`DA:line,1` for hit and `DA:line,0` for coverable-not-hit, so genhtml shows true
percentages (covered/coverable). Without it, covered-only (reads 100%).
```bash
python3 rtqcov_to_lcov.py sp01.covered.jsonl \
  --coverable sp01.coverable.jsonl --out sp01.info
```

### 3.4 `rtqcov_merge.py` — aggregate `.info`
Many per-test `.info` → one aggregate, merged by source path + line. Tracks
coverable vs covered separately so real percentages survive: a line is covered
in the aggregate if **any** test covered it; coverable if **any** test lists it.
```bash
python3 rtqcov_merge.py coverage/lcov/per-test/*.info \
  --name microblaze --out coverage/lcov/aggregate-microblaze.info
```

### 3.5 `rtqcov_report.sh` — one-command driver
Runs the whole chain over a directory of `.cov` files. Reads each ELF from the
`.cov`'s own metadata (no manifest), then symbolizes, builds coverable
inventories (cached per ELF), emits per-test LCOV, merges, and runs `genhtml`.
```bash
rtqcov_report.sh --raw-dir coverage/raw \
  --source-root /path/to/rtems --out-dir coverage \
  --toolchain-prefix microblaze-rtems6- --arch microblaze \
  [--include-testsuites]
```
genhtml is invoked from the source root so relative `SF:` paths resolve.
Produces:
```
coverage/raw/*.cov
coverage/symbolized/*.jsonl
coverage/coverable/*.jsonl
coverage/lcov/per-test/*.info
coverage/lcov/aggregate-<arch>.info
coverage/html/index.html
```

### 3.6 Path selection (RTEMS-only vs. application ELFs)

By default the tools keep only the RTEMS OS trees (`cpukit/`, `bsps/`,
`contrib/`) and normalize to repo-relative paths so the same source merges
across many test ELFs. For an **application ELF that mixes RTEMS and project
code** living in different trees (e.g. a cFS/RKI image), two options widen the
path filter (on `rtqcov_addr2line.py`, `rtqcov_dwarf_lines.py`, and
`rtqcov_report.sh`):

- `--all-paths` — keep **every** source file by its absolute path; genhtml
  renders each from its real on-disk location, so RTEMS and project code both
  appear. Best for single-ELF reports (absolute paths defeat cross-ELF
  merge-by-source).
- `--keep <marker>` (repeatable) — add a "keep from here" path substring (e.g.
  a project directory name) so that tree is kept and normalized relative to the
  marker, while RTEMS stays repo-relative.

With `--all-paths`, `--source-root` is optional.

### 3.7 `rtqcov restrict` — limit a report to a target ELF (qualification)

Take a finished aggregate `.info` and keep coverage **only for the symbols/lines
present in a given target ELF**, dropping everything else. The common use is
qualification: point `--elf` at a deliverable binary to see how well the test
campaign covered just the code that ships in it.

```bash
rtqcov restrict \
  --aggregate coverage/lcov/aggregate-microblaze.info \
  --elf /path/to/target.elf \
  --source-root /path/to/rtems-used-to-build-target \
  --out restricted.info --html restrict_html
# -> restrict_html/index.html
```

How it works: it builds the target's coverable inventory (same normalizer as the
aggregate) and intersects — a function not in the target contributes no
coverable lines, so it drops out. It is **filter-only**: covered counts and the
surviving denominator come from the aggregate; target lines the suite never had
are *not* added (so `restricted ⊆ aggregate`). The percentage typically *rises*,
because the denominator shrinks to just the target's code. Scope follows the
usual path flags (default RTEMS-OS-only; add `--all-paths`/`--keep` for app
code). Pass `--coverable target.cab.jsonl` to reuse a precomputed inventory and
skip the objdump/addr2line pass.

### 3.8 `rtqcov gap` — code an app runs that the suite doesn't cover

The inverse view: run your application, then see the **RTEMS code the app
executes that the test suite never covers** — the untested-but-used paths.

```bash
rtqcov gap \
  --cov app.cov --elf app.elf \
  --baseline coverage/lcov/aggregate-microblaze.info \
  --source-root /path/to/rtems-used-to-build-app \
  --out gap.info --html gap_html
# -> gap_html/index.html : RED lines = the gap (app runs them, suite doesn't)
```

It computes the set difference (app-covered − baseline-covered). The report's
universe is the **app-executed** lines; a line is "covered" iff the baseline
also covers it, so in the HTML the **uncovered (red) lines are the gap** and the
percentage is "how much of the app-executed code the suite covers." Both sides
must share normalization — symbolize the app with the same path flags as the
baseline (default RTEMS-OS-only) and pass the app's RTEMS source root so
`contrib/` paths line up. Instead of `--cov/--elf` you can pass a precomputed app
report with `--app app.jsonl` (symbolized) or `--app app.info`.

`restrict` and `gap` are complementary: `restrict` answers "how well is the code
in my binary tested?"; `gap` answers "what does my binary run that isn't tested?"

---

## 4. Running the full RTEMS test suite with coverage

Coverage is integrated into the existing rtems-test wrapper in
`~/src/rtems_builder2/tester/`. It is **opt-in** and changes nothing when off.

```bash
cd ~/src/rtems_builder2
./tester/run_qemu_bsp.sh --coverage --samples     # or --sptests / --all / <path>
```

`--coverage` always records per-line execution counts (the tester passes
`counts=1`), so the HTML report doubles as a hotspot view. Coverage is just
`count > 0`, so the covered set and percentages are identical to a no-counts
run -- only the magnitude of the non-zero counts varies run to run.

**What happens:**
1. `run_qemu_bsp.sh --coverage` exports `RTQCOV_DIR`, `RTQCOV_PLUGIN`,
   `RTQCOV_BSP`, `RTQCOV_MODE` and creates `<report>/coverage/raw/`.
2. `rtems-test` runs each test as usual, invoking `bsps/microblaze-qemu-run`.
   That runner sees `RTQCOV_DIR` and injects
   `-plugin libqemu_rtems_cov.so,out=<raw>/<test>.cov,…` with a **unique
   per-test** out file (safe under `--jobs > 1`).
3. After `rtems-test` finishes, the wrapper runs `rtqcov_report.sh` to produce
   per-test and aggregate LCOV + HTML under `<report>/coverage/`.

No edit to the BSP `.ini` is required — the env vars flow through `rtems-test`
to the per-test runner. The per-test runner also accepts explicit flags
(`--coverage-dir`, `--coverage-plugin`, `--coverage-bsp`, `--coverage-mode`) if
invoked directly.

**Coverage env overrides** (defaults shown) for `run_qemu_bsp.sh`:
```
COV_PLUGIN        /local/home/sprice5/src/qemu-xilinx/contrib/plugins/libqemu_rtems_cov.so
RTEMS_SRC         <project>/src/rtems
COV_TOOLCHAIN     microblaze-rtems6-
COV_TOOLCHAIN_BIN /home/sprice5/local/opt/rtems/6/bin   # addr2line/objdump dir, prepended to PATH
COV_ARCH          microblaze
COV_MODE          tb-insn
```

---

## 5. Multi-BSP / multi-arch

`rtqcov_merge.py` already merges arbitrary per-test `.info` by source identity,
so cross-BSP aggregation is just a matter of pointing it at multiple BSPs'
per-test outputs. The same `cpukit/**` source lines merge together across BSPs;
arch-specific `bsps/**` files stay distinct because their paths differ. Suggested
report set once a second BSP is built: `aggregate-all.info`,
`aggregate-<arch>.info`, plus `per-bsp/*.info`.

---

## 6. Known limitations

- **TB granularity.** In `tb-insn` mode a line is "covered" if its TB executed;
  a guard that skips part of a TB can over-report. Acceptable for OS-level
  completeness; add an instruction-execution mode if exact line precision is
  needed.
- **Coverable is conservative by design** — lines with no emitted instructions
  (fully optimized away) are not counted as coverable.
- **Missing sources.** A few `cpukit/posix-newlib/*.c` referenced by DWARF
  aren't on disk; they count in the denominator but genhtml can't render them
  (run with `--ignore-errors source`, as the driver does).
- **Dynamic loading** (cFS / `.so` modules) is not handled yet — that needs a
  runtime module map and is deferred until the static foundation is solid across
  BSPs.
