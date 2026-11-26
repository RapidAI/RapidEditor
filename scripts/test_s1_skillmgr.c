/* test_s1_skillmgr.c — S1: X3/X4 Skill 管理 + X7 原生文本工具 单元测试. */
#include "np_skillmgr.h"
#include "np_texttools.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  setup fail: %s\n", path); return; }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static int test_manifest(void) {
    printf("== X3: SKILL.md 解析 ==\n");
    write_file(".s1test_skill_src/SKILL.md",
               "---\nname: loongarch-disasm\nversion: 1.2.0\n"
               "description: LoongArch disassembly helper\n"
               "entry: run.py\n---\nbody here\n");
    np_skill_t s;
    CHECK(np_skill_parse_manifest(".s1test_skill_src", &s) == NP_SKILL_OK);
    CHECK(strcmp(s.name, "loongarch-disasm") == 0);
    CHECK(strcmp(s.version, "1.2.0") == 0);
    CHECK(strstr(s.description, "LoongArch") != NULL);
    CHECK(strcmp(s.entry, "run.py") == 0);

    /* 缺 manifest */
    CHECK(np_skill_parse_manifest(".s1test_nonexist", &s) == NP_SKILL_ERR_NO_MANIFEST);

    /* 缺 name → bad */
    write_file(".s1test_skill_bad/manifest.json",
               "{\"version\": \"0.1\", \"description\": \"no name\"}");
    CHECK(np_skill_parse_manifest(".s1test_skill_bad", &s) == NP_SKILL_ERR_BAD_MANIFEST);
    return 0;
}

static int test_install_lifecycle(void) {
    printf("== X4: 安装/启停/删除 ==\n");

    np_skill_store_t *st = np_skill_store_open(".s1test_store");
    CHECK(st != NULL);

    /* 本地安装（无远程来源 → 记录实际哈希，不要求预置哈希） */
    np_skill_t inst;
    CHECK(np_skill_install(st, ".s1test_skill_src", NULL, NULL, &inst) == NP_SKILL_OK);
    CHECK(strcmp(inst.name, "loongarch-disasm") == 0);
    CHECK(inst.enabled == true);
    CHECK(strlen(inst.sha256) == 64);
    CHECK(np_skill_count(st) == 1);
    /* 安装目录已拷贝 */
    CHECK(np_skill_find(st, "loongarch-disasm") != NULL);

    /* 重复安装 → EXISTS */
    CHECK(np_skill_install(st, ".s1test_skill_src", NULL, NULL, NULL)
          == NP_SKILL_ERR_EXISTS);

    /* 远程来源但无哈希 → 拒装 */
    CHECK(np_skill_install(st, ".s1test_skill_bad", "https://skillhub.example/pkg", NULL, NULL)
          == NP_SKILL_ERR_BAD_MANIFEST); /* bad manifest 先挂 */

    write_file(".s1test_skill_src2/SKILL.md",
               "---\nname: mcp-server-x\nversion: 0.3.0\ndescription: demo mcp\n---\n");
    CHECK(np_skill_install(st, ".s1test_skill_src2", "https://github.com/a/b", NULL, NULL)
          == NP_SKILL_ERR_HASH_MISSING); /* 远程无哈希 → X3 红线 */

    /* 重开 store 持久化验证 */
    np_skill_store_close(st);
    st = np_skill_store_open(".s1test_store");
    CHECK(np_skill_count(st) == 1);
    const np_skill_t *f = np_skill_find(st, "loongarch-disasm");
    CHECK(f && f->enabled == true);

    /* 启停 */
    CHECK(np_skill_set_enabled(st, "loongarch-disasm", false) == NP_SKILL_OK);
    f = np_skill_find(st, "loongarch-disasm");
    CHECK(f && f->enabled == false);
    CHECK(np_skill_set_enabled(st, "no-such", true) == NP_SKILL_ERR_NOT_FOUND);

    /* 删除 */
    CHECK(np_skill_uninstall(st, "loongarch-disasm") == NP_SKILL_OK);
    CHECK(np_skill_count(st) == 0);
    CHECK(np_skill_uninstall(st, "loongarch-disasm") == NP_SKILL_ERR_NOT_FOUND);
    np_skill_store_close(st);
    return 0;
}

static int test_texttools(void) {
    printf("== X7: 原生文本工具 ==\n");
    np_patch_buffer_t b;
    np_patch_buffer_init(&b);
    const char *src_text = "int main(void) {\n    return 0;\n}\n// TODO: fix\n";
    CHECK(np_patch_buffer_set(&b, src_text, strlen(src_text)));

    /* read */
    char out[256];
    size_t nread = 0;
    CHECK(np_txt_read(&b, 2, 3, out, sizeof(out), &nread) == NP_TXT_OK);
    CHECK(nread == 2);
    CHECK(strstr(out, "2|     return 0;") != NULL);
    CHECK(strstr(out, "3| }") != NULL);
    CHECK(np_txt_read(&b, 0, 99, out, sizeof(out), &nread) == NP_TXT_OK);
    CHECK(nread == 4);
    CHECK(np_txt_read(&b, 5, 9, out, sizeof(out), &nread) == NP_TXT_ERR_OUT_OF_RANGE);

    /* search */
    size_t hits[8], nh = 0;
    CHECK(np_txt_search(&b, "return", false, hits, 8, &nh) == NP_TXT_OK);
    CHECK(nh == 1 && hits[0] == 2);
    CHECK(np_txt_search(&b, "todo", true, hits, 8, &nh) == NP_TXT_OK); /* 大小写不敏感 */
    CHECK(nh == 1 && hits[0] == 4);
    CHECK(np_txt_search(&b, "not-there", false, hits, 8, &nh) == NP_TXT_ERR_NO_HIT);

    /* goto */
    size_t off = 0;
    CHECK(np_txt_goto(&b, 3, &off) == NP_TXT_OK);
    CHECK(strncmp((const char *)b.data + off, "}", 1) == 0);
    CHECK(np_txt_goto(&b, 9, &off) == NP_TXT_ERR_OUT_OF_RANGE);

    /* patch：stale revision 拒绝 */
    np_patch_set_t *ps = np_patch_set_create(0); /* buffer revision=0? set后为0 */
    /* X7 用 1-based 行号 → patch API 是 0-based 半开区间：1-based 第 4 行
     * = 0-based [3,4) */
    np_patch_add_text(ps, NP_OP_REPLACE, 3, 4, "// DONE: fixed\n", 15);
    size_t newrev = 0;
    /* buffer 初始 revision 0，ps base 0 → 应成功 */
    CHECK(np_txt_patch(ps, &b, &newrev) == NP_TXT_OK);
    CHECK(newrev == 1);
    np_patch_set_destroy(ps);
    /* 用旧 base 再提交 → stale */
    ps = np_patch_set_create(0);
    np_patch_add_text(ps, NP_OP_INSERT, 1, 1, "x\n", 2);
    CHECK(np_txt_patch(ps, &b, &newrev) == NP_TXT_ERR_STALE_REVISION);
    np_patch_set_destroy(ps);

    /* patch 生效后内容正确：TODO 行被替换为 DONE 行 */
    char full[256];
    np_txt_read(&b, 0, 0, full, sizeof(full), &nread);
    CHECK(strstr(full, "// DONE: fixed") != NULL);
    CHECK(strstr(full, "TODO") == NULL);

    np_patch_buffer_free(&b);

    /* 注册进能力注册表 */
    np_registry_t *reg = np_registry_create();
    CHECK(np_texttools_register(reg) == 4);
    CHECK(np_registry_get(reg, "editor.text.read") != NULL);
    CHECK(np_registry_get(reg, "editor.text.patch") != NULL);
    const np_tool_desc_t *d = np_registry_get(reg, "editor.text.patch");
    CHECK(d->risk == NP_RISK_WRITE);
    CHECK(np_tool_eligible(d, 10));
    /* 路由：query "patch text" 命中 patch 工具 */
    CHECK(np_tool_relevance(d, "apply text patch") > 0);
    np_registry_destroy(reg);
    return 0;
}

int main(void) {
    /* 准备测试目录（跨平台） */
#ifdef _WIN32
    system("rmdir /S /Q .s1test_skill_src > NUL 2>&1");
    system("rmdir /S /Q .s1test_skill_bad > NUL 2>&1");
    system("rmdir /S /Q .s1test_skill_src2 > NUL 2>&1");
    system("rmdir /S /Q .s1test_store > NUL 2>&1");
    system("mkdir .s1test_skill_src");
    system("mkdir .s1test_skill_bad");
    system("mkdir .s1test_skill_src2");
#else
    system("rm -rf .s1test_skill_src .s1test_skill_bad .s1test_skill_src2 .s1test_store");
    system("mkdir -p .s1test_skill_src .s1test_skill_bad .s1test_skill_src2");
#endif
    test_manifest();
    test_install_lifecycle();
    test_texttools();
    if (failures == 0) printf("ALL SKILLMGR/TEXTTOOLS TESTS PASSED\n");
    else               printf("%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
