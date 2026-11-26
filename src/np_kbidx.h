#ifndef NP_KBIDX_H
#define NP_KBIDX_H

/*
 * np_kbidx.h - X10: hybrid knowledge index + Context Pack.
 *
 * Design-doc X10: "本地 SQLite FTS5 + 可选向量索引；按来源/标签/质量/域名
 * 过滤；Context Pack 带 token 预算、Citation 与 provenance；未读回正文不进
 * 上下文".
 *
 * Implementation note (honest scope): this repo's np_sqlite is a READ-ONLY
 * SQLite parser, so there is no writable FTS5 table. X10 therefore ships a
 * self-contained BM25 inverted index (ASCII tokens + CJK unigrams) plus an
 * OPTIONAL injected embedding callback (cosine similarity) for hybrid
 * ranking. Functionally equivalent retrieval; no external dependency.
 *
 * Safety rules carried over from X9/X6:
 *   - Domain filter is fail-closed: empty allow-list blocks web sources;
 *     block-list wins over allow-list.
 *   - Only documents whose body was READ BACK (has_body) may enter a
 *     Context Pack; summary-only rows are counted as blocked, never inlined.
 *   - All body text is PII-redacted before it touches the pack buffer.
 *   - Pack generation is budgeted (tokens, estimated as chars/4) and bounded.
 *
 * Pure C99, no file I/O of its own except explicit save/load paths.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* ---------------- limits ---------------- */
#define NP_KB_MAX_DOCS      512
#define NP_KB_ID_CAP        80
#define NP_KB_SOURCE_CAP    260
#define NP_KB_DOMAIN_CAP    100
#define NP_KB_TAGS_CAP      160
#define NP_KB_TITLE_CAP     160
#define NP_KB_BODY_CAP      16384
#define NP_KB_LINE_CAP      4096
#define NP_KB_PACK_CAP      65536
#define NP_KB_MAX_POSTINGS  1024   /* distinct terms per doc (long tail truncated) */
#define NP_KB_EMBED_DIM     32     /* fixed dim for injected embeddings  */
#define NP_KB_QUERY_CAP     256

/* ---------------- document ---------------- */
typedef struct {
    char     id[NP_KB_ID_CAP];        /* stable doc id, e.g. "maclaw-readme" */
    char     source[NP_KB_SOURCE_CAP];/* URL or file path (provenance)       */
    char     source_type[24];         /* "web" / "file" / "note" / "manual"  */
    char     domain[NP_KB_DOMAIN_CAP];/* host for web docs, "" for local     */
    char     tags[NP_KB_TAGS_CAP];    /* comma-separated, case-insensitive   */
    int      quality;                 /* 0=low 1=medium 2=high               */
    char     title[NP_KB_TITLE_CAP];
    char     body[NP_KB_BODY_CAP];    /* read-back full text ("" if unread)  */
    bool     has_body;                /* true ONLY when body was read back   */
    uint64_t fetched_at;              /* unix seconds, provenance            */
    uint32_t body_hash;               /* CRC32 of body (empty if unread)     */
} np_kb_doc_t;

/* ---------------- store ---------------- */
typedef struct np_kb_store np_kb_store_t;

np_kb_store_t *np_kb_store_create(void);
void np_kb_store_destroy(np_kb_store_t *st);

/* Add/replace (by id). Re- indexes body if has_body. Copy semantics. */
bool np_kb_add(np_kb_store_t *st, const np_kb_doc_t *doc);
bool np_kb_remove(np_kb_store_t *st, const char *id);
size_t np_kb_count(const np_kb_store_t *st);
const np_kb_doc_t *np_kb_at(const np_kb_store_t *st, size_t i);
int np_kb_index_of(const np_kb_store_t *st, const char *id); /* -1 if absent */
const np_kb_doc_t *np_kb_get(const np_kb_store_t *st, const char *id);

/* ---------------- hybrid scoring ---------------- */
/* Injected embedding provider. Returns dim NP_KB_EMBED_DIM floats.
 * May be NULL: then ranking is BM25-only and stored vectors are ignored. */
typedef bool (*np_kb_embed_fn)(const char *text, float out[NP_KB_EMBED_DIM],
                               void *user);

typedef struct {
    float w_bm25;   /* weight of lexical score (default 0.6)  */
    float w_vec;    /* weight of cosine score  (default 0.4)  */
    np_kb_embed_fn embed;   /* NULL = BM25-only               */
    void *embed_user;
} np_kb_rank_t;

/* Fill defaults (0.6/0.4, no embed). */
void np_kb_rank_defaults(np_kb_rank_t *rk);

/* ---------------- filters (all optional; zeroed = no constraint) ------- */
typedef struct {
    char domains_allow[256];  /* comma list; "*.example.com" wildcards ok.
                               * Empty + web source => fail-closed block. */
    char domains_block[256];  /* wins over allow                          */
    char source_type[24];     /* exact match, "" = any                    */
    char tag[64];             /* substring on tag list, "" = any          */
    int  min_quality;         /* -1 = any                                 */
} np_kb_filter_t;

void np_kb_filter_defaults(np_kb_filter_t *f);

/* ---------------- search ---------------- */
typedef struct {
    char   id[NP_KB_ID_CAP];
    double score;       /* blended 0..~ (bm25 idf-sum + cosine mix) */
    double bm25;
    double cosine;      /* -1 if no embed / no vector                */
} np_kb_hit_t;

/* Search the index. Returns hit count written to out (<= out_cap).
 * `query` is tokenized the same way as document bodies. */
size_t np_kb_search(const np_kb_store_t *st, const char *query,
                    const np_kb_filter_t *f, const np_kb_rank_t *rk,
                    np_kb_hit_t *out, size_t out_cap);

/* ---------------- context pack ---------------- */
typedef struct {
    size_t considered;      /* docs passing filter                          */
    size_t blocked_no_body; /* passing filter but body never read back      */
    size_t included;        /* entries written into the pack                */
    size_t tokens_used;     /* estimated tokens (chars/4, min 1 per entry)  */
    size_t tokens_budget;
} np_kb_pack_stats_t;

/* Build a Context Pack: ranked, budgeted, cited, provenance-stamped,
 * PII-redacted. Only has_body docs enter; others increment
 * blocked_no_body. Writes rendered pack text into out (NUL-terminated).
 * Returns included count (0 on no-match or too-tight budget). */
size_t np_kb_pack_build(const np_kb_store_t *st, const char *query,
                        const np_kb_filter_t *f, const np_kb_rank_t *rk,
                        size_t tokens_budget,
                        char *out, size_t cap,
                        np_kb_pack_stats_t *stats);

/* ---------------- persistence (line format "KBI1") ---------------- */
/* Save all docs. Body newlines/backslashes escaped. Returns true on full
 * success (false if any write fails). */
bool np_kb_save(const np_kb_store_t *st, const char *path);
/* Load/replace store contents. Returns true if the file parsed cleanly. */
bool np_kb_load(np_kb_store_t *st, const char *path);

/* ---------------- PII redaction (same policy as X9) ---------------- */
/* Replace emails -> [EMAIL], phone-like numbers -> [PHONE],
 * 15-18 digit ids -> [ID], other long digit runs -> [NUM]. */
void np_kb_redact(const char *in, char *out, size_t cap);

/* ---------------- tokenizer exposure (test hook) ---------------- */
/* Lowercases and splits text into tokens (ASCII alnum runs, CJK chars as
 * single UTF-8 codepoint tokens). Returns token count, capped by out_cap. */
size_t np_kb_tokenize(const char *text, char out[][64], size_t out_cap);

#ifdef __cplusplus
}
#endif
#endif /* NP_KBIDX_H */
