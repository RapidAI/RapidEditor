/* np_latex_html.h - lightweight LaTeX -> HTML converter.
 *
 * Renders a common article-style LaTeX subset into a standalone HTML page
 * (KaTeX for math) for the preview pane on platforms without a TeX toolchain
 * (Android). This is NOT a LaTeX compiler: unsupported macros are dropped,
 * so the output approximates the document instead of reproducing it.
 */
#ifndef NP_LATEX_HTML_H
#define NP_LATEX_HTML_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Convert `tex` (len bytes, UTF-8) into a NUL-terminated malloc'd HTML page.
 * Returns NULL on allocation failure; *out_len (optional) gets the byte
 * length excluding the NUL. The caller frees the result. */
char *np_latex_html(const char *tex, size_t len, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
