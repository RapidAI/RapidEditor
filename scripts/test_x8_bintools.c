/* scripts/test_x8_bintools.c — X8 原生二进制与格式工具集测试.
 * read/search/patch三门禁/白名单/disasm/tree/query/注册/失败路径。
 */
#include "np_bintools.h"
#include "np_integrity.h"
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

static unsigned char DATA[] = {
    0x48,0x89,0x5c,0x24,0x08,0xde,0xad,0xbe,0xef,0x00,
    'H','E','L','L','O','_','W','O','R','L','D'
};

static void test_read(void)
{
    char out[1024];
    np_citation_t c;
    np_bt_status_t st;
    memset(&c, 0, sizeof(c));
    st = np_bt_binary_read(DATA, sizeof(DATA), 0, 10, "fw.bin", 7,
                           out, sizeof(out), &c);
    CHECK(st == NP_BT_OK);
    CHECK(strstr(out, "00000000  48 89 5c 24 08 de ad be ef 00") != NULL);
    CHECK(strstr(out, "|H.\\$......|") != NULL);
    CHECK(c.kind == NP_CITE_OFFSET);
    CHECK_STR(c.file, "fw.bin");
    CHECK(c.start == 0 && c.end == 10);
    CHECK(c.revision == 7);
    CHECK(c.excerpt_hash == np_crc32(DATA, 10));
    /* 偏移读取 */
    CHECK(np_bt_binary_read(DATA, sizeof(DATA), 10, 5, "fw.bin", 0,
                            out, sizeof(out), NULL) == NP_BT_OK);
    CHECK(strstr(out, "0000000a  48 45 4c 4c 4f") != NULL);
    /* 容量不足 */
    CHECK(np_bt_binary_read(DATA, sizeof(DATA), 0, 10, "x", 0,
                            out, 16, NULL) == NP_BT_ERR_FULL);
}

static void test_search(void)
{
    size_t offs[8], nh = 0;
    bool tr = false;
    unsigned char needle[] = {0xde, 0xad};
    unsigned char hello[] = {'H','E','L','L','O'};
    CHECK(np_bt_binary_search(DATA, sizeof(DATA), needle, 2,
                              offs, 8, &nh, &tr) == NP_BT_OK);
    CHECK(nh == 1);
    CHECK(offs[0] == 5);
    CHECK(!tr);
    nh = 0;
    CHECK(np_bt_binary_search(DATA, sizeof(DATA), hello, 5,
                              offs, 8, &nh, &tr) == NP_BT_OK);
    CHECK(nh == 1 && offs[0] == 10);
    /* 不存在的字节 */
    {
        unsigned char none[] = {0xff, 0xff};
        nh = 0;
        CHECK(np_bt_binary_search(DATA, sizeof(DATA), none, 2,
                                  offs, 8, &nh, &tr) == NP_BT_ERR_NO_HIT);
        CHECK(nh == 0);
    }
    /* 超长 needle */
    CHECK(np_bt_binary_search(DATA, sizeof(DATA), DATA, sizeof(DATA) + 1,
                              offs, 8, &nh, &tr) == NP_BT_ERR_NO_HIT);
}

static void test_path_whitelist(void)
{
    const char *pre[] = { "workspace/", "tests/" };
    CHECK(np_bt_path_allowed("workspace/fw.bin", pre, 2));
    CHECK(np_bt_path_allowed("tests/golden/a.bin", pre, 2));
    CHECK(!np_bt_path_allowed("evil.bin", pre, 2));
    CHECK(!np_bt_path_allowed("workspace/../evil.bin", pre, 2));
    CHECK(!np_bt_path_allowed("../evil.bin", pre, 2));
    CHECK(!np_bt_path_allowed("/abs/fw.bin", pre, 2));
    CHECK(!np_bt_path_allowed("C:\\evil.bin", pre, 2));
    /* 空表 fail-closed */
    CHECK(!np_bt_path_allowed("workspace/fw.bin", NULL, 0));
    {
        const char *empty[] = { "" };
        CHECK(!np_bt_path_allowed("workspace/fw.bin", empty, 1));
    }
}

static void test_patch_gates(void)
{
    np_patch_set_t *ps;
    np_bt_write_policy_t p;
    const char *pre[] = { "workspace/" };
    unsigned char rep[] = {0x90, 0x90};
    uint32_t crc = np_crc32(DATA, sizeof(DATA));
    const np_patch_hunk_t *h;
    np_bt_status_t st;

    memset(&p, 0, sizeof(p));
    p.target = "workspace/fw.bin";
    p.expected_crc32 = crc;
    p.base_revision = 5;
    p.allow_prefix = pre;
    p.n_prefix = 1;
    ps = np_patch_set_create(5);

    /* 正常路径：只加 hunk，不改 buffer/磁盘 */
    st = np_bt_binary_patch(DATA, sizeof(DATA), &p, ps, 5, 4, rep, 2);
    CHECK(st == NP_BT_OK);
    CHECK(np_patch_set_count(ps) == 1);
    h = np_patch_set_hunk(ps, 0);
    CHECK(h->op == NP_OP_REPLACE);
    CHECK(h->u.byte.offset == 5 && h->u.byte.len == 4);
    CHECK(h->u.byte.bytes_len == 2);
    CHECK(memcmp(h->u.byte.bytes, rep, 2) == 0);
    /* 源字节未被修改（不裸写红线） */
    CHECK(DATA[5] == 0xde && DATA[6] == 0xad);

    /* 门禁 1：路径白名单拒绝（空表） */
    p.allow_prefix = NULL; p.n_prefix = 0;
    CHECK(np_bt_binary_patch(DATA, sizeof(DATA), &p, ps, 0, 1, rep, 2)
          == NP_BT_ERR_DENIED);
    CHECK(np_patch_set_count(ps) == 1);  /* 未加 hunk */
    p.allow_prefix = pre; p.n_prefix = 1;

    /* 门禁 2：expected_hash 不符 */
    p.expected_crc32 = 0;
    CHECK(np_bt_binary_patch(DATA, sizeof(DATA), &p, ps, 0, 1, rep, 2)
          == NP_BT_ERR_HASH_MISMATCH);
    CHECK(np_patch_set_count(ps) == 1);
    p.expected_crc32 = crc;

    /* 门禁 3：base_revision 过期 */
    p.base_revision = 99;
    CHECK(np_bt_binary_patch(DATA, sizeof(DATA), &p, ps, 0, 1, rep, 2)
          == NP_BT_ERR_STALE_REVISION);
    CHECK(np_patch_set_count(ps) == 1);
    p.base_revision = 5;

    /* 范围越界 */
    CHECK(np_bt_binary_patch(DATA, sizeof(DATA), &p, ps,
                             sizeof(DATA), 4, rep, 2) == NP_BT_ERR_RANGE);
    np_patch_set_destroy(ps);
}

static void test_disasm(void)
{
    np_bt_insn_view_t ins[3];
    char out[512];
    memset(ins, 0, sizeof(ins));
    ins[0].addr = 0x401000; ins[0].offset = 0; ins[0].size = 1;
    ins[1].addr = 0x401001; ins[1].offset = 1; ins[1].size = 3;
    ins[2].addr = 0x401004; ins[2].offset = 4; ins[2].size = 1;
    CHECK(np_bt_binary_disasm(ins, 3, 0, 2, out, sizeof(out)) == NP_BT_OK);
    CHECK(strstr(out, "#0 @0x401000 off=0x0 size=1") != NULL);
    CHECK(strstr(out, "#1 @0x401001 off=0x1 size=3") != NULL);
    CHECK(strstr(out, "#2 @0x401004") != NULL);
    CHECK(np_bt_binary_disasm(ins, 3, 2, 1, out, sizeof(out)) == NP_BT_ERR_RANGE);
    CHECK(np_bt_binary_disasm(ins, 3, 0, 3, out, sizeof(out)) == NP_BT_ERR_RANGE);
}

static void test_tree_query(void)
{
    np_bt_tree_node_t nodes[3];
    char out[1024];
    np_citation_t c;
    memset(nodes, 0, sizeof(nodes));
    snprintf(nodes[0].type, sizeof(nodes[0].type), "paragraph");
    snprintf(nodes[0].path, sizeof(nodes[0].path), "/w:p[0]");
    snprintf(nodes[0].label, sizeof(nodes[0].label), "Hello");
    nodes[0].depth = 0;
    snprintf(nodes[1].type, sizeof(nodes[1].type), "table");
    snprintf(nodes[1].path, sizeof(nodes[1].path), "/w:tbl[0]");
    nodes[1].depth = 1;
    snprintf(nodes[2].type, sizeof(nodes[2].type), "cell");
    snprintf(nodes[2].path, sizeof(nodes[2].path), "/w:tbl[0]/w:tc[0]");
    nodes[2].depth = 2;
    CHECK(np_bt_format_tree(nodes, 3, 2, out, sizeof(out), &c,
                            "doc.docx") == NP_BT_OK);
    CHECK(strstr(out, "paragraph") != NULL);
    CHECK(strstr(out, "table") != NULL);
    CHECK(strstr(out, "cell") == NULL);  /* 截断到 2 个节点 */
    CHECK(c.kind == NP_CITE_STRUCT);
    CHECK_STR(c.path, "/w:p[0]");

    /* query_readonly：只接受带 F6 provenance 的结果 */
    {
        char q[1024];
        const char *good = "# 结构化查询结果 · db\n| a |\n";
        CHECK(np_bt_format_query_readonly(good, q, sizeof(q), &c,
                                          "db") == NP_BT_OK);
        CHECK(strstr(q, "结构化查询结果") != NULL);
        CHECK(c.kind == NP_CITE_STRUCT);
        CHECK_STR(c.path, "query_result");
        CHECK(np_bt_format_query_readonly("DELETE FROM users", q, sizeof(q),
                                          NULL, "db") == NP_BT_ERR_UNSUPPORTED);
        CHECK(np_bt_format_query_readonly("", q, sizeof(q), NULL, "db")
              == NP_BT_ERR_ARG);
    }
}

static void test_registry(void)
{
    np_registry_t *reg = np_registry_create();
    const np_tool_desc_t *d;
    size_t n = np_bintools_register(reg);
    CHECK(n == 6);
    CHECK(np_registry_count(reg) == 6);
    d = np_registry_get(reg, "editor.binary.read");
    CHECK(d != NULL && d->risk == NP_RISK_READONLY);
    CHECK(d->scopes & NP_SCOPE_BINARY);
    d = np_registry_get(reg, "editor.binary.patch");
    CHECK(d != NULL && d->risk == NP_RISK_WRITE);
    d = np_registry_get(reg, "editor.format.tree");
    CHECK(d != NULL && d->scopes & NP_SCOPE_FORMAT);
    d = np_registry_get(reg, "editor.format.query_readonly");
    CHECK(d != NULL && d->risk == NP_RISK_READONLY);
    CHECK(np_bintools_register(NULL) == 0);
    np_registry_destroy(reg);
}

static void test_failure(void)
{
    char out[64];
    np_citation_t c;
    CHECK(np_bt_binary_read(NULL, 10, 0, 1, "x", 0, out, sizeof(out), &c)
          == NP_BT_ERR_ARG);
    CHECK(np_bt_binary_read(DATA, sizeof(DATA), sizeof(DATA), 1,
                            "x", 0, out, sizeof(out), &c) == NP_BT_ERR_RANGE);
    CHECK(np_bt_binary_search(NULL, 10, DATA, 1, (size_t *)out, 1, NULL, NULL)
          == NP_BT_ERR_ARG);
    CHECK(np_bt_binary_patch(NULL, 10, NULL, NULL, 0, 0, NULL, 0)
          == NP_BT_ERR_ARG);
    CHECK(np_bt_binary_disasm(NULL, 0, 0, 0, out, sizeof(out)) == NP_BT_ERR_ARG);
    CHECK(np_bt_format_tree(NULL, 0, 0, out, sizeof(out), NULL, NULL)
          == NP_BT_ERR_ARG);
    CHECK_STR(np_bt_status_name(NP_BT_OK), "ok");
    CHECK_STR(np_bt_status_name(NP_BT_ERR_HASH_MISMATCH), "hash-mismatch");
}

int main(void)
{
    printf("=== X8 binary tools tests ===\n");
    test_read();
    test_search();
    test_path_whitelist();
    test_patch_gates();
    test_disasm();
    test_tree_query();
    test_registry();
    test_failure();
    printf("g_pass=%d g_fail=%d\n", g_pass, g_fail);
    if (g_fail) { printf("X8 TESTS FAILED\n"); return 1; }
    printf("X8 TESTS PASSED (%d assertions)\n", g_pass);
    return 0;
}
