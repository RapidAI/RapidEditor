#ifndef NP_CITATION_H
#define NP_CITATION_H

/*
 * np_citation.h - B5: unified citation protocol + resolver.
 *
 * Three citation forms (design doc section 09), each mapped to a track:
 *   file:line          -> text     (1-based line numbers)
 *   file:0xOFFSET      -> binary   (hex byte offset, half-open range)
 *   file#struct-path   -> format   (part/cell/table path)
 * Plus external knowledge sources (X11): provider#doc-id.
 *
 * ANTI-HALLUCINATION RULE: a citation's excerpt is NEVER trusted from the
 * model. The resolver reads bytes back from a local reader callback; if the
 * range is out of bounds or the reader fails, the citation is dropped.
 * Pure C99, no file I/O of its own (caller supplies the reader).
 */
#ifdef __cplusplus
extern "C" {
#endif

#include "np_contract.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    np_cite_kind_t kind;
    char     file[260];
    size_t   start;             /* line number (1-based) / byte offset  */
    size_t   end;               /* inclusive line / exclusive offset   */
    char     path[256];         /* struct-path for NP_CITE_STRUCT       */
    uint64_t revision;          /* document revision binding           */
    uint32_t excerpt_hash;      /* CRC32 of read-back excerpt          */
} np_citation_t;

/* Parse a citation string. Returns true and fills `out` on success.
 * Examples:
 *   "src/main.c:42"          -> LINE, start=end=42
 *   "src/main.c:10-20"       -> LINE, start=10 end=20
 *   "firmware.elf:0x401000"  -> OFFSET, start=end=0x401000
 *   "firmware.elf:0x10-0x20" -> OFFSET, start=0x10 end=0x20
 *   "report.docx#/w:tbl[0]"  -> STRUCT, path="/w:tbl[0]"
 *   "kb.example.com/doc#p2"  -> EXTERNAL, path="doc#p2" */
bool np_citation_parse(const char *text, np_citation_t *out);

/* Render a citation back to canonical string form into buf. */
void np_citation_format(const np_citation_t *c, char *buf, size_t cap);

/* Local read-back callback: given file + [start,end), write bytes into buf
 * (up to cap) and return the number of bytes read, or -1 on failure/OOB.
 * This is where the app reads from the real document or disk. */
typedef long (*np_cite_reader)(const char *file, size_t start, size_t end,
                               unsigned char *buf, size_t cap, void *user);

/* Resolve a citation against a local reader. On success:
 *   - reads the excerpt back locally (never trusts model-provided text)
 *   - fills excerpt_hash with CRC32 of the excerpt
 *   - returns true
 * Returns false (=> drop the citation) if the reader fails or range is OOB. */
bool np_citation_resolve(const np_citation_t *c, np_cite_reader reader,
                         void *user, unsigned char *out_buf, size_t cap,
                         size_t *out_len, uint32_t *out_hash);

#ifdef __cplusplus
}
#endif
#endif /* NP_CITATION_H */
