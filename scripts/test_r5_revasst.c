/* scripts/test_r5_revasst.c — R5 AI 逆向助手测试.
 * 上下文包 / 可疑行为 / 命名待确认 / 调用链 / AI 回调 / 只读红线 /
 * 清洗 / 渲染 / 失败路径。
 */
#include "np_revasst.h"
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

static np_disasm_insn_t mk(uint64_t addr, const char *t)
{
    np_disasm_insn_t x;
    memset(&x, 0, sizeof(x));
    x.addr = addr; x.size = 1;
    snprintf(x.text, sizeof(x.text), "%s", t);
    x.source = NP_DSRC_TEST;
    return x;
}

static size_t build_sample(np_disasm_insn_t ins[7])
{
    ins[0] = mk(0x401000, "push rbp");
    ins[1] = mk(0x401001, "call socket");
    ins[2] = mk(0x401006, "call ptrace");
    ins[3] = mk(0x40100b, "call 401020");
    ins[4] = mk(0x401010, "ret");
    ins[5] = mk(0x401020, "push rbp");
    ins[6] = mk(0x401021, "ret");
    return 7;
}

static bool ai_cb(const char *ctx, char *out, size_t cap, void *user)
{
    (void)user;
    if (!strstr(ctx, "## 指令")) return false;   /* 上下文缺指令则拒绝 */
    snprintf(out, cap, "AI: 网络初始化与反调试 [待确认]");
    return true;
}

static bool ai_fail_cb(const char *ctx, char *out, size_t cap, void *user)
{
    (void)ctx; (void)out; (void)cap; (void)user;
    return false;   /* 模拟 AI 失败，应回退本地 */
}

static bool ai_dirty_cb(const char *ctx, char *out, size_t cap, void *user)
{
    (void)ctx; (void)user;
    snprintf(out, cap, "AI\x01\x07 结果\n含控制字符");  /* 不可信输入 */
    return true;
}

static void test_sanitize(void)
{
    char out[128];
    np_ra_sanitize("abc\x01" "\x07" "\x02" "def\nghi\tjkl", out, sizeof(out));
    CHECK(strstr(out, "abcdef") != NULL);
    CHECK(strstr(out, "\n") != NULL);       /* 换行保留 */
    CHECK(strstr(out, "jkl") != NULL);
    CHECK(strchr(out, '\x01') == NULL);     /* 控制字符剥离 */
    np_ra_sanitize(NULL, out, sizeof(out));
    CHECK_STR(out, "");
}

static void test_context(void)
{
    np_disasm_insn_t ins[7];
    size_t n = build_sample(ins);
    np_cfg_graph_t g;
    char ctx[NP_RA_MAX_CTX];
    CHECK(np_cfg_build(ins, n, NP_PARCH_X64, NULL, NULL, &g) == NP_CFG_OK);
    CHECK(np_ra_build_context("sample.bin", &g, 0, ins, n,
                              ctx, sizeof(ctx)) == NP_RA_OK);
    CHECK(strstr(ctx, "# 二进制函数分析上下文") != NULL);
    CHECK(strstr(ctx, "sample.bin:0x401000") != NULL);
    CHECK(strstr(ctx, "call socket") != NULL);
    CHECK(strstr(ctx, "call ptrace") != NULL);
    CHECK(strstr(ctx, "## CFG 摘要") != NULL);
    /* 容量不足拒绝（cap<128 判 ARG） */
    CHECK(np_ra_build_context("x", &g, 0, ins, n, ctx, 32) == NP_RA_ERR_ARG);
    /* 无指令数组时给出说明 */
    CHECK(np_ra_build_context("x", &g, 0, NULL, 0, ctx, sizeof(ctx)) == NP_RA_OK);
    CHECK(strstr(ctx, "未提供指令数组") != NULL);
}

static void test_analyze_local(void)
{
    np_disasm_insn_t ins[7];
    size_t n = build_sample(ins);
    np_cfg_graph_t g;
    np_ra_result_t r;
    char md[8192];
    CHECK(np_cfg_build(ins, n, NP_PARCH_X64, NULL, NULL, &g) == NP_CFG_OK);
    CHECK(np_ra_analyze("sample.bin", &g, 0, ins, n, NULL, NULL, &r)
          == NP_RA_OK);
    CHECK(r.func_index == 0);
    CHECK(r.func_addr == 0x401000);
    CHECK_STR(r.func_name, "sub_401000");
    CHECK(!r.explanation_from_ai);
    CHECK(strstr(r.explanation, "本地启发式") != NULL);
    /* 命名一律待确认（红线） */
    CHECK(r.name.needs_confirm);
    CHECK(r.name.name[0] != '\0');
    /* 调用链：func0 → func1（call 401020） */
    CHECK(r.nchain == 1);
    CHECK(r.chain[0].func_index == 1);
    CHECK(r.chain[0].addr == 0x401020);
    CHECK(strstr(r.chain[0].ref, "0x401020") != NULL);
    CHECK(strstr(r.chain_summary, "静态调用 1") != NULL);
    /* 可疑行为：网络(socket) + 反调试(ptrace) */
    CHECK(r.nsuspects == 2);
    CHECK(r.suspects[0].kind == NP_RA_S_NETWORK);
    CHECK(r.suspects[0].addr == 0x401001);
    CHECK(strstr(r.suspects[0].ref, "0x401001") != NULL);
    CHECK(r.suspects[1].kind == NP_RA_S_ANTIDEBUG);
    CHECK(r.suspects[1].conf == 90);
    /* 渲染 */
    CHECK(np_ra_render(&r, md, sizeof(md)) > 0);
    CHECK(strstr(md, "# AI 逆向分析 · sub_401000") != NULL);
    CHECK(strstr(md, "待确认") != NULL);
    CHECK(strstr(md, "网络") != NULL);
    CHECK(strstr(md, "反调试") != NULL);
    CHECK(strstr(md, "未修改任何源字节") != NULL);
    CHECK(np_ra_render(&r, md, 32) == -1);
}

static void test_analyze_ai(void)
{
    np_disasm_insn_t ins[7];
    size_t n = build_sample(ins);
    np_cfg_graph_t g;
    np_ra_result_t r;
    CHECK(np_cfg_build(ins, n, NP_PARCH_X64, NULL, NULL, &g) == NP_CFG_OK);
    /* AI 成功 */
    CHECK(np_ra_analyze("s", &g, 0, ins, n, ai_cb, NULL, &r) == NP_RA_OK);
    CHECK(r.explanation_from_ai);
    CHECK(strstr(r.explanation, "AI: 网络初始化") != NULL);
    /* AI 失败 → 回退本地 */
    CHECK(np_ra_analyze("s", &g, 0, ins, n, ai_fail_cb, NULL, &r) == NP_RA_OK);
    CHECK(!r.explanation_from_ai);
    CHECK(strstr(r.explanation, "本地启发式") != NULL);
    /* AI 输出含控制字符 → 清洗 */
    CHECK(np_ra_analyze("s", &g, 0, ins, n, ai_dirty_cb, NULL, &r) == NP_RA_OK);
    CHECK(r.explanation_from_ai);
    CHECK(strchr(r.explanation, '\x01') == NULL);
    CHECK(strchr(r.explanation, '\x07') == NULL);
    CHECK(strstr(r.explanation, "AI") != NULL);
}

static void test_no_insns(void)
{
    np_disasm_insn_t ins[7];
    size_t n = build_sample(ins);
    np_cfg_graph_t g;
    np_ra_result_t r;
    CHECK(np_cfg_build(ins, n, NP_PARCH_X64, NULL, NULL, &g) == NP_CFG_OK);
    /* 不传指令数组：不崩溃，无可疑标记，解释降级为 CFG 摘要 */
    CHECK(np_ra_analyze("s", &g, 0, NULL, 0, NULL, NULL, &r) == NP_RA_OK);
    CHECK(r.nsuspects == 0);
    CHECK(r.name.needs_confirm);
    CHECK(strstr(r.explanation, "本地启发式") != NULL);
}

static void test_failure(void)
{
    np_disasm_insn_t ins[7];
    size_t n = build_sample(ins);
    np_cfg_graph_t g;
    np_ra_result_t r;
    char ctx[NP_RA_MAX_CTX];
    CHECK(np_cfg_build(ins, n, NP_PARCH_X64, NULL, NULL, &g) == NP_CFG_OK);
    CHECK(np_ra_analyze("s", &g, -1, ins, n, NULL, NULL, &r) == NP_RA_ERR_ARG);
    CHECK(np_ra_analyze("s", &g, 99, ins, n, NULL, NULL, &r) == NP_RA_ERR_ARG);
    CHECK(np_ra_analyze("s", &g, 0, ins, n, NULL, NULL, NULL) == NP_RA_ERR_ARG);
    CHECK(np_ra_analyze("s", NULL, 0, ins, n, NULL, NULL, &r) == NP_RA_ERR_ARG);
    CHECK(np_ra_build_context("s", &g, 0, ins, n, NULL, 100) == NP_RA_ERR_ARG);
    CHECK(np_ra_build_context("s", &g, -1, ins, n, ctx, sizeof(ctx)) == NP_RA_ERR_ARG);
    CHECK(np_ra_render(NULL, ctx, sizeof(ctx)) == -1);
    CHECK_STR(np_ra_status_name(NP_RA_OK), "OK");
    CHECK_STR(np_ra_suspect_name(NP_RA_S_CRYPTO), "加密特征");
    /* 只读红线：分析不修改指令数组 */
    {
        np_disasm_insn_t before[7];
        memcpy(before, ins, sizeof(ins));
        np_ra_analyze("s", &g, 0, ins, n, ai_cb, NULL, &r);
        CHECK(memcmp(before, ins, sizeof(ins)) == 0);
    }
}

int main(void)
{
    printf("=== R5 reverse assistant tests ===\n");
    test_sanitize();
    test_context();
    test_analyze_local();
    test_analyze_ai();
    test_no_insns();
    test_failure();
    printf("g_pass=%d g_fail=%d\n", g_pass, g_fail);
    if (g_fail) { printf("R5 TESTS FAILED\n"); return 1; }
    printf("R5 TESTS PASSED (%d assertions)\n", g_pass);
    return 0;
}
