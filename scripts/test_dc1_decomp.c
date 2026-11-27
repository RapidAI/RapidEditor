/* scripts/test_dc1_decomp.c — DC1 本地反编译后端桥接测试.
 * status/backend 名称 / 清洗 / 内置启发式 / 注入外部后端 / offset 映射 /
 * Markdown 渲染 / 失败路径。
 */
#include "np_decomp.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)
#define CHECK_STR(a, e) do { \
    const char *_a = (a); \
    if (_a && strcmp(_a, (e)) == 0) { g_pass++; } \
    else { printf("FAIL %s:%d  got '%s' expect '%s'\n", __FILE__, __LINE__, \
                  _a ? _a : "(null)", (e)); g_fail++; } \
} while (0)

static np_disasm_insn_t mk(size_t off, uint64_t addr, const char *t)
{
    np_disasm_insn_t x;
    memset(&x, 0, sizeof(x));
    x.offset = off; x.size = 1; x.addr = addr;
    snprintf(x.text, sizeof(x.text), "%s", t);
    x.source = NP_DSRC_TEST;
    return x;
}

/* 单函数样本：push/mov/cmp/je/call/jmp/mov/pop/ret */
static size_t build_sample(np_disasm_insn_t ins[9])
{
    ins[0] = mk(0, 0x401000, "push rbp");
    ins[1] = mk(1, 0x401001, "mov rbp,rsp");
    ins[2] = mk(2, 0x401004, "cmp eax,0x1");
    ins[3] = mk(3, 0x401006, "je 401010");
    ins[4] = mk(4, 0x401008, "call 401020");
    ins[5] = mk(5, 0x40100d, "jmp 401015");
    ins[6] = mk(6, 0x401010, "mov eax,0x2");
    ins[7] = mk(7, 0x401015, "pop rbp");
    ins[8] = mk(8, 0x401016, "ret");
    return 9;
}

static void test_names(void)
{
    CHECK_STR(np_dc_status_name(NP_DC_OK), "OK");
    CHECK_STR(np_dc_status_name(NP_DC_ERR_ARG), "ARG");
    CHECK_STR(np_dc_status_name(NP_DC_ERR_NOCFG), "NOCFG");
    CHECK_STR(np_dc_status_name(NP_DC_ERR_FULL), "FULL");
    CHECK_STR(np_dc_status_name(NP_DC_ERR_BACKEND), "BACKEND");
    CHECK_STR(np_dc_status_name(NP_DC_ERR_UNSUPPORTED), "UNSUPPORTED");

    CHECK_STR(np_dc_backend_name(NP_DC_BUILTIN), "builtin-heuristic");
    CHECK_STR(np_dc_backend_name(NP_DC_GHIDRA), "Ghidra-headless");
    CHECK_STR(np_dc_backend_name(NP_DC_RIZIN), "rizin");
    CHECK_STR(np_dc_backend_name(NP_DC_RETDEC), "retdec");
    CHECK_STR(np_dc_backend_name(NP_DC_CUSTOM), "custom");
}

static void test_sanitize(void)
{
    char out[32];
    np_dc_sanitize("ab\x01""cd\x7f\ne\tf", out, sizeof(out));
    CHECK_STR(out, "abcd\ne\tf");
    np_dc_sanitize("1234567890", out, 4);
    CHECK_STR(out, "123");
    np_dc_sanitize(NULL, out, sizeof(out));
    CHECK_STR(out, "");
    np_dc_sanitize("abc", NULL, 8); /* 不崩溃 */
    np_dc_sanitize("abc", out, 0);
}

static void test_builtin(void)
{
    np_disasm_insn_t ins[9];
    size_t n = build_sample(ins);
    np_cfg_graph_t g;
    np_dc_result_t r;
    char all[8192];

    CHECK(np_cfg_build(ins, n, NP_PARCH_X64, NULL, NULL, &g) == NP_CFG_OK);
    CHECK(np_dc_generate_builtin(ins, n, &g, 0, "sample.bin", &r) == NP_DC_OK);

    CHECK(r.backend == NP_DC_BUILTIN);
    CHECK(r.candidate);
    CHECK_STR(r.func_name, "sub_401000");
    CHECK_STR(r.source, "sample.bin");
    CHECK(r.func_addr == 0x401000);
    CHECK(r.conf == 70); /* 85 被保守截到 70 */
    CHECK(r.nlines == 9);

    /* 逐行 offset 映射与指令一一对应 */
    CHECK(r.lines[0].offset == 0 && r.lines[0].addr == 0x401000);
    CHECK(r.lines[1].offset == 1 && r.lines[1].addr == 0x401001);
    CHECK(r.lines[8].offset == 8 && r.lines[8].addr == 0x401016);
    CHECK(r.lines[0].line_no == 1 && r.lines[8].line_no == 9);

    /* 启发式伪代码形态 */
    CHECK(strstr(r.lines[1].text, "rbp = rsp") != NULL);
    CHECK(strstr(r.lines[1].text, "candidate") != NULL);
    CHECK(strstr(r.lines[2].text, "cmp") != NULL);
    CHECK(strstr(r.lines[3].text, "if (cond) goto") != NULL);
    CHECK(strstr(r.lines[4].text, "call") != NULL);
    CHECK(strstr(r.lines[5].text, "goto") != NULL);
    CHECK(strstr(r.lines[7].text, "stack") != NULL);
    CHECK_STR(r.lines[8].text, "return;");

    /* provenance 头 + 完整 code */
    CHECK(strstr(r.code, "候选·待确认") != NULL);
    CHECK(strstr(r.code, "内置启发式") != NULL);
    CHECK(strstr(r.code, "sample.bin") != NULL);
    CHECK(strstr(r.code, "sub_401000") != NULL);
    CHECK(strstr(r.code, "/* 0x401000 */ push rbp") != NULL);
    CHECK(strstr(r.code, "}") != NULL);

    /* 查询 */
    CHECK(np_dc_line_for_offset(&r, 4) == 5);
    CHECK(np_dc_line_for_offset(&r, 8) == 9);
    CHECK(np_dc_line_for_offset(&r, 999) == -1);
    CHECK(np_dc_offset_for_line(&r, 1) == 0);
    CHECK(np_dc_offset_for_line(&r, 9) == 8);
    CHECK(np_dc_offset_for_line(&r, 0) == (size_t)-1);
    CHECK(np_dc_offset_for_line(&r, 10) == (size_t)-1);
    CHECK(np_dc_line_for_offset(NULL, 0) == -1);
    CHECK(np_dc_offset_for_line(NULL, 1) == (size_t)-1);

    /* 渲染 */
    CHECK(np_dc_render(&r, all, sizeof(all)) > 0);
    CHECK(strstr(all, "# 反编译候选 · sub_401000") != NULL);
    CHECK(strstr(all, "Ghidra") == NULL); /* 后端标注正确 */
    CHECK(strstr(all, "builtin-heuristic") != NULL);
    CHECK(strstr(all, "候选·待确认") != NULL);
    CHECK(strstr(all, "逐行 offset 映射") != NULL);
    CHECK(strstr(all, "1 | 0x401000 | push rbp") != NULL);
    CHECK(strstr(all, "未经 DC5 等价验证") != NULL);
    CHECK(np_dc_render(&r, all, 16) == -1);
    CHECK(np_dc_render(NULL, all, sizeof(all)) == -1);
}

/* 注入外部后端：写入带控制字符的伪代码 */
typedef struct {
    int calls;
    const char *seen_path;
    const char *seen_func;
    uint64_t seen_addr;
} backend_state_t;

static bool fake_backend(const char *local_path, const char *func_name,
                         uint64_t func_addr, char *out, size_t cap, void *user)
{
    backend_state_t *st = (backend_state_t *)user;
    st->calls++;
    st->seen_path = local_path;
    st->seen_func = func_name;
    st->seen_addr = func_addr;
    snprintf(out, cap, "int foo(void) {\x01\n  return 0;\r\n}\n");
    return true;
}

static bool fail_backend(const char *p, const char *f, uint64_t a,
                         char *o, size_t c, void *u)
{
    (void)p; (void)f; (void)a; (void)o; (void)c; (void)u;
    return false;
}

static bool empty_backend(const char *p, const char *f, uint64_t a,
                          char *o, size_t c, void *u)
{
    (void)p; (void)f; (void)a; (void)u;
    if (c) o[0] = '\0';
    return true;
}

static void test_external(void)
{
    backend_state_t st;
    np_dc_result_t r;
    char md[4096];

    memset(&st, 0, sizeof(st));
    CHECK(np_dc_generate_external(NP_DC_RETDEC, "C:\\bin\\app.exe", "foo",
                                  0x401000, fake_backend, &st, "app.exe", &r)
          == NP_DC_OK);
    CHECK(st.calls == 1);
    CHECK_STR(st.seen_path, "C:\\bin\\app.exe"); /* 只传本地路径 */
    CHECK_STR(st.seen_func, "foo");
    CHECK(st.seen_addr == 0x401000);

    CHECK(r.backend == NP_DC_RETDEC);
    CHECK(r.candidate);
    CHECK(r.conf == 50);
    CHECK_STR(r.func_name, "foo");
    CHECK_STR(r.source, "app.exe");
    CHECK(r.func_addr == 0x401000);
    /* 3 行（CR 被清洗，空尾行不计） */
    CHECK(r.nlines == 3);
    CHECK_STR(r.lines[0].text, "int foo(void) {");
    CHECK_STR(r.lines[1].text, "  return 0;");
    CHECK_STR(r.lines[2].text, "}");
    CHECK(r.lines[0].offset == (size_t)-1); /* 无 R2 指令，占位 */
    CHECK(strstr(r.code, "retdec") != NULL);
    CHECK(strstr(r.code, "未经 DC5 等价验证") != NULL);
    CHECK(strstr(r.code, "\x01") == NULL); /* 控制字符已剥离 */

    CHECK(np_dc_render(&r, md, sizeof(md)) > 0);
    CHECK(strstr(md, "retdec") != NULL);
    CHECK(strstr(md, "0xffffffffffffffff") != NULL || strstr(md, "0x") != NULL);
}

static void test_failure(void)
{
    np_dc_result_t r;
    backend_state_t st;
    memset(&st, 0, sizeof(st));

    CHECK(np_dc_generate_builtin(NULL, 0, NULL, 0, "x", &r) == NP_DC_ERR_ARG);
    CHECK(np_dc_generate_builtin(NULL, 0, NULL, 0, "x", NULL) == NP_DC_ERR_ARG);

    CHECK(np_dc_generate_external(NP_DC_GHIDRA, "x", "f", 0, NULL, NULL,
                                  "x", &r) == NP_DC_ERR_ARG);
    CHECK(np_dc_generate_external(NP_DC_BUILTIN, "x", "f", 0, fake_backend,
                                  &st, "x", &r) == NP_DC_ERR_UNSUPPORTED);
    CHECK(np_dc_generate_external(NP_DC_GHIDRA, "x", "f", 0, fail_backend,
                                  NULL, "x", &r) == NP_DC_ERR_BACKEND);
    CHECK(np_dc_generate_external(NP_DC_GHIDRA, "x", "f", 0, empty_backend,
                                  NULL, "x", &r) == NP_DC_ERR_BACKEND);
}

/* 行数上限守卫：257 条指令 → 第 257 行触发 NP_DC_ERR_FULL */
static void test_full(void)
{
    np_disasm_insn_t ins[NP_DC_MAX_LINES + 1];
    np_cfg_graph_t g;
    np_dc_result_t r;
    size_t i;
    for (i = 0; i < NP_DC_MAX_LINES + 1; i++)
        ins[i] = mk(i, 0x401000 + i, "nop");
    CHECK(np_cfg_build(ins, NP_DC_MAX_LINES + 1, NP_PARCH_X64,
                       NULL, NULL, &g) == NP_CFG_OK);
    CHECK(np_dc_generate_builtin(ins, NP_DC_MAX_LINES + 1, &g, 0,
                                 "big.bin", &r) == NP_DC_ERR_FULL);
}

int main(void)
{
    printf("=== DC1 decomp tests ===\n");
    test_names();
    test_sanitize();
    test_builtin();
    test_external();
    test_failure();
    test_full();
    printf("g_pass=%d g_fail=%d\n", g_pass, g_fail);
    if (g_fail) { printf("DC1 TESTS FAILED\n"); return 1; }
    printf("DC1 TESTS PASSED (%d assertions)\n", g_pass);
    return 0;
}
