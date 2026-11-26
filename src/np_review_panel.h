/* np_review_panel.h — M2/B3: 统一 Patch Review 面板（M3 起支持双轨）.
 *
 * 文档要求（B3/H7 · S1）：cui_diff_view 外壳共用、逐块接受/拒绝、
 * 页脚校验和。本模块承载面板状态机，rapideditor.c 只做最小接线。
 *
 * 数据流：
 *   主程序每帧 bind(当前文档 + 选区 + 写开关, 轨道)
 *     → 二进制轨：面板内"捕获选区为新 hunk"（替换/删除/插入，hex 文本编辑）
 *     → 文本轨：hunk 来自 np_review_ui_inject_text 注入的 patch_set
 *       （M3 Composer 产出；模型层 np_review_import_text 复制）
 *     → 逐 hunk 接受/拒绝（H2 保护区冲突的 hunk 标红不可提交）
 *     → 提交（H8 gate 复查 → commit_byte / commit_text 应用到工作副本；
 *       提交前快照，提交后可在面板内「撤销上次提交」——单级，双轨统一。
 *       面板发起的写回通过预期内容（expect）吸收指纹变化，会话存活）
 *     → 返回 APPLY/UNDO/CLOSE 动作，主程序按轨写回 d->text。
 */
#ifndef NP_REVIEW_PANEL_H
#define NP_REVIEW_PANEL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "cui/cui.h"
#include "np_review.h"
#include "np_filesafety.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NP_RVUI_NONE = 0,
    NP_RVUI_APPLY,      /* 提交成功：调用方读取 data 写回文档 */
    NP_RVUI_UNDO,       /* 撤销成功：调用方读取 data 写回文档 */
    NP_RVUI_CLOSED      /* 用户点 ✕ */
} np_rvui_action_t;

const char *np_rvui_action_name(np_rvui_action_t a);

typedef struct np_review_ui np_review_ui_t;

/* 进程级单例。 */
np_review_ui_t *np_review_ui_get(void);

/* 每帧绑定当前文档上下文。内容变化（path/len/crc32）会重置会话。
 * user_write_enabled：用户对当前文件的显式写入开关（H8 opt-in）。
 * 等价于 bind_ex(..., NP_TRACK_BINARY)（M2 行为保持不变）。 */
void np_review_ui_bind(np_review_ui_t *rv, const char *path,
                       const unsigned char *data, size_t len,
                       bool user_write_enabled);

/* 显式轨道绑定。NP_TRACK_BINARY = M2 字节线；NP_TRACK_TEXT = 文本线
 * （hunk 由 inject_text 注入，面板不提供手工捕获）。 */
void np_review_ui_bind_ex(np_review_ui_t *rv, const char *path,
                          const unsigned char *data, size_t len,
                          bool user_write_enabled, np_track_t track);

/* 注入 Composer 产出的文本 patch_set（M3/B1 → B3 汇合）。
 * 面板立即深拷贝（与 Composer 的生命周期解耦：调用方随后 reset/销毁
 * patch_set 都不影响导入）。导入仅发生在绑定了同一文档（src_path）的
 * 会话建立时——切换标签页不会把补丁误贴到别的文件上，回到源文档再导入。 */
void np_review_ui_inject_text(np_review_ui_t *rv, const np_patch_set_t *ps,
                              const char *src_path);

/* 传入选区（hex 视图当前选区，可为空）。 */
void np_review_ui_set_selection(np_review_ui_t *rv, size_t off, size_t sel_len);

/* 每帧绘制。面板宽度由主程序 split 决定，本函数填满可用区。 */
np_rvui_action_t np_review_ui_draw(np_review_ui_t *rv, cui_ctx *ui);

/* NP_RVUI_APPLY / NP_RVUI_UNDO 后读取当前字节（面板内部持有，勿释放；
 * 调用方负责拷贝写回文档）。文本轨返回补丁应用后的整份文本。 */
const unsigned char *np_review_ui_data(np_review_ui_t *rv, size_t *out_len);

/* 当前会话轨道。 */
np_track_t np_review_ui_track(const np_review_ui_t *rv);

#ifdef __cplusplus
}
#endif
#endif /* NP_REVIEW_PANEL_H */
