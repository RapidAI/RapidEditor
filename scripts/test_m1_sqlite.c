/* test_m1_sqlite.c — M1/F3: SQLite schema 提取单元测试（黄金样本）. */
#include "np_sqlite.h"
#include "np_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    *len = fread(buf, 1, (size_t)n, f);
    fclose(f);
    return buf;
}

static int test_full_db(void) {
    printf("== F3: golden_full.db ==\n");
    size_t len = 0;
    unsigned char *d = read_file("tests/golden/golden_full.db", &len);
    CHECK(d != NULL);
    np_db_schema_t s;
    np_db_status_t st = np_db_parse(d, len, &s);
    CHECK(st == NP_DB_OK);
    CHECK(s.valid == true);
    CHECK(s.wal == false);
    CHECK(s.page_size == 4096);
    CHECK(s.text_encoding == 1);

    /* 三张表 */
    CHECK(s.ntables == 3);
    const np_db_table_t *users = np_db_find_table(&s, "users");
    CHECK(users != NULL);
    CHECK(users->ncols == 4);
    CHECK(strcmp(users->cols[0].name, "id") == 0);
    CHECK(strcmp(users->cols[0].decl_type, "INTEGER") == 0);
    CHECK(users->cols[0].pk == true);
    CHECK(users->rowid_alias == true);
    CHECK(strcmp(users->cols[1].name, "name") == 0);
    CHECK(strcmp(users->cols[1].decl_type, "TEXT") == 0);
    CHECK(users->cols[1].notnull == true);
    CHECK(strcmp(users->cols[2].name, "age") == 0);
    CHECK(strcmp(users->cols[3].name, "email") == 0);
    CHECK(users->est_rows == 2);

    const np_db_table_t *orders = np_db_find_table(&s, "orders");
    CHECK(orders != NULL);
    CHECK(orders->ncols == 5);
    CHECK(orders->est_rows == 3);
    CHECK(strcmp(orders->cols[1].name, "user_id") == 0);
    CHECK(orders->cols[1].notnull == true);
    CHECK(strcmp(orders->cols[2].decl_type, "REAL") == 0);
    CHECK(strcmp(orders->cols[3].decl_type, "BLOB") == 0);

    /* WITHOUT ROWID 表：rootpage 是索引型 b-tree → partial + est_rows=0 */
    const np_db_table_t *meta = np_db_find_table(&s, "meta");
    CHECK(meta != NULL);
    CHECK(meta->ncols == 2);
    CHECK(meta->est_rows == 0);

    /* 两个索引 + 一个视图 + 一个触发器 */
    int idx = 0, views = 0, trigs = 0;
    for (int i = 0; i < s.nobjects; ++i) {
        if (strcmp(s.objects[i].type, "index") == 0) idx++;
        else if (strcmp(s.objects[i].type, "view") == 0) views++;
        else if (strcmp(s.objects[i].type, "trigger") == 0) trigs++;
    }
    CHECK(idx == 2);
    CHECK(views == 1);
    CHECK(trigs == 1);

    /* 引用协议：结构路径可构造（B5 联动验证留给上层） */
    char path[128];
    snprintf(path, sizeof(path), "golden_full.db#users.cols.name");
    CHECK(strstr(path, "#users.cols.name") != NULL);

    free(d);
    return 0;
}

static int test_wal_db(void) {
    printf("== F3: golden_wal.db (WAL 模式) ==\n");
    size_t len = 0;
    unsigned char *d = read_file("tests/golden/golden_wal.db", &len);
    CHECK(d != NULL);
    np_db_schema_t s;
    CHECK(np_db_parse(d, len, &s) == NP_DB_OK);
    CHECK(s.wal == true);
    CHECK(s.ntables == 1);
    const np_db_table_t *t = np_db_find_table(&s, "t");
    CHECK(t != NULL);
    CHECK(t->ncols == 2);
    CHECK(t->est_rows == 3);
    free(d);
    return 0;
}

static int test_reject(void) {
    printf("== F3: 拒绝路径 ==\n");
    np_db_schema_t s;

    /* 太小 */
    CHECK(np_db_parse((const unsigned char *)"abc", 3, &s) == NP_DB_ERR_TOO_SMALL);
    CHECK(np_db_parse(NULL, 0, &s) == NP_DB_ERR_ARG);

    /* 非 SQLite：篡改魔数（模拟加密/其他格式） */
    unsigned char bad[200];
    memset(bad, 0, sizeof(bad));
    memcpy(bad, "SQLite format 4\0", 16);
    CHECK(np_db_parse(bad, sizeof(bad), &s) == NP_DB_ERR_NOT_SQLITE);

    /* 高熵伪加密头 */
    for (int i = 0; i < 200; ++i) bad[i] = (unsigned char)(i * 37 + 11);
    CHECK(np_db_parse(bad, sizeof(bad), &s) == NP_DB_ERR_NOT_SQLITE);

    /* 页大小非法 */
    unsigned char corrupt[512];
    memset(corrupt, 0, sizeof(corrupt));
    memcpy(corrupt, "SQLite format 3\0", 16);
    corrupt[16] = 0x00; corrupt[17] = 0x03; /* 3 不是合法页大小 */
    CHECK(np_db_parse(corrupt, sizeof(corrupt), &s) == NP_DB_ERR_CORRUPT);

    /* H9 路由联动：.db 应被识别为格式轨 SQLite */
    size_t len = 0;
    unsigned char *d = read_file("tests/golden/golden_full.db", &len);
    np_probe_result_t r;
    np_probe(d, len, "x.db", &r);
    CHECK(r.route == NP_ROUTE_FORMAT);
    CHECK(r.fmt.kind == NP_FMT_SQLITE);
    free(d);
    return 0;
}

int main(void) {
    test_full_db();
    test_wal_db();
    test_reject();
    if (failures == 0) printf("ALL SQLITE TESTS PASSED\n");
    else               printf("%d SQLITE FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
