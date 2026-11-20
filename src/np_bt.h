#ifndef NP_BT_H
#define NP_BT_H

/* Self-contained interpreter for a practical subset of 010 Editor's Binary
 * Template scripting language. A template is a small C-like program that
 * walks a byte buffer, declares typed fields and produces a list of named
 * ranges ("fields") with their offset, size and formatted value. RapidEditor
 * uses the fields to highlight bytes in the hex view and to build a
 * clickable field tree that jumps to the corresponding offset.
 *
 * Supported syntax:
 *   - scalar types: char, byte, ubyte, short, ushort, int, uint,
 *     int64, uint64, float, double, string, wstring
 *   - fixed arrays:  char sig[4];  uint32 values[count];
 *   - struct / enum definitions, nested structs
 *   - statements: declarations, assignment, if / else, while, for,
 *     break, continue, blocks
 *   - operators: arithmetic, bitwise, comparison, logical, ternary,
 *     ++ / --, compound assignment
 *   - builtins: FTell(), FSeek(off), FSize(), FEof(), BigEndian(),
 *     LittleEndian(), ReadByte()/ReadUShort()/... , Abs(), Min(), Max()
 *
 * No dependencies beyond the C runtime. The caller owns the result and must
 * release it with np_bt_result_free(). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One parsed field. Arrays and struct members are flattened into a parent
 * with children; `depth` and `parent` rebuild the tree for display. */
typedef struct np_bt_field {
    char     name[128];      /* e.g. "IHDR.width", "chunks[2].type" */
    char     type[40];       /* declared type, e.g. "uint32", "char[4]" */
    char     value[160];     /* formatted value */
    size_t   offset;         /* byte offset in the buffer */
    size_t   size;           /* byte length */
    int      depth;          /* 0 = top level, 1 = first nested level */
    int      parent;         /* index of parent field, -1 for top level */
    int      color;          /* 0..7 stable tone for hex highlighting */
} np_bt_field;

typedef struct np_bt_result {
    np_bt_field *fields;
    int          count;
    int          cap;
    char         error[256]; /* set when ok == false */
    bool         ok;
} np_bt_result;

/* Execute `script` over `data`/`size`. Always initialises `out` (even on
 * failure). Returns the same as out->ok. A failed run still frees every
 * internal allocation; the caller only needs np_bt_result_free(). */
bool np_bt_execute(const char *script, const uint8_t *data, size_t size,
                   np_bt_result *out);

void np_bt_result_free(np_bt_result *out);

/* Convenience: verify a script parses (executes against an empty buffer).
 * Useful for template validation without a file. */
bool np_bt_validate(const char *script, char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* NP_BT_H */
