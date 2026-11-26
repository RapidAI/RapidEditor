/* np_plancard.h — M4/B2: 计划卡模型（UI-free，可单测）.
 *
 * 文档要求（B2 · M4）：
 *  - 计划卡：多步任务展示"将自动调用哪些工具"，工具清单来自 X2 路由
 *    （预算 + 评分 + 核心常驻过滤，np_registry_route 已实现）。
 *  - 工具链预告（T6 场景）：打开 ELF → np_probe 识别架构 → 计划卡显示
 *    "将用 bundled Capstone 反汇编（Tier 0），Ghidra 缺失已降级"
 *    （T7 pick 的 reason 直出 UI）。
 *  - F3 汇合点：打开 .db → 路由格式轨 → schema 树（M1 产出），
 *    点击节点 → Citation 回跳（file#路径，B5 协议）→ 选中表生成上下文片段。
 *
 * 本头文件是纯模型层（无 CUI 依赖）；面板 UI 见 np_plancard_ui.h。
 * 红线：模型只读。schema 只展示事实（M1 解析结果）。
 */
#ifndef NP_PLANCARD_H
#define NP_PLANCARD_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "np_contract.h"
#include "np_probe.h"
#include "np_sqlite.h"
#include "np_toolinstall.h"
#include "np_capability.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 计划模型                                                            */
/* ------------------------------------------------------------------ */

/* 与 cui_task_status 对齐的步骤状态（UI 层直接 cast） */
enum {
    NP_PLAN_PENDING = 0,
    NP_PLAN_RUNNING,
    NP_PLAN_DONE,
    NP_PLAN_FAILED,
    NP_PLAN_SKIPPED
};

#define NP_PLAN_MAX_STEPS 8
#define NP_PLAN_MAX_TOOLS 8

typedef struct {
    char title[96];
    char detail[192];
    int  status;                /* NP_PLAN_* */
} np_plan_step_info_t;

typedef struct {
    np_route_t route;
    np_track_t track;
    char route_desc[256];       /* 探测结论人话（置信度/线索） */
    char subject_desc[160];     /* 程序轨=架构/位宽/端序；格式轨=格式名 */
    int  nsteps;
    np_plan_step_info_t steps[NP_PLAN_MAX_STEPS];
    /* X2 路由出的工具清单（"将自动调用哪些工具"） */
    int  ntools;
    char tool_ids[NP_PLAN_MAX_TOOLS][80];
    char tool_names[NP_PLAN_MAX_TOOLS][80];
    /* T7 降级链选择（程序轨有效） */
    bool has_t7;
    np_ti_pick_t t7;
    /* SQLite schema（格式轨且识别为 SQLite 时有效） */
    bool has_schema;
    np_db_schema_t schema;
} np_plan_t;

np_plan_t *np_plan_create(void);
void np_plan_destroy(np_plan_t *p);

/* 重建计划：np_probe 探测路由 →（程序轨）T7 降级链 → X2 工具路由
 * →（SQLite）np_db_parse schema 提取。
 * reg 传 NULL 时跳过 X2 路由（工具清单为空，其余不受影响）。 */
void np_plan_refresh(np_plan_t *p, const char *path,
                     const unsigned char *data, size_t len,
                     np_registry_t *reg);

/* 选中 schema 节点生成 Citation 串（B5: "file#表" 或 "file#表/列"）。
 * table_idx 越界或无 schema → buf[0]='\0'。col_idx < 0 只引用到表。 */
void np_plan_citation(const np_plan_t *p, const char *file,
                      int table_idx, int col_idx, char *buf, size_t cap);

/* 选中表生成上下文片段（行数估算 + CREATE TABLE SQL + 列清单）。
 * 返回写入的字符数（不含 NUL）。供 M3 Composer 直接喂入。 */
size_t np_plan_context_snippet(const np_plan_t *p, int table_idx,
                               char *buf, size_t cap);

/* 缺省能力注册表（懒创建，注册 X7 的 4 个 editor.text.* 工具）。
 * 主程序没有现成注册表时用它，保持接线最小。进程级单例。 */
np_registry_t *np_plan_default_registry(void);

#ifdef __cplusplus
}
#endif
#endif /* NP_PLANCARD_H */
