/* np_review_panel.c — M2/B3: 统一 Patch Review 面板（M3 起双轨：字节 + 文本）. */
#include "np_review_panel.h"
#include "np_patch.h"
#include "np_integrity.h"
#include "np_bytepatch.h"
#include "np_bytediff.h"

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
    np_track_t track;

    /* 会话 */
    np_review_t   *review;      /* 审查模型 */
    np_bytepatch_t *bp;         /* 字节轨：目标缓冲的工作副本 */
    np_patch_buffer_t pb;       /* 文本轨：目标缓冲的工作副本 */
    bool           pb_init;
    np_fs_decision_t fs_gate;
    char           fs_reason[160];
    bool           has_target;

    /* 提交/撤销（面板内单级撤销，双轨统一）
     * - pre_snap：最近一次成功提交前的工作副本快照 → 「撤销上次提交」
     * - expect  ：面板发起写回后的预期内容。主程序写回后下一帧 bind 的
     *   内容若与 expect 吻合，吸收变更而不是重置会话（否则提交后指纹
     *   变化会把会话连同撤销状态一起清掉）。 */
    unsigned char *pre_snap;
    size_t         pre_len;
    bool           can_undo;
    unsigned char *expect;
    size_t         expect_len;
    uint32_t       expect_crc; /* 缓存：避免每帧对 expect 全量 crc32 */
    bool           expect_undo;

    /* Composer → B3 注入（面板拥有深拷贝；与 Composer 生命周期解耦，
     * 否则 Composer reset 会销毁 ps，滞留的注入指针变悬垂 → UAF） */
    np_patch_set_t *inject;
    char            inject_path[260];  /* 源文档：导入只在同文档会话 */

    /* H7 字节 Diff 页脚信息 */
    bool     diff_active;
    size_t   diff_len_before, diff_len_after;
    uint32_t diff_crc_before, diff_crc_after;

    /* 交互状态 */
    char hex_input[256];        /* 新内容 hex 文本 "DE AD BE EF" */
    char msg[192];              /* 面板内提示（成功/失败） */
    np_fs_decision_t msg_tone;  /* 0=danger 1=ok 2=warn（借用枚举值域） */
};

static np_review_ui_t g_rui;

np_review_ui_t *np_review_ui_get(void) { return &g_rui; }

np_track_t np_review_ui_track(const np_review_ui_t *rv) {
    return rv ? rv->track : NP_TRACK_BINARY;
}

/* 快照复制（提交前快照 / 写回预期内容共用）。 */
static bool snap_copy(const unsigned char *src, size_t n,
                      unsigned char **dst, size_t *dlen) {
    if (!src) return false;
    unsigned char *p = malloc(n ? n : 1);
    if (!p) return false;
    memcpy(p, src, n);
    free(*dst);
    *dst = p;
    *dlen = n;
    return true;
}

/* 记录"面板发起写回"的预期内容（含指纹缓存，bind 吸收用）。 */
static bool set_expect(np_review_ui_t *r, const unsigned char *src, size_t n) {
    if (!snap_copy(src, n, &r->expect, &r->expect_len)) return false;
    r->expect_crc = np_crc32(r->expect, r->expect_len);
    return true;
}

/* 提交成功后的收尾：快照→可撤销；记录写回预期；重建审查模型——
 * 旧 hunk 的行号/偏移在提交后已失效，且 commit 会推进 buffer revision，
 * 不重建的话二次提交必撞 stale base revision。 */
static void post_commit(np_review_ui_t *r, bool snapped) {
    if (snapped) {
        r->can_undo = true;
    } else {
        free(r->pre_snap); r->pre_snap = NULL; r->pre_len = 0; r->can_undo = false;
    }
    size_t alen = 0;
    const unsigned char *ad = np_review_ui_data(r, &alen);
    if (ad) set_expect(r, ad, alen);
    r->expect_undo = false;
    uint64_t base = (r->track == NP_TRACK_TEXT && r->pb_init)
                        ? r->pb.revision : 0;
    np_review_destroy(r->review);
    r->review = np_review_create(r->track, base);
    r->has_target = r->review != NULL;
    if (!r->has_target) r->can_undo = false;
}

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
    if (r->pb_init) { np_patch_buffer_free(&r->pb); r->pb_init = false; }
    free(r->pre_snap); r->pre_snap = NULL; r->pre_len = 0; r->can_undo = false;
    free(r->expect);   r->expect   = NULL; r->expect_len = 0;
    r->expect_crc = 0; r->expect_undo = false;
    r->has_target = false;
    r->msg[0] = '\0';
}

/* 深拷贝 patch_set（hunk 结构公开，add_* 内部再做二级复制）。 */
static np_patch_set_t *clone_patch_set(const np_patch_set_t *ps) {
    if (!ps) return NULL;
    np_patch_set_t *cl = np_patch_set_create(np_patch_set_revision(ps));
    if (!cl) return NULL;
    for (size_t i = 0; i < np_patch_set_count(ps); ++i) {
        const np_patch_hunk_t *h = np_patch_set_hunk(ps, i);
        np_patch_result_t rr;
        switch (h->track) {
        case NP_TRACK_TEXT:
            rr = np_patch_add_text(cl, h->op, h->u.text.line_first,
                                   h->u.text.line_last,
                                   h->u.text.text, h->u.text.text_len);
            break;
        case NP_TRACK_BINARY:
            rr = np_patch_add_byte(cl, h->op, h->u.byte.offset,
                                   h->u.byte.len, h->u.byte.bytes,
                                   h->u.byte.bytes_len);
            break;
        default:
            rr = np_patch_add_format(cl, h->op, h->u.format.part,
                                     h->u.format.path, h->u.format.payload,
                                     h->u.format.payload_len);
            break;
        }
        if (rr != NP_PATCH_OK) {
            np_patch_set_destroy(cl);
            return NULL;
        }
    }
    return cl;
}

/* 丢弃待注入的 patch_set。 */
static void drop_inject(np_review_ui_t *r) {
    np_patch_set_destroy(r->inject);
    r->inject = NULL;
    r->inject_path[0] = '\0';
}

/* H2 保护区：文件头 8 字节（定义在下方；bind_locked 的字节轨导入要用） */
static size_t header_protect(const unsigned char *d, size_t len,
                             np_protected_t *out);

static void bind_locked(np_review_ui_t *r, const char *path,
                        const unsigned char *data, size_t len,
                        bool user_write_enabled, np_track_t track) {
    uint32_t crc = data && len ? np_crc32(data, len) : 0;
    bool undo_landed = false;
    /* 面板发起的写回到达（内容与 expect 吻合）：吸收而不是重置。
     * 有待注入的 patch_set 时不吸收——导入只发生在重置路径，
     * 吸收会让 Composer 的"送入补丁审查"静默丢失。 */
    if (r->expect && !r->inject && r->expect_len == len &&
        r->track == track && strcmp(r->path, path ? path : "") == 0 &&
        crc == r->expect_crc) {
        bool was_undo = r->expect_undo;
        free(r->expect); r->expect = NULL;
        r->expect_len = 0; r->expect_crc = 0; r->expect_undo = false;
        if (!was_undo) {
            /* 提交写回：pb/bp 已是提交后内容 → 会话保持，撤销可用 */
            r->data_len = len;
            r->data_crc = crc;
            r->user_write_enabled = user_write_enabled;
            r->fs_gate = np_fs_decide(r->path, data, len, user_write_enabled);
            snprintf(r->fs_reason, sizeof(r->fs_reason), "%s",
                     np_fs_reason(r->path, data, len));
            return;
        }
        /* 撤销写回：快照已回滚进工作副本，落到底部按普通重置重建干净会话 */
        undo_landed = true;
    }
    /* 同一文档且内容未变且同轨 → 保持会话 */
    if (r->has_target && r->track == track && r->data_len == len &&
        r->data_crc == crc && strcmp(r->path, path ? path : "") == 0) {
        r->user_write_enabled = user_write_enabled;
        /* gate 每帧复算（用户开关可变） */
        r->fs_gate = np_fs_decide(r->path, data, len, user_write_enabled);
        snprintf(r->fs_reason, sizeof(r->fs_reason), "%s",
                 np_fs_reason(r->path, data, len));
        return;
    }
    /* 新文档/内容变化/换轨 → 重置。
     * H7: inject 触发的强制重建不清 diff 页脚（此时 r->inject 仍在）；
     *     真正切换文档/编辑后才清。 */
    if (!r->inject) r->diff_active = false;
    snprintf(r->path, sizeof(r->path), "%s", path ? path : "");
    r->data_len = len;
    r->data_crc = crc;
    r->user_write_enabled = user_write_enabled;
    r->track = track;
    reset_session(r);

    if (data && len) {
        r->fs_gate = np_fs_decide(r->path, data, len, user_write_enabled);
        snprintf(r->fs_reason, sizeof(r->fs_reason), "%s",
                 np_fs_reason(r->path, data, len));
        if (track == NP_TRACK_TEXT) {
            np_patch_buffer_init(&r->pb);
            r->pb_init = np_patch_buffer_set(&r->pb, data, len);
            r->review = np_review_create(NP_TRACK_TEXT, 0);
            r->has_target = r->pb_init && r->review;
            /* M3/B1 → B3 汇合：导入 Composer 产出的 patch_set
             * （仅同源文档：切标签页不误贴，回源文档再导入） */
            if (r->has_target && r->inject &&
                strcmp(r->path, r->inject_path) == 0) {
                size_t n = np_review_import_text(r->review, r->inject);
                if (n) {
                    snprintf(r->msg, sizeof(r->msg),
                             "已从 AI 工作台导入 %zu 个候选改动", n);
                    r->msg_tone = NP_FS_WRITE_ALLOWED;
                }
                drop_inject(r);
            }
        } else {
            r->bp = np_bytepatch_create(data, len);
            r->review = np_review_create(NP_TRACK_BINARY, 0);
            r->has_target = r->bp && r->review;
            /* M3/B1 → B3 汇合（字节轨）：同源文档 + H2 保护区预检 */
            if (r->has_target && r->inject &&
                strcmp(r->path, r->inject_path) == 0) {
                size_t blen = 0;
                const unsigned char *bd = np_bytepatch_data(r->bp, &blen);
                np_protected_t prot[1];
                size_t nprot = header_protect(bd, blen, prot);
                size_t n = np_review_import_byte(r->review, r->inject,
                                                 prot, nprot);
                if (n) {
                    snprintf(r->msg, sizeof(r->msg),
                             "已从 AI 工作台导入 %zu 个候选改动", n);
                    r->msg_tone = NP_FS_WRITE_ALLOWED;
                }
                drop_inject(r);
            }
        }
    }

    /* 撤销写回落地：重置会话后重提提示（reset_session 会清 msg） */
    if (undo_landed) {
        snprintf(r->msg, sizeof(r->msg), "已撤销最近一次提交（会话已重建）");
        r->msg_tone = NP_FS_WRITE_ALLOWED;
    }
}

void np_review_ui_bind(np_review_ui_t *r, const char *path,
                       const unsigned char *data, size_t len,
                       bool user_write_enabled) {
    if (!r) return;
    bind_locked(r, path, data, len, user_write_enabled, NP_TRACK_BINARY);
}

void np_review_ui_bind_ex(np_review_ui_t *r, const char *path,
                          const unsigned char *data, size_t len,
                          bool user_write_enabled, np_track_t track) {
    if (!r) return;
    bind_locked(r, path, data, len, user_write_enabled, track);
}

void np_review_ui_inject_text(np_review_ui_t *r, const np_patch_set_t *ps,
                              const char *src_path) {
    if (!r) return;
    drop_inject(r);
    r->inject = clone_patch_set(ps);
    snprintf(r->inject_path, sizeof(r->inject_path), "%s",
             src_path ? src_path : "");
    /* 强制下一次 bind 重建会话并导入（会话可能已存在且指纹未变） */
    r->data_crc = 0;
    r->data_len = 0;
}

/* H7: 计算 before/after 字节 Diff → 构造字节 patch_set → 注入审查会话。 */
size_t np_review_ui_inject_byte(np_review_ui_t *r,
                                const unsigned char *before, size_t blen,
                                const unsigned char *after, size_t alen,
                                const char *src_path) {
    if (!r || !before || !blen || !after) return 0;
    np_bd_result_t diff;
    if (!np_bytediff_compute(before, blen, after, alen, &diff)) return 0;

    r->diff_active     = true;
    r->diff_len_before = blen;
    r->diff_len_after  = alen;
    r->diff_crc_before = diff.crc_before;
    r->diff_crc_after  = diff.crc_after;

    drop_inject(r);
    np_patch_set_t *ps = np_patch_set_create(0);
    size_t added = 0;
    if (ps) {
        for (size_t i = 0; i < diff.nhunks; ++i) {
            const np_bd_hunk_t *h = &diff.hunks[i];
            const unsigned char *bytes = NULL;
            size_t bytes_len = 0;
            np_patch_op_t op;
            if (h->kind == NP_BD_REPLACE) {
                op = NP_OP_REPLACE;
                bytes = after + h->after_off; bytes_len = h->after_len;
            } else if (h->kind == NP_BD_INSERT) {
                op = NP_OP_INSERT;
                bytes = after + h->after_off; bytes_len = h->after_len;
            } else {
                op = NP_OP_DELETE;
            }
            if (np_patch_add_byte(ps, op, h->before_off, h->before_len,
                                  bytes, bytes_len) == NP_PATCH_OK)
                added++;
        }
    }
    np_bytediff_free(&diff);
    if (!ps || !added) { np_patch_set_destroy(ps); return 0; }

    r->inject = ps;
    snprintf(r->inject_path, sizeof(r->inject_path), "%s",
             src_path ? src_path : "");
    /* 强制下一次 bind 重建会话并导入 */
    r->data_crc = 0;
    r->data_len = 0;
    snprintf(r->msg, sizeof(r->msg),
             "字节 Diff：%zu 处改动（%zu→%zu 字节）", added, blen, alen);
    r->msg_tone = NP_FS_WRITE_ALLOWED;
    return added;
}

bool np_review_ui_diff_footer(const np_review_ui_t *rv,
                              size_t *len_before, size_t *len_after,
                              uint32_t *crc_before, uint32_t *crc_after) {
    if (!rv || !rv->diff_active) return false;
    if (len_before)  *len_before  = rv->diff_len_before;
    if (len_after)   *len_after   = rv->diff_len_after;
    if (crc_before)  *crc_before  = rv->diff_crc_before;
    if (crc_after)   *crc_after   = rv->diff_crc_after;
    return true;
}

void np_review_ui_set_selection(np_review_ui_t *r, size_t off, size_t sel_len) {
    if (!r) return;
    r->sel_off = off;
    r->sel_len = sel_len;
}

const unsigned char *np_review_ui_data(np_review_ui_t *r, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!r) return NULL;
    if (r->track == NP_TRACK_TEXT) {
        if (!r->pb_init) return NULL;
        if (out_len) *out_len = r->pb.len;
        return r->pb.data;
    }
    if (!r->bp) return NULL;
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

/* 文本轨 hunk 行号标签 */
static void text_hunk_label(char *buf, size_t cap, const np_review_hunk_t *h) {
    const char *op = h->op == NP_OP_INSERT ? "插入"
                   : h->op == NP_OP_DELETE ? "删除" : "替换";
    if (h->op == NP_OP_INSERT)
        snprintf(buf, cap, "L%zu  %s", h->line_first + 1, op);
    else
        snprintf(buf, cap, "L%zu-%zu  %s", h->line_first + 1, h->line_last, op);
}

np_rvui_action_t np_review_ui_draw(np_review_ui_t *r, cui_ctx *ui) {
    if (!r) return NP_RVUI_NONE;
    np_rvui_action_t act = NP_RVUI_NONE;
    bool is_text = r->track == NP_TRACK_TEXT;

    cui_column(ui, .id = "review-panel", .width = CUI_FILL, .height = CUI_FILL,
               .gap = 8, .pad = 10) {
        /* Header -------------------------------------------------------- */
        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "补丁审查", .size = 13, .font = CUI_FONT_MEDIUM);
            cui_text(ui, is_text ? "· 文本轨" : "· 二进制轨", .size = 11,
                     .color = CUI_COLOR_TEXT_MUTED);
            cui_spacer(ui);
            if (cui_button(ui, "✕", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST))
                act = NP_RVUI_CLOSED;
        }

        if (!r->has_target) {
            cui_text(ui, is_text ? "当前没有打开的文本文档。"
                                 : "当前没有打开的二进制文档。",
                     .size = 12, .color = CUI_COLOR_TEXT_MUTED);
            cui_text(ui, is_text
                     ? "在 AI 工作台提交意图并「送入补丁审查」后，这里逐块确认 AI 的文本改动。"
                     : "用十六进制模式打开文件后，这里可以审查 AI 的字节补丁。",
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

        /* 字节轨：捕获选区 ------------------------------------------------ */
        if (!is_text && r->has_target) {
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
        }

        /* 候选 hunk 列表 ------------------------------------------------ */
        if (r->has_target) {
            size_t n = np_review_count(r->review);
            if (n == 0) {
                cui_text(ui, is_text
                         ? "暂无候选改动。在 AI 工作台提交意图后「送入补丁审查」。"
                         : "暂无候选改动。选择字节并点击「捕获选区」。",
                         .size = 12, .color = CUI_COLOR_TEXT_MUTED);
            }
            for (size_t i = 0; i < n; ++i) {
                const np_review_hunk_t *h = np_review_hunk(r->review, i);
                char label[200];
                if (is_text) {
                    char tl[64];
                    text_hunk_label(tl, sizeof(tl), h);
                    snprintf(label, sizeof(label), "%s  %s%s", tl, h->preview,
                             h->guard_blocked ? "  ⚠受保护区" : "");
                } else {
                    snprintf(label, sizeof(label), "%s  %s%s", h->preview,
                             h->state == NP_REVIEW_ACCEPTED ? "[✓已接受]" :
                             h->state == NP_REVIEW_REJECTED ? "[✗已拒绝]" : "[待审]",
                             h->guard_blocked ? "  ⚠受保护区" : "");
                }
                cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                    cui_text(ui, label, .size = 11,
                             .color = h->guard_blocked ? CUI_COLOR_DANGER :
                                      h->state == NP_REVIEW_ACCEPTED ? CUI_COLOR_SUCCESS :
                                      h->state == NP_REVIEW_REJECTED ? CUI_COLOR_DANGER :
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
        if (r->has_target) {
            char foot[160];
            if (is_text) {
                size_t acc = 0;
                for (size_t i = 0; i < np_review_count(r->review); ++i)
                    if (np_review_hunk(r->review, i)->state == NP_REVIEW_ACCEPTED)
                        acc++;
                snprintf(foot, sizeof(foot),
                         "已接受 %zu/%zu 处改动 · 提交后可在面板内撤销",
                         acc, np_review_count(r->review));
            } else {
                snprintf(foot, sizeof(foot), "已接受改动合计：文件长度 %+ld 字节",
                         np_review_byte_delta(r->review));
            }
            cui_text(ui, foot, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            /* H7 字节 Diff 页脚：长度前后 + CRC32 前后（H2 完整性证据） */
            if (!is_text) {
                size_t lb = 0, la = 0;
                uint32_t cb = 0, ca = 0;
                if (np_review_ui_diff_footer(r, &lb, &la, &cb, &ca)) {
                    char dfoot[176];
                    snprintf(dfoot, sizeof(dfoot),
                             "字节 Diff：长度 %zu→%zu · CRC32 %08X→%08X",
                             lb, la, cb, ca);
                    cui_text(ui, dfoot, .size = 11, .color = CUI_COLOR_TEXT_MUTED);
                }
            }
        }
        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
            /* 开关变化后的 gate 复算在下一帧 bind 时进行（单帧滞后） */
            cui_checkbox(ui, "允许写入此文件（H8 显式开启）",
                         &r->user_write_enabled);
            cui_spacer(ui);
            /* 双轨统一：撤销最近一次面板内提交（单级） */
            if (cui_button(ui, "撤销上次提交", .size = CUI_SIZE_SM,
                           .disabled = !r->can_undo)) {
                bool rolled = false;
                if (r->track == NP_TRACK_TEXT) {
                    rolled = np_patch_buffer_set(&r->pb, r->pre_snap,
                                                 r->pre_len);
                } else if (r->bp) {
                    np_bytepatch_t *nb =
                        np_bytepatch_create(r->pre_snap, r->pre_len);
                    if (nb) {
                        np_bytepatch_destroy(r->bp);
                        r->bp = nb;
                        rolled = true;
                    }
                }
                if (rolled && set_expect(r, r->pre_snap, r->pre_len)) {
                    r->expect_undo = true;
                    r->can_undo = false;
                    snprintf(r->msg, sizeof(r->msg), "已撤销最近一次提交");
                    r->msg_tone = NP_FS_WRITE_ALLOWED;
                    act = NP_RVUI_UNDO;
                } else {
                    snprintf(r->msg, sizeof(r->msg),
                             "撤销失败：内存不足，工作副本未变动");
                    r->msg_tone = NP_FS_WRITE_DENIED;
                }
            }
            if (cui_button(ui, "应用接受的改动", .size = CUI_SIZE_SM,
                           .variant = CUI_BUTTON_PRIMARY,
                           .disabled = !r->has_target ||
                                       r->fs_gate != NP_FS_WRITE_ALLOWED)) {
                char why[160];
                if (is_text) {
                    size_t plen = 0;
                    const unsigned char *pd = np_review_ui_data(r, &plen);
                    bool snapped = snap_copy(pd, plen, &r->pre_snap, &r->pre_len);
                    np_patch_result_t rr = np_review_commit_text(r->review, &r->pb,
                                                                 why, sizeof(why));
                    if (rr == NP_PATCH_OK) {
                        post_commit(r, snapped);
                        act = NP_RVUI_APPLY;
                        snprintf(r->msg, sizeof(r->msg), "已写入（可在面板内撤销）");
                        r->msg_tone = NP_FS_WRITE_ALLOWED;
                    } else {
                        free(r->pre_snap); r->pre_snap = NULL; r->pre_len = 0;
                        snprintf(r->msg, sizeof(r->msg), "提交失败：%s",
                                 why[0] ? why : np_patch_result_name(rr));
                        r->msg_tone = NP_FS_WRITE_DENIED;
                    }
                } else {
                    size_t plen = 0;
                    const unsigned char *pd = np_review_ui_data(r, &plen);
                    bool snapped = snap_copy(pd, plen, &r->pre_snap, &r->pre_len);
                    np_bp_result_t rr = np_review_commit_byte(r->review, r->bp,
                                                              why, sizeof(why));
                    if (rr == NP_BP_OK) {
                        post_commit(r, snapped);
                        act = NP_RVUI_APPLY;
                        snprintf(r->msg, sizeof(r->msg), "已应用（可在面板内撤销）");
                        r->msg_tone = NP_FS_WRITE_ALLOWED;
                    } else {
                        free(r->pre_snap); r->pre_snap = NULL; r->pre_len = 0;
                        snprintf(r->msg, sizeof(r->msg), "提交失败：%s",
                                 why[0] ? why : np_bp_result_name(rr));
                        r->msg_tone = NP_FS_WRITE_DENIED;
                    }
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
