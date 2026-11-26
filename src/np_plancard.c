/* np_plancard.c — M4/B2: 计划卡模型（无 CUI 依赖，可单测）. */
#include "np_plancard.h"
#include "np_texttools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

np_plan_t *np_plan_create(void) {
    return (np_plan_t *)calloc(1, sizeof(np_plan_t));
}

void np_plan_destroy(np_plan_t *p) { free(p); }

static void parch_to_arch(np_parch_t p, np_arch_t *out) {
    switch (p) {
    case NP_PARCH_X86:       *out = NP_ARCH_X86; break;
    case NP_PARCH_X64:       *out = NP_ARCH_X64; break;
    case NP_PARCH_ARM:       *out = NP_ARCH_ARM; break;
    case NP_PARCH_AARCH64:   *out = NP_ARCH_ARM64; break;
    case NP_PARCH_LOONGARCH: *out = NP_ARCH_LOONGARCH64; break;
    case NP_PARCH_MIPS:      *out = NP_ARCH_MIPS; break;
    case NP_PARCH_RISCV:     *out = NP_ARCH_RISCV; break;
    default:                 *out = NP_ARCH_UNKNOWN; break;
    }
}

static void add_step(np_plan_t *p, const char *title, const char *detail,
                     int status) {
    if (p->nsteps >= NP_PLAN_MAX_STEPS) return;
    np_plan_step_info_t *s = &p->steps[p->nsteps++];
    snprintf(s->title, sizeof(s->title), "%s", title);
    snprintf(s->detail, sizeof(s->detail), "%s", detail ? detail : "");
    s->status = status;
}

static void route_tools(np_plan_t *p, np_registry_t *reg, const char *query) {
    char ids[NP_PLAN_MAX_TOOLS][80];
    size_t n = np_registry_route(reg, query, 24, NP_PLAN_MAX_TOOLS,
                                 ids, NP_PLAN_MAX_TOOLS);
    p->ntools = 0;
    for (size_t i = 0; i < n; ++i) {
        const np_tool_desc_t *d = np_registry_get(reg, ids[i]);
        snprintf(p->tool_ids[p->ntools], sizeof(p->tool_ids[0]), "%s", ids[i]);
        snprintf(p->tool_names[p->ntools], sizeof(p->tool_names[0]), "%s",
                 d ? d->name : ids[i]);
        p->ntools++;
    }
}

void np_plan_refresh(np_plan_t *p, const char *path,
                     const unsigned char *data, size_t len,
                     np_registry_t *reg) {
    if (!p) return;
    memset(p, 0, sizeof(*p));

    np_probe_result_t pr;
    np_probe(data, len, path, &pr);
    p->route = pr.route;
    p->track = pr.track;
    snprintf(p->route_desc, sizeof(p->route_desc),
             "%s（置信度 %d%%）%s", np_route_name(pr.route),
             pr.confidence,
             pr.route == NP_ROUTE_HEX && pr.clue[0] ? pr.clue : "");

    switch (pr.route) {
    case NP_ROUTE_PROGRAM: {
        np_prog_info_t *g = &pr.prog;
        snprintf(p->subject_desc, sizeof(p->subject_desc),
                 "%s · %s端序 · %s位宽%s%s",
                 np_parch_name(g->arch),
                 g->endian == NP_ENDIAN_BIG ? "大" : "小",
                 g->bits == 0 ? "?" : (g->bits == 64 ? "64" : "32"),
                 g->is_pie ? " · PIE" : "",
                 g->is_stripped ? " · 无符号表" : "");

        add_step(p, "格式探测（H9/R1）", p->route_desc, NP_PLAN_DONE);

        /* T7 降级链：reason 直出 UI（T6 场景） */
        np_platform_t plat;
        np_toolchain_probe(&plat, NULL);
        np_arch_t need;
        parch_to_arch(g->arch, &need);
        np_ti_pick_t pick;
        bool ok = np_ti_pick(&plat, "disasm", need, plat.has_capstone_v6,
                             "objdump", &pick);
        p->has_t7 = true;
        p->t7 = pick;
        add_step(p, "选择分析工具链（T7 降级链）",
                 ok ? pick.reason : "未找到可用的反汇编工具层",
                 ok ? NP_PLAN_DONE : NP_PLAN_FAILED);
        add_step(p, "反汇编与函数边界（R2/DC1）",
                 "S2 集成 Capstone v6 后启用；AI 只做解释，命名标「待确认」",
                 NP_PLAN_PENDING);
        break;
    }
    case NP_ROUTE_FORMAT: {
        snprintf(p->subject_desc, sizeof(p->subject_desc), "%s",
                 np_format_name(pr.fmt.kind));
        add_step(p, "格式探测（H9/F1）", p->route_desc, NP_PLAN_DONE);

        if (pr.fmt.kind == NP_FMT_SQLITE && data && len >= 16) {
            np_db_status_t st = np_db_parse(data, len, &p->schema);
            if (st == NP_DB_OK && p->schema.valid) {
                p->has_schema = true;
                snprintf(p->subject_desc, sizeof(p->subject_desc),
                         "SQLite · %u 页 · 页大小 %u · %s",
                         p->schema.n_pages, p->schema.page_size,
                         p->schema.wal ? "WAL 模式" : "回滚日志模式");
                char det[192];
                snprintf(det, sizeof(det), "%d 张表 · %d 个索引/视图/触发器%s",
                         p->schema.ntables, p->schema.nobjects,
                         p->schema.partially_parsed ? "（遍历中断，部分结果）" : "");
                add_step(p, "提取 Schema（F3 → M1 np_sqlite）", det, NP_PLAN_DONE);
                add_step(p, "引用回跳与上下文片段（B5）",
                         "点击树节点生成 file#表/列 引用并本地读回验证",
                         NP_PLAN_DONE);
                add_step(p, "结构化写回（F8 SQL Patch）",
                         "S2 提供结构化 writer 后启用；禁止直接改 DB 页字节",
                         NP_PLAN_SKIPPED);
            } else {
                add_step(p, "提取 Schema（F3）",
                         st == NP_DB_ERR_CORRUPT ? "头字段非法，拒绝解析"
                                                 : "解析失败",
                         NP_PLAN_FAILED);
            }
        } else {
            add_step(p, "解析容器结构（F2）",
                     "S2 提供 OOXML/ZIP 部件解析后启用", NP_PLAN_PENDING);
            add_step(p, "结构化写回（F8）",
                     "S2 结构化 writer 后启用", NP_PLAN_SKIPPED);
        }
        break;
    }
    case NP_ROUTE_TEXT:
        snprintf(p->subject_desc, sizeof(p->subject_desc),
                 "可打印率 %.0f%%%s", pr.printable_ratio * 100.0,
                 pr.has_bom ? " · 带 BOM" : "");
        add_step(p, "格式探测（H9）", p->route_desc, NP_PLAN_DONE);
        add_step(p, "路由原生工具（X2）", "见工具清单", NP_PLAN_DONE);
        add_step(p, "文本补丁 → B3 审查",
                 "AI 产出 patch_set 后进入补丁审查面板，逐块接受/拒绝",
                 NP_PLAN_PENDING);
        break;
    default: /* NP_ROUTE_HEX */
        snprintf(p->subject_desc, sizeof(p->subject_desc), "未识别格式");
        add_step(p, "格式探测（H9）", p->route_desc, NP_PLAN_DONE);
        add_step(p, "字节线索解释", "按 Hex + 线索提供解释，不写入", NP_PLAN_PENDING);
        add_step(p, "字节补丁 → B3 审查",
                 "H1 可变长补丁 + 单一撤销组，经补丁审查面板落地",
                 NP_PLAN_PENDING);
        break;
    }

    /* X2 路由：按轨构造查询词，预算内挑工具 */
    if (reg) {
        const char *query;
        switch (pr.route) {
        case NP_ROUTE_PROGRAM: query = "disassemble analyze binary reverse"; break;
        case NP_ROUTE_FORMAT:  query = "database schema format query"; break;
        case NP_ROUTE_HEX:     query = "hex bytes patch"; break;
        default:               query = "read search patch text edit"; break;
        }
        route_tools(p, reg, query);
    }
}

void np_plan_citation(const np_plan_t *p, const char *file,
                      int table_idx, int col_idx, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    buf[0] = '\0';
    if (!p || !p->has_schema || table_idx < 0 || table_idx >= p->schema.ntables)
        return;
    const np_db_table_t *t = &p->schema.tables[table_idx];
    if (col_idx >= 0 && col_idx < t->ncols)
        snprintf(buf, cap, "%s#%s/%s", file ? file : "", t->name,
                 t->cols[col_idx].name);
    else
        snprintf(buf, cap, "%s#%s", file ? file : "", t->name);
}

size_t np_plan_context_snippet(const np_plan_t *p, int table_idx,
                               char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    buf[0] = '\0';
    if (!p || !p->has_schema || table_idx < 0 || table_idx >= p->schema.ntables)
        return 0;
    const np_db_table_t *t = &p->schema.tables[table_idx];
    size_t off = 0;
    int w;
#define PP(...) do { \
        if (off < cap - 1) { \
            w = snprintf(buf + off, cap - off, __VA_ARGS__); \
            if (w > 0) { off += (size_t)w; if (off > cap - 1) off = cap - 1; } \
        } \
    } while (0)
    PP("[上下文片段 · %s]\n", t->name);
    PP("行数估算: %llu · rootpage %u%s\n",
       (unsigned long long)t->est_rows, t->rootpage,
       t->rowid_alias ? " · INTEGER PK → rowid 别名" : "");
    if (t->sql[0]) PP("SQL: %s\n", t->sql);
    for (int i = 0; i < t->ncols; ++i) {
        const np_db_col_t *c = &t->cols[i];
        PP("列 %d: %s %s%s%s\n", i + 1, c->name,
           c->decl_type[0] ? c->decl_type : "BLOB",
           c->notnull ? " NOT NULL" : "", c->pk ? " PRIMARY KEY" : "");
    }
#undef PP
    return off;
}

/* ------------------------------------------------------------------ */
/* 缺省能力注册表                                                       */
/* ------------------------------------------------------------------ */

static np_registry_t *g_plan_reg;

np_registry_t *np_plan_default_registry(void) {
    if (!g_plan_reg) {
        g_plan_reg = np_registry_create();
        if (g_plan_reg) np_texttools_register(g_plan_reg);
    }
    return g_plan_reg;
}
