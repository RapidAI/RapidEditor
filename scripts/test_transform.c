/* Extracted-implementation regression for np_row_tab2sp / np_row_sp2tab
 * (Tab<->space line transforms). The build extracts the real function
 * bodies from src/rapideditor.c into .impl files; if extraction or
 * compilation fails the regression fails. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np_row_tab2sp.impl"
#include "np_row_sp2tab.impl"

static int g_fail = 0;

static void t(const char *in, const char *want, const char *name) {
    char got[256];
    memset(got, 0xEE, sizeof got);
    size_t n = np_row_tab2sp(in, got, 4);
    got[n] = 0;
    if (strcmp(got, want) != 0 || n != strlen(want)) {
        printf("FAIL-  %s\n  in:   \"%s\"\n  want: \"%s\"\n  got:  \"%s\"\n",
               name, in, want, got);
        g_fail++;
    }
}

static void s2t(const char *in, const char *want, const char *name) {
    char got[256];
    memset(got, 0xEE, sizeof got);
    size_t n = np_row_sp2tab(in, got, 4);
    got[n] = 0;
    if (strcmp(got, want) != 0 || n != strlen(want)) {
        printf("FAIL-  %s\n  in:   \"%s\"\n  want: \"%s\"\n  got:  \"%s\"\n",
               name, in, want, got);
        g_fail++;
    }
}

int main(void) {
    /* --- Tab -> spaces (tab width 4, byte columns) --- */
    t("\tA", "    A", "tab at col 0 -> 4 spaces");
    t("\t\t", "        ", "two tabs -> 8 spaces");
    t("a\tb", "a   b", "tab after 1 char -> stop at 4");
    t("abc\t", "abc ", "tab lands on the next stop already");
    t("ab", "ab", "no tabs, untouched");
    t("", "", "empty row");
    /* Editor counts bytes: CJK char = 3 columns */
    t("\xE4\xB8\xAD\t", "\xE4\xB8\xAD ", "CJK 3 bytes, tab -> col 4 (1 space)");
    t("\t\xE4\xB8\xAD", "    \xE4\xB8\xAD", "tab then CJK");

    /* --- leading spaces -> tabs --- */
    s2t("    x", "\tx", "4 leading spaces -> 1 tab");
    s2t("        x", "\t\tx", "8 leading spaces -> 2 tabs");
    s2t("      x", "\t  x", "6 spaces -> tab + 2 kept");
    s2t("   x", "   x", "3 spaces below a stop stay spaces");
    s2t("     x", "\t x", "5 spaces -> tab + 1 kept");
    s2t("  \t x", "\t x", "tab absorbs the pending 2 spaces");
    s2t("a    b", "a    b", "mid-line spaces are never touched");
    s2t("\t    \t", "\t\t\t", "tab+4sp+tab = 12 columns = 3 tabs");
    s2t("", "", "empty row");

    if (g_fail == 0) printf("PASSED  test_transform (18 cases)\n");
    else printf("FAILED  test_transform: %d failures\n", g_fail);
    return g_fail ? 1 : 0;
}
