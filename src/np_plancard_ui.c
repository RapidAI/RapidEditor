/* np_plancard_ui.c — M4/B2: 计划卡面板 UI. */
#include "np_plancard_ui.h"
#include "np_integrity.h"
#include "np_citation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *np_planui_action_name(np_planui_action_t a) {
    switch (a) {
    case NP_PLANUI_CLOSED: return "closed";
    default: return "none";
    }
}

struct np_plan_ui {
    char     path[260];
    size_t   data_len;
    uint32_t data_crc;
    np_plan_t *plan;

    /* schema 树交互状态 */
    int  sel_table, sel_col;
    bool tbl_open[NP_DB_MAX_TABLES];
    bool obj_open;

    /* 选中节点的 Citation 与本地读回结果 */
    char cite_str[300];
    char cite_excerpt[NP_DB_MAX_SQL + 64];
    uint32_t cite_hash;
    char snippet[NP_DB_MAX_SQL + 512];
};

static np_plan_ui_t g_pui;

np_plan_ui_t *np_plan_ui_get(void) { return &g_pui; }

/* STRUCT 引用的本地读回：path = "表" 或 "表/列"。
 * B5 的 reader 回调不携带 struct path（签名只有 file+范围），
 * 因此 STRUCT 的读回在本模块内完成——excerpt 全部来自 M1 解析事实，
 * 绝不接受模型口述；读回后用 CRC32 做摘要展示（防幻觉）。 */
static bool resolve_struct(np_plan_ui_t *pu, const char *struct_path) {
    const np_plan_t *p = pu->plan;
    if (!p || !p->has_schema) return false;
    char tbl[NP_DB_MAX_NAME];
    const char *col = NULL;
    const char *slash = strchr(struct_path, '/');
    size_t tl = slash ? (size_t)(slash - struct_path) : strlen(struct_path);
    if (tl == 0 || tl >= sizeof(tbl)) return false;
    memcpy(tbl, struct_path, tl);
    tbl[tl] = '\0';
    if (slash && slash[1]) col = slash + 1;

    const np_db_table_t *t = np_db_find_table(&p->schema, tbl);
    if (!t) return false;

    pu->cite_excerpt[0] = '\0';
    if (col) {
        for (int i = 0; i < t->ncols; ++i) {
            if (strcmp(t->cols[i].name, col) == 0) {
                snprintf(pu->cite_excerpt, sizeof(pu->cite_excerpt),
                         "%s %s%s%s", t->cols[i].name,
                         t->cols[i].decl_type[0] ? t->cols[i].decl_type : "BLOB",
                         t->cols[i].notnull ? " NOT NULL" : "",
                         t->cols[i].pk ? " PRIMARY KEY" : "");
                break;
            }
        }
        if (!pu->cite_excerpt[0]) return false;  /* 列名不存在 → 丢弃引用 */
    } else if (t->sql[0]) {
        snprintf(pu->cite_excerpt, sizeof(pu->cite_excerpt), "%s", t->sql);
    } else {
        snprintf(pu->cite_excerpt, sizeof(pu->cite_excerpt), "(无 CREATE TABLE SQL)");
    }
    pu->cite_hash = np_crc32((const unsigned char *)pu->cite_excerpt,
                             strlen(pu->cite_excerpt));
    return true;
}

/* 选中变化 → 重建 citation 串 + 本地读回 + 上下文片段 */
static void update_selection(np_plan_ui_t *pu) {
    if (!pu->plan || !pu->plan->has_schema ||
        pu->sel_table < 0 || pu->sel_table >= pu->plan->schema.ntables) {
        pu->sel_table = -1;
        pu->sel_col = -1;
        pu->cite_str[0] = '\0';
        pu->cite_excerpt[0] = '\0';
        pu->snippet[0] = '\0';
        return;
    }
    np_plan_citation(pu->plan, pu->path, pu->sel_table, pu->sel_col,
                     pu->cite_str, sizeof(pu->cite_str));
    /* B5 协议：字符串先经 np_citation_parse，再本地读回 */
    np_citation_t c;
    if (np_citation_parse(pu->cite_str, &c) &&
        c.kind == NP_CITE_STRUCT && c.path[0]) {
        resolve_struct(pu, c.path);
    } else {
        pu->cite_excerpt[0] = '\0';
    }
    np_plan_context_snippet(pu->plan, pu->sel_table,
                            pu->snippet, sizeof(pu->snippet));
}

/* 把多行文本压成单行（cui_text 按单行渲染） */
static void flatten(char *s) {
    for (; *s; ++s)
        if (*s == '\n' || *s == '\r') *s = ' ';
}

void np_plan_ui_bind(np_plan_ui_t *pu, const char *path,
                     const unsigned char *data, size_t len) {
    if (!pu) return;
    uint32_t crc = data && len ? np_crc32(data, len) : 0;
    if (pu->plan && pu->data_len == len && pu->data_crc == crc &&
        strcmp(pu->path, path ? path : "") == 0)
        return;

    snprintf(pu->path, sizeof(pu->path), "%s", path ? path : "");
    pu->data_len = len;
    pu->data_crc = crc;
    pu->sel_table = -1;
    pu->sel_col = -1;
    pu->cite_str[0] = '\0';
    pu->cite_excerpt[0] = '\0';
    pu->snippet[0] = '\0';
    memset(pu->tbl_open, 0, sizeof(pu->tbl_open));
    pu->obj_open = false;

    if (!pu->plan) pu->plan = np_plan_create();
    np_plan_refresh(pu->plan, path, data, len, np_plan_default_registry());
}

int np_plan_ui_selected_table(const np_plan_ui_t *pu) {
    return pu ? pu->sel_table : -1;
}

size_t np_plan_ui_snippet(const np_plan_ui_t *pu, char *buf, size_t cap) {
    if (!pu || !buf || cap == 0) return 0;
    snprintf(buf, cap, "%s", pu->snippet);
    return strlen(buf);
}

bool np_plan_ui_citation(const np_plan_ui_t *pu, char *buf, size_t cap) {
    if (!pu || !buf || cap == 0 || pu->cite_str[0] == '\0') {
        if (buf && cap) buf[0] = '\0';
        return false;
    }
    snprintf(buf, cap, "%s", pu->cite_str);
    return true;
}

np_planui_action_t np_plan_ui_draw(np_plan_ui_t *pu, cui_ctx *ui) {
    if (!pu) return NP_PLANUI_NONE;
    np_planui_action_t act = NP_PLANUI_NONE;
    np_plan_t *p = pu->plan;

    cui_column(ui, .id = "plan-panel", .width = CUI_FILL, .height = CUI_FILL,
               .gap = 8, .pad = 10) {
        /* Header -------------------------------------------------------- */
        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "计划卡", .size = 13, .font = CUI_FONT_MEDIUM);
            cui_text(ui, "· B2 工具可视化", .size = 11,
                     .color = CUI_COLOR_TEXT_MUTED);
            cui_spacer(ui);
            if (cui_button(ui, "✕", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST))
                act = NP_PLANUI_CLOSED;
        }

        if (!p) {
            cui_text(ui, "当前没有打开的文档。", .size = 12,
                     .color = CUI_COLOR_TEXT_MUTED);
        } else {
            /* 探测结论 ------------------------------------------------------ */
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, np_route_name(p->route), .size = 11,
                         .font = CUI_FONT_MEDIUM);
                cui_text(ui, p->route_desc, .size = 11,
                         .color = CUI_COLOR_TEXT_MUTED);
            }
            if (p->subject_desc[0])
                cui_text(ui, p->subject_desc, .size = 11,
                         .color = CUI_COLOR_TEXT_MUTED);

            /* 执行计划（cui_agent_plan：步骤 + 连接轨 + 进度） ---------------- */
            {
                cui_plan_step ps[NP_PLAN_MAX_STEPS];
                for (int i = 0; i < p->nsteps; ++i) {
                    ps[i].title = p->steps[i].title;
                    ps[i].detail = p->steps[i].detail;
                    ps[i].status = (cui_task_status)p->steps[i].status;
                }
                cui_agent_plan_opts po;
                memset(&po, 0, sizeof po);
                po.title = "执行计划";
                cui_agent_plan_ex(ui, "np-plan", ps, p->nsteps, &po);
            }

            /* T7 工具链预告（reason 直出） ----------------------------------- */
            if (p->has_t7) {
                cui_reasoning(ui, "np-t7", .title = "工具链选择（T7 降级链）",
                              .thinking = false) {
                    char line[320];
                    snprintf(line, sizeof(line), "%s（%s）%s",
                             p->t7.reason, np_ti_tier_name(p->t7.tier),
                             p->t7.tool_id[0] ? p->t7.tool_id : "");
                    cui_text(ui, line, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
                }
            }

            /* X2 工具清单：将自动调用哪些工具 -------------------------------- */
            if (p->ntools > 0) {
                cui_text(ui, "将调用的工具（X2 路由）", .size = 11,
                         .font = CUI_FONT_MEDIUM);
                for (int i = 0; i < p->ntools; ++i) {
                    char id[96];
                    snprintf(id, sizeof(id), "np-tool-%d", i);
                    cui_tool_call_opts to;
                    memset(&to, 0, sizeof to);
                    to.status = CUI_TASK_PENDING;
                    to.detail = p->tool_ids[i];
                    to.icon = CUI_ICON_CPU;
                    cui_tool_call_ex(ui, id, p->tool_names[i], &to);
                }
            }

            /* F3 汇合：SQLite schema 树 + Citation 回跳 ---------------------- */
            if (p->has_schema) {
                cui_text(ui, "Schema 树（点击节点生成引用）", .size = 11,
                         .font = CUI_FONT_MEDIUM);
                for (int i = 0; i < p->schema.ntables; ++i) {
                    const np_db_table_t *t = &p->schema.tables[i];
                    char label[160];
                    snprintf(label, sizeof(label), "%s（%llu 行 · %d 列）###st%d",
                             t->name, (unsigned long long)t->est_rows, t->ncols, i);
                    cui_tree_opts to;
                    memset(&to, 0, sizeof to);
                    to.icon = CUI_ICON_DATABASE;
                    to.selected = i == pu->sel_table;
                    bool expanded = cui_tree_begin(ui, label, &pu->tbl_open[i], &to);
                    if (cui_tree_clicked(ui)) {
                        pu->sel_table = i;
                        pu->sel_col = -1;
                        update_selection(pu);
                    }
                    if (expanded) {
                        for (int j = 0; j < t->ncols; ++j) {
                            const np_db_col_t *c = &t->cols[j];
                            char cl[160];
                            snprintf(cl, sizeof(cl), "%s %s%s%s###sc%d_%d",
                                     c->name,
                                     c->decl_type[0] ? c->decl_type : "BLOB",
                                     c->notnull ? " NOT NULL" : "",
                                     c->pk ? " PK" : "", i, j);
                            cui_tree_opts co;
                            memset(&co, 0, sizeof co);
                            co.leaf = true;
                            co.selected = (i == pu->sel_table && j == pu->sel_col);
                            cui_tree_begin(ui, cl, NULL, &co);
                            if (cui_tree_clicked(ui)) {
                                pu->sel_table = i;
                                pu->sel_col = j;
                                update_selection(pu);
                            }
                        }
                        cui_tree_end(ui);
                    }
                }
                if (p->schema.nobjects > 0) {
                    char ol[128];
                    snprintf(ol, sizeof(ol), "索引 / 视图 / 触发器（%d）###so",
                             p->schema.nobjects);
                    cui_tree_opts oo;
                    memset(&oo, 0, sizeof oo);
                    bool expanded = cui_tree_begin(ui, ol, &pu->obj_open, &oo);
                    if (expanded) {
                        for (int i = 0; i < p->schema.nobjects; ++i) {
                            const np_db_object_t *o = &p->schema.objects[i];
                            char il[192];
                            snprintf(il, sizeof(il), "%s: %s（%s）###ob%d",
                                     o->type, o->name, o->tbl, i);
                            cui_tree_opts io;
                            memset(&io, 0, sizeof io);
                            io.leaf = true;
                            cui_tree_begin(ui, il, NULL, &io);
                        }
                        cui_tree_end(ui);
                    }
                }
                if (p->schema.partially_parsed)
                    cui_text(ui, "⚠ 遍历中断：结果可能不完整（畸形页/超限）",
                             .size = 11, .color = CUI_COLOR_WARNING);
            }

            cui_spacer(ui);

            /* 选中节点的引用卡 ---------------------------------------------- */
            if (pu->cite_str[0]) {
                cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    cui_text(ui, "引用", .size = 11, .font = CUI_FONT_MEDIUM);
                    cui_text(ui, pu->cite_str, .size = 11,
                             .color = CUI_COLOR_SUCCESS);
                }
                if (pu->cite_excerpt[0]) {
                    char ex[NP_DB_MAX_SQL + 64];
                    snprintf(ex, sizeof(ex), "%s", pu->cite_excerpt);
                    flatten(ex);
                    cui_text(ui, ex, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
                    char vh[96];
                    snprintf(vh, sizeof(vh), "本地读回验证 · CRC32 %08X",
                             pu->cite_hash);
                    cui_text(ui, vh, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
                }
            }
            /* 上下文片段（M3 Composer 就绪后经 np_plan_ui_snippet 喂入） */
            if (pu->snippet[0]) {
                char sn[160];
                snprintf(sn, sizeof(sn),
                         "上下文片段已就绪（%zu 字符，供 Composer 使用）",
                         strlen(pu->snippet));
                cui_text(ui, sn, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            }
        }
    }
    return act;
}
