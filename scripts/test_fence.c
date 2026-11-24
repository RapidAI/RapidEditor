/* Regression test for np_ai_strip_code_fence. The function body is
 * extracted verbatim from src/rapideditor.c by run_tests.ps1 into
 * np_ai_strip_code_fence.impl (found via the include path), so the test
 * always exercises the real implementation and never goes stale. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "np_ai_strip_code_fence.impl"
typedef unsigned char bool_;
static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("ok  - %s\n", msg); \
    else { printf("FAIL- %s\n", msg); fails++; } } while (0)
static void t(const char *in, const char *expect, const char *msg) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", in);
    np_ai_strip_code_fence(buf, sizeof buf);
    bool ok = strcmp(buf, expect) == 0;
    CHECK(ok, msg);
    if (!ok) printf("      in=[%s] expect=[%s] got=[%s]\n", in, expect, buf);
}
int main(void) {
    t("```python\nprint(1)\n```", "print(1)", "wrap with lang tag");
    t("```\nhello\n```", "hello", "wrap without lang tag");
    t("```json\n{\"a\": 1}\n```", "{\"a\": 1}", "json wrap");
    t("```\nhello\n```\n", "hello", "trailing newline stripped");
    t("plain text\nno fences", "plain text\nno fences", "no fence untouched");
    t("text:/n```c\nint x;\n```", "text:/n```c\nint x;\n```",
      "prose + fence: untouched");
    t("```\n```c\nx=1\n```\n```", "```\n```c\nx=1\n```\n```",
      "nested fences: untouched");
    t("```py\nA\n``` mid\n```", "```py\nA\n``` mid\n```",
      "inner fence line: untouched");
    t("```\n```\n", "```\n```\n", "empty block: untouched");
    t("```\nabc", "```\nabc", "no closing fence: untouched");
    t("```py\nprint(1)\n```extra", "```py\nprint(1)\n```extra",
      "text after closing fence: untouched");
    t("````\ncode\n````", "````\ncode\n````", "4-backtick fence: untouched");
    printf("%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED",
           fails);
    return fails != 0;
}
