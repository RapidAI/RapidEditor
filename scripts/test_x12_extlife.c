/* scripts/test_x12_extlife.c — X12 扩展生命周期审计、回滚与清理测试.
 * 账本记录/去重/审计/事务卸载/失败回滚/手动回滚/持久化/渲染/失败路径。
 */
#include "np_extlife.h"
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

static void clock_fn(char *buf, size_t cap, void *user)
{
    (void)user;
    snprintf(buf, cap, "2026-11-27T12:00:00");
}

typedef struct {
    int cred, cache, tool;
    bool fail_on_cache;
} clear_ctx_t;

static bool clear_fn(np_xl_clear_kind_t kind, const char *value, void *user)
{
    clear_ctx_t *c = (clear_ctx_t *)user;
    (void)value;
    if (kind == NP_XL_CLEAR_CREDENTIAL) { c->cred++; return true; }
    if (kind == NP_XL_CLEAR_CACHE) {
        c->cache++;
        return !c->fail_on_cache;
    }
    if (kind == NP_XL_CLEAR_TOOLREG) { c->tool++; return true; }
    return false;
}

static bool restore_fn(np_xl_clear_kind_t kind, const char *value, void *user)
{
    clear_ctx_t *c = (clear_ctx_t *)user;
    (void)value;
    if (kind == NP_XL_CLEAR_CREDENTIAL) c->cred--;
    else if (kind == NP_XL_CLEAR_CACHE) c->cache--;
    else if (kind == NP_XL_CLEAR_TOOLREG) c->tool--;
    return true;
}

static np_xl_entry_t make_detail(const char *id)
{
    np_xl_entry_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.id, sizeof(d.id), "%s", id);
    snprintf(d.source, sizeof(d.source), "https://example.com/%s", id);
    snprintf(d.sha256, sizeof(d.sha256), "0123456789abcdef");
    snprintf(d.license, sizeof(d.license), "MIT");
    snprintf(d.auth_ref, sizeof(d.auth_ref), "keychain:%s", id);
    snprintf(d.cache_path, sizeof(d.cache_path), "cache/%s", id);
    snprintf(d.tool_ref, sizeof(d.tool_ref), "mcp:%s", id);
    snprintf(d.call_summary, sizeof(d.call_summary), "demo extension");
    snprintf(d.perms[0], sizeof(d.perms[0]), "read");
    d.nperms = 1;
    return d;
}

static void test_record(void)
{
    np_xl_ledger_t l;
    np_xl_entry_t d = make_detail("skill-a");
    const np_xl_entry_t *e;
    np_xl_status_t st;
    np_xl_init(&l, "unit-test");
    CHECK(l.n == 0);
    CHECK_STR(l.source, "unit-test");

    st = np_xl_record(&l, NP_XL_INSTALL, 100, &d, clock_fn, NULL);
    CHECK(st == NP_XL_OK);
    CHECK(l.n == 1);
    e = np_xl_latest(&l, "skill-a");
    CHECK(e != NULL);
    CHECK(e->op == NP_XL_INSTALL);
    CHECK(e->state == NP_XL_ACTIVE);
    CHECK(e->job_id == 100);
    CHECK(!e->tombstone);
    CHECK_STR(e->timestamp, "2026-11-27T12:00:00");
    CHECK_STR(e->source, "https://example.com/skill-a");
    CHECK_STR(e->auth_ref, "keychain:skill-a");
    CHECK_STR(e->perms[0], "read");
    CHECK(e->nperms == 1);
    /* 重复 id+op+job_id 拒绝 */
    CHECK(np_xl_record(&l, NP_XL_INSTALL, 100, &d, clock_fn, NULL)
          == NP_XL_ERR_DUP);
    CHECK(l.n == 1);
    /* 更新是不同 op → 允许 */
    CHECK(np_xl_record(&l, NP_XL_UPDATE, 101, &d, clock_fn, NULL) == NP_XL_OK);
    CHECK(l.n == 2);
    CHECK(np_xl_latest(&l, "skill-a")->op == NP_XL_UPDATE);
    /* 未知 id */
    CHECK(np_xl_latest(&l, "missing") == NULL);
    /* 坏参数 */
    CHECK(np_xl_record(&l, NP_XL_INSTALL, 1, NULL, NULL, NULL) == NP_XL_ERR_ARG);
    {
        np_xl_entry_t bad = d;
        bad.id[0] = '\0';
        CHECK(np_xl_record(&l, NP_XL_INSTALL, 1, &bad, NULL, NULL)
              == NP_XL_ERR_ARG);
    }
}

static void test_uninstall_success(void)
{
    np_xl_ledger_t l;
    np_xl_entry_t d = make_detail("skill-b");
    clear_ctx_t cx = {0, 0, 0, false};
    bool rolled = true;
    const np_xl_entry_t *e;
    np_xl_status_t st;
    np_xl_init(&l, "t");
    np_xl_record(&l, NP_XL_INSTALL, 200, &d, clock_fn, NULL);
    st = np_xl_uninstall(&l, "skill-b", clear_fn, restore_fn, &cx, &rolled);
    CHECK(st == NP_XL_OK);
    CHECK(!rolled);
    CHECK(cx.cred == 1 && cx.cache == 1 && cx.tool == 1); /* 三类都清理 */
    e = np_xl_latest(&l, "skill-b");
    CHECK(e->state == NP_XL_REMOVED);
    CHECK(e->tombstone);   /* 仅留墓碑 */
    /* 重复卸载（无 active）→ NOT_FOUND */
    CHECK(np_xl_uninstall(&l, "skill-b", clear_fn, restore_fn, &cx, NULL)
          == NP_XL_ERR_NOT_FOUND);
}

static void test_uninstall_fail_rollback(void)
{
    np_xl_ledger_t l;
    np_xl_entry_t d = make_detail("skill-c");
    clear_ctx_t cx = {0, 0, 0, true};  /* cache 清理失败 */
    bool rolled = false;
    const np_xl_entry_t *e;
    np_xl_status_t st;
    np_xl_init(&l, "t");
    np_xl_record(&l, NP_XL_INSTALL, 300, &d, clock_fn, NULL);
    st = np_xl_uninstall(&l, "skill-c", clear_fn, restore_fn, &cx, &rolled);
    CHECK(st == NP_XL_ERR_TXN_FAILED);
    CHECK(rolled);
    /* cred 与 tool 已清后恢复 → 净 0；cache 失败不计入 done */
    CHECK(cx.cred == 0);
    CHECK(cx.cache == 1);    /* 失败那次仍被调用（计数），但未进 done */
    CHECK(cx.tool == 0);
    e = np_xl_latest(&l, "skill-c");
    CHECK(e->state == NP_XL_ROLLED_BACK);
    CHECK(!e->tombstone);
}

static void test_manual_rollback(void)
{
    np_xl_ledger_t l;
    np_xl_entry_t d = make_detail("skill-d");
    clear_ctx_t cx = {0, 0, 0, false};
    const np_xl_entry_t *e;
    np_xl_status_t st;
    np_xl_init(&l, "t");
    np_xl_record(&l, NP_XL_INSTALL, 400, &d, clock_fn, NULL);
    (void)np_xl_uninstall(&l, "skill-d", clear_fn, restore_fn, &cx, NULL);
    st = np_xl_rollback(&l, "skill-d", restore_fn, &cx);
    CHECK(st == NP_XL_OK);
    CHECK(cx.cred == 0 && cx.cache == 0 && cx.tool == 0); /* 三类恢复 */
    e = np_xl_latest(&l, "skill-d");
    CHECK(e->state == NP_XL_ACTIVE);
    CHECK(e->tombstone);  /* 审计墓碑保留可追溯 */
    /* 未注入 restore → ARG */
    CHECK(np_xl_rollback(&l, "skill-d", NULL, NULL) == NP_XL_ERR_ARG);
    CHECK(np_xl_rollback(&l, "missing", restore_fn, &cx) == NP_XL_ERR_NOT_FOUND);
}

static void test_no_injected_clear(void)
{
    np_xl_ledger_t l;
    np_xl_entry_t d = make_detail("skill-e");
    const np_xl_entry_t *e;
    np_xl_init(&l, "t");
    np_xl_record(&l, NP_XL_INSTALL, 500, &d, NULL, NULL);
    /* 未注入 clear：仍标记 REMOVED（无外部资源可清），不伪造回滚 */
    CHECK(np_xl_uninstall(&l, "skill-e", NULL, NULL, NULL, NULL) == NP_XL_OK);
    e = np_xl_latest(&l, "skill-e");
    CHECK(e->state == NP_XL_REMOVED && e->tombstone);
}

static void test_persist_render(void)
{
    np_xl_ledger_t l, l2;
    np_xl_entry_t d = make_detail("skill-f");
    char md[NP_XL_MAX_OUT];
    np_xl_status_t st;
    np_xl_init(&l, "persist");
    np_xl_record(&l, NP_XL_INSTALL, 600, &d, clock_fn, NULL);
    CHECK(np_xl_save(&l, "x12_test_ledger.ini") == NP_XL_OK);
    st = np_xl_load(&l2, "x12_test_ledger.ini");
    CHECK(st == NP_XL_OK);
    CHECK(l2.n == 1);
    CHECK_STR(l2.entries[0].id, "skill-f");
    CHECK_STR(l2.entries[0].auth_ref, "keychain:skill-f");
    CHECK_STR(l2.entries[0].perms[0], "read");
    CHECK(l2.entries[0].nperms == 1);
    CHECK(np_xl_render(&l2, md, sizeof(md)) > 0);
    CHECK(strstr(md, "# 扩展生命周期审计报告") != NULL);
    CHECK(strstr(md, "skill-f") != NULL);
    CHECK(strstr(md, "keychain:skill-f") != NULL);
    CHECK(strstr(md, "非明文") != NULL);
    CHECK(np_xl_render(&l2, md, 32) == -1);
    remove("x12_test_ledger.ini");
}

static void test_failure(void)
{
    np_xl_ledger_t l;
    char out[64];
    np_xl_init(&l, "t");
    CHECK(np_xl_uninstall(NULL, "x", NULL, NULL, NULL, NULL) == NP_XL_ERR_ARG);
    CHECK(np_xl_uninstall(&l, "missing", NULL, NULL, NULL, NULL)
          == NP_XL_ERR_NOT_FOUND);
    CHECK(np_xl_rollback(NULL, "x", NULL, NULL) == NP_XL_ERR_ARG);
    CHECK(np_xl_save(NULL, "x") == NP_XL_ERR_ARG);
    CHECK(np_xl_load(&l, "no-such-ledger.ini") == NP_XL_ERR_IO);
    CHECK(np_xl_render(NULL, out, sizeof(out)) == -1);
    CHECK_STR(np_xl_status_name(NP_XL_OK), "OK");
    CHECK_STR(np_xl_op_name(NP_XL_UPDATE), "更新");
    CHECK_STR(np_xl_state_name(NP_XL_ROLLED_BACK), "ROLLED_BACK");
}

int main(void)
{
    printf("=== X12 extension lifecycle tests ===\n");
    test_record();
    test_uninstall_success();
    test_uninstall_fail_rollback();
    test_manual_rollback();
    test_no_injected_clear();
    test_persist_render();
    test_failure();
    printf("g_pass=%d g_fail=%d\n", g_pass, g_fail);
    if (g_fail) { printf("X12 TESTS FAILED\n"); return 1; }
    printf("X12 TESTS PASSED (%d assertions)\n", g_pass);
    return 0;
}
