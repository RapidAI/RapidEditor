#ifndef NP_ZIP_H
#define NP_ZIP_H

/* Minimal ZIP reader used to import multi-file LaTeX project templates.
 * Only STORE (method 0) and DEFLATE (method 8) entries are supported, which
 * covers GitHub / Overleaf template archives. The decoder is self-contained
 * (no zlib dependency) and all paths/sizes are validated before extraction. */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Extract `zip_path` into the existing directory `dest_dir`.
 * strip_root: drop a single common top-level folder (Overleaf/GitHub zips
 * usually wrap the project in one). Returns the number of files written, or
 * -1 on error (a human-readable reason is written to err/err_cap).
 * main_tex (optional) receives the best main-entry candidate found while
 * unpacking (a .tex containing \\documentclass, preferred stem first). */
int  np_zip_extract(const char *zip_path, const char *dest_dir, bool strip_root,
                    size_t max_total_bytes, int max_files,
                    char *main_tex, size_t main_tex_cap,
                    char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* NP_ZIP_H */
