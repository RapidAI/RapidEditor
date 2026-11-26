/* np_review_panel.c — M2/B3: 统一 Patch Review 面板（二进制轨）. */
#include "np_review_panel.h"
#include "np_integrity.h"
#include "np_bytepatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *np_rvui_action_name(np_rvui_action_t a) {
    switch (a) {
    case NP_RVUI_APPLY: return "apply";
    case NP_RVUI_UNDO: return "undo";
    case NP_RVUI_CLOSED: return "closed";
    default: return "none";
    }
}

struct np_review_ui {
    /* 绑定状态 */
    char     path[260];
    size_t   data_len;
    uint32_t data_crc;          /* 绑定内容指纹（变化→重置会话） */
    bool     user_write_enabled;
    size_t   sel_off, sel_len;

    /* 会话 */
    np_review_t   *review;      /* 审查模型（NP_TRACK_BINARY） */
    np_bytepatch_t *bp;         /* 目标缓冲的工作副本 */
    np_fs_decision_t fs_gate;
    char           fs_reason[160];
    bool           has_target;

    /* 交互状态 */
    char hex_input[256];        /* 新内容 hex 文本 "DE AD BE EF" */
    char msg[192];              /* 面板内提示（成功/失败） */
    np_fs_decision_t msg_tone;  /* 0=danger 1=ok 2=warn（借用枚举值域） */
};

static np_review_ui_t g_rui;

np_review_ui_t *np_review_ui_get(void) { return &g_rui; }

/* "DE AD BE EF" / "DEADBEEF" / "0xdeadbeef" → 字节 */
static size_t parse_hex_bytes(const char *s, unsigned char *out, size_t cap) {
    size_t n = 0;
    int hi = -1;
    for (; *s; ++s) {
        char c = *s;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c == ' ' || c == '\t' || c == ',' || c == ':' || c == '\n') continue;
        else if (c == 'x' || c == 'X') continue;
        else return (size_t)-1; /* 非法字符 */
        if (hi < 0) { hi = v; }
        else {
            if (n >= cap) return (size_t)-2; /* 超长 */
            out[n++] = (unsigned char)((hi << 4) | v);
            hi = -1;
        }
    }
    if (hi >= 0) return (size_t)-1; /* 半个字节悬空 */
    return n;
}

static void reset_session(np_review_ui_t *r) {
    np_review_destroy(r->review); r->review = NULL;
    np_bytepatch_destroy(r->bp);  r->bp = NULL;
    r->has_target = false;
    r->msg[0] = '\0';
}

static void bind_locked(np_review_ui_t *r, const char *path,
                        const unsigned char *data, size_t len,
                        bool user_write_enabled) {
    uint32_t crc = data && len ? np_crc32(data, len) : 0;
    /* 同一文档且内容未变 → 保持会话 */
    if (r->has_target && r->data_len == len && r->data_crc == crc &&
        strcmp(r->path, path ? path : "") == 0) {
        r->user_write_enabled = user_write_enabled;
        /* gate 每帧复算（用户开关可变） */
        r->fs_gate = np_fs_decide(r->path, data, len, user_write_enabled);
        snprintf(r->fs_reason, sizeof(r->fs_reason), "%s",
                 np_fs_reason(r->path, data, len));
        return;
    }
    /* 新文档/内容变化 → 重置 */
    snprintf(r->path, sizeof(r->path), "%s", path ? path : "");
    r->data_len = len;
    r->data_crc = crc;
    r->user_write_enabled = user_write_enabled;
    reset_session(r);

    if (data && len) {
        r->bp = np_bytepatch_create(data, len);
        r->review = np_review_create(NP_TRACK_BINARY, 0);
        r->has_target = r->bp && r->review;
        r->fs_gate = np_fs_decide(r->path, data, len, user_write_enabled);
        snprintf(r->fs_reason, sizeof(r->fs_reason), "%s",
                 np_fs_reason(r->path, data, len));
    }
}

void np_review_ui_bind(np_review_ui_t *r, const char *path,
                       const unsigned char *data, size_t len,
                       bool user_write_enabled) {
    if (!r) return;
    bind_locked(r, path, data, len, user_write_enabled);
}

void np_review_ui_set_selection(np_review_ui_t *r, size_t off, size_t sel_len) {
    if (!r) return;
    r->sel_off = off;
    r->sel_len = sel_len;
}

const unsigned char *np_review_ui_data(np_review_ui_t *r, size_t *out_len) {
    if (!r || !r->bp) { if (out_len) *out_len = 0; return NULL; }
    return np_bytepatch_data(r->bp, out_len);
}

/* H2 保护区：文件头 8 字节（首个真实闭环；R1 魔数表接入后细化长度） */
static size_t header_protect(const unsigned char *d, size_t len,
                             np_protected_t *out) {
    size_t n = len < 8 ? len : 8;
    if (n == 0) return 0;
    memset(out, 0, sizeof(*out));
    out->offset = 0;
    out->len = n;
    memcpy(out->bytes, d, n);
    snprintf(out->name, sizeof(out->name), "%s", "file header (first 8 bytes)");
    return 1;
}

np_rvui_action_t np_review_ui_draw(np_review_ui_t *r, cui_ctx *ui) {
    if (!r) return NP_RVUI_NONE;
    np_rvui_action_t act = NP_RVUI_NONE;

    cui_column(ui, .id = "review-panel", .width = CUI_FILL, .height = CUI_FILL,
               .gap = 8, .pad = 10) {
        /* Header -------------------------------------------------------- */
        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "补丁审查", .size = 13, .font = CUI_FONT_MEDIUM);
            cui_text(ui, "· 二进制轨", .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            cui_spacer(ui);
            if (cui_button(ui, "✕", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST))
                act = NP_RVUI_CLOSED;
        }

        if (!r->has_target) {
            cui_text(ui, "当前没有打开的二进制文档。", .size = 12,
                     .color = CUI_COLOR_TEXT_MUTED);
            cui_text(ui, "用十六进制模式打开文件后，这里可以审查 AI 的字节补丁。",
                     .size = 12, .color = CUI_COLOR_TEXT_MUTED);
        }

        /* Gate 状态 ------------------------------------------------------ */
        if (r->fs_gate == NP_FS_WRITE_ALLOWED)
            cui_text(ui, "允许写入（用户已开启）", .size = 11,
                     .color = CUI_COLOR_SUCCESS);
        else if (r->fs_gate == NP_FS_EXPLAIN_ONLY)
            cui_text(ui, "敏感文件：只解释，永不写入", .size = 11,
                     .color = CUI_COLOR_DANGER);
        else
            cui_text(ui, "默认禁写：可在下方开启写入开关", .size = 11,
                     .color = CUI_COLOR_WARNING);

        /* 捕获选区 ------------------------------------------------------ */
        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
            char sel_label[80];
            if (r->sel_len > 0)
                snprintf(sel_label, sizeof(sel_label), "选区 0x%zX [%zu 字节]",
                         r->sel_off, r->sel_len);
            else
                snprintf(sel_label, sizeof(sel_label), "无选区（在 0x%zX 插入）",
                         r->sel_off);
            cui_text(ui, sel_label, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
        }
        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "新内容(hex)", .size = 11);
            if (cui_input(ui, "rv-hex", r->hex_input, sizeof(r->hex_input),
                          .width = CUI_FILL, .placeholder = "DE AD BE EF 或留空=删除"))
            { }
            if (cui_button(ui, "＋ 捕获选区", .size = CUI_SIZE_SM)) {
                unsigned char parsed[128];
                size_t pn = parse_hex_bytes(r->hex_input, parsed, sizeof(parsed));
                if (pn == (size_t)-1) {
                    snprintf(r->msg, sizeof(r->msg), "hex 格式无效");
                    r->msg_tone = NP_FS_WRITE_DENIED;
                } else if (pn == (size_t)-2) {
                    snprintf(r->msg, sizeof(r->msg), "新内容过长（上限 128 字节）");
                    r->msg_tone = NP_FS_WRITE_DENIED;
                } else {
                    /* H2 保护区 */
                    size_t blen = 0;
                    const unsigned char *bd = np_bytepatch_data(r->bp, &blen);
                    np_protected_t prot[1];
                    size_t nprot = header_protect(bd, blen, prot);
                    bool ok;
                    if (r->sel_len > 0)
                        ok = np_review_add_byte(r->review, r->sel_off, r->sel_len,
                                                pn ? parsed : NULL, pn, prot, nprot);
                    else
                        ok = np_review_add_byte(r->review, r->sel_off, 0,
                                                pn ? parsed : NULL, pn, prot, nprot);
                    if (ok) {
                        snprintf(r->msg, sizeof(r->msg), "已加入候选 hunk #%zu",
                                 np_review_count(r->review));
                        r->msg_tone = NP_FS_WRITE_ALLOWED;
                        r->hex_input[0] = '\0';
                    } else {
                        snprintf(r->msg, sizeof(r->msg), "加入失败（越界或无目标）");
                        r->msg_tone = NP_FS_WRITE_DENIED;
                    }
                }
            }
        }

        /* 候选 hunk 列表 ------------------------------------------------ */
        {
            size_t n = np_review_count(r->review);
            if (n == 0) {
                cui_text(ui, "暂无候选改动。选择字节并点击「捕获选区」。",
                         .size = 12, .color = CUI_COLOR_TEXT_MUTED);
            }
            for (size_t i = 0; i < n; ++i) {
                const np_review_hunk_t *h = np_review_hunk(r->review, i);
                char label[200];
                snprintf(label, sizeof(label), "%s  %s%s", h->preview,
                         h->state == NP_REVIEW_ACCEPTED ? "[✓已接受]" :
                         h->state == NP_REVIEW_REJECTED ? "[✗已拒绝]" : "[待审]",
                         h->guard_blocked ? "  ⚠受保护区" : "");
                cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                    cui_text(ui, label, .size = 11,
                             .color = h->guard_blocked ? CUI_COLOR_DANGER :
                                      CUI_COLOR_TEXT_MUTED);
                    cui_spacer(ui);
                    if (!h->guard_blocked) {
                        if (cui_button(ui, "接受", .size = CUI_SIZE_SM,
                                       .variant = h->state == NP_REVIEW_ACCEPTED
                                                  ? CUI_BUTTON_PRIMARY
                                                  : CUI_BUTTON_GHOST))
                            np_review_accept(r->review, i);
                        if (cui_button(ui, "拒绝", .size = CUI_SIZE_SM,
                                       .variant = h->state == NP_REVIEW_REJECTED
                                                  ? CUI_BUTTON_PRIMARY
                                                  : CUI_BUTTON_GHOST))
                            np_review_reject(r->review, i);
                    } else {
                        cui_text(ui, h->guard_reason, .size = 11,
                                 .color = CUI_COLOR_DANGER);
                    }
                }
            }
        }

        /* 页脚：gate 开关 + 合计 + 提交/撤销 ------------------------------ */
        cui_spacer(ui);
        {
            long delta = np_review_byte_delta(r->review);
            char foot[160];
            snprintf(foot, sizeof(foot), "已接受改动合计：文件长度 %+ld 字节",
                     delta);
            cui_text(ui, foot, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
        }
        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
            /* 开关变化后的 gate 复算在下一帧 bind 时进行（单帧滞后） */
            cui_checkbox(ui, "允许写入此文件（H8 显式开启）",
                         &r->user_write_enabled);
            cui_spacer(ui);
            if (cui_button(ui, "撤销上次应用", .size = CUI_SIZE_SM,
                           .disabled = !(r->bp && np_bytepatch_can_undo(r->bp)))) {
                if (np_bytepatch_undo(r->bp)) act = NP_RVUI_UNDO;
            }
            if (cui_button(ui, "应用接受的改动", .size = CUI_SIZE_SM,
                           .variant = CUI_BUTTON_PRIMARY,
                           .disabled = r->fs_gate != NP_FS_WRITE_ALLOWED)) {
                char why[160];
                np_bp_result_t rr = np_review_commit_byte(r->review, r->bp,
                                                          why, sizeof(why));
                if (rr == NP_BP_OK) {
                    act = NP_RVUI_APPLY;
                    snprintf(r->msg, sizeof(r->msg), "已应用（单一撤销组，可撤销）");
                    r->msg_tone = NP_FS_WRITE_ALLOWED;
                } else {
                    snprintf(r->msg, sizeof(r->msg), "提交失败：%s",
                             why[0] ? why : np_bp_result_name(rr));
                    r->msg_tone = NP_FS_WRITE_DENIED;
                }
            }
        }

        /* 提示条 -------------------------------------------------------- */
        if (r->msg[0]) {
            cui_text(ui, r->msg, .size = 11,
                     .color = r->msg_tone == NP_FS_WRITE_DENIED
                              ? CUI_COLOR_DANGER
                              : r->msg_tone == NP_FS_WRITE_ALLOWED
                                ? CUI_COLOR_SUCCESS : CUI_COLOR_TEXT_MUTED);
        }
    }
    return act;
}
