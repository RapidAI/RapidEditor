/* test_m2_review.c — M2/B3: 统一审查会话模型单元测试. */
#include "np_review.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static int test_text_flow(void) {
    printf("== B3: 文本轨审查 ==\n");
    np_patch_buffer_t b;
    np_patch_buffer_init(&b);
    const char *src = "line0\nline1\nline2\nline3\n";
    CHECK(np_patch_buffer_set(&b, src, strlen(src)));

    /* AI 产出两个候选 hunk：替换 line1、插入新行 */
    np_patch_set_t *ps = np_patch_set_create(b.revision);
    CHECK(np_patch_add_text(ps, NP_OP_REPLACE, 1, 2, "NEW1\n", 5) == NP_PATCH_OK);
    CHECK(np_patch_add_text(ps, NP_OP_INSERT, 3, 3, "INSERTED\n", 9) == NP_PATCH_OK);

    np_review_t *rv = np_review_create(NP_TRACK_TEXT, b.revision);
    CHECK(rv != NULL);
    CHECK(np_review_import_text(rv, ps) == 2);
    CHECK(np_review_count(rv) == 2);
    CHECK(strcmp(np_review_state_name(np_review_hunk(rv, 0)->state), "pending") == 0);

    /* 只接受第一个，拒绝第二个 → 部分应用 */
    CHECK(np_review_accept(rv, 0));
    CHECK(np_review_reject(rv, 1));

    char why[160];
    CHECK(np_review_commit_text(rv, &b, why, sizeof(why)) == NP_PATCH_OK);
    CHECK(b.revision == 1);
    CHECK(strstr((const char *)b.data, "NEW1") != NULL);
    CHECK(strstr((const char *)b.data, "INSERTED") == NULL);
    CHECK(strstr((const char *)b.data, "line2") != NULL); /* 未被触碰 */

    /* stale revision 拒绝 */
    {
        np_review_t *rv2 = np_review_create(NP_TRACK_TEXT, 0); /* 旧 base */
        np_patch_set_t *ps2 = np_patch_set_create(0);
        np_patch_add_text(ps2, NP_OP_REPLACE, 0, 1, "x\n", 2);
        np_review_import_text(rv2, ps2);
        np_review_accept(rv2, 0);
        CHECK(np_review_commit_text(rv2, &b, why, sizeof(why)) == NP_PATCH_ERR_REVISION);
        np_patch_set_destroy(ps2);
        np_review_destroy(rv2);
    }

    /* 全拒绝 → nothing accepted */
    {
        np_review_t *rv3 = np_review_create(NP_TRACK_TEXT, b.revision);
        np_patch_set_t *ps3 = np_patch_set_create(b.revision);
        np_patch_add_text(ps3, NP_OP_REPLACE, 0, 1, "y\n", 2);
        np_review_import_text(rv3, ps3);
        np_review_reject(rv3, 0);
        CHECK(np_review_commit_text(rv3, &b, why, sizeof(why)) == NP_PATCH_ERR_ARG);
        CHECK(strstr(why, "nothing accepted") != NULL);
        np_patch_set_destroy(ps3);
        np_review_destroy(rv3);
    }

    np_patch_set_destroy(ps);
    np_review_destroy(rv);
    np_patch_buffer_free(&b);
    return 0;
}

static int test_byte_flow(void) {
    printf("== B3: 字节轨审查 + H2 守卫 ==\n");
    /* PNG 签名 + 数据区 */
    unsigned char img[32];
    memcpy(img, "\x89PNG\r\n\x1a\n", 8);
    memcpy(img + 8, "PAYLOAD-DATA-HERE-12345", 23);

    np_protected_t sig = np_protected_make(0, img, 8, "PNG signature");
    np_bytepatch_t *bp = np_bytepatch_create(img, sizeof(img));
    CHECK(bp != NULL);

    np_review_t *rv = np_review_create(NP_TRACK_BINARY, 0);
    CHECK(rv != NULL);

    /* 3 个候选：①改 payload（安全）②碰签名（被守卫拒绝）③插入 */
    CHECK(np_review_add_byte(rv, 8, 7, (const unsigned char *)"PATCHED", 7, &sig, 1));
    CHECK(np_review_add_byte(rv, 0, 4, (const unsigned char *)"XXXX", 4, &sig, 1));
    CHECK(np_review_add_byte(rv, 32, 0, (const unsigned char *)"!", 1, &sig, 1));

    const np_review_hunk_t *h1 = np_review_hunk(rv, 0);
    const np_review_hunk_t *h2 = np_review_hunk(rv, 1);
    CHECK(h1->guard_blocked == false);
    CHECK(h2->guard_blocked == true);
    CHECK(strstr(h2->guard_reason, "PNG signature") != NULL);

    CHECK(np_review_accept(rv, 0));
    CHECK(np_review_accept(rv, 2)); /* 插入也接受 */
    /* hunk1 保持 pending（不提交） */

    long delta = np_review_byte_delta(rv);
    CHECK(delta == -7 + 7 - 0 + 0 + 1); /* replace -7+7, insert +1 */

    char why[160];
    CHECK(np_review_commit_byte(rv, bp, why, sizeof(why)) == NP_BP_OK);
    size_t out_len = 0;
    const unsigned char *out = np_bytepatch_data(bp, &out_len);
    CHECK(out_len == 33); /* 32 -7+7 +1 */
    CHECK(memcmp(out + 8, "PATCHED", 7) == 0);   /* hunk0 生效 */
    CHECK(memcmp(out, "\x89PNG", 4) == 0);       /* 签名未动 */
    CHECK(out[32] == '!');                        /* hunk2 生效 */

    /* 守卫块 hunk 提交 → 整组回滚 */
    {
        np_review_t *rv2 = np_review_create(NP_TRACK_BINARY, 0);
        np_review_add_byte(rv2, 0, 2, (const unsigned char *)"ZZ", 2, &sig, 1);
        np_review_accept(rv2, 0);
        np_bytepatch_t *bp2 = np_bytepatch_create(out, out_len);
        size_t before = 0;
        np_bytepatch_data(bp2, &before);
        CHECK(np_review_commit_byte(rv2, bp2, why, sizeof(why)) == NP_BP_ERR_ARG);
        CHECK(strstr(why, "blocked") != NULL);
        size_t after = 0;
        np_bytepatch_data(bp2, &after);
        CHECK(after == before); /* 整组回滚 */
        np_bytepatch_destroy(bp2);
        np_review_destroy(rv2);
    }

    /* H8 gate 禁止提交 */
    {
        np_review_t *rv3 = np_review_create(NP_TRACK_BINARY, 0);
        np_review_add_byte(rv3, 10, 1, (const unsigned char *)"Q", 1, NULL, 0);
        np_review_accept(rv3, 0);
        np_review_set_gate(rv3, false, "target is a private key");
        np_bytepatch_t *bp3 = np_bytepatch_create(out, out_len);
        CHECK(np_review_commit_byte(rv3, bp3, why, sizeof(why)) == NP_BP_ERR_ARG);
        CHECK(strstr(why, "private key") != NULL);
        np_bytepatch_destroy(bp3);
        np_review_destroy(rv3);
    }

    np_bytepatch_destroy(bp);
    np_review_destroy(rv);
    return 0;
}

int main(void) {
    test_text_flow();
    test_byte_flow();
    if (failures == 0) printf("ALL REVIEW TESTS PASSED\n");
    else               printf("%d REVIEW FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
