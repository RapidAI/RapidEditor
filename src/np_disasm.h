#ifndef NP_DISASM_H
#define NP_DISASM_H

/*
 * np_disasm.h - R2: multi-architecture disassembly view substrate.
 *
 * The editor never hard-links Capstone. Decoding is injected as a callback
 * (a Capstone adapter, an objdump parser adapter, or a fake in tests), so the
 * view logic - instruction list, offset<->instruction navigation, syntax /
 * mode switching, confidence gate, failing-over between decoders with source
 * provenance - is UI-free and fully testable with zero native dependencies.
 *
 * Responsibility boundary (per blueprint): the decoder decides whether the
 * decode is CORRECT; this module never guesses instructions. When the
 * architecture is unconfirmed (low confidence) it refuses to decode instead
 * of decoding bytes with the wrong ISA.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include "np_probe.h"       /* np_parch_t, np_probe_result_t */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Syntax / ARM mode selectors. Kept independent of any decoder library. */
typedef enum {
    NP_DSYNTAX_INTEL = 0,
    NP_DSYNTAX_ATT
} np_disasm_syntax_t;

typedef enum {
    NP_DMODE_ARM = 0,       /* ARM 32-bit ARM mode */
    NP_DMODE_THUMB          /* ARM Thumb/Thumb-2 */
} np_disasm_armmode_t;

typedef enum {
    NP_DSRC_NONE = 0,
    NP_DSRC_CAPSTONE,
    NP_DSRC_OBJDUMP,
    NP_DSRC_TEST           /* injected fake decoder (tests) */
} np_disasm_source_t;

const char *np_disasm_source_name(np_disasm_source_t s);

/* One decoded instruction. `offset` is the byte offset in the source buffer,
 * `size` its length in bytes, `text` the rendered mnemonic+operands. */
typedef struct {
    size_t          offset;
    size_t          size;
    uint64_t        addr;          /* virtual address (image_base + offset) */
    char            text[160];
    np_disasm_source_t source;
} np_disasm_insn_t;

/* Decoder callback. `data+off..len` is the byte window to decode; the decoder
 * appends instructions into `out` (up to `cap`) and returns the count, or -1
 * on failure / unsupported. `user` is decoder-owned state. `image_base` is
 * used for virtual addresses; syntax/mode/arch guide the decode. */
typedef int (*np_disasm_fn)(const unsigned char *data, size_t off, size_t len,
                            np_parch_t arch, np_disasm_syntax_t syntax,
                            np_disasm_armmode_t armmode, uint64_t image_base,
                            np_disasm_insn_t *out, size_t cap, void *user);

/* A decoder in the fallback chain: function + opaque user pointer + label. */
typedef struct {
    np_disasm_fn fn;
    void *user;
    np_disasm_source_t source;
} np_disasm_decoder_t;

/* Confidence gate: minimum probe confidence required before decoding. */
#define NP_DISASM_MIN_CONFIDENCE 70

typedef struct np_disasm_view np_disasm_view_t;

np_disasm_view_t *np_disasm_view_create(void);
void np_disasm_view_destroy(np_disasm_view_t *v);

/* Clear instructions but keep decoder chain / syntax / mode settings. */
void np_disasm_view_clear(np_disasm_view_t *v);

/* Set the decoder chain (copied). Decoders are tried in order until one
 * succeeds; the winning decoder's source is stamped on every instruction. */
void np_disasm_view_set_decoders(np_disasm_view_t *v,
                                 const np_disasm_decoder_t *decoders, size_t count);
void np_disasm_view_set_syntax(np_disasm_view_t *v, np_disasm_syntax_t s);
void np_disasm_view_set_armmode(np_disasm_view_t *v, np_disasm_armmode_t m);
np_disasm_syntax_t np_disasm_view_syntax(const np_disasm_view_t *v);
np_disasm_armmode_t np_disasm_view_armmode(const np_disasm_view_t *v);

/* Decode `len` bytes at `off` using the view's decoder chain. `probe` supplies
 * architecture / image_base / confidence. Returns true on success. If the
 * probe confidence < NP_DISASM_MIN_CONFIDENCE or arch is unknown, nothing is
 * decoded and false is returned (the caller must ask the user to pick an ISA).
 * `err` optionally receives a short reason. */
bool np_disasm_decode(np_disasm_view_t *v, const unsigned char *data,
                      size_t data_len, size_t off, size_t len,
                      const np_probe_result_t *probe,
                      char *err, size_t err_cap);

/* Access decoded instructions. */
size_t np_disasm_count(const np_disasm_view_t *v);
const np_disasm_insn_t *np_disasm_at(const np_disasm_view_t *v, size_t i);
np_disasm_source_t np_disasm_source(const np_disasm_view_t *v);

/* Bidirectional navigation.
 *  - insn_for_offset(v, off): index of the instruction covering `off`, or -1.
 *    An offset inside an instruction maps to that instruction.
 *  - offset_for_insn(v, i): start byte offset of instruction i, or SIZE_MAX. */
int np_disasm_insn_for_offset(const np_disasm_view_t *v, size_t off);
size_t np_disasm_offset_for_insn(const np_disasm_view_t *v, size_t i);

/* Parse an objdump-style text line into an instruction: "  401000:  48 89 5c 24 08  mov ..."
 * Accepts optional address prefix, 1..16 hex bytes, then mnemonic+operands.
 * Returns true and fills `ins` (source = NP_DSRC_OBJDUMP). */
bool np_disasm_parse_objdump_line(const char *line, np_disasm_insn_t *ins);

#ifdef __cplusplus
}
#endif
#endif /* NP_DISASM_H */
