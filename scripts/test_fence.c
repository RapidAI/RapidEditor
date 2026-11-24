/* Regression test for np_ai_strip_code_fence — the function body is
 * extracted verbatim from src/rapideditor.c at build time, so the test
 * always exercises the real implementation. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
typedef unsigned char bool_;
static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("ok  - %s\n", msg); \
    else { printf("FAIL- %s\n", msg); fails++; } } while (0)
static void np_ai_strip_code_fence(char *buf, size_t cap) {
    size_t n = strlen(buf);
    if (n < 8 || strncmp(buf, "```", 3) != 0) return;
    char *first_nl = strchr(buf, '\n');
    if (!first_nl) return;
    /* Trim trailing newline bytes, then require the last line to be ``` */
    char *end = buf + n;
    while (end > buf && (end[-1] == '\n' || end[-1] == '\r')) end--;
    if (end - buf < 3 || strncmp(end - 3, "```", 3) != 0) return;
    char *last = end - 3;                /* start of the closing fence line */
    if (last <= first_nl || last[-1] != '\n') return;
    /* Any other fence-looking line in between (```anything) means the
     * fences may be real content — do not unwrap at all. */
    for (char *p = first_nl + 1; p + 3 < last; p++) {
        if (p[0] == '\n' && strncmp(p + 1, "```", 3) == 0)
            return;
    }
    /* Also check the very first inner line (right after the language tag). */
    if (first_nl + 4 <= last && strncmp(first_nl + 1, "```", 3) == 0) return;
    /* Non-empty content between the fences only. */
    if (first_nl + 1 >= last - 1) return;
    size_t clen = (size_t)(last - 1 - (first_nl + 1));
    memmove(buf, first_nl + 1, clen);
    buf[clen] = 0;
}
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
