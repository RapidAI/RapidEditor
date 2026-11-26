/* Unit tests for A2 (np_context) + B5 (np_citation). */
#include "np_context.h"
#include "np_citation.h"
#include "np_integrity.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

/* A local read-back reader over a fixed in-memory file. */
typedef struct { const unsigned char *data; size_t len; } memfile_t;
static long reader(const char *file, size_t start, size_t end,
                   unsigned char *buf, size_t cap, void *user) {
    (void)file;
    memfile_t *m = (memfile_t *)user;
    if (start > m->len || end > m->len || end < start) return -1; /* OOB -> drop */
    size_t n = end - start;
    if (buf && n > cap) return -1;
    if (buf && n) memcpy(buf, m->data + start, n);
    return (long)n;
}

static int test_citations(void) {
    np_citation_t c;
    CHECK(np_citation_parse("src/main.c:42", &c));
    CHECK(c.kind == NP_CITE_LINE && c.start == 42 && c.end == 42);
    CHECK(strcmp(c.file, "src/main.c") == 0);
    CHECK(np_citation_parse("src/main.c:10-20", &c));
    CHECK(c.kind == NP_CITE_LINE && c.start == 10 && c.end == 20);
    CHECK(np_citation_parse("firmware.elf:0x401000", &c));
    CHECK(c.kind == NP_CITE_OFFSET && c.start == 0x401000);
    CHECK(np_citation_parse("firmware.elf:0x10-0x20", &c));
    CHECK(c.kind == NP_CITE_OFFSET && c.start == 0x10 && c.end == 0x20);
    CHECK(np_citation_parse("report.docx#/w:tbl[0]", &c));
    CHECK(c.kind == NP_CITE_STRUCT && strcmp(c.path, "/w:tbl[0]") == 0);
    CHECK(np_citation_parse("kb.example.com/doc#p2", &c));
    CHECK(c.kind == NP_CITE_EXTERNAL);
    CHECK(!np_citation_parse("no-citation-here", &c));
    CHECK(!np_citation_parse("file.c:", &c));

    char buf[300];
    np_citation_parse("firmware.elf:0x401000", &c);
    np_citation_format(&c, buf, sizeof(buf));
    CHECK(strcmp(buf, "firmware.elf:0x401000") == 0);

    /* resolver: read-back + hash, OOB dropped */
    unsigned char data[32]; for (size_t i=0;i<32;i++) data[i]=(unsigned char)i;
    memfile_t m = {data, sizeof(data)};
    np_citation_parse("f.bin:0x4-0x8", &c);
    unsigned char out[64]; size_t n; uint32_t h;
    CHECK(np_citation_resolve(&c, reader, &m, out, sizeof(out), &n, &h));
    CHECK(n == 4 && out[0]==4 && out[3]==7);
    CHECK(h == np_crc32(data+4, 4));
    /* out of range -> drop */
    np_citation_parse("f.bin:0x100-0x200", &c);
    CHECK(!np_citation_resolve(&c, reader, &m, out, sizeof(out), &n, &h));
    return 0;
}

static int test_context(void) {
    np_ctx_budget_t b = np_ctx_default_budget();
    CHECK(b.align_bytes == 16);
    CHECK(np_ctx_binary_limit(100, 33, 16) == 32);  /* align down to 16 */
    CHECK(np_ctx_binary_limit(10, 100, 16) == 10);  /* under budget */
    CHECK(np_ctx_text_limit(100, 50) == 50);

    np_ignore_t *ig = np_ignore_create("# comment\n*.bin\nsecret.txt\nsecrets/*\n");
    CHECK(np_ignore_matches(ig, "firmware.bin"));
    CHECK(np_ignore_matches(ig, "notes/secret.txt"));
    CHECK(np_ignore_matches(ig, "secrets/key.txt"));
    CHECK(!np_ignore_matches(ig, "src/main.c"));

    np_ctx_fragment_t frags[3];
    char t1[] = "hello world text";
    char t2[] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"; /* 32 bytes */
    char t3[] = "ignored content";
    frags[0].track = NP_TRACK_TEXT; strcpy(frags[0].source, "src/main.c");
    frags[0].text = t1; frags[0].len = strlen(t1);
    frags[1].track = NP_TRACK_BINARY; strcpy(frags[1].source, "blob.bin2");
    frags[1].text = t2; frags[1].len = strlen(t2);
    frags[2].track = NP_TRACK_TEXT; strcpy(frags[2].source, "secret.txt");
    frags[2].text = t3; frags[2].len = strlen(t3);

    /* small binary budget to force truncation */
    b.max_bytes = 16;
    char *out = NULL; size_t olen = 0; np_ctx_audit_t audit;
    CHECK(np_ctx_build(frags, 3, &b, ig, &out, &olen, &audit));
    CHECK(out && olen > 0);
    CHECK(audit.fragments_included == 2);     /* t1 + t2 */
    CHECK(audit.fragments_ignored == 1);      /* secret.txt */
    CHECK(audit.fragments_truncated == 1);    /* blob truncated to 16 */
    CHECK(strstr(audit.audit, "TRUNCATED") != NULL);
    CHECK(strstr(audit.audit, ".aiignore") != NULL);
    CHECK(strstr(out, "hello world text") != NULL);
    np_ctx_free_text(out);
    np_ignore_destroy(ig);
    return 0;
}

int main(void) {
    if (test_citations()) return 1;
    if (test_context()) return 1;
    printf("test_s0_context: ALL PASSED\n");
    return 0;
}
