/* scripts/test_x5_gapdetect.c — X5 能力缺口检测与自动补齐测试.
 * 信任评分/授权判定/缺口匹配/授权卡/注入安装重试/审计/渲染/失败路径。
 */
#include "np_gapdetect.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)
#define CHECK_STR(a, e) do { \
    if (strcmp((a), (e)) == 0) { g_pass++; } \
    else { printf("FAIL %s:%d  got '%s' expect '%s'\n", __FILE__, __LINE__, (a), (e)); g_fail++; } \
} while (0)

typedef struct {
    int installs, retries;
    bool install_fail, retry_fail;
    char last_install[NP_GD_MAX_IDENT];
} cb_ctx_t;

static bool install_ok(const np_gd_candidate_t *c,
                       const np_gd_authcard_t *card, void *user)
{
    cb_ctx_t *x = (cb_ctx_t *)user;
    (void)card;
    x->installs++;
    snprintf(x->last_install, sizeof(x->last_install), "%s", c->id);
    return true;
}

static bool install_fail(const np_gd_candidate_t *c,
                         const np_gd_authcard_t *card, void *user)
{
    cb_ctx_t *x = (cb_ctx_t *)user;
    (void)c; (void)card;
    x->installs++;
    return false;
}

static bool retry_ok(const char *task, void *user)
{
    cb_ctx_t *x = (cb_ctx_t *)user;
    (void)task;
    x->retries++;
    return true;
}

static bool retry_fail(const char *task, void *user)
{
    cb_ctx_t *x = (cb_ctx_t *)user;
    (void)task;
    x->retries++;
    return false;
}

static np_gd_signal_t sig_disasm(void)
{
    np_gd_signal_t s;
    memset(&s, 0, sizeof(s));
    snprintf(s.task, sizeof(s.task), "需要反汇编二进制找函数边界");
    snprintf(s.failure, sizeof(s.failure), "缺少 disasm 工具");
    s.need_scopes = NP_SCOPE_BINARY;
    s.need_risk = NP_RISK_READONLY;
    return s;
}

static np_gd_signal_t sig_network(void)
{
    np_gd_signal_t s;
    memset(&s, 0, sizeof(s));
    snprintf(s.task, sizeof(s.task), "network query 搜索");
    snprintf(s.failure, sizeof(s.failure), "缺 network 工具");
    s.need_scopes = NP_SCOPE_NETWORK;
    s.need_risk = NP_RISK_NETWORK;
    return s;
}

static np_gd_candidate_t cand_disasm(void)
{
    np_gd_candidate_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.id, sizeof(c.id), "disasm-skill");
    snprintf(c.name, sizeof(c.name), "Disasm Skill");
    snprintf(c.kind, sizeof(c.kind), "skill");
    snprintf(c.description, sizeof(c.description), "disasm 反汇编 binary 函数");
    c.scopes = NP_SCOPE_BINARY;
    c.risk = NP_RISK_READONLY;
    snprintf(c.source, sizeof(c.source), "hub://skill");
    snprintf(c.sha256, sizeof(c.sha256), "abc123");
    snprintf(c.license, sizeof(c.license), "MIT");
    c.health = true;
    c.user_rating = 20;
    return c;
}

static np_gd_candidate_t cand_network(void)
{
    np_gd_candidate_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.id, sizeof(c.id), "net-mcp");
    snprintf(c.name, sizeof(c.name), "Net MCP");
    snprintf(c.kind, sizeof(c.kind), "mcp");
    snprintf(c.description, sizeof(c.description), "network query tool 联网搜索");
    c.scopes = NP_SCOPE_NETWORK;
    c.risk = NP_RISK_NETWORK;
    snprintf(c.source, sizeof(c.source), "hub://mcp");
    snprintf(c.sha256, sizeof(c.sha256), "def456");
    snprintf(c.license, sizeof(c.license), "Apache-2.0");
    c.health = true;
    return c;
}

static np_gd_policy_t policy_ro(void)
{
    np_gd_policy_t p;
    memset(&p, 0, sizeof(p));
    p.allow_scopes = NP_SCOPE_BINARY;
    p.max_risk = NP_RISK_READONLY;
    p.auto_install = true;
    return p;
}

static void test_trust(void)
{
    np_gd_candidate_t c = cand_disasm();
    CHECK(np_gd_trust_score(&c) == 100);   /* 30+15+25+20+10 */
    c.sha256[0] = '\0';
    CHECK(np_gd_trust_score(&c) == 70);
    c.health = false;
    CHECK(np_gd_trust_score(&c) == 45);
    c.user_rating = -50;
    CHECK(np_gd_trust_score(&c) == 10);
    c.license[0] = '\0'; c.source[0] = '\0';
    CHECK(np_gd_trust_score(&c) == 0);
    CHECK(np_gd_trust_score(NULL) == 0);
}

static void test_authorized(void)
{
    np_gd_candidate_t c = cand_disasm();
    np_gd_policy_t p = policy_ro();
    CHECK(np_gd_authorized(&c, &p));
    p.auto_install = false;
    CHECK(!np_gd_authorized(&c, &p));      /* 总开关关 */
    p = policy_ro();
    p.allow_scopes = NP_SCOPE_NONE;
    CHECK(!np_gd_authorized(&c, &p));      /* scope 不匹配 */
    p = policy_ro();
    p.max_risk = NP_RISK_READONLY;
    {
        np_gd_candidate_t d = cand_network();
        CHECK(!np_gd_authorized(&d, &p));  /* 风险超上限 */
    }
    CHECK(!np_gd_authorized(NULL, &p));
}

static void test_detect_match(void)
{
    np_gd_signal_t s = sig_disasm();
    np_gd_candidate_t cs[2];
    np_gd_policy_t p = policy_ro();
    np_gd_plan_t plan;
    np_gd_log_t log;
    np_gd_status_t st;
    cs[0] = cand_disasm();
    cs[1] = cand_network();
    memset(&log, 0, sizeof(log));
    st = np_gd_detect(&s, cs, 2, &p, &plan, &log);
    CHECK(st == NP_GD_OK);
    CHECK_STR(plan.chosen.id, "disasm-skill");
    CHECK(plan.authorized);
    CHECK(plan.trust == 100);
    CHECK(!plan.has_card);
    CHECK(log.n >= 2);
    /* 低信任候选被排除 */
    cs[0].sha256[0] = '\0'; cs[0].health = false; cs[0].user_rating = 0;
    st = np_gd_detect(&s, cs, 2, &p, &plan, &log);
    CHECK(st == NP_GD_ERR_LOW_TRUST || st == NP_GD_ERR_NO_CANDIDATE);
}

static void test_authcard(void)
{
    np_gd_signal_t s = sig_network();
    np_gd_candidate_t cs[2];
    np_gd_policy_t p = policy_ro();
    np_gd_plan_t plan;
    np_gd_log_t log;
    cb_ctx_t cx = {0, 0, false, false, {0}};
    np_gd_status_t st;
    cs[0] = cand_disasm();
    cs[1] = cand_network();
    memset(&log, 0, sizeof(log));
    st = np_gd_detect(&s, cs, 2, &p, &plan, &log);
    CHECK(st == NP_GD_ERR_UNAUTHORIZED);
    CHECK(plan.has_card);
    CHECK(!plan.authorized);
    CHECK(plan.card.token[0]);
    CHECK_STR(plan.card.candidate_id, "net-mcp");
    CHECK(plan.card.ask_scopes == NP_SCOPE_NETWORK);
    /* 未授权时 fill 拒绝，不安装不重试 */
    CHECK(np_gd_fill(&plan, install_ok, retry_ok, &cx, s.task, &log)
          == NP_GD_ERR_UNAUTHORIZED);
    CHECK(cx.installs == 0 && cx.retries == 0);
    /* 错误 token 拒绝 */
    CHECK(np_gd_consume_card(&plan, "wrong-token", &log) == NP_GD_ERR_ARG);
    CHECK(!plan.authorized);
    /* 正确 token 消费 → 一次性授权 */
    CHECK(np_gd_consume_card(&plan, plan.card.token, &log) == NP_GD_OK);
    CHECK(plan.authorized && plan.card.consumed);
    /* 再次消费拒绝（一次性） */
    CHECK(np_gd_consume_card(&plan, plan.card.token, &log) == NP_GD_ERR_ARG);
    /* 授权后 fill 执行正确候选 */
    CHECK(np_gd_fill(&plan, install_ok, retry_ok, &cx, s.task, &log) == NP_GD_OK);
    CHECK(cx.installs == 1 && cx.retries == 1);
    CHECK_STR(cx.last_install, "net-mcp");
}

static void test_fill_failures(void)
{
    np_gd_signal_t s = sig_disasm();
    np_gd_candidate_t c = cand_disasm();
    np_gd_policy_t p = policy_ro();
    np_gd_plan_t plan;
    np_gd_log_t log;
    cb_ctx_t cx = {0, 0, false, false, {0}};
    CHECK(np_gd_detect(&s, &c, 1, &p, &plan, &log) == NP_GD_OK);
    /* 安装失败 */
    CHECK(np_gd_fill(&plan, install_fail, retry_ok, &cx, s.task, &log)
          == NP_GD_ERR_INSTALL_FAILED);
    CHECK(cx.installs == 1 && cx.retries == 0);  /* 重试未执行 */
    /* 重试失败 */
    cx.installs = 0; cx.retries = 0;
    CHECK(np_gd_fill(&plan, install_ok, retry_fail, &cx, s.task, &log)
          == NP_GD_ERR_RETRY_FAILED);
    CHECK(cx.installs == 1 && cx.retries == 1);
    /* 无回调：只产出计划，仍 OK */
    memset(&cx, 0, sizeof(cx));
    CHECK(np_gd_fill(&plan, NULL, NULL, &cx, s.task, &log) == NP_GD_OK);
    CHECK(cx.installs == 0 && cx.retries == 0);
}

static void test_render(void)
{
    np_gd_signal_t s = sig_network();
    np_gd_candidate_t cs[2];
    np_gd_policy_t p = policy_ro();
    np_gd_plan_t plan;
    np_gd_log_t log;
    char md[NP_GD_MAX_OUT];
    cs[0] = cand_disasm(); cs[1] = cand_network();
    memset(&log, 0, sizeof(log));
    np_gd_detect(&s, cs, 2, &p, &plan, &log);
    CHECK(np_gd_render(&plan, &log, md, sizeof(md)) > 0);
    CHECK(strstr(md, "# 能力缺口补齐计划") != NULL);
    CHECK(strstr(md, "一次性授权卡") != NULL);
    CHECK(strstr(md, "未授权") != NULL);
    CHECK(strstr(md, "审计轨迹") != NULL);
    CHECK(strstr(md, "绝不静默安装") != NULL);
    CHECK(np_gd_render(&plan, &log, md, 32) == -1);
}

static void test_failure(void)
{
    np_gd_signal_t s = sig_disasm();
    np_gd_candidate_t c = cand_disasm();
    np_gd_policy_t p = policy_ro();
    np_gd_plan_t plan;
    np_gd_log_t log;
    memset(&log, 0, sizeof(log));
    CHECK(np_gd_detect(NULL, &c, 1, &p, &plan, &log) == NP_GD_ERR_ARG);
    CHECK(np_gd_detect(&s, NULL, 0, &p, &plan, &log) == NP_GD_ERR_NO_CANDIDATE);
    CHECK(np_gd_detect(&s, &c, 1, NULL, &plan, &log) == NP_GD_ERR_ARG);
    CHECK(np_gd_fill(NULL, NULL, NULL, NULL, NULL, &log) == NP_GD_ERR_ARG);
    CHECK(np_gd_consume_card(NULL, "x", &log) == NP_GD_ERR_ARG);
    CHECK(np_gd_render(NULL, &log, (char *)plan.chosen.id, 64) == -1);
    CHECK_STR(np_gd_status_name(NP_GD_ERR_UNAUTHORIZED), "UNAUTHORIZED");
}

int main(void)
{
    printf("=== X5 gap detection tests ===\n");
    test_trust();
    test_authorized();
    test_detect_match();
    test_authcard();
    test_fill_failures();
    test_render();
    test_failure();
    printf("g_pass=%d g_fail=%d\n", g_pass, g_fail);
    if (g_fail) { printf("X5 TESTS FAILED\n"); return 1; }
    printf("X5 TESTS PASSED (%d assertions)\n", g_pass);
    return 0;
}
