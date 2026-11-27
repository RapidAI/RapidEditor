/* scripts/test_f6_sqlquery.c — F6 结构化查询与采样分析测试.
 * 解析 / 安全门禁 / 采样 / where / 统计 / 渲染 / 需确认 / 失败路径。
 */
#include "np_sqlquery.h"
#include "np_sqlite.h"
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

static np_db_schema_t g_sch;
static unsigned char g_db[1 << 20];
static size_t g_len;

static void load_db(void)
{
    FILE *f = fopen("tests/golden/golden_full.db", "rb");
    if (!f) { printf("FAIL cannot open golden_full.db\n"); g_fail++; return; }
    g_len = fread(g_db, 1, sizeof(g_db), f);
    fclose(f);
    if (np_db_parse(g_db, g_len, &g_sch) != NP_DB_OK) {
        printf("FAIL np_db_parse\n"); g_fail++;
    }
}

static void test_parse_sql(void)
{
    np_sq_plan_t p;
    CHECK(np_sq_parse_sql("SELECT * FROM users LIMIT 50", &g_sch, &p) == NP_SQ_OK);
    CHECK_STR(p.table, "users");
    CHECK(p.nsel == 0);
    CHECK(p.limit == 50);
    CHECK_STR(p.sql, "SELECT * FROM users LIMIT 50");

    CHECK(np_sq_parse_sql("SELECT name, age FROM users WHERE age > 25 LIMIT 10",
                          &g_sch, &p) == NP_SQ_OK);
    CHECK(p.nsel == 2);
    CHECK_STR(p.cols[0], "name");
    CHECK_STR(p.cols[1], "age");
    CHECK(p.has_where);
    CHECK_STR(p.where_col, "age");
    CHECK(p.where_op == NP_SQ_GT);
    CHECK(p.where_lit_is_num);
    CHECK_STR(p.where_lit, "25");
    CHECK_STR(p.sql, "SELECT name, age FROM users WHERE age > 25 LIMIT 10");

    /* 字符串字面量 */
    CHECK(np_sq_parse_sql("SELECT name FROM users WHERE name = 'alice'",
                          &g_sch, &p) == NP_SQ_OK);
    CHECK(!p.where_lit_is_num);
    CHECK_STR(p.where_lit, "alice");
    CHECK(p.limit == 0);
    CHECK_STR(p.sql, "SELECT name FROM users WHERE name = 'alice'");

    /* 大小写不敏感 */
    CHECK(np_sq_parse_sql("select id from USERS limit 5", &g_sch, &p) == NP_SQ_OK);
    CHECK_STR(p.table, "USERS");
    /* 表名大小写不敏感由 np_db_find_table 决定：golden 是小写 users */
    (void)p;
}

static void test_security(void)
{
    np_sq_plan_t p;
    /* 写操作与注入全部拒绝 */
    CHECK(np_sq_parse_sql("DELETE FROM users", &g_sch, &p) == NP_SQ_ERR_DENIED);
    CHECK(np_sq_parse_sql("UPDATE users SET age=1", &g_sch, &p) == NP_SQ_ERR_DENIED);
    CHECK(np_sq_parse_sql("INSERT INTO users VALUES (1)", &g_sch, &p) == NP_SQ_ERR_DENIED);
    CHECK(np_sq_parse_sql("DROP TABLE users", &g_sch, &p) == NP_SQ_ERR_DENIED);
    CHECK(np_sq_parse_sql("SELECT * FROM users; DROP TABLE users", &g_sch, &p)
          == NP_SQ_ERR_DENIED);
    CHECK(np_sq_parse_sql("PRAGMA table_info(users)", &g_sch, &p) == NP_SQ_ERR_DENIED);
    /* 未声明的列/表拒绝 */
    CHECK(np_sq_parse_sql("SELECT nope FROM users", &g_sch, &p) == NP_SQ_ERR_DENIED);
    CHECK(np_sq_parse_sql("SELECT * FROM no_such_table", &g_sch, &p) == NP_SQ_ERR_DENIED);
    CHECK(np_sq_parse_sql("SELECT * FROM users WHERE nope = 1", &g_sch, &p)
          == NP_SQ_ERR_DENIED);
    /* 非 select 开头 */
    CHECK(np_sq_parse_sql("SHOW TABLES", &g_sch, &p) == NP_SQ_ERR_PARSE);
    /* 自然语言里含写词也拒绝 */
    CHECK(np_sq_parse_nl("请删除 users 表的记录", &g_sch, &p) == NP_SQ_ERR_DENIED);
    CHECK(np_sq_parse_nl("", &g_sch, &p) == NP_SQ_ERR_PARSE);
}

static void test_preview_and_confirm(void)
{
    np_sq_plan_t p;
    char prev[1024];
    np_sq_result_t r;
    CHECK(np_sq_parse_sql("SELECT * FROM users LIMIT 10", &g_sch, &p) == NP_SQ_OK);
    CHECK(np_sq_preview(&p, prev, sizeof(prev)) > 0);
    CHECK(strstr(prev, "需确认后执行") != NULL);
    CHECK(strstr(prev, "SELECT * FROM users LIMIT 10") != NULL);
    CHECK(np_sq_preview(&p, prev, 32) == -1);
    /* 未确认时执行拒绝 */
    CHECK(np_sq_run("tests/golden/golden_full.db", &p, 50, &r) ==
          NP_SQ_ERR_UNCONFIRMED);
    p.confirmed = true;
    CHECK(np_sq_run("tests/golden/golden_full.db", &p, 50, &r) == NP_SQ_OK);
}

static void test_sample_all(void)
{
    np_sq_plan_t p;
    np_sq_result_t r;
    CHECK(np_sq_parse_sql("SELECT * FROM users LIMIT 50", &g_sch, &p) == NP_SQ_OK);
    p.confirmed = true;
    CHECK(np_sq_run("tests/golden/golden_full.db", &p, 50, &r) == NP_SQ_OK);
    CHECK(r.ncols == 4);
    CHECK(r.rows_returned == 2);
    CHECK(r.rows_scanned == 2);
    CHECK(!r.sampled);
    CHECK(!r.partial);
    CHECK_STR(r.cols[0], "id");
    CHECK_STR(r.cols[1], "name");
    CHECK_STR(r.cols[2], "age");
    CHECK_STR(r.cols[3], "email");
    /* INTEGER PRIMARY KEY 别名列：rowid 回填（缺陷①回归） */
    CHECK_STR(r.rows[0][0], "1");
    CHECK_STR(r.rows[1][0], "2");
    CHECK_STR(r.rows[0][1], "alice");
    CHECK_STR(r.rows[1][1], "bob");
    CHECK_STR(r.rows[1][3], "NULL");          /* bob 的 email 是 NULL */
    /* 空值率：email 1/2 = 50% */
    CHECK(r.stats[3].nulls == 1);
    CHECK(r.stats[3].nonnull == 1);
    CHECK(r.stats[2].num_count == 2);          /* age 两行数值 */
    CHECK(r.stats[2].num_min == 25.0);
    CHECK(r.stats[2].num_max == 30.0);
}

static void test_where(void)
{
    np_sq_plan_t p;
    np_sq_result_t r;
    /* 数值比较 */
    CHECK(np_sq_parse_sql("SELECT name, age FROM users WHERE age > 25 LIMIT 50",
                          &g_sch, &p) == NP_SQ_OK);
    p.confirmed = true;
    CHECK(np_sq_run("tests/golden/golden_full.db", &p, 50, &r) == NP_SQ_OK);
    CHECK(r.rows_returned == 1);
    CHECK_STR(r.rows[0][0], "alice");
    CHECK_STR(r.rows[0][1], "30");
    CHECK(r.rows_scanned == 2);

    /* 字符串相等 */
    CHECK(np_sq_parse_sql("SELECT id FROM users WHERE name = 'bob'",
                          &g_sch, &p) == NP_SQ_OK);
    p.confirmed = true;
    CHECK(np_sq_run("tests/golden/golden_full.db", &p, 50, &r) == NP_SQ_OK);
    CHECK(r.rows_returned == 1);
    CHECK_STR(r.rows[0][0], "2");

    /* 浮点值（缺陷②回归） */
    CHECK(np_sq_parse_sql("SELECT id, amount FROM orders WHERE user_id = 1 LIMIT 50",
                          &g_sch, &p) == NP_SQ_OK);
    p.confirmed = true;
    CHECK(np_sq_run("tests/golden/golden_full.db", &p, 50, &r) == NP_SQ_OK);
    CHECK(r.rows_returned == 2);
    CHECK_STR(r.rows[0][0], "1");
    CHECK_STR(r.rows[0][1], "150.5");          /* float 正确解析 */
    CHECK_STR(r.rows[1][1], "200");
}

static void test_nl(void)
{
    np_sq_plan_t p;
    CHECK(np_sq_parse_nl("显示 users 表里 age 大于 25 的 name 和 age",
                         &g_sch, &p) == NP_SQ_OK);
    CHECK_STR(p.table, "users");
    CHECK(p.nsel == 2);
    CHECK(strstr(p.sql, "name") != NULL);
    CHECK(strstr(p.sql, "age") != NULL);
    CHECK(strstr(p.sql, "LIMIT") != NULL);
}

static void test_render_quality(void)
{
    np_sq_plan_t p;
    np_sq_result_t r;
    char md[NP_SQ_MAX_OUT], q[1024];
    CHECK(np_sq_parse_sql("SELECT * FROM users LIMIT 50", &g_sch, &p) == NP_SQ_OK);
    p.confirmed = true;
    CHECK(np_sq_run("tests/golden/golden_full.db", &p, 50, &r) == NP_SQ_OK);
    CHECK(np_sq_render(&p, &r, "golden_full.db", md, sizeof(md)) > 0);
    CHECK(strstr(md, "# 结构化查询结果 · golden_full.db") != NULL);
    CHECK(strstr(md, "SELECT * FROM users") != NULL);
    CHECK(strstr(md, "| id | name | age | email |") != NULL);
    CHECK(strstr(md, "alice") != NULL);
    CHECK(np_sq_render(&p, &r, "x", md, 32) == -1);
    CHECK(np_sq_quality(&r, q, sizeof(q)) > 0);
    CHECK(strstr(q, "空值 50.0%") != NULL);
    CHECK(strstr(q, "数值范围 [25.00, 30.00]") != NULL);
    CHECK(np_sq_quality(&r, q, 16) == -1);
}

static void test_limits(void)
{
    np_sq_plan_t p;
    np_sq_result_t r;
    /* limit 超过上限被夹紧 */
    CHECK(np_sq_parse_sql("SELECT * FROM users LIMIT 99999", &g_sch, &p) == NP_SQ_OK);
    CHECK(p.limit == NP_SQ_MAX_ROWS);
    /* limit=0（无子句）→ run 用默认上限 */
    CHECK(np_sq_parse_sql("SELECT * FROM users", &g_sch, &p) == NP_SQ_OK);
    CHECK(p.limit == 0);
    p.confirmed = true;
    CHECK(np_sq_run("tests/golden/golden_full.db", &p, 1, &r) == NP_SQ_OK);
    CHECK(r.rows_returned == 1);
    CHECK(r.sampled);                          /* 采样上限截断标注 */

    /* 失败路径 */
    CHECK(np_sq_run(NULL, &p, 50, &r) == NP_SQ_ERR_ARG);
    CHECK(np_sq_run("tests/golden/no-such.db", &p, 50, &r) == NP_SQ_ERR_NOFILE);
    CHECK(np_sq_run("tests/golden/plain.zip", &p, 50, &r) == NP_SQ_ERR_NOTSQLITE);
    CHECK(np_sq_parse_sql(NULL, &g_sch, &p) == NP_SQ_ERR_ARG);
    CHECK(np_sq_parse_sql("SELECT * FROM users", NULL, &p) == NP_SQ_ERR_ARG);
    CHECK(np_sq_preview(&p, NULL, 100) == -1);
}

int main(void)
{
    printf("=== F6 structured query tests ===\n");
    load_db();
    test_parse_sql();
    test_security();
    test_preview_and_confirm();
    test_sample_all();
    test_where();
    test_nl();
    test_render_quality();
    test_limits();
    printf("g_pass=%d g_fail=%d\n", g_pass, g_fail);
    if (g_fail) { printf("F6 TESTS FAILED\n"); return 1; }
    printf("F6 TESTS PASSED (%d assertions)\n", g_pass);
    return 0;
}
