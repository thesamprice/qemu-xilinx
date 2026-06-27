/*
 * mb_cycles.c — QEMU plugin: MicroBlaze deterministic-cycle regression oracle
 *
 * PURPOSE
 * -------
 * Provide a deterministic, machine-readable cycle model for LLVM/GCC codegen
 * regression testing on MicroBlaze.  The goal is NOT hardware-cycle accuracy.
 * The goal is a reproducible measurement so that compiler changes produce clear
 * before/after diffs.
 *
 * TWO MEASUREMENT MODES
 * ---------------------
 * weighted  (stalls=off, default):
 *   Counts weighted instruction cycles from a fixed UG984 §5 table.  No stalls,
 *   no memory effects, no branch penalties.  Use as the primary regression baseline.
 *
 * raw-stalls  (stalls=on):
 *   Adds in-order RAW forwarding stalls on top of weighted cycles.  Tracks
 *   register readiness for all 32 GPRs.  Loads add 1 stall on a direct consumer
 *   (load result latency = 2 cycles, forwarded from MEM stage).  Blocking ops
 *   (idiv=34, FPU=per-func, mts/mfs=2) stall the pipeline for their base cost;
 *   no extra RAW stall after them.  Ignores WAW/WAR hazards, memory hierarchy,
 *   and branch penalties.
 *
 * BENCHMARK CONTROL (magic NOP instructions)
 * ------------------------------------------
 * Benchmarks delimit the measured hot region with "ori r0, r0, 0x4DXX" words.
 * Writing to r0 is always a hardware NOP on MicroBlaze.
 *
 *   .word 0xA0004D01   BENCH_RESTART — snapshot counters, begin measurement
 *   .word 0xA0004D02   BENCH_STOP    — compute delta, end measurement
 *   .word 0xA0004D03   BENCH_EXIT_PASS — emit JSON, status=pass
 *   .word 0xA0004D04   BENCH_EXIT_FAIL — emit JSON, status=fail
 *
 * Only cycles/instructions between BENCH_RESTART and BENCH_STOP are reported.
 * Magic instructions themselves are not counted as code instructions.
 *
 * COUNTERS EMITTED PER BENCHMARK REGION
 * --------------------------------------
 *   instructions              total instruction count
 *   weighted_cycles           sum of UG984 §5 instruction weights
 *   raw_stall_cycles          additional RAW stall cycles (stalls=on only)
 *   raw_cycles                weighted_cycles + raw_stall_cycles
 *   cpi_weighted              weighted_cycles / instructions
 *   cpi_raw                   raw_cycles / instructions
 *   alu_count, load_count, store_count, branch_count
 *   nop_count, div_count, mul_count, fpu_count, mts_mfs_count
 *   delayed_branch_count      branches with a delay slot (D-form)
 *   delay_slot_nop_count      wasted delay slots (branch + canonical NOP)
 *   delay_slot_filled_count   useful delay slots (branch + non-NOP)
 *   delay_slot_fill_rate      delay_slot_filled / delayed_branch
 *
 * PLUGIN ARGUMENTS
 * ----------------
 *   bench=NAME        benchmark name  (default: "unknown")
 *   compiler=NAME     compiler name   (default: "unknown")
 *   opt=FLAGS         optimisation flags  (default: "")
 *   target=CONFIG     target configuration  (default: "")
 *   commit=HASH       compiler commit hash  (default: "")
 *   stalls=on|off     enable RAW stall model  (default: off)
 *   area_opt=0|1|2    FPU latency variant  (default: 0)
 *   verbose=on|off    per-opcode breakdown appended to JSON  (default: off)
 *
 * OUTPUT
 * ------
 * One JSON object per benchmark run at BENCH_EXIT_PASS/FAIL.  If no
 * BENCH_RESTART is ever seen, one JSON record covering the full run is emitted
 * at plugin exit.
 *
 * NOT MODELLED
 * ------------
 * I-cache, D-cache, DDR/BRAM wait states, PLB/AXI bus, branch prediction,
 * taken/not-taken penalties, interrupts, DMA, timer effects.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/* =========================================================================
 * Instruction encoding
 * ========================================================================= */

/* ori r0, r0, IMM: (0x28<<26)|(0<<21)|(0<<16)|IMM16 = 0xA0000000 | IMM */
#define MB_ORI_R0_MASK  0xFFFF0000u
#define MB_ORI_R0_BASE  0xA0000000u
#define MB_MAGIC_MASK   0x0000FF00u
#define MB_MAGIC_BYTE   0x00004D00u  /* 'M' in the IMM high byte */

#define MBEV_RESTART    0x4D01u
#define MBEV_STOP       0x4D02u
#define MBEV_PASS       0x4D03u
#define MBEV_FAIL       0x4D04u

/* or r0, r0, r0 = 0x80000000: canonical delay-slot NOP */
#define MB_NOP_WORD     0x80000000u

/* =========================================================================
 * Weighted cost table  (UG984 v2026.1 §5, C_AREA_OPTIMIZED=0)
 * ========================================================================= */
static const uint8_t mb_weight[64] = {
    /* 0x00–0x0F: integer add/sub variants */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 0x10 mul  */ 1, /* 0x11 bsrl/a/l */ 1, /* 0x12 idiv */ 34, /* 0x13 getd */ 1,
    /* 0x14 rsv  */ 1, /* 0x15 rsv      */ 1, /* 0x16 fpu  */  6, /* 0x17 rsv  */ 1,
    /* 0x18 muli */ 1, /* 0x19 bsrli    */ 1, /* 0x1A rsv  */  1, /* 0x1B get  */ 1,
    /* 0x1C–0x1F reserved */ 1, 1, 1, 1,
    /* 0x20 or   */ 1, /* 0x21 and      */ 1, /* 0x22 xor  */  1, /* 0x23 andn */ 1,
    /* 0x24 sra  */ 1, /* 0x25 mts/mfs  */ 2, /* 0x26 br   */  1, /* 0x27 beq  */ 1,
    /* 0x28 ori  */ 1, /* 0x29 andi     */ 1, /* 0x2A xori */  1, /* 0x2B andni*/ 1,
    /* 0x2C imm  */ 1, /* 0x2D rtsd     */ 1, /* 0x2E bri  */  1, /* 0x2F beqi */ 1,
    /* 0x30–0x37: loads/stores */ 1, 1, 1, 1, 1, 1, 1, 1,
    /* 0x38–0x3F: loads/stores */ 1, 1, 1, 1, 1, 1, 1, 1,
};

/* Per-instruction FPU latency [area_opt 0/1/2][func bits[9:7]]:
 *  idx  func   name      opt=0  opt=1  opt=2
 *   0   0x000  fadd/frsub  4      6      1
 *   2   0x100  fmul        4      6      1
 *   3   0x180  fdiv       30     32     29
 *   4   0x200  fcmp.*      4      6      1
 *   5   0x280  flt         5      7      2
 *   6   0x300  fint        4      6      1
 *   7   0x380  fsqrt      27     29     23
 */
static const uint8_t fpu_latency[3][8] = {
    { 4,  4,  4, 30, 4, 5, 4, 27 },
    { 6,  6,  6, 32, 6, 7, 6, 29 },
    { 1,  1,  1, 29, 1, 2, 1, 23 },
};
static unsigned fpu_area_opt;

static inline uint8_t mb_fpu_lat(uint32_t word)
{
    return fpu_latency[fpu_area_opt][(word >> 7) & 7];
}

/*
 * RAW forwarding latency for the stall model.
 * 0 = no GPR written (store, branch, imm-prefix, reserved).
 * Blocking ops: latency == base cost → consumer starts right after → 0 extra stall.
 * FPU (0x16): handled separately via mb_fpu_lat().
 */
static const uint8_t mb_result_latency[64] = {
    1, 1, 1, 1, 1, 1, 1, 1,    /* 0x00–0x07 */
    1, 1, 1, 1, 1, 1, 1, 1,    /* 0x08–0x0F */
    1, 1,34, 1, 0, 0, 4, 0,    /* 0x10–0x17  mul=1, idiv=34, fpu=4(base, overridden) */
    1, 1, 0, 1, 0, 0, 0, 0,    /* 0x18–0x1F  muli=1 */
    1, 1, 1, 1, 1, 2, 1, 0,    /* 0x20–0x27  mts/mfs=2, beq=0 */
    1, 1, 1, 1, 0, 0, 1, 0,    /* 0x28–0x2F  imm=0, rtsd=0, bri writes LR=1 */
    2, 2, 2, 0, 0, 0, 0, 0,    /* 0x30–0x37  loads=2, stores=0 */
    2, 2, 2, 0, 0, 0, 0, 0,    /* 0x38–0x3F  loads=2, stores=0 */
};

/*
 * Opcodes where bits[25:21] are a SOURCE register, not the destination.
 * Store rD = data register; branch 0x27/0x2F rD field = condition register.
 */
static const bool mb_rd_is_src[64] = {
    [0x27] = true, [0x2F] = true,
    [0x34] = true, [0x35] = true, [0x36] = true,
    [0x3C] = true, [0x3D] = true, [0x3E] = true,
};

/* Human-readable opcode names for verbose output. */
static const char *const mb_op_name[64] = {
    "add",  "rsub",  "addc",  "rsubc",  "addk", "rsubk/cmp", "addkc", "rsubkc",
    "addi", "rsubi", "addic", "rsubic", "addik","rsubik","addikc","rsubikc",
    "mul","bsrl/a","idiv","getd","rsv","rsv","fpu","rsv",
    "muli","bsrli","rsv","get","rsv","rsv","rsv","rsv",
    "or","and","xor","andn","sra/ext","mts/mfs","br/brld","beq-bge",
    "ori","andi","xori","andni","imm","rtsd","bri/braid","beqi-bgei",
    "lbu","lhu","lw","rsv","sb","sh","sw","rsv",
    "lbui","lhui","lwi","rsv","sbi","shi","swi","rsv",
};

/* =========================================================================
 * Instruction class
 * ========================================================================= */
typedef enum {
    CLS_ALU, CLS_MUL, CLS_DIV, CLS_FPU,
    CLS_LOAD, CLS_STORE, CLS_BRANCH, CLS_MTS_MFS, CLS_NOP,
    CLS_OTHER
} MBClass;

static const uint8_t mb_class[64] = {
    /* 0x00–0x0F: integer ALU */
    CLS_ALU,CLS_ALU,CLS_ALU,CLS_ALU,CLS_ALU,CLS_ALU,CLS_ALU,CLS_ALU,
    CLS_ALU,CLS_ALU,CLS_ALU,CLS_ALU,CLS_ALU,CLS_ALU,CLS_ALU,CLS_ALU,
    /* 0x10 mul  */ CLS_MUL, /* 0x11 bsrl   */ CLS_ALU,
    /* 0x12 idiv */ CLS_DIV, /* 0x13 getd   */ CLS_OTHER,
    /* 0x14 rsv  */ CLS_OTHER, /* 0x15 rsv  */ CLS_OTHER,
    /* 0x16 fpu  */ CLS_FPU, /* 0x17 rsv    */ CLS_OTHER,
    /* 0x18 muli */ CLS_MUL, /* 0x19 bsrli  */ CLS_ALU,
    CLS_OTHER,CLS_OTHER,CLS_OTHER,CLS_OTHER,CLS_OTHER,CLS_OTHER,
    /* 0x20–0x27 */
    CLS_ALU,CLS_ALU,CLS_ALU,CLS_ALU,CLS_ALU,CLS_MTS_MFS,CLS_BRANCH,CLS_BRANCH,
    /* 0x28–0x2F */
    CLS_ALU,CLS_ALU,CLS_ALU,CLS_ALU,CLS_OTHER,CLS_BRANCH,CLS_BRANCH,CLS_BRANCH,
    /* 0x30–0x37 */
    CLS_LOAD,CLS_LOAD,CLS_LOAD,CLS_OTHER,CLS_STORE,CLS_STORE,CLS_STORE,CLS_OTHER,
    /* 0x38–0x3F */
    CLS_LOAD,CLS_LOAD,CLS_LOAD,CLS_OTHER,CLS_STORE,CLS_STORE,CLS_STORE,CLS_OTHER,
};

/* =========================================================================
 * Counter block — used for totals, snapshot, and bench delta
 * ========================================================================= */
typedef struct {
    uint64_t insns;
    uint64_t wcycles;    /* weighted cycles (no stalls)      */
    uint64_t scycles;    /* RAW stall cycles (stalls=on)     */
    /* per-class */
    uint64_t alu, mul, div_i, fpu, load, store, branch, mts_mfs, nop;
    /* delay slot quality */
    uint64_t delayed_branch;
    uint64_t delay_slot_nop;
    uint64_t delay_slot_filled;
    /* per-opcode */
    uint64_t op_count[64];
    uint64_t op_cycles[64];
} MBCounts;

static MBCounts total;   /* always-accumulating totals */
static MBCounts snap;    /* snapshot at BENCH_RESTART  */
static MBCounts bench;   /* delta (total - snap)       */

static uint64_t trans_count;

/* Pointer table: opcode → class counter in 'total'. Built at install time. */
static uint64_t *op_class_ctr[64];

/* =========================================================================
 * Stall model state
 * ========================================================================= */
static uint64_t reg_ready[32];
static uint64_t pipeline_cycle;

/* =========================================================================
 * Plugin configuration
 * ========================================================================= */
static bool model_stalls;
static bool verbose;
static bool big_endian_target;

static char bench_name[256]    = "unknown";
static char compiler_name[256] = "unknown";
static char opt_flags[512]     = "";
static char target_config[256] = "";
static char commit_hash[128]   = "";

/* =========================================================================
 * Measurement state machine
 * ========================================================================= */
typedef enum { ST_IDLE, ST_MEASURING, ST_STOPPED } MBState;
static MBState  state = ST_IDLE;
static bool     bench_event_seen;
static uint64_t bench_seq;   /* incremented on each EXIT_PASS or EXIT_FAIL */

/* =========================================================================
 * Helpers
 * ========================================================================= */
static inline uint32_t insn_word32(const uint8_t *data)
{
    if (big_endian_target)
        return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
             | ((uint32_t)data[2] <<  8) |  (uint32_t)data[3];
    return   ((uint32_t)data[3] << 24) | ((uint32_t)data[2] << 16)
             | ((uint32_t)data[1] <<  8) |  (uint32_t)data[0];
}

static inline const uint8_t *insn_bytes(const struct qemu_plugin_insn *insn)
{
    if (qemu_plugin_insn_size(insn) >= 4)
        return (const uint8_t *)qemu_plugin_insn_data(insn);
    return (const uint8_t *)qemu_plugin_insn_haddr(insn);
}

static inline bool is_bench_magic(uint32_t w)
{
    return (w & MB_ORI_R0_MASK) == MB_ORI_R0_BASE
        && (w & MB_MAGIC_MASK)  == MB_MAGIC_BYTE;
}

/* Return true if this is a delayed-branch (D-form) opcode. */
static inline bool is_delayed_branch(uint32_t w)
{
    uint8_t op = (w >> 26) & 0x3F;
    switch (op) {
    case 0x26: return (w >>  8) & 1;   /* brd/brad/brld/brald */
    case 0x27: return (w >> 20) & 1;   /* beqd-bged */
    case 0x2D: return true;            /* rtsd/rtid/rtbd/rted always have delay slot */
    case 0x2E: return (w >>  8) & 1;   /* brid/braid/brlid/bralid */
    case 0x2F: return (w >> 20) & 1;   /* beqid-bgeid */
    default:   return false;
    }
}

static inline uint64_t stall_for(uint8_t r, uint64_t now)
{
    return (r && reg_ready[r] > now) ? reg_ready[r] - now : 0;
}

/* =========================================================================
 * Bench state machine operations
 * ========================================================================= */
static void do_restart(void)
{
    memcpy(&snap, &total, sizeof(total));
    state = ST_MEASURING;
    bench_event_seen = true;
}

static void do_stop(void)
{
    if (state != ST_MEASURING)
        return;
    state = ST_STOPPED;

#define DELTA(f) bench.f = total.f - snap.f
    DELTA(insns); DELTA(wcycles); DELTA(scycles);
    DELTA(alu); DELTA(mul); DELTA(div_i); DELTA(fpu);
    DELTA(load); DELTA(store); DELTA(branch); DELTA(mts_mfs); DELTA(nop);
    DELTA(delayed_branch); DELTA(delay_slot_nop); DELTA(delay_slot_filled);
    for (int i = 0; i < 64; i++) {
        bench.op_count[i]  = total.op_count[i]  - snap.op_count[i];
        bench.op_cycles[i] = total.op_cycles[i] - snap.op_cycles[i];
    }
#undef DELTA
}

static void emit_json(const MBCounts *c, const char *status)
{
    uint64_t raw_cycles = c->wcycles + c->scycles;
    double cpi_w  = c->insns ? (double)c->wcycles     / c->insns          : 0.0;
    double cpi_r  = c->insns ? (double)raw_cycles     / c->insns          : 0.0;
    double fill   = c->delayed_branch
                  ? (double)c->delay_slot_filled / c->delayed_branch : 0.0;

    /* Emit in ~4 KB chunks via a local buffer; avoids heap allocation. */
    char buf[8192];
    int  n = 0;

#define A(fmt, ...) n += snprintf(buf + n, (int)sizeof(buf) - n, fmt, ##__VA_ARGS__)

    A("{\n");
    A("  \"benchmark\": \"%s\",\n",     bench_name);
    A("  \"compiler\": \"%s\",\n",      compiler_name);
    A("  \"opt\": \"%s\",\n",           opt_flags);
    A("  \"target_config\": \"%s\",\n", target_config);
    A("  \"compiler_commit\": \"%s\",\n", commit_hash);
    A("  \"status\": \"%s\",\n",        status);
    A("  \"stall_model\": \"%s\",\n",   model_stalls ? "raw-stalls" : "weighted");
    A("  \"instructions\": %" PRIu64 ",\n",      c->insns);
    A("  \"weighted_cycles\": %" PRIu64 ",\n",   c->wcycles);
    A("  \"raw_stall_cycles\": %" PRIu64 ",\n",  c->scycles);
    A("  \"raw_cycles\": %" PRIu64 ",\n",        raw_cycles);
    A("  \"cpi_weighted\": %.3f,\n",             cpi_w);
    A("  \"cpi_raw\": %.3f,\n",                  cpi_r);
    A("  \"alu_count\": %" PRIu64 ",\n",         c->alu);
    A("  \"load_count\": %" PRIu64 ",\n",        c->load);
    A("  \"store_count\": %" PRIu64 ",\n",       c->store);
    A("  \"branch_count\": %" PRIu64 ",\n",      c->branch);
    A("  \"nop_count\": %" PRIu64 ",\n",         c->nop);
    A("  \"div_count\": %" PRIu64 ",\n",         c->div_i);
    A("  \"mul_count\": %" PRIu64 ",\n",         c->mul);
    A("  \"fpu_count\": %" PRIu64 ",\n",         c->fpu);
    A("  \"mts_mfs_count\": %" PRIu64 ",\n",     c->mts_mfs);
    A("  \"delayed_branch_count\": %" PRIu64 ",\n",    c->delayed_branch);
    A("  \"delay_slot_nop_count\": %" PRIu64 ",\n",    c->delay_slot_nop);
    A("  \"delay_slot_filled_count\": %" PRIu64 ",\n", c->delay_slot_filled);
    A("  \"delay_slot_fill_rate\": %.3f,\n", fill);
    A("  \"bench_seq\": %" PRIu64, bench_seq);

    if (verbose) {
        A(",\n  \"opcode_breakdown\": [\n");
        bool first = true;
        for (int op = 0; op < 64; op++) {
            if (!c->op_count[op]) continue;
            if (!first) A(",\n");
            first = false;
            A("    {\"op\": \"0x%02x\", \"name\": \"%s\", \"count\": %" PRIu64
              ", \"cycles\": %" PRIu64 ", \"cpi\": %.2f}",
              op, mb_op_name[op], c->op_count[op], c->op_cycles[op],
              (double)c->op_cycles[op] / c->op_count[op]);
        }
        A("\n  ]");
    }

    A("\n}\n");
#undef A
    qemu_plugin_outs(buf);
}

/* =========================================================================
 * Execution callbacks
 * ========================================================================= */

/*
 * Bench event callback — registered on magic NOP instructions in both modes.
 * Userdata: low byte = event nibble (MBEV_xxx & 0xFF).
 */
static void vcpu_insn_exec_bench(unsigned int vcpu_idx, void *userdata)
{
    uint8_t ev = (uint8_t)(uintptr_t)userdata;
    switch (ev) {
    case (MBEV_RESTART & 0xFF): do_restart(); break;
    case (MBEV_STOP    & 0xFF): do_stop();    break;
    case (MBEV_PASS    & 0xFF):
        emit_json(&bench, "pass");
        bench_seq++;
        state = ST_IDLE;
        break;
    case (MBEV_FAIL    & 0xFF):
        emit_json(&bench, "fail");
        bench_seq++;
        state = ST_IDLE;
        break;
    }
}

/*
 * RAW-stall callback for normal instructions (stalls=on).
 *
 * Packed metadata in userdata (uintptr_t, 64-bit host):
 *   bits [ 5: 0]  op          6-bit opcode
 *   bits [10: 6]  rd          bits[25:21]
 *   bits [15:11]  ra          bits[20:16]
 *   bits [20:16]  rb          bits[15:11]
 *   bit  [21]     type_b      1 = no Rb field
 *   bit  [22]     rd_is_src   1 = rd is also a source
 *   bit  [23]     valid       1 = instruction bytes were available
 *   bits [31:24]  base_cyc    pre-computed weighted cost
 *   bit  [32]     is_nop      1 = canonical NOP word
 */
static void vcpu_insn_exec_stall(unsigned int vcpu_idx, void *userdata)
{
    uintptr_t meta   = (uintptr_t)userdata;
    uint8_t op       = (meta >>  0) & 0x3F;
    uint8_t rd       = (meta >>  6) & 0x1F;
    uint8_t ra       = (meta >> 11) & 0x1F;
    uint8_t rb       = (meta >> 16) & 0x1F;
    bool    type_b   = (meta >> 21) & 1;
    bool    rd_src   = (meta >> 22) & 1;
    bool    valid    = (meta >> 23) & 1;
    uint64_t base    = (meta >> 24) & 0xFF;
    bool    is_nop   = (meta >> 32) & 1;

    uint64_t stall = 0;
    if (valid) {
        uint64_t now = pipeline_cycle;
        uint64_t s;
        s = stall_for(ra, now); if (s > stall) stall = s;
        if (!type_b) { s = stall_for(rb, now); if (s > stall) stall = s; }
        if (rd_src)  { s = stall_for(rd, now); if (s > stall) stall = s; }
    }

    uint64_t issue  = pipeline_cycle + stall;
    pipeline_cycle  = issue + base;

    uint8_t lat = (op == 0x16) ? (uint8_t)base : mb_result_latency[op];
    if (lat && rd)
        reg_ready[rd] = issue + lat;

    total.insns++;
    total.wcycles += base;
    total.scycles += stall;
    total.op_count[op]++;
    total.op_cycles[op] += base + stall;

    if (is_nop) {
        total.nop++;
    } else if (op_class_ctr[op]) {
        (*op_class_ctr[op])++;
    }
}

/* =========================================================================
 * TB translation callback
 * ========================================================================= */
static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    trans_count++;
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        const uint8_t *data = insn_bytes(insn);

        if (!data) {
            /* No bytes available: 1-cycle unknown. */
            if (model_stalls) {
                uintptr_t meta = (0x3Fu) | (1u << 23) | ((uintptr_t)1u << 24);
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, vcpu_insn_exec_stall,
                    QEMU_PLUGIN_CB_NO_REGS, (void *)meta);
            } else {
                qemu_plugin_register_vcpu_insn_exec_inline(
                    insn, QEMU_PLUGIN_INLINE_ADD_U64, &total.insns, 1);
                qemu_plugin_register_vcpu_insn_exec_inline(
                    insn, QEMU_PLUGIN_INLINE_ADD_U64, &total.wcycles, 1);
            }
            continue;
        }

        uint32_t w  = insn_word32(data);
        uint8_t  op = (w >> 26) & 0x3F;

        /* ---- Bench control magic NOPs ---- */
        if (is_bench_magic(w)) {
            /* Magic instructions are hardware NOPs: zero pipeline cost,
             * not counted in insns/cycles.  Only the event fires.       */
            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, vcpu_insn_exec_bench,
                QEMU_PLUGIN_CB_NO_REGS, (void *)(uintptr_t)(w & 0xFF));
            continue;
        }

        /* ---- Delay slot quality (look-ahead at next insn in same TB) ---- */
        if (is_delayed_branch(w) && i + 1 < n) {
            struct qemu_plugin_insn *next = qemu_plugin_tb_get_insn(tb, i + 1);
            const uint8_t *nd = insn_bytes(next);
            bool slot_nop = nd && (insn_word32(nd) == MB_NOP_WORD);
            qemu_plugin_register_vcpu_insn_exec_inline(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, &total.delayed_branch, 1);
            qemu_plugin_register_vcpu_insn_exec_inline(
                insn, QEMU_PLUGIN_INLINE_ADD_U64,
                slot_nop ? &total.delay_slot_nop : &total.delay_slot_filled, 1);
        }

        bool is_nop = (w == MB_NOP_WORD);

        /* ---- Per-instruction counters ---- */
        if (model_stalls) {
            uint8_t rd    = (w >> 21) & 0x1F;
            uint8_t ra    = (w >> 16) & 0x1F;
            uint8_t rb    = (w >> 11) & 0x1F;
            bool type_b   = (op & 0x08) != 0;
            bool rd_src   = mb_rd_is_src[op];

            uint8_t base_cyc;
            if (op == 0x16)
                base_cyc = mb_fpu_lat(w);
            else if (op == 0x24 && (w & 8u))
                base_cyc = 2;          /* WIC: func bit[3]=1 → 2 cycles */
            else
                base_cyc = mb_weight[op];

            uintptr_t meta = (uintptr_t)op
                           | ((uintptr_t)rd       <<  6)
                           | ((uintptr_t)ra       << 11)
                           | ((uintptr_t)rb       << 16)
                           | ((uintptr_t)type_b   << 21)
                           | ((uintptr_t)rd_src   << 22)
                           | ((uintptr_t)1u        << 23)   /* valid */
                           | ((uintptr_t)base_cyc << 24)
                           | ((uintptr_t)is_nop   << 32);

            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, vcpu_insn_exec_stall,
                QEMU_PLUGIN_CB_NO_REGS, (void *)meta);

        } else {
            /* Weighted inline mode. */
            uint64_t weight;
            if (op == 0x16)
                weight = mb_fpu_lat(w);
            else if (op == 0x24 && (w & 8u))
                weight = 2;
            else
                weight = mb_weight[op];

            qemu_plugin_register_vcpu_insn_exec_inline(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, &total.insns, 1);
            qemu_plugin_register_vcpu_insn_exec_inline(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, &total.wcycles, weight);
            qemu_plugin_register_vcpu_insn_exec_inline(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, &total.op_count[op], 1);
            qemu_plugin_register_vcpu_insn_exec_inline(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, &total.op_cycles[op], weight);

            /* Class counter. */
            uint64_t *cls = is_nop ? &total.nop : op_class_ctr[op];
            if (cls)
                qemu_plugin_register_vcpu_insn_exec_inline(
                    insn, QEMU_PLUGIN_INLINE_ADD_U64, cls, 1);
        }
    }
}

/* =========================================================================
 * Plugin exit
 * ========================================================================= */
static void plugin_exit(qemu_plugin_id_t id, void *userdata)
{
    if (!bench_event_seen) {
        /* Legacy fallback: no bench events — emit totals for full run. */
        emit_json(&total, "no_bench_events");
        return;
    }
    if (state == ST_STOPPED)
        emit_json(&bench, "incomplete");
    /* If state == ST_IDLE, BENCH_EXIT_PASS/FAIL already emitted the record. */
}

/* =========================================================================
 * Plugin install
 * ========================================================================= */
QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                        int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        char *opt = argv[i];
        g_auto(GStrv) tok = g_strsplit(opt, "=", 2);
        const char *k = tok[0];
        const char *v = tok[1] ? tok[1] : "";

#define BOOL_ARG(flag, field) \
        if (!g_strcmp0(k, flag)) { \
            field = !g_strcmp0(v,"on") || !g_strcmp0(v,"yes") || \
                    !g_strcmp0(v,"true") || !g_strcmp0(v,"1"); \
            continue; \
        }
        BOOL_ARG("stalls",  model_stalls)
        BOOL_ARG("verbose", verbose)
#undef BOOL_ARG

        if (!g_strcmp0(k, "bench"))    { snprintf(bench_name,    sizeof(bench_name),    "%s", v); continue; }
        if (!g_strcmp0(k, "compiler")) { snprintf(compiler_name, sizeof(compiler_name), "%s", v); continue; }
        if (!g_strcmp0(k, "opt"))      { snprintf(opt_flags,     sizeof(opt_flags),     "%s", v); continue; }
        if (!g_strcmp0(k, "target"))   { snprintf(target_config, sizeof(target_config), "%s", v); continue; }
        if (!g_strcmp0(k, "commit"))   { snprintf(commit_hash,   sizeof(commit_hash),   "%s", v); continue; }
        if (!g_strcmp0(k, "area_opt")) {
            fpu_area_opt = (unsigned)atoi(v);
            if (fpu_area_opt > 2) { fprintf(stderr, "mb_cycles: area_opt must be 0-2\n"); return -1; }
            continue;
        }
        fprintf(stderr, "mb_cycles: unknown option '%s'\n", opt);
        return -1;
    }

    big_endian_target = info->target_name &&
                        strcmp(info->target_name, "microblaze") == 0;

    /* Build opcode → class counter pointer table. */
    for (int op = 0; op < 64; op++) {
        switch ((MBClass)mb_class[op]) {
        case CLS_ALU:     op_class_ctr[op] = &total.alu;     break;
        case CLS_MUL:     op_class_ctr[op] = &total.mul;     break;
        case CLS_DIV:     op_class_ctr[op] = &total.div_i;   break;
        case CLS_FPU:     op_class_ctr[op] = &total.fpu;     break;
        case CLS_LOAD:    op_class_ctr[op] = &total.load;    break;
        case CLS_STORE:   op_class_ctr[op] = &total.store;   break;
        case CLS_BRANCH:  op_class_ctr[op] = &total.branch;  break;
        case CLS_MTS_MFS: op_class_ctr[op] = &total.mts_mfs; break;
        case CLS_NOP:
        case CLS_OTHER:
        default:          op_class_ctr[op] = NULL;            break;
        }
    }

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
