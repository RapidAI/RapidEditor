/* test_x10_kbidx.c - X10 hybrid knowledge index + Context Pack tests. */
#include "np_kbidx.h"
#include "np_integrity.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void cpy(char *d, size_t c, const char *s) {
    size_t i = 0;
    for (; i + 1 < c && s[i]; ++i) d[i] = s[i];
    d[i] = 0;
}

static np_kb_doc_t mkdoc(const char *id, const char *body, bool has_body,
                         const char *domain, const char *type, int quality) {
    np_kb_doc_t d;
    memset(&d, 0, sizeof d);
    cpy(d.id, sizeof d.id, id);
    cpy(d.source, sizeof d.source,
        domain[0] ? "https://" : "");
    if (domain[0]) {
        char u[260];
        snprintf(u, sizeof u, "https://%s/doc/%s", domain, id);
        cpy(d.source, sizeof d.source, u);
    } else {
        snprintf(d.source, sizeof d.source, "local://notes/%s", id);
    }
    cpy(d.source_type, sizeof d.source_type, type);
    cpy(d.domain, sizeof d.domain, domain);
    d.quality = quality;
    d.has_body = has_body;
    if (has_body) cpy(d.body, sizeof d.body, body);
    d.fetched_at = 1700000000;
    return d;
}

/* ---- fake embedding: bag-of-signature so we control similarity ---- */
static bool fake_embed(const char *text, float out[NP_KB_EMBED_DIM], void *u) {
    (void)u;
    memset(out, 0, NP_KB_EMBED_DIM * sizeof(float));
    if (!text) return false;
    /* dim0: alpha-ness, dim1: beta-ness, dim2: length signal */
    size_t n = strlen(text);
    const char *p = text;
    for (; *p; ++p) {
        if (*p == 'a') out[0] += 1.0f;
        if (*p == 'b') out[1] += 1.0f;
    }
    if (n > 0) out[2] = (float)(n < 100 ? n : 100);
    return true;
}

static void test_tokenize(void) {
    char toks[32][64];
    size_t n = np_kb_tokenize("Hello World  foo_bar", toks, 32);
    CHECK(n == 3);
    CHECK(strcmp(toks[0], "hello") == 0);
    CHECK(strcmp(toks[1], "world") == 0);
    CHECK(strcmp(toks[2], "foo_bar") == 0);
    /* CJK unigram */
    n = np_kb_tokenize("检索知识库", toks, 32);
    CHECK(n == 5);   /* 5 codepoints */
    /* punctuation split */
    n = np_kb_tokenize("a.b,c-d", toks, 32);
    CHECK(n == 4);
    /* truncation safety: cap respected */
    char few[2][64];
    n = np_kb_tokenize("one two three", few, 2);
    CHECK(n == 2);
}

static void test_bm25_ranking(void) {
    np_kb_store_t *st = np_kb_store_create();
    CHECK(st != NULL);

    np_kb_doc_t a = mkdoc("alpha-doc", "alpha alpha beta retrieval engine", true, "", "file", 2);
    np_kb_doc_t b = mkdoc("beta-doc", "alpha vector search knowledge", true, "", "file", 1);
    np_kb_doc_t c = mkdoc("unrelated", "cooking recipes pasta", true, "", "file", 2);
    CHECK(np_kb_add(st, &a));
    CHECK(np_kb_add(st, &b));
    CHECK(np_kb_add(st, &c));

    np_kb_filter_t f; np_kb_filter_defaults(&f);
    np_kb_hit_t hits[8];
    size_t nh = np_kb_search(st, "alpha retrieval", &f, NULL, hits, 8);
    /* zero-lexical-overlap docs (unrelated) are not hits by design */
    CHECK(nh == 2);
    /* alpha-doc matches both terms -> rank 1 */
    CHECK(strcmp(hits[0].id, "alpha-doc") == 0);
    CHECK(hits[0].bm25 > hits[1].bm25);

    /* empty query -> no hits */
    CHECK(np_kb_search(st, "   ", &f, NULL, hits, 8) == 0);
    /* no-store guards */
    CHECK(np_kb_search(NULL, "x", &f, NULL, hits, 8) == 0);

    np_kb_store_destroy(st);
}

static void test_hybrid_vector(void) {
    np_kb_store_t *st = np_kb_store_create();
    np_kb_doc_t a2 = mkdoc("rich-a", "shared aaaa aaaa aaaa", true, "", "file", 2);
    np_kb_doc_t b2 = mkdoc("rich-b", "shared bbbb", true, "", "file", 2);
    np_kb_add(st, &a2);
    np_kb_add(st, &b2);

    np_kb_rank_t rk; np_kb_rank_defaults(&rk);
    rk.embed = fake_embed;
    np_kb_filter_t f; np_kb_filter_defaults(&f);
    /* query "aa" has zero lexical overlap with any doc token -> BM25 = 0
     * for both; the vector signal must be the sole discriminator. */
    np_kb_hit_t hits[8];
    size_t nh = np_kb_search(st, "aa", &f, &rk, hits, 8);
    CHECK(nh == 2);
    CHECK(hits[0].bm25 == 0.0);
    /* rich-a is dense in 'a' -> cosine strictly higher than rich-b */
    CHECK(strcmp(hits[0].id, "rich-a") == 0);
    CHECK(hits[0].cosine > hits[1].cosine);
    CHECK(hits[1].cosine >= 0.0);
    np_kb_store_destroy(st);
}

static void test_filters(void) {
    np_kb_store_t *st = np_kb_store_create();
    np_kb_doc_t web1 = mkdoc("web-ok", "knowledge about indexing", true, "docs.example.org", "web", 2);
    np_kb_doc_t web2 = mkdoc("web-bad", "knowledge spam", true, "evil.example.com", "web", 2);
    np_kb_doc_t web3 = mkdoc("web-unknown", "knowledge unknown host", true, "random.org", "web", 2);
    np_kb_doc_t note = mkdoc("note-local", "knowledge note body", true, "", "note", 1);
    np_kb_add(st, &web1); np_kb_add(st, &web2); np_kb_add(st, &web3); np_kb_add(st, &note);

    np_kb_filter_t f; np_kb_filter_defaults(&f);
    /* fail-closed: empty allow blocks ALL web docs, local note passes */
    np_kb_hit_t hits[8];
    size_t nh = np_kb_search(st, "knowledge", &f, NULL, hits, 8);
    CHECK(nh == 1);
    CHECK(strcmp(hits[0].id, "note-local") == 0);

    /* allow-list + wildcard */
    cpy(f.domains_allow, sizeof f.domains_allow, "*.example.org,docs.example.org");
    nh = np_kb_search(st, "knowledge", &f, NULL, hits, 8);
    CHECK(nh == 2);   /* web-ok + note */
    /* block-list wins over allow */
    cpy(f.domains_block, sizeof f.domains_block, "docs.example.org");
    nh = np_kb_search(st, "knowledge", &f, NULL, hits, 8);
    CHECK(nh == 1);
    CHECK(strcmp(hits[0].id, "note-local") == 0);

    /* source_type / tag / quality */
    np_kb_filter_t g; np_kb_filter_defaults(&g);
    cpy(g.source_type, sizeof g.source_type, "note");
    nh = np_kb_search(st, "knowledge", &g, NULL, hits, 8);
    CHECK(nh == 1 && strcmp(hits[0].id, "note-local") == 0);

    np_kb_doc_t tagged = mkdoc("tagged", "knowledge tagged body", true, "", "file", 1);
    cpy(tagged.tags, sizeof tagged.tags, "rag,core,Indexing");
    np_kb_add(st, &tagged);
    np_kb_filter_t h; np_kb_filter_defaults(&h);
    cpy(h.tag, sizeof h.tag, "CORE");   /* case-insensitive tag match */
    nh = np_kb_search(st, "knowledge", &h, NULL, hits, 8);
    CHECK(nh == 1 && strcmp(hits[0].id, "tagged") == 0);

    np_kb_filter_t q; np_kb_filter_defaults(&q);
    q.min_quality = 2;
    nh = np_kb_search(st, "knowledge", &q, NULL, hits, 8);
    for (size_t i = 0; i < nh; ++i) {
        const np_kb_doc_t *d = np_kb_get(st, hits[i].id);
        CHECK(d && d->quality >= 2);
    }
    np_kb_store_destroy(st);
}

static void test_readback_gate_and_pack(void) {
    np_kb_store_t *st = np_kb_store_create();
    np_kb_doc_t full = mkdoc("read-doc", "context pack alpha body text here", true, "", "file", 2);
    np_kb_doc_t summ = mkdoc("unread-doc", "summary-only content alpha", false, "", "file", 2);
    np_kb_add(st, &full);
    np_kb_add(st, &summ);

    np_kb_filter_t f; np_kb_filter_defaults(&f);
    /* search only surfaces read-back docs */
    np_kb_hit_t hits[8];
    CHECK(np_kb_search(st, "alpha", &f, NULL, hits, 8) == 1);
    CHECK(strcmp(hits[0].id, "read-doc") == 0);

    char pack[NP_KB_PACK_CAP];
    np_kb_pack_stats_t stt;
    size_t inc = np_kb_pack_build(st, "alpha", &f, NULL, 2000, pack, sizeof pack, &stt);
    CHECK(inc == 1);
    CHECK(stt.considered == 2);
    CHECK(stt.blocked_no_body == 1);
    CHECK(stt.included == 1);
    CHECK(strstr(pack, "[1] id=read-doc") != NULL);
    CHECK(strstr(pack, "type=file") != NULL);
    CHECK(strstr(pack, "fetched=1700000000") != NULL);
    CHECK(strstr(pack, "unread-doc") == NULL);          /* never inlined */
    /* provenance sha matches np_crc32 of body (exact length) */
    char sha[9];
    { const char *bd = "context pack alpha body text here";
      uint32_t h = np_crc32(bd, strlen(bd));
      static const char hexd[] = "0123456789abcdef";
      for (int i = 7; i >= 0; --i) { sha[i] = hexd[h & 0xF]; h >>= 4; }
      sha[8] = 0; }
    CHECK(strstr(pack, sha) != NULL);

    /* budget tighter than the header alone -> no entries fit */
    inc = np_kb_pack_build(st, "alpha", &f, NULL, 3, pack, sizeof pack, &stt);
    CHECK(stt.included == 0);
    CHECK(stt.tokens_used >= 1);   /* header still rendered/counted */

    /* no match */
    inc = np_kb_pack_build(st, "zzz-notfound", &f, NULL, 2000, pack, sizeof pack, &stt);
    CHECK(inc == 0 && stt.included == 0);
    np_kb_store_destroy(st);
}

static void test_budget_truncation(void) {
    np_kb_store_t *st = np_kb_store_create();
    /* realistic repeated tokens (not one giant run, which the tokenizer
     * correctly treats as a single long word) */
    char big[4096];
    size_t bi = 0;
    for (int i = 0; i < 500 && bi + 4 < sizeof big; ++i) {
        big[bi++] = 'x'; big[bi++] = 'x'; big[bi++] = 'x'; big[bi++] = ' ';
    }
    big[bi] = 0;
    np_kb_doc_t d1 = mkdoc("big1", big, true, "", "file", 2);
    np_kb_doc_t d2 = mkdoc("small2", "tail doc", true, "", "file", 2);
    np_kb_add(st, &d1);
    np_kb_add(st, &d2);
    np_kb_filter_t f; np_kb_filter_defaults(&f);
    np_kb_pack_stats_t stt;
    char pack[NP_KB_PACK_CAP];
    size_t inc = np_kb_pack_build(st, "xxx tail", &f, NULL, 300, pack, sizeof pack, &stt);
    CHECK(inc >= 1);
    CHECK(stt.tokens_used <= 300);
    /* big1 outranks (tf=500) and only fits truncated */
    CHECK(strstr(pack, "big1") != NULL);
    CHECK(strstr(pack, "...") != NULL);   /* truncation marker present */
    np_kb_store_destroy(st);
}

static void test_redaction(void) {
    char out[256];
    np_kb_redact("contact bob@example.com or 13800138000 ok", out, sizeof out);
    CHECK(strstr(out, "[EMAIL]") != NULL);
    CHECK(strstr(out, "bob@") == NULL);
    CHECK(strstr(out, "13800138000") == NULL);
    /* long digit run -> [ID]/[NUM]; short numbers kept */
    np_kb_redact("id 110101199003077512 and 5 digits 12345 keep 42", out, sizeof out);
    CHECK(strstr(out, "110101199003077512") == NULL);
    CHECK(strstr(out, "42") != NULL);
}

static void test_persistence(void) {
    static const char *PATH = "x10_kb_roundtrip.kb";
    np_kb_store_t *st = np_kb_store_create();
    np_kb_doc_t a = mkdoc("doc-a", "line1\nline2\\path alpha", true, "docs.example.org", "web", 2);
    cpy(a.tags, sizeof a.tags, "rag,core");
    cpy(a.title, sizeof a.title, "Doc A title");
    np_kb_doc_t b = mkdoc("doc-b", "", false, "", "note", 0);
    np_kb_add(st, &a);
    np_kb_add(st, &b);
    CHECK(np_kb_save(st, PATH));

    np_kb_store_t *st2 = np_kb_store_create();
    CHECK(np_kb_load(st2, PATH));
    CHECK(np_kb_count(st2) == 2);
    const np_kb_doc_t *ra = np_kb_get(st2, "doc-a");
    CHECK(ra && ra->has_body);
    CHECK(strcmp(ra->body, "line1\nline2\\path alpha") == 0);
    CHECK(strcmp(ra->title, "Doc A title") == 0);
    CHECK(strcmp(ra->domain, "docs.example.org") == 0);
    CHECK(ra->fetched_at == 1700000000);
    CHECK(ra->body_hash != 0);
    const np_kb_doc_t *rb = np_kb_get(st2, "doc-b");
    CHECK(rb && !rb->has_body && rb->body[0] == 0);
    /* loaded store still searchable (postings rebuilt on add during load);
     * doc-a is a web source -> domain allow must be set (fail-closed) */
    np_kb_filter_t f; np_kb_filter_defaults(&f);
    cpy(f.domains_allow, sizeof f.domains_allow, "docs.example.org");
    np_kb_hit_t hits[4];
    CHECK(np_kb_search(st2, "alpha", &f, NULL, hits, 4) == 1);

    /* corrupt magic rejected */
    FILE *fp = fopen(PATH, "wb");
    fputs("NOTKB\n", fp);
    fclose(fp);
    np_kb_store_t *st3 = np_kb_store_create();
    CHECK(!np_kb_load(st3, PATH));
    /* missing file */
    CHECK(!np_kb_load(st2, "x10_no_such_file.kb"));

    remove(PATH);
    np_kb_store_destroy(st);
    np_kb_store_destroy(st2);
    np_kb_store_destroy(st3);
}

static void test_limits_and_upsert(void) {
    np_kb_store_t *st = np_kb_store_create();
    /* upsert replaces by id, postings rebuilt */
    np_kb_doc_t a = mkdoc("dup", "old content alpha", true, "", "file", 1);
    np_kb_add(st, &a);
    np_kb_doc_t a2 = mkdoc("dup", "new content beta", true, "", "file", 2);
    CHECK(np_kb_add(st, &a2));
    CHECK(np_kb_count(st) == 1);
    np_kb_filter_t f; np_kb_filter_defaults(&f);
    np_kb_hit_t hits[4];
    CHECK(np_kb_search(st, "alpha", &f, NULL, hits, 4) == 0);
    CHECK(np_kb_search(st, "beta", &f, NULL, hits, 4) == 1);

    /* remove */
    CHECK(np_kb_remove(st, "dup"));
    CHECK(!np_kb_remove(st, "dup"));
    CHECK(np_kb_count(st) == 0);

    /* empty id rejected */
    np_kb_doc_t bad = mkdoc("", "x", true, "", "file", 1);
    CHECK(!np_kb_add(st, &bad));
    /* NULL guards */
    CHECK(!np_kb_add(NULL, &a));
    CHECK(np_kb_at(NULL, 0) == NULL);

    /* fill to capacity boundary: add 3 docs, all present */
    for (int i = 0; i < 3; ++i) {
        char id[16]; snprintf(id, sizeof id, "cap%d", i);
        np_kb_doc_t d = mkdoc(id, "capacity check body", true, "", "file", 1);
        CHECK(np_kb_add(st, &d));
    }
    CHECK(np_kb_count(st) == 3);
    CHECK(np_kb_index_of(st, "cap2") == 2);
    np_kb_store_destroy(st);
}

int main(void) {
    test_tokenize();
    test_bm25_ranking();
    test_hybrid_vector();
    test_filters();
    test_readback_gate_and_pack();
    test_budget_truncation();
    test_redaction();
    test_persistence();
    test_limits_and_upsert();
    printf("x10_kbidx: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
