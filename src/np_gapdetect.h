/* np_gapdetect.h — X5: 能力缺口检测与自动补齐.
 *
 * 文档要求（X5 · S2 · P1）：AI 遇到不会的任务时只能道歉或让用户自己找扩展。
 * 失败/缺工具信号 → 匹配候选 Skill/MCP → 信任评分 → 在已授权策略范围内
 * 自动安装并重试；超权限仅生成一次性授权卡，不静默执行。
 *
 * 安全红线：
 *  - 只有候选所需 scope/risk <b>全部落在用户已授权策略内</b>才允许自动安装；
 *    超权限一律生成<b>一次性授权卡</b>（one-shot，用后失效），绝不静默执行；
 *  - 信任评分低于阈值、无哈希声明、许可证不明或健康检查未过的候选不推荐；
 *  - 真实安装通过<b>注入回调</b>完成（本模块不访问网络/不写磁盘/不硬链后端）；
 *  - 重试也通过注入回调，且只在安装成功后执行；
 *  - 全程审计记录（缺口/候选/评分/授权决策/安装结果/重试结果）。
 *
 * 诚实边界：候选来自调用方提供的目录（本模块不主动抓取），信任评分是
 * 基于元数据的启发式（非真实信誉系统），匹配是关键词/scope 重叠而非语义理解。
 */
#ifndef NP_GAPDETECT_H
#define NP_GAPDETECT_H

#include "np_contract.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_GD_MAX_CANDIDATES 32
#define NP_GD_MAX_IDENT     96
#define NP_GD_MAX_TEXT      200
#define NP_GD_MAX_REF       200
#define NP_GD_MAX_AUDIT     32
#define NP_GD_MAX_OUT       16384
#define NP_GD_MIN_TRUST     60       /* 低于此分不推荐 */

typedef enum {
    NP_GD_OK = 0,
    NP_GD_ERR_ARG,
    NP_GD_ERR_NO_CANDIDATE,
    NP_GD_ERR_LOW_TRUST,
    NP_GD_ERR_UNAUTHORIZED,   /* 超权限 → 应生成授权卡 */
    NP_GD_ERR_INSTALL_FAILED,
    NP_GD_ERR_RETRY_FAILED,
    NP_GD_ERR_FULL
} np_gd_status_t;
const char *np_gd_status_name(np_gd_status_t s);

/* 缺口信号 */
typedef struct {
    char   task[NP_GD_MAX_TEXT];      /* 任务描述（匹配关键词） */
    char   failure[NP_GD_MAX_TEXT];   /* 失败原因/缺工具信号 */
    unsigned need_scopes;             /* 需要的 NP_SCOPE_* 位掩码 */
    np_risk_t need_risk;              /* 需要的最高风险等级 */
} np_gd_signal_t;

/* 候选扩展（Skill/MCP 元数据，来自调用方目录） */
typedef struct {
    char     id[NP_GD_MAX_IDENT];     /* 需固定长度，见下 */
    char     name[NP_GD_MAX_TEXT];
    char     kind[24];                /* "skill" / "mcp" */
    char     description[NP_GD_MAX_TEXT];
    unsigned scopes;                  /* 提供的能力范围 */
    np_risk_t risk;
    char     source[NP_GD_MAX_REF];   /* 来源 URL/路径 */
    char     sha256[65];              /* 哈希（空=不信任） */
    char     license[NP_GD_MAX_TEXT]; /* 许可证（空/Unknown=降分） */
    bool     health;                  /* 健康检查是否通过 */
    int      user_rating;             /* -100..100，0=未知 */
} np_gd_candidate_t;

/* 用户授权策略 */
typedef struct {
    unsigned allow_scopes;            /* 允许的 scope 位掩码 */
    np_risk_t max_risk;               /* 允许的最高风险 */
    bool     auto_install;            /* 是否允许自动安装（总开关） */
} np_gd_policy_t;

/* 一次性授权卡（超权限时生成，不静默执行） */
typedef struct {
    char     token[32];               /* 一次性 token（调用方生成/注入） */
    char     candidate_id[NP_GD_MAX_IDENT];
    char     reason[NP_GD_MAX_TEXT];  /* 为何需要额外授权 */
    unsigned ask_scopes;
    np_risk_t ask_risk;
    bool     consumed;                /* 用后失效 */
} np_gd_authcard_t;

/* 补齐计划（可审阅） */
typedef struct {
    np_gd_candidate_t chosen;
    int      trust;
    bool     authorized;
    np_gd_authcard_t card;            /* 仅超权限时有效 */
    bool     has_card;
    char     rationale[NP_GD_MAX_TEXT];
} np_gd_plan_t;

/* 审计条目 */
typedef enum {
    NP_GD_A_DETECT = 0,
    NP_GD_A_MATCH,
    NP_GD_A_AUTH_DENIED,
    NP_GD_A_AUTHCARD,
    NP_GD_A_INSTALL,
    NP_GD_A_RETRY
} np_gd_audit_kind_t;

typedef struct {
    np_gd_audit_kind_t kind;
    char detail[NP_GD_MAX_TEXT];
    bool success;
} np_gd_audit_t;

typedef struct {
    int    n;
    np_gd_audit_t items[NP_GD_MAX_AUDIT];
} np_gd_log_t;

/* 安装回调：安装候选（真实动作由调用方实现；可注入凭据/网络/文件）。
 * 成功返回 true；authcard 非 NULL 时表示携带一次性授权卡。 */
typedef bool (*np_gd_install_fn)(const np_gd_candidate_t *c,
                                 const np_gd_authcard_t *authcard, void *user);
/* 重试回调：安装成功后重试原任务。成功返回 true。 */
typedef bool (*np_gd_retry_fn)(const char *task, void *user);

/* 检测缺口并生成补齐计划（纯逻辑，不安装、不重试）。
 * 返回 OK 表示找到可授权候选；UNAUTHORIZED 表示生成授权卡（has_card=true）；
 * NO_CANDIDATE / LOW_TRUST 表示无法补齐。 */
np_gd_status_t np_gd_detect(const np_gd_signal_t *sig,
                            const np_gd_candidate_t *cands, size_t ncands,
                            const np_gd_policy_t *policy,
                            np_gd_plan_t *out, np_gd_log_t *log);

/* 执行补齐：按计划安装（若授权）并重试；未授权时拒绝（需先用授权卡）。
 * install/retry 可为 NULL（此时只产出计划/授权卡，不执行）。 */
np_gd_status_t np_gd_fill(const np_gd_plan_t *plan,
                          np_gd_install_fn install, np_gd_retry_fn retry,
                          void *user, const char *task,
                          np_gd_log_t *log);

/* 消费一次性授权卡：把 card 标记为 consumed 并重算计划授权状态。 */
np_gd_status_t np_gd_consume_card(np_gd_plan_t *plan,
                                  const char *token, np_gd_log_t *log);

/* 审计记录追加（供注入的 install/retry 回调记录自定义事件）。 */
void np_gd_audit(np_gd_log_t *log, np_gd_audit_kind_t kind,
                 const char *detail, bool success);

/* 计算候选信任分（可单测）。 */
int np_gd_trust_score(const np_gd_candidate_t *c);

/* 授权判定（可单测）：scope 与 risk 全部在策略内才 true。 */
bool np_gd_authorized(const np_gd_candidate_t *c, const np_gd_policy_t *p);

/* 渲染计划/审计为 Markdown（含授权卡与诚实边界）。 */
int np_gd_render(const np_gd_plan_t *plan, const np_gd_log_t *log,
                 char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_GAPDETECT_H */
