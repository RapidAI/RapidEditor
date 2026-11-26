/* np_plancard_ui.h — M4/B2: 计划卡面板 UI（cui_agent_plan / reasoning /
 * tool_call / tree 绘制，均已在 vendor CUI 核对存在）.
 *
 * 模式与 np_review_panel 一致：主程序每帧 bind + draw，逻辑在本模块。
 */
#ifndef NP_PLANCARD_UI_H
#define NP_PLANCARD_UI_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "cui/cui.h"
#include "np_plancard.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NP_PLANUI_NONE = 0,
    NP_PLANUI_CLOSED          /* 用户点 ✕ */
} np_planui_action_t;

const char *np_planui_action_name(np_planui_action_t a);

typedef struct np_plan_ui np_plan_ui_t;

/* 进程级单例。 */
np_plan_ui_t *np_plan_ui_get(void);

/* 每帧绑定当前文档。内容变化（path/len/crc32）触发重建计划。 */
void np_plan_ui_bind(np_plan_ui_t *pu, const char *path,
                     const unsigned char *data, size_t len);

/* 每帧绘制。面板宽度由主程序 split 决定，本函数填满可用区。 */
np_planui_action_t np_plan_ui_draw(np_plan_ui_t *pu, cui_ctx *ui);

/* 最近一次选中的表（M3 Composer 消费）。无选中返回 -1。 */
int np_plan_ui_selected_table(const np_plan_ui_t *pu);

/* 选中表的上下文片段（写入 buf，返回长度）。 */
size_t np_plan_ui_snippet(const np_plan_ui_t *pu, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_PLANCARD_UI_H */
