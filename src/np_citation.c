/* np_citation.c - B5 citation protocol + resolver (see header). */
#include "np_citation.h"
#include "np_integrity.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* Parse "0x1234" or decimal into *v. */
static bool parse_num(const char *s, size_t *v) {
    if (!s || !*s) return false;
    char *end = NULL;
    unsigned long long n;
    if (s[0]=='0' && (s[1]=='x'||s[1]=='X'))
        n = strtoull(s + 2, &end, 16);
    else
        n = strtoull(s, &end, 10);
    if (end == s || *end != '\0') return false;
    *v = (size_t)n;
    return true;
}

bool np_citation_parse(const char *text, np_citation_t *out) {
    if (!text || !*text || !out) return false;
    memset(out, 0, sizeof(*out));

    /* STRUCT / EXTERNAL: contains '#' and no ':'-offset form */
    const char *hash = strchr(text, '#');
    if (hash) {
        size_t filelen = (size_t)(hash - text);
        if (filelen == 0 || filelen >= sizeof(out->file)) return false;
        memcpy(out->file, text, filelen);
        out->file[filelen] = '\0';
        const char *path = hash + 1;
        if (!*path) return false;
        snprintf(out->path, sizeof(out->path), "%s", path);
        /* external knowledge sources are identified by a domain-like prefix */
        if (strchr(out->file, '.') && strchr(out->file, '/')) {
            /* e.g. kb.example.com/doc#p2 -> file has '/', treat as external */
            out->kind = NP_CITE_EXTERNAL;
        } else {
            out->kind = NP_CITE_STRUCT;  /* e.g. report.docx#/w:tbl[0] */
        }
        return true;
    }

    /* file:line or file:0xOFFSET - split at last ':' (drive letters possible) */
    const char *colon = strrchr(text, ':');
    if (!colon || colon == text) return false;
    size_t filelen = (size_t)(colon - text);
    if (filelen >= sizeof(out->file)) return false;
    memcpy(out->file, text, filelen);
    out->file[filelen] = '\0';
    const char *range = colon + 1;
    if (!*range) return false;

    /* Determine hex offset vs line by presence of 0x prefix. */
    bool hex = (range[0]=='0' && (range[1]=='x'||range[1]=='X'));
    const char *dash = strchr(range, '-');
    if (dash) {
        char a[64], b[64];
        size_t alen = (size_t)(dash - range);
        if (alen == 0 || alen >= sizeof(a)) return false;
        memcpy(a, range, alen); a[alen] = '\0';
        snprintf(b, sizeof(b), "%s", dash + 1);
        if (!parse_num(a, &out->start) || !parse_num(b, &out->end)) return false;
    } else {
        if (!parse_num(range, &out->start)) return false;
        out->end = out->start;
    }
    out->kind = hex ? NP_CITE_OFFSET : NP_CITE_LINE;
    if (out->end < out->start) return false;
    return true;
}

void np_citation_format(const np_citation_t *c, char *buf, size_t cap) {
    if (!c || !buf || cap == 0) return;
    switch (c->kind) {
        case NP_CITE_LINE:
            if (c->start == c->end)
                snprintf(buf, cap, "%s:%zu", c->file, c->start);
            else
                snprintf(buf, cap, "%s:%zu-%zu", c->file, c->start, c->end);
            break;
        case NP_CITE_OFFSET:
            if (c->start == c->end)
                snprintf(buf, cap, "%s:0x%zX", c->file, c->start);
            else
                snprintf(buf, cap, "%s:0x%zX-0x%zX", c->file, c->start, c->end);
            break;
        case NP_CITE_STRUCT:
        case NP_CITE_EXTERNAL:
            snprintf(buf, cap, "%s#%s", c->file, c->path);
            break;
        default:
            buf[0] = '\0';
    }
}

bool np_citation_resolve(const np_citation_t *c, np_cite_reader reader,
                         void *user, unsigned char *out_buf, size_t cap,
                         size_t *out_len, uint32_t *out_hash) {
    if (!c || !reader) return false;
    /* For LINE citations the reader interprets start/end as line numbers;
     * for OFFSET/STRUCT it uses byte/struct positions. */
    long n = reader(c->file, c->start, c->end, out_buf ? out_buf : NULL,
                    out_buf ? cap : 0, user);
    if (n < 0) return false;            /* OOB / reader failure -> drop */
    if (out_len) *out_len = (size_t)n;
    if (out_hash) {
        if (out_buf) *out_hash = np_crc32(out_buf, (size_t)n);
        else *out_hash = 0;
    }
    return true;
}
