/* test_f8_rewrite.c - F8 safe OOXML write-back regression tests. */
#include "np_ooxml_write.h"
#include "np_ooxml.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int checks = 0, fails = 0;
#define CHECK(x) do { ++checks; if (!(x)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); ++fails; } } while (0)

static const char *SRC = "tests/golden/golden.docx";
static const char *OUT = "tests/golden/_f8_test_out.docx";
static const char *BAK = "tests/golden/_f8_test_bak.docx";

/* Real open-read-back verifier: parse with np_ooxml. */
static bool verify_parse(const char *path, void *user) {
    (void)user;
    np_ox_doc_t d;
    np_ox_status_t st = np_ox_parse_file(path, &d);
    return st == NP_OX_OK && d.valid && d.kind == NP_OX_WORD;
}
/* A verifier that always fails (gate must abort the commit). */
static bool verify_fail(const char *path, void *user) { (void)path; (void)user; return false; }

static unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long z = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = (unsigned char *)malloc((size_t)z);
    if (!b) { fclose(f); return NULL; }
    *n = fread(b, 1, (size_t)z, f);
    fclose(f);
    return b;
}

static void test_read_parts(void) {
    np_oxw_pkg_t pkg;
    CHECK(np_oxw_read(SRC, &pkg));
    CHECK(pkg.count >= 4);
    CHECK(np_oxw_find(&pkg, "[Content_Types].xml") != NULL);
    CHECK(np_oxw_find(&pkg, "word/document.xml") != NULL);
    CHECK(np_oxw_find(&pkg, "_rels/.rels") != NULL);
    const np_oxw_part_t *doc = np_oxw_find_c(&pkg, "word/document.xml");
    CHECK(doc && doc->len > 0);
    CHECK(doc && strstr((const char *)doc->data, "Quarterly Report") != NULL);
    np_oxw_free(&pkg);
}

static void test_xml_gate(void) {
    char err[160];
    CHECK(np_oxw_xml_wellformed("<a><b>text</b></a>", 18, err, sizeof err));
    { const char *decl = "<?xml version=\"1.0\"?><a/>";
      CHECK(np_oxw_xml_wellformed(decl, strlen(decl), err, sizeof err)); }
    CHECK(!np_oxw_xml_wellformed("<a><b></a>", 10, err, sizeof err));       /* mismatch */
    CHECK(!np_oxw_xml_wellformed("<a>", 3, err, sizeof err));               /* unclosed */
    { const char ctrlxml[] = { '<','a','>',0x01,'<','/','a','>',0 };
      CHECK(!np_oxw_xml_wellformed(ctrlxml, 8, err, sizeof err)); }          /* control */
    CHECK(!np_oxw_xml_wellformed("", 0, err, sizeof err));
}

static void test_write_readback(void) {
    np_oxw_pkg_t pkg;
    CHECK(np_oxw_read(SRC, &pkg));
    const char *new_doc =
        "<?xml version=\"1.0\"?><w:document xmlns:w=\"http://x\"><w:body>"
        "<w:p><w:r><w:t>Safe rewritten content</w:t></w:r></w:p></w:body></w:document>";
    CHECK(np_oxw_set_part(&pkg, "word/document.xml",
                          (const unsigned char *)new_doc, strlen(new_doc)));
    /* non-existent part rejected */
    CHECK(!np_oxw_set_part(&pkg, "no/such.xml", (const unsigned char *)"x", 1));
    char err[200];
    CHECK(np_oxw_commit(&pkg, OUT, SRC, BAK, verify_parse, NULL, err, sizeof err));
    /* open-read-back: new content present */
    np_oxw_pkg_t out;
    CHECK(np_oxw_read(OUT, &out));
    const np_oxw_part_t *doc = np_oxw_find_c(&out, "word/document.xml");
    CHECK(doc && strstr((const char *)doc->data, "Safe rewritten content") != NULL);
    /* all other parts preserved */
    CHECK(np_oxw_find_c(&out, "[Content_Types].xml") != NULL);
    CHECK(np_oxw_find_c(&out, "_rels/.rels") != NULL);
    np_oxw_free(&out);
    /* Ctrl+Z: restore original bytes exactly */
    size_t a, b;
    unsigned char *orig = slurp(SRC, &a), *bak = slurp(BAK, &b);
    CHECK(orig && bak && a == b && memcmp(orig, bak, a) == 0);
    free(orig); free(bak);
    CHECK(np_oxw_restore(BAK, OUT, err, sizeof err));
    unsigned char *restored = slurp(OUT, &a), *orig2 = slurp(SRC, &b);
    CHECK(restored && orig2 && a == b && memcmp(restored, orig2, a) == 0);
    free(restored); free(orig2);
    np_oxw_free(&pkg);
    remove(OUT); remove(BAK);
}

static void test_verify_gate_abort(void) {
    np_oxw_pkg_t pkg;
    CHECK(np_oxw_read(SRC, &pkg));
    char err[200];
    /* verifier fails -> no final file */
    remove(OUT);
    CHECK(!np_oxw_commit(&pkg, OUT, SRC, BAK, verify_fail, NULL, err, sizeof err));
    CHECK(strstr(err, "回读") != NULL);
    FILE *f = fopen(OUT, "rb");
    CHECK(f == NULL);   /* fail-closed: no final artifact */
    if (f) fclose(f);
    np_oxw_free(&pkg);
    remove(BAK);
}

static void test_bad_args(void) {
    np_oxw_pkg_t pkg;
    memset(&pkg, 0, sizeof pkg);
    char err[160];
    CHECK(!np_oxw_read("no-such-file.zip", &pkg));
    CHECK(!np_oxw_commit(&pkg, OUT, SRC, BAK, NULL, NULL, err, sizeof err));
    CHECK(!np_oxw_restore("no-such-bak", OUT, err, sizeof err));
    CHECK(!np_oxw_xml_wellformed(NULL, 0, err, sizeof err));
    CHECK(np_oxw_find(&pkg, "x") == NULL);
}

int main(void) {
    test_read_parts();
    test_xml_gate();
    test_write_readback();
    test_verify_gate_abort();
    test_bad_args();
    printf("test_f8_rewrite: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
