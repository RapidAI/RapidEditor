/* np_texttools.h — S1: X7 原生文本编辑工具集（供 AI 直接调用）.
 *
 * 文档要求（X7）：模型不能只给"建议"，需要稳定地读选区、搜索、定位与提交
 * 补丁。四个原生工具，全部走 A3 三轨 Patch / A1 Job 契约，产出 B5 Citation。
 *
 *   editor.text.read    读行范围（附行号，供模型引用）
 *   editor.text.search  字面子串搜索 → 命中行号列表
 *   editor.text.patch   提交文本补丁（绑定 base_revision，成功即 revision+1）
 *   editor.text.goto    行号 ↔ 字节偏移换算
 *
 * 全部函数在 np_patch_buffer_t 上操作（可撤销），不直接碰磁盘。
 */
#ifndef NP_TEXTTOOLS_H
#define NP_TEXTTOOLS_H

#include <stddef.h>
#include <stdbool.h>

#include "np_patch.h"
#include "np_capability.h"
#include "np_citation.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NP_TXT_MAX_HITS 32

typedef enum {
    NP_TXT_OK = 0,
    NP_TXT_ERR_BAD_ARGS,
    NP_TXT_ERR_OUT_OF_RANGE,     /* 行号越界 */
    NP_TXT_ERR_STALE_REVISION,   /* 补丁基线过期（A3 契约） */
    NP_TXT_ERR_PATCH_FAILED,
    NP_TXT_ERR_NO_HIT
} np_txt_status_t;

const char *np_txt_status_name(np_txt_status_t s);

/* --- editor.text.read --- */
/* 读取 [start_line, end_line]（1-based，含端点）。结果写入 out_text
 * （NUL 结尾）。start_line=0 表示全文件。返回状态码。 */
np_txt_status_t np_txt_read(const np_patch_buffer_t *b,
                            size_t start_line, size_t end_line,
                            char *out_text, size_t cap,
                            size_t *lines_read);

/* --- editor.text.search --- */
/* 字面子串搜索（不区分大小写可选）。命中行号写入 out_lines。 */
np_txt_status_t np_txt_search(const np_patch_buffer_t *b,
                              const char *needle, bool case_insensitive,
                              size_t out_lines[], size_t out_cap,
                              size_t *n_hits);

/* --- editor.text.patch --- */
/* 提交文本补丁组（A3 协议：ps 的 base_revision 不符即拒绝）。
 * 成功后 *new_revision = b->revision。 */
np_txt_status_t np_txt_patch(np_patch_set_t *ps, np_patch_buffer_t *b,
                             size_t *new_revision);

/* --- editor.text.goto --- */
/* 行号 → 字节偏移（行首）。行号从 1 开始。 */
np_txt_status_t np_txt_goto(const np_patch_buffer_t *b, size_t line,
                            size_t *byte_offset);

/* --- 注册进能力注册表（X1/X2 路由用） --- */
/* 注册 4 个 NP_TOOL_NATIVE 工具描述符（core 常驻）。
 * 返回成功注册的数量。 */
size_t np_texttools_register(np_registry_t *reg);

#ifdef __cplusplus
}
#endif
#endif /* NP_TEXTTOOLS_H */
