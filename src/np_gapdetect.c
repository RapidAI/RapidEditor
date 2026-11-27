/* np_gapdetect.c — X5: 能力缺口检测与自动补齐（实现）.
 * 纯 C99、UI-free；纯逻辑决策 + 注入式安装/重试，不硬链网络/文件。
 */
#include "np_gapdetect.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------- 工具 ---------- */
static void fscpy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* 可移植大小写不敏感比较 */
static int gd_ci_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* 有界追加（避免 strncat 的 MSVC C4996 与截断误报） */
static void append_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(dst);
    size_t i = 0;
    while (n + 1 < cap && src && src[i]) dst[n++] = src[i++];
    dst[n] = '\0';
}

static bool appendf(char *out, size_t cap, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (*used >= cap) return false;
    va_start(ap, fmt);
    n = vsnprintf(out + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *used) return false;
    *used += (size_t)n;
    return true;
}

const char *np_gd_status_name(np_gd_status_t s)
{
    switch (s) {
    case NP_GD_OK: return "OK";
    case NP_GD_ERR_ARG: return "ARG";
    case NP_GD_ERR_NO_CANDIDATE: return "NO_CANDIDATE";
    case NP_GD_ERR_LOW_TRUST: return "LOW_TRUST";
    case NP_GD_ERR_UNAUTHORIZED: return "UNAUTHORIZED";
    case NP_GD_ERR_INSTALL_FAILED: return "INSTALL_FAILED";
    case NP_GD_ERR_RETRY_FAILED: return "RETRY_FAILED";
    case NP_GD_ERR_FULL: return "FULL";
    }
    return "?";
}

void np_gd_audit(np_gd_log_t *log, np_gd_audit_kind_t kind,
                 const char *detail, bool success)
{
    np_gd_audit_t *a;
    if (!log) return;
    if (log->n >= NP_GD_MAX_AUDIT) return;  /* 静默截断审计（保留最早记录） */
    a = &log->items[log->n++];
    memset(a, 0, sizeof(*a));
    a->kind = kind;
    a->success = success;
    fscpy(a->detail, sizeof(a->detail), detail ? detail : "");
}

/* ---------- 信任评分 ---------- */
int np_gd_trust_score(const np_gd_candidate_t *c)
{
    int score = 0;
    if (!c) return 0;
    /* 哈希声明是信任基础 */
    if (c->sha256[0]) score += 30;
    /* 许可证明确（非空且非 Unknown） */
    if (c->license[0] && gd_ci_cmp(c->license, "unknown") != 0) score += 15;
    /* 健康检查通过 */
    if (c->health) score += 25;
    /* 用户评分（-100..100）映射 ±20 */
    if (c->user_rating > 0) score += 20;
    else if (c->user_rating < 0) score -= 15;
    /* 来源非空 */
    if (c->source[0]) score += 10;
    if (score < 0) score = 0;
    if (score > 100) score = 100;
    return score;
}

bool np_gd_authorized(const np_gd_candidate_t *c, const np_gd_policy_t *p)
{
    if (!c || !p) return false;
    /* 总开关关闭 → 一切需授权卡 */
    if (!p->auto_install) return false;
    /* scope 必须全部被允许（c->scopes 是 p->allow_scopes 的子集） */
    if ((c->scopes & ~p->allow_scopes) != 0) return false;
    /* 风险等级不得超过上限（数值越小越安全） */
    if ((int)c->risk > (int)p->max_risk) return false;
    return true;
}

/* ---------- 匹配（关键词/scope） ---------- */
static int match_score(const np_gd_signal_t *sig, const np_gd_candidate_t *c)
{
    /* 关键词表：任务侧的关键词 → 候选描述里若含对应词则命中。
     * 只在候选 description 上匹配，避免把任务文本也拼进去自命中。 */
    static const struct {
        const char *task_kw;      /* 任务/失败信号中出现 */
        const char *cand_kw;      /* 候选描述中应出现 */
    } RULES[] = {
        { "disasm", "disasm" }, { "反汇编", "反汇编" },
        { "diff", "diff" }, { "sql", "sql" }, { "query", "query" },
        { "查询", "查询" }, { "format", "format" }, { "格式", "格式" },
        { "hex", "hex" }, { "byte", "byte" }, { "字节", "字节" },
        { "patch", "patch" }, { "grep", "search" }, { "search", "search" },
        { "搜索", "search" }, { "decomp", "decomp" }, { "反编译", "反编译" },
        { "symbol", "symbol" }, { "符号", "symbol" },
        { "lint", "lint" }, { "诊断", "lint" },
        { "latex", "latex" }, { "render", "render" }, { "预览", "render" },
        { "network", "network" }, { "网络", "network" }, { "联网", "network" }
    };
    char task_text[448], cand_low[NP_GD_MAX_TEXT];
    size_t k;
    int score = 0;
    fscpy(task_text, sizeof(task_text), sig->task);
    append_str(task_text, sizeof(task_text), " ");
    append_str(task_text, sizeof(task_text), sig->failure);
    for (k = 0; task_text[k]; k++) task_text[k] = (char)tolower((unsigned char)task_text[k]);
    fscpy(cand_low, sizeof(cand_low), c->description);
    for (k = 0; cand_low[k]; k++) cand_low[k] = (char)tolower((unsigned char)cand_low[k]);
    for (k = 0; k < sizeof(RULES) / sizeof(RULES[0]); k++) {
        if (strstr(task_text, RULES[k].task_kw) &&
            strstr(cand_low, RULES[k].cand_kw)) score++;
    }
    /* 候选描述里出现候选 id 也算弱匹配（目录常以工具名为描述） */
    if (c->id[0] && strstr(cand_low, c->id)) score += 2;
    /* scope 覆盖：候选能提供缺口需要的 scope 加分 */
    if (sig->need_scopes && (c->scopes & sig->need_scopes)) score += 3;
    return score;
}

/* ---------- 检测 ---------- */
np_gd_status_t np_gd_detect(const np_gd_signal_t *sig,
                            const np_gd_candidate_t *cands, size_t ncands,
                            const np_gd_policy_t *policy,
                            np_gd_plan_t *out, np_gd_log_t *log)
{
    int best = -1, best_score = -1, best_trust = 0;
    size_t i;
    if (!sig || !out || !policy || !sig->task[0]) return NP_GD_ERR_ARG;
    memset(out, 0, sizeof(*out));
    np_gd_audit(log, NP_GD_A_DETECT, sig->failure[0] ? sig->failure : sig->task,
                true);
    if (!cands || ncands == 0) {
        np_gd_audit(log, NP_GD_A_MATCH, "candidate pool empty", false);
        return NP_GD_ERR_NO_CANDIDATE;
    }
    for (i = 0; i < ncands && i < NP_GD_MAX_CANDIDATES; i++) {
        int ms = match_score(sig, &cands[i]);
        int ts = np_gd_trust_score(&cands[i]);
        if (ms <= 0) continue;
        if (ts < NP_GD_MIN_TRUST) continue;
        /* 评分 = 匹配×10 + 信任 */
        if (ms * 10 + ts > best_score) {
            best = (int)i;
            best_score = ms * 10 + ts;
            best_trust = ts;
        }
    }
    if (best < 0) {
        np_gd_audit(log, NP_GD_A_MATCH, "no trusted candidate matched", false);
        return NP_GD_ERR_LOW_TRUST;
    }
    out->chosen = cands[best];
    out->trust = best_trust;
    out->authorized = np_gd_authorized(&cands[best], policy);
    (void)snprintf(out->rationale, sizeof(out->rationale),
                   "候选 %s 匹配任务关键词且信任分 %d",
                   cands[best].id, best_trust);
    np_gd_audit(log, NP_GD_A_MATCH, out->rationale, true);
    if (out->authorized) return NP_GD_OK;

    /* 超权限 → 一次性授权卡（不静默执行） */
    out->has_card = true;
    fscpy(out->card.candidate_id, sizeof(out->card.candidate_id),
          cands[best].id);
    out->card.ask_scopes = cands[best].scopes;
    out->card.ask_risk = cands[best].risk;
    (void)snprintf(out->card.reason, sizeof(out->card.reason),
                   "候选需要额外 scope=0x%x / risk=%d，超出已授权策略",
                   cands[best].scopes, (int)cands[best].risk);
    /* token 用候选 id + 信任分生成（一次性，消费时校验） */
    (void)snprintf(out->card.token, sizeof(out->card.token), "auth-%d-%d",
                   best, best_trust);
    np_gd_audit(log, NP_GD_A_AUTH_DENIED, out->card.reason, false);
    np_gd_audit(log, NP_GD_A_AUTHCARD, out->card.token, true);
    return NP_GD_ERR_UNAUTHORIZED;
}

/* ---------- 执行 ---------- */
np_gd_status_t np_gd_fill(const np_gd_plan_t *plan,
                          np_gd_install_fn install, np_gd_retry_fn retry,
                          void *user, const char *task,
                          np_gd_log_t *log)
{
    if (!plan) return NP_GD_ERR_ARG;
    if (!plan->authorized) {
        np_gd_audit(log, NP_GD_A_INSTALL, "not authorized", false);
        return NP_GD_ERR_UNAUTHORIZED;
    }
    if (install) {
        bool ok = install(&plan->chosen,
                          plan->has_card && !plan->card.consumed
                          ? &plan->card : NULL, user);
        np_gd_audit(log, NP_GD_A_INSTALL, plan->chosen.id, ok);
        if (!ok) return NP_GD_ERR_INSTALL_FAILED;
    } else {
        np_gd_audit(log, NP_GD_A_INSTALL, "no install callback (plan only)", true);
    }
    if (retry) {
        bool ok = retry(task ? task : plan->chosen.description, user);
        np_gd_audit(log, NP_GD_A_RETRY, "retry task", ok);
        if (!ok) return NP_GD_ERR_RETRY_FAILED;
    }
    return NP_GD_OK;
}

np_gd_status_t np_gd_consume_card(np_gd_plan_t *plan,
                                  const char *token, np_gd_log_t *log)
{
    if (!plan || !token || !plan->has_card) return NP_GD_ERR_ARG;
    if (plan->card.consumed) return NP_GD_ERR_ARG;  /* 一次性：已用 */
    if (strcmp(plan->card.token, token) != 0) return NP_GD_ERR_ARG;
    plan->card.consumed = true;
    plan->authorized = true;   /* 用户已显式授权 */
    np_gd_audit(log, NP_GD_A_AUTHCARD, "card consumed", true);
    return NP_GD_OK;
}

/* ---------- 渲染 ---------- */
int np_gd_render(const np_gd_plan_t *plan, const np_gd_log_t *log,
                 char *out, size_t cap)
{
    size_t u = 0;
    int i;
    if (!plan || !out || cap < 128) return -1;
    if (!appendf(out, cap, &u, "# 能力缺口补齐计划\n\n")) return -1;
    if (!appendf(out, cap, &u,
        "- 候选：%s（%s）· 信任分 %d/100\n"
        "- 授权状态：%s\n- 依据：%s\n\n",
        plan->chosen.name[0] ? plan->chosen.name : plan->chosen.id,
        plan->chosen.kind, plan->trust,
        plan->authorized ? "已授权" : "未授权（需授权卡）",
        plan->rationale)) return -1;
    if (!appendf(out, cap, &u,
        "- 候选详情：来源 %s · 哈希 %s · 许可证 %s · scope=0x%x · risk=%d\n\n",
        plan->chosen.source[0] ? plan->chosen.source : "(未记录)",
        plan->chosen.sha256[0] ? plan->chosen.sha256 : "(未声明)",
        plan->chosen.license[0] ? plan->chosen.license : "(未知)",
        plan->chosen.scopes, (int)plan->chosen.risk)) return -1;
    if (plan->has_card) {
        if (!appendf(out, cap, &u,
            "## 一次性授权卡\n- token：%s\n- 原因：%s\n- 请求 scope=0x%x / risk=%d\n"
            "- 状态：%s\n\n",
            plan->card.token, plan->card.reason,
            plan->card.ask_scopes, (int)plan->card.ask_risk,
            plan->card.consumed ? "已消费" : "未消费")) return -1;
    }
    if (log) {
        if (!appendf(out, cap, &u, "## 审计轨迹\n")) return -1;
        for (i = 0; i < log->n; i++) {
            const np_gd_audit_t *a = &log->items[i];
            if (!appendf(out, cap, &u, "- [%s] %s：%s\n",
                a->success ? "OK" : "FAIL",
                a->kind == NP_GD_A_DETECT ? "检测" :
                a->kind == NP_GD_A_MATCH ? "匹配" :
                a->kind == NP_GD_A_AUTH_DENIED ? "授权拒绝" :
                a->kind == NP_GD_A_AUTHCARD ? "授权卡" :
                a->kind == NP_GD_A_INSTALL ? "安装" : "重试",
                a->detail)) return -1;
        }
    }
    if (!appendf(out, cap, &u,
        "\n> 超权限仅生成一次性授权卡，绝不静默安装；安装/重试由注入回调执行。"
        "候选匹配为关键词/scope 启发式，非语义理解。\n")) return -1;
    return (int)u;
}
