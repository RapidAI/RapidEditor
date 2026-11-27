/* np_patchimpact.c — R6: 补丁影响分析（实现）.
 * 纯 C99、UI-free、只读；字节改动 → 指令/函数/块影响映射 + H2 校验串联。
 */
#include "np_patchimpact.h"
#include "np_integrity.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------- 工具 ---------- */
static void fscpy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static bool appendf(char *out, size_t cap, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (*used >= cap) return false;
    va_start(ap, fmt);
    n = vsnprintf(out + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *used) return false;
    *used += (size_t)n;
    return true;
}

const char *np_pi_status_name(np_pi_status_t s)
{
    switch (s) {
    case NP_PI_OK: return "OK";
    case NP_PI_ERR_ARG: return "ARG";
    case NP_PI_ERR_FULL: return "FULL";
    case NP_PI_ERR_NODIFF: return "NODIFF";
    }
    return "?";
}

const char *np_pi_level_name(np_pi_level_t l)
{
    switch (l) {
    case NP_PI_DIRECT: return "直接改写";
    case NP_PI_SHIFTED: return "位移失效";
    case NP_PI_BOUNDARY: return "边界增删";
    case NP_PI_FUNC_UNCERTAIN: return "位置不确定";
    }
    return "?";
}

/* 判断字节区间 [a, b) 与指令覆盖区间 [off, off+size) 是否相交 */
static bool insn_overlaps(const np_disasm_insn_t *ins, size_t a, size_t b)
{
    size_t s = ins->offset;
    size_t e = ins->size ? s + ins->size : s + 1;
    return (s < b && e > a);
}

/* 添加一条受影响记录（去重：同 index+level 不重复） */
static bool add_affected(np_pi_result_t *out, np_pi_level_t level,
                         size_t insn_index, size_t byte_off,
                         size_t affected_bytes,
                         const np_disasm_insn_t *ins,
                         const np_cfg_graph_t *cfg,
                         const np_disasm_insn_t *after_ins, size_t n_after)
{
    int i;
    if (out->naffected >= NP_PI_MAX_AFFECTED) { out->truncated = true; return false; }
    for (i = 0; i < out->naffected; i++) {
        if (out->affected[i].insn_index == insn_index &&
            out->affected[i].level == level) return true; /* 已记 */
    }
    {
        np_pi_affected_t *af = &out->affected[out->naffected++];
        memset(af, 0, sizeof(*af));
        af->level = level;
        af->insn_index = insn_index;
        af->byte_off = byte_off;
        af->affected_bytes = affected_bytes;
        af->func_index = cfg ? np_cfg_func_for_insn(cfg, insn_index) : -1;
        af->block_id = cfg ? np_cfg_block_for_insn(cfg, insn_index) : (size_t)-1;
        if (ins) {
            af->before_addr = ins->addr;
            fscpy(af->label, sizeof(af->label), ins->text);
        }
        /* 尝试按地址在 after 中匹配对应指令 */
        if (after_ins && ins) {
            size_t k;
            for (k = 0; k < n_after; k++) {
                if (after_ins[k].addr == ins->addr) { af->after_addr = after_ins[k].addr; break; }
            }
        }
    }
    return true;
}

np_pi_status_t np_pi_analyze(const void *before, size_t blen,
                             const void *after, size_t alen,
                             const np_bd_result_t *diff,
                             const np_disasm_insn_t *ins_before, size_t n_before,
                             const np_disasm_insn_t *ins_after, size_t n_after,
                             const np_cfg_graph_t *cfg_before,
                             const np_cfg_graph_t *cfg_after,
                             const char *source_before, const char *source_after,
                             np_pi_result_t *out)
{
    size_t h;
    (void)cfg_after;
    if (!before || !after || !diff || !out) return NP_PI_ERR_ARG;
    if (diff->nhunks == 0) return NP_PI_ERR_NODIFF;
    memset(out, 0, sizeof(*out));
    fscpy(out->source_before, sizeof(out->source_before),
          source_before ? source_before : "(before)");
    fscpy(out->source_after, sizeof(out->source_after),
          source_after ? source_after : "(after)");
    out->nhunks = diff->nhunks;

    /* H2 完整性校验：CRC32 + SHA256 摘要（只读） */
    out->digest.crc_before = np_crc32(before, blen);
    out->digest.crc_after = np_crc32(after, alen);
    {
        unsigned char sha[32];
        np_sha256(before, blen, sha);
        np_sha256_hex(sha, out->digest.sha_before);
        np_sha256(after, alen, sha);
        np_sha256_hex(sha, out->digest.sha_after);
    }
    out->digest.verified = np_bytediff_verify(before, blen, diff, after, alen);

    /* 遍历每个 hunk 做影响映射 */
    for (h = 0; h < diff->nhunks && !out->truncated; h++) {
        const np_bd_hunk_t *hk = &diff->hunks[h];
        size_t a = hk->before_off;
        size_t b = hk->before_len ? a + hk->before_len : a; /* 空区间=插入点 */
        size_t i;
        if (hk->kind == NP_BD_INSERT) {
            /* 插入：落在插入点之后的指令 offset 会位移（SHIFTED）；
             * 恰好在插入点的指令视为边界受影响（BOUNDARY，可能整条新增） */
            for (i = 0; i < n_before; i++) {
                const np_disasm_insn_t *ins = &ins_before[i];
                np_pi_level_t lv;
                if (ins->offset == a) lv = NP_PI_BOUNDARY;
                else if (ins->offset > a) lv = NP_PI_SHIFTED;
                else continue;
                add_affected(out, lv, i, a, hk->after_len, ins,
                             cfg_before, ins_after, n_after);
            }
        } else {
            /* REPLACE/DELETE：区间覆盖的指令 DIRECT；之后的指令 SHIFTED（仅 DELETE
             * 或长度变化时）；边界外紧邻的视为 BOUNDARY */
            size_t delta = (hk->after_len > hk->before_len)
                           ? hk->after_len - hk->before_len : 0;
            bool shrink = hk->before_len > hk->after_len;
            for (i = 0; i < n_before; i++) {
                const np_disasm_insn_t *ins = &ins_before[i];
                np_pi_level_t lv = NP_PI_DIRECT;
                bool hit = insn_overlaps(ins, a, b);
                if (!hit) {
                    if ((shrink || delta) && ins->offset >= b) lv = NP_PI_SHIFTED;
                    else continue;
                }
                add_affected(out, lv, i, a, hk->before_len, ins,
                             cfg_before, ins_after, n_after);
            }
        }
    }

    /* 归并函数/块命中数（去重） */
    {
        int nf = 0, nb = 0;
        for (h = 0; h < (size_t)out->naffected; h++) {
            const np_pi_affected_t *af = &out->affected[h];
            int fi = af->func_index;
            size_t bi = af->block_id;
            bool seen_f = false, seen_b = false;
            int k;
            for (k = 0; k < (int)h; k++) {
                if (out->affected[k].func_index == fi) seen_f = true;
                if (out->affected[k].block_id == bi) seen_b = true;
            }
            if (fi >= 0 && !seen_f) nf++;
            if (bi != (size_t)-1 && !seen_b) nb++;
        }
        out->nfuncs_hit = nf;
        out->nblocks_hit = nb;
    }
    return NP_PI_OK;
}

int np_pi_render(const np_pi_result_t *r, char *out, size_t cap)
{
    size_t u = 0;
    int i;
    static const char *const LABEL[] = {
        "直接改写", "位移失效", "边界增删", "位置不确定"
    };
    (void)LABEL;
    if (!r || !out || cap < 128) return -1;
    if (!appendf(out, cap, &u, "# 补丁影响分析\n\n")) return -1;
    if (!appendf(out, cap, &u,
        "- 版本：%s → %s\n- Hunk %lu · 受影响指令 %d · 函数 %d · 基本块 %d%s\n",
        r->source_before, r->source_after,
        (unsigned long)r->nhunks, r->naffected, r->nfuncs_hit,
        r->nblocks_hit, r->truncated ? " · 已达上限" : "")) return -1;
    if (!appendf(out, cap, &u,
        "- H2 完整性：before CRC32=%08x SHA256=%s · after CRC32=%08x SHA256=%s"
        " · Diff 回放校验=%s\n\n",
        r->digest.crc_before, r->digest.sha_before,
        r->digest.crc_after, r->digest.sha_after,
        r->digest.verified ? "通过" : "未通过")) return -1;
    if (!appendf(out, cap, &u,
        "| 级别 | 指令(before) | 字节偏移 | 影响字节 | 函数 | 基本块 | 证据 |\n"
        "|---|---|---|---|---|---|---|\n")) return -1;
    for (i = 0; i < r->naffected; i++) {
        const np_pi_affected_t *af = &r->affected[i];
        char label[NP_PI_MAX_LABEL];
        size_t k, o = 0;
        for (k = 0; af->label[k] && o + 1 < sizeof(label); k++) {
            label[o++] = (af->label[k] == '|' || af->label[k] == '\n')
                         ? ' ' : af->label[k];
        }
        label[o] = '\0';
        if (!appendf(out, cap, &u, "| %s | #%lu @0x%llx | 0x%lx | %lu | %s | %s | %s |\n",
                     np_pi_level_name(af->level), (unsigned long)af->insn_index,
                     (unsigned long long)af->before_addr,
                     (unsigned long)af->byte_off,
                     (unsigned long)af->affected_bytes,
                     af->func_index >= 0 ? "已归属" : "未知",
                     af->block_id != (size_t)-1 ? "已归属" : "未知",
                     label)) return -1;
    }
    if (!appendf(out, cap, &u,
        "\n> 静态启发式（L1）：位移后的指令需重新反汇编才能精确判定；"
        "H2 摘要只证明两端字节未被篡改，不证明补丁语义正确。\n")) return -1;
    return (int)u;
}
