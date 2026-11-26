/* test_m4_plancard.c — M4/B2: 计划卡模型单元测试.
 *
 * 覆盖：探测路由 → 计划步骤、T7 降级链预告、X2 工具路由、
 * SQLite schema 汇合（citation 串 / B5 解析 / 上下文片段）。
 * 本测试不链接 CUI（模型层红线）。
 */
#include "np_plancard.h"
#include "np_citation.h"
#include "np_texttools.h"

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

/* 最小 ELF64 LE x86-64 头（与 test_s1_probe 同构造） */
static void make_elf64_x64(unsigned char *b, size_t n) {
    memset(b, 0, n);
    memcpy(b, "\x7F" "ELF", 4);
    b[4] = 2;  /* ELFCLASS64 */
    b[5] = 1;  /* LE */
    b[6] = 1;  /* EV_CURRENT */
    b[0x10] = 2; b[0x11] = 0; /* ET_EXEC */
    b[0x12] = 0x3E;           /* EM_X86_64 */
    unsigned char e[8] = {0x00,0x10,0x40,0x00,0x00,0x00,0x00,0x00};
    memcpy(b + 0x18, e, 8);
    (void)n;
}

static int test_sqlite_plan(void) {
    printf("== M4: SQLite 计划（golden_full.db） ==\n");
    size_t len = 0;
    unsigned char *d = read_file("tests/golden/golden_full.db", &len);
    CHECK(d != NULL);
    if (!d) return 1;

    np_plan_t *p = np_plan_create();
    np_plan_refresh(p, "golden_full.db", d, len, np_plan_default_registry());

    /* 路由：格式轨 */
    CHECK(p->route == NP_ROUTE_FORMAT);
    CHECK(p->track == NP_TRACK_FORMAT);
    CHECK(p->has_schema == true);
    CHECK(p->schema.ntables == 3);
    CHECK(strstr(p->subject_desc, "SQLite") != NULL);
    CHECK(strstr(p->subject_desc, "WAL") == NULL);   /* 非 WAL 库 */

    /* 步骤：探测 done → schema done → 引用 done → F8 skipped */
    CHECK(p->nsteps == 4);
    CHECK(p->steps[0].status == NP_PLAN_DONE);
    CHECK(strstr(p->steps[1].title, "Schema") != NULL);
    CHECK(p->steps[1].status == NP_PLAN_DONE);
    CHECK(strstr(p->steps[1].detail, "3 张表") != NULL);
    CHECK(strstr(p->steps[2].title, "B5") != NULL);
    CHECK(p->steps[2].status == NP_PLAN_DONE);
    CHECK(p->steps[3].status == NP_PLAN_SKIPPED);
    CHECK(strstr(p->steps[3].title, "F8") != NULL);

    /* X2 路由：X7 四个 core 工具全部入围（顺序不保证） */
    CHECK(p->ntools == 4);
    CHECK(strcmp(p->tool_ids[0], "editor.text.read") == 0);
    int has_goto = 0;
    for (int i = 0; i < p->ntools; ++i)
        if (strcmp(p->tool_ids[i], "editor.text.goto") == 0) has_goto = 1;
    CHECK(has_goto == 1);

    /* Citation 串（B5: file#表 与 file#表/列） */
    char cite[300];
    np_plan_citation(p, "golden_full.db", 0, -1, cite, sizeof(cite));
    CHECK(strstr(cite, "#") != NULL);
    CHECK(strstr(cite, p->schema.tables[0].name) != NULL);
    np_plan_citation(p, "golden_full.db", 0, 0, cite, sizeof(cite));
    CHECK(strstr(cite, "/") != NULL);
    CHECK(strstr(cite, p->schema.tables[0].cols[0].name) != NULL);

    /* B5 协议：citation 串必须能被 np_citation_parse 解析为 STRUCT */
    np_citation_t c;
    CHECK(np_citation_parse(cite, &c) == true);
    CHECK(c.kind == NP_CITE_STRUCT);
    CHECK(strchr(c.path, '/') != NULL);

    /* 越界 → 空串 */
    np_plan_citation(p, "x", 99, 0, cite, sizeof(cite));
    CHECK(cite[0] == '\0');
    np_plan_citation(p, "x", -1, 0, cite, sizeof(cite));
    CHECK(cite[0] == '\0');

    /* 上下文片段：行数 + SQL + 列清单 */
    char sn[2048];
    size_t n = np_plan_context_snippet(p, 0, sn, sizeof(sn));
    CHECK(n > 0);
    CHECK(strstr(sn, "上下文片段") != NULL);
    CHECK(strstr(sn, "CREATE TABLE") != NULL);
    CHECK(strstr(sn, "PRIMARY KEY") != NULL);
    /* 越界 → 空片段 */
    CHECK(np_plan_context_snippet(p, 99, sn, sizeof(sn)) == 0);
    CHECK(sn[0] == '\0');

    np_plan_destroy(p);
    free(d);
    return 0;
}

static int test_wal_plan(void) {
    printf("== M4: WAL 库计划（golden_wal.db） ==\n");
    size_t len = 0;
    unsigned char *d = read_file("tests/golden/golden_wal.db", &len);
    CHECK(d != NULL);
    if (!d) return 1;
    np_plan_t *p = np_plan_create();
    np_plan_refresh(p, "golden_wal.db", d, len, NULL);  /* reg=NULL → 无工具 */
    CHECK(p->route == NP_ROUTE_FORMAT);
    CHECK(p->has_schema == true);
    CHECK(p->schema.wal == true);
    CHECK(strstr(p->subject_desc, "WAL") != NULL);
    CHECK(p->ntools == 0);   /* reg=NULL 跳过路由 */
    CHECK(p->nsteps == 4);
    np_plan_destroy(p);
    free(d);
    return 0;
}

static int test_program_plan(void) {
    printf("== M4: ELF 计划（T7 工具链预告） ==\n");
    unsigned char b[128];
    make_elf64_x64(b, sizeof(b));
    np_plan_t *p = np_plan_create();
    np_plan_refresh(p, "firmware.elf", b, sizeof(b), np_plan_default_registry());

    CHECK(p->route == NP_ROUTE_PROGRAM);
    CHECK(p->track == NP_TRACK_BINARY);
    CHECK(strstr(p->subject_desc, "x86-64") != NULL ||
          strstr(p->subject_desc, "x64") != NULL);
    CHECK(strstr(p->subject_desc, "64") != NULL);

    /* T7 降级链 reason 直出（T6 场景） */
    CHECK(p->has_t7 == true);
    CHECK(p->t7.tier >= NP_TI_TIER_BUNDLED && p->t7.tier < NP_TI_TIER_NONE);
    CHECK(p->t7.reason[0] != '\0');

    /* 步骤：探测 done → 工具链 done/failed → 反汇编 pending */
    CHECK(p->nsteps == 3);
    CHECK(p->steps[0].status == NP_PLAN_DONE);
    CHECK(strstr(p->steps[1].title, "T7") != NULL);
    CHECK(p->steps[1].status == NP_PLAN_DONE ||
          p->steps[1].status == NP_PLAN_FAILED);
    CHECK(strstr(p->steps[2].title, "R2") != NULL);
    CHECK(p->steps[2].status == NP_PLAN_PENDING);

    /* X2 路由仍按 core 常驻给工具 */
    CHECK(p->ntools == 4);
    np_plan_destroy(p);
    return 0;
}

static int test_text_plan(void) {
    printf("== M4: 文本计划 ==\n");
    const char *txt = "hello world\nplain text file\nthird line\n";
    np_plan_t *p = np_plan_create();
    np_plan_refresh(p, "notes.txt", (const unsigned char *)txt, strlen(txt),
                    np_plan_default_registry());
    CHECK(p->route == NP_ROUTE_TEXT);
    CHECK(p->track == NP_TRACK_TEXT);
    CHECK(p->nsteps == 3);
    CHECK(strstr(p->steps[2].title, "B3") != NULL);
    CHECK(p->steps[2].status == NP_PLAN_PENDING);
    CHECK(p->ntools == 4);
    /* 无 schema：citation / snippet 全空 */
    char cite[64];
    np_plan_citation(p, "notes.txt", 0, 0, cite, sizeof(cite));
    CHECK(cite[0] == '\0');
    CHECK(np_plan_context_snippet(p, 0, cite, sizeof(cite)) == 0);
    np_plan_destroy(p);
    return 0;
}

static int test_registry_singleton(void) {
    printf("== M4: 缺省注册表单例 ==\n");
    np_registry_t *r1 = np_plan_default_registry();
    np_registry_t *r2 = np_plan_default_registry();
    CHECK(r1 != NULL);
    CHECK(r1 == r2);
    CHECK(np_registry_count(r1) >= 4);
    return 0;
}

int main(void) {
    test_sqlite_plan();
    test_wal_plan();
    test_program_plan();
    test_text_plan();
    test_registry_singleton();
    if (failures == 0) {
        printf("ALL PASSED\n");
        return 0;
    }
    printf("%d FAILURES\n", failures);
    return 1;
}
