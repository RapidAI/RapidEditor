/* np_decomp.c — DC1: 本地反编译后端桥接（实现）.
 * 纯 C99、UI-free；内置启发式 + 注入式外部后端，输出统一"伪代码+offset"。
 */
#include "np_decomp.h"

#include <ctype.h>
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

const char *np_dc_status_name(np_dc_status_t s)
{
    switch (s) {
    case NP_DC_OK: return "OK";
    case NP_DC_ERR_ARG: return "ARG";
    case NP_DC_ERR_NOCFG: return "NOCFG";
    case NP_DC_ERR_FULL: return "FULL";
    case NP_DC_ERR_BACKEND: return "BACKEND";
    case NP_DC_ERR_UNSUPPORTED: return "UNSUPPORTED";
    }
    return "?";
}

const char *np_dc_backend_name(np_dc_backend_t b)
{
    switch (b) {
    case NP_DC_BUILTIN: return "builtin-heuristic";
    case NP_DC_GHIDRA: return "Ghidra-headless";
    case NP_DC_RIZIN: return "rizin";
    case NP_DC_RETDEC: return "retdec";
    case NP_DC_CUSTOM: return "custom";
    }
    return "?";
}

/* ---------- 清洗 ---------- */
void np_dc_sanitize(const char *in, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    if (!out || cap == 0) return;
    if (!in) { out[0] = '\0'; return; }
    while (in[i] && o + 1 < cap) {
        unsigned char c = (unsigned char)in[i];
        if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7f) { i++; continue; }
        out[o++] = (char)c;
        i++;
    }
    out[o] = '\0';
}

/* 添加伪代码行（结果 + code 文本），行数上限守卫 */
static bool add_line(np_dc_result_t *r, size_t offset, uint64_t addr,
                     const char *text)
{
    np_dc_line_t *ln;
    if (r->nlines >= NP_DC_MAX_LINES) { return false; }
    ln = &r->lines[r->nlines++];
    memset(ln, 0, sizeof(*ln));
    ln->line_no = r->nlines;
    ln->offset = offset;
    ln->addr = addr;
    fscpy(ln->text, sizeof(ln->text), text);
    return true;
}

/* ---------- 内置启发式：单条指令 → 伪代码 ---------- */
static void insn_to_pseudo(const char *raw, char *out, size_t cap)
{
    char mnem[24], ops[160];
    size_t n = 0, i = 0;
    const char *p = raw;
    while (*p && isspace((unsigned char)*p)) p++;
    while (*p && n < sizeof(mnem) - 1 &&
           (isalnum((unsigned char)*p) || *p == '_')) {
        mnem[n++] = (char)tolower((unsigned char)*p++);
    }
    mnem[n] = '\0';
    while (*p && isspace((unsigned char)*p)) p++;  /* 跳过助记符后的空白 */
    fscpy(ops, sizeof(ops), p);
    if (!*ops) {
        if (!strcmp(mnem, "ret") || !strcmp(mnem, "retq"))
            fscpy(out, cap, "return;");
        else
            (void)snprintf(out, cap, "%s;", mnem);
        return;
    }
    if (!strcmp(mnem, "mov") || !strcmp(mnem, "movq") || !strcmp(mnem, "movl")) {
        /* 拆分 "dst, src" → "dst = src;"（x86 AT&T 源在前，但 objdump Intel
         * 惯例是 dst, src；此处按 Intel 输出，保守标注） */
        char *comma = strchr(ops, ',');
        if (comma) {
            *comma = '\0';
            (void)snprintf(out, cap, "%s = %s;  /* candidate */", ops,
                           comma + 1);
        } else {
            (void)snprintf(out, cap, "mov %s;", ops);
        }
    }
    else if (!strcmp(mnem, "call") || !strcmp(mnem, "callq"))
        (void)snprintf(out, cap, "call %s;", ops);
    else if (!strcmp(mnem, "ret") || !strcmp(mnem, "retq"))
        fscpy(out, cap, "return;");
    else if (!strcmp(mnem, "push") || !strcmp(mnem, "pop"))
        (void)snprintf(out, cap, "%s %s;  /* stack */", mnem, ops);
    else if (!strcmp(mnem, "cmp") || !strcmp(mnem, "test"))
        (void)snprintf(out, cap, "cmp %s;", ops);
    else if (!strcmp(mnem, "jmp") || !strcmp(mnem, "jmpq"))
        (void)snprintf(out, cap, "goto %s;", ops);
    else if (mnem[0] == 'j' && n > 1)
        (void)snprintf(out, cap, "if (cond) goto %s;", ops);
    else
        (void)snprintf(out, cap, "%s %s;", mnem, ops);
    (void)i;
}

/* ---------- 内置生成 ---------- */
np_dc_status_t np_dc_generate_builtin(const np_disasm_insn_t *insns, size_t n,
                                      const np_cfg_graph_t *g, int func_index,
                                      const char *source, np_dc_result_t *out)
{
    const np_cfg_func_t *f;
    size_t i;
    size_t u = 0;
    if (!insns || !g || !out || func_index < 0 || func_index >= g->nfuncs)
        return NP_DC_ERR_ARG;
    memset(out, 0, sizeof(*out));
    f = &g->funcs[func_index];
    out->backend = NP_DC_BUILTIN;
    out->candidate = true;
    out->func_addr = f->start_addr;
    fscpy(out->func_name, sizeof(out->func_name), f->name);
    fscpy(out->source, sizeof(out->source), source ? source : "(unknown)");
    out->conf = f->conf > 70 ? 70 : f->conf;  /* 启发式保守置信度 */
    /* provenance 头 */
    if (!appendf(out->code, sizeof(out->code), &u,
        "// 候选·待确认 · 内置启发式伪代码（非精确反编译）\n"
        "// 来源：%s · 函数：%s @0x%llx · 置信度 %d\n\n"
        "%s() {\n",
        out->source, out->func_name, (unsigned long long)f->start_addr,
        out->conf, out->func_name)) return NP_DC_ERR_FULL;
    /* 逐指令生成伪代码行并登记 offset 映射 */
    for (i = f->start_insn; i <= f->end_insn && i < n; i++) {
        char pseudo[NP_DC_MAX_LINE], clean[NP_DC_MAX_LINE];
        np_dc_sanitize(insns[i].text, clean, sizeof(clean));
        insn_to_pseudo(clean, pseudo, sizeof(pseudo));
        if (!add_line(out, insns[i].offset, insns[i].addr, pseudo))
            return NP_DC_ERR_FULL;
        if (!appendf(out->code, sizeof(out->code), &u,
                     "    /* 0x%llx */ %s\n",
                     (unsigned long long)insns[i].addr, pseudo))
            return NP_DC_ERR_FULL;
    }
    if (!appendf(out->code, sizeof(out->code), &u, "}\n")) return NP_DC_ERR_FULL;
    return NP_DC_OK;
}

/* ---------- 外部后端 ---------- */
np_dc_status_t np_dc_generate_external(np_dc_backend_t backend,
                                       const char *local_path,
                                       const char *func_name, uint64_t func_addr,
                                       np_dc_backend_fn fn, void *user,
                                       const char *source,
                                       np_dc_result_t *out)
{
    char raw[NP_DC_MAX_CODE];
    size_t u = 0;
    char *cur;
    if (!local_path || !fn || !out) return NP_DC_ERR_ARG;
    if (backend < NP_DC_GHIDRA || backend > NP_DC_CUSTOM)
        return NP_DC_ERR_UNSUPPORTED;
    memset(out, 0, sizeof(*out));
    raw[0] = '\0';
    /* 只传本地路径与函数名，不传二进制内容、不访问网络 */
    if (!fn(local_path, func_name, func_addr, raw, sizeof(raw), user) ||
        !raw[0]) return NP_DC_ERR_BACKEND;
    out->backend = backend;
    out->candidate = true;
    out->func_addr = func_addr;
    fscpy(out->func_name, sizeof(out->func_name), func_name ? func_name : "");
    fscpy(out->source, sizeof(out->source), source ? source : local_path);
    out->conf = 50;  /* 外部输出未经 DC5 验证，置信度保守 */
    /* provenance 头（清洗不可信输出） */
    if (!appendf(out->code, sizeof(out->code), &u,
        "// 候选·待确认 · 外部后端 %s（未经 DC5 等价验证）\n"
        "// 来源：%s · 函数：%s @0x%llx\n\n",
        np_dc_backend_name(backend), out->source, out->func_name,
        (unsigned long long)func_addr)) return NP_DC_ERR_FULL;
    /* 逐行清洗 + offset 映射（无 R2 指令数组，按行序占位 SIZE_MAX）。
     * 手动按 \n 切分而非 strtok：可移植、避免 MSVC C4996、保留空行语义。 */
    cur = raw;
    while (*cur) {
        char clean[NP_DC_MAX_LINE];
        char *nl = strchr(cur, '\n');
        if (nl) *nl = '\0';
        np_dc_sanitize(cur, clean, sizeof(clean));
        if (!add_line(out, (size_t)-1, 0, clean)) return NP_DC_ERR_FULL;
        if (!appendf(out->code, sizeof(out->code), &u, "%s\n", clean))
            return NP_DC_ERR_FULL;
        if (!nl) break;
        cur = nl + 1;
    }
    return NP_DC_OK;
}

/* ---------- 查询 ---------- */
int np_dc_line_for_offset(const np_dc_result_t *r, size_t offset)
{
    int i;
    if (!r) return -1;
    for (i = 0; i < r->nlines; i++)
        if (r->lines[i].offset == offset) return r->lines[i].line_no;
    return -1;
}

size_t np_dc_offset_for_line(const np_dc_result_t *r, int line_no)
{
    if (!r || line_no < 1 || line_no > r->nlines) return (size_t)-1;
    return r->lines[line_no - 1].offset;
}

/* ---------- 渲染 ---------- */
int np_dc_render(const np_dc_result_t *r, char *out, size_t cap)
{
    size_t u = 0;
    int i;
    if (!r || !out || cap < 128) return -1;
    if (!appendf(out, cap, &u, "# 反编译候选 · %s\n\n", r->func_name)) return -1;
    if (!appendf(out, cap, &u,
        "- 后端：%s · 置信度 %d/100 · %s\n- 来源：%s @0x%llx\n\n",
        np_dc_backend_name(r->backend), r->conf,
        r->candidate ? "候选·待确认" : "（未标注）",
        r->source, (unsigned long long)r->func_addr)) return -1;
    if (!appendf(out, cap, &u, "## 伪代码（逐行 offset 映射）\n\n")) return -1;
    for (i = 0; i < r->nlines; i++) {
        const np_dc_line_t *ln = &r->lines[i];
        if (!appendf(out, cap, &u, "%3d | 0x%llx | %s\n",
            ln->line_no, (unsigned long long)ln->addr, ln->text)) return -1;
    }
    if (!appendf(out, cap, &u,
        "\n> 所有输出均为候选·待确认，未覆盖原文件、未写回二进制；"
        "未经 DC5 等价验证不得标为等价。\n")) return -1;
    return (int)u;
}
