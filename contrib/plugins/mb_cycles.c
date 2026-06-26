/*
 * mb_cycles.c — QEMU plugin: MicroBlaze per-instruction cycle counting
 *
 * Two operation modes (select with plugin argument):
 *
 *   stalls=off  (default)
 *     Throughput model: every instruction costs its base cycle count from the
 *     table below.  Uses QEMU_PLUGIN_INLINE_ADD_U64 — minimal overhead.
 *     Measures differences in instruction COUNT between compilers/options.
 *
 *   stalls=on
 *     RAW-stall model: additionally inserts pipeline bubble cycles whenever
 *     an instruction reads a register whose forwarded value is not yet ready.
 *     Uses full per-instruction callbacks (3-5× slower; run on a quiet
 *     machine with a single benchmark process).
 *
 * Pipeline model used by stalls=on  (C_AREA_OPTIMIZED=0, forwarding enabled):
 *
 *   Producer          Latency   Stall on immediate consumer
 *   ---------------   -------   --------------------------
 *   ALU               1 cycle   0  (result forwarded EX→EX)
 *   Load (lw/lh/lb)   2 cycles  1  (result from MEM, one cycle late)
 *   idiv/idivu        34 cycles  0 extra  (blocking; base cost covers it)
 *   fpu               6 cycles   0 extra  (same)
 *   mts / mfs         2 cycles   0 extra  (base cost = 2)
 *   Branch (link)     1 cycle    0  (PC+8 forwarded from IF stage)
 *   Store / branch    —          no GPR written
 *
 * Stall counts are conservative (may overcount on rare edge cases).
 *
 * Usage:
 *   qemu-system-microblazeel ... \
 *       -plugin /path/to/libmb_cycles.so[,verbose=on][,stalls=on]
 *
 * Works with both microblaze (BE) and microblazeel (LE) targets.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/*
 * Base throughput cost per opcode[31:26].
 * Source: UG984 v2026.1 §5 latency tables, C_AREA_OPTIMIZED=0.
 */
static const uint8_t mb_cycle_table[64] = {
    /* 0x00 */ 1, /* add                                         */
    /* 0x01 */ 1, /* rsub                                        */
    /* 0x02 */ 1, /* addc                                        */
    /* 0x03 */ 1, /* rsubc                                       */
    /* 0x04 */ 1, /* addk                                        */
    /* 0x05 */ 1, /* rsubk / cmp / cmpu                          */
    /* 0x06 */ 1, /* addkc                                       */
    /* 0x07 */ 1, /* rsubkc                                      */
    /* 0x08 */ 1, /* addi                                        */
    /* 0x09 */ 1, /* rsubi                                       */
    /* 0x0A */ 1, /* addic                                       */
    /* 0x0B */ 1, /* rsubic                                      */
    /* 0x0C */ 1, /* addik                                       */
    /* 0x0D */ 1, /* rsubik                                      */
    /* 0x0E */ 1, /* addikc                                      */
    /* 0x0F */ 1, /* rsubikc                                     */
    /* 0x10 */ 1, /* mul / mulh / mulhu / mulhsu (pipelined)     */
    /* 0x11 */ 1, /* bsrl / bsra / bsll                          */
    /* 0x12 */34, /* idiv / idivu (UG984: 34 cycles, blocking)   */
    /* 0x13 */ 1, /* getd / putd (FSL)                           */
    /* 0x14 */ 1, /* (reserved)                                  */
    /* 0x15 */ 1, /* (reserved)                                  */
    /* 0x16 */ 6, /* fadd/frsub/fmul/fdiv/flt/fint/fcmp (6 cyc) */
    /* 0x17 */ 1, /* (reserved)                                  */
    /* 0x18 */ 1, /* muli                                        */
    /* 0x19 */ 1, /* bsrli / bsrai / bslli / bsefi / bsifi      */
    /* 0x1A */ 1, /* (reserved)                                  */
    /* 0x1B */ 1, /* get / put (FSL)                             */
    /* 0x1C */ 1, /* (reserved)                                  */
    /* 0x1D */ 1, /* (reserved)                                  */
    /* 0x1E */ 1, /* (reserved)                                  */
    /* 0x1F */ 1, /* (reserved)                                  */
    /* 0x20 */ 1, /* or / pcmpbf                                 */
    /* 0x21 */ 1, /* and                                         */
    /* 0x22 */ 1, /* xor / pcmpeq                                */
    /* 0x23 */ 1, /* andn / pcmpne                               */
    /* 0x24 */ 1, /* sra/src/srl/sext8/sext16/clz/swapb/swaph   */
    /* 0x25 */ 2, /* mts / mfs / msrset / msrclr (UG984: 2 cyc) */
    /* 0x26 */ 1, /* br/bra/brd/brad/brld/brald/brk              */
    /* 0x27 */ 1, /* beq-bge and delay-slot variants             */
    /* 0x28 */ 1, /* ori                                         */
    /* 0x29 */ 1, /* andi                                        */
    /* 0x2A */ 1, /* xori                                        */
    /* 0x2B */ 1, /* andni                                       */
    /* 0x2C */ 1, /* imm                                         */
    /* 0x2D */ 1, /* rtsd / rtid / rtbd / rted                   */
    /* 0x2E */ 1, /* bri/brai/brid/braid/brlid/bralid/brki/mbar  */
    /* 0x2F */ 1, /* beqi-bgei and delay-slot variants            */
    /* 0x30 */ 1, /* lbu / lbur / lbuea                          */
    /* 0x31 */ 1, /* lhu / lhur / lhuea                          */
    /* 0x32 */ 1, /* lw / lwr / lwea / lwx                       */
    /* 0x33 */ 1, /* (reserved)                                  */
    /* 0x34 */ 1, /* sb / sbr / sbea                             */
    /* 0x35 */ 1, /* sh / shr / shea                             */
    /* 0x36 */ 1, /* sw / swr / swea / swx                       */
    /* 0x37 */ 1, /* (reserved)                                  */
    /* 0x38 */ 1, /* lbui                                        */
    /* 0x39 */ 1, /* lhui                                        */
    /* 0x3A */ 1, /* lwi                                         */
    /* 0x3B */ 1, /* (reserved)                                  */
    /* 0x3C */ 1, /* sbi                                         */
    /* 0x3D */ 1, /* shi                                         */
    /* 0x3E */ 1, /* swi                                         */
    /* 0x3F */ 1, /* (reserved)                                  */
};

/*
 * Forwarding latency table for the stall model.
 *
 * For each opcode: number of cycles after the instruction ISSUES before
 * its GPR result is available for forwarding to the next instruction's
 * execute stage.  0 means the instruction writes no GPR.
 *
 * For blocking instructions (idiv=34, fpu=6, mts/mfs=2): the latency equals
 * the base throughput cost, so the next instruction (which can only start
 * after the blocking instruction completes) finds the result already ready —
 * no additional stall is incurred beyond what is already in mb_cycle_table.
 */
static const uint8_t mb_result_latency[64] = {
    /* 0x00 add    */ 1, /* 0x01 rsub   */ 1, /* 0x02 addc   */ 1, /* 0x03 rsubc  */ 1,
    /* 0x04 addk   */ 1, /* 0x05 rsubk  */ 1, /* 0x06 addkc  */ 1, /* 0x07 rsubkc */ 1,
    /* 0x08 addi   */ 1, /* 0x09 rsubi  */ 1, /* 0x0A addic  */ 1, /* 0x0B rsubic */ 1,
    /* 0x0C addik  */ 1, /* 0x0D rsubik */ 1, /* 0x0E addikc */ 1, /* 0x0F rsubikc*/ 1,
    /* 0x10 mul    */ 1, /* 0x11 bsrl   */ 1, /* 0x12 idiv   */34, /* 0x13 getd   */ 1,
    /* 0x14 rsv    */ 0, /* 0x15 rsv    */ 0, /* 0x16 fpu    */ 6, /* 0x17 rsv    */ 0,
    /* 0x18 muli   */ 1, /* 0x19 bsrli  */ 1, /* 0x1A rsv    */ 0, /* 0x1B get    */ 1,
    /* 0x1C rsv    */ 0, /* 0x1D rsv    */ 0, /* 0x1E rsv    */ 0, /* 0x1F rsv    */ 0,
    /* 0x20 or     */ 1, /* 0x21 and    */ 1, /* 0x22 xor    */ 1, /* 0x23 andn   */ 1,
    /* 0x24 sra    */ 1, /* 0x25 mfs    */ 2, /* 0x26 br     */ 1, /* 0x27 beq    */ 0,
    /* 0x28 ori    */ 1, /* 0x29 andi   */ 1, /* 0x2A xori   */ 1, /* 0x2B andni  */ 1,
    /* 0x2C imm    */ 0, /* 0x2D rtsd   */ 0, /* 0x2E bri    */ 1, /* 0x2F beqi   */ 0,
    /* 0x30 lbu    */ 2, /* 0x31 lhu    */ 2, /* 0x32 lw     */ 2, /* 0x33 rsv    */ 0,
    /* 0x34 sb     */ 0, /* 0x35 sh     */ 0, /* 0x36 sw     */ 0, /* 0x37 rsv    */ 0,
    /* 0x38 lbui   */ 2, /* 0x39 lhui   */ 2, /* 0x3A lwi    */ 2, /* 0x3B rsv    */ 0,
    /* 0x3C sbi    */ 0, /* 0x3D shi    */ 0, /* 0x3E swi    */ 0, /* 0x3F rsv    */ 0,
};

/*
 * For these opcodes, bits[25:21] encode a SOURCE register rather than (or in
 * addition to) the usual Rd destination field:
 *
 *   0x27 beq-bge   : bits[25:21] = Ra (condition register)
 *   0x2F beqi-bgei : bits[25:21] = Ra (condition register)
 *   0x34 sb        : bits[25:21] = rD (data to store)
 *   0x35 sh        : bits[25:21] = rD (data to store)
 *   0x36 sw        : bits[25:21] = rD (data to store)
 *   0x3C sbi       : bits[25:21] = rD (data to store, imm addressing)
 *   0x3D shi       : bits[25:21] = rD (data to store, imm addressing)
 *   0x3E swi       : bits[25:21] = rD (data to store, imm addressing)
 *
 * The stall model checks reg_ready for these bits[25:21] as an extra source.
 */
static const bool mb_rd_is_src[64] = {
    [0x27] = true, [0x2F] = true,
    [0x34] = true, [0x35] = true, [0x36] = true,
    [0x3C] = true, [0x3D] = true, [0x3E] = true,
};

/* Human-readable class label for each opcode slot (verbose output). */
static const char *const mb_class_name[64] = {
    "add",      "rsub",     "addc",     "rsubc",
    "addk",     "rsubk/cmp","addkc",    "rsubkc",
    "addi",     "rsubi",    "addic",    "rsubic",
    "addik",    "rsubik",   "addikc",   "rsubikc",
    "mul",      "bsrl/a/l", "idiv",     "getd/putd",
    "rsv",      "rsv",      "fpu",      "rsv",
    "muli",     "bsrli/ai", "rsv",      "get/put",
    "rsv",      "rsv",      "rsv",      "rsv",
    "or",       "and",      "xor",      "andn",
    "sra/ext",  "mts/mfs",  "br/brld",  "beq-bge",
    "ori",      "andi",     "xori",     "andni",
    "imm",      "rtsd",     "bri/braid","beqi-bgei",
    "lbu",      "lhu",      "lw",       "rsv",
    "sb",       "sh",       "sw",       "rsv",
    "lbui",     "lhui",     "lwi",      "rsv",
    "sbi",      "shi",      "swi",      "rsv",
};

/* ---- Counters (single-core MicroBlaze; not thread-safe for SMP) ---- */
static uint64_t total_cycles;
static uint64_t total_insns;
static uint64_t total_stall_cycles;
static uint64_t opcode_cycles[64];
static uint64_t opcode_count[64];
static uint64_t trans_count;

static bool verbose;
static bool model_stalls;
static bool big_endian_target;

/* ---- Stall model state ---- */
static uint64_t reg_ready[32];   /* cycle when each GPR result is forwardable */
static uint64_t pipeline_cycle;  /* logical pipeline cycle counter             */

/*
 * Extract the 6-bit opcode from the MSB byte of a MicroBlaze instruction.
 * Works for both endian variants (see insn_bytes() comment).
 */
static inline uint32_t insn_opcode(const uint8_t *data)
{
    return (big_endian_target ? data[0] : data[3]) >> 2;
}

/*
 * Reconstruct the full 32-bit instruction word from guest memory bytes.
 * MicroBlaze bit-31 = MSB.  In little-endian ELF (microblazeel): byte[3] is MSB.
 */
static inline uint32_t insn_word32(const uint8_t *data)
{
    if (big_endian_target)
        return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
             | ((uint32_t)data[2] <<  8) | (uint32_t)data[3];
    else
        return ((uint32_t)data[3] << 24) | ((uint32_t)data[2] << 16)
             | ((uint32_t)data[1] <<  8) | (uint32_t)data[0];
}

/*
 * Return host-virtual pointer to the 4 bytes of a guest instruction.
 * qemu_plugin_insn_haddr() is always valid for RAM-backed pages.
 */
static inline const uint8_t *insn_bytes(const struct qemu_plugin_insn *insn)
{
    if (qemu_plugin_insn_size(insn) >= 4)
        return (const uint8_t *)qemu_plugin_insn_data(insn);
    return (const uint8_t *)qemu_plugin_insn_haddr(insn);
}

/* Return the number of stall cycles register R would impose right now. */
static inline uint64_t stall_for(uint8_t r, uint64_t now)
{
    return (r && reg_ready[r] > now) ? reg_ready[r] - now : 0;
}

/*
 * Full callback for the RAW-stall model (stalls=on).
 *
 * Userdata is a packed 25-bit value:
 *   bits [ 5: 0]  op           (6-bit opcode)
 *   bits [10: 6]  rd           (bits[25:21] of instruction word)
 *   bits [15:11]  ra           (bits[20:16])
 *   bits [20:16]  rb           (bits[15:11], meaningful only if !type_b)
 *   bit  [21]     type_b       (1 = Type-B instruction, no Rb register field)
 *   bit  [22]     rd_is_src    (1 = also check rd as a source, per mb_rd_is_src)
 *   bit  [23]     valid        (1 = instruction bytes were available at trans time)
 */
static void vcpu_insn_exec_stall(unsigned int vcpu_idx, void *userdata)
{
    uintptr_t meta    = (uintptr_t)userdata;
    uint8_t   op      = (meta >>  0) & 0x3f;
    uint8_t   rd      = (meta >>  6) & 0x1f;
    uint8_t   ra      = (meta >> 11) & 0x1f;
    uint8_t   rb      = (meta >> 16) & 0x1f;
    bool      type_b     = (meta >> 21) & 1;
    bool      rd_is_src  = (meta >> 22) & 1;
    bool      valid      = (meta >> 23) & 1;

    uint64_t base  = mb_cycle_table[op];
    uint64_t stall = 0;

    if (valid) {
        uint64_t now = pipeline_cycle;
        uint64_t s;

        s = stall_for(ra, now);
        if (s > stall) stall = s;

        if (!type_b) {
            s = stall_for(rb, now);
            if (s > stall) stall = s;
        }

        if (rd_is_src) {
            s = stall_for(rd, now);
            if (s > stall) stall = s;
        }
    }

    uint64_t issue = pipeline_cycle + stall;
    pipeline_cycle = issue + base;

    /* Record when this instruction's GPR result will be forwardable. */
    uint8_t lat = mb_result_latency[op];
    if (lat && rd)
        reg_ready[rd] = issue + lat;

    uint64_t cost = base + stall;
    total_cycles       += cost;
    total_stall_cycles += stall;
    total_insns++;
    opcode_count[op]++;
    opcode_cycles[op] += cost;
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    trans_count++;
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        const uint8_t *data = insn_bytes(insn);

        if (model_stalls) {
            /* --- RAW-stall mode: full callback with packed instruction metadata --- */
            uint8_t op = 63, rd = 0, ra = 0, rb = 0;
            bool type_b = false, rd_is_src = false, valid = false;

            if (data) {
                uint32_t w = insn_word32(data);
                op         = (w >> 26) & 0x3f;
                rd         = (w >> 21) & 0x1f;
                ra         = (w >> 16) & 0x1f;
                rb         = (w >> 11) & 0x1f;
                type_b     = (op & 0x08) != 0;
                rd_is_src  = mb_rd_is_src[op];
                valid      = true;
            }

            uintptr_t meta = (uintptr_t)op
                           | ((uintptr_t)rd         <<  6)
                           | ((uintptr_t)ra         << 11)
                           | ((uintptr_t)rb         << 16)
                           | ((uintptr_t)type_b     << 21)
                           | ((uintptr_t)rd_is_src  << 22)
                           | ((uintptr_t)valid      << 23);

            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, vcpu_insn_exec_stall,
                QEMU_PLUGIN_CB_NO_REGS, (void *)meta);

        } else {
            /* --- Throughput mode: four inline atomic counters per instruction --- */
            uint8_t  op;
            uint64_t cycles;
            if (data) {
                op     = insn_opcode(data);
                cycles = mb_cycle_table[op];
            } else {
                op     = 63;
                cycles = 1;
            }

            qemu_plugin_register_vcpu_insn_exec_inline(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, &total_insns,      1);
            qemu_plugin_register_vcpu_insn_exec_inline(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, &total_cycles,     cycles);
            qemu_plugin_register_vcpu_insn_exec_inline(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, &opcode_count[op], 1);
            qemu_plugin_register_vcpu_insn_exec_inline(
                insn, QEMU_PLUGIN_INLINE_ADD_U64, &opcode_cycles[op],cycles);
        }
    }
}

static void plugin_exit(qemu_plugin_id_t id, void *userdata)
{
    char buf[320];
    const char *mode = model_stalls
        ? "(C_AREA_OPTIMIZED=0, branch-predicted, RAW stalls modeled)"
        : "(C_AREA_OPTIMIZED=0, branch-predicted, throughput only)";

    if (model_stalls) {
        snprintf(buf, sizeof(buf),
                 "MicroBlaze cycle model  %s\n"
                 "Total instructions : %" PRIu64 "\n"
                 "Total cycles       : %" PRIu64 "\n"
                 "Stall cycles       : %" PRIu64 "\n"
                 "CPI                : %.3f\n"
                 "Stall CPI          : %.3f\n"
                 "TB translations    : %" PRIu64 "\n",
                 mode,
                 total_insns, total_cycles, total_stall_cycles,
                 total_insns ? (double)total_cycles / total_insns : 0.0,
                 total_insns ? (double)total_stall_cycles / total_insns : 0.0,
                 trans_count);
    } else {
        snprintf(buf, sizeof(buf),
                 "MicroBlaze cycle model  %s\n"
                 "Total instructions : %" PRIu64 "\n"
                 "Total cycles       : %" PRIu64 "\n"
                 "CPI                : %.3f\n"
                 "TB translations    : %" PRIu64 "\n",
                 mode,
                 total_insns, total_cycles,
                 total_insns ? (double)total_cycles / total_insns : 0.0,
                 trans_count);
    }
    qemu_plugin_outs(buf);

    if (!verbose)
        return;

    qemu_plugin_outs("Opcode class breakdown:\n"
                     "  Op    Class            Insns         Cycles    CPI\n"
                     "  ----  ---------------  ------------  --------  ----\n");

    for (int op = 0; op < 64; op++) {
        if (!opcode_count[op])
            continue;
        snprintf(buf, sizeof(buf),
                 "  0x%02x  %-15s  %12" PRIu64 "  %8" PRIu64 "  %.2f\n",
                 op, mb_class_name[op],
                 opcode_count[op], opcode_cycles[op],
                 (double)opcode_cycles[op] / opcode_count[op]);
        qemu_plugin_outs(buf);
    }
}

QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                        int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        char *opt = argv[i];
        g_auto(GStrv) tokens = g_strsplit(opt, "=", 2);
        const char *key = tokens[0];
        const char *val = tokens[1] ? tokens[1] : "";

        if (!g_strcmp0(key, "verbose")) {
            if (!g_strcmp0(val,"on")||!g_strcmp0(val,"yes")||
                !g_strcmp0(val,"true")||!g_strcmp0(val,"1"))
                verbose = true;
            else if (!g_strcmp0(val,"off")||!g_strcmp0(val,"no")||
                     !g_strcmp0(val,"false")||!g_strcmp0(val,"0"))
                verbose = false;
            else {
                fprintf(stderr, "mb_cycles: bad value for verbose: %s\n", opt);
                return -1;
            }
        } else if (!g_strcmp0(key, "stalls")) {
            if (!g_strcmp0(val,"on")||!g_strcmp0(val,"yes")||
                !g_strcmp0(val,"true")||!g_strcmp0(val,"1"))
                model_stalls = true;
            else if (!g_strcmp0(val,"off")||!g_strcmp0(val,"no")||
                     !g_strcmp0(val,"false")||!g_strcmp0(val,"0"))
                model_stalls = false;
            else {
                fprintf(stderr, "mb_cycles: bad value for stalls: %s\n", opt);
                return -1;
            }
        } else {
            fprintf(stderr, "mb_cycles: unknown option '%s'\n", opt);
            return -1;
        }
    }

    big_endian_target = info->target_name &&
                        strcmp(info->target_name, "microblaze") == 0;

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
