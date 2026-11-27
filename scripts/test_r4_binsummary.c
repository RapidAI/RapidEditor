/* scripts/test_r4_binsummary.c — R4 符号/字符串/导入表摘要测试.
 * ELF64 符号/导入 / PE64 导入目录 / 字符串 / AI 回调 / 只读 / 渲染 / 失败路径。
 */
#include "np_binsummary.h"
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

static int find_sym(const np_bs_result_t *r, const char *name)
{
    int i;
    for (i = 0; i < r->nsymbols; i++)
        if (strcmp(r->symbols[i].name, name) == 0) return i;
    return -1;
}

static int find_import(const np_bs_result_t *r, const char *name)
{
    int i;
    for (i = 0; i < r->nimports; i++)
        if (strcmp(r->imports[i].name, name) == 0) return i;
    return -1;
}

static bool ai_cb(const char *digest, char *out, size_t cap, void *user)
{
    (void)user;
    if (!strstr(digest, "## 符号")) return false;
    snprintf(out, cap, "AI: 疑似标准库封装 [待确认]");
    return true;
}

static bool ai_fail_cb(const char *d, char *o, size_t c, void *u)
{
    (void)d; (void)o; (void)c; (void)u;
    return false;
}

static void test_elf64(void)
{
    static np_bs_result_t r;
    char md[NP_BS_MAX_OUT];
    int idx;
    CHECK(np_bs_parse_file("tests/golden/elf64_sample.bin", &r) == NP_BS_OK);
    CHECK(r.format == NP_BS_ELF);
    CHECK(r.arch == NP_PARCH_X64);
    CHECK(r.bits == 64);
    CHECK(r.nsymbols == 4);
    CHECK(r.nimports == 2);
    /* 符号：2 undefined(导入) + 2 defined */
    idx = find_sym(&r, "memcpy");
    CHECK(idx >= 0);
    CHECK(r.symbols[idx].undefined);
    CHECK(r.symbols[idx].is_func);
    CHECK(strstr(r.symbols[idx].ref, "elf64_sample.bin:0x") != NULL);
    idx = find_sym(&r, "printf");
    CHECK(idx >= 0 && r.symbols[idx].undefined);
    idx = find_sym(&r, "my_function");
    CHECK(idx >= 0 && !r.symbols[idx].undefined);
    CHECK(r.symbols[idx].value == 0x401000);
    idx = find_sym(&r, "global_var");
    CHECK(idx >= 0 && !r.symbols[idx].is_func);
    CHECK(r.symbols[idx].value == 0x601000);
    /* 导入 */
    CHECK(find_import(&r, "memcpy") >= 0);
    CHECK(find_import(&r, "printf") >= 0);
    CHECK(strstr(r.imports[0].lib, "ELF:") != NULL);
    /* 字符串（含符号名） */
    CHECK(r.nstrings > 0);
    {
        int found = 0;
        int i;
        for (i = 0; i < r.nstrings; i++)
            if (strstr(r.strings[i].text, "my_function")) found = 1;
        CHECK(found);
    }
    /* 渲染 */
    CHECK(np_bs_render(&r, md, sizeof(md)) > 0);
    CHECK(strstr(md, "# 二进制摘要报告") != NULL);
    CHECK(strstr(md, "ELF") != NULL);
    CHECK(strstr(md, "memcpy") != NULL);
    CHECK(strstr(md, "导入函数") != NULL);
    CHECK(strstr(md, "可打印字符串") != NULL);
    CHECK(strstr(md, "offset 引用") != NULL);
    CHECK(np_bs_render(&r, md, 32) == -1);
}

static void test_pe64(void)
{
    static np_bs_result_t r;
    char md[NP_BS_MAX_OUT];
    CHECK(np_bs_parse_file("tests/golden/pe64_sample.bin", &r) == NP_BS_OK);
    CHECK(r.format == NP_BS_PE);
    CHECK(r.arch == NP_PARCH_X64);
    CHECK(r.bits == 64);
    CHECK(r.nimports > 0);
    CHECK(r.ndlls > 0);
    /* 真实 MinGW PE 应含 KERNEL32.dll 导入 */
    CHECK(find_import(&r, "GetProcAddress") >= 0 ||
          find_import(&r, "VirtualProtect") >= 0);
    {
        int i, has_k32 = 0;
        for (i = 0; i < r.nimports; i++)
            if (strstr(r.imports[i].lib, "KERNEL32") ||
                strstr(r.imports[i].lib, "api-ms-win")) has_k32 = 1;
        CHECK(has_k32);
        CHECK(strstr(r.imports[0].ref, "pe64_sample.bin:0x") != NULL);
    }
    CHECK(np_bs_render(&r, md, sizeof(md)) > 0);
    CHECK(strstr(md, "PE") != NULL);
    CHECK(strstr(md, "导入函数") != NULL);
}

static void test_digest_and_ai(void)
{
    static np_bs_result_t r;
    char dg[NP_BS_MAX_OUT];
    CHECK(np_bs_parse_file("tests/golden/elf64_sample.bin", &r) == NP_BS_OK);
    CHECK(np_bs_digest(&r, dg, sizeof(dg)) > 0);
    CHECK(strstr(dg, "memcpy") != NULL);
    CHECK(np_bs_digest(&r, dg, 16) == -1);
    /* AI 回调成功 */
    CHECK(np_bs_ai_classify(&r, ai_cb, NULL) == NP_BS_OK);
    CHECK(r.ai_used);
    CHECK_STR(r.ai_summary, "AI: 疑似标准库封装 [待确认]");
    /* 渲染含 AI 段 */
    {
        char md[NP_BS_MAX_OUT];
        np_bs_render(&r, md, sizeof(md));
        CHECK(strstr(md, "AI 分类摘要") != NULL);
    }
    /* AI 回调失败 → 不使用，但已有结果保留（只读语义） */
    {
        static np_bs_result_t r2;
        np_bs_parse_file("tests/golden/elf64_sample.bin", &r2);
        CHECK(np_bs_ai_classify(&r2, ai_fail_cb, NULL) == NP_BS_ERR_PARSE);
        CHECK(!r2.ai_used);
    }
}

static void test_strings(void)
{
    static np_bs_result_t r;
    CHECK(np_bs_parse_file("tests/golden/elf64_sample.bin", &r) == NP_BS_OK);
    /* 每条字符串带 offset 引用，长度>=4 */
    {
        int i;
        for (i = 0; i < r.nstrings; i++) {
            CHECK(r.strings[i].len >= NP_BS_MIN_STRLEN);
            CHECK(strstr(r.strings[i].ref, ":0x") != NULL);
        }
    }
}

static void test_failure(void)
{
    static np_bs_result_t r;
    unsigned char fake[64];
    CHECK(np_bs_parse(NULL, 64, "x", &r) == NP_BS_ERR_ARG);
    CHECK(np_bs_parse(fake, 4, "x", &r) == NP_BS_ERR_ARG);
    memset(fake, 0, sizeof(fake));
    CHECK(np_bs_parse(fake, sizeof(fake), "x", &r) == NP_BS_ERR_NOTPROG);
    CHECK(np_bs_parse_file("tests/golden/no-such.bin", &r) == NP_BS_ERR_ARG);
    CHECK(np_bs_parse_file("tests/golden/golden_full.db", &r) == NP_BS_ERR_NOTPROG);
    CHECK(np_bs_ai_classify(NULL, ai_cb, NULL) == NP_BS_ERR_ARG);
    CHECK(np_bs_ai_classify(&r, NULL, NULL) == NP_BS_ERR_ARG);
    CHECK(np_bs_render(NULL, (char *)fake, sizeof(fake)) == -1);
    CHECK_STR(np_bs_status_name(NP_BS_OK), "OK");
    CHECK_STR(np_bs_format_name(NP_BS_PE), "PE");
    /* 只读红线：解析不改源字节（用内存缓冲比对） */
    {
        FILE *f = fopen("tests/golden/elf64_sample.bin", "rb");
        unsigned char buf[1024], before[1024];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        memcpy(before, buf, n);
        np_bs_parse(buf, n, "x", &r);
        CHECK(memcmp(before, buf, n) == 0);
    }
}

int main(void)
{
    printf("=== R4 binary summary tests ===\n");
    test_elf64();
    test_pe64();
    test_digest_and_ai();
    test_strings();
    test_failure();
    printf("g_pass=%d g_fail=%d\n", g_pass, g_fail);
    if (g_fail) { printf("R4 TESTS FAILED\n"); return 1; }
    printf("R4 TESTS PASSED (%d assertions)\n", g_pass);
    return 0;
}
