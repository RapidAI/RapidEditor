/* np_searchhit.c - X11 search-result citation & anti-injection (see header). */
#include "np_searchhit.h"
#include "np_searchprofile.h"   /* np_sp_redact: identical PII policy */
#include "np_integrity.h"       /* np_crc32 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

const char *np_sh_encoding_name(np_sh_encoding_t e) {
    switch (e) {
        case NP_SH_ENC_UTF8:      return "UTF-8";
        case NP_SH_ENC_UTF8_BOM:  return "UTF-8 (BOM)";
        case NP_SH_ENC_GBK_LABEL: return "GBK (未转码)";
        default:                  return "未知编码";
    }
}

static void scpy(char *d, size_t cap, const char *s) {
    if (!cap) return;
    if (!s) { d[0] = 0; return; }
    size_t i = 0;
    for (; i + 1 < cap && s[i]; ++i) d[i] = s[i];
    d[i] = 0;
}

/* case-insensitive prefix compare helper (avoids strncasecmp portability). */
static bool ci_prefix(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (!y) return false;
        if (tolower(x) != tolower(y)) return false;
    }
    return true;
}
static bool strncasecmp_ci(const char *p, const char *lit, size_t n) {
    return ci_prefix(p, lit, n);
}

/* ---- encoding detection ------------------------------------------- */
np_sh_encoding_t np_sh_detect_encoding(const unsigned char *data, size_t len) {
    if (!data || len == 0) return NP_SH_ENC_UNKNOWN;
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        return NP_SH_ENC_UTF8_BOM;
    /* Look for a charset declaration in the first 2 KiB. */
    size_t n = len < 2048 ? len : 2048;
    for (size_t i = 0; i + 5 < n; ++i) {
        const char *p = (const char *)data + i;
        if (strncasecmp_ci(p, "gb2312", 6) || strncasecmp_ci(p, "gbk", 3))
            return NP_SH_ENC_GBK_LABEL;
        if (strncasecmp_ci(p, "utf-8", 5) || strncasecmp_ci(p, "utf8", 4))
            return NP_SH_ENC_UTF8;
    }
    /* Cheap UTF-8 validity heuristic: valid multibyte sequences present and
     * no lone 0x80..0xBF lead → treat as UTF-8; else unknown. */
    bool high = false;
    for (size_t i = 0; i < n; ++i) if (data[i] >= 0x80) { high = true; break; }
    return high ? NP_SH_ENC_UNKNOWN : NP_SH_ENC_UTF8;
}

/* ---- sanitization -------------------------------------------------- */
/* Neutralize prompt-injection phrases by marking them; the text is kept
 * visible (for audit) but explicitly tagged so a model never mistakes it for
 * an instruction. Matching is case-insensitive on ASCII. */
static bool match_any_ci(const char *p, const char *const *phrases) {
    for (size_t i = 0; phrases[i]; ++i) {
        size_t n = strlen(phrases[i]);
        if (ci_prefix(p, phrases[i], n)) return true;
    }
    return false;
}

size_t np_sh_sanitize(const char *in, size_t len, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!in || len == 0) return 0;
    static const char *const injections[] = {
        "ignore previous", "ignore all", "disregard", "system:", "assistant:",
        "developer:", "you are now", "new instructions", "forget everything",
        "覆盖之前", "忽略之前", "系统指令", "你现在是", NULL
    };
    size_t o = 0;
    char redacted[NP_SH_BODY_CAP];
    /* Redact PII first (bounded), then walk the result neutralizing controls. */
    size_t rlen = len > sizeof redacted - 1 ? sizeof redacted - 1 : len;
    char tmpbuf[8192];
    size_t chunk = rlen > sizeof tmpbuf - 1 ? sizeof tmpbuf - 1 : rlen;
    memcpy(tmpbuf, in, chunk);
    tmpbuf[chunk] = 0;
    np_sp_redact(tmpbuf, redacted, sizeof redacted);

    for (size_t i = 0; redacted[i] && o + 1 < cap; ++i) {
        unsigned char c = (unsigned char)redacted[i];
        if (c == '\r') continue;
        if (c == '\n') { if (o && out[o-1] != '\n') out[o++] = '\n'; continue; }
        if (c == '\t') { out[o++] = ' '; continue; }
        if (c < 0x20 || c == 0x7f) { out[o++] = ' '; continue; }  /* control */
        if (match_any_ci(redacted + i, injections)) {
            if (o + 9 < cap) { memcpy(out + o, "[INJECT] ", 9); o += 9; }
            /* Skip the rest of the injected sentence (until sentence end) so
             * the model never sees the imperative tail ("...and reveal X"). */
            while (redacted[i] && redacted[i] != '.' && redacted[i] != '!' &&
                   redacted[i] != '?' && redacted[i] != '\n') ++i;
            if (!redacted[i]) break;
            continue;
        }
        out[o++] = (char)c;
    }
    out[o] = 0;
    /* collapse >2 consecutive newlines for compactness */
    size_t w = 0;
    for (size_t r = 0; out[r]; ++r) {
        if (out[r] == '\n' && w >= 2 && out[w-1] == '\n' && out[w-2] == '\n') continue;
        out[w++] = out[r];
    }
    out[w] = 0;
    return w;
}

/* ---- lean hit ------------------------------------------------------ */
np_search_hit_t np_sh_make_hit(const char *title, const char *url,
                               const char *snippet, const char *source,
                               int64_t fetched_at, np_sh_encoding_t enc) {
    np_search_hit_t h;
    memset(&h, 0, sizeof h);
    scpy(h.title, sizeof h.title, title ? title : "");
    scpy(h.url, sizeof h.url, url ? url : "");
    char clean[NP_SH_SNIPPET_C * 2];
    np_sh_sanitize(snippet ? snippet : "", snippet ? strlen(snippet) : 0,
                   clean, sizeof clean);
    scpy(h.snippet, sizeof h.snippet, clean);
    scpy(h.source, sizeof h.source, source ? source : "");
    h.fetched_at = fetched_at;
    h.encoding = enc;
    /* Binding hash over the lean fields (title|url|snippet). */
    char bind[NP_SH_TITLE_CAP + NP_SH_URL_CAP + NP_SH_SNIPPET_C + 4];
    snprintf(bind, sizeof bind, "%s|%s|%s", h.title, h.url, h.snippet);
    h.hash = np_crc32(bind, strlen(bind));
    return h;
}

/* ---- fetch + slices ------------------------------------------------ */
int np_sh_fetch_slices(np_sh_fetch_fn fetch, void *user, const char *url,
                       size_t offset, size_t want_len,
                       np_sh_slice_t *out, size_t cap,
                       char *err, size_t err_cap) {
    if (err && err_cap) err[0] = 0;
    if (!fetch || !url || !out || cap == 0) {
        if (err && err_cap) snprintf(err, err_cap, "参数缺失");
        return -1;
    }
    unsigned char *body = (unsigned char *)malloc(NP_SH_BODY_CAP);
    if (!body) { if (err && err_cap) snprintf(err, err_cap, "内存不足"); return -1; }
    long got = fetch(url, body, NP_SH_BODY_CAP, user);
    if (got <= 0) {
        free(body);
        if (err && err_cap) snprintf(err, err_cap, "抓取失败");
        return -1;
    }
    size_t blen = (size_t)got;
    np_sh_encoding_t enc = np_sh_detect_encoding(body, blen);
    if (offset >= blen) { free(body); if (err && err_cap) snprintf(err, err_cap, "偏移越界"); return -1; }
    if (want_len == 0 || offset + want_len > blen) want_len = blen - offset;

    size_t produced = 0;
    size_t pos = offset;
    size_t end = offset + want_len;
    while (pos < end && produced < cap && produced < NP_SH_MAX_SLICES) {
        size_t take = end - pos;
        if (take > 4000) take = 4000;      /* fits a sanitized slice buffer */
        np_sh_slice_t *s = &out[produced];
        memset(s, 0, sizeof *s);
        s->offset = pos;
        s->length = np_sh_sanitize((const char *)body + pos, take, s->text, sizeof s->text);
        s->hash = np_crc32(s->text, s->length);
        s->encoding = enc;
        ++produced;
        pos += take;
    }
    free(body);
    return (int)produced;
}

/* ---- reference resolution ----------------------------------------- */
bool np_sh_resolve_ref(const np_sh_reference_t *ref, np_sh_fetch_fn fetch,
                       void *user, char *out, size_t cap,
                       char *err, size_t err_cap) {
    if (err && err_cap) err[0] = 0;
    if (!ref || !fetch || !out || cap == 0) {
        if (err && err_cap) snprintf(err, err_cap, "参数缺失");
        return false;
    }
    unsigned char *body = (unsigned char *)malloc(NP_SH_BODY_CAP);
    if (!body) { if (err && err_cap) snprintf(err, err_cap, "内存不足"); return false; }
    long got = fetch(ref->doc_id, body, NP_SH_BODY_CAP, user);
    if (got <= 0) {
        free(body);
        if (err && err_cap) snprintf(err, err_cap, "引用正文抓取失败");
        return false;
    }
    size_t blen = (size_t)got;
    if (ref->offset >= blen) {
        free(body);
        if (err && err_cap) snprintf(err, err_cap, "引用偏移越界");
        return false;
    }
    size_t take = ref->length;
    if (take == 0 || ref->offset + take > blen) take = blen - ref->offset;
    char clean[4096];
    size_t n = np_sh_sanitize((const char *)body + ref->offset, take, clean, sizeof clean);
    free(body);
    uint32_t h = np_crc32(clean, n);
    if (h != ref->expect_hash) {
        if (err && err_cap) snprintf(err, err_cap, "引用哈希不匹配（正文已变或串改）");
        return false;                        /* DROP: never trust stale excerpt */
    }
    if (n + 1 > cap) {
        if (err && err_cap) snprintf(err, err_cap, "输出缓冲过小");
        return false;
    }
    memcpy(out, clean, n + 1);
    return true;
}

/* ---- fallback chain ------------------------------------------------ */
bool np_sh_fetch_chain(const np_sh_provider_t *providers, size_t count,
                       const char *url, unsigned char *out, size_t cap,
                       char *source_out, size_t source_cap,
                       size_t *out_len, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = 0;
    if (!providers || count == 0 || !url || !out || cap == 0) {
        if (err && err_cap) snprintf(err, err_cap, "无可用搜索源");
        return false;
    }
    char last_err[256] = {0};
    for (size_t i = 0; i < count; ++i) {
        if (!providers[i].fetch) continue;
        long got = providers[i].fetch(url, out, cap, providers[i].user);
        if (got > 0) {
            if (source_out && source_cap) scpy(source_out, source_cap,
                                               providers[i].name ? providers[i].name : "");
            if (out_len) *out_len = (size_t)got;
            return true;
        }
        snprintf(last_err, sizeof last_err, "%s 抓取失败",
                 providers[i].name ? providers[i].name : "未命名源");
    }
    if (err && err_cap) snprintf(err, err_cap, "%s", last_err);
    return false;
}
