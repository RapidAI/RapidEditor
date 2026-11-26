/* np_composer_ui.c — M3/B1: Composer 工作台面板. */
#include "np_composer_ui.h"
#include "np_plancard_ui.h"
#include "np_job.h"
#include "np_integrity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *np_cpui_action_name(np_cpui_action_t a) {
    switch (a) {
    case NP_CPUI_CLOSED: return "closed";
    case NP_CPUI_SEND_REVIEW: return "send_review";
    default: return "none";
    }
}

struct np_composer_ui {
    /* 绑定指纹（变化才重绑模型层） */
    char     path[260];
    np_track_t track;
    size_t   data_len;
    uint32_t data_crc;

    np_composer_t *c;
    size_t sel_off0, sel_off1;

    /* 本帧交互 */
    char intent[512];
    char msg[320];
    int  msg_tone;              /* 0=danger 1=ok 2=muted */
    bool audit_dirty;
    char audit[512];
    const unsigned char *frame_data;   /* 帧内数据指针（行号换算用，勿存） */
    size_t frame_len;
};

static np_composer_ui_t g_cui;

np_composer_ui_t *np_composer_ui_get(void) { return &g_cui; }

/* 文本缓冲：字节偏移 → 1-based 行号（行 = '\n' 分隔） */
static size_t byte_to_line(const unsigned char *d, size_t len, size_t off) {
    size_t line = 1;
    if (off > len) off = len;
    for (size_t i = 0; i < off; ++i)
        if (d[i] == '\n') line++;
    return line;
}

static void refresh_audit(np_composer_ui_t *u) {
    u->audit[0] = '\0';
    if (u->c) np_composer_audit(u->c, u->audit, sizeof(u->audit));
    u->audit_dirty = false;
}

void np_composer_ui_bind(np_composer_ui_t *u, const char *path,
                         np_track_t track, uint64_t revision,
                         const unsigned char *data, size_t len) {
    if (!u) return;
    u->frame_data = data;
    u->frame_len = len;
    uint32_t crc = data && len ? np_crc32(data, len) : 0;
    if (u->c && u->track == track && u->data_len == len && u->data_crc == crc &&
        strcmp(u->path, path ? path : "") == 0)
        return;

    snprintf(u->path, sizeof(u->path), "%s", path ? path : "");
    u->track = track;
    u->data_len = len;
    u->data_crc = crc;
    u->msg[0] = '\0';
    u->intent[0] = '\0';
    u->audit_dirty = true;

    if (!u->c) u->c = np_composer_create(NULL, NULL);
    np_composer_bind(u->c, path, track, revision, data, len);
}

void np_composer_ui_set_selection(np_composer_ui_t *u,
                                  size_t off0, size_t off1) {
    if (!u) return;
    u->sel_off0 = off0;
    u->sel_off1 = off1;
}

const np_patch_set_t *np_composer_ui_patch(np_composer_ui_t *u) {
    return u && u->c ? np_composer_patch(u->c) : NULL;
}

np_cpui_action_t np_composer_ui_draw(np_composer_ui_t *u, cui_ctx *ui) {
    if (!u) return NP_CPUI_NONE;
    np_cpui_action_t act = NP_CPUI_NONE;
    np_composer_t *c = u->c;
    np_composer_state_t st = c ? np_composer_state(c) : NP_COMPOSER_IDLE;

    cui_column(ui, .id = "composer-panel", .width = CUI_FILL, .height = CUI_FILL,
               .gap = 8, .pad = 10) {
        /* Header -------------------------------------------------------- */
        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "AI 工作台", .size = 13, .font = CUI_FONT_MEDIUM);
            cui_text(ui, "· B1 Composer", .size = 11,
                     .color = CUI_COLOR_TEXT_MUTED);
            cui_spacer(ui);
            if (cui_button(ui, "✕", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST))
                act = NP_CPUI_CLOSED;
        }

        if (!c || !u->path[0]) {
            cui_text(ui, "当前没有打开的文档。", .size = 12,
                     .color = CUI_COLOR_TEXT_MUTED);
        } else {
            /* 轨道标签 ------------------------------------------------------ */
            cui_text(ui, u->track == NP_TRACK_BINARY
                     ? "文档轨道：二进制（字节选区芯片 / 0x 意图）"
                     : "文档轨道：文本（行选区芯片 / 行意图）",
                     .size = 11, .color = CUI_COLOR_TEXT_MUTED);

            /* 附件芯片 ------------------------------------------------------ */
            cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, "附件", .size = 11, .font = CUI_FONT_MEDIUM);
                if (u->track == NP_TRACK_TEXT && u->frame_data &&
                    u->sel_off1 > u->sel_off0) {
                    size_t l0 = byte_to_line(u->frame_data, u->frame_len,
                                             u->sel_off0);
                    size_t l1 = byte_to_line(u->frame_data, u->frame_len,
                                             u->sel_off1);
                    if (cui_button(ui, "＋ 文本选区", .size = CUI_SIZE_SM)) {
                        if (np_composer_add_text(c, l0, l1)) {
                            snprintf(u->msg, sizeof(u->msg),
                                     "已附文本选区 L%zu-%zu", l0, l1);
                            u->msg_tone = 1;
                            u->audit_dirty = true;
                        } else {
                            snprintf(u->msg, sizeof(u->msg), "附件加入失败");
                            u->msg_tone = 0;
                        }
                    }
                } else if (u->track == NP_TRACK_BINARY &&
                           u->sel_off1 > u->sel_off0) {
                    if (cui_button(ui, "＋ 字节选区", .size = CUI_SIZE_SM)) {
                        size_t off = u->sel_off0;
                        size_t n = u->sel_off1 - u->sel_off0;
                        if (np_composer_add_byte(c, off, n)) {
                            snprintf(u->msg, sizeof(u->msg),
                                     "已附字节选区 0x%zX [%zu]", off, n);
                            u->msg_tone = 1;
                            u->audit_dirty = true;
                        } else {
                            snprintf(u->msg, sizeof(u->msg), "附件加入失败");
                            u->msg_tone = 0;
                        }
                    }
                }
                /* 格式片段：来自计划卡选中的 schema 节点（F3 汇合） */
                {
                    char cite[300], full[NP_DB_MAX_SQL + 512];
                    np_plan_ui_t *pu = np_plan_ui_get();
                    if (np_plan_ui_citation(pu, cite, sizeof(cite)) &&
                        np_plan_ui_snippet(pu, full, sizeof(full)) > 0) {
                        if (cui_button(ui, "＋ 格式片段", .size = CUI_SIZE_SM)) {
                            if (np_composer_add_format(c, full, cite)) {
                                snprintf(u->msg, sizeof(u->msg),
                                         "已附格式片段 %s", cite);
                                u->msg_tone = 1;
                                u->audit_dirty = true;
                            } else {
                                snprintf(u->msg, sizeof(u->msg), "附件加入失败");
                                u->msg_tone = 0;
                            }
                        }
                    }
                }
            }
            {
                size_t na = np_composer_attach_count(c);
                for (size_t i = 0; i < na; ++i) {
                    const np_composer_attach_t *at = np_composer_attach(c, i);
                    char idx[32];
                    snprintf(idx, sizeof(idx), "cp-at-%zu", i);
                    cui_row(ui, .width = CUI_FILL, .gap = 6,
                            .align = CUI_ALIGN_CENTER) {
                        cui_text(ui, at->label, .size = 11);
                        cui_text(ui, at->citation, .size = 11,
                                 .color = CUI_COLOR_TEXT_MUTED);
                        cui_spacer(ui);
                        if (cui_button(ui, "移除", .size = CUI_SIZE_SM,
                                       .variant = CUI_BUTTON_GHOST)) {
                            np_composer_remove_attach(c, i);
                            u->audit_dirty = true;
                        }
                    }
                }
            }

            /* 审计摘要（发送前可见） ------------------------------------------ */
            if (u->audit_dirty) refresh_audit(u);
            if (u->audit[0])
                cui_text(ui, u->audit, .size = 11, .nowrap = false,
                         .color = CUI_COLOR_TEXT_MUTED);

            /* 意图输入 ------------------------------------------------------ */
            cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                if (cui_input(ui, "cp-intent", u->intent, sizeof(u->intent),
                              .width = CUI_FILL,
                              .placeholder = u->track == NP_TRACK_BINARY
                              ? "如：0x40 改成 90 EA"
                              : "如：把第 3 行改成 X / 删除第 2 行"))
                { }
                if (cui_button(ui, "提交", .size = CUI_SIZE_SM,
                               .variant = CUI_BUTTON_PRIMARY)) {
                    np_submit_result_t r = np_composer_submit(c, u->intent);
                    if (r == NP_SUBMIT_OK) {
                        snprintf(u->msg, sizeof(u->msg),
                                 "Job 完成：补丁已生成，等待人工审查");
                        u->msg_tone = 1;
                        u->audit_dirty = true;
                    } else {
                        snprintf(u->msg, sizeof(u->msg), "提交失败：%s",
                                 np_composer_why(c) ? np_composer_why(c)
                                                    : np_submit_result_name(r));
                        u->msg_tone = 0;
                    }
                }
            }

            /* Job 卡 -------------------------------------------------------- */
            if (c) {
                np_job_t *j = np_composer_job(c);
                if (j) {
                    char job_line[256];
                    snprintf(job_line, sizeof(job_line), "Job #%llu · %s · 进度 %d%% · 事件 %d",
                             (unsigned long long)np_job_id(j),
                             np_job_status_name(np_job_status(j)),
                             np_job_progress(j), np_job_event_count(j));
                    cui_tool_call_opts to;
                    memset(&to, 0, sizeof to);
                    to.status = np_job_is_terminal(j)
                                ? (np_job_status(j) == NP_JOB_SUCCEEDED
                                   ? CUI_TASK_DONE : CUI_TASK_FAILED)
                                : CUI_TASK_RUNNING;
                    to.detail = job_line;
                    to.icon = CUI_ICON_SPARKLES;
                    cui_tool_call_ex(ui, "cp-job", "本地意图规划器", &to);
                }

                /* X2 路由工具清单 --------------------------------------------- */
                {
                    char ids[NP_COMPOSER_MAX_TOOLS][80];
                    size_t nt = np_composer_routed_tools(c, ids, NP_COMPOSER_MAX_TOOLS);
                    if (nt) {
                        cui_text(ui, "本次调用的工具（X2 路由）", .size = 11,
                                 .font = CUI_FONT_MEDIUM);
                        for (size_t i = 0; i < nt; ++i) {
                            char id[96];
                            snprintf(id, sizeof(id), "cp-tool-%zu", i);
                            cui_tool_call_opts to;
                            memset(&to, 0, sizeof to);
                            to.status = CUI_TASK_DONE;
                            to.detail = ids[i];
                            to.icon = CUI_ICON_CPU;
                            cui_tool_call_ex(ui, id, ids[i], &to);
                        }
                    }
                }

                /* Citation 芯片 ----------------------------------------------- */
                {
                    char cites[NP_COMPOSER_MAX_CITES][280];
                    size_t nc = np_composer_citations(c, cites, NP_COMPOSER_MAX_CITES);
                    if (nc) {
                        cui_text(ui, "引用（本地读回验证）", .size = 11,
                                 .font = CUI_FONT_MEDIUM);
                        for (size_t i = 0; i < nc; ++i)
                            cui_text(ui, cites[i], .size = 11,
                                     .color = CUI_COLOR_SUCCESS);
                    }
                }

                /* patch_set 预览 ---------------------------------------------- */
                {
                    const np_patch_set_t *ps = np_composer_patch(c);
                    if (ps) {
                        size_t n = np_patch_set_count(ps);
                        cui_text(ui, "产出的补丁（未写入，等待审查）", .size = 11,
                                 .font = CUI_FONT_MEDIUM);
                        for (size_t i = 0; i < n; ++i) {
                            const np_patch_hunk_t *h = np_patch_set_hunk(ps, i);
                            char line[480];
                            if (h->track == NP_TRACK_TEXT) {
                                const char *op = h->op == NP_OP_INSERT ? "插入"
                                               : h->op == NP_OP_DELETE ? "删除"
                                               : "替换";
                                if (h->op == NP_OP_INSERT)
                                    snprintf(line, sizeof(line), "L%zu  %s",
                                             h->u.text.line_first + 1, op);
                                else
                                    snprintf(line, sizeof(line), "L%zu-%zu  %s",
                                             h->u.text.line_first + 1,
                                             h->u.text.line_last, op);
                            } else if (h->track == NP_TRACK_BINARY) {
                                snprintf(line, sizeof(line), "0x%zX [%zu] → %zu 字节",
                                         h->u.byte.offset, h->u.byte.len,
                                         h->u.byte.bytes_len);
                            } else {
                                snprintf(line, sizeof(line), "格式: %s%s",
                                         h->u.format.part, h->u.format.path);
                            }
                            cui_text(ui, line, .size = 11,
                                     .color = CUI_COLOR_TEXT_MUTED);
                        }
                    }
                }
            }

            cui_spacer(ui);

            /* 动作区 -------------------------------------------------------- */
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                if (cui_button(ui, "重置", .size = CUI_SIZE_SM,
                               .variant = CUI_BUTTON_GHOST)) {
                    np_composer_reset(c);
                    u->intent[0] = '\0';
                    u->msg[0] = '\0';
                    u->audit_dirty = true;
                }
                cui_spacer(ui);
                if (cui_button(ui, "送入补丁审查 (B3)", .size = CUI_SIZE_SM,
                               .variant = CUI_BUTTON_PRIMARY,
                               .disabled = st != NP_COMPOSER_DONE))
                    act = NP_CPUI_SEND_REVIEW;
            }
        }

        /* 提示条 -------------------------------------------------------- */
        if (u->msg[0]) {
            cui_text(ui, u->msg, .size = 11,
                     .color = u->msg_tone == 0 ? CUI_COLOR_DANGER
                            : u->msg_tone == 1 ? CUI_COLOR_SUCCESS
                                               : CUI_COLOR_TEXT_MUTED);
        }
    }
    return act;
}
