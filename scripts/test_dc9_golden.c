/* test_dc9_golden.c - DC9 staged equivalence gate unit tests. */
#include "np_deq.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
#define CHECK(x) do { ++checks; if (!(x)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); ++fails; } } while (0)

/* Injected checker: passes every level up to `pass_through`, fails beyond. */
typedef struct { np_deq_level_t pass_through; int calls; } fake_ctx;
static np_deq_check_t fake_check(np_deq_level_t level, const char *orig,
                                 const char *cand, void *user) {
    fake_ctx *c = (fake_ctx *)user;
    (void)orig; (void)cand;
    np_deq_check_t r; memset(&r, 0, sizeof r);
    r.level = level;
    r.passed = (int)level <= (int)c->pass_through;
    c->calls++;
    snprintf(r.detail, sizeof r.detail, "%s %s", np_deq_level_name(level),
             r.passed ? "通过" : "失败");
    return r;
}

/* Golden key points for a sample function. */
static const char *const KP[] = {
    "load argument from stack", "compare with zero", "return early if null",
    "compute length", "loop over bytes", "accumulate checksum", "return result"
};

static void test_golden(void) {
    np_deq_golden_t g = np_deq_make_golden("x64", "checksum", KP, 7);
    CHECK(strcmp(g.arch, "x64") == 0);
    CHECK(strcmp(g.func, "checksum") == 0);
    CHECK(g.nkeypoints == 7 && g.hash != 0);
    CHECK(np_deq_match_golden(&g, KP, 7));
    /* missing a key point -> mismatch */
    CHECK(!np_deq_match_golden(&g, KP, 3));
    /* extra points are fine as long as all expected appear */
    const char *const extra[] = { "prologue", "load argument from stack", "compare with zero",
        "return early if null", "compute length", "loop over bytes", "accumulate checksum",
        "return result", "epilogue" };
    CHECK(np_deq_match_golden(&g, extra, 9));
    /* wrong text -> mismatch */
    const char *const wrong[] = { "totally different", "nothing relevant" };
    CHECK(!np_deq_match_golden(&g, wrong, 2));
}

static void test_equivalent(void) {
    np_deq_golden_t g = np_deq_make_golden("x64", "checksum", KP, 7);
    fake_ctx c = { NP_DEQ_V3_HUMAN, 0 };
    np_deq_result_t r = np_deq_evaluate(NP_DEQ_V3_HUMAN, "orig bytes",
        "candidate source", fake_check, &c, &g, KP, 7, "");
    CHECK(c.calls == 4);                       /* V0..V3 all run */
    CHECK(r.verdict == NP_DEQ_EQUIVALENT);
    CHECK(r.achieved_level == NP_DEQ_V3_HUMAN);
    CHECK(r.golden_matched);
    CHECK(r.nchecks == 4);
    char sum[256]; np_deq_summary(&r, sum, sizeof sum);
    CHECK(strstr(sum, "等价") != NULL);
}

static void test_fail_closed(void) {
    np_deq_golden_t g = np_deq_make_golden("arm", "checksum", KP, 7);
    /* V1 recompile fails -> NOT_EQUIVALENT, stop at V1 (fail-closed). */
    fake_ctx c = { NP_DEQ_V0_REASSEMBLY, 0 };
    np_deq_result_t r = np_deq_evaluate(NP_DEQ_V3_HUMAN, "orig", "cand",
        fake_check, &c, &g, KP, 7, "known gap A");
    CHECK(c.calls == 2);                       /* V0 pass, V1 fail, stop */
    CHECK(r.verdict == NP_DEQ_NOT_EQUIVALENT);
    CHECK(r.achieved_level == NP_DEQ_V0_REASSEMBLY);
    CHECK(r.nchecks == 2);
    CHECK(strstr(r.known_gaps, "known gap A") != NULL);
    CHECK(strstr(r.known_gaps, "未通过") != NULL);
}

static void test_no_golden_never_equivalent(void) {
    fake_ctx c = { NP_DEQ_V3_HUMAN, 0 };
    /* All checks pass but no golden pin -> UNVERIFIED, never "equivalent". */
    np_deq_result_t r = np_deq_evaluate(NP_DEQ_V2_BEHAVIOUR, "orig", "cand",
        fake_check, &c, NULL, KP, 7, "");
    CHECK(c.calls == 3);
    CHECK(r.verdict == NP_DEQ_UNVERIFIED);
    CHECK(!r.golden_matched);
    CHECK(strstr(r.known_gaps, "golden") != NULL);
}

static void test_golden_mismatch_blocks(void) {
    np_deq_golden_t g = np_deq_make_golden("x64", "checksum", KP, 7);
    const char *const partial[] = { "load argument from stack" };
    fake_ctx c = { NP_DEQ_V3_HUMAN, 0 };
    np_deq_result_t r = np_deq_evaluate(NP_DEQ_V0_REASSEMBLY, "orig", "cand",
        fake_check, &c, &g, partial, 1, "");
    CHECK(r.verdict == NP_DEQ_UNVERIFIED);     /* check passed but golden missing */
    CHECK(!r.golden_matched);
}

static void test_no_verifier(void) {
    np_deq_golden_t g = np_deq_make_golden("x64", "f", KP, 2);
    np_deq_result_t r = np_deq_evaluate(NP_DEQ_V3_HUMAN, "orig", "cand",
        NULL, NULL, &g, KP, 2, "");
    CHECK(r.verdict == NP_DEQ_UNVERIFIED);
    CHECK(r.nchecks == 0);
    CHECK(strstr(r.known_gaps, "无注入验证器") != NULL);
    r = np_deq_evaluate(NP_DEQ_V0_REASSEMBLY, NULL, "cand", fake_check, NULL, &g, KP, 2, "");
    CHECK(r.verdict == NP_DEQ_UNVERIFIED);
}

static void test_arch_goldens(void) {
    const char *arches[] = { "x64", "arm", "aarch64", "mips", "loongarch" };
    for (size_t i = 0; i < 5; ++i) {
        np_deq_golden_t g = np_deq_make_golden(arches[i], "calc", KP, 4);
        CHECK(g.hash != 0);
        CHECK(np_deq_match_golden(&g, KP, 4));
    }
}

int main(void) {
    test_golden();
    test_equivalent();
    test_fail_closed();
    test_no_golden_never_equivalent();
    test_golden_mismatch_blocks();
    test_no_verifier();
    test_arch_goldens();
    printf("test_dc9_golden: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
