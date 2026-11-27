#ifndef NP_SEARCHHIT_H
#define NP_SEARCHHIT_H

/*
 * np_searchhit.h - X11: search-result citation, read-back and anti-injection.
 *
 * The retrieved body of a web page is UNTRUSTED INPUT: text inside it must
 * never be treated as system/developer instructions. This module:
 *   - keeps SearchHit lean (title/url/snippet/source/time/hash — NO body);
 *   - sanitizes fetched body before it ever reaches a prompt (prompt-injection
 *     patterns, control chars, PII via np_sp_redact, bounded truncation);
 *   - detects encoding markers (UTF-8/BOM/GBK label) and slices long pages by
 *     byte offset without ever trusting a model-supplied excerpt;
 *   - binds citations to a local read-back + CRC32 hash: a tampered or OOB
 *     excerpt is dropped (anti-hallucination, same rule as B5);
 *   - runs a fail-closed fallback chain across profiles, stamping which
 *     source actually answered.
 *
 * UI-free, no network of its own: the fetch transport is injected. C99.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define NP_SH_TITLE_CAP   160
#define NP_SH_URL_CAP     512
#define NP_SH_SNIPPET_C   512
#define NP_SH_SOURCE_CAP  128
#define NP_SH_HASH_CAP    32      /* hex CRC32 = 8 chars; room for sha-like */
#define NP_SH_BODY_CAP    (256u * 1024u)
#define NP_SH_MAX_HITS    50
#define NP_SH_MAX_SLICES  64

typedef enum {
    NP_SH_ENC_UTF8 = 0,
    NP_SH_ENC_UTF8_BOM,
    NP_SH_ENC_GBK_LABEL,   /* declaration seen; bytes NOT transcoded */
    NP_SH_ENC_UNKNOWN
} np_sh_encoding_t;

const char *np_sh_encoding_name(np_sh_encoding_t e);

/* A lean search hit. Deliberately has no `body` field — fetched content goes
 * through np_sh_fetch + np_sh_sanitize separately and is never cached here. */
typedef struct {
    char     title[NP_SH_TITLE_CAP];
    char     url[NP_SH_URL_CAP];
    char     snippet[NP_SH_SNIPPET_C];
    char     source[NP_SH_SOURCE_CAP];   /* provider/profile name (provenance) */
    int64_t  fetched_at;                /* unix seconds, 0 = unknown */
    uint32_t hash;                      /* CRC32 of (title|url|snippet) binding */
    np_sh_encoding_t encoding;
} np_search_hit_t;

/* A sanitized, offset-addressable slice of a fetched body. */
typedef struct {
    size_t   offset;                    /* byte offset in the original body */
    size_t   length;                    /* bytes in `text` */
    char     text[4096];                /* sanitized, NUL-terminated */
    uint32_t hash;                      /* CRC32 of sanitized text */
    np_sh_encoding_t encoding;
} np_sh_slice_t;

/* Fetch callback: write the raw body into `out` (up to cap), return bytes, or
 * -1 on failure. `user` is transport-owned. No network dependency here. */
typedef long (*np_sh_fetch_fn)(const char *url, unsigned char *out, size_t cap,
                               void *user);

/* Encoding detection from BOM / <meta charset> markers. Detection is honest:
 * GBK_LABEL means a label was seen but bytes are not transcoded — callers must
 * not feed them as UTF-8. */
np_sh_encoding_t np_sh_detect_encoding(const unsigned char *data, size_t len);

/* Sanitize untrusted body text into `out`: strip control chars, neutralize
 * common prompt-injection phrasings (marked [INJECT] rather than obeyed),
 * redact PII (np_sp_redact), bounded copy. Returns bytes written. */
size_t np_sh_sanitize(const char *in, size_t len, char *out, size_t cap);

/* Build a lean hit from raw fields; computes the binding hash. The snippet is
 * sanitized (snippets are search-provider text = untrusted too). */
np_search_hit_t np_sh_make_hit(const char *title, const char *url,
                               const char *snippet, const char *source,
                               int64_t fetched_at, np_sh_encoding_t enc);

/* Fetch + sanitize + slice a body into offset-addressable slices. `offset`
 * is the byte offset in the RAW body; each slice's text is sanitized. Returns
 * the number of slices, or -1 on fetch failure. */
int np_sh_fetch_slices(np_sh_fetch_fn fetch, void *user, const char *url,
                       size_t offset, size_t want_len,
                       np_sh_slice_t *out, size_t cap,
                       char *err, size_t err_cap);

/* Reference binding: a citation is provider#doc-id#offset. Resolve reads the
 * body back through `fetch`, sanitizes the slice at `offset`, and verifies
 * `expect_hash` (CRC32 of sanitized text). A mismatch / OOB / fetch failure
 * drops the reference (returns false) — never trust a model-supplied excerpt. */
typedef struct {
    char     provider[NP_SH_SOURCE_CAP];
    char     doc_id[NP_SH_URL_CAP];
    size_t   offset;
    size_t   length;
    uint32_t expect_hash;
} np_sh_reference_t;

bool np_sh_resolve_ref(const np_sh_reference_t *ref, np_sh_fetch_fn fetch,
                       void *user, char *out, size_t cap,
                       char *err, size_t err_cap);

/* Fallback chain: try each fetch in order; first success wins and `source_out`
 * receives that provider's name. Every failure is fail-closed (no silent
 * fallback to an unconfigured provider). Returns true on first success. */
typedef struct {
    const char *name;
    np_sh_fetch_fn fetch;
    void *user;
} np_sh_provider_t;

bool np_sh_fetch_chain(const np_sh_provider_t *providers, size_t count,
                       const char *url, unsigned char *out, size_t cap,
                       char *source_out, size_t source_cap,
                       size_t *out_len, char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_SEARCHHIT_H */
