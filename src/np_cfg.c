/* np_cfg.c — R3: 函数边界与控制流图（实现）.
 * 纯 C99、UI-free；消费 R2 已解码指令，启发式识别函数/基本块/CFG。
 */
#include "np_cfg.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SIZE_MAX 通过 stdint.h 提供；这里定义一个无宏展开冲突的别名 */
#define NP_CFG_NOIDX ((size_t)-1)

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

const char *np_cfg_status_name(np_cfg_status_t s)
{
    switch (s) {
    case NP_CFG_OK: return "OK";
    case NP_CFG_ERR_ARG: return "ARG";
    case NP_CFG_ERR_FULL: return "FULL";
    case NP_CFG_ERR_NOINSN: return "NOINSN";
    case NP_CFG_ERR_UNSUPPORTED: return "UNSUPPORTED";
    }
    return "?";
}

/* ---------- 内置 x86/x64 文本分类器 ---------- */
static bool mnem_is(const char *s, size_t n, const char *kw)
{
    return n == strlen(kw) && strncmp(s, kw, n) == 0;
}

/* 解析操作数里的目标地址（十六进制 0x... 或裸 hex，objdump x86 常见
 * "call 401000 <foo>" / "jmp 0x401000"），返回是否解析成功。 */
static bool parse_target(const char *ops, uint64_t *addr)
{
    const char *p = ops;
    uint64_t v = 0;
    int ndig = 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    while (*p && isxdigit((unsigned char)*p)) {
        int d = isdigit((unsigned char)*p) ? (*p - '0')
                                           : (tolower((unsigned char)*p) - 'a' + 10);
        v = (v << 4) | (uint64_t)d;
        ndig++;
        p++;
    }
    if (ndig == 0) return false;
    /* 结束必须是非 hex 字母数字（避免把符号名开头当地址） */
    if (isalnum((unsigned char)*p)) return false;
    *addr = v;
    return true;
}

np_cfg_itype_t np_cfg_classify_x86(const np_disasm_insn_t *ins, void *user)
{
    np_cfg_itype_t t;
    const char *s = ins ? ins->text : NULL;
    char mnem[24];
    size_t n = 0, i;
    (void)user;
    memset(&t, 0, sizeof(t));
    t.cls = NP_IC_OTHER;
    if (!s) return t;
    while (*s && isspace((unsigned char)*s)) s++;
    while (*s && n < sizeof(mnem) - 1 &&
           (isalnum((unsigned char)*s) || *s == '_' || *s == '.')) {
        mnem[n++] = (char)tolower((unsigned char)*s++);
    }
    mnem[n] = '\0';

    /* 操作数字串（跳过大写助记符后），供 parse_target */
    {
        const char *ops = ins->text;
        while (*ops && !isspace((unsigned char)*ops)) ops++;
        while (*ops && isspace((unsigned char)*ops)) ops++;
        if (mnem_is(mnem, n, "call") || mnem_is(mnem, n, "callq") ||
            mnem_is(mnem, n, "lcall")) {
            t.cls = NP_IC_CALL;
            t.target_valid = parse_target(ops, &t.target);
            return t;
        }
        if (mnem_is(mnem, n, "jmp") || mnem_is(mnem, n, "jmpq") ||
            mnem_is(mnem, n, "ljmp")) {
            t.cls = NP_IC_JMP;
            t.target_valid = parse_target(ops, &t.target);
            return t;
        }
        /* 条件跳转：j + 非 mp 的两三字母（je/jne/jz/jnz/jg/jl/jge/jle/ja/jb/...） */
        if (mnem[0] == 'j' && n >= 2 && n <= 5 && !mnem_is(mnem, n, "jmp")) {
            t.cls = NP_IC_JCC;
            t.target_valid = parse_target(ops, &t.target);
            return t;
        }
        if (mnem_is(mnem, n, "loop") || mnem_is(mnem, n, "loope") ||
            mnem_is(mnem, n, "loopne")) {
            t.cls = NP_IC_JCC;
            t.target_valid = parse_target(ops, &t.target);
            return t;
        }
        if (mnem_is(mnem, n, "ret") || mnem_is(mnem, n, "retq") ||
            mnem_is(mnem, n, "iret") || mnem_is(mnem, n, "sysret")) {
            t.cls = NP_IC_RET;
            return t;
        }
        if (mnem_is(mnem, n, "hlt") || mnem_is(mnem, n, "ud2")) {
            t.cls = NP_IC_RET;   /* 终止基本块 */
            return t;
        }
        if (mnem_is(mnem, n, "nop") || mnem_is(mnem, n, "nopl") ||
            mnem_is(mnem, n, "nopw")) {
            t.cls = NP_IC_NOP;
            return t;
        }
        /* prologue: push rbp / push ebp / endbr64 视作可能函数起点 */
        if (mnem_is(mnem, n, "push")) {
            const char *q = ops;
            while (*q && isspace((unsigned char)*q)) q++;
            if (strncmp(q, "rbp", 3) == 0 || strncmp(q, "ebp", 3) == 0)
                t.cls = NP_IC_PROLOGUE;
            return t;
        }
        if (mnem_is(mnem, n, "endbr64") || mnem_is(mnem, n, "endbr32")) {
            t.cls = NP_IC_PROLOGUE;
            return t;
        }
    }
    (void)i;
    return t;
}

/* ---------- 查询 ---------- */
size_t np_cfg_block_for_insn(const np_cfg_graph_t *g, size_t insn)
{
    int i;
    if (!g) return SIZE_MAX;
    for (i = 0; i < g->nblocks; i++) {
        const np_cfg_block_t *b = &g->blocks[i];
        if (insn >= b->start_insn && insn <= b->end_insn) return b->id;
    }
    return SIZE_MAX;
}

size_t np_cfg_block_for_addr(const np_cfg_graph_t *g, uint64_t addr)
{
    int i;
    if (!g) return SIZE_MAX;
    for (i = 0; i < g->nblocks; i++) {
        const np_cfg_block_t *b = &g->blocks[i];
        if (addr >= b->start_addr && addr <= b->end_addr) return b->id;
    }
    return SIZE_MAX;
}

int np_cfg_func_for_insn(const np_cfg_graph_t *g, size_t insn)
{
    int i;
    if (!g) return -1;
    for (i = 0; i < g->nfuncs; i++) {
        const np_cfg_func_t *f = &g->funcs[i];
        if (insn >= f->start_insn && insn <= f->end_insn) return i;
    }
    return -1;
}

/* ---------- 构建 ---------- */
np_cfg_status_t np_cfg_build(const np_disasm_insn_t *insns, size_t n,
                             np_parch_t arch, np_cfg_classify_fn classify,
                             void *user, np_cfg_graph_t *out)
{
    np_cfg_itype_t *types;
    bool *leader, *func_start;
    size_t i, j;
    if (!insns || !out || n == 0) return NP_CFG_ERR_ARG;
    if (n > NP_CFG_MAX_BLOCKS * 8) return NP_CFG_ERR_FULL;
    memset(out, 0, sizeof(*out));
    out->arch = arch;
    out->ninstructions = n;
    if (!classify) {
        if (arch != NP_PARCH_X86 && arch != NP_PARCH_X64)
            return NP_CFG_ERR_UNSUPPORTED;   /* 内置分类器仅 x86/x64 */
        classify = np_cfg_classify_x86;
    }
    types = (np_cfg_itype_t *)calloc(n, sizeof(*types));
    leader = (bool *)calloc(n, sizeof(*leader));
    func_start = (bool *)calloc(n, sizeof(*func_start));
    if (!types || !leader || !func_start) {
        free(types); free(leader); free(func_start);
        return NP_CFG_ERR_FULL;
    }
    for (i = 0; i < n; i++) {
        types[i] = classify(&insns[i], user);
    }

    /* 1) 函数起点候选：指令 0、prologue、被 call 目标地址指向的指令。
     *    jmp/jcc 目标只作基本块 leader（通常仍是同一函数内的块），不作函数起点，
     *    否则函数内的跳转目标会被误判为独立函数。
     *    置信度：第一条/无条件跳转目标=60，prologue=85，call 目标=95。 */
    func_start[0] = true;
    for (i = 0; i < n; i++) {
        if (types[i].cls == NP_IC_PROLOGUE) func_start[i] = true;
    }
    for (i = 0; i < n; i++) {
        uint64_t tgt;
        size_t k;
        if (!types[i].target_valid || types[i].cls != NP_IC_CALL) continue;
        tgt = types[i].target;
        for (k = 0; k < n; k++) {
            if (insns[k].addr == tgt) { func_start[k] = true; break; }
        }
    }

    /* 2) 基本块 leader：每个函数起点、跳转/调用目标、跳转/ret 后一条指令。 */
    for (i = 0; i < n; i++) {
        if (func_start[i]) leader[i] = true;
        if (types[i].cls == NP_IC_CALL || types[i].cls == NP_IC_JMP ||
            types[i].cls == NP_IC_JCC || types[i].cls == NP_IC_RET) {
            if (i + 1 < n) leader[i + 1] = true;
            if (types[i].target_valid) {
                uint64_t tgt = types[i].target;
                for (j = 0; j < n; j++) {
                    if (insns[j].addr == tgt) { leader[j] = true; break; }
                }
            }
        }
    }

    /* 3) 生成基本块：leader..下一个 leader-1（或 n-1） */
    {
        size_t cur_start = (size_t)-1;
        size_t blk = 0;
        for (i = 0; i <= n; i++) {
            if (i == n || leader[i]) {
                if (cur_start != (size_t)-1) {
                    np_cfg_block_t *b;
                    size_t e = i - 1;
                    if (blk >= NP_CFG_MAX_BLOCKS) { out->truncated = true; break; }
                    b = &out->blocks[out->nblocks++];
                    memset(b, 0, sizeof(*b));
                    b->id = (size_t)blk;
                    b->start_insn = cur_start;
                    b->end_insn = e;
                    b->start_addr = insns[cur_start].addr;
                    b->end_addr = insns[e].addr;
                    b->func_index = -1;
                    /* label: 起止地址 + 指令数 + 首条助记符 */
                    {
                        char first[48];
                        fscpy(first, sizeof(first), insns[cur_start].text);
                        /* 截断到第一个空格（助记符） */
                        for (j = 0; first[j] && first[j] != ' '; j++) {}
                        first[j] = '\0';
                        snprintf(b->label, sizeof(b->label),
                                 "B%zu @0x%llx [%zu] %s", blk,
                                 (unsigned long long)b->start_addr,
                                 e - cur_start + 1, first);
                    }
                    blk++;
                }
                cur_start = (i < n && leader[i]) ? i : (size_t)-1;
            }
        }
    }

    /* 4) 生成函数：func_start 到下一个 func_start-1；统计块数 */
    {
        size_t cur = (size_t)-1;
        for (i = 0; i <= n; i++) {
            if (i == n || func_start[i]) {
                if (cur != (size_t)-1) {
                    np_cfg_func_t *f;
                    size_t e = i - 1;
                    int nb = 0, conf;
                    size_t k;
                    if (out->nfuncs >= NP_CFG_MAX_FUNCS) {
                        out->truncated = true; break;
                    }
                    f = &out->funcs[out->nfuncs];
                    memset(f, 0, sizeof(*f));
                    f->start_insn = cur;
                    f->end_insn = e;
                    f->start_addr = insns[cur].addr;
                    f->end_addr = insns[e].addr;
                    snprintf(f->name, sizeof(f->name), "sub_%llx",
                             (unsigned long long)f->start_addr);
                    conf = 60;
                    if (types[cur].cls == NP_IC_PROLOGUE) conf = 85;
                    /* 被 call 目标指向 → 95 */
                    for (k = 0; k < n; k++) {
                        if (types[k].target_valid && types[k].target == f->start_addr &&
                            types[k].cls == NP_IC_CALL) {
                            conf = 95; break;
                        }
                    }
                    f->conf = conf;
                    for (k = 0; k < (size_t)out->nblocks; k++) {
                        if (out->blocks[k].start_insn >= cur &&
                            out->blocks[k].start_insn <= e) {
                            out->blocks[k].func_index = out->nfuncs;
                            nb++;
                        }
                    }
                    f->nblocks = nb;
                    out->nfuncs++;
                }
                cur = (i < n && func_start[i]) ? i : (size_t)-1;
            }
        }
    }

    /* 5) CFG 边 */
    for (i = 0; i < (size_t)out->nblocks && !out->truncated; i++) {
        const np_cfg_block_t *b = &out->blocks[i];
        const np_disasm_insn_t *last = &insns[b->end_insn];
        np_cfg_itype_t lt = types[b->end_insn];
        size_t next_b = (i + 1 < (size_t)out->nblocks) ? out->blocks[i + 1].id
                                                       : SIZE_MAX;
        /* 添加边 helper 内联：用宏风格手动 push */
#define PUSH_EDGE(ekind, eto, etgt, eti) do { \
        if (out->nedges >= NP_CFG_MAX_EDGES) { out->truncated = true; break; } \
        { np_cfg_edge_t *e2 = &out->edges[out->nedges++]; \
          memset(e2, 0, sizeof(*e2)); \
          e2->from = b->id; e2->to = (eto); e2->kind = (ekind); \
          e2->target_addr = (etgt); e2->target_insn = (eti); } \
    } while (0)
        (void)last;
        switch (lt.cls) {
        case NP_IC_RET:
            PUSH_EDGE(NP_CFG_E_RET, NP_CFG_NOIDX, 0, NP_CFG_NOIDX);
            break;
        case NP_IC_JMP:
            if (lt.target_valid) {
                size_t tb = np_cfg_block_for_addr(out, lt.target);
                size_t ti = NP_CFG_NOIDX;
                for (j = 0; j < n; j++) if (insns[j].addr == lt.target) { ti = j; break; }
                PUSH_EDGE(NP_CFG_E_JMP, tb, lt.target, ti);
            } else {
                PUSH_EDGE(NP_CFG_E_JMP, next_b, 0, NP_CFG_NOIDX);
            }
            break;
        case NP_IC_JCC:
            if (lt.target_valid) {
                size_t tb = np_cfg_block_for_addr(out, lt.target);
                size_t ti = NP_CFG_NOIDX;
                for (j = 0; j < n; j++) if (insns[j].addr == lt.target) { ti = j; break; }
                PUSH_EDGE(NP_CFG_E_JCC_TAKEN, tb, lt.target, ti);
            }
            PUSH_EDGE(NP_CFG_E_FALLTHROUGH, next_b, 0, NP_CFG_NOIDX);
            break;
        case NP_IC_CALL: {
            size_t ti = NP_CFG_NOIDX;
            if (lt.target_valid)
                for (j = 0; j < n; j++) if (insns[j].addr == lt.target) { ti = j; break; }
            PUSH_EDGE(NP_CFG_E_CALL, (lt.target_valid && ti != NP_CFG_NOIDX)
                      ? np_cfg_block_for_insn(out, ti) : NP_CFG_NOIDX,
                      lt.target_valid ? lt.target : 0, ti);
            PUSH_EDGE(NP_CFG_E_FALLTHROUGH, next_b, 0, NP_CFG_NOIDX);
            break;
        }
        default:
            PUSH_EDGE(NP_CFG_E_FALLTHROUGH, next_b, 0, NP_CFG_NOIDX);
            break;
        }
#undef PUSH_EDGE
    }

    free(types); free(leader); free(func_start);
    return NP_CFG_OK;
}

/* ---------- 渲染 ---------- */
int np_cfg_render_mermaid(const np_cfg_graph_t *g, char *out, size_t cap)
{
    size_t u = 0;
    int i;
    if (!g || !out || cap < 128) return -1;
    if (!appendf(out, cap, &u, "flowchart TD\n")) return -1;
    for (i = 0; i < g->nblocks; i++) {
        const np_cfg_block_t *b = &g->blocks[i];
        char lbl[NP_CFG_MAX_LABEL];
        size_t k, o = 0;
        for (k = 0; b->label[k] && o + 1 < sizeof(lbl); k++) {
            char c = b->label[k];
            lbl[o++] = (c == '"' || c == '\n' || c == '\r') ? ' ' : c;
        }
        lbl[o] = '\0';
        if (!appendf(out, cap, &u, "  B%zu[\"%s\"]\n", b->id, lbl)) return -1;
    }
    for (i = 0; i < g->nedges; i++) {
        const np_cfg_edge_t *e = &g->edges[i];
        const char *ek;
        switch (e->kind) {
        case NP_CFG_E_FALLTHROUGH: ek = ""; break;
        case NP_CFG_E_JCC_TAKEN: ek = "T"; break;
        case NP_CFG_E_JMP: ek = "JMP"; break;
        case NP_CFG_E_CALL: ek = "CALL"; break;
        case NP_CFG_E_RET: ek = "RET"; break;
        default: ek = ""; break;
        }
        if (e->to == SIZE_MAX) {
            if (!appendf(out, cap, &u, "  B%zu -. %s .-> EXT\n", e->from, ek))
                return -1;
        } else {
            if (!appendf(out, cap, &u, "  B%zu -->|%s| B%zu\n",
                         e->from, ek, e->to)) return -1;
        }
    }
    if (!appendf(out, cap, &u,
        "\n%% %d 函数 / %d 基本块 / %d 边 · 启发式 CFG（L1，非精确反编译）\n",
        g->nfuncs, g->nblocks, g->nedges)) return -1;
    return (int)u;
}

int np_cfg_render_summary(const np_cfg_graph_t *g, char *out, size_t cap)
{
    size_t u = 0;
    int i;
    if (!g || !out || cap < 64) return -1;
    if (!appendf(out, cap, &u, "函数边界与 CFG（启发式 L1）\n\n")) return -1;
    if (!appendf(out, cap, &u, "- 指令 %lu / 函数 %d / 基本块 %d / 边 %d%s\n\n",
        (unsigned long)g->ninstructions, g->nfuncs, g->nblocks, g->nedges,
        g->truncated ? " · 已达上限（截断）" : "")) return -1;
    for (i = 0; i < g->nfuncs; i++) {
        const np_cfg_func_t *f = &g->funcs[i];
        if (!appendf(out, cap, &u, "%s  0x%llx–0x%llx  %zu 条指令 / %d 块  置信度 %d\n",
            f->name, (unsigned long long)f->start_addr,
            (unsigned long long)f->end_addr,
            f->end_insn - f->start_insn + 1, f->nblocks, f->conf)) return -1;
    }
    return (int)u;
}
