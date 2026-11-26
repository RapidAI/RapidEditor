/* test_h7_bytediff.c - H7 byte diff unit tests (no CUI dependency). */
#include "np_bytediff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* Full gate for every case: compute -> verify -> apply -> byte-equal. */
static bool case_run(const void *before, size_t blen,
                     const void *after, size_t alen) {
    np_bd_result_t r;
    if (!np_bytediff_compute(before, blen, after, alen, &r)) return false;
    bool ok = np_bytediff_verify(before, blen, &r, after, alen);
    unsigned char *out = (unsigned char *)malloc(alen ? alen : 1);
    size_t on = 0;
    if (out) {
        ok = ok && np_bytediff_apply(before, blen, after, &r, out, alen, &on) &&
             on == alen && (alen == 0 || memcmp(out, after, alen) == 0);
    } else ok = false;
    free(out);
    np_bytediff_free(&r);
    return ok;
}

static void test_identical(void) {
    unsigned char d[] = "ABCDEFGH";
    np_bd_result_t r;
    CHECK(np_bytediff_compute(d, 8, d, 8, &r));
    CHECK(r.nhunks == 0);
    CHECK(r.crc_before == r.crc_after);
    CHECK(r.before_len == 8 && r.after_len == 8);
    CHECK(np_bytediff_verify(d, 8, &r, d, 8));
    np_bytediff_free(&r);
    CHECK(case_run("", 0, "", 0));
}

static void test_replace_equal_len(void) {
    unsigned char a[] = "ABCDEFGH";
    unsigned char b[] = "ABXXEFGH";   /* [4,6) C,D -> X,X */
    np_bd_result_t r;
    CHECK(np_bytediff_compute(a, 8, b, 8, &r));
    CHECK(r.nhunks == 1);
    if (r.nhunks == 1) {
        CHECK(r.hunks[0].kind == NP_BD_REPLACE);
        CHECK(r.hunks[0].before_off == 2 && r.hunks[0].before_len == 2);
        CHECK(r.hunks[0].after_off == 2 && r.hunks[0].after_len == 2);
        CHECK(r.hunks[0].before_prev_n == 2 && r.hunks[0].before_bytes[0] == 'C');
        CHECK(r.hunks[0].after_prev_n == 2 && r.hunks[0].after_bytes[0] == 'X');
    }
    CHECK(case_run(a, 8, b, 8));
    np_bytediff_free(&r);
}

static void test_insert_delete(void) {
    unsigned char a[] = "ABCDEFGH";
    unsigned char ins[] = "ABXYCDEFGH";  /* insert XY at 2 */
    unsigned char del[] = "ABEFGH";      /* delete [2,4) */
    np_bd_result_t r;
    CHECK(np_bytediff_compute(a, 8, ins, 10, &r));
    CHECK(r.nhunks == 1);
    if (r.nhunks == 1) {
        CHECK(r.hunks[0].kind == NP_BD_INSERT);
        CHECK(r.hunks[0].before_off == 2 && r.hunks[0].before_len == 0);
        CHECK(r.hunks[0].after_off == 2 && r.hunks[0].after_len == 2);
    }
    np_bytediff_free(&r);
    CHECK(case_run(a, 8, ins, 10));

    CHECK(np_bytediff_compute(a, 8, del, 6, &r));
    CHECK(r.nhunks == 1);
    if (r.nhunks == 1) {
        CHECK(r.hunks[0].kind == NP_BD_DELETE);
        CHECK(r.hunks[0].before_off == 2 && r.hunks[0].before_len == 2);
        CHECK(r.hunks[0].after_len == 0);
    }
    np_bytediff_free(&r);
    CHECK(case_run(a, 8, del, 6));

    /* pure insert/delete at boundaries */
    CHECK(case_run("", 0, "AB", 2));
    CHECK(case_run("AB", 2, "", 0));
}

static void test_mixed_variable_len(void) {
    /* 16-byte sequence; change [4,6)->FF (shrink by 1) and insert AA BB at 10. */
    unsigned char a[16], b[17];
    for (int i = 0; i < 16; ++i) a[i] = (unsigned char)i;
    memcpy(b, a, 4);
    b[4] = 0xFF;
    memcpy(b + 5, a + 6, 5);      /* 06 07 08 09 0A */
    b[10] = 0xAA; b[11] = 0xBB;
    memcpy(b + 12, a + 11, 5);    /* 0B 0C 0D 0E 0F */
    CHECK(case_run(a, sizeof a, b, sizeof b));

    np_bd_result_t r;
    CHECK(np_bytediff_compute(a, sizeof a, b, sizeof b, &r));
    CHECK(r.nhunks == 2);
    CHECK(!r.fuzzy);
    CHECK(r.before_len == 16 && r.after_len == 17);
    CHECK(r.crc_before != r.crc_after);
    np_bytediff_free(&r);
}

static void test_crc_and_lengths(void) {
    /* standard CRC-32 of "123456789" is 0xCBF43926 */
    np_bd_result_t r;
    CHECK(np_bytediff_compute("123456789", 9, "123456789", 9, &r));
    CHECK(r.crc_before == 0xCBF43926u);
    np_bytediff_free(&r);

    CHECK(np_bytediff_compute("AAAA", 4, "BBBBBB", 6, &r));
    CHECK(r.before_len == 4 && r.after_len == 6);
    CHECK(r.crc_before != r.crc_after);
    CHECK(case_run("AAAA", 4, "BBBBBB", 6));
    np_bytediff_free(&r);
}

static void test_fuzzy_degradation(void) {
    /* 2000 wholly-different bytes force D>512 -> coarse single hunk. */
    unsigned char a[2000], b[2000];
    for (int i = 0; i < 2000; ++i) { a[i] = (unsigned char)i; b[i] = (unsigned char)(255 - i); }
    np_bd_result_t r;
    CHECK(np_bytediff_compute(a, sizeof a, b, sizeof b, &r));
    CHECK(r.fuzzy);
    CHECK(r.nhunks == 1);
    CHECK(np_bytediff_verify(a, sizeof a, &r, b, sizeof b));
    np_bytediff_free(&r);
    CHECK(case_run(a, sizeof a, b, sizeof b));
}

static void test_invalid_args(void) {
    np_bd_result_t r;
    CHECK(!np_bytediff_compute(NULL, 4, "ABCD", 4, &r));
    CHECK(!np_bytediff_compute("ABCD", 4, NULL, 4, &r));
    CHECK(!np_bytediff_compute("ABCD", 4, "ABCD", 4, NULL));
    /* verify rejects wrong lengths / corrupted hunk */
    CHECK(np_bytediff_compute("ABCD", 4, "ABXD", 4, &r));
    CHECK(np_bytediff_verify("ABCD", 4, &r, "ABXD", 4));
    CHECK(!np_bytediff_verify("ABCD", 4, &r, "ABXD", 5));
    r.hunks[0].after_off = 99;
    CHECK(!np_bytediff_verify("ABCD", 4, &r, "ABXD", 4));
    np_bytediff_free(&r);
}

static void test_random_roundtrip(void) {
    /* deterministic pseudo-random buffers with sparse mutations */
    unsigned int s = 12345u;
    unsigned char a[512], b[512];
    for (int i = 0; i < 512; ++i) { s = s * 1103515245u + 12345u; a[i] = (unsigned char)(s >> 16); }
    memcpy(b, a, sizeof a);
    for (int k = 0; k < 25; ++k) {
        s = s * 1103515245u + 12345u;
        size_t off = (s >> 16) % 512;
        s = s * 1103515245u + 12345u;
        b[off] = (unsigned char)(s >> 16);
    }
    CHECK(case_run(a, sizeof a, b, sizeof b));
    CHECK(case_run(b, sizeof b, a, sizeof a));
}

int main(void) {
    test_identical();
    test_replace_equal_len();
    test_insert_delete();
    test_mixed_variable_len();
    test_crc_and_lengths();
    test_fuzzy_degradation();
    test_invalid_args();
    test_random_roundtrip();
    if (failures == 0) printf("ALL H7 BYTEDIFF TESTS PASSED\n");
    else printf("%d H7 BYTEDIFF TEST(S) FAILED\n", failures);
    return failures != 0;
}
