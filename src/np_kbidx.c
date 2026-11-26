/* np_kbidx.c - X10: hybrid knowledge index + Context Pack. See np_kbidx.h.
 *
 * Honest scope note: self-contained BM25 inverted index + optional injected
 * embeddings (no SQLite FTS5 — this repo's np_sqlite is read-only). Safety:
 * fail-closed domain filter, read-back gate for Context Pack, PII redaction
 * (reuses np_sp_redact from X9 so policy stays identical), bounded budgets.
 * Pure C99. */
#include "np_kbidx.h"

#include "np_searchprofile.h"   /* np_sp_redact: identical PII policy */
#include "np_integrity.h"       /* np_crc32 for body_hash */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------------- small helpers ---------------- */
static size_t scpy(char *d, size_t cap, const char *s) {
    size_t i = 0;
    if (cap == 0) return 0;
    if (s) for (; i + 1 < cap && s[i]; ++i) d[i] = s[i];
    d[i] = 0;
    return i;
}

static bool ci_eq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        if (ca == 0) return true;
    }
    return true;
}

static bool is_alnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* UTF-8 codepoint length from lead byte; 1 = invalid/continuation. */
static int utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t np_kb_tokenize(const char *text, char out[][64], size_t out_cap) {
    if (!text || !out || out_cap == 0) return 0;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        if (*p < 0x80) {
            if (is_alnum(*p)) {
                char tok[64];
                size_t t = 0;
                while (*p && *p < 0x80 && is_alnum(*p) && t < 63)
                    tok[t++] = (char)((*p >= 'A' && *p <= 'Z')
                                          ? (*p - 'A' + 'a') : *p), ++p;
                tok[t] = 0;
                if (t && n < out_cap) { scpy(out[n], 64, tok); ++n; }
            } else {
                ++p;
            }
        } else {
            int L = utf8_len(*p);
            char tok[64];
            size_t t = 0;
            while (L-- > 0 && *p && t < 62) tok[t++] = (char)*p++;
            tok[t] = 0;
            if (t && n < out_cap) { scpy(out[n], 64, tok); ++n; }
        }
    }
    return n;
}

/* ---------------- store internals ---------------- */
typedef struct {
    char   term[64];
    size_t tf;
} kb_post_t;

struct np_kb_store {
    size_t       count;
    np_kb_doc_t *docs;        /* [NP_KB_MAX_DOCS] */
    kb_post_t  **posts;       /* per-doc postings, malloc'd */
    size_t      *n_posts;
    size_t      *dl;          /* token count per doc (length for BM25) */
    size_t       total_tokens;
};

np_kb_store_t *np_kb_store_create(void) {
    np_kb_store_t *st = (np_kb_store_t *)calloc(1, sizeof *st);
    if (!st) return NULL;
    st->docs = (np_kb_doc_t *)calloc(NP_KB_MAX_DOCS, sizeof(np_kb_doc_t));
    st->posts = (kb_post_t **)calloc(NP_KB_MAX_DOCS, sizeof(kb_post_t *));
    st->n_posts = (size_t *)calloc(NP_KB_MAX_DOCS, sizeof(size_t));
    st->dl = (size_t *)calloc(NP_KB_MAX_DOCS, sizeof(size_t));
    if (!st->docs || !st->posts || !st->n_posts || !st->dl) {
        np_kb_store_destroy(st);
        return NULL;
    }
    return st;
}

void np_kb_store_destroy(np_kb_store_t *st) {
    if (!st) return;
    if (st->posts)
        for (size_t i = 0; i < NP_KB_MAX_DOCS; ++i) free(st->posts[i]);
    free(st->docs); free(st->posts); free(st->n_posts); free(st->dl);
    free(st);
}

/* (Re)build postings for doc at index i from its body.
 * Distinct terms beyond NP_KB_MAX_POSTINGS are truncated (long tail has
 * negligible BM25 impact; documented honestly in the X10 report). */
static void kb_reindex(np_kb_store_t *st, size_t i) {
    free(st->posts[i]);
    st->posts[i] = NULL;
    st->total_tokens -= st->dl[i];
    st->n_posts[i] = 0;
    st->dl[i] = 0;
    const np_kb_doc_t *d = &st->docs[i];
    if (!d->has_body || !d->body[0]) return;

    char (*toks)[64] = (char (*)[64])malloc(NP_KB_MAX_POSTINGS * 64);
    kb_post_t *ps = (kb_post_t *)calloc(NP_KB_MAX_POSTINGS, sizeof(kb_post_t));
    if (!toks || !ps) { free(toks); free(ps); return; }
    size_t nt = np_kb_tokenize(d->body, toks, NP_KB_MAX_POSTINGS);
    size_t np = 0;
    for (size_t t = 0; t < nt; ++t) {
        bool merged = false;
        for (size_t k = 0; k < np; ++k) {
            if (strcmp(ps[k].term, toks[t]) == 0) { ps[k].tf++; merged = true; break; }
        }
        if (!merged && np < NP_KB_MAX_POSTINGS) {
            scpy(ps[np].term, 64, toks[t]);
            ps[np].tf = 1;
            ++np;
        }
    }
    free(toks);
    kb_post_t *shrunk = (kb_post_t *)realloc(ps, (np ? np : 1) * sizeof(kb_post_t));
    if (shrunk) ps = shrunk;   /* realloc shrink failure: keep original block */
    st->posts[i] = ps;
    st->n_posts[i] = np;
    st->dl[i] = nt;
    st->total_tokens += nt;
}

int np_kb_index_of(const np_kb_store_t *st, const char *id) {
    if (!st || !id) return -1;
    for (size_t i = 0; i < st->count; ++i)
        if (strcmp(st->docs[i].id, id) == 0) return (int)i;
    return -1;
}

bool np_kb_add(np_kb_store_t *st, const np_kb_doc_t *doc) {
    if (!st || !doc || !doc->id[0]) return false;
    int cur = np_kb_index_of(st, doc->id);
    size_t slot;
    if (cur >= 0) {
        slot = (size_t)cur;
        st->total_tokens -= st->dl[slot];
    } else {
        if (st->count >= NP_KB_MAX_DOCS) return false;
        slot = st->count;
        ++st->count;
    }
    np_kb_doc_t copy = *doc;
    copy.body[NP_KB_BODY_CAP - 1] = 0;
    if (!copy.has_body) copy.body[0] = 0;
    copy.body_hash = copy.has_body ? np_crc32(copy.body, strlen(copy.body)) : 0;
    st->docs[slot] = copy;
    kb_reindex(st, slot);
    st->total_tokens += st->dl[slot];
    return true;
}

bool np_kb_remove(np_kb_store_t *st, const char *id) {
    if (!st || !id) return false;
    int cur = np_kb_index_of(st, id);
    if (cur < 0) return false;
    size_t i = (size_t)cur;
    st->total_tokens -= st->dl[i];
    free(st->posts[i]);
    for (size_t k = i; k + 1 < st->count; ++k) {
        st->docs[k]   = st->docs[k + 1];
        st->posts[k]  = st->posts[k + 1];
        st->n_posts[k] = st->n_posts[k + 1];
        st->dl[k]     = st->dl[k + 1];
    }
    --st->count;
    st->posts[st->count] = NULL;
    st->n_posts[st->count] = 0;
    st->dl[st->count] = 0;
    return true;
}

size_t np_kb_count(const np_kb_store_t *st) { return st ? st->count : 0; }
const np_kb_doc_t *np_kb_at(const np_kb_store_t *st, size_t i) {
    return (st && i < st->count) ? &st->docs[i] : NULL;
}
const np_kb_doc_t *np_kb_get(const np_kb_store_t *st, const char *id) {
    int i = np_kb_index_of(st, id);
    return i >= 0 ? &st->docs[(size_t)i] : NULL;
}

/* ---------------- filters ---------------- */
void np_kb_rank_defaults(np_kb_rank_t *rk) {
    if (!rk) return;
    rk->w_bm25 = 0.6f; rk->w_vec = 0.4f;
    rk->embed = NULL; rk->embed_user = NULL;
}

void np_kb_filter_defaults(np_kb_filter_t *f) {
    if (!f) return;
    memset(f, 0, sizeof *f);
    f->min_quality = -1;
}

/* comma/semicolon separated list membership, case-insensitive;
 * supports "*.suffix" wildcards. */
static bool list_has(const char *list, const char *item) {
    if (!list || !*list || !item || !*item) return false;
    size_t il = strlen(item);
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == ';' || *p == '\t') ++p;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != ',' && *e != ';' && *e != ' ' && *e != '\t') ++e;
        size_t tl = (size_t)(e - p);
        if (tl >= 2 && p[0] == '*' && p[1] == '.') {
            size_t sl = tl - 1;                    /* ".suffix" length */
            if (il >= sl && ci_eq(item + il - sl, p + 1, sl)) return true;
        } else if (tl == il && ci_eq(p, item, il)) {
            return true;
        }
        p = e;
    }
    return false;
}

static bool tag_match(const char *tags, const char *want) {
    if (!want || !*want) return true;
    return list_has(tags, want);
}

static bool domain_ok(const np_kb_doc_t *d, const np_kb_filter_t *f) {
    if (!d->domain[0]) return true;             /* local docs unrestricted */
    if (!f) return false;                        /* fail-closed default     */
    if (list_has(f->domains_block, d->domain)) return false;
    if (!f->domains_allow[0]) return false;      /* empty allow = block all */
    return list_has(f->domains_allow, d->domain);
}

static bool filter_pass(const np_kb_doc_t *d, const np_kb_filter_t *f) {
    if (f) {
        if (f->source_type[0] && !ci_eq(f->source_type, d->source_type,
                                        strlen(f->source_type))) return false;
        if (!tag_match(d->tags, f->tag)) return false;
        if (f->min_quality >= 0 && d->quality < f->min_quality) return false;
    }
    return domain_ok(d, f);
}

/* ---------------- BM25 + hybrid search ---------------- */
static double kb_bm25_score(const np_kb_store_t *st, size_t i,
                            const char (*qtoks)[64], size_t nq) {
    size_t N = 0;
    for (size_t k = 0; k < st->count; ++k) if (st->dl[k]) ++N;
    if (N == 0 || st->total_tokens == 0) return 0.0;
    double avgdl = (double)st->total_tokens / (double)N;
    const double k1 = 1.2, b = 0.75;
    double dl = (double)st->dl[i];
    double score = 0.0;
    for (size_t q = 0; q < nq; ++q) {
        /* tf of this query token in doc i */
        size_t tf = 0;
        for (size_t p = 0; p < st->n_posts[i]; ++p)
            if (strcmp(st->posts[i][p].term, qtoks[q]) == 0) {
                tf = st->posts[i][p].tf;
                break;
            }
        if (tf == 0) continue;
        /* df across all docs */
        size_t df = 0;
        for (size_t k = 0; k < st->count; ++k)
            for (size_t p = 0; p < st->n_posts[k]; ++p)
                if (strcmp(st->posts[k][p].term, qtoks[q]) == 0) { ++df; break; }
        double idf = log(1.0 + ((double)N - (double)df + 0.5) /
                               ((double)df + 0.5));
        score += idf * (double)tf * (k1 + 1.0) /
                 ((double)tf + k1 * (1.0 - b + b * dl / avgdl));
    }
    return score;
}

static double cosine(const float *a, const float *b, int dim) {
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < dim; ++i) {
        dot += (double)a[i] * (double)b[i];
        na  += (double)a[i] * (double)a[i];
        nb  += (double)b[i] * (double)b[i];
    }
    if (na <= 0 || nb <= 0) return 0.0;
    return dot / (sqrt(na) * sqrt(nb));
}

static int hit_cmp(const void *pa, const void *pb) {
    const np_kb_hit_t *a = (const np_kb_hit_t *)pa;
    const np_kb_hit_t *b = (const np_kb_hit_t *)pb;
    if (a->score < b->score) return 1;
    if (a->score > b->score) return -1;
    return strcmp(a->id, b->id);
}

size_t np_kb_search(const np_kb_store_t *st, const char *query,
                    const np_kb_filter_t *f, const np_kb_rank_t *rk,
                    np_kb_hit_t *out, size_t out_cap) {
    if (!st || !query || !out || out_cap == 0) return 0;
    static char qtoks[128][64];
    size_t nq = np_kb_tokenize(query, qtoks, 128);
    if (nq == 0) return 0;

    float qvec[NP_KB_EMBED_DIM];
    bool have_qvec = false;
    np_kb_rank_t def;
    if (!rk) { np_kb_rank_defaults(&def); rk = &def; }
    if (rk->embed && rk->embed(query, qvec, rk->embed_user)) have_qvec = true;

    size_t nh = 0;
    for (size_t i = 0; i < st->count; ++i) {
        const np_kb_doc_t *d = &st->docs[i];
        if (!d->has_body) continue;                 /* only readable content */
        if (!filter_pass(d, f)) continue;
        double bm = kb_bm25_score(st, i, (const char (*)[64])qtoks, nq);
        double cs = 0.0;
        if (have_qvec) {
            float dvec[NP_KB_EMBED_DIM];
            if (rk->embed(d->body, dvec, rk->embed_user))
                cs = cosine(qvec, dvec, NP_KB_EMBED_DIM);
        }
        /* zero-signal docs are not hits: no lexical overlap AND no vector
         * similarity means the query did not retrieve this document. */
        if (bm <= 0.0 && (!have_qvec || cs <= 0.0)) continue;
        double blend = have_qvec
            ? (double)rk->w_bm25 * (bm / (bm + 1.0)) +
              (double)rk->w_vec * (cs > 0 ? cs : 0.0)
            : bm;
        np_kb_hit_t *h = &out[nh];
        scpy(h->id, NP_KB_ID_CAP, d->id);
        h->bm25 = bm; h->cosine = have_qvec ? cs : -1.0;
        h->score = blend;
        ++nh;
        if (nh >= out_cap) break;
    }
    if (nh > 1) qsort(out, nh, sizeof(np_kb_hit_t), hit_cmp);
    return nh;
}

/* ---------------- context pack ---------------- */
static size_t est_tokens(const char *s) {
    size_t n = s ? strlen(s) : 0;
    return n / 4 ? n / 4 : 1;
}

static void hex8(uint32_t v, char *out /* 9 bytes */) {
    static const char hexd[] = "0123456789abcdef";
    for (int i = 7; i >= 0; --i) { out[i] = hexd[v & 0xF]; v >>= 4; }
    out[8] = 0;
}

size_t np_kb_pack_build(const np_kb_store_t *st, const char *query,
                        const np_kb_filter_t *f, const np_kb_rank_t *rk,
                        size_t tokens_budget,
                        char *out, size_t cap,
                        np_kb_pack_stats_t *stats) {
    if (stats) memset(stats, 0, sizeof *stats);
    if (!st || !query || !out || cap == 0) return 0;
    if (stats) stats->tokens_budget = tokens_budget;

    /* pass 1: considered / blocked_no_body */
    for (size_t i = 0; i < st->count; ++i) {
        if (!filter_pass(&st->docs[i], f)) continue;
        if (stats) ++stats->considered;
        if (!st->docs[i].has_body && stats) ++stats->blocked_no_body;
    }

    np_kb_hit_t hits[NP_KB_MAX_DOCS];
    size_t nh = np_kb_search(st, query, f, rk, hits, NP_KB_MAX_DOCS);

    char hdr[NP_KB_LINE_CAP];
    snprintf(hdr, sizeof hdr,
             "# Context Pack query=\"%s\" tokens<=%zu\n", query, tokens_budget);
    size_t used = est_tokens(hdr);
    size_t at = scpy(out, cap, hdr);
    size_t included = 0;

    static char redacted[NP_KB_BODY_CAP + 64];
    for (size_t h = 0; h < nh; ++h) {
        const np_kb_doc_t *d = np_kb_get(st, hits[h].id);
        if (!d) continue;
        np_sp_redact(d->body, redacted, sizeof redacted);
        char sha[9]; hex8(d->body_hash, sha);
        char cite[NP_KB_LINE_CAP];
        snprintf(cite, sizeof cite,
                 "[%zu] id=%s source=%s type=%s domain=%s quality=%d "
                 "fetched=%llu sha=%s\n",
                 included + 1, d->id, d->source, d->source_type,
                 d->domain[0] ? d->domain : "-", d->quality,
                 (unsigned long long)d->fetched_at, sha);
        size_t need = est_tokens(cite) + est_tokens(redacted) + 1;
        if (used + need <= tokens_budget) {
            if (at + strlen(cite) + strlen(redacted) + 2 < cap) {
                at += scpy(out + at, cap - at, cite);
                at += scpy(out + at, cap - at, redacted);
                at += scpy(out + at, cap - at, "\n");
            }
            used += need;
            ++included;
        } else {
            /* try to fit a truncated tail of this body */
            size_t rem = tokens_budget > used ? tokens_budget - used : 0;
            size_t cite_t = est_tokens(cite);
            if (rem > cite_t + 8) {
                size_t body_t = rem - cite_t - 1;
                size_t chars = body_t * 4;
                if (chars < strlen(redacted) && chars >= 32 &&
                    at + strlen(cite) + chars + 2 < cap) {
                    char keep = redacted[chars];
                    redacted[chars] = 0;
                    at += scpy(out + at, cap - at, cite);
                    at += scpy(out + at, cap - at, redacted);
                    at += scpy(out + at, cap - at, "\n...\n");
                    redacted[chars] = keep;
                    used = tokens_budget;
                    ++included;
                }
            }
            break;   /* budget exhausted; lower-ranked hits skipped */
        }
    }
    if (stats) { stats->included = included; stats->tokens_used = used; }
    return included;
}

/* ---------------- PII redaction (thin wrapper over X9 policy) --------- */
void np_kb_redact(const char *in, char *out, size_t cap) {
    np_sp_redact(in, out, cap);
}

/* ---------------- persistence ---------------- */
static void esc_line(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p && o + 2 < cap; ++p) {
        if (*p == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
        else if (*p == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else out[o++] = *p;
    }
    out[o] = 0;
}

static void unesc_line(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < cap; ++p) {
        if (*p == '\\' && p[1]) {
            ++p;
            if (*p == 'n') out[o++] = '\n';
            else if (*p == '\\') out[o++] = '\\';
            else out[o++] = *p;
        } else out[o++] = *p;
    }
    out[o] = 0;
}

/* dynamic line reader (escaped body lines can exceed 4KB) */
static bool read_line(FILE *fp, char **buf, size_t *cap) {
    size_t n = 0;
    int c;
    bool any = false;
    for (;;) {
        c = fgetc(fp);
        if (c == EOF) break;
        any = true;
        if (c == '\n') break;
        if (n + 1 >= *cap) {
            size_t nc = *cap * 2;
            char *nb = (char *)realloc(*buf, nc);
            if (!nb) return false;
            *buf = nb; *cap = nc;
        }
        (*buf)[n++] = (char)c;
    }
    if (!any) return false;
    (*buf)[n] = 0;
    return true;
}

bool np_kb_save(const np_kb_store_t *st, const char *path) {
    if (!st || !path || !path[0]) return false;
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    bool ok = fputs("KBI1\n", fp) >= 0;
    for (size_t i = 0; ok && i < st->count; ++i) {
        const np_kb_doc_t *d = &st->docs[i];
        char esc[NP_KB_BODY_CAP * 2 + 8];
        if (fprintf(fp, "KBDOC %s|%s|%s|%s|%d|%llu|%d\n",
                    d->id, d->source, d->source_type, d->domain, d->quality,
                    (unsigned long long)d->fetched_at,
                    d->has_body ? 1 : 0) < 0) ok = false;
        if (ok) {
            esc_line(d->title, esc, sizeof esc);
            ok = fprintf(fp, "KBT %s\n", esc) >= 0;
        }
        if (ok) {
            esc_line(d->tags, esc, sizeof esc);
            ok = fprintf(fp, "KBG %s\n", esc) >= 0;
        }
        if (ok) {
            esc_line(d->has_body ? d->body : "", esc, sizeof esc);
            ok = fprintf(fp, "KBB %s\n", esc) >= 0;
        }
        if (ok) ok = fputs("KBEND\n", fp) >= 0;
    }
    if (fclose(fp) != 0) ok = false;
    return ok;
}

bool np_kb_load(np_kb_store_t *st, const char *path) {
    if (!st || !path || !path[0]) return false;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    /* clear current contents */
    while (st->count) np_kb_remove(st, st->docs[0].id);

    size_t lcap = 4096;
    char *line = (char *)malloc(lcap);
    bool ok = line != NULL, saw_magic = false;
    np_kb_doc_t cur;
    bool in_doc = false;
    while (ok && read_line(fp, &line, &lcap)) {
        if (strcmp(line, "KBI1") == 0) { saw_magic = true; continue; }
        if (strncmp(line, "KBDOC ", 6) == 0) {
            memset(&cur, 0, sizeof cur);
            cur.quality = -1;
            char *fields[7];
            int nf = 0;
            char *p = line + 6;
            fields[nf++] = p;
            while (*p && nf < 7) {
                if (*p == '|') { *p = 0; fields[nf++] = p + 1; }
                ++p;
            }
            if (nf == 7) {
                scpy(cur.id, NP_KB_ID_CAP, fields[0]);
                scpy(cur.source, NP_KB_SOURCE_CAP, fields[1]);
                scpy(cur.source_type, 24, fields[2]);
                scpy(cur.domain, NP_KB_DOMAIN_CAP, fields[3]);
                cur.quality = atoi(fields[4]);
                cur.fetched_at = (uint64_t)strtoull(fields[5], NULL, 10);
                cur.has_body = fields[6][0] == '1';
                in_doc = true;
            } else ok = false;
            continue;
        }
        if (!in_doc) continue;
        if (strncmp(line, "KBT ", 4) == 0) {
            unesc_line(line + 4, cur.title, NP_KB_TITLE_CAP);
        } else if (strncmp(line, "KBG ", 4) == 0) {
            unesc_line(line + 4, cur.tags, NP_KB_TAGS_CAP);
        } else if (strncmp(line, "KBB ", 4) == 0) {
            unesc_line(line + 4, cur.body, NP_KB_BODY_CAP);
        } else if (strcmp(line, "KBEND") == 0) {
            np_kb_add(st, &cur);
            in_doc = false;
        }
    }
    free(line);
    fclose(fp);
    return ok && saw_magic;
}
