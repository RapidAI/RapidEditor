/* test_latex_html.c - standalone tests for np_latex_html.c.
 * Build: cc -o test_latex_html test_latex_html.c np_latex_html.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "np_latex_html.h"

static int g_fail;

static void check(const char *name, const char *html, const char *needle) {
    if (html && strstr(html, needle)) {
        printf("PASS %s\n", name);
    } else {
        printf("FAIL %s\n  wanted: %s\n", name, needle ? needle : "(null)");
        g_fail++;
    }
}

static void check_absent(const char *name, const char *html, const char *needle) {
    if (!html || !strstr(html, needle)) {
        printf("PASS %s\n", name);
    } else {
        printf("FAIL %s\n  must not contain: %s\n", name, needle);
        g_fail++;
    }
}

static const char *DOC =
    "\\documentclass{article}\n"
    "\\usepackage{amsmath,graphicx}\n"
    "\\title{A Small \\LaTeX{} Article}\n"
    "\\author{Your Name \\and Other}\n"
    "\\date{\\today}\n"
    "\\begin{document}\n"
    "\\maketitle\n"
    "\n"
    "\\begin{abstract}\n"
    "Short intro with $E=mc^2$ inline math.\n"
    "\\end{abstract}\n"
    "\n"
    "% this comment must vanish\n"
    "\\section{Introduction}\n"
    "Hello ``world'' -- ok --- done.\n"
    "\\label{sec:intro}\n"
    "See Section\\ref{sec:intro} and \\eqref{eq:one}.\n"
    "\\textbf{bold} and \\textit{ital} and \\texttt{mono}.\n"
    "\n"
    "\\subsection{Background}\n"
    "\\begin{itemize}\n"
    "  \\item first\n"
    "  \\item second\n"
    "  \\begin{enumerate}\n"
    "    \\item nested\n"
    "  \\end{enumerate}\n"
    "\\end{itemize}\n"
    "\n"
    "\\begin{description}\n"
    "  \\item[Term] explained here.\n"
    "\\end{description}\n"
    "\n"
    "\\begin{equation}\n"
    "  \\label{eq:one}\n"
    "  a^2 + b^2 = c^2\n"
    "\\end{equation}\n"
    "\n"
    "\\begin{verbatim}\n"
    "\\section { this is NOT a section }\n"
    "\\end{verbatim}\n"
    "\n"
    "\\begin{tabular}{llr}\n"
    "\\hline\n"
    "A & B & 1 \\\\\n"
    "\\multicolumn{2}{c}{Span} & 2 \\\\\n"
    "\\bottomrule\n"
    "\\end{tabular}\n"
    "\n"
    "\\begin{figure}\n"
    "\\includegraphics[width=3cm]{img.png}\n"
    "\\caption{A figure caption}\n"
    "\\end{figure}\n"
    "\n"
    "\\unknowncmd{kept argument}\n"
    "\\vspace{1em}\n"
    "End of doc.\n"
    "\\end{document}\n";

int main(void) {
    size_t n = 0;
    char *h = np_latex_html(DOC, strlen(DOC), &n);
    if (!h) { printf("FAIL alloc\n"); return 1; }
    printf("html bytes: %zu\n", n);
    if (getenv("NP_DUMP")) {
        FILE *d = fopen(getenv("NP_DUMP"), "wb");
        if (d) { fwrite(h, 1, n, d); fclose(d); }
    }

    check("title block", h, "<h1>A Small LaTeX Article</h1>");
    check("author joined", h, "Your Name");
    check("abstract", h, "np-abstract");
    check("section numbered", h, "<h2>1");
    check("subsection numbered", h, "<h3>1.1");
    check_absent("comment gone", h, "this comment must vanish");
    check("smart quote open", h, "\xe2\x80\x9c");
    check("en dash", h, "\xe2\x80\x93");
    check("em dash", h, "\xe2\x80\x94");
    check("label anchor", h, "id=\"lbl-sec:intro\"");
    check("ref link", h, "href=\"#lbl-sec:intro\"");
    check("inline math attr", h, "data-tex=\"E=mc^2\"");
    check("equation math", h, "data-tex=\"a^2 + b^2 = c^2\"");
    check("bold tag", h, "<b>bold</b>");
    check("italic tag", h, "<i>ital</i>");
    check("tt tag", h, "<code>mono</code>");
    check("itemize li", h, "<li>first");
    check("nested ol", h, "<ol>");
    check("description dt", h, "<dt>Term</dt>");
    check("verbatim preserved", h, "\\section { this is NOT a section }");
    check_absent("verbatim not parsed", h, "<h2> this is NOT");
    check("table tag", h, "np-table");
    check("multicolumn colspan", h, "colspan=\"2\"");
    check("caption", h, "A figure caption");
    check("includegraphics placeholder", h, "img.png");
    check("unknown cmd keeps arg", h, "kept argument");
    check_absent("unknown cmd name dropped", h, "unknowncmd");
    check_absent("vspace arg dropped", h, "1em");
    check("katex css local", h, "/katex/katex.min.css");
    check("katex js local", h, "/katex/katex.min.js");
    check("render script", h, "katex.render");
    check("lite note", h, "np-lite-note");

    /* Snippet mode: no \begin{document}. */
    const char *snip = "\\section*{X}\nBody text with $x$ math.\n";
    size_t n2 = 0;
    char *h2 = np_latex_html(snip, strlen(snip), &n2);
    if (!h2) { printf("FAIL alloc2\n"); return 1; }
    check("snippet section", h2, "X");
    check("snippet math", h2, "data-tex=\"x\"");
    check("snippet body", h2, "Body text with");
    free(h2);

    /* Malformed: unterminated math and env must not hang or crash. */
    const char *bad = "\\begin{itemize}\\item oops $open\n\\begin{tabular}{cc}a & b\\\\ c & d\n";
    char *h3 = np_latex_html(bad, strlen(bad), &n2);
    check("malformed survives", h3, "oops");
    free(h3);

    /* Empty input. */
    char *h4 = np_latex_html("", 0, &n2);
    check("empty ok", h4, "<!doctype html>");
    free(h4);

    free(h);
    printf(g_fail ? "\n%d FAILURES\n" : "\nALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
