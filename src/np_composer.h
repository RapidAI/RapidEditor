/* np_composer.h — M3/B1: Composer → Job 闭环（UI-free 模型层）.
 *
 * 文档要求（B1 · Composer 工作台）：
 *  - 侧栏是任务表单而非纯对话：提交意图 → np_job_create 生成 Job，
 *    A1 状态机可观察（created → queued → running → succeeded/failed）。
 *  - 附件芯片区分文本选区 / 字节选区 / 格式片段，复用 np_context 的三轨
 *    预算；发送前审计摘要可见（不偷偷把整份文档发出去）。
 *  - 每次调用带 Citation 芯片（file:line / file:0xOFFSET / file#路径），
 *    本地读回验证后才算可信（B5 防幻觉红线）。
 *  - AI 产物不直接落盘：生成 patch_set 交给 B3 Review 审查后写入。
 *
 * 本模块是纯 C99 模型层，无 CUI / 网络依赖，可独立单测；面板 UI 另接。
 * 意图解析为确定性本地规划器（不调用模型），用于把 Job→Patch→Review
 * 的闭环在 UI 接线前先跑通；真实模型输出后续可替换同一入口。
 */
#ifndef NP_COMPOSER_H
#define NP_COMPOSER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "np_contract.h"
#include "np_job.h"
#include "np_capability.h"
#include "np_patch.h"
#include "np_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NP_ATTACH_TEXT   = 0,   /* 文本选区  file:line      */
    NP_ATTACH_BYTE   = 1,   /* 字节选区  file:0xOFFSET  */
    NP_ATTACH_FORMAT = 2    /* 格式片段  file#struct    */
} np_attach_kind_t;

typedef struct {
    np_attach_kind_t kind;
    char  label[128];       /* 芯片标题 */
    char  citation[280];    /* B5 规范串 */
    char *text;             /* 拥有；字节轨为 hex 文本 */
    size_t len;
} np_composer_attach_t;

typedef enum {
    NP_COMPOSER_IDLE = 0,
    NP_COMPOSER_READY,      /* 已绑定文档 + 审计完成 */
    NP_COMPOSER_RUNNING,
    NP_COMPOSER_DONE,
    NP_COMPOSER_FAILED
} np_composer_state_t;

const char *np_composer_state_name(np_composer_state_t s);

typedef enum {
    NP_SUBMIT_OK = 0,
    NP_SUBMIT_ERR_ARG,
    NP_SUBMIT_ERR_NO_DOC,
    NP_SUBMIT_ERR_UNSUPPORTED,
    NP_SUBMIT_ERR_STATE,
    NP_SUBMIT_ERR_PATCH
} np_submit_result_t;

const char *np_submit_result_name(np_submit_result_t r);

#define NP_COMPOSER_MAX_ATTACH 8
#define NP_COMPOSER_MAX_TOOLS  8
#define NP_COMPOSER_MAX_CITES  12

typedef struct np_composer np_composer_t;

/* reg 传 NULL 时内部创建注册表并注册 X7 的 4 个 editor.text.* 工具。
 * budget 传 NULL 用 np_ctx_default_budget()。 */
np_composer_t *np_composer_create(np_registry_t *reg,
                                  const np_ctx_budget_t *budget);
void np_composer_destroy(np_composer_t *c);

/* 绑定当前文档（深拷贝）。会清空上一次 Job/Patch/上下文，但保留 reg 与预算。 */
void np_composer_bind(np_composer_t *c, const char *path, np_track_t track,
                      uint64_t revision, const unsigned char *data, size_t len);

/* 附件芯片。文本/字节为 1-based 行号与半开字节区间；格式片段传已生成的
 * 上下文串与 file#路径 Citation。越界返回 false。 */
bool np_composer_add_text(np_composer_t *c, size_t line_first, size_t line_last);
bool np_composer_add_byte(np_composer_t *c, size_t offset, size_t len);
bool np_composer_add_format(np_composer_t *c, const char *snippet,
                            const char *citation);

size_t np_composer_attach_count(const np_composer_t *c);
const np_composer_attach_t *np_composer_attach(const np_composer_t *c, size_t i);
void np_composer_remove_attach(np_composer_t *c, size_t i);

/* 发送前审计：按三轨预算打包并把摘要写入 out（可 NULL 仅刷新内部状态）。
 * 返回 false 表示无文档或分配失败。 */
bool np_composer_audit(np_composer_t *c, char *out, size_t cap);
const char *np_composer_context(const np_composer_t *c, size_t *len);

/* 提交意图：Job 状态机 + X2 工具路由 + 本地确定性规划 → patch_set。
 * 成功时 state=NP_COMPOSER_DONE，job 终态 succeeded；调用方把 patch_set
 * 送进 B3 Review，不要直接 apply。 */
np_submit_result_t np_composer_submit(np_composer_t *c, const char *intent);

/* 访问器（均可为 NULL 安全） */
np_job_t *np_composer_job(np_composer_t *c);
const np_patch_set_t *np_composer_patch(const np_composer_t *c);
np_composer_state_t np_composer_state(const np_composer_t *c);
size_t np_composer_routed_tools(const np_composer_t *c,
                                char ids[][80], size_t cap);
/* 全部芯片 Citation（附件 + 本次 patch 目标），写到 out[][280]。 */
size_t np_composer_citations(const np_composer_t *c,
                             char out[][280], size_t cap);
/* 逐条本地读回验证；失败原因写 why。格式轨 Citation 需 UI 提供 reader，
 * 模型层只验证语法与来源绑定。 */
bool np_composer_verify_citations(np_composer_t *c, char *why, size_t cap);
const char *np_composer_why(const np_composer_t *c);

/* 清空一次提交结果（保留文档与附件），回到 IDLE。 */
void np_composer_reset(np_composer_t *c);

#ifdef __cplusplus
}
#endif
#endif /* NP_COMPOSER_H */
