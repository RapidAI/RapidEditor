/* Regression test for np_inline_clean (inline completion result cleaning).
 * The function body is extracted verbatim from src/rapideditor.c by
 * run_tests.ps1 into np_inline_clean.impl, so the test always exercises
 * the real implementation and never goes stale. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "np_inline_clean.impl"
static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("ok  - %s\n", msg); \
    else { printf("FAIL- %s\n", msg); fails++; } } while (0)
static void t(char *in, const char *expect, const char *msg) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", in);
    char *got = np_inline_clean(buf);
    bool ok = got && strcmp(got, expect) == 0;
    CHECK(ok, msg);
    if (!ok) printf("      in=[%s] expect=[%s] got=[%s]\n", in, expect,
                    got ? got : "(null)");
}
int main(void) {
    t("print(1)", "print(1)", "plain text passes through");
    t("  hello  ", "hello", "leading/trailing spaces trimmed");
    t("hello\r\n", "hello", "crlf trimmed");
    t("first\nsecond", "first", "multiline keeps only first line");
    t("```int x```", "int x", "one-line code fence unwrapped");
    t("```int x``", "```int x``", "unbalanced fence untouched");
    t("````", "````", "fence markers only: untouched (>=6 required)");
    t("a\n```b```", "a", "first line wins before fence logic");
    {
        char b[64];
        snprintf(b, sizeof b, "   \n\t  ");
        CHECK(np_inline_clean(b) == NULL, "whitespace-only yields NULL");
        char c[8] = "";
        CHECK(np_inline_clean(c) == NULL, "empty input yields NULL");
    }
    printf("%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED",
           fails);
    return fails != 0;
}
