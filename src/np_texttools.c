/* np_texttools.c — S1: X7 原生文本编辑工具集. */
#include "np_texttools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *np_txt_status_name(np_txt_status_t s) {
    switch (s) {
    case NP_TXT_OK: return "ok";
    case NP_TXT_ERR_BAD_ARGS: return "bad-args";
    case NP_TXT_ERR_OUT_OF_RANGE: return "out-of-range";
    case NP_TXT_ERR_STALE_REVISION: return "stale-revision";
    case NP_TXT_ERR_PATCH_FAILED: return "patch-failed";
    default: return "no-hit";
    }
}

/* --- editor.text.read --- */
np_txt_status_t np_txt_read(const np_patch_buffer_t *b,
                            size_t start_line, size_t end_line,
                            char *out_text, size_t cap,
                            size_t *lines_read) {
    if (!b || !out_text || cap == 0) return NP_TXT_ERR_BAD_ARGS;
    out_text[0] = '\0';
    if (lines_read) *lines_read = 0;

    size_t total = np_patch_line_count(b);
    if (total == 0) return NP_TXT_ERR_OUT_OF_RANGE;
    if (start_line == 0) { start_line = 1; end_line = total; }
    if (start_line < 1 || end_line < start_line || start_line > total)
        return NP_TXT_ERR_OUT_OF_RANGE;
    if (end_line > total) end_line = total;

    size_t written = 0;
    for (size_t ln = start_line; ln <= end_line; ++ln) {
        size_t s = np_patch_line_byte_start(b, ln - 1); /* patch API: 0-based */
        size_t e = np_patch_line_byte_end(b, ln - 1);
        /* 行前缀 "%zu| " 便于模型引用行号 */
        char prefix[24];
        int pl = snprintf(prefix, sizeof(prefix), "%zu| ", ln);
        size_t line_len = e - s;
        if (written + (size_t)pl + line_len + 1 >= cap) break; /* 截断停止 */
        memcpy(out_text + written, prefix, (size_t)pl);
        written += (size_t)pl;
        memcpy(out_text + written, b->data + s, line_len);
        written += line_len;
        out_text[written++] = '\n';
    }
    out_text[written] = '\0';
    if (lines_read) *lines_read = end_line - start_line + 1;
    return NP_TXT_OK;
}

/* --- editor.text.search --- */
static char ci_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

np_txt_status_t np_txt_search(const np_patch_buffer_t *b,
                              const char *needle, bool case_insensitive,
                              size_t out_lines[], size_t out_cap,
                              size_t *n_hits) {
    if (!b || !b->data || !needle || !*needle || !out_lines || out_cap == 0)
        return NP_TXT_ERR_BAD_ARGS;
    *n_hits = 0;
    size_t nl = strlen(needle);
    size_t total = np_patch_line_count(b);
    for (size_t ln = 1; ln <= total; ++ln) {
        size_t s = np_patch_line_byte_start(b, ln - 1); /* patch API: 0-based */
        size_t e = np_patch_line_byte_end(b, ln - 1);
        if (e - s < nl) continue;
        for (size_t i = s; i + nl <= e; ++i) {
            bool hit = true;
            for (size_t j = 0; j < nl; ++j) {
                char a = (char)b->data[i + j], c = needle[j];
                if (case_insensitive) {
                    if (ci_lower(a) != ci_lower(c)) { hit = false; break; }
                } else if (a != c) { hit = false; break; }
            }
            if (hit) {
                if (*n_hits < out_cap) out_lines[*n_hits] = ln;
                (*n_hits)++;
                break; /* 每行只计一次 */
            }
        }
    }
    return *n_hits > 0 ? NP_TXT_OK : NP_TXT_ERR_NO_HIT;
}

/* --- editor.text.patch --- */
np_txt_status_t np_txt_patch(np_patch_set_t *ps, np_patch_buffer_t *b,
                             size_t *new_revision) {
    if (!ps || !b) return NP_TXT_ERR_BAD_ARGS;
    np_patch_result_t r = np_patch_apply(ps, NP_TRACK_TEXT, b);
    if (r == NP_PATCH_ERR_REVISION) return NP_TXT_ERR_STALE_REVISION;
    if (r != NP_PATCH_OK)         return NP_TXT_ERR_PATCH_FAILED;
    if (new_revision) *new_revision = (size_t)b->revision;
    return NP_TXT_OK;
}

/* --- editor.text.goto --- */
np_txt_status_t np_txt_goto(const np_patch_buffer_t *b, size_t line,
                            size_t *byte_offset) {
    if (!b || !byte_offset || line < 1) return NP_TXT_ERR_BAD_ARGS;
    size_t total = np_patch_line_count(b);
    if (line > total) return NP_TXT_ERR_OUT_OF_RANGE;
    *byte_offset = np_patch_line_byte_start(b, line - 1); /* patch API: 0-based */
    return NP_TXT_OK;
}

/* --- 注册（X1/X2 路由） --- */
size_t np_texttools_register(np_registry_t *reg) {
    if (!reg) return 0;
    np_tool_desc_t d;
    size_t n = 0;

    memset(&d, 0, sizeof(d));
    snprintf(d.id, sizeof(d.id), "%s", "editor.text.read");
    d.kind = NP_TOOL_NATIVE; d.core = true; d.budget = 1;
    d.risk = NP_RISK_READONLY; d.health = true; d.authorized = true;
    snprintf(d.description, sizeof(d.description),
             "%s", "read a line range with line numbers");
    if (np_registry_register(reg, &d)) n++;

    memset(&d, 0, sizeof(d));
    snprintf(d.id, sizeof(d.id), "%s", "editor.text.search");
    d.kind = NP_TOOL_NATIVE; d.core = true; d.budget = 1;
    d.risk = NP_RISK_READONLY; d.health = true; d.authorized = true;
    snprintf(d.description, sizeof(d.description),
             "%s", "literal substring search, returns line numbers");
    if (np_registry_register(reg, &d)) n++;

    memset(&d, 0, sizeof(d));
    snprintf(d.id, sizeof(d.id), "%s", "editor.text.patch");
    d.kind = NP_TOOL_NATIVE; d.core = true; d.budget = 2;
    d.risk = NP_RISK_WRITE; d.health = true; d.authorized = true;
    snprintf(d.description, sizeof(d.description),
             "%s", "apply a text patch set bound to base_revision");
    if (np_registry_register(reg, &d)) n++;

    memset(&d, 0, sizeof(d));
    snprintf(d.id, sizeof(d.id), "%s", "editor.text.goto");
    d.kind = NP_TOOL_NATIVE; d.core = true; d.budget = 1;
    d.risk = NP_RISK_READONLY; d.health = true; d.authorized = true;
    snprintf(d.description, sizeof(d.description),
             "%s", "map line number to byte offset");
    if (np_registry_register(reg, &d)) n++;

    return n;
}
