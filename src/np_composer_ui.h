/* np_composer_ui.h — M3/B1: Composer 工作台面板（意图表单 → Job 卡 → B3）.
 *
 * 文档要求（B1 · S1）：侧栏是任务表单而非纯对话。提交意图 → Job 状态机
 * 可观察；附件芯片区分文本/字节/格式三轨；发送前审计摘要可见；每次调用
 * 带 Citation 芯片；patch_set 不直接 apply ——「送入补丁审查」交给 B3。
 *
 * 模式与 np_review_panel 一致：主程序每帧 bind + draw，逻辑在本模块。
 * 模型层 np_composer.c 已由 test_m3_composer.c 覆盖。
 */
#ifndef NP_COMPOSER_UI_H
#define NP_COMPOSER_UI_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "cui/cui.h"
#include "np_contract.h"
#include "np_composer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NP_CPUI_NONE = 0,
    NP_CPUI_CLOSED,         /* 用户点 ✕ */
    NP_CPUI_SEND_REVIEW     /* 「送入补丁审查」：调用方取 patch_set 注入 B3 */
} np_cpui_action_t;

const char *np_cpui_action_name(np_cpui_action_t a);

typedef struct np_composer_ui np_composer_ui_t;

/* 进程级单例。 */
np_composer_ui_t *np_composer_ui_get(void);

/* 每帧绑定当前文档（深拷贝进模型；内容变化才重绑）。 */
void np_composer_ui_bind(np_composer_ui_t *cu, const char *path,
                         np_track_t track, uint64_t revision,
                         const unsigned char *data, size_t len);

/* 当前选区（字节偏移；文本轨换算成行号芯片，字节轨直接作 0x 芯片）。 */
void np_composer_ui_set_selection(np_composer_ui_t *cu,
                                  size_t off0, size_t off1);

/* 每帧绘制。 */
np_cpui_action_t np_composer_ui_draw(np_composer_ui_t *cu, cui_ctx *ui);

/* NP_CPUI_SEND_REVIEW 后取 Composer 产出的 patch_set（模型层拥有，
 * 调用方只在注入 B3 前保持有效；B3 导入时逐 hunk 复制）。 */
const np_patch_set_t *np_composer_ui_patch(np_composer_ui_t *cu);

#ifdef __cplusplus
}
#endif
#endif /* NP_COMPOSER_UI_H */
