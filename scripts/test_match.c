/* Regression test for np_match (find/replace matching semantics).
 * The function body is extracted verbatim from src/rapideditor.c by
 * run_tests.ps1 into np_match.impl, so the test always exercises the
 * real implementation. Covers both case modes, ASCII folding limits and
 * UTF-8 multi-byte safety - the same semantics the editor find bar now
 * honours via cui_code_editor_find_case.
 *
 * NOTE: np_match compares the n bytes at the given position - it is the
 * per-position predicate of a scan loop (callers pass text+i), so every
 * case below starts AT the match offset. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include "np_match.impl"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("ok  - %s\n", msg); \
    else { printf("FAIL- %s\n", msg); fails++; } } while (0)

static void m(const char *p, const char *needle, bool mc, bool expect,
              const char *msg) {
    bool got = np_match(p, needle, strlen(needle), mc);
    CHECK(got == expect, msg);
    if (got != expect)
        printf("      p=[%s] needle=[%s] mc=%d expect=%d got=%d\n",
               p, needle, (int)mc, (int)expect, (int)got);
}

int main(void) {
    /* case-insensitive (default find bar behaviour) */
    m("Hello World", "hello", false, true, "ci: prefix match ignores case");
    m("Hello World", "HELLO", false, true, "ci: uppercase needle matches");
    m("heLLo", "hello", false, true, "ci: mixed case folds");
    m("World", "world", false, true, "ci: lowercase needle at offset");

    /* case-sensitive (match_case=true, dialog + find bar setting) */
    m("Hello World", "hello", true, false, "cs: case mismatch rejected");
    m("hello World", "hello", true, true, "cs: exact case matches");
    m("HELLO", "HELLO", true, true, "cs: uppercase vs uppercase");

    /* ASCII folding must not leak into UTF-8 continuation bytes */
    m("\xC3\x89" "a", "\xC3\xA9" "a", false, false,
      "utf8: E-acute != e-acute in ci mode (0x89 vs 0xA9)");
    m("\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C",
      "\xE5\xA5\xBD\xE4\xB8\x96", false, true, "utf8: Chinese ci match");
    m("\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C",
      "\xE5\xA5\xBD\xE4\xB8\x96", true, true, "utf8: Chinese cs match");

    /* terminator and boundary behaviour */
    m("ab", "abc", false, false, "haystack shorter than needle");
    m("abc", "abc", true, true, "exact full-string match");
    m("aXc", "abc", false, false, "middle mismatch");
    m("", "a", false, false, "empty haystack");

    printf(fails ? "\n%d FAILURES\n" : "\nALL TESTS PASSED (%d failures)\n",
           fails);
    return fails ? 1 : 0;
}
