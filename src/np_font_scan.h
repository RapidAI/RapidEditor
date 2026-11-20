#ifndef NP_FONT_SCAN_H
#define NP_FONT_SCAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* Enumerate installed font FAMILY names for the fontspec/xeCJK picker.
 * Returns the number of names written into `out` (each NUL-terminated), or
 * -1 if out is too small. `out` is a flat char buffer; `cap` its byte size.
 * Results are sorted and de-duplicated. On Windows this uses
 * EnumFontFamiliesExW (real family names); elsewhere it parses `fc-list`
 * and/or scans common font directories (file names as a fallback). */
int np_font_scan_families(char *out, size_t cap, size_t per_name);

#ifdef __cplusplus
}
#endif
#endif
