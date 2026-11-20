#include "../src/np_latex_table.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)

int main(void) {
    char err[256];
    np_latex_table t;
    char out[8192];

    /* 1. TSV from Excel: row1 = Alice,92,"a,b" -> cells[3..5] */
    const char *tsv = "Name\tScore\tNote\nAlice\t92\t\"a,b\"\nBob\t88\tx";
    CHECK(np_latex_table_parse(tsv, NULL, &t, err, sizeof err) == 0, "parse TSV ok");
    CHECK(t.rows == 3 && t.cols == 3, "TSV dims 3x3");
    CHECK(strcmp(t.cells[0], "Name") == 0, "TSV first cell");
    CHECK(strcmp(t.cells[5], "a,b") == 0, "quotes stripped around comma field");

    np_table_render_opts ro; memset(&ro, 0, sizeof ro);
    ro.booktabs = true; ro.first_row_header = true; ro.align = "lcr";
    ro.caption = "Scores"; ro.label = "tab:score";
    int n = np_latex_table_render(&t, &ro, out, sizeof out);
    CHECK(n > 0, "render booktabs ok");
    CHECK(strstr(out, "\\toprule") && strstr(out, "\\midrule") && strstr(out, "\\bottomrule"), "booktabs rules present");
    CHECK(strstr(out, "Name & Score & Note") != NULL, "header row rendered");
    CHECK(strstr(out, "Alice & 92 & a,b") != NULL, "cells rendered with stripped quotes");
    CHECK(strstr(out, "\\begin{tabular}{lcr}") != NULL, "align string applied");
    np_latex_table_free(&t);

    /* 2. CSV with CRLF, doubled quotes, ragged rows */
    const char *csv = "h1,h2,h3\r\n\"say \"\"hi\"\"\",2\r\n3,4,5,6\r\n";
    CHECK(np_latex_table_parse(csv, NULL, &t, err, sizeof err) == 0, "parse CSV CRLF ok");
    CHECK(t.rows == 3 && t.cols == 4, "CSV ragged padded to 3x4");
    CHECK(strcmp(t.cells[4], "say \"hi\"") == 0, "doubled quote decoded");
    CHECK(t.cells[6][0] == 0 && t.cells[7][0] == 0, "missing cells padded empty");
    memset(&ro, 0, sizeof ro);
    ro.booktabs = false; ro.first_row_header = true;
    n = np_latex_table_render(&t, &ro, out, sizeof out);
    CHECK(n > 0 && strstr(out, "\\hline"), "hline render ok");
    np_latex_table_free(&t);

    /* 3. Escaping specials */
    char e[512];
    np_latex_escape_cell("a&b_c%d$e~f^g\\h{x}", e, sizeof e);
    CHECK(strstr(e, "\\&") && strstr(e, "\\_") && strstr(e, "\\%") && strstr(e, "\\$") &&
          strstr(e, "\\textasciitilde{}") && strstr(e, "\\textasciicircum{}") &&
          strstr(e, "\\textbackslash{}") && strstr(e, "\\{"), "all specials escaped");

    /* 4. semicolon + BOM */
    const char *semi = "\xEF\xBB\xBF" "a;b;c\n1;2;3\n";
    CHECK(np_latex_table_parse(semi, NULL, &t, err, sizeof err) == 0, "parse BOM semicolon ok");
    CHECK(strcmp(t.cells[0], "a") == 0 && t.cols == 3, "BOM stripped, semicolon detected");
    np_latex_table_free(&t);

    /* 5. longtable */
    memset(&ro, 0, sizeof ro);
    ro.longtable = true; ro.booktabs = true;
    np_latex_table_parse("a,b\n1,2", NULL, &t, err, sizeof err);
    np_latex_table_render(&t, &ro, out, sizeof out);
    CHECK(strstr(out, "\\begin{longtable}") && strstr(out, "\\end{longtable}"), "longtable env emitted");
    np_latex_table_free(&t);

    /* 6. invalid input */
    CHECK(np_latex_table_parse("", NULL, &t, err, sizeof err) == -1, "empty rejected");
    CHECK(np_latex_table_parse("   \n  ", NULL, &t, err, sizeof err) == -1, "whitespace-only rejected");

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
