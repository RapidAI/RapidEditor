/* test_m3_composer.c — M3/B1: Composer → Job → PatchSet 闭环单测. */
#include "np_composer.h"
#include "np_review.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static bool buf_eq(const np_patch_buffer_t *b, const char *s) {
    size_t n = strlen(s);
    return b && b->len == n && memcmp(b->data, s, n) == 0;
}

static int test_text_flow(void) {
    printf("== B1: 文本意图 → Job → 审查 ==\n");
    np_composer_t *c = np_composer_create(NULL, NULL);
    CHECK(c != NULL);
    if (!c) return 1;

    const char *src = "line0\nline1\nline2\n";
    np_composer_bind(c, "demo.txt", NP_TRACK_TEXT, 7,
                     (const unsigned char *)src, strlen(src));
    CHECK(np_composer_add_text(c, 2, 2));   /* line1 */
    CHECK(np_composer_add_byte(c, 0, 5));   /* line0 的字节片段（演示跨轨芯片） */
    CHECK(np_composer_attach_count(c) == 2);
    CHECK(strstr(np_composer_attach(c, 0)->citation, "demo.txt:2") != NULL);
    CHECK(strstr(np_composer_attach(c, 1)->citation, "demo.txt:0x0-0x5") != NULL);

    char audit[1024];
    CHECK(np_composer_audit(c, audit, sizeof(audit)));
    CHECK(strstr(audit, "TRUNCATED") == NULL);
    size_t ctx_len = 0;
    CHECK(np_composer_context(c, &ctx_len) != NULL && ctx_len > 0);

    CHECK(np_composer_submit(c, "把第 2 行改成 NEW") == NP_SUBMIT_OK);
    CHECK(np_composer_state(c) == NP_COMPOSER_DONE);
    np_job_t *job = np_composer_job(c);
    CHECK(job != NULL);
    CHECK(np_job_status(job) == NP_JOB_SUCCEEDED);
    CHECK(np_job_event_count(job) >= 5);
    CHECK(np_job_progress(job) == 100);

    char tools[NP_COMPOSER_MAX_TOOLS][80];
    size_t ntools = np_composer_routed_tools(c, tools, NP_COMPOSER_MAX_TOOLS);
    CHECK(ntools >= 1);

    const np_patch_set_t *ps = np_composer_patch(c);
    CHECK(ps != NULL && np_patch_set_count(ps) == 1);
    const np_patch_hunk_t *h = np_patch_set_hunk(ps, 0);
    CHECK(h != NULL && h->op == NP_OP_REPLACE);
    CHECK(h->u.text.line_first == 1 && h->u.text.line_last == 2);
    CHECK(h->u.text.text_len == 4 && memcmp(h->u.text.text, "NEW\n", 4) == 0);

    /* 送进 B3 Review 再写入，不直接 apply（红线） */
    np_patch_buffer_t b;
    np_patch_buffer_init(&b);
    CHECK(np_patch_buffer_set(&b, src, strlen(src)));
    b.revision = 7;
    np_review_t *rv = np_review_create(NP_TRACK_TEXT, b.revision);
    np_review_set_gate(rv, true, "test allow");
    CHECK(np_review_import_text(rv, ps) == 1);
    np_review_accept(rv, 0);
    char why[160];
    CHECK(np_review_commit_text(rv, &b, why, sizeof(why)) == NP_PATCH_OK);
    CHECK(buf_eq(&b, "line0\nNEW\nline2\n"));

    /* Composer 产出的 patch_set 自身可 apply / revert（整组一次撤销） */
    CHECK(np_patch_buffer_set(&b, src, strlen(src)));
    b.revision = 7;
    CHECK(np_patch_apply((np_patch_set_t *)ps, NP_TRACK_TEXT, &b) == NP_PATCH_OK);
    CHECK(np_patch_revert((np_patch_set_t *)ps, &b));
    CHECK(buf_eq(&b, src));

    np_review_destroy(rv);
    np_patch_buffer_free(&b);
    np_composer_destroy(c);
    return 0;
}

static int test_insert_delete(void) {
    printf("== B1: 插入 / 删除意图 ==\n");
    const char *src = "line0\nline1\nline2\n";
    np_composer_t *c = np_composer_create(NULL, NULL);
    CHECK(c != NULL);
    if (!c) return 1;
    np_composer_bind(c, "demo.txt", NP_TRACK_TEXT, 0,
                     (const unsigned char *)src, strlen(src));

    CHECK(np_composer_submit(c, "在第 2 行插入 MID") == NP_SUBMIT_OK);
    const np_patch_hunk_t *h = np_patch_set_hunk(np_composer_patch(c), 0);
    CHECK(h != NULL && h->op == NP_OP_INSERT);
    CHECK(h->u.text.line_first == 1 && h->u.text.line_last == 1);
    np_patch_buffer_t b;
    np_patch_buffer_init(&b);
    np_patch_buffer_set(&b, src, strlen(src));
    CHECK(np_patch_apply((np_patch_set_t *)np_composer_patch(c), NP_TRACK_TEXT, &b) == NP_PATCH_OK);
    CHECK(buf_eq(&b, "line0\nMID\nline1\nline2\n"));
    np_patch_buffer_free(&b);

    np_composer_reset(c);
    CHECK(np_composer_state(c) == NP_COMPOSER_IDLE);
    CHECK(np_composer_job(c) == NULL && np_composer_patch(c) == NULL);
    CHECK(np_composer_submit(c, "删除第 1 行") == NP_SUBMIT_OK);
    h = np_patch_set_hunk(np_composer_patch(c), 0);
    CHECK(h != NULL && h->op == NP_OP_DELETE);
    CHECK(h->u.text.line_first == 0 && h->u.text.line_last == 1);

    np_composer_destroy(c);
    return 0;
}

static int test_byte_flow(void) {
    printf("== B1: 字节意图（轨道不混用） ==\n");
    np_composer_t *c = np_composer_create(NULL, NULL);
    CHECK(c != NULL);
    if (!c) return 1;
    const unsigned char raw[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    np_composer_bind(c, "fw.bin", NP_TRACK_BINARY, 3, raw, sizeof(raw));
    CHECK(np_composer_add_byte(c, 0, 4));
    CHECK(np_composer_submit(c, "0x1 改成 CA FE") == NP_SUBMIT_OK);
    CHECK(np_job_track(np_composer_job(c)) == NP_TRACK_BINARY);
    CHECK(np_job_kind(np_composer_job(c)) == NP_JOB_KIND_BYTE_PATCH);
    const np_patch_hunk_t *h = np_patch_set_hunk(np_composer_patch(c), 0);
    CHECK(h != NULL && h->op == NP_OP_REPLACE);
    CHECK(h->u.byte.offset == 1 && h->u.byte.len == 2 && h->u.byte.bytes_len == 2);
    CHECK(h->u.byte.bytes[0] == 0xCA && h->u.byte.bytes[1] == 0xFE);

    /* 回归：值带 0x 前缀（曾是 '0'+'x' 凑不成对 → 静默产出 0x00 单字节） */
    CHECK(np_composer_submit(c, "0x1 改成 0xCA 0xFE") == NP_SUBMIT_OK);
    h = np_patch_set_hunk(np_composer_patch(c), 0);
    CHECK(h != NULL && h->u.byte.offset == 1 && h->u.byte.bytes_len == 2);
    CHECK(h->u.byte.bytes[0] == 0xCA && h->u.byte.bytes[1] == 0xFE);

    char why[200];
    CHECK(np_composer_verify_citations(c, why, sizeof(why)));

    np_patch_buffer_t b;
    np_patch_buffer_init(&b);
    np_patch_buffer_set(&b, raw, sizeof(raw));
    b.revision = 3;
    CHECK(np_patch_apply((np_patch_set_t *)np_composer_patch(c), NP_TRACK_BINARY, &b) == NP_PATCH_OK);
    CHECK(b.len == 4 && b.data[0] == 0xDE && b.data[1] == 0xCA &&
          b.data[2] == 0xFE && b.data[3] == 0xEF);
    np_patch_buffer_free(&b);
    np_composer_destroy(c);
    return 0;
}

static int test_errors(void) {
    printf("== B1: 失败路径 ==\n");
    np_composer_t *c = np_composer_create(NULL, NULL);
    CHECK(c != NULL);
    if (!c) return 1;
    const char *src = "abc\n";
    np_composer_bind(c, "x.txt", NP_TRACK_TEXT, 0, (const unsigned char *)src, strlen(src));
    CHECK(np_composer_submit(c, "随便聊聊") == NP_SUBMIT_ERR_UNSUPPORTED);
    CHECK(np_composer_state(c) == NP_COMPOSER_FAILED);
    CHECK(strlen(np_composer_why(c)) > 0);
    CHECK(np_composer_submit(c, "删除第 99 行") == NP_SUBMIT_ERR_PATCH);
    CHECK(!np_composer_add_text(c, 0, 1));
    CHECK(!np_composer_add_byte(c, 2, 99));
    CHECK(!np_composer_add_format(c, "users", "not-a-citation"));
    np_composer_destroy(c);

    np_composer_t *empty = np_composer_create(NULL, NULL);
    CHECK(np_composer_submit(empty, "把第 1 行改成 X") == NP_SUBMIT_ERR_NO_DOC);
    np_composer_destroy(empty);
    return 0;
}

int main(void) {
    printf("== M3 / B1 Composer 单元测试 ==\n");
    int rc = 0;
    rc |= test_text_flow();
    rc |= test_insert_delete();
    rc |= test_byte_flow();
    rc |= test_errors();
    if (failures == 0) printf("M3 COMPOSER TESTS: ALL PASSED\n");
    else printf("M3 COMPOSER TESTS: %d FAILURE(S)\n", failures);
    return (failures || rc) ? 1 : 0;
}
