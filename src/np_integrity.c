/* np_integrity.c - H2 byte-integrity guard (see np_integrity.h). */
#include "np_integrity.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ---------------- CRC32 (IEEE 802.3, reflected) ---------------- */
static uint32_t crc_table[256];
static bool crc_ready = false;

static void crc_init(void) {
    if (crc_ready) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = true;
}

uint32_t np_crc32(const void *data, size_t len) {
    crc_init();
    uint32_t c = 0xFFFFFFFFu;
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; ++i) c = crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---------------- SHA-256 ---------------- */
typedef struct {
    uint32_t h[8];
    unsigned char block[64];
    size_t block_len;
    uint64_t total;
} sha_ctx_t;

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha_transform(sha_ctx_t *ctx, const unsigned char *p) {
    static const uint32_t K[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1 = rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    uint32_t a=ctx->h[0],b=ctx->h[1],c=ctx->h[2],d=ctx->h[3],
             e=ctx->h[4],f=ctx->h[5],g=ctx->h[6],h=ctx->h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e,6)^rotr(e,11)^rotr(e,25);
        uint32_t ch = (e&f)^((~e)&g);
        uint32_t t1 = h+S1+ch+K[i]+w[i];
        uint32_t S0 = rotr(a,2)^rotr(a,13)^rotr(a,22);
        uint32_t maj = (a&b)^(a&c)^(b&c);
        uint32_t t2 = S0+maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->h[0]+=a; ctx->h[1]+=b; ctx->h[2]+=c; ctx->h[3]+=d;
    ctx->h[4]+=e; ctx->h[5]+=f; ctx->h[6]+=g; ctx->h[7]+=h;
}

static void sha_update(sha_ctx_t *c, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    c->total += len;
    while (len) {
        size_t n = 64 - c->block_len;
        if (n > len) n = len;
        memcpy(c->block + c->block_len, p, n);
        c->block_len += n; p += n; len -= n;
        if (c->block_len == 64) { sha_transform(c, c->block); c->block_len = 0; }
    }
}

static void sha_final(sha_ctx_t *c, unsigned char out[32]) {
    uint64_t bits = c->total * 8;
    unsigned char pad = 0x80;
    sha_update(c, &pad, 1);
    unsigned char zero = 0;
    while (c->block_len != 56) sha_update(c, &zero, 1);
    unsigned char tail[8];
    for (int i = 0; i < 8; ++i) tail[i] = (unsigned char)(bits >> (56 - i*8));
    /* append length without updating total */
    for (int i = 0; i < 8; ++i) {
        c->block[c->block_len++] = tail[i];
        if (c->block_len == 64) { sha_transform(c, c->block); c->block_len = 0; }
    }
    for (int i = 0; i < 8; ++i) {
        out[i*4]   = (unsigned char)(c->h[i]>>24);
        out[i*4+1] = (unsigned char)(c->h[i]>>16);
        out[i*4+2] = (unsigned char)(c->h[i]>>8);
        out[i*4+3] = (unsigned char)(c->h[i]);
    }
}

void np_sha256(const void *data, size_t len, unsigned char out[32]) {
    sha_ctx_t c;
    c.h[0]=0x6a09e667u; c.h[1]=0xbb67ae85u; c.h[2]=0x3c6ef372u; c.h[3]=0xa54ff53au;
    c.h[4]=0x510e527fu; c.h[5]=0x9b05688cu; c.h[6]=0x1f83d9abu; c.h[7]=0x5be0cd19u;
    c.block_len = 0; c.total = 0;
    sha_update(&c, data, len);
    sha_final(&c, out);
}

/* Streaming API (S1/T3): same engine, public lifetime. */
void np_sha256_begin(np_sha256_stream_t *ctx) {
    if (!ctx) return;
    ctx->h[0]=0x6a09e667u; ctx->h[1]=0xbb67ae85u; ctx->h[2]=0x3c6ef372u; ctx->h[3]=0xa54ff53au;
    ctx->h[4]=0x510e527fu; ctx->h[5]=0x9b05688cu; ctx->h[6]=0x1f83d9abu; ctx->h[7]=0x5be0cd19u;
    ctx->block_len = 0; ctx->total = 0;
    memset(ctx->block, 0, sizeof(ctx->block));
}

void np_sha256_stream_update(np_sha256_stream_t *ctx, const void *data, size_t len) {
    if (!ctx) return;
    sha_update((sha_ctx_t *)ctx, data, len);
}

void np_sha256_stream_final(np_sha256_stream_t *ctx, unsigned char out[32]) {
    if (!ctx) return;
    sha_final((sha_ctx_t *)ctx, out);
}

np_digest_t np_digest_compute(const void *data, size_t len) {
    np_digest_t d;
    d.crc32 = np_crc32(data, len);
    d.len = len;
    np_sha256(data, len, d.sha256);
    return d;
}

bool np_digest_equal(const np_digest_t *a, const np_digest_t *b) {
    if (!a || !b) return false;
    return a->len == b->len && a->crc32 == b->crc32 &&
           memcmp(a->sha256, b->sha256, 32) == 0;
}

void np_sha256_hex(const unsigned char sha[32], char out_hex[65]) {
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out_hex[i*2]   = hex[sha[i] >> 4];
        out_hex[i*2+1] = hex[sha[i] & 15];
    }
    out_hex[64] = '\0';
}

/* ---------------- write guard ---------------- */
struct np_write_guard {
    np_digest_t    before;
    unsigned char *before_copy;
    size_t         before_len;
    np_protected_t *regions;
    size_t          nregions;
};

np_protected_t np_protected_make(size_t offset, const unsigned char *bytes,
                                 size_t len, const char *name) {
    np_protected_t r;
    memset(&r, 0, sizeof(r));
    r.offset = offset;
    r.len = len > 16 ? 16 : len;
    if (bytes && r.len) memcpy(r.bytes, bytes, r.len);
    if (name) snprintf(r.name, sizeof(r.name), "%s", name);
    return r;
}

np_write_guard_t *np_guard_begin(const void *before, size_t before_len,
                                 const np_protected_t *regions, size_t nregions) {
    np_write_guard_t *g = (np_write_guard_t *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->before = np_digest_compute(before, before_len);
    if (before_len) {
        g->before_copy = (unsigned char *)malloc(before_len);
        if (!g->before_copy) { free(g); return NULL; }
        memcpy(g->before_copy, before, before_len);
    }
    g->before_len = before_len;
    if (regions && nregions) {
        g->regions = (np_protected_t *)malloc(nregions * sizeof(*regions));
        if (g->regions) {
            memcpy(g->regions, regions, nregions * sizeof(*regions));
            g->nregions = nregions;
        }
    }
    return g;
}

void np_guard_free(np_write_guard_t *g) {
    if (!g) return;
    free(g->before_copy);
    free(g->regions);
    free(g);
}

const char *np_guard_result_name(np_guard_result_t r) {
    switch (r) {
        case NP_GUARD_OK:         return "ok";
        case NP_GUARD_ERR_STALE:  return "stale-hash";
        case NP_GUARD_ERR_LENGTH: return "length";
        case NP_GUARD_ERR_REGION: return "protected-region";
        case NP_GUARD_ERR_ARG:    return "arg";
        default:                  return "unknown";
    }
}

static bool hex_matches_sha(const char *hex, const unsigned char sha[32]) {
    if (!hex || !*hex) return true; /* no expected hash -> skip */
    if (strlen(hex) < 64) return false;
    static const char *hd = "0123456789abcdef";
    for (int i = 0; i < 64; ++i) {
        char c = hex[i];
        if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
        int v = -1;
        for (int k = 0; k < 16; ++k) if (hd[k] == c) { v = k; break; }
        if (v < 0) return false;
        int nibble = (i % 2 == 0) ? (sha[i/2] >> 4) : (sha[i/2] & 15);
        if (v != nibble) return false;
    }
    return true;
}

np_guard_result_t np_guard_check(np_write_guard_t *g,
                                 const void *after, size_t after_len,
                                 const char *expected_hash_hex,
                                 int32_t expected_len_delta) {
    if (!g || (!after && after_len)) return NP_GUARD_ERR_ARG;

    /* stale check: recompute the BEFORE digest from the captured copy and
     * compare it with the hash the AI patch says it was authored against. */
    if (expected_hash_hex && *expected_hash_hex) {
        if (!hex_matches_sha(expected_hash_hex, g->before.sha256))
            return NP_GUARD_ERR_STALE;
    }

    /* length check */
    if (expected_len_delta != NP_GUARD_SKIP_DELTA) {
        size_t expect = (expected_len_delta >= 0)
            ? g->before_len + (size_t)expected_len_delta
            : g->before_len - (size_t)(-expected_len_delta);
        if (after_len != expect) return NP_GUARD_ERR_LENGTH;
    }

    /* protected header/footer regions must be byte-identical */
    const unsigned char *p = (const unsigned char *)after;
    for (size_t i = 0; i < g->nregions; ++i) {
        const np_protected_t *r = &g->regions[i];
        if (r->offset > after_len || r->len > after_len - r->offset)
            return NP_GUARD_ERR_REGION;
        if (memcmp(p + r->offset, r->bytes, r->len) != 0)
            return NP_GUARD_ERR_REGION;
    }
    return NP_GUARD_OK;
}
