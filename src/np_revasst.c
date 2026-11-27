/* np_revasst.c — R5: AI 逆向助手（实现）.
 * 纯 C99、UI-free、只读；启发式分析 + 注入式 AI 解释，不改源字节。
 */
#include "np_revasst.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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

const char *np_ra_status_name(np_ra_status_t s)
{
    switch (s) {
    case NP_RA_OK: return "OK";
    case NP_RA_ERR_ARG: return "ARG";
    case NP_RA_ERR_FULL: return "FULL";
    case NP_RA_ERR_NOCFG: return "NOCFG";
    case NP_RA_ERR_UNSUPPORTED: return "UNSUPPORTED";
    }
    return "?";
}

const char *np_ra_suspect_name(np_ra_suspect_kind_t k)
{
    switch (k) {
    case NP_RA_S_SYSCALL: return "系统调用";
    case NP_RA_S_NETWORK: return "网络";
    case NP_RA_S_FILE: return "文件操作";
    case NP_RA_S_CRYPTO: return "加密特征";
    case NP_RA_S_ANTIDEBUG: return "反调试";
    case NP_RA_S_PRIVILEGE: return "权限/提权";
    case NP_RA_S_PROCESS: return "进程/注入";
    case NP_RA_S_OTHER: return "其他";
    }
    return "?";
}

void np_ra_sanitize(const char *in, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    if (!out || cap == 0) return;
    if (!in) { out[0] = '\0'; return; }
    while (in[i] && o + 1 < cap) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20 && c != '\n' && c != '\t') { i++; continue; } /* 控制字符 */
        out[o++] = (char)c;
        i++;
    }
    out[o] = '\0';
}

/* 引用串：file:0xOFFSET（十六进制小写） */
static void make_ref(char *out, size_t cap, const char *source, uint64_t addr)
{
    (void)snprintf(out, cap, "%s:0x%llx", source ? source : "(unknown)",
                   (unsigned long long)addr);
}

/* ---------- 可疑行为模式表 ---------- */
typedef struct {
    np_ra_suspect_kind_t kind;
    const char *pat;
    int conf;
} sus_pat_t;

static const sus_pat_t SUS_PATTERNS[] = {
    { NP_RA_S_SYSCALL,   "syscall",   90 },
    { NP_RA_S_SYSCALL,   "int 0x80",  88 },
    { NP_RA_S_SYSCALL,   " svc ",     85 },
    { NP_RA_S_NETWORK,   "socket",    80 },
    { NP_RA_S_NETWORK,   "connect",   80 },
    { NP_RA_S_NETWORK,   "send",      70 },
    { NP_RA_S_NETWORK,   "recv",      70 },
    { NP_RA_S_FILE,      "fopen",     80 },
    { NP_RA_S_FILE,      "open",      65 },
    { NP_RA_S_FILE,      "read",      60 },
    { NP_RA_S_FILE,      "write",     60 },
    { NP_RA_S_CRYPTO,    "aes",       82 },
    { NP_RA_S_CRYPTO,    "sha",       82 },
    { NP_RA_S_CRYPTO,    "md5",       82 },
    { NP_RA_S_CRYPTO,    "xorb",      60 },  /* xor byte/循环加密特征 */
    { NP_RA_S_ANTIDEBUG, "ptrace",    90 },
    { NP_RA_S_ANTIDEBUG, "isdebuggerpresent", 92 },
    { NP_RA_S_ANTIDEBUG, "rdtsc",     70 },
    { NP_RA_S_ANTIDEBUG, "outputdebugstring", 85 },
    { NP_RA_S_PRIVILEGE, "setuid",    88 },
    { NP_RA_S_PRIVILEGE, "adjusttokenprivileges", 90 },
    { NP_RA_S_PROCESS,   "createthread", 85 },
    { NP_RA_S_PROCESS,   "virtualalloc", 82 },
    { NP_RA_S_PROCESS,   "dlopen",    78 },
    { NP_RA_S_PROCESS,   "loadlibrary", 80 },
    { NP_RA_S_PROCESS,   "execve",    85 }
};

static bool ci_contains(const char *hay, const char *needle)
{
    char low[512];
    size_t i;
    fscpy(low, sizeof(low), hay);
    for (i = 0; low[i]; i++) low[i] = (char)tolower((unsigned char)low[i]);
    return strstr(low, needle) != NULL;
}

/* ---------- 上下文包 ---------- */
np_ra_status_t np_ra_build_context(const char *source,
                                   const np_cfg_graph_t *g, int func_index,
                                   const np_disasm_insn_t *insns, size_t n,
                                   char *out, size_t cap)
{
    size_t u = 0;
    const np_cfg_func_t *f;
    size_t i;
    if (!g || !out || cap < 128 || func_index < 0 || func_index >= g->nfuncs)
        return NP_RA_ERR_ARG;
    f = &g->funcs[func_index];
    if (!appendf(out, cap, &u,
        "# 二进制函数分析上下文（只读）\n\n"
        "函数：%s\n起始地址：0x%llx\n引用：%s:0x%llx\n"
        "指令 %zu 条 / 基本块 %d\n\n",
        f->name, (unsigned long long)f->start_addr,
        source ? source : "(unknown)", (unsigned long long)f->start_addr,
        f->end_insn - f->start_insn + 1, f->nblocks)) return NP_RA_ERR_FULL;
    if (!appendf(out, cap, &u, "## 指令（虚拟地址 · 文本）\n")) return NP_RA_ERR_FULL;
    if (insns) {
        for (i = f->start_insn; i <= f->end_insn && i < n; i++) {
            char clean[NP_RA_MAX_TEXT];
            np_ra_sanitize(insns[i].text, clean, sizeof(clean));
            if (!appendf(out, cap, &u, "- 0x%llx  %s\n",
                         (unsigned long long)insns[i].addr, clean)) return NP_RA_ERR_FULL;
        }
    } else {
        if (!appendf(out, cap, &u, "- (未提供指令数组，仅 CFG 摘要)\n")) return NP_RA_ERR_FULL;
    }
    if (!appendf(out, cap, &u,
        "\n## CFG 摘要\n- 基本块 %d / 边 %d（启发式 L1）\n\n"
        "注意：本上下文为只读分析材料；任何命名都是「待确认」建议，"
        "不得直接写回二进制。\n",
        g->nblocks, g->nedges)) return NP_RA_ERR_FULL;
    return NP_RA_OK;
}

/* ---------- 本地启发式分析 ---------- */
/* 命名候选与调用链基于 CFG 图保守推导；可疑行为基于真实指令文本模式匹配。
 * 一切语义结论均标「待确认」，不臆造。 */

static np_ra_name_t make_name(const np_cfg_graph_t *g, int fi)
{
    np_ra_name_t nm;
    const np_cfg_func_t *f = &g->funcs[fi];
    memset(&nm, 0, sizeof(nm));
    nm.needs_confirm = true; /* 红线 */
    /* 基于块数/置信度的保守命名候选（不臆造语义） */
    if (f->conf >= 95)
        fscpy(nm.name, sizeof(nm.name), "sub_called");
    else if (f->nblocks > 3)
        fscpy(nm.name, sizeof(nm.name), "sub_multiblock");
    else
        fscpy(nm.name, sizeof(nm.name), "sub_routine");
    (void)snprintf(nm.basis, sizeof(nm.basis),
                   "基于 CFG 块数(%d)与边界置信度(%d)的保守候选，语义未确认",
                   f->nblocks, f->conf);
    nm.conf = f->conf > 70 ? 70 : f->conf;
    return nm;
}

static int build_chain(const np_cfg_graph_t *g, int fi, np_ra_result_t *r)
{
    const np_cfg_func_t *f = &g->funcs[fi];
    int i, n = 0;
    /* 遍历 CFG 中 from 属于本函数的 CALL 边，记录目标函数 */
    for (i = 0; i < g->nedges && n < NP_RA_MAX_CHAIN; i++) {
        const np_cfg_edge_t *e = &g->edges[i];
        int target_fi;
        if (e->kind != NP_CFG_E_CALL || e->target_insn == (size_t)-1) continue;
        /* 边 from 属于本函数的块？ */
        if (e->from >= (size_t)g->nblocks) continue;
        if (g->blocks[e->from].func_index != fi) continue;
        target_fi = np_cfg_func_for_insn(g, e->target_insn);
        if (target_fi < 0) continue;
        {
            np_ra_chain_item_t *it = &r->chain[n++];
            const np_cfg_func_t *tf = &g->funcs[target_fi];
            it->func_index = target_fi;
            it->addr = tf->start_addr;
            fscpy(it->name, sizeof(it->name), tf->name);
            make_ref(it->ref, sizeof(it->ref), r->source, tf->start_addr);
        }
    }
    (void)f;
    return n;
}

np_ra_status_t np_ra_analyze(const char *source, const np_cfg_graph_t *g,
                             int func_index,
                             const np_disasm_insn_t *insns, size_t n,
                             np_ra_explain_fn explain, void *user,
                             np_ra_result_t *out)
{
    const np_cfg_func_t *f;
    size_t i;
    if (!g || !out || func_index < 0 || func_index >= g->nfuncs)
        return NP_RA_ERR_ARG;
    memset(out, 0, sizeof(*out));
    f = &g->funcs[func_index];
    out->func_index = func_index;
    out->func_addr = f->start_addr;
    fscpy(out->func_name, sizeof(out->func_name), f->name);
    fscpy(out->source, sizeof(out->source), source ? source : "(unknown)");

    /* 1) 本地命名候选（恒待确认） */
    out->name = make_name(g, func_index);

    /* 2) 调用链摘要（静态 CALL 边） */
    out->nchain = build_chain(g, func_index, out);
    if (out->nchain == 0)
        fscpy(out->chain_summary, sizeof(out->chain_summary),
              "未发现静态 call 目标（可能叶函数或间接调用）");
    else
        (void)snprintf(out->chain_summary, sizeof(out->chain_summary),
                       "静态调用 %d 个函数（不含间接调用）", out->nchain);

    /* 3) 可疑行为：对本函数的真实指令文本做保守模式匹配，仅在确实命中
     *    时标记，绝不臆造；每条带 offset 引用回跳。 */
    if (insns) {
        for (i = f->start_insn; i <= f->end_insn && i < n &&
                                out->nsuspects < NP_RA_MAX_SUSPECT; i++) {
            size_t p;
            for (p = 0; p < sizeof(SUS_PATTERNS) / sizeof(SUS_PATTERNS[0]); p++) {
                const sus_pat_t *sp = &SUS_PATTERNS[p];
                if (ci_contains(insns[i].text, sp->pat)) {
                    np_ra_suspect_t *s = &out->suspects[out->nsuspects++];
                    memset(s, 0, sizeof(*s));
                    s->kind = sp->kind;
                    s->conf = sp->conf;
                    s->insn_index = i;
                    s->addr = insns[i].addr;
                    fscpy(s->evidence, sizeof(s->evidence), insns[i].text);
                    make_ref(s->ref, sizeof(s->ref), source, insns[i].addr);
                    break; /* 同一指令只记一个最高优先命中 */
                }
            }
        }
    }

    /* 4) 解释：AI 回调优先，失败/缺失回退本地 */
    {
        char ctx[NP_RA_MAX_CTX];
        np_ra_status_t cs = np_ra_build_context(source, g, func_index,
                                                insns, n, ctx, sizeof(ctx));
        bool ai_ok = false;
        if (cs == NP_RA_OK && explain) {
            char tmp[NP_RA_MAX_TEXT];
            tmp[0] = '\0';
            if (explain(ctx, tmp, sizeof(tmp), user) && tmp[0]) {
                np_ra_sanitize(tmp, out->explanation,
                               sizeof(out->explanation));
                out->explanation_from_ai = true;
                ai_ok = true;
            }
        }
        if (!ai_ok) {
            char chain[64];
            fscpy(chain, sizeof(chain), out->chain_summary);
            (void)snprintf(out->explanation, sizeof(out->explanation),
                "函数 %s：%zu 条指令 / %d 块，边界置信度 %d。"
                "静态调用 %d 个；%s。本地启发式摘要，语义需人工核验。",
                f->name, f->end_insn - f->start_insn + 1, f->nblocks,
                f->conf, out->nchain, chain);
        }
    }
    return NP_RA_OK;
}

/* ---------- 渲染 ---------- */
int np_ra_render(const np_ra_result_t *r, char *out, size_t cap)
{
    size_t u = 0;
    int i;
    if (!r || !out || cap < 128) return -1;
    if (!appendf(out, cap, &u, "# AI 逆向分析 · %s\n\n", r->func_name)) return -1;
    if (!appendf(out, cap, &u, "- 引用：%s:0x%llx\n", r->source,
                 (unsigned long long)r->func_addr)) return -1;
    if (!appendf(out, cap, &u, "- 解释来源：%s\n\n",
                 r->explanation_from_ai ? "外部 AI（注入回调，已清洗）"
                                        : "本地启发式")) return -1;
    if (!appendf(out, cap, &u, "## 解释\n%s\n\n", r->explanation)) return -1;
    if (!appendf(out, cap, &u, "## 命名建议（待确认，不写回原文）\n"
                 "- 建议：**%s（待确认）**\n- 依据：%s\n- 置信度：%d\n\n",
                 r->name.name, r->name.basis, r->name.conf)) return -1;
    if (!appendf(out, cap, &u, "## 调用链摘要\n%s\n\n", r->chain_summary)) return -1;
    for (i = 0; i < r->nchain; i++) {
        if (!appendf(out, cap, &u, "- %s @ %s\n",
                     r->chain[i].name, r->chain[i].ref)) return -1;
    }
    if (r->nchain) {
        if (!appendf(out, cap, &u, "\n")) return -1;
    }
    if (!appendf(out, cap, &u, "## 可疑行为标记（仅模式命中，非定性）\n")) return -1;
    if (r->nsuspects == 0) {
        if (!appendf(out, cap, &u, "- 未命中已知模式\n")) return -1;
    } else {
        for (i = 0; i < r->nsuspects; i++) {
            const np_ra_suspect_t *s = &r->suspects[i];
            if (!appendf(out, cap, &u, "- %s（置信度 %d）· %s · %s\n",
                         np_ra_suspect_name(s->kind), s->conf,
                         s->ref, s->evidence)) return -1;
        }
    }
    if (!appendf(out, cap, &u,
        "\n> 只读分析：所有命名均为「待确认」建议，未修改任何源字节。"
        "可疑行为基于启发式模式匹配，不构成安全定性。\n")) return -1;
    return (int)u;
}
