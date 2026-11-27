/* test_hexundo.c - multi-level hex undo session semantics. */
#include "np_hexundo.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
#define CHECK(x) do { ++checks; if (!(x)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); ++fails; } } while (0)

static void eq_bytes(np_hexundo_t *h, const char *expect) {
    size_t n = 0;
    const unsigned char *p = np_hexundo_data(h, &n);
    CHECK(n == strlen(expect));
    CHECK(n == 0 || memcmp(p, expect, n) == 0);
}

static void test_basic_undo_redo(void) {
    np_hexundo_t *h = np_hexundo_create("AAA", 3);
    CHECK(!np_hexundo_can_undo(h));
    CHECK(!np_hexundo_can_redo(h));

    /* session 1: A A A -> B A A */
    np_hexundo_observe(h, "BAA", 3);
    np_hexundo_tick(h, 0.1);
    np_hexundo_observe(h, "BBA", 3);   /* same window -> merged */
    np_hexundo_flush(h);
    eq_bytes(h, "BBA");
    CHECK(np_hexundo_can_undo(h));

    /* session 2: B B A -> B B B */
    np_hexundo_tick(h, 1.0);
    np_hexundo_observe(h, "BBB", 3);
    np_hexundo_flush(h);
    eq_bytes(h, "BBB");

    /* undo twice: BBB -> BBA -> AAA */
    CHECK(np_hexundo_undo(h)); eq_bytes(h, "BBA");
    CHECK(np_hexundo_undo(h)); eq_bytes(h, "AAA");
    CHECK(!np_hexundo_can_undo(h));
    CHECK(np_hexundo_can_redo(h));

    /* redo twice: AAA -> BBA -> BBB */
    CHECK(np_hexundo_redo(h)); eq_bytes(h, "BBA");
    CHECK(np_hexundo_redo(h)); eq_bytes(h, "BBB");
    CHECK(!np_hexundo_can_redo(h));
    np_hexundo_destroy(h);
}

static void test_window_merge(void) {
    np_hexundo_t *h = np_hexundo_create("0000", 4);
    np_hexundo_observe(h, "1000", 4);
    np_hexundo_tick(h, 0.3);
    np_hexundo_observe(h, "1100", 4);
    np_hexundo_tick(h, 0.3);           /* 0.6 total, within window */
    np_hexundo_observe(h, "1110", 4);
    np_hexundo_flush(h);
    /* one undo must jump straight back to the original, not 1110 -> 1100 */
    CHECK(np_hexundo_undo(h));
    eq_bytes(h, "0000");
    CHECK(!np_hexundo_can_undo(h));
    np_hexundo_destroy(h);
}

static void test_flush_splits_session(void) {
    np_hexundo_t *h = np_hexundo_create("ab", 2);
    np_hexundo_observe(h, "cb", 2);
    np_hexundo_flush(h);              /* explicit close */
    np_hexundo_tick(h, 0.1);          /* short time, but flushed */
    np_hexundo_observe(h, "cd", 2);
    np_hexundo_flush(h);
    CHECK(np_hexundo_undo(h)); eq_bytes(h, "cb");
    CHECK(np_hexundo_undo(h)); eq_bytes(h, "ab");
    np_hexundo_destroy(h);
}

static void test_redo_invalidated_on_new_edit(void) {
    np_hexundo_t *h = np_hexundo_create("XY", 2);
    np_hexundo_observe(h, "ZY", 2);
    np_hexundo_flush(h);
    CHECK(np_hexundo_undo(h)); eq_bytes(h, "XY");
    CHECK(np_hexundo_can_redo(h));
    /* new edit after undo invalidates the redo branch */
    np_hexundo_observe(h, "XZ", 2);
    np_hexundo_flush(h);
    CHECK(!np_hexundo_can_redo(h));
    CHECK(np_hexundo_undo(h)); eq_bytes(h, "XY");
    np_hexundo_destroy(h);
}

static void test_length_change(void) {
    np_hexundo_t *h = np_hexundo_create("abc", 3);
    np_hexundo_observe(h, "abd", 3);
    np_hexundo_flush(h);
    /* variable-length edit: undo must restore exact bytes AND length (H1) */
    np_hexundo_observe(h, "abcdef", 6);
    np_hexundo_flush(h);
    eq_bytes(h, "abcdef");
    CHECK(np_hexundo_undo(h)); eq_bytes(h, "abd");
    CHECK(np_hexundo_undo(h)); eq_bytes(h, "abc");
    CHECK(np_hexundo_redo(h)); eq_bytes(h, "abd");
    np_hexundo_destroy(h);
}

static void test_reset_and_noop(void) {
    np_hexundo_t *h = np_hexundo_create("q", 1);
    np_hexundo_observe(h, "q", 1);     /* no actual change */
    CHECK(!np_hexundo_can_undo(h));
    np_hexundo_observe(h, "r", 1);
    np_hexundo_flush(h);
    CHECK(np_hexundo_can_undo(h));
    np_hexundo_reset(h, "newbase", 7);
    CHECK(!np_hexundo_can_undo(h));
    eq_bytes(h, "newbase");
    CHECK(!np_hexundo_redo(h));       /* no redo available */
    np_hexundo_destroy(h);
}

static void test_no_ghost_group_after_undo(void) {
    np_hexundo_t *h = np_hexundo_create("AAA", 3);
    np_hexundo_observe(h, "BAA", 3);
    np_hexundo_flush(h);
    np_hexundo_tick(h, 1.0);
    np_hexundo_observe(h, "BBB", 3);
    np_hexundo_flush(h);
    CHECK(np_hexundo_undo(h));                  /* -> BAA, redo available */
    CHECK(np_hexundo_can_redo(h));
    /* The doc is copied back from shadow, so the next frame observes the
     * same bytes. That must be a no-op, not a new group killing redo. */
    np_hexundo_observe(h, "BAA", 3);
    CHECK(np_hexundo_can_redo(h));
    eq_bytes(h, "BAA");
    np_hexundo_destroy(h);
}

static void test_empty(void) {
    np_hexundo_t *h = np_hexundo_create(NULL, 0);
    CHECK(h != NULL);
    eq_bytes(h, "");
    np_hexundo_observe(h, "A", 1);
    np_hexundo_flush(h);
    CHECK(np_hexundo_can_undo(h));
    CHECK(np_hexundo_undo(h));
    eq_bytes(h, "");
    np_hexundo_destroy(h);
}

int main(void) {
    test_basic_undo_redo();
    test_window_merge();
    test_flush_splits_session();
    test_redo_invalidated_on_new_edit();
    test_length_change();
    test_reset_and_noop();
    test_no_ghost_group_after_undo();
    test_empty();
    printf("test_hexundo: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
