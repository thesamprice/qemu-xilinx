/*
 * qemu_rtems_cov - non-intrusive translation-block coverage for RTEMS tests.
 *
 * Observes executed translation blocks, deduplicates covered guest code
 * addresses in memory, and writes one compact "RTQCov1" binary artifact at
 * QEMU exit. Symbolization (addr2line/LCOV) is done offline by a host tool.
 *
 * This follows the same structure as contrib/plugins/drcov.c: the QEMU
 * translation callback carries no userdata, so plugin state lives in a single
 * global object. Retranslated TBs simply produce duplicate per-TB records;
 * that is harmless because addresses are sorted and de-duplicated at exit.
 *
 * License: GNU GPL, version 2 or later.
 *   See the COPYING file in the top-level directory.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/* ------------------------------------------------------------------ */
/* Raw file format (RTQCov1).                                          */
/* ------------------------------------------------------------------ */

#define RTQCOV_MAGIC "RTQCov1\0"   /* 8 bytes including the NUL */

enum {
    RTQCOV_REC_TB_ADDR   = 1,      /* records are TB start addresses */
    RTQCOV_REC_INSN_ADDR = 2,      /* records are instruction addresses */
};

/* Header flags. */
enum {
    /*
     * When set, each record is a 16-byte { uint64 addr; uint64 count; } pair
     * (execution count) instead of a bare 8-byte address. record_type still
     * indicates the address granularity (TB vs instruction).
     */
    RTQCOV_FLAG_HAS_COUNTS = 0x1,
};

/* All multi-byte header fields are little-endian on disk. */
typedef struct rtqcov_header {
    char     magic[8];
    uint16_t version;
    uint16_t endian;               /* 1 = little, 2 = big */
    uint32_t header_size;
    uint32_t record_type;
    uint32_t flags;
    uint64_t record_count;
    uint64_t metadata_offset;
    uint64_t metadata_size;
    uint64_t records_offset;
    uint64_t records_size;
} rtqcov_header;

/* ------------------------------------------------------------------ */
/* Internal data structures.                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t *items;
    size_t count;
    size_t cap;
} U64Vec;

typedef struct {
    uint64_t tb_vaddr;
    U64Vec insns;                  /* in-range insn vaddrs (tb-insn mode) */
    volatile gint executed;
    uint64_t count;                /* execution count (counts mode), 64-bit */
} CovTb;

/* { address, execution count } record used when counts mode is enabled. */
typedef struct {
    uint64_t addr;
    uint64_t count;
} AddrCount;

typedef struct {
    uint64_t start;
    uint64_t end;                  /* exclusive */
} Range;

typedef struct {
    GMutex lock;
    GPtrArray *blocks;             /* of CovTb* */

    char *out_path;
    char *test_id;
    char *bsp;
    char *elf_path;
    char *target_name;
    bool system_emulation;

    bool expand_tb_to_insns;       /* mode=tb-insn */
    bool counts;
    bool verbose;

    Range *ranges;
    size_t range_count;
} CovState;

static CovState g_state;

/* ------------------------------------------------------------------ */
/* Helpers.                                                           */
/* ------------------------------------------------------------------ */

static void u64vec_push(U64Vec *v, uint64_t x)
{
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = g_realloc(v->items, v->cap * sizeof(uint64_t));
    }
    v->items[v->count++] = x;
}

static bool range_contains(CovState *s, uint64_t addr)
{
    if (s->range_count == 0) {
        return true;
    }
    for (size_t i = 0; i < s->range_count; i++) {
        if (addr >= s->ranges[i].start && addr < s->ranges[i].end) {
            return true;
        }
    }
    return false;
}

static gint u64_compare(gconstpointer a, gconstpointer b)
{
    uint64_t av = *(const uint64_t *)a;
    uint64_t bv = *(const uint64_t *)b;
    return (av > bv) - (av < bv);
}

static gint addrcount_compare(gconstpointer a, gconstpointer b)
{
    uint64_t av = ((const AddrCount *)a)->addr;
    uint64_t bv = ((const AddrCount *)b)->addr;
    return (av > bv) - (av < bv);
}

/* ------------------------------------------------------------------ */
/* TCG callbacks.                                                     */
/* ------------------------------------------------------------------ */

static void vcpu_tb_exec(unsigned int cpu_index, void *udata)
{
    CovTb *ctb = (CovTb *)udata;

    g_atomic_int_set(&ctb->executed, 1);

    if (g_state.counts) {
        /* 64-bit atomic add (GLib has no portable g_atomic_int64_add). */
        __atomic_fetch_add(&ctb->count, 1, __ATOMIC_RELAXED);
    }
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    CovState *s = &g_state;
    uint64_t tb_vaddr = qemu_plugin_tb_vaddr(tb);

    /* If the TB start is outside every filter range, ignore it entirely. */
    if (!range_contains(s, tb_vaddr)) {
        return;
    }

    g_mutex_lock(&s->lock);

    CovTb *ctb = g_new0(CovTb, 1);
    ctb->tb_vaddr = tb_vaddr;

    if (s->expand_tb_to_insns) {
        size_t n = qemu_plugin_tb_n_insns(tb);
        for (size_t i = 0; i < n; i++) {
            struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
            uint64_t vaddr = qemu_plugin_insn_vaddr(insn);
            if (range_contains(s, vaddr)) {
                u64vec_push(&ctb->insns, vaddr);
            }
        }
    }

    g_ptr_array_add(s->blocks, ctb);

    g_mutex_unlock(&s->lock);

    qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_tb_exec,
                                         QEMU_PLUGIN_CB_NO_REGS, ctb);
}

/* ------------------------------------------------------------------ */
/* Output.                                                            */
/* ------------------------------------------------------------------ */

static char *build_metadata_json(CovState *s, uint64_t record_count)
{
    GString *m = g_string_new(NULL);

    g_string_append(m, "{\n");
    g_string_append(m, "  \"format\": \"RTQCov\",\n");
    g_string_append(m, "  \"version\": 1,\n");
    g_string_append_printf(m, "  \"mode\": \"%s\",\n",
                           s->expand_tb_to_insns ? "tb-insn" : "tb");
    g_string_append_printf(m, "  \"target_name\": \"%s\",\n",
                           s->target_name ? s->target_name : "");
    g_string_append_printf(m, "  \"system_emulation\": %s,\n",
                           s->system_emulation ? "true" : "false");
    g_string_append_printf(m, "  \"test_id\": \"%s\",\n",
                           s->test_id ? s->test_id : "");
    g_string_append_printf(m, "  \"bsp\": \"%s\",\n", s->bsp ? s->bsp : "");
    g_string_append_printf(m, "  \"elf\": \"%s\",\n",
                           s->elf_path ? s->elf_path : "");
    g_string_append_printf(m, "  \"address_kind\": \"vaddr\",\n");
    g_string_append_printf(m, "  \"counts_enabled\": %s,\n",
                           s->counts ? "true" : "false");
    g_string_append_printf(m, "  \"record_count\": %" PRIu64 ",\n",
                           record_count);

    g_string_append(m, "  \"filters\": [");
    for (size_t i = 0; i < s->range_count; i++) {
        g_string_append_printf(m, "%s{\"start\": \"0x%" PRIx64 "\", "
                               "\"end\": \"0x%" PRIx64 "\"}",
                               i ? ", " : "",
                               s->ranges[i].start, s->ranges[i].end);
    }
    g_string_append(m, "]\n");
    g_string_append(m, "}\n");

    return g_string_free(m, FALSE);
}

/*
 * Counts-mode writer: emit sorted unique { addr, count } records. Each covered
 * address inherits the execution count of every TB it belongs to; counts for
 * the same address (from retranslated/overlapping TBs) are summed.
 */
static void plugin_exit_counts(CovState *s)
{
    GArray *pairs = g_array_new(FALSE, FALSE, sizeof(AddrCount));

    g_mutex_lock(&s->lock);

    for (guint i = 0; i < s->blocks->len; i++) {
        CovTb *ctb = g_ptr_array_index(s->blocks, i);
        if (!g_atomic_int_get(&ctb->executed)) {
            continue;
        }
        uint64_t c = __atomic_load_n(&ctb->count, __ATOMIC_RELAXED);
        if (s->expand_tb_to_insns && ctb->insns.count > 0) {
            for (size_t j = 0; j < ctb->insns.count; j++) {
                AddrCount ac = { ctb->insns.items[j], c };
                g_array_append_val(pairs, ac);
            }
        } else {
            AddrCount ac = { ctb->tb_vaddr, c };
            g_array_append_val(pairs, ac);
        }
    }

    g_mutex_unlock(&s->lock);

    g_array_sort(pairs, addrcount_compare);

    /* Merge-sum duplicate addresses in place. */
    guint unique = 0;
    for (guint i = 0; i < pairs->len; i++) {
        AddrCount cur = g_array_index(pairs, AddrCount, i);
        if (unique > 0 &&
            cur.addr == g_array_index(pairs, AddrCount, unique - 1).addr) {
            g_array_index(pairs, AddrCount, unique - 1).count += cur.count;
        } else {
            g_array_index(pairs, AddrCount, unique) = cur;
            unique++;
        }
    }

    char *meta = build_metadata_json(s, unique);
    size_t meta_size = strlen(meta);

    rtqcov_header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, RTQCOV_MAGIC, 8);
    h.version = 1;
    h.endian = 1;
    h.header_size = (uint32_t)sizeof(h);
    h.record_type = s->expand_tb_to_insns ? RTQCOV_REC_INSN_ADDR
                                          : RTQCOV_REC_TB_ADDR;
    h.flags = RTQCOV_FLAG_HAS_COUNTS;
    h.record_count = unique;
    h.metadata_offset = sizeof(h);
    h.metadata_size = meta_size;
    h.records_offset = sizeof(h) + meta_size;
    h.records_size = (uint64_t)unique * sizeof(AddrCount);

    char *tmp = g_strdup_printf("%s.tmp", s->out_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        g_printerr("qemu_rtems_cov: failed to open %s\n", tmp);
        goto out;
    }

    fwrite(&h, sizeof(h), 1, f);
    fwrite(meta, 1, meta_size, f);
    for (guint i = 0; i < unique; i++) {
        AddrCount ac = g_array_index(pairs, AddrCount, i);
        fwrite(&ac, sizeof(ac), 1, f);
    }
    fclose(f);

    if (rename(tmp, s->out_path) != 0) {
        g_printerr("qemu_rtems_cov: failed to rename %s -> %s\n",
                   tmp, s->out_path);
    } else if (s->verbose) {
        g_printerr("qemu_rtems_cov: wrote %u count records to %s\n",
                   unique, s->out_path);
    }

out:
    g_free(tmp);
    g_free(meta);
    g_array_free(pairs, TRUE);
}

static void plugin_exit(qemu_plugin_id_t id, void *userdata)
{
    CovState *s = &g_state;

    if (s->counts) {
        plugin_exit_counts(s);
        return;
    }

    GArray *covered = g_array_new(FALSE, FALSE, sizeof(uint64_t));

    g_mutex_lock(&s->lock);

    for (guint i = 0; i < s->blocks->len; i++) {
        CovTb *ctb = g_ptr_array_index(s->blocks, i);
        if (!g_atomic_int_get(&ctb->executed)) {
            continue;
        }
        if (s->expand_tb_to_insns && ctb->insns.count > 0) {
            for (size_t j = 0; j < ctb->insns.count; j++) {
                g_array_append_val(covered, ctb->insns.items[j]);
            }
        } else {
            g_array_append_val(covered, ctb->tb_vaddr);
        }
    }

    g_mutex_unlock(&s->lock);

    g_array_sort(covered, u64_compare);

    /* De-duplicate in place. */
    guint unique = 0;
    for (guint i = 0; i < covered->len; i++) {
        uint64_t cur = g_array_index(covered, uint64_t, i);
        if (unique == 0 || cur != g_array_index(covered, uint64_t, unique - 1)) {
            g_array_index(covered, uint64_t, unique) = cur;
            unique++;
        }
    }

    char *meta = build_metadata_json(s, unique);
    size_t meta_size = strlen(meta);

    rtqcov_header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, RTQCOV_MAGIC, 8);
    h.version = 1;
    h.endian = 1;                          /* file is written little-endian */
    h.header_size = (uint32_t)sizeof(h);
    h.record_type = s->expand_tb_to_insns ? RTQCOV_REC_INSN_ADDR
                                          : RTQCOV_REC_TB_ADDR;
    h.flags = 0;
    h.record_count = unique;
    h.metadata_offset = sizeof(h);
    h.metadata_size = meta_size;
    h.records_offset = sizeof(h) + meta_size;
    h.records_size = (uint64_t)unique * sizeof(uint64_t);

    char *tmp = g_strdup_printf("%s.tmp", s->out_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        g_printerr("qemu_rtems_cov: failed to open %s\n", tmp);
        goto out;
    }

    fwrite(&h, sizeof(h), 1, f);
    fwrite(meta, 1, meta_size, f);
    for (guint i = 0; i < unique; i++) {
        uint64_t a = g_array_index(covered, uint64_t, i);
        fwrite(&a, sizeof(a), 1, f);
    }
    fclose(f);

    if (rename(tmp, s->out_path) != 0) {
        g_printerr("qemu_rtems_cov: failed to rename %s -> %s\n",
                   tmp, s->out_path);
    } else if (s->verbose) {
        g_printerr("qemu_rtems_cov: wrote %u records to %s\n",
                   unique, s->out_path);
    }

out:
    g_free(tmp);
    g_free(meta);
    g_array_free(covered, TRUE);
}

/* ------------------------------------------------------------------ */
/* Argument parsing.                                                  */
/* ------------------------------------------------------------------ */

/* Parse "0xSTART-0xEND[,0xSTART-0xEND...]" into g_state.ranges. */
static void parse_filter(CovState *s, const char *spec)
{
    g_autofree char **parts = g_strsplit(spec, ",", -1);
    for (int i = 0; parts[i] != NULL; i++) {
        if (parts[i][0] == '\0') {
            continue;
        }
        g_autofree char **se = g_strsplit(parts[i], "-", 2);
        if (!se[0] || !se[1]) {
            g_printerr("qemu_rtems_cov: bad filter range '%s'\n", parts[i]);
            continue;
        }
        uint64_t start = g_ascii_strtoull(g_strstrip(se[0]), NULL, 0);
        uint64_t end = g_ascii_strtoull(g_strstrip(se[1]), NULL, 0);
        s->ranges = g_realloc(s->ranges, (s->range_count + 1) * sizeof(Range));
        s->ranges[s->range_count].start = start;
        s->ranges[s->range_count].end = end;
        s->range_count++;
    }
}

static void parse_arg(CovState *s, const char *arg)
{
    g_autofree char **kv = g_strsplit(arg, "=", 2);
    const char *k = kv[0];
    const char *v = kv[1] ? kv[1] : "";

    if (g_strcmp0(k, "out") == 0) {
        g_free(s->out_path);
        s->out_path = g_strdup(v);
    } else if (g_strcmp0(k, "test_id") == 0) {
        s->test_id = g_strdup(v);
    } else if (g_strcmp0(k, "bsp") == 0) {
        s->bsp = g_strdup(v);
    } else if (g_strcmp0(k, "elf") == 0) {
        s->elf_path = g_strdup(v);
    } else if (g_strcmp0(k, "mode") == 0) {
        if (g_strcmp0(v, "tb") == 0) {
            s->expand_tb_to_insns = false;
        } else if (g_strcmp0(v, "tb-insn") == 0 || g_strcmp0(v, "insn") == 0) {
            s->expand_tb_to_insns = true;
        } else {
            g_printerr("qemu_rtems_cov: unknown mode '%s'\n", v);
        }
    } else if (g_strcmp0(k, "filter") == 0) {
        parse_filter(s, v);
    } else if (g_strcmp0(k, "counts") == 0) {
        s->counts = (g_strcmp0(v, "1") == 0);
    } else if (g_strcmp0(k, "verbose") == 0) {
        s->verbose = (g_strcmp0(v, "1") == 0);
    } else {
        g_printerr("qemu_rtems_cov: ignoring unknown arg '%s'\n", arg);
    }
}

QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                        int argc, char **argv)
{
    CovState *s = &g_state;

    memset(s, 0, sizeof(*s));
    g_mutex_init(&s->lock);
    s->blocks = g_ptr_array_new();
    s->out_path = g_strdup("qemu-rtems.cov");
    s->target_name = g_strdup(info->target_name);
    s->system_emulation = info->system_emulation;
    s->expand_tb_to_insns = true;  /* default mode=tb-insn */

    for (int i = 0; i < argc; i++) {
        parse_arg(s, argv[i]);
    }

    if (s->verbose) {
        g_printerr("qemu_rtems_cov: target=%s system=%d mode=%s out=%s "
                   "filters=%zu\n",
                   s->target_name, s->system_emulation,
                   s->expand_tb_to_insns ? "tb-insn" : "tb",
                   s->out_path, s->range_count);
    }

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);

    return 0;
}
