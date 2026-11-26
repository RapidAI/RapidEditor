/* Unit tests for H1 (np_bytepatch) + H2 (np_integrity) + H8 (np_filesafety). */
#include "np_bytepatch.h"
#include "np_integrity.h"
#include "np_filesafety.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static int test_sha_crc(void) {
    /* NIST SHA-256("abc") */
    unsigned char sha[32];
    np_sha256("abc", 3, sha);
    char hex[65]; np_sha256_hex(sha, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    /* CRC32("123456789") = 0xCBF43926 */
    CHECK(np_crc32("123456789", 9) == 0xCBF43926u);
    /* empty buffer digests are well-defined */
    CHECK(np_crc32(NULL, 0) == 0u);
    return 0;
}

static int test_guard(void) {
    /* 8-byte PNG signature as protected header + payload bytes after it */
    unsigned char orig[12] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A, 0,0,0,13};
    np_protected_t sig = np_protected_make(0, orig, 8, "PNG signature");
    np_write_guard_t *g = np_guard_begin(orig, 12, &sig, 1);
    CHECK(g != NULL);

    char hash[65];
    { unsigned char s[32]; np_sha256(orig, 12, s); np_sha256_hex(s, hash); }

    unsigned char good[12]; memcpy(good, orig, 12); good[9] = 0x01; /* change after sig */
    CHECK(np_guard_check(g, good, 12, hash, 0) == NP_GUARD_OK);

    /* length changed unexpectedly */
    CHECK(np_guard_check(g, good, 13, hash, NP_GUARD_SKIP_DELTA) == NP_GUARD_OK); /* skip delta => no len check */
    CHECK(np_guard_check(g, good, 13, hash, 0) == NP_GUARD_ERR_LENGTH);
    CHECK(np_guard_check(g, good, 14, hash, 2) == NP_GUARD_OK); /* +2 expected */

    /* protected region modified */
    unsigned char bad[12]; memcpy(bad, orig, 12); bad[0] = 0x00;
    CHECK(np_guard_check(g, bad, 12, hash, 0) == NP_GUARD_ERR_REGION);

    /* stale hash */
    CHECK(np_guard_check(g, good, 12, "0000000000000000000000000000000000000000000000000000000000000000", 0)
          == NP_GUARD_ERR_STALE);
    np_guard_free(g);
    return 0;
}

static int test_bytepatch_undo(void) {
    unsigned char d[4] = {0x10,0x20,0x30,0x40};
    np_bytepatch_t *bp = np_bytepatch_create(d, 4);
    CHECK(bp != NULL);
    CHECK(!np_bytepatch_can_undo(bp));

    /* group 1: replace equal-length */
    CHECK(np_bytepatch_begin_group(bp));
    unsigned char rep[1] = {0x99};
    CHECK(np_bytepatch_replace(bp, 1, 1, rep, 1) == NP_BP_OK);
    CHECK(np_bytepatch_end_group(bp));
    size_t n; const unsigned char *p = np_bytepatch_data(bp, &n);
    CHECK(n == 4 && p[1] == 0x99);
    CHECK(np_bytepatch_can_undo(bp));

    /* group 2: INSERT 2 bytes -> size changes */
    CHECK(np_bytepatch_begin_group(bp));
    unsigned char ins[2] = {0xAA,0xBB};
    CHECK(np_bytepatch_insert(bp, 2, ins, 2) == NP_BP_OK);
    CHECK(np_bytepatch_end_group(bp));
    p = np_bytepatch_data(bp, &n);
    CHECK(n == 6 && p[2]==0xAA && p[3]==0xBB && p[5]==0x40);

    /* group 3: DELETE 3 bytes -> size shrinks */
    CHECK(np_bytepatch_begin_group(bp));
    CHECK(np_bytepatch_delete(bp, 0, 3) == NP_BP_OK);
    CHECK(np_bytepatch_end_group(bp));
    p = np_bytepatch_data(bp, &n);
    CHECK(n == 3);

    /* undo group 3 -> back to 6 bytes */
    CHECK(np_bytepatch_undo(bp));
    p = np_bytepatch_data(bp, &n);
    CHECK(n == 6 && p[0]==0x10 && p[2]==0xAA);
    /* undo group 2 -> back to 4 bytes with the group-1 replacement */
    CHECK(np_bytepatch_undo(bp));
    p = np_bytepatch_data(bp, &n);
    CHECK(n == 4 && p[1]==0x99 && p[2]==0x30);
    /* redo twice */
    CHECK(np_bytepatch_redo(bp));
    p = np_bytepatch_data(bp, &n); CHECK(n == 6 && p[2]==0xAA);
    CHECK(np_bytepatch_redo(bp));
    p = np_bytepatch_data(bp, &n); CHECK(n == 3);
    CHECK(!np_bytepatch_can_redo(bp));

    np_bytepatch_destroy(bp);
    return 0;
}

static int test_filesafety(void) {
    unsigned char elf[4] = {0x7F,'E','L','F'};
    unsigned char normal[4] = {1,2,3,4};
    /* explanation-only credentials never writable */
    CHECK(np_fs_decide("id_rsa", normal, 4, true) == NP_FS_EXPLAIN_ONLY);
    CHECK(np_fs_decide("cert.pem", normal, 4, true) == NP_FS_EXPLAIN_ONLY);
    /* executables denied by default, allowed with opt-in */
    CHECK(np_fs_decide("app.exe", elf, 4, false) == NP_FS_WRITE_DENIED);
    CHECK(np_fs_decide("app.exe", elf, 4, true) == NP_FS_WRITE_ALLOWED);
    /* ELF magic detected even with a benign extension; follows opt-in too */
    CHECK(np_fs_decide("data.bin", elf, 4, false) == NP_FS_WRITE_DENIED);
    CHECK(np_fs_decide("data.bin", elf, 4, true) == NP_FS_WRITE_ALLOWED);
    /* ordinary file: opt-in required */
    CHECK(np_fs_decide("notes.txt", normal, 4, false) == NP_FS_WRITE_DENIED);
    CHECK(np_fs_decide("notes.txt", normal, 4, true) == NP_FS_WRITE_ALLOWED);

    np_audit_log_t *log = np_audit_create(2);
    CHECK(log && np_audit_count(log) == 0);
    np_audit_record_t r; memset(&r, 0, sizeof(r));
    strcpy(r.file, "a.bin"); strcpy(r.action, "write");
    CHECK(np_audit_append(log, &r));
    CHECK(np_audit_append(log, &r));
    CHECK(np_audit_append(log, &r)); /* wraps */
    CHECK(np_audit_count(log) == 2);
    CHECK(strcmp(np_audit_at(log, 0)->file, "a.bin") == 0);
    np_audit_destroy(log);
    return 0;
}

int main(void) {
    if (test_sha_crc()) return 1;
    if (test_guard()) return 1;
    if (test_bytepatch_undo()) return 1;
    if (test_filesafety()) return 1;
    printf("test_s0_binary: ALL PASSED\n");
    return 0;
}
