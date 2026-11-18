/* np_synctex.h - minimal SyncTeX reader for preview -> source navigation.
 *
 * Only the parts we need are parsed: Input: file table and the h/g/k/v box
 * records with their page and source line. No external library is required;
 * LaTeX is compiled with -synctex=-1 so the file is plain text.
 */
#ifndef NP_SYNCTEX_H
#define NP_SYNCTEX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct np_synctex np_synctex;

/* Returns NULL when the file cannot be read or contains no usable records. */
np_synctex *np_synctex_load(const char *path);
void np_synctex_free(np_synctex *stx);

/* x/y are PDF points measured from the top-left of the page. Returns 1 when
 * a source position was found and fills file/line/col (col is best effort). */
int np_synctex_locate(np_synctex *stx, int page, float x, float y,
                      char *file, size_t file_cap, int *line, int *col);

#ifdef __cplusplus
}
#endif
#endif
