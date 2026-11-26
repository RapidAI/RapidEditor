/* Unit tests for A3 (np_patch) - three-track patch protocol.
 * Covers the document's red line: text/byte/format primitives never mix,
 * base_revision binding, and one-step undo of a whole patch set. */
#include "np_patch.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static int test_text_lines(void) {
    np_patch_buffer_t b; np_patch_buffer_init(&b);
    const char *src = "line0\nline1\nline2\nline3\n";
    CHECK(np_patch_buffer_set(&b, src, strlen(src)));
    CHECK(np_patch_line_count(&b) == 4);

    np_patch_set_t *ps = np_patch_set_create(b.revision);
    CHECK(ps);
    /* replace line1..line2 with one line */
    CHECK(np_patch_add_text(ps, NP_OP_REPLACE, 1, 2, "NEW\n", 4) == NP_PATCH_OK);
    CHECK(np_patch_validate(ps, NP_TRACK_TEXT, &b) == NP_PATCH_OK);
    CHECK(np_patch_apply(ps, NP_TRACK_TEXT, &b) == NP_PATCH_OK);
    CHECK(b.revision == 1);
    /* [line_first,line_last) = [1,2) -> replace "line1\n" with "NEW\n" */
    {
        const char *expect = "line0\nNEW\nline2\nline3\n";
        size_t el = strlen(expect);
        CHECK(b.len == el && memcmp(b.data, expect, el) == 0);
    }
    CHECK(np_patch_line_count(&b) == 4);

    CHECK(np_patch_revert(ps, &b));
    CHECK(b.revision == 0);
    CHECK(memcmp(b.data, src, strlen(src)) == 0);
    np_patch_set_destroy(ps);
    np_patch_buffer_free(&b);
    return 0;
}

static int test_byte_insert_delete(void) {
    np_patch_buffer_t b; np_patch_buffer_init(&b);
    unsigned char d[4] = {0x10,0x20,0x30,0x40};
    CHECK(np_patch_buffer_set(&b, d, 4));

    /* INSERT two bytes at offset 1: file grows */
    np_patch_set_t *ps = np_patch_set_create(b.revision);
    unsigned char ins[2] = {0xAA,0xBB};
    CHECK(np_patch_add_byte(ps, NP_OP_INSERT, 1, 0, ins, 2) == NP_PATCH_OK);
    CHECK(np_patch_apply(ps, NP_TRACK_BINARY, &b) == NP_PATCH_OK);
    CHECK(b.len == 6);
    CHECK(b.data[0]==0x10 && b.data[1]==0xAA && b.data[2]==0xBB && b.data[5]==0x40);
    /* one undo restores original size + bytes */
    CHECK(np_patch_revert(ps, &b));
    CHECK(b.len == 4 && memcmp(b.data, d, 4) == 0);

    /* DELETE in a fresh set from the restored state */
    np_patch_set_t *ps2 = np_patch_set_create(b.revision);
    CHECK(np_patch_add_byte(ps2, NP_OP_DELETE, 1, 2, NULL, 0) == NP_PATCH_OK);
    CHECK(np_patch_apply(ps2, NP_TRACK_BINARY, &b) == NP_PATCH_OK);
    CHECK(b.len == 2 && b.data[0]==0x10 && b.data[1]==0x40);
    CHECK(np_patch_revert(ps2, &b));
    CHECK(b.len == 4);

    np_patch_set_destroy(ps); np_patch_set_destroy(ps2);
    np_patch_buffer_free(&b);
    return 0;
}

static int test_guards(void) {
    np_patch_buffer_t b; np_patch_buffer_init(&b);
    unsigned char d[4] = {1,2,3,4};
    CHECK(np_patch_buffer_set(&b, d, 4));

    /* stale base_revision is rejected */
    np_patch_set_t *ps = np_patch_set_create(99);
    CHECK(np_patch_add_byte(ps, NP_OP_REPLACE, 0, 1, (unsigned char*)"X", 1) == NP_PATCH_OK);
    CHECK(np_patch_validate(ps, NP_TRACK_BINARY, &b) == NP_PATCH_ERR_REVISION);
    CHECK(np_patch_apply(ps, NP_TRACK_BINARY, &b) == NP_PATCH_ERR_REVISION);
    CHECK(b.data[0] == 1);  /* unchanged */
    np_patch_set_destroy(ps);

    /* out-of-range is rejected without touching the buffer */
    ps = np_patch_set_create(b.revision);
    CHECK(np_patch_add_byte(ps, NP_OP_REPLACE, 3, 2, (unsigned char*)"X", 1) == NP_PATCH_OK);
    CHECK(np_patch_apply(ps, NP_TRACK_BINARY, &b) == NP_PATCH_ERR_RANGE);
    CHECK(b.len == 4 && b.data[0] == 1);
    np_patch_set_destroy(ps);

    /* format hunk is protocol-valid but needs F8 writer */
    ps = np_patch_set_create(b.revision);
    CHECK(np_patch_add_format(ps, NP_OP_REPLACE, "word/document.xml", "/w:p[42]", "<w:t>x</w:t>", 13) == NP_PATCH_OK);
    CHECK(np_patch_validate(ps, NP_TRACK_FORMAT, &b) == NP_PATCH_ERR_NEEDS_F8);
    np_patch_set_destroy(ps);

    /* track mismatch rejected */
    ps = np_patch_set_create(b.revision);
    CHECK(np_patch_add_byte(ps, NP_OP_REPLACE, 0, 1, (unsigned char*)"X", 1) == NP_PATCH_OK);
    CHECK(np_patch_validate(ps, NP_TRACK_TEXT, &b) == NP_PATCH_ERR_TRACK);
    np_patch_set_destroy(ps);

    np_patch_buffer_free(&b);
    return 0;
}

int main(void) {
    if (test_text_lines()) return 1;
    if (test_byte_insert_delete()) return 1;
    if (test_guards()) return 1;
    printf("test_s0_patch: ALL PASSED\n");
    return 0;
}
