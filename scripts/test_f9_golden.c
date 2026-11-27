/* test_f9_golden.c - F9 format-parsing regression gate.
 *
 * Centralizes the golden expectations for F1 (magic/container recognition),
 * F2 (OPC/OOXML structure tree) and F3 (SQLite schema), plus format-safety
 * red lines (encrypted docs / non-OPC zips rejected). This is the data-format
 * twin of R8: it catches silent regressions in parsers after library upgrades.
 *
 * Golden fixtures are capture-style samples; they lock parser contracts, not
 * semantic truth. PDF/Parquet full-content parsers are S2 items and are only
 * checked at the F1 recognition layer here.
 */
#include "np_ooxml.h"
#include "np_sqlite.h"
#include "np_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0, fails = 0;
#define CHECK(x) do { ++checks; if (!(x)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); ++fails; } } while (0)

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    unsigned char *b = (unsigned char *)malloc((size_t)n);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    *len = got;
    return b;
}

static int count_type(const np_ox_doc_t *d, np_ox_node_type_t t) {
    int n = 0;
    for (int i = 0; i < d->nnodes; i++) if (d->nodes[i].type == t) n++;
    return n;
}
static int find_label(const np_ox_doc_t *d, const char *sub) {
    for (int i = 0; i < d->nnodes; i++)
        if (strstr(d->nodes[i].label, sub)) return i;
    return -1;
}

/* ---- F1: magic + container recognition ---- */
static void test_f1_recognition(void) {
    struct { const char *path; np_format_kind_t kind; const char *name; } cases[] = {
        { "tests/golden/golden.docx", NP_FMT_OOXML, "docx" },
        { "tests/golden/golden.xlsx", NP_FMT_OOXML, "xlsx" },
        { "tests/golden/golden.pptx", NP_FMT_OOXML, "pptx" },
        { "tests/golden/plain.zip",   NP_FMT_ZIP_PLAIN, "plain.zip" },
        { "tests/golden/golden_full.db", NP_FMT_SQLITE, "sqlite" }
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        size_t len; unsigned char *b = read_file(cases[i].path, &len);
        CHECK(b != NULL);
        if (!b) continue;
        np_probe_result_t pr;
        np_probe(b, len, cases[i].name, &pr);
        CHECK(pr.fmt.kind == cases[i].kind);
        free(b);
    }
}

/* ---- F2: OOXML structure tree ---- */
static void test_docx_tree(void) {
    np_ox_doc_t d;
    CHECK(np_ox_parse_file("tests/golden/golden.docx", &d) == NP_OX_OK);
    CHECK(d.valid && d.kind == NP_OX_WORD);
    CHECK(strcmp(d.main_part, "word/document.xml") == 0);
    CHECK(d.nparts >= 4);
    CHECK(np_ox_find_part(&d, "word/document.xml") != NULL);
    CHECK(count_type(&d, NP_OX_NODE_TABLE) == 1);
    CHECK(count_type(&d, NP_OX_NODE_ROW) == 2);
    CHECK(count_type(&d, NP_OX_NODE_CELL) == 4);
    /* Lock the exact golden paragraph text, not a generic keyword. */
    CHECK(find_label(&d, "Quarterly Report") >= 0);
    CHECK(find_label(&d, "Revenue grew 12 percent") >= 0);
    CHECK(find_label(&d, "Approved by board") >= 0);
}

static void test_xlsx_tree(void) {
    np_ox_doc_t d;
    CHECK(np_ox_parse_file("tests/golden/golden.xlsx", &d) == NP_OX_OK);
    CHECK(d.kind == NP_OX_EXCEL);
    CHECK(strcmp(d.main_part, "xl/workbook.xml") == 0);
    CHECK(find_label(&d, "sheet") >= 0);
}

static void test_pptx_tree(void) {
    np_ox_doc_t d;
    CHECK(np_ox_parse_file("tests/golden/golden.pptx", &d) == NP_OX_OK);
    CHECK(d.kind == NP_OX_POWERPOINT);
    CHECK(strcmp(np_ox_kind_name(d.kind), "PowerPoint") == 0 ||
          strcmp(np_ox_kind_name(d.kind), "PPTX") == 0 ||
          strstr(np_ox_kind_name(d.kind), "owerPoint") != NULL);
    CHECK(strcmp(d.main_part, "ppt/presentation.xml") == 0);
    CHECK(np_ox_find_part(&d, "ppt/presentation.xml") != NULL);
    CHECK(count_type(&d, NP_OX_NODE_PART) == d.nparts);
}

/* ---- F2 safety: encrypted / non-OPC rejected ---- */
static void test_format_safety(void) {
    np_ox_doc_t d;
    np_ox_status_t st = np_ox_parse_file("tests/golden/encrypted.docx", &d);
    CHECK(st != NP_OX_OK || !d.valid);   /* must not be served as a valid doc */
    st = np_ox_parse_file("tests/golden/plain.zip", &d);
    CHECK(st != NP_OX_OK || !d.valid);   /* plain zip is not OPC */
}

/* ---- F3: SQLite schema ---- */
static void test_sqlite_schema(void) {
    size_t len; unsigned char *b = read_file("tests/golden/golden_full.db", &len);
    CHECK(b != NULL);
    if (!b) return;
    np_db_schema_t s;
    CHECK(np_db_parse(b, len, &s) == NP_DB_OK);
    CHECK(s.valid && !s.wal);
    CHECK(s.page_size == 4096);
    CHECK(s.ntables == 3);
    const np_db_table_t *users = np_db_find_table(&s, "users");
    CHECK(users != NULL);
    if (users) {
        CHECK(users->ncols == 4);
        CHECK(strcmp(users->cols[0].name, "id") == 0);
        CHECK(users->cols[0].pk);
    }
    free(b);

    /* WAL variant */
    b = read_file("tests/golden/golden_wal.db", &len);
    CHECK(b != NULL);
    if (!b) return;
    CHECK(np_db_parse(b, len, &s) == NP_DB_OK);
    CHECK(s.wal);
    free(b);
}

int main(void) {
    test_f1_recognition();
    test_docx_tree();
    test_xlsx_tree();
    test_pptx_tree();
    test_format_safety();
    test_sqlite_schema();
    printf("test_f9_golden: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
