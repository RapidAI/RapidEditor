/* scripts/test_f4_featcard.c — F4 特征抽取与特征卡测试.
 * 门禁 / PII 脱敏 / F2 docx 提取 / F3 DB 提取 / 渲染 / 失败路径。
 */
#include "np_featcard.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)

#define CHECK_STR(actual, expect) do { \
    if (strcmp((actual), (expect)) == 0) { g_pass++; } \
    else { printf("FAIL %s:%d  got '%s' expect '%s'\n", \
                 __FILE__, __LINE__, (actual), (expect)); g_fail++; } \
} while (0)

static void test_gate(void)
{
    np_fc_card_t c;
    int i;
    np_fc_init(&c, NP_FC_DOC, "test.bin");
    CHECK(c.n == 0);
    CHECK_STR(c.source, "test.bin");

    CHECK(np_fc_add(&c, NP_FC_FORMAT, NP_FC_L0, "a", "A", "v",
                    "test.bin#/x", 80) == NP_FC_OK);
    CHECK(np_fc_find(&c, "a") != NULL);
    CHECK(np_fc_find(&c, "missing") == NULL);
    CHECK(np_fc_add(&c, NP_FC_FORMAT, NP_FC_L0, "a", "dup", "v",
                    "test.bin#/x", 80) == NP_FC_ERR_DUP);
    CHECK(np_fc_add(&c, NP_FC_SEMANTIC, NP_FC_L2, "b", "low", "v",
                    "test.bin#/x", 24) == NP_FC_ERR_LOW_CONF);
    CHECK(np_fc_add(&c, NP_FC_STRUCTURE, NP_FC_L0, "c", "noev", "v",
                    "plain-no-citation", 90) == NP_FC_ERR_NO_EVIDENCE);
    CHECK(np_fc_add(&c, NP_FC_FORMAT, NP_FC_L0, "", "empty-id", "v",
                    "test.bin#/x", 80) == NP_FC_ERR_ARG);
    CHECK(c.n == 1);
    CHECK(c.rejected_dup == 1);
    CHECK(c.rejected_low_conf == 1);
    CHECK(c.rejected_no_evidence == 1);

    /* PII 值入卡强制脱敏 */
    CHECK(np_fc_add(&c, NP_FC_SECURITY, NP_FC_L0, "pii", "敏感值",
                    "bob@example.com", "test.bin#/x", 100) == NP_FC_OK);
    CHECK(c.redacted == 1);
    CHECK_STR(np_fc_find(&c, "pii")->value, "[EMAIL]");

    /* 上限 fail-closed */
    for (i = 0; i < NP_FC_MAX_FEATURES + 10; i++) {
        char id[32];
        (void)snprintf(id, sizeof(id), "bulk%d", i);
        if (i < NP_FC_MAX_FEATURES - 2) {
            CHECK(np_fc_add(&c, NP_FC_STRUCTURE, NP_FC_L0, id, "批量",
                            "v", "test.bin#/x", 100) == NP_FC_OK);
        } else {
            CHECK(np_fc_add(&c, NP_FC_STRUCTURE, NP_FC_L0, id, "批量",
                            "v", "test.bin#/x", 100) == NP_FC_ERR_FULL);
        }
    }
    CHECK(c.n == NP_FC_MAX_FEATURES);
    CHECK(c.truncated);
}

static void test_pii(void)
{
    const char *why = NULL;
    char out[128];
    CHECK(np_fc_looks_like_pii("alice@example.com", &why));
    CHECK_STR(why, "邮箱");
    CHECK(np_fc_looks_like_pii("110101199001011234", &why));
    CHECK_STR(why, "身份证号");
    CHECK(np_fc_looks_like_pii("13812345678", &why));
    CHECK_STR(why, "手机号");
    CHECK(np_fc_looks_like_pii("sk-abcdefghijklmnopqrst", &why));
    CHECK_STR(why, "凭证形态");
    CHECK(np_fc_looks_like_pii("order 12345678901234", &why));
    CHECK_STR(why, "长数字");
    CHECK(!np_fc_looks_like_pii("Quarterly Report 2024", &why));
    CHECK(!np_fc_looks_like_pii("", NULL));
    CHECK(!np_fc_looks_like_pii(NULL, NULL));

    np_fc_redact("mail alice@example.com or 13812345678", out, sizeof(out));
    CHECK(strstr(out, "[EMAIL]") != NULL);
    CHECK(strstr(out, "[PHONE]") != NULL);
    CHECK(strstr(out, "alice@example.com") == NULL);
    CHECK(strstr(out, "13812345678") == NULL);
    np_fc_redact("id 110101199001011234", out, sizeof(out));
    CHECK(strstr(out, "[ID]") != NULL);
    np_fc_redact("token sk-abc123def456", out, sizeof(out));
    CHECK(strstr(out, "[SECRET]") != NULL);
}

static void test_ooxml(void)
{
    np_fc_card_t c;
    char md[NP_FC_MAX_CARD];
    int n;
    CHECK(np_fc_extract_ooxml("tests/golden/golden.docx", "golden.docx",
                              &c) == NP_FC_OK);
    CHECK(c.n > 0);
    CHECK(c.domain == NP_FC_DOC);
    CHECK_STR(c.source, "golden.docx");
    CHECK_STR(np_fc_find(&c, "doc.type")->value, "Word");
    CHECK(np_fc_find(&c, "doc.type")->conf == 100);
    CHECK_STR(np_fc_find(&c, "doc.parts")->value, "4");
    CHECK_STR(np_fc_find(&c, "doc.paragraphs")->value, "8");
    CHECK_STR(np_fc_find(&c, "doc.tables")->value, "1 表 / 4 单元格");
    CHECK_STR(np_fc_find(&c, "doc.styles")->value, "1");
    CHECK(np_fc_find(&c, "doc.annotations") != NULL);
    /* 主题属 L2 候选 */
    CHECK(np_fc_find(&c, "doc.topic.1") != NULL);
    CHECK(np_fc_find(&c, "doc.topic.1")->level == NP_FC_L2);
    CHECK(np_fc_find(&c, "doc.topic.1")->conf < 100);
    /* 每条特征都可回跳 */
    {
        int i;
        for (i = 0; i < c.n; i++) {
            CHECK(strchr(c.items[i].evidence, '#') != NULL ||
                  strstr(c.items[i].evidence, ":0x") != NULL);
        }
    }
    n = np_fc_render(&c, md, sizeof(md));
    CHECK(n > 0);
    CHECK(strstr(md, "# 特征卡 · golden.docx") != NULL);
    CHECK(strstr(md, "| id | 类别 | 等级 | 置信度 | 值 | 证据 |") != NULL);
    CHECK(strstr(md, "doc.type") != NULL);
    CHECK(strstr(md, "L2=AI 候选启发式") != NULL);
    CHECK(np_fc_render(&c, md, 64) == -1);   /* 容量不足不产半张卡 */
}

static void test_sqlite(void)
{
    np_fc_card_t c;
    const np_fc_feature_t *f;
    CHECK(np_fc_extract_sqlite("tests/golden/golden_full.db", "golden_full.db",
                               &c) == NP_FC_OK);
    CHECK(c.n > 0);
    CHECK(c.domain == NP_FC_DB);
    CHECK_STR(np_fc_find(&c, "db.format")->value, "SQLite");
    CHECK_STR(np_fc_find(&c, "db.page_size")->value, "4096 字节");
    CHECK_STR(np_fc_find(&c, "db.wal")->value, "否");
    CHECK_STR(np_fc_find(&c, "db.table.users")->value, "4 列 / 2 行");
    CHECK_STR(np_fc_find(&c, "db.table.users.pk")->value, "INTEGER 主键别名");
    CHECK(np_fc_find(&c, "db.table.orders") != NULL);
    CHECK(np_fc_find(&c, "db.table.meta") != NULL);
    /* 列语义（L2） */
    f = np_fc_find(&c, "db.table.users.col.email.semantic");
    CHECK(f != NULL && f->level == NP_FC_L2);
    if (f) CHECK_STR(f->value, "email 地址");
    f = np_fc_find(&c, "db.table.orders.col.amount.semantic");
    CHECK(f != NULL);
    if (f) CHECK_STR(f->value, "金额");
    /* PII 列标记（L1） */
    f = np_fc_find(&c, "db.table.users.col.email.pii");
    CHECK(f != NULL && f->level == NP_FC_L1 && f->conf == 85);
    /* 外键推断（单数列名 user_id → 复数表名 users） */
    f = np_fc_find(&c, "db.table.orders.col.user_id.fk");
    CHECK(f != NULL && f->level == NP_FC_L1 && f->conf == 70);
    if (f) CHECK(strstr(f->value, "users") != NULL);
    /* schema 未完全解析被诚实记录而非伪造完整 */
    CHECK(np_fc_find(&c, "db.partial") != NULL);
}

static void test_failure(void)
{
    np_fc_card_t c;
    CHECK(np_fc_extract_ooxml(NULL, "x", &c) == NP_FC_ERR_ARG);
    CHECK(np_fc_extract_sqlite(NULL, "x", &c) == NP_FC_ERR_ARG);
    CHECK(np_fc_extract_ooxml("tests/golden/no-such.docx", "x", &c) ==
          NP_FC_ERR_PARSE);
    CHECK(np_fc_extract_ooxml("tests/golden/encrypted.docx", "x", &c) ==
          NP_FC_ERR_PARSE);
    CHECK(np_fc_extract_sqlite("tests/golden/plain.zip", "x", &c) ==
          NP_FC_ERR_PARSE);
}

int main(void)
{
    printf("=== F4 feature card tests ===\n");
    test_gate();
    test_pii();
    test_ooxml();
    test_sqlite();
    test_failure();
    printf("g_pass=%d g_fail=%d\n", g_pass, g_fail);
    if (g_fail) { printf("F4 TESTS FAILED\n"); return 1; }
    printf("F4 TESTS PASSED (%d assertions)\n", g_pass);
    return 0;
}
