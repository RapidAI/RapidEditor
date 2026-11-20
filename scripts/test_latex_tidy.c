#include "../src/np_latex_tidy.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)
#define TIDY(src, opts) do { char *_r=NULL; np_latex_tidy((src), strlen(src), (opts), &_r); out=_r; } while (0)

int main(void) {
    char *out = NULL;
    np_tidy_opts o; memset(&o, 0, sizeof o);
    o.normalize_indent = true; o.join_hard_wraps = true; o.trim_trailing_ws = true; o.indent_spaces = 4;

    /* 1. Indentation by env depth */
    TIDY("\\documentclass{article}\n\\begin{document}\n\\begin{itemize}\n\\item hello\n\\end{itemize}\n\\end{document}\n", &o);
    CHECK(strstr(out, "    \\begin{itemize}") != NULL, "nested begin indented 4");
    CHECK(strstr(out, "        \\item hello") != NULL, "item indented 8");
    CHECK(strstr(out, "    \\end{itemize}") != NULL, "end matches begin depth");
    free(out);

    /* 2. Hard-wrap joining: two plain text lines merge */
    o.normalize_indent = false;
    TIDY("This is a long sentence\nthat was hard wrapped.\n\nSecond paragraph.\n", &o);
    CHECK(strstr(out, "This is a long sentence that was hard wrapped.") != NULL, "soft lines joined with space");
    CHECK(strstr(out, "wrapped.\n\nSecond") != NULL, "blank line preserved between paragraphs");
    free(out);

    /* 3. Verbatim protected byte-for-byte (indent + trailing spaces + hard wrap) */
    o.normalize_indent = true; o.join_hard_wraps = true; o.trim_trailing_ws = true;
    const char *verb = "\\begin{verbatim}\n  int  x = 1;   \n    raw   spaces\n\\end{verbatim}\n";
    TIDY(verb, &o);
    CHECK(strstr(out, "  int  x = 1;   \n") != NULL, "verbatim keeps leading+trailing spaces");
    CHECK(strstr(out, "    raw   spaces\n") != NULL, "verbatim keeps hard wrap and inner spaces");
    free(out);

    /* 4. Math env protected */
    const char *math = "\\begin{align}\n  a &= b \\\\\n   &= c\n\\end{align}\n";
    TIDY(math, &o);
    CHECK(strstr(out, "  a &= b") != NULL && strstr(out, "   &= c") != NULL, "align content untouched");
    free(out);

    /* 5. Comment lines protected */
    const char *cmt = "normal line   \n% keep   this  comment  \ntext\n";
    TIDY(cmt, &o);
    CHECK(strstr(out, "% keep   this  comment  \n") != NULL, "comment keeps trailing spaces");
    CHECK(strstr(out, "normal line\n") != NULL, "ordinary line trailing ws trimmed");
    free(out);

    /* 6. $$ display math line not joined */
    const char *disp = "text before\n$$ x + y $$\ntext after\n";
    TIDY(disp, &o);
    CHECK(strstr(out, "text before\n$$ x + y $$\ntext after") != NULL, "display math lines not joined");
    free(out);

    /* 7. Escaped percent not a comment */
    const char *esc = "a 50\\% value  \nnext\n";
    TIDY(esc, &o);
    CHECK(strstr(out, "a 50\\% value next") != NULL, "escaped percent line treated as ordinary (joined)");
    free(out);

    printf("\n%s (%d failures)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
