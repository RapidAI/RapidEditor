/* np_ooxml_ui.h — F2: OOXML 结构树面板 UI（cui_tree 展示 OPC 部件 + 结构节点）.
 *
 * 模式与 np_plancard_ui 一致：主程序每帧 bind + draw，逻辑在本模块。
 * 点击节点生成 B5/F5 的 file#结构路径 引用（本地读回，防幻觉）。
 */
#ifndef NP_OOXML_UI_H
#define NP_OOXML_UI_H

#include <stddef.h>
#include <stdbool.h>

#include "cui/cui.h"
#include "np_ooxml.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NP_OXUI_NONE = 0,
    NP_OXUI_CLOSED          /* 用户点 ✕ */
} np_oxui_action_t;

typedef struct np_ox_ui np_ox_ui_t;

/* 进程级单例。 */
np_ox_ui_t *np_ox_ui_get(void);

/* 每帧绑定当前文档。path/len/crc32 变化触发重建结构树。 */
void np_ox_ui_bind(np_ox_ui_t *u, const char *path,
                   const unsigned char *data, size_t len);

/* 每帧绘制，填满可用区。 */
np_oxui_action_t np_ox_ui_draw(np_ox_ui_t *u, cui_ctx *ui);

/* 选中节点的 Citation 串（file#/part#/tag[i]；无选中返回 false）。 */
bool np_ox_ui_citation(const np_ox_ui_t *u, char *buf, size_t cap);

/* 选中节点的上下文片段（label + 类型 + 偏移，写入 buf，返回长度）。 */
size_t np_ox_ui_snippet(const np_ox_ui_t *u, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_OOXML_UI_H */
