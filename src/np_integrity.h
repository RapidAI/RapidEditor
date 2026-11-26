#ifndef NP_INTEGRITY_H
#define NP_INTEGRITY_H

/*
 * np_integrity.h - H2: byte-integrity guard.
 *
 * Guards every AI-driven binary write with:
 *   - length check (detect unexpected size change, support expected delta)
 *   - CRC32 + SHA-256 of the whole buffer (before/after)
 *   - header/footer allow-list: protected byte regions that must not change
 *   - expected_hash: the hash the AI patch was computed against (stale check)
 *
 * Any mismatch -> caller MUST rollback the patch (the guard reports it, it
 * never writes bytes itself). Pure C99, self-contained (no OpenSSL).
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t crc32;
    unsigned char sha256[32];
    size_t   len;
} np_digest_t;

/* Streaming-style one-shot digests. NULL data with len 0 is valid. */
uint32_t np_crc32(const void *data, size_t len);
void     np_sha256(const void *data, size_t len, unsigned char out[32]);
np_digest_t np_digest_compute(const void *data, size_t len);
bool     np_digest_equal(const np_digest_t *a, const np_digest_t *b);

/* Streaming SHA-256 for large files (S1/T3: verify downloaded packages
 * without loading them into memory). */
typedef struct {
    uint32_t      h[8];
    unsigned char block[64];
    size_t        block_len;
    uint64_t      total;
} np_sha256_stream_t;

void np_sha256_begin(np_sha256_stream_t *ctx);
void np_sha256_stream_update(np_sha256_stream_t *ctx, const void *data, size_t len);
void np_sha256_stream_final(np_sha256_stream_t *ctx, unsigned char out[32]);

/* Render sha256 as 64 lowercase hex chars into out (must be >= 65 bytes). */
void np_sha256_hex(const unsigned char sha[32], char out_hex[65]);

/* A protected region: bytes at [offset, offset+len) that a patch must not
 * change. Used for magic numbers / headers / trailers (PE MZ, ELF, PNG...). */
typedef struct {
    size_t         offset;
    size_t         len;       /* <= 16 */
    unsigned char  bytes[16];
    char           name[32];  /* e.g. "ELF magic" */
} np_protected_t;

/* Capture a write guard BEFORE applying a patch.
 * `regions` / `nregions` describe the header/footer allow-list (may be NULL). */
typedef struct np_write_guard np_write_guard_t;
np_write_guard_t *np_guard_begin(const void *before, size_t before_len,
                                 const np_protected_t *regions, size_t nregions);
void np_guard_free(np_write_guard_t *g);

/* Result of checking a patched buffer against the guard. */
typedef enum {
    NP_GUARD_OK = 0,
    NP_GUARD_ERR_STALE,      /* before bytes don't match expected_hash        */
    NP_GUARD_ERR_LENGTH,     /* size changed unexpectedly                     */
    NP_GUARD_ERR_REGION,     /* a protected header/footer region changed      */
    NP_GUARD_ERR_ARG
} np_guard_result_t;

const char *np_guard_result_name(np_guard_result_t r);

/* Check a candidate `after` buffer.
 *   expected_hash_hex : 64-hex SHA-256 the patch was authored against, or
 *                       NULL/"" to skip the stale check.
 *   expected_len_delta: signed size change the patch claims (e.g. +2/-3),
 *                       or INT32_MIN sentinel to skip the length check.
 * On NP_GUARD_OK the patch is safe to keep; any other result => rollback. */
#define NP_GUARD_SKIP_DELTA  ((int32_t)0x80000000)
np_guard_result_t np_guard_check(np_write_guard_t *g,
                                 const void *after, size_t after_len,
                                 const char *expected_hash_hex,
                                 int32_t expected_len_delta);

/* Convenience: build a protected region from a literal byte prefix of data. */
np_protected_t np_protected_make(size_t offset, const unsigned char *bytes,
                                 size_t len, const char *name);

#ifdef __cplusplus
}
#endif
#endif /* NP_INTEGRITY_H */
