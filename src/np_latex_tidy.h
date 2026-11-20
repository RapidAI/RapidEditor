#ifndef NP_LATEX_TIDY_H
#define NP_LATEX_TIDY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>

/* "Tidy source": normalize indentation, join hard-wrapped lines inside a
 * paragraph, and trim trailing whitespace -- while strictly protecting
 * verbatim/code/math environments, display math and comment lines. */

typedef struct {
    bool normalize_indent;   /* re-indent by \begin/\end nesting depth */
    bool join_hard_wraps;    /* merge soft-wrapped paragraph lines into one */
    bool trim_trailing_ws;   /* drop trailing spaces/tabs on ordinary lines */
    int  indent_spaces;      /* spaces per nesting level (0 => tab) */
} np_tidy_opts;

/* Tidy `src` (length n, need not be NUL-terminated) into a freshly malloc'd
 * NUL-terminated string returned via *out. Returns 0 on success, -1 on OOM.
 * Caller frees *out. */
int np_latex_tidy(const char *src, size_t n, const np_tidy_opts *opts, char **out);

#ifdef __cplusplus
}
#endif
#endif
