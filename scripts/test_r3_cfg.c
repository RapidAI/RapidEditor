/* scripts/test_r3_cfg.c — R3 函数边界与控制流图测试.
 * 分类器 / 函数边界 / 基本块 / CFG 边 / 导航 / 渲染 / 注入分类器 / 失败路径。
 */
#include "np_cfg.h"
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

static np_disasm_insn_t mk(size_t off, uint64_t addr, const char *t)
{
    np_disasm_insn_t x;
    memset(&x, 0, sizeof(x));
    x.offset = off; x.size = 1; x.addr = addr;
    snprintf(x.text, sizeof(x.text), "%s", t);
    x.source = NP_DSRC_TEST;
    return x;
}

static void test_classifier(void)
{
    np_cfg_itype_t t;
    np_disasm_insn_t x = mk(0, 0x1000, "call 401000 <foo>");
    t = np_cfg_classify_x86(&x, NULL);
    CHECK(t.cls == NP_IC_CALL); CHECK(t.target_valid); CHECK(t.target == 0x401000);

    x = mk(0, 0x1000, "jmp 0x401000");
    t = np_cfg_classify_x86(&x, NULL);
    CHECK(t.cls == NP_IC_JMP); CHECK(t.target_valid); CHECK(t.target == 0x401000);

    x = mk(0, 0x1000, "je 401010");
    t = np_cfg_classify_x86(&x, NULL);
    CHECK(t.cls == NP_IC_JCC); CHECK(t.target == 0x401010);

    x = mk(0, 0x1000, "loop 401000");
    t = np_cfg_classify_x86(&x, NULL);
    CHECK(t.cls == NP_IC_JCC);

    x = mk(0, 0x1000, "ret");
    t = np_cfg_classify_x86(&x, NULL);
    CHECK(t.cls == NP_IC_RET); CHECK(!t.target_valid);

    x = mk(0, 0x1000, "hlt");
    t = np_cfg_classify_x86(&x, NULL);
    CHECK(t.cls == NP_IC_RET);

    x = mk(0, 0x1000, "push rbp");
    t = np_cfg_classify_x86(&x, NULL);
    CHECK(t.cls == NP_IC_PROLOGUE);

    x = mk(0, 0x1000, "endbr64");
    t = np_cfg_classify_x86(&x, NULL);
    CHECK(t.cls == NP_IC_PROLOGUE);

    x = mk(0, 0x1000, "nop");
    t = np_cfg_classify_x86(&x, NULL);
    CHECK(t.cls == NP_IC_NOP);

    x = mk(0, 0x1000, "mov eax,1");
    t = np_cfg_classify_x86(&x, NULL);
    CHECK(t.cls == NP_IC_OTHER);

    x = mk(0, 0x1000, "call foo_bar");
    t = np_cfg_classify_x86(&x, NULL);
    CHECK(t.cls == NP_IC_CALL); CHECK(!t.target_valid);  /* 符号名非地址 */
}

/* 构造一个含两函数的指令序列（与探针同构） */
static size_t build_sample(np_disasm_insn_t ins[12])
{
    ins[0]  = mk(0,  0x401000, "push rbp");
    ins[1]  = mk(1,  0x401001, "mov rbp,rsp");
    ins[2]  = mk(2,  0x401004, "cmp eax,0x1");
    ins[3]  = mk(3,  0x401006, "je 401010");
    ins[4]  = mk(4,  0x401008, "call 401020");
    ins[5]  = mk(5,  0x40100d, "jmp 401015");
    ins[6]  = mk(6,  0x401010, "mov eax,0x2");
    ins[7]  = mk(7,  0x401015, "pop rbp");
    ins[8]  = mk(8,  0x401016, "ret");
    ins[9]  = mk(9,  0x401020, "push rbp");
    ins[10] = mk(10, 0x401021, "nop");
    ins[11] = mk(11, 0x401022, "ret");
    return 12;
}

static void test_func_boundaries(void)
{
    np_disasm_insn_t ins[12];
    size_t n = build_sample(ins);
    np_cfg_graph_t g;
    CHECK(np_cfg_build(ins, n, NP_PARCH_X64, NULL, NULL, &g) == NP_CFG_OK);
    /* jmp/jcc 目标不应被误判为独立函数（探针曾暴露的缺陷） */
    CHECK(g.nfuncs == 2);
    CHECK(g.funcs[0].start_addr == 0x401000);
    CHECK(g.funcs[0].end_addr == 0x401016);
    CHECK(g.funcs[0].start_insn == 0 && g.funcs[0].end_insn == 8);
    CHECK(g.funcs[0].conf == 85);            /* prologue */
    CHECK(g.funcs[1].start_addr == 0x401020);/* call 目标 */
    CHECK(g.funcs[1].conf == 95);
    CHECK(g.nblocks == 6);
    CHECK(g.nedges == 8);
    CHECK(!g.truncated);
    CHECK(g.arch == NP_PARCH_X64);
    CHECK(g.ninstructions == 12);
    CHECK_STR(g.funcs[0].name, "sub_401000");
}

static void test_blocks_edges(void)
{
    np_disasm_insn_t ins[12];
    size_t n = build_sample(ins);
    np_cfg_graph_t g;
    int i, found_jcc = 0, found_call = 0, found_ret = 0, found_jmp = 0;
    CHECK(np_cfg_build(ins, n, NP_PARCH_X64, NULL, NULL, &g) == NP_CFG_OK);
    /* B0 含 4 条指令：push/mov/cmp/je（je 是块尾） */
    CHECK(g.blocks[0].start_insn == 0 && g.blocks[0].end_insn == 3);
    CHECK(g.blocks[0].func_index == 0);
    /* B3 是 je 的目标块 0x401010，仍属函数 0 */
    CHECK(g.blocks[3].start_addr == 0x401010);
    CHECK(g.blocks[3].func_index == 0);
    /* 边类型齐全 */
    for (i = 0; i < g.nedges; i++) {
        if (g.edges[i].kind == NP_CFG_E_JCC_TAKEN) {
            found_jcc++;
            CHECK(g.edges[i].target_addr == 0x401010);
            CHECK(g.edges[i].to == 3);
        }
        if (g.edges[i].kind == NP_CFG_E_CALL) {
            found_call++;
            CHECK(g.edges[i].target_addr == 0x401020);
            CHECK(g.edges[i].to == 5);
        }
        if (g.edges[i].kind == NP_CFG_E_JMP) {
            found_jmp++;
            CHECK(g.edges[i].target_addr == 0x401015);
        }
        if (g.edges[i].kind == NP_CFG_E_RET) {
            found_ret++;
            CHECK(g.edges[i].to == (size_t)-1);
        }
    }
    CHECK(found_jcc == 1); CHECK(found_call == 1);
    CHECK(found_jmp == 1); CHECK(found_ret == 2); /* 两函数各一个 ret */
}

static void test_navigation(void)
{
    np_disasm_insn_t ins[12];
    size_t n = build_sample(ins);
    np_cfg_graph_t g;
    CHECK(np_cfg_build(ins, n, NP_PARCH_X64, NULL, NULL, &g) == NP_CFG_OK);
    /* insn 3（je）在 B0；insn 6 在 B3；addr 0x401010 在 B3 */
    CHECK(np_cfg_block_for_insn(&g, 3) == 0);
    CHECK(np_cfg_block_for_insn(&g, 6) == 3);
    CHECK(np_cfg_block_for_insn(&g, 10) == 5);
    CHECK(np_cfg_block_for_insn(&g, 999) == (size_t)-1);
    CHECK(np_cfg_block_for_addr(&g, 0x401010) == 3);
    CHECK(np_cfg_block_for_addr(&g, 0x999999) == (size_t)-1);
    CHECK(np_cfg_func_for_insn(&g, 3) == 0);
    CHECK(np_cfg_func_for_insn(&g, 10) == 1);
    CHECK(np_cfg_func_for_insn(&g, 999) == -1);
}

static void test_render(void)
{
    np_disasm_insn_t ins[12];
    size_t n = build_sample(ins);
    np_cfg_graph_t g;
    char md[4096], sum[2048];
    CHECK(np_cfg_build(ins, n, NP_PARCH_X64, NULL, NULL, &g) == NP_CFG_OK);
    CHECK(np_cfg_render_mermaid(&g, md, sizeof(md)) > 0);
    CHECK(strstr(md, "flowchart TD") != NULL);
    CHECK(strstr(md, "B0[") != NULL);
    CHECK(strstr(md, "CALL") != NULL);
    CHECK(strstr(md, "RET") != NULL);
    CHECK(strstr(md, "启发式 CFG") != NULL);
    CHECK(np_cfg_render_mermaid(&g, md, 32) == -1);

    CHECK(np_cfg_render_summary(&g, sum, sizeof(sum)) > 0);
    CHECK(strstr(sum, "函数边界与 CFG") != NULL);
    CHECK(strstr(sum, "sub_401000") != NULL);
    CHECK(strstr(sum, "置信度 85") != NULL);
    CHECK(np_cfg_render_summary(&g, sum, 16) == -1);
}

/* 注入分类器：把所有指令都归为 OTHER（除了 ret），验证可插拔 */
static np_cfg_itype_t classify_all_other(const np_disasm_insn_t *ins, void *user)
{
    np_cfg_itype_t t;
    (void)user;
    memset(&t, 0, sizeof(t));
    t.cls = strstr(ins->text, "ret") ? NP_IC_RET : NP_IC_OTHER;
    return t;
}

static void test_injected(void)
{
    np_disasm_insn_t ins[12];
    size_t n = build_sample(ins);
    np_cfg_graph_t g;
    CHECK(np_cfg_build(ins, n, NP_PARCH_X64, classify_all_other, NULL, &g)
          == NP_CFG_OK);
    /* 无 prologue/call 目标 → 只有一个起点（指令0）→ 1 函数；
     * ret（insn 8）后产生 leader insn 9 → 2 块 */
    CHECK(g.nfuncs == 1);
    CHECK(g.funcs[0].start_insn == 0);
    CHECK(g.funcs[0].end_insn == 11);
    CHECK(g.nblocks == 2);
}

static void test_failure(void)
{
    np_disasm_insn_t ins[2];
    np_cfg_graph_t g;
    ins[0] = mk(0, 0x1000, "ret");
    ins[1] = mk(1, 0x1001, "ret");
    CHECK(np_cfg_build(NULL, 2, NP_PARCH_X64, NULL, NULL, &g) == NP_CFG_ERR_ARG);
    CHECK(np_cfg_build(ins, 0, NP_PARCH_X64, NULL, NULL, &g) == NP_CFG_ERR_ARG);
    CHECK(np_cfg_build(ins, 2, NP_PARCH_X64, NULL, NULL, NULL) == NP_CFG_ERR_ARG);
    /* ARM 无注入分类器 → 拒绝（不猜测） */
    CHECK(np_cfg_build(ins, 2, NP_PARCH_ARM, NULL, NULL, &g) == NP_CFG_ERR_UNSUPPORTED);
    /* ARM 有注入分类器 → 成功 */
    CHECK(np_cfg_build(ins, 2, NP_PARCH_ARM, classify_all_other, NULL, &g) == NP_CFG_OK);
    CHECK_STR(np_cfg_status_name(NP_CFG_OK), "OK");
    CHECK_STR(np_cfg_status_name(NP_CFG_ERR_UNSUPPORTED), "UNSUPPORTED");
}

int main(void)
{
    printf("=== R3 CFG tests ===\n");
    test_classifier();
    test_func_boundaries();
    test_blocks_edges();
    test_navigation();
    test_render();
    test_injected();
    test_failure();
    printf("g_pass=%d g_fail=%d\n", g_pass, g_fail);
    if (g_fail) { printf("R3 TESTS FAILED\n"); return 1; }
    printf("R3 TESTS PASSED (%d assertions)\n", g_pass);
    return 0;
}
