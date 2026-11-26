/* np_review.h — M2/B3: 统一 Patch Review 会话模型.
 *
 * 文档要求（B3 · S1 · P0）：文本与字节改动共用一套审查逻辑，
 * cui_diff_view 外壳共用；文本按行 hunk、字节按 offset 块；逐块接受/拒绝。
 *
 * 会话 = 一组候选 hunk + 逐 hunk 状态 + 提交前置检（H8 安全、H2 完整性）。
 * 部分接受 → 重建仅含 accepted 的 patch_set 再 apply（A3 协议）。
 * 字节线 hunk 附带前后校验和（H2 页脚），文本线 hunk 附行号范围。
 *
 * 本模块是纯模型（无 UI），UI 在 np_review_ui.c 接 cui_diff_view。
 */
#ifndef NP_REVIEW_H
#define NP_REVIEW_H

#include <stddef.h>
#include <stdbool.h>

#include "np_patch.h"
#include "np_bytepatch.h"
#include "np_filesafety.h"
#include "np_integrity.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NP_REVIEW_PENDING = 0,
    NP_REVIEW_ACCEPTED,
    NP_REVIEW_REJECTED
} np_review_state_t;

const char *np_review_state_name(np_review_state_t s);

/* 单个待审 hunk 的展示与守卫信息 */
typedef struct {
    np_review_state_t state;
    /* 文本轨 */
    size_t line_first, line_last;   /* 0-based 半开（patch API 语义） */
    np_patch_op_t op;
    char preview[96];               /* 单行摘要（截断） */
    /* 字节轨 */
    size_t byte_off, byte_len, new_len; /* [off,off+len) → new_len 字节 */
    /* 守卫 */
    bool guard_blocked;             /* H8 禁写 / H2 保护区冲突 */
    char guard_reason[96];
} np_review_hunk_t;

typedef struct np_review np_review_t;

/* 创建会话：track 决定文本/字节线。base_revision 来自当前 buffer。 */
np_review_t *np_review_create(np_track_t track, uint64_t base_revision);
void np_review_destroy(np_review_t *rv);

/* 从 patch set 导入全部文本 hunk（复制）。返回导入数。 */
size_t np_review_import_text(np_review_t *rv, const np_patch_set_t *ps);

/* 从字节轨 patch set 导入全部字节 hunk（复制；逐 hunk 过 H2 保护区预检）。
 * prot 可为 NULL。返回导入数。 */
size_t np_review_import_byte(np_review_t *rv, const np_patch_set_t *ps,
                             const np_protected_t *prot, size_t nprot);

/* 从字节编辑会话导入单个字节操作（H1 splice 描述）。 */
bool np_review_add_byte(np_review_t *rv, size_t off, size_t len,
                        const unsigned char *bytes, size_t bytes_len,
                        const np_protected_t *prot, size_t nprot);

/* 审查操作。 */
bool np_review_accept(np_review_t *rv, size_t idx);
bool np_review_reject(np_review_t *rv, size_t idx);
size_t np_review_count(const np_review_t *rv);
const np_review_hunk_t *np_review_hunk(const np_review_t *rv, size_t idx);

/* H8 安全预检：对整组改动判定目标文件可写性（决定面板是否允许提交）。 */
typedef struct {
    bool allowed;               /* 是否允许提交（全部线） */
    char reason[160];
} np_review_gate_t;

/* gate 由调用方在会话创建后设置（UI 每文件一次）。 */
void np_review_set_gate(np_review_t *rv, bool allowed, const char *reason);

/* 提交：把 accepted 的 hunk 重建为 patch_set 并 apply 到 buffer。
 * 文本轨要求 base_revision 匹配；字节轨在一个新 undo group 内依次执行
 * np_bytepatch_replace/insert/delete。
 * 错误码经 why 输出（可 NULL）。 */
np_patch_result_t np_review_commit_text(np_review_t *rv, np_patch_buffer_t *b,
                                        char *why, size_t why_cap);
np_bp_result_t np_review_commit_byte(np_review_t *rv, np_bytepatch_t *bp,
                                     char *why, size_t why_cap);

/* 页脚：accepted 字节改动的合计（长度增量 + 前/后 SHA-256 由调用方在
 * apply 前后计算展示；这里给合计 delta）。 */
long np_review_byte_delta(const np_review_t *rv);

#ifdef __cplusplus
}
#endif
#endif /* NP_REVIEW_H */
