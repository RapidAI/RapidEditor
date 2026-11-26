/* np_ooxml_ui.c — F2: OOXML 结构树面板 UI. */
#include "np_ooxml_ui.h"
#include "np_integrity.h"

#include <stdio.h>
#include <string.h>

struct np_ox_ui {
    char     path[260];
    size_t   data_len;
    uint32_t data_crc;
    np_ox_doc_t doc;
    bool     bound;

    int    sel;                          /* 当前选中节点下标 */
    bool   node_open[NP_OX_MAX_NODES];   /* 每个节点的展开状态（按节点下标） */
};

static np_ox_ui_t g_oxui;

np_ox_ui_t *np_ox_ui_get(void) { return &g_oxui; }

static void flatten(char *s) {
    for (; *s; ++s) if (*s == '\n' || *s == '\r') *s = ' ';
}

void np_ox_ui_bind(np_ox_ui_t *u, const char *path,
                   const unsigned char *data, size_t len) {
    if (!u) return;
    uint32_t crc = data && len ? np_crc32(data, len) : 0;
    if (u->bound && u->data_len == len && u->data_crc == crc &&
        strcmp(u->path, path ? path : "") == 0)
        return;

    memset(u, 0, sizeof *u);
    snprintf(u->path, sizeof u->path, "%s", path ? path : "");
    u->data_len = len;
    u->data_crc = crc;
    u->sel = -1;
    u->bound = true;

    if (data && len) {
        np_ox_status_t st = np_ox_parse_mem(data, len, &u->doc);
        if (st != NP_OX_OK) {
            /* 非 OOXML / 加密 / 损坏：doc 保持空，draw 显示原因 */
            u->doc.valid = false;
        }
    }
}

bool np_ox_ui_citation(const np_ox_ui_t *u, char *buf, size_t cap) {
    if (!u || !buf || cap == 0 || u->sel < 0 || u->sel >= u->doc.nnodes) {
        if (buf && cap) buf[0] = 0;
        return false;
    }
    const np_ox_node_t *nd = &u->doc.nodes[u->sel];
    /* file#结构路径：文件名 + '#' + 节点路径（节点路径已含 /part#...） */
    const char *base = u->path;
    const char *slash = strrchr(base, '/');
    if (slash) base = slash + 1;
    for (const char *p = base; *p; p++)
        if (*p == '\\') base = p + 1;
    snprintf(buf, cap, "%s#%s", base, nd->path);
    return true;
}

size_t np_ox_ui_snippet(const np_ox_ui_t *u, char *buf, size_t cap) {
    if (!u || !buf || cap == 0) return 0;
    if (u->sel < 0 || u->sel >= u->doc.nnodes) { buf[0] = 0; return 0; }
    const np_ox_node_t *nd = &u->doc.nodes[u->sel];
    snprintf(buf, cap, "[%s] %s  ·  +%llu",
             np_ox_node_type_name(nd->type), nd->label,
             (unsigned long long)nd->part_offset);
    flatten(buf);
    return strlen(buf);
}

/* 递归绘制一棵子树（按 parent 关系）。 */
static void draw_node(np_ox_ui_t *u, cui_ctx *ui, int idx, int depth) {
    if (depth > NP_OX_MAX_DEPTH) return;
    np_ox_node_t *nd = &u->doc.nodes[idx];
    char label[NP_OX_MAX_PATH + NP_OX_MAX_LABEL + 8];
    bool has_children = false;
    for (int j = 0; j < u->doc.nnodes; j++)
        if (u->doc.nodes[j].parent == idx) { has_children = true; break; }

    if (nd->type == NP_OX_NODE_PART) {
        snprintf(label, sizeof label, "%s###ox%d", nd->label, idx);
    } else {
        snprintf(label, sizeof label, "%s · %s###ox%d",
                 np_ox_node_type_name(nd->type), nd->label, idx);
    }
    flatten(label);

    cui_tree_opts to;
    memset(&to, 0, sizeof to);
    to.selected = (idx == u->sel);
    if (nd->type == NP_OX_NODE_PART) to.icon = CUI_ICON_FILE;
    else if (nd->type == NP_OX_NODE_TABLE || nd->type == NP_OX_NODE_SHEET) to.icon = CUI_ICON_GRID_3X3;
    else if (nd->type == NP_OX_NODE_STYLE || nd->type == NP_OX_NODE_COMMENT) to.icon = CUI_ICON_TAG;
    else if (nd->type == NP_OX_NODE_FORMULA) to.icon = CUI_ICON_HASH;
    else to.icon = CUI_ICON_LIST;
    if (!has_children) to.leaf = true;

    bool expanded = cui_tree_begin(ui, label, &u->node_open[idx], &to);
    if (cui_tree_clicked(ui)) u->sel = idx;

    if (expanded && has_children) {
        for (int j = 0; j < u->doc.nnodes; j++)
            if (u->doc.nodes[j].parent == idx)
                draw_node(u, ui, j, depth + 1);
        cui_tree_end(ui);
    }
}

np_oxui_action_t np_ox_ui_draw(np_ox_ui_t *u, cui_ctx *ui) {
    if (!u) return NP_OXUI_NONE;
    np_oxui_action_t act = NP_OXUI_NONE;

    cui_column(ui, .id = "ooxml-panel", .width = CUI_FILL, .height = CUI_FILL,
               .gap = 8, .pad = 10) {
        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "OOXML 结构树", .size = 13, .font = CUI_FONT_MEDIUM);
            cui_text(ui, "· F2 OPC 容器", .size = 11,
                     .color = CUI_COLOR_TEXT_MUTED);
            cui_spacer(ui);
            if (cui_button(ui, "✕", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST))
                act = NP_OXUI_CLOSED;
        }

        np_ox_doc_t *d = &u->doc;
        if (!d->valid) {
            const char *msg = "当前文档不是可解析的 OOXML 包。";
            if (d->encrypted) msg = "加密文档不支持结构解析（EncryptedPackage）。";
            cui_text(ui, msg, .size = 12, .color = CUI_COLOR_TEXT_MUTED);
        } else {
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, np_ox_kind_name(d->kind), .size = 11,
                         .font = CUI_FONT_MEDIUM);
                char meta[200];
                snprintf(meta, sizeof meta, "%d 部件 · %d 节点 · %llu 字符%s",
                         d->nparts, d->nnodes,
                         (unsigned long long)d->ntext_chars,
                         d->truncated ? " · 结果可能不完整" : "");
                cui_text(ui, meta, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            }
            if (d->main_part[0])
                cui_text(ui, d->main_part, .size = 11,
                         .color = CUI_COLOR_TEXT_MUTED);

            /* 自检：选中下标越界则清空 */
            if (u->sel >= d->nnodes) u->sel = -1;

            cui_text(ui, "结构（点击节点生成引用）", .size = 11,
                     .font = CUI_FONT_MEDIUM);
            for (int i = 0; i < d->nnodes; i++)
                if (d->nodes[i].parent == -1)
                    draw_node(u, ui, i, 0);
        }

        /* 选中节点的引用卡（本地读回，防幻觉） ---------------------------- */
        if (u->sel >= 0 && u->sel < d->nnodes) {
            cui_spacer(ui);
            char cite[NP_OX_MAX_PATH + 64];
            if (np_ox_ui_citation(u, cite, sizeof cite)) {
                cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    cui_text(ui, "引用", .size = 11, .font = CUI_FONT_MEDIUM);
                    char tmp[NP_OX_MAX_PATH + 64];
                    snprintf(tmp, sizeof tmp, "%s", cite);
                    flatten(tmp);
                    cui_text(ui, tmp, .size = 11, .color = CUI_COLOR_SUCCESS);
                }
            }
            char snip[NP_OX_MAX_LABEL + 80];
            if (np_ox_ui_snippet(u, snip, sizeof snip)) {
                cui_text(ui, snip, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            }
        }
    }
    return act;
}
