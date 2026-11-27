/* test_f7_fmtdiff.c - F7 structured format/schema diff unit tests. */
#include "np_fmtdiff.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
#define CHECK(x) do { ++checks; if (!(x)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); ++fails; } } while (0)

static void test_no_diff(void) {
    np_fd_set_t a, b; np_fd_set_init(&a); np_fd_set_init(&b);
    np_fd_set_add(&a, "/word/document.xml#w:p[0]", "Hello", "段落0");
    np_fd_set_add(&b, "/word/document.xml#w:p[0]", "Hello", "段落0");
    np_fd_result_t r; np_fd_result_init(&r);
    CHECK(np_fd_diff(&a, &b, &r));
    CHECK(r.count == 0);
    np_fd_result_free(&r); np_fd_set_free(&a); np_fd_set_free(&b);
}

static void test_add_del_mod(void) {
    np_fd_set_t a, b; np_fd_set_init(&a); np_fd_set_init(&b);
    np_fd_set_add(&a, "p0", "old text", "P0");
    np_fd_set_add(&a, "p1", "same", "P1");
    np_fd_set_add(&a, "p2", "will be deleted", "P2");
    np_fd_set_add(&b, "p0", "new text", "P0");       /* MOD */
    np_fd_set_add(&b, "p1", "same", "P1");           /* unchanged */
    np_fd_set_add(&b, "p3", "added", "P3");          /* ADD */
    np_fd_result_t r; np_fd_result_init(&r);
    CHECK(np_fd_diff(&a, &b, &r));
    CHECK(r.count == 3);
    /* sorted by path: p0(MOD), p2(DEL), p3(ADD) */
    CHECK(r.items[0].op == NP_FD_MOD);
    CHECK(strcmp(r.items[0].path, "p0") == 0);
    CHECK(strcmp(r.items[0].before_text, "old text") == 0);
    CHECK(strcmp(r.items[0].after_text, "new text") == 0);
    CHECK(r.items[1].op == NP_FD_DEL && strcmp(r.items[1].path, "p2") == 0);
    CHECK(r.items[2].op == NP_FD_ADD && strcmp(r.items[2].path, "p3") == 0);
    CHECK(strcmp(np_fd_op_name(NP_FD_MOD), "MOD") == 0);
    np_fd_result_free(&r); np_fd_set_free(&a); np_fd_set_free(&b);
}

static void test_move_is_del_add(void) {
    np_fd_set_t a, b; np_fd_set_init(&a); np_fd_set_init(&b);
    np_fd_set_add(&a, "sec1/p", "identical text", "P");
    np_fd_set_add(&b, "sec2/p", "identical text", "P");  /* moved */
    np_fd_result_t r; np_fd_result_init(&r);
    CHECK(np_fd_diff(&a, &b, &r));
    CHECK(r.count == 2);
    CHECK(r.items[0].op == NP_FD_DEL && r.items[1].op == NP_FD_ADD);
    np_fd_result_free(&r); np_fd_set_free(&a); np_fd_set_free(&b);
}

static void test_label_change_is_mod(void) {
    np_fd_set_t a, b; np_fd_set_init(&a); np_fd_set_init(&b);
    np_fd_set_add(&a, "cell", "100", "old label");
    np_fd_set_add(&b, "cell", "100", "new label");
    np_fd_result_t r; np_fd_result_init(&r);
    CHECK(np_fd_diff(&a, &b, &r));
    CHECK(r.count == 1 && r.items[0].op == NP_FD_MOD);
    CHECK(strcmp(r.items[0].before_label, "old label") == 0);
    CHECK(strcmp(r.items[0].after_label, "new label") == 0);
    np_fd_result_free(&r); np_fd_set_free(&a); np_fd_set_free(&b);
}

static void test_schema_diff(void) {
    /* Simulate F3 schema elements: tables + columns + index. */
    np_fd_set_t a, b; np_fd_set_init(&a); np_fd_set_init(&b);
    np_fd_set_add(&a, "users", "", "TABLE");
    np_fd_set_add(&a, "users.id", "INTEGER PK", "COLUMN");
    np_fd_set_add(&a, "users.name", "TEXT", "COLUMN");
    np_fd_set_add(&a, "users@idx_name", "name", "INDEX");
    np_fd_set_add(&b, "users", "", "TABLE");
    np_fd_set_add(&b, "users.id", "INTEGER PK", "COLUMN");
    np_fd_set_add(&b, "users.name", "VARCHAR(80)", "COLUMN");  /* MOD */
    /* index removed */
    np_fd_set_add(&b, "posts", "", "TABLE");                 /* ADD */
    np_fd_result_t r; np_fd_result_init(&r);
    CHECK(np_fd_diff(&a, &b, &r));
    CHECK(r.count == 3);   /* sorted by path: posts ADD, users.name MOD, users@idx DEL */
    CHECK(r.items[0].op == NP_FD_ADD && strcmp(r.items[0].path, "posts") == 0);
    CHECK(r.items[1].op == NP_FD_MOD && strcmp(r.items[1].path, "users.name") == 0);
    CHECK(r.items[2].op == NP_FD_DEL && strstr(r.items[2].path, "idx_name") != NULL);
    np_fd_result_free(&r); np_fd_set_free(&a); np_fd_set_free(&b);
}

static void test_review_states(void) {
    np_fd_set_t a, b; np_fd_set_init(&a); np_fd_set_init(&b);
    np_fd_set_add(&a, "x", "1", ""); np_fd_set_add(&b, "x", "2", "");
    np_fd_set_add(&a, "y", "1", ""); np_fd_set_add(&b, "y", "2", "");
    np_fd_result_t r; np_fd_result_init(&r);
    CHECK(np_fd_diff(&a, &b, &r));
    CHECK(r.count == 2);
    CHECK(r.items[0].state == NP_FD_PENDING);
    CHECK(np_fd_accept(&r, 0));
    CHECK(np_fd_reject(&r, 1));
    CHECK(!np_fd_accept(&r, 99));       /* OOB */
    CHECK(!np_fd_reject(&r, 99));
    CHECK(np_fd_count_by_state(&r, NP_FD_ACCEPTED) == 1);
    CHECK(np_fd_count_by_state(&r, NP_FD_REJECTED) == 1);
    CHECK(np_fd_count_by_state(&r, NP_FD_PENDING) == 0);
    CHECK(strcmp(np_fd_state_name(NP_FD_ACCEPTED), "accepted") == 0);
    char patch[512];
    size_t n = np_fd_render_accepted(&r, patch, sizeof patch);
    CHECK(n > 0 && strstr(patch, "MOD\tx\t2") != NULL);
    CHECK(strstr(patch, "\ty\t") == NULL);   /* rejected not rendered */
    np_fd_result_free(&r); np_fd_set_free(&a); np_fd_set_free(&b);
}

static void test_bounds(void) {
    np_fd_set_t s; np_fd_set_init(&s);
    CHECK(!np_fd_set_add(&s, NULL, "x", ""));
    CHECK(!np_fd_set_add(&s, "", "x", ""));
    CHECK(np_fd_set_add(&s, "p", "x", ""));
    CHECK(!np_fd_set_add(&s, "p", "dup", ""));   /* duplicate path rejected */
    CHECK(np_fd_set_find(&s, "p") != NULL);
    CHECK(np_fd_set_find(&s, "missing") == NULL);
    np_fd_set_free(&s);
    /* NULL args to diff */
    np_fd_result_t r; np_fd_result_init(&r);
    CHECK(!np_fd_diff(NULL, NULL, &r));
    np_fd_result_free(&r);
}

int main(void) {
    test_no_diff();
    test_add_del_mod();
    test_move_is_del_add();
    test_label_change_is_mod();
    test_schema_diff();
    test_review_states();
    test_bounds();
    printf("test_f7_fmtdiff: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
