/* scripts/test_r6_patchimpact.c — R6 补丁影响分析测试.
 * REPLACE 直接改写 / INSERT 边界+位移 / DELETE 直接+位移 / 函数块归属 /
 * H2 完整性 / 渲染 / 失败路径。
 */
#include "np_patchimpact.h"
#include "np_bytediff.h"
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

static void init_diff(np_bd_result_t *d, np_bd_hunk_t *h,
                      np_bd_kind_t kind, size_t off, size_t blen, size_t alen)
{
    memset(d, 0, sizeof(*d));
    memset(h, 0, sizeof(*h));
    h->kind = kind;
    h->before_off = off; h->before_len = blen;
    h->after_off = off; h->after_len = alen;
    d->hunks = h; d->nhunks = 1;
    d->before_len = 10; d->after_len = 10 - blen + alen;
}

static void test_replace_direct(void)
{
    unsigned char before[16] = {0x55,0x48,0x89,0xe5,0x90,0x5d,0xc3};
    unsigned char after[16]  = {0x55,0x48,0x89,0xe5,0xeb,0x5d,0xc3};
    np_bd_result_t d;
    np_disasm_insn_t ins[3];
    np_cfg_graph_t g;
    np_pi_result_t r;
    np_pi_status_t st;
    char md[8192];
    /* 用真实 H7 compute 生成 diff（手工 hunk 的 preview 缓冲不足以回放） */
    CHECK(np_bytediff_compute(before, 7, after, 7, &d));
    ins[0] = mk(0, 0x1000, "push rbp");
    ins[1] = mk(1, 0x1001, "mov rbp,rsp");
    ins[2] = mk(4, 0x1004, "nop");
    np_cfg_build(ins, 3, NP_PARCH_X64, NULL, NULL, &g);
    st = np_pi_analyze(before, 7, after, 7, &d, ins, 3, NULL, 0,
                       &g, NULL, "bin_old", "bin_new", &r);
    CHECK(st == NP_PI_OK);
    CHECK(r.naffected == 1);
    CHECK(r.affected[0].level == NP_PI_DIRECT);
    CHECK(r.affected[0].insn_index == 2);
    CHECK(r.affected[0].before_addr == 0x1004);
    CHECK(r.affected[0].byte_off == 4);
    CHECK(r.affected[0].func_index >= 0);   /* CFG 归属 */
    CHECK(r.affected[0].block_id != (size_t)-1);
    CHECK_STR(r.affected[0].label, "nop");
    CHECK(r.nfuncs_hit == 1);
    CHECK(r.nblocks_hit == 1);
    CHECK(r.nhunks == 1);
    /* H2 摘要 */
    CHECK(r.digest.crc_before != r.digest.crc_after);
    CHECK(strlen(r.digest.sha_before) == 64);
    CHECK(strlen(r.digest.sha_after) == 64);
    CHECK(r.digest.verified);
    CHECK_STR(r.source_before, "bin_old");
    CHECK_STR(r.source_after, "bin_new");
    CHECK(np_pi_render(&r, md, sizeof(md)) > 0);
    CHECK(strstr(md, "# 补丁影响分析") != NULL);
    CHECK(strstr(md, "bin_old → bin_new") != NULL);
    CHECK(strstr(md, "直接改写") != NULL);
    CHECK(strstr(md, "CRC32=") != NULL);
    CHECK(strstr(md, "回放校验=通过") != NULL);
    CHECK(np_pi_render(&r, md, 32) == -1);
    np_bytediff_free(&d);
}

static void test_insert_shift(void)
{
    unsigned char before[16] = {0}, after[16] = {0};
    np_bd_result_t d; np_bd_hunk_t h;
    np_disasm_insn_t ins[3];
    np_pi_result_t r;
    init_diff(&d, &h, NP_BD_INSERT, 1, 0, 1);
    ins[0] = mk(0, 0x1000, "push rbp");
    ins[1] = mk(1, 0x1001, "nop");
    ins[2] = mk(2, 0x1002, "ret");
    CHECK(np_pi_analyze(before, 10, after, 11, &d, ins, 3, NULL, 0,
                        NULL, NULL, "b", "a", &r) == NP_PI_OK);
    CHECK(r.naffected == 2);
    /* 插入点 insn1=BOUNDARY；后续 insn2=SHIFTED */
    CHECK(r.affected[0].level == NP_PI_BOUNDARY);
    CHECK(r.affected[0].insn_index == 1);
    CHECK(r.affected[1].level == NP_PI_SHIFTED);
    CHECK(r.affected[1].insn_index == 2);
    CHECK(r.affected[0].func_index == -1);  /* 无 CFG → 未知 */
}

static void test_delete_shift(void)
{
    unsigned char before[16] = {0}, after[16] = {0};
    np_bd_result_t d; np_bd_hunk_t h;
    np_disasm_insn_t ins[3];
    np_pi_result_t r;
    init_diff(&d, &h, NP_BD_DELETE, 1, 1, 0);
    ins[0] = mk(0, 0x1000, "push rbp");
    ins[1] = mk(1, 0x1001, "nop");
    ins[2] = mk(2, 0x1002, "ret");
    CHECK(np_pi_analyze(before, 10, after, 9, &d, ins, 3, NULL, 0,
                        NULL, NULL, "b", "a", &r) == NP_PI_OK);
    CHECK(r.naffected == 2);
    CHECK(r.affected[0].level == NP_PI_DIRECT);   /* 被删字节覆盖的指令 */
    CHECK(r.affected[0].insn_index == 1);
    CHECK(r.affected[1].level == NP_PI_SHIFTED);  /* 后续位移 */
    CHECK(r.affected[1].insn_index == 2);
}

static void test_no_insns(void)
{
    unsigned char before[8] = {0}, after[8] = {0};
    np_bd_result_t d; np_bd_hunk_t h;
    np_pi_result_t r;
    init_diff(&d, &h, NP_BD_REPLACE, 0, 1, 1);
    /* 无指令数组：不崩溃，无受影响指令，但 digest 正常 */
    CHECK(np_pi_analyze(before, 8, after, 8, &d, NULL, 0, NULL, 0,
                        NULL, NULL, "b", "a", &r) == NP_PI_OK);
    CHECK(r.naffected == 0);
    CHECK(strlen(r.digest.sha_before) == 64);
}

static void test_dedup_and_upper_bound(void)
{
    unsigned char before[16] = {0}, after[16] = {0};
    np_bd_result_t d; np_bd_hunk_t hs[2];
    np_disasm_insn_t ins[2];
    np_pi_result_t r;
    memset(&d, 0, sizeof(d)); memset(hs, 0, sizeof(hs));
    /* 两个 hunk 覆盖同一指令 → 去重（同 index+level 只记一次） */
    hs[0].kind = NP_BD_REPLACE; hs[0].before_off = 0; hs[0].before_len = 1; hs[0].after_len = 1;
    hs[1].kind = NP_BD_REPLACE; hs[1].before_off = 0; hs[1].before_len = 1; hs[1].after_len = 1;
    d.hunks = hs; d.nhunks = 2; d.before_len = 4; d.after_len = 4;
    ins[0] = mk(0, 0x1000, "nop"); ins[1] = mk(1, 0x1001, "ret");
    CHECK(np_pi_analyze(before, 4, after, 4, &d, ins, 2, NULL, 0,
                        NULL, NULL, "b", "a", &r) == NP_PI_OK);
    CHECK(r.naffected == 1);   /* 去重生效 */
}

static void test_failure(void)
{
    unsigned char b[8] = {0}, a[8] = {0};
    np_bd_result_t d; np_bd_hunk_t h;
    np_pi_result_t r;
    init_diff(&d, &h, NP_BD_REPLACE, 0, 1, 1);
    CHECK(np_pi_analyze(NULL, 8, a, 8, &d, NULL, 0, NULL, 0, NULL, NULL, "b", "a", &r)
          == NP_PI_ERR_ARG);
    CHECK(np_pi_analyze(b, 8, a, 8, NULL, NULL, 0, NULL, 0, NULL, NULL, "b", "a", &r)
          == NP_PI_ERR_ARG);
    CHECK(np_pi_analyze(b, 8, a, 8, &d, NULL, 0, NULL, 0, NULL, NULL, "b", "a", NULL)
          == NP_PI_ERR_ARG);
    d.nhunks = 0;
    CHECK(np_pi_analyze(b, 8, a, 8, &d, NULL, 0, NULL, 0, NULL, NULL, "b", "a", &r)
          == NP_PI_ERR_NODIFF);
    CHECK_STR(np_pi_status_name(NP_PI_OK), "OK");
    CHECK_STR(np_pi_level_name(NP_PI_SHIFTED), "位移失效");
    /* 只读红线：分析不改 before/after */
    {
        unsigned char bb[8] = {1,2,3,4,5,6,7,8}, aa[8] = {1,2,3,9,5,6,7,8};
        unsigned char bb0[8], aa0[8];
        memcpy(bb0, bb, 8); memcpy(aa0, aa, 8);
        d.nhunks = 1;
        np_pi_analyze(bb, 8, aa, 8, &d, NULL, 0, NULL, 0, NULL, NULL, "b", "a", &r);
        CHECK(memcmp(bb0, bb, 8) == 0 && memcmp(aa0, aa, 8) == 0);
    }
}

int main(void)
{
    printf("=== R6 patch impact tests ===\n");
    test_replace_direct();
    test_insert_shift();
    test_delete_shift();
    test_no_insns();
    test_dedup_and_upper_bound();
    test_failure();
    printf("g_pass=%d g_fail=%d\n", g_pass, g_fail);
    if (g_fail) { printf("R6 TESTS FAILED\n"); return 1; }
    printf("R6 TESTS PASSED (%d assertions)\n", g_pass);
    return 0;
}
